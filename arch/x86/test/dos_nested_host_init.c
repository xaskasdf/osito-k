#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* PID 1 of the disposable Linux host, never an Osito-K or DOS component. */
static _Noreturn void finish(int code)
{
    if (getpid() != 1) _exit(code);
    printf("[DOS-HOST] qemu-exit=%d\n", code);
    FILE *file = fopen("/output/inner.exit", "w");
    if (file) { fprintf(file, "%d\n", code); fclose(file); }
    fflush(NULL);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}

static void require(int success, const char *operation)
{
    if (!success) { perror(operation); finish(125); }
}

static void mount_fs(const char *source, const char *target, const char *type,
                     const char *options)
{
    require(mkdir(target, 0755) == 0 || errno == EEXIST, target);
    require(mount(source, target, type, 0, options) == 0, target);
}

static void network_init(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0, "network socket");
    struct ifreq req = {0};
    strcpy(req.ifr_name, "eth0");
    struct sockaddr_in address = { .sin_family = AF_INET };
    require(inet_pton(AF_INET, "10.0.2.15", &address.sin_addr) == 1, "IPv4 address");
    memcpy(&req.ifr_addr, &address, sizeof(address));
    require(ioctl(fd, SIOCSIFADDR, &req) == 0, "SIOCSIFADDR eth0");
    require(inet_pton(AF_INET, "255.255.255.0", &address.sin_addr) == 1, "IPv4 mask");
    memcpy(&req.ifr_netmask, &address, sizeof(address));
    require(ioctl(fd, SIOCSIFNETMASK, &req) == 0, "SIOCSIFNETMASK eth0");
    require(ioctl(fd, SIOCGIFFLAGS, &req) == 0, "SIOCGIFFLAGS eth0");
    req.ifr_flags |= IFF_UP;
    require(ioctl(fd, SIOCSIFFLAGS, &req) == 0, "SIOCSIFFLAGS eth0");
    close(fd);
}

static void print_parameter(const char *name)
{
    char path[128], value[32];
    snprintf(path, sizeof(path), "/sys/module/kvm_amd/parameters/%s", name);
    FILE *file = fopen(path, "r");
    require(file && fscanf(file, "%31s", value) == 1, path);
    fclose(file);
    printf("[DOS-HOST] %s=%s\n", name, value);
}

static void copy_serial(int source, int destination)
{
    char buffer[65536];
    ssize_t count;
    while ((count = read(source, buffer, sizeof(buffer))) != 0) {
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "read local serial log");
        for (ssize_t offset = 0; offset < count;) {
            ssize_t written = write(destination, buffer + offset, count - offset);
            if (written < 0 && errno == EINTR) continue;
            require(written > 0, "write exported serial log");
            offset += written;
        }
    }
}

static int select_input_devices(char **args, const char *profile)
{
    if (!profile || strcmp(profile, "usb") == 0) return 0;
    if (strcmp(profile, "ps2") != 0) return -1;
    unsigned destination = 0;
    for (unsigned source = 0; args[source]; ++source) {
        if (strcmp(args[source], "-device") == 0 && args[source + 1] &&
            (strcmp(args[source + 1], "qemu-xhci,id=usb") == 0 ||
             strcmp(args[source + 1], "usb-kbd,bus=usb.0") == 0 ||
             strcmp(args[source + 1], "usb-mouse,bus=usb.0") == 0)) {
            ++source;
            continue;
        }
        args[destination++] = args[source];
    }
    args[destination] = NULL;
    return 0;
}

int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "Only run this init as PID 1 in the disposable Linux VM.\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mount_fs("proc", "/proc", "proc", NULL);
    mount_fs("sysfs", "/sys", "sysfs", NULL);
    mount_fs("devtmpfs", "/dev", "devtmpfs", NULL);
    mount_fs("tmpfs", "/tmp", "tmpfs", "mode=1777");
    mount_fs("evidence", "/output", "9p", "trans=virtio,version=9p2000.L,cache=none");
    struct utsname name;
    require(uname(&name) == 0, "uname");
    printf("[DOS-HOST] kernel=%s machine=%s\n", name.release, name.machine);
    print_parameter("nrips");
    print_parameter("nested");
    print_parameter("npt");
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    require(kvm >= 0, "/dev/kvm");
    close(kvm);
    network_init();
    char *args[] = {
        "/usr/bin/qemu-system-x86_64", "-accel", "kvm", "-cpu", "host",
        "-machine", "q35", "-m", "8G", "-smp", "4", "-name", "osito-dos-regression",
        "-bios", "/OVMF.fd", "-L", "/usr/share/qemu",
        "-drive", "file=/esp.img,format=raw,if=none,id=esp",
        "-device", "ide-hd,drive=esp,bus=ide.0,bootindex=1",
        "-drive", "file=/disk.img,format=raw,if=none,id=nvme0,cache=writeback",
        "-device", "nvme,serial=deadbeef,drive=nvme0",
        "-device", "igb,netdev=net0,romfile=", "-netdev", "user,id=net0,restrict=on",
        "-device", "qemu-xhci,id=usb", "-device", "usb-kbd,bus=usb.0",
        "-device", "usb-mouse,bus=usb.0",
        "-audiodev", "wav,id=wav0,path=/output/audio.wav,out.fixed-settings=on,out.frequency=48000,out.channels=2,out.format=s16",
        "-device", "intel-hda,id=hda0", "-device", "hda-output,id=snd0,audiodev=wav0",
        "-device", "virtio-vga", "-display", "none",
        "-chardev", "socket,id=serial,host=0.0.0.0,port=43290,server=on,wait=off,logfile=/serial.log",
        "-serial", "chardev:serial", "-monitor", "tcp:0.0.0.0:43291,server=on,wait=off",
        "-qmp", "tcp:0.0.0.0:43292,server=on,wait=off", "-snapshot", "-no-reboot", NULL
    };
    const char *input_profile = getenv("OSITO_DOS_INPUT");
    if (select_input_devices(args, input_profile) != 0) {
        fprintf(stderr, "Invalid OSITO_DOS_INPUT: expected usb or ps2\n");
        finish(125);
    }
    printf("[DOS-HOST] input-profile=%s\n", input_profile ? input_profile : "usb");
    FILE *argv_file = fopen("/output/inner.argv", "wb");
    require(argv_file != NULL, "inner.argv");
    for (unsigned i = 0; args[i]; ++i) fwrite(args[i], 1, strlen(args[i]) + 1, argv_file);
    fclose(argv_file);
    int serial_source = open("/serial.log", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int serial_output = open("/output/serial.log", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(serial_source >= 0 && serial_output >= 0, "serial logs");
    pid_t child = fork();
    require(child >= 0, "fork QEMU");
    if (child == 0) {
        int log = open("/output/inner-qemu.log", O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(125);
        close(log);
        execv(args[0], args);
        perror("exec inner QEMU");
        _exit(126);
    }
    printf("[DOS-HOST] inner-qemu-pid=%d\n", child);
    int status;
    /* Batch the evidence export; per-byte 9p writes stall the emulated UART. */
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result < 0 && errno == EINTR) continue;
        require(result >= 0, "wait QEMU");
        copy_serial(serial_source, serial_output);
        if (result == child) break;
        usleep(100000);
    }
    require(fsync(serial_output) == 0, "sync serial log");
    close(serial_source);
    close(serial_output);
    finish(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}
