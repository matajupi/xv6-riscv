#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char **argv) {
    ushort fds = opendfd();
    printf("Before:\n");
    for (int fd = 0; 16 > fd; fd++) {
        if (fds & (1 << fd)) {
            printf("fd %d open\n", fd);
        }
    }
    printf("\n");

    int pfd[2];
    pipe(pfd);

    fds = opendfd();
    printf("After:\n");
    for (int fd = 0; 16 > fd; fd++) {
        if (fds & (1 << fd)) {
            printf("fd %d open\n", fd);
        }
    }
    printf("\n");

    exit(0);
}
