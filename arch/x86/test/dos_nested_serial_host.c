#define main nested_init_main
#include "dos_nested_host_init.c"
#undef main

int main(void)
{
    char *original[] = { "qemu", "-smp", "4", "-device", "nvme,id=disk",
        "-device", "qemu-xhci,id=usb", "-device", "usb-kbd,bus=usb.0",
        "-device", "usb-mouse,bus=usb.0", "-device", "intel-hda,id=hda0", NULL };
    char *expected[] = { "qemu", "-smp", "4", "-device", "nvme,id=disk",
        "-device", "intel-hda,id=hda0", NULL };
    const char *profiles[] = { NULL, "usb", "ps2", "", "USB", "ps2-extra" };
    for (unsigned i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        char *args[sizeof(original) / sizeof(original[0])];
        memcpy(args, original, sizeof(args));
        int result = select_input_devices(args, profiles[i]);
        if (result != (i < 3 ? 0 : -1)) return 20;
        if (i != 2) {
            if (memcmp(args, original, sizeof(args)) != 0) return 21;
        } else {
            for (unsigned j = 0; j < sizeof(expected) / sizeof(expected[0]); ++j)
                if ((args[j] == NULL) != (expected[j] == NULL) ||
                    (args[j] && strcmp(args[j], expected[j]) != 0)) return 22;
        }
    }
    puts("PASS input profiles: default argv unchanged, PS/2 preserves other devices, invalid values rejected");
    FILE *source = tmpfile(), *output = tmpfile();
    if (!source || !output) return 1;
    int input = fileno(source), destination = fileno(output);
    char data[150001], actual[sizeof(data)];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (char)(i % 251);
    if (pwrite(input, data, 7, 0) != 7) return 2;
    copy_serial(input, destination);
    copy_serial(input, destination);
    if (pwrite(input, data + 7, sizeof(data) - 7, 7) != sizeof(data) - 7) return 3;
    copy_serial(input, destination);
    copy_serial(input, destination);
    if (pread(destination, actual, sizeof(actual), 0) != sizeof(actual)) return 4;
    struct stat info;
    if (fstat(destination, &info) != 0 || info.st_size != sizeof(data) ||
        memcmp(data, actual, sizeof(data)) != 0) return 5;
    for (int invalid_output = 0; invalid_output < 2; ++invalid_output) {
        if (lseek(input, 0, SEEK_SET) != 0) return 6;
        pid_t child = fork();
        if (child < 0) return 7;
        if (!child) {
            copy_serial(invalid_output ? input : -1, invalid_output ? -1 : destination);
            _exit(0);
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 125) return 8;
    }
    fclose(source);
    fclose(output);
    puts("PASS serial export: binary data, short first batch, multiple blocks, append after EOF, no duplication");
    puts("PASS serial read/write errors exit 125 without host initialization or shutdown");
    return 0;
}
