#include <stddef.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

static struct net {
    struct spinlock lock;
#define NET_BUF_SIZE 512
    char buf[NET_BUF_SIZE];
} net;

int netwrite(int user_src, uint64 src, int n) {
    int i = 0;

    for (i = 0; n > i; i++) {
        char c;
        if (either_copyin(&c, user_src, src + i, 1) == -1) {
            break;
        }
        net.buf[i % NET_BUF_SIZE] = c;
    }
    virtio_net_send(net.buf, n);

    return i;
}

void netinit(void) {
    initlock(&net.lock, "net");

    devsw[NET].read = NULL;
    devsw[NET].write = netwrite;
}

