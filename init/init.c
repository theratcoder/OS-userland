// init.c - minimal init for RatOS
// Build with: gcc -static -o init init.c

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main() {
    printf("[RatOS init] Starting...\n");

    // Create mount points
    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/dev", 0755);

    // Mount pseudo-filesystems
    if (mount("proc", "/proc", "proc", 0, "") < 0)
        perror("mount /proc");
    if (mount("sysfs", "/sys", "sysfs", 0, "") < 0)
        perror("mount /sys");
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, "") < 0)
        perror("mount /dev");

    // Launch a shell
    printf("[RatOS init] Launching /bin/sh...\n");
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/dash", "", NULL);
        perror("exec /bin/sh");
        exit(1);
    }

    // Wait for shell to exit
    int status;
    while (1) {
        pid_t w = waitpid(pid, &status, 0);
        if (w == -1) {
            perror("waitpid");
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            printf("[RatOS init] Shell exited. Dropping to emergency loop.\n");
            break;
        }
    }

    // Emergency loop if shell dies
    while (1) {
        printf("[RatOS init] Emergency loop. System halted.\n");
        sleep(60);
    }

    return 0;
}