#include "kernel/types.h"
#include "kernel/fs.h"
#include "user/user.h"

#define DIRENT_SIZE sizeof(struct dirent)
#define MAX_PATH_SIZE 512

void strjoin(char *str1, char *str2) {
    while (*str1 != '\0') {
        str1++;
    }
    strcpy(str1, str2);
}

void getwd_helper(char *curpath, char *fullpath, int chi_inum) {
    int fd;
    if ((fd = open(curpath, 0)) < 0) {
        fprintf(2, "Cannot open: %s\n", curpath);
        exit(1);
    }

    struct dirent de;
    char name[MAX_PATH_SIZE] = "";
    int cur_inum = -1;
    while (read(fd, &de, DIRENT_SIZE) == DIRENT_SIZE) {
        if (strcmp(de.name, ".") == 0) {
            cur_inum = de.inum;
        }
        if (de.inum == chi_inum) {
            strcpy(name, de.name);
        }
    }

    if (chi_inum == cur_inum) {
        return;
    }

    strjoin(curpath, "../");
    getwd_helper(curpath, fullpath, cur_inum);

    if (strcmp(name, "") != 0) {
        strjoin(fullpath, "/");
        strjoin(fullpath, name);
    }
}

void getwd(char *fullpath) {
    char curpath[MAX_PATH_SIZE] = "./";
    getwd_helper(curpath, fullpath, -1);
    if (strcmp(fullpath, "") == 0) {
        strcpy(fullpath, "/");
    }
}

int main(int argc, char **argv, char **envp) {
    char fullpath[MAX_PATH_SIZE] = "";
    getwd(fullpath);

    printf("%s\n", fullpath);
    return 0;
}

