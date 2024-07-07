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
    struct virtq recvq;
    struct virtq transq;

    struct spinlock vnet_lock;
} net;

struct virtio_net_config {
    uint8 mac[6];
    uint16 status;
    uint16 max_virtq_pairs;
};

struct virtio_net_hdr empty_hdr;

static void setup_virtq(uint8 sel, struct virtq *que) {
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

    que->desc = kalloc();
    que->avail = kalloc();
    que->used = kalloc();
    if (!que->desc || !que->avail || !que->used) {
        panic("virtio net kalloc");
    }
    memset(que->desc, 0, PGSIZE);
    memset(que->avail, 0, PGSIZE);
    memset(que->used, 0, PGSIZE);

    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    *R(VIRTIO_MMIO_QUEUE_DESC_LOW)      = (uint64)que->desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH)     = (uint64)que->desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW)     = (uint64)que->avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH)    = (uint64)que->avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW)     = (uint64)que->used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH)    = (uint64)que->used >> 32;
}

void virtio_net_init(void) {
    uint32 status = 0;

    initlock(&net.vnet_lock, "virtio_net");

    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(VIRTIO_MMIO_VERSION) != 2 ||
        *R(VIRTIO_MMIO_DEVICE_ID) != 1 ||
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
        panic("could not find virtio nic");
    }

    // initialize
    *R(VIRTIO_MMIO_STATUS) = status;

    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;

    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // TODO: Initialize features bits

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    setup_virtq(0, &net.recvq);
    setup_virtq(1, &net.transq);

    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    for (int i = 0; NUM > i; i++) {
        net.recvq.free[i] = 1;
        net.transq.free[i] = 1;
    }

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // print mac address
    struct virtio_net_config *cfg = (struct virtio_net_config *)R(VIRTIO_MMIO_CONFIG);
    printf("%x:%x:%x:%x:%x:%x\n", cfg->mac[0], cfg->mac[1], cfg->mac[2], cfg->mac[3],
        cfg->mac[4], cfg->mac[5]);
}

static int alloc_desc(struct virtq *que) {
    for (int i = 0; NUM > i; i++) {
        if (que->free[i]) {
            que->free[i] = 0;
            return i;
        }
    }
    return -1;
}

static void free_desc(struct virtq *que, int i) {
    if (i >= NUM) {
        panic("free_desc 1");
    }
    if (que->free[i]) {
        panic("free_desc 2");
    }
    que->desc[i].addr = 0;
    que->desc[i].len = 0;
    que->desc[i].flags = 0;
    que->desc[i].next = 0;
    que->free[i] = 1;
    wakeup(&que->free[0]);
}

static int alloc_desc_n(struct virtq *que, int *idx, int n) {
    for (int i = 0; n > i; i++) {
        idx[i] = alloc_desc(que);
        if (idx[i] < 0) {
            for (int j = 0; i > j; j++) {
                free_desc(que, idx[j]);
                return -1;
            }
        }
    }
    return 0;
}

void virtio_net_send(void *buf, uint32 length) {
    acquire(&net.vnet_lock);

    int idx[2];
    while (1) {
        if (alloc_desc_n(&net.transq, idx, 2) == 0) {
            break;
        }
        sleep(&net.transq.free[0], &net.vnet_lock);
    }

    net.transq.desc[idx[0]].addr = (uint64)&empty_hdr;
    net.transq.desc[idx[0]].len = sizeof(struct virtio_net_hdr);
    net.transq.desc[idx[0]].flags = VRING_DESC_F_NEXT;
    net.transq.desc[idx[0]].next = idx[1];

    net.transq.desc[idx[1]].addr = (uint64)buf;
    net.transq.desc[idx[1]].len = length;
    net.transq.desc[idx[1]].flags = 0;
    net.transq.desc[idx[1]].next = 0;

    net.transq.avail->ring[net.transq.avail->idx % NUM] = idx[0];

    __sync_synchronize();

    net.transq.avail->idx++;

    __sync_synchronize();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 1;

    // TODO: Wait

    release(&net.vnet_lock);
}

// void virtio_net_intr() {
//     acquire(&net.vnet_lock);
// 
//     *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
// 
//     __sync_synchornize();
//     // TODO:
// 
//     release(&net.vnet_lock);
// }
