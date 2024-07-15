#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define BUF_SIZE 512

int main(int argc, char **argv, char **envp) {
    int fd = open("net", O_WRONLY);
    if (fd < 0) {
        fprintf(2, "netwrite: failed to open net");
        exit(-1);
    }
    char buf[BUF_SIZE + 1];
    int n;
    while ((n = read(0, buf, BUF_SIZE)) > 0) {
        write(fd, buf, n);
    }
    close(fd);
    return 0;
}

