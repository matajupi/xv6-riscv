#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define BUF_SIZE 512

int main(int argc, char **argv, char **envp) {
    int fd = open("net", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "netwrite: failed to open net");
        exit(-1);
    }
    char buf[BUF_SIZE];
    int n = read(fd, buf, BUF_SIZE);
    if (n > 0) {
        write(0, buf, n);
    }
    close(fd);
    return 0;
}

