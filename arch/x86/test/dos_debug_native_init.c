#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* PID 1 for a diskless, disposable Linux/KVM diagnostic VM. */
static void finish(int code)
{
    printf("[KVM-HOST-BOOT] probe-exit=%d\n", code);
    fflush(NULL);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}

static void mount_fs(const char *type, const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror(path);
        finish(125);
    }
    if (mount(type, path, type, 0, NULL) != 0) {
        perror(type);
        finish(125);
    }
}

static void print_kvm_parameter(const char *name, const char *path)
{
    FILE *file = fopen(path, "r");
    char value[16];
    if (file && fscanf(file, "%15s", value) == 1)
        printf("[KVM-HOST-BOOT] %s=%s\n", name, value);
    else
        printf("[KVM-HOST-BOOT] %s=unavailable\n", name);
    if (file) fclose(file);
}

int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "This init is only for the disposable test VM.\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mount_fs("proc", "/proc");
    mount_fs("sysfs", "/sys");
    mount_fs("devtmpfs", "/dev");
    int console = open("/dev/console", O_RDWR);
    if (console >= 0) {
        for (int fd = 0; fd < 3; ++fd) dup2(console, fd);
        if (console > 2) close(console);
    }
    struct utsname name;
    if (uname(&name) != 0) finish(125);
    printf("[KVM-HOST-BOOT] kernel=%s machine=%s\n", name.release, name.machine);
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        perror("nested /dev/kvm");
        finish(77);
    }
    close(kvm);
    print_kvm_parameter("nrips", "/sys/module/kvm_amd/parameters/nrips");
    print_kvm_parameter("nested", "/sys/module/kvm_amd/parameters/nested");
    print_kvm_parameter("npt", "/sys/module/kvm_amd/parameters/npt");
    pid_t pid = fork();
    if (pid < 0) finish(125);
    if (pid == 0) {
        execl("/debug-host", "debug-host", "/guest.bin", (char *)NULL);
        perror("exec debug-host");
        _exit(126);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) finish(125);
    }
    finish(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}
