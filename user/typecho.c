#include "kernel/types.h"
#include "user/user.h"

#define BUF_SIZE 512
#define STDIN 0

int main(int argc, char **argv, char **envp) {
    char buf[BUF_SIZE + 1];
    int n;
    while ((n = read(STDIN, buf, BUF_SIZE)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    return 0;
}

