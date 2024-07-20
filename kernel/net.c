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

    char buf[PGSIZE];
} net;

int netwrite(int user_src, uint64 src, int n) {
    acquire(&net.lock);

    int retval;
    int size = MIN(n, PGSIZE);
    if (either_copyin(&net.buf, user_src, src, size) == -1) {
        retval = -1;
        goto end;
    }
    int real_size = virtio_net_send(&net.buf, size);
    retval = real_size;

end:
    release(&net.lock);
    return retval;
}

int netread(int user_dst, uint64 dst, int n) {
    acquire(&net.lock);

    int retval;
    int size = MIN(n, PGSIZE);
    int real_size = virtio_net_recv(&net.buf, size);
    if (either_copyout(user_dst, dst, &net.buf, real_size) == -1) {
        retval = -1;
        goto end;
    }
    retval = real_size;

end:
    release(&net.lock);
    return retval;
}

void netinit(void) {
    initlock(&net.lock, "net");

    devsw[NET].read = netread;
    devsw[NET].write = netwrite;
}

