// qemu ... -netdev tap,script=no,downscript=no,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1
// https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "virtio.h"

#define R(r) ((volatile uint32 *)(VIRTIO1 + (r)))

static struct net {
    struct virtq rx_vq;
    struct virtq tx_vq;

    struct spinlock vnet_lock;
} net;

struct virtio_net_config {
    uint8 mac[6];
    uint16 status;
    uint16 max_virtq_pairs;
};

static void setup_virtq(uint8 sel, struct virtq *vq) {
    *R(VIRTIO_MMIO_QUEUE_SEL) = sel;

    if (*R(VIRTIO_MMIO_QUEUE_READY)) {
        panic("virtio net should not be ready");
    }

    uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0) {
        panic("virtio net has no queue 0");
    }
    if (max < NUM) {
        panic("virtio net max queue too short");
    }

    vq->desc = kalloc();
    vq->avail = kalloc();
    vq->used = kalloc();

    if (!vq->desc || !vq->avail || !vq->used) {
        panic("virtio net kalloc");
    }
    memset(vq->desc, 0, PGSIZE);
    memset(vq->avail, 0, PGSIZE);
    memset(vq->used, 0, PGSIZE);

    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    *R(VIRTIO_MMIO_QUEUE_DESC_LOW)      = (uint64)vq->desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH)     = (uint64)vq->desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW)     = (uint64)vq->avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH)    = (uint64)vq->avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW)     = (uint64)vq->used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH)    = (uint64)vq->used >> 32;

    for (int i = 0; NUM > i; i++) {
        vq->free[i] = 1;
    }
}

void virtio_net_init(void) {
    uint32 status = 0;

    initlock(&net.vnet_lock, "virtio_net");

    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 || // "virt"(little endian)
        *R(VIRTIO_MMIO_VERSION) != 0x2 || // modern device
        *R(VIRTIO_MMIO_DEVICE_ID) != 1 || // network card
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
        panic("could not find virtio nic");
    }

    // initialize
    *R(VIRTIO_MMIO_STATUS) = status;

    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;

    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // initialize features bits
    uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    features &= ~(1 << VIRTIO_NET_F_GUEST_CSUM);
    features &= ~(1 << VIRTIO_NFT_F_MAC);
    features &= ~(1 << VIRTIO_NET_F_MQ);
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    setup_virtq(0, &net.rx_vq);
    setup_virtq(1, &net.tx_vq);

    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // print mac address
    struct virtio_net_config *cfg = (struct virtio_net_config *)R(VIRTIO_MMIO_CONFIG);
    printf("mac: %x:%x:%x:%x:%x:%x\n", cfg->mac[0], cfg->mac[1], cfg->mac[2],
        cfg->mac[3], cfg->mac[4], cfg->mac[5]);
}

static int alloc_desc(struct virtq *vq) {
    for (int i = 0; NUM > i; i++) {
        if (vq->free[i]) {
            vq->free[i] = 0;
            return i;
        }
    }
    return -1;
}

static void free_desc(struct virtq *vq, int i) {
    if (i >= NUM) {
        panic("free_desc 1");
    }
    if (vq->free[i]) {
        panic("free_desc 2");
    }
    vq->desc[i].addr = 0;
    vq->desc[i].len = 0;
    vq->desc[i].flags = 0;
    vq->desc[i].next = 0;
    vq->free[i] = 1;
    wakeup(&vq->free[0]);
}

static void free_chain(struct virtq *vq, int i) {
    while (1) {
        int flag = vq->desc[i].flags;
        int nxt = vq->desc[i].next;
        free_desc(vq, i);
        if (flag & VRING_DESC_F_NEXT) {
            i = nxt;
        }
        else {
            break;
        }
    }
}

static int alloc_desc_n(struct virtq *vq, int *idx, int n) {
    for (int i = 0; n > i; i++) {
        idx[i] = alloc_desc(vq);
        if (idx[i] < 0) {
            for (int j = 0; i > j; j++) {
                free_desc(vq, idx[j]);
                return -1;
            }
        }
    }
    return 0;
}

void virtio_net_send(void *buf, int len) {
    acquire(&net.vnet_lock);

    int idx[2];
    while (1) {
        if (alloc_desc_n(&net.tx_vq, idx, 2) == 0) {
            break;
        }
        sleep(&net.tx_vq.free[0], &net.vnet_lock);
    }

    struct virtio_net_hdr hdr = {0};

    net.tx_vq.desc[idx[0]].addr = (uint64)&hdr;
    net.tx_vq.desc[idx[0]].len = sizeof(struct virtio_net_hdr);
    net.tx_vq.desc[idx[0]].flags = VRING_DESC_F_NEXT;
    net.tx_vq.desc[idx[0]].next = idx[1];

    net.tx_vq.desc[idx[1]].addr = (uint64)buf;
    net.tx_vq.desc[idx[1]].len = len;
    net.tx_vq.desc[idx[1]].flags = 0;
    net.tx_vq.desc[idx[1]].next = 0;

    net.tx_vq.avail->ring[net.tx_vq.avail->idx % NUM] = idx[0];

    // Memory barrier
    __sync_synchronize();

    net.tx_vq.avail->idx++;

    __sync_synchronize();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 1;
    net.tx_vq.used_idx++;

    while (net.tx_vq.used_idx != net.tx_vq.used->idx) {
        __asm__("nop");
    }
    free_chain(&net.tx_vq, idx[0]);

    release(&net.vnet_lock);
}

uint16 virtio_net_recv(void *buf) {
    acquire(&net.vnet_lock);

    while (net.rx_vq.used_idx == net.rx_vq.used->idx) {
        __asm__("nop");
    }

    int idx = net.rx_vq.used->ring[net.rx_vq.used_idx].id;
    net.rx_vq.used_idx++;

    uint16 len = 0;
    while (1) {
        len += net.rx_vq.desc[idx].len;
        if (net.rx_vq.desc[idx].flags & VRING_DESC_F_NEXT) {
            idx = net.rx_vq.desc[idx].next;
        }
        else {
            break;
        }
    }

    release(&net.vnet_lock);
    return len;
}

