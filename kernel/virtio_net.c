// virtio-document: https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html#x1-7500013

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

static void free_desc(struct virtq *vq, int idx, uint16 flags) {
    memset((void *)vq->desc[idx].addr, 0, PGSIZE);
    vq->desc[idx].len = PGSIZE;
    vq->desc[idx].flags = flags;
    vq->desc[idx].next = 0;
}

static void set_avail(struct virtq *vq, int idx) {
    vq->avail->ring[vq->avail->idx % NUM] = idx;
    vq->avail->idx++;
}

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

    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;
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
    features &= ~(1 << VIRTIO_NET_F_MQ);
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    setup_virtq(0, &net.rx_vq);
    setup_virtq(1, &net.tx_vq);

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // initialize rx_vq
    // memory barrier(NOTIFYするため)
    __sync_synchronize();

    for (int i = 0; NUM > i; i++) {
        net.rx_vq.desc[i].addr = (uint64)kalloc();
        free_desc(&net.rx_vq, i, VRING_DESC_F_WRITE);
        set_avail(&net.rx_vq, i);
    }

    __sync_synchronize();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

    // initialize tx_vq
    __sync_synchronize();

    for (int i = 0; NUM > i; i++) {
        net.tx_vq.desc[i].addr = (uint64)kalloc();
        free_desc(&net.tx_vq, i, 0);
    }

    __sync_synchronize();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 1;

    // print mac address
    struct virtio_net_config *cfg
        = (struct virtio_net_config *)R(VIRTIO_MMIO_CONFIG);
    printf("mac: %x:%x:%x:%x:%x:%x\n", cfg->mac[0], cfg->mac[1], cfg->mac[2],
        cfg->mac[3], cfg->mac[4], cfg->mac[5]);
}

int virtio_net_send(void *buf, int buf_size) {
    acquire(&net.vnet_lock);

    struct virtq *vq = &net.tx_vq;

    struct virtio_net_hdr hdr = {0};

    int idx = 0;
    int offset = 0;
    memmove((void *)vq->desc[idx].addr, &hdr, sizeof(struct virtio_net_hdr));
    vq->desc[idx].len = sizeof(struct virtio_net_hdr);
    vq->desc[idx].flags = VRING_DESC_F_NEXT;
    vq->desc[idx].next = idx + 1;
    idx++;

    for (; buf_size > 0 && NUM > idx; idx++) {
        int size = MIN(PGSIZE, buf_size);
        memmove((void *)vq->desc[idx].addr, buf + offset, size);
        buf_size -= size;
        offset += size;

        vq->desc[idx].len = size;
        if (buf_size > 0 && idx != NUM - 1) {
            vq->desc[idx].flags = VRING_DESC_F_NEXT;
            vq->desc[idx].next = idx + 1;
        }
        else {
            vq->desc[idx].flags = 0;
            vq->desc[idx].next = 0;
        }
    }

    vq->avail->ring[vq->avail->idx % NUM] = 0;

    __sync_synchronize();

    vq->avail->idx++;

    __sync_synchronize();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 1;

    vq->used_idx++;
    while (vq->used_idx != vq->used->idx) {
        __asm__("nop");
    }

    for (int i = 0; idx > i; i++) {
        free_desc(vq, i, 0);
    }

    release(&net.vnet_lock);
    return offset;
}

int virtio_net_recv(void *buf, int buf_size) {
    acquire(&net.vnet_lock);

    struct virtq *vq = &net.rx_vq;

    while (vq->used_idx == vq->used->idx) {
        __asm__("nop");
    }

    int idx = vq->used->ring[vq->used_idx % NUM].id;
    int offset = 0;
    while (buf_size > 0) {
        int size = MIN(vq->used->ring[vq->used_idx % NUM].len, buf_size);
        memmove(buf, (void *)(vq->desc[idx].addr + offset), size);
        buf_size -= size;
        offset += size;

        if (vq->desc[idx].flags & VRING_DESC_F_NEXT) {
            idx = vq->desc[idx].next;
        }
        else {
            break;
        }
    }

    idx = vq->used->ring[vq->used_idx % NUM].id;
    while (1) {
        int flag = vq->desc[idx].flags;
        int nxt = vq->desc[idx].next;

        free_desc(vq, idx, VRING_DESC_F_WRITE);
        set_avail(vq, idx);

        if (flag & VRING_DESC_F_NEXT) {
            idx = nxt;
        }
        else {
            break;
        }
    }

    vq->used->ring[vq->used_idx % NUM].len = 0;
    vq->used_idx++;

    release(&net.vnet_lock);
    return offset;
}

