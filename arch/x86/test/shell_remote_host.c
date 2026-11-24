/* The real parser, session I/O and admission policy, without kernel execution. */
#include "../kernel/shell.c"

extern int printf(const char *format, ...);
extern void *malloc(size_t);
extern void free(void *);
static unsigned allocations;
static bool allocation_failure;
void *kmalloc(uint64_t size)
{
    void *p = allocation_failure ? NULL : malloc(size);
    if (p) allocations++;
    return p;
}
void kfree(void *p) { if (p) { allocations--; free(p); } }
static int current_task;
int32_t proc_current_pid(void) { return current_task + 100; }
static unsigned stop_requests;
static int stopped_owner, stop_result;
int win32_request_console_exit(int owner, uint32_t status)
{
    stop_requests++;
    stopped_owner = owner;
    if (status != 0xC000013AU) return -1;
    return stop_result;
}
static unsigned checks, failures, yields;
static unsigned reboots;
static unsigned reboot_checks;
static int reboot_check_result;
int sched_current_get(void) { return current_task; }
void sched_yield(void) { yields++; }
void serial_puts(const char *s) { (void)s; }
void fb_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void fb_putdec(uint64_t n) { (void)n; }
void acpi_reboot(void) { reboots++; }
int acpi_reboot_check(void) { reboot_checks++; return reboot_check_result; }

#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

static bool admissible(const char *text)
{
    char line[256], *argv[MAX_ARGS];
    memcpy(line, text, strlen(text)+1);
    int argc = parse_args(line, argv);
    redir_t redir;
    parse_redirects(&argc, argv, &redir);
    return argc && shell_remote_supported(argc, argv) &&
           (strcmp(argv[0], "console") || (!redir.in_file && !redir.out_file)) &&
           shell_remote_independent_command(argc, argv);
}

static char *launch(const char *text)
{
    char line[8192], *argv[MAX_ARGS];
    memcpy(line, text, strlen(text)+1);
    return shell_console_launch_line(parse_args(line, argv), argv);
}

static void console_tests(void)
{
    char *line = launch("console launch winexec 'Games/UT 99/game.exe' -safe");
    CHECK(line && !strcmp(line, "\"winexec\" \"Games/UT 99/game.exe\" \"-safe\""));
    uint64_t id = 0;
    CHECK(!shell_console_submit(line, &id, true) && !id);
    CHECK(!shell_console_poll(true) && console_job.ready);
    CHECK(shell_console_submit(line, &id, true) && id == 1 && !console_job.ready);
    CHECK(!shell_console_submit(NULL, &id, false));
    char *stop[] = {"console", "stop", "1"};
    char *stale[] = {"console", "stop", "2"};
    char *overflow[] = {"console", "stop", "18446744073709551616"};
    char *negative[] = {"console", "stop", "-1"};
    shell_console_command(3, stop);
    CHECK(!stop_requests && !console_job.stop_requested);
    CHECK(shell_console_poll(true) && console_job.phase == CONSOLE_RUNNING);
    CHECK(!shell_console_poll(false) && console_job.phase == CONSOLE_RUNNING);
    CHECK(shell_console_take() == line && !console_job.line);
    CHECK(console_job.owner_pid == proc_current_pid());
    shell_console_command(3, stale);
    shell_console_command(3, overflow);
    shell_console_command(3, negative);
    CHECK(!stop_requests && !console_job.stop_requested);
    stop_result = -1;
    shell_console_command(3, stop);
    CHECK(stop_requests == 1 && !console_job.stop_requested);
    stop_result = 0;
    shell_console_command(3, stop);
    CHECK(stop_requests == 2 && console_job.stop_requested &&
          stopped_owner == proc_current_pid());
    CHECK(!shell_console_poll(true) && !console_job.ready);
    kfree(line);
    shell_console_returned();
    shell_console_command(3, stop);
    CHECK(stop_requests == 2);
    CHECK(console_job.phase == CONSOLE_RETURNED);
    CHECK(!shell_console_submit(NULL, &id, false));
    CHECK(!shell_console_poll(true) && console_job.ready);
    line = launch("console launch desktop");
    CHECK(line && shell_console_submit(line, &id, false) && id == 2);
    CHECK(!shell_console_poll(false) && console_job.phase == CONSOLE_CANCELLED);
    CHECK(!console_job.line && !allocations && !console_job.ready);
    CHECK(!shell_console_poll(false) && !allocations);
    const char *bad[] = {"console", "console status", "console launch",
        "console launch desktop extra", "console launch winexec", "console launch winexec ''",
        "console launch wlan connect", "console launch echo hi", "console launch reboot",
        "console launch reboot --typo", "console launch reboot --check extra",
        "console launch reboot '--check;reboot'",
        "console launch 'winexec game.exe'", "console launch winexec 'g|echo evil'",
        "console launch winexec '<secret'", "console launch winexec '>out'",
        "console launch winexec 'game;reboot'", "console launch winexec 'game\nreboot'",
        "console launch winexec 'a\"b'", "console launch winexec 'a\tb'"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) CHECK(!launch(bad[i]));
    char big[8192];
    memcpy(big, "console launch winexec ", 23);
    memset(big+23, 'x', 4096); big[4119] = 0;
    CHECK(!launch(big));
    char *dos = launch("console launch dosrun DOOM.EXE -nosound");
    CHECK(dos != NULL); kfree(dos);
    char *check = launch("console launch reboot --check");
    CHECK(check && !strcmp(check, "\"reboot\" \"--check\"")); kfree(check);
    char *process_test = launch("console launch win32-process-test");
    CHECK(process_test && !strcmp(process_test,"\"win32-process-test\"")); kfree(process_test);
    CHECK(!launch("console launch win32-process-test extra"));
    CHECK(!launch("console launch win32-process-test 'x;reboot'"));
    char *vm_test = launch("console launch win32-vm-test");
    CHECK(vm_test && !strcmp(vm_test,"\"win32-vm-test\"")); kfree(vm_test);
    CHECK(!launch("console launch win32-vm-test extra"));
    CHECK(!launch("console launch win32-vm-test 'x;reboot'"));
    char *signal_test = launch("console launch sched-signal-test");
    CHECK(signal_test && !strcmp(signal_test,"\"sched-signal-test\"")); kfree(signal_test);
    CHECK(!launch("console launch sched-signal-test extra"));
    char *worker_test = launch("console launch gpu-worker-test");
    CHECK(worker_test && !strcmp(worker_test,"\"gpu-worker-test\"")); kfree(worker_test);
    CHECK(!launch("console launch gpu-worker-test extra"));
    CHECK(!launch("console launch gpu-worker-test 'x;reboot'"));
    char *render_test=launch("console launch gpu-render-test");
    CHECK(render_test && !strcmp(render_test,"\"gpu-render-test\"")); kfree(render_test);
    CHECK(!launch("console launch gpu-render-test extra"));
    CHECK(!launch("console launch gpu-render-test 'x;reboot'"));
    allocation_failure = true;
    CHECK(!launch("console launch desktop"));
    allocation_failure = false;
    CHECK(!allocations && console_job.phase == CONSOLE_CANCELLED);
}

static void reboot_tests(void)
{
    char *plain[] = {"reboot"};
    char *check[] = {"reboot", "--check"};
    char *extra[] = {"reboot", "--check", "extra"};
    char *bad[] = {"reboot", "--typo", "extra"};
    current_task = remote_owner = 9;
    remote_io.status = 0;
    cmd_reboot(2, bad);
    CHECK(remote_io.status == 126 && !allocations && !reboots && !reboot_checks);
    cmd_reboot(3, extra);
    CHECK(remote_io.status == 126 && !allocations && !reboots && !reboot_checks);
    cmd_reboot(2, check);
    CHECK(remote_io.status == 0 && reboot_checks == 1 && !reboots && !allocations);
    reboot_check_result = -1;
    cmd_reboot(2, check);
    CHECK(remote_io.status == 1 && reboot_checks == 2 && !reboots && !allocations);
    reboot_check_result = 0;
    remote_io.status = 0;
    cmd_reboot(1, plain);
    CHECK(remote_io.status == 126 && !allocations && !reboots);
    CHECK(!shell_console_poll(true));
    remote_io.status = 0;
    allocation_failure = true;
    cmd_reboot(1, plain);
    CHECK(remote_io.status == 126 && !allocations && console_job.ready);
    allocation_failure = false;
    remote_io.status = 0;
    cmd_reboot(1, plain);
    CHECK(remote_io.status == 0 && allocations == 1 && !reboots);
    CHECK(console_job.phase == CONSOLE_QUEUED);
    CHECK(!shell_console_poll(false) && console_job.phase == CONSOLE_CANCELLED);
    CHECK(!allocations && !reboots);
    CHECK(!shell_console_poll(true));
    remote_io.input = "piped input";
    cmd_reboot(1, plain);
    CHECK(remote_io.status == 126 && !allocations && console_job.ready);
    remote_io.input = NULL;
    remote_io.status = 0;
    cmd_reboot(1, plain);
    CHECK(shell_console_poll(true));
    char *line = shell_console_take();
    CHECK(line && !strcmp(line, "reboot") && !reboots);
    kfree(line);
    current_task = 7;
    cmd_reboot(2, bad);
    CHECK(!reboots && !allocations);
    cmd_reboot(2, check);
    CHECK(!reboots && reboot_checks == 3 && !allocations);
    cmd_reboot(1, plain);
    CHECK(reboots == 1 && !allocations);
    shell_console_returned();
}

int main(void)
{
    const char *allow[] = {
        "dmesg", "ps", "mem", "uptime", "uname", "cpus", "ifconfig", "route", "thermal", "thermal sample",
        "cpuidle", "cpuidle status", "lockup", "lockup status",
        "kprof", "kprof start", "kprof stop", "kprof raw", "kprof > profile.txt",
        "sched stats", "perf", "perf snapshot", "serial-stats", "usb-perf", "usb-perf > timing.txt",
        "hwbp", "hwbp status", "hwbp clear 0",
        "hwbp trace 0 1 138e6bf95 edi32 8 000102030405060708090a0b0c0d0e0f",
        "hwbp trace 0 1 138e6bf95 edi32 8 000102030405060708090a0b0c0d0e0f caller64:424",
        "sshd status", "drivers status", "wlan status", "wlan link", "wlan networks",
        "storage 1", "echo diagnostic", "cat", "cat log.txt", "head -n 24",
        "tail -n 12", "grep ARP", "grep 'two words'", "console status",
        "ls", "hexdump log.txt", "touch changed", "rm old",
        "mv old new", "cp old new", "sync", "dmesg > out", "echo yes >>out",
        "cat <input", "thermal > out", "thermal sample > out", "cpuidle > out",
        "lockup > out", "console launch desktop", "console launch reboot --check",
        "console stop 1"
    };
    const char *deny[] = {
        "", "sshd stop", "drivers auto on", "wlan stop", "wlan connect",
        "wlan scan", "wlan auto off", "winexec game.exe", "dosrun DOOM.EXE",
        "kexec kernel.elf", "reboot", "reboot --check",
        "dhcp", "apipa", "ipconf 1.2.3.4", "ping 1.2.3.4",
        "dmesg unexpected", "wlan status unexpected", "sshd status unexpected", "unknown",
        "console status > out", "console stop 1 > out", "thermal selftest",
        "thermal unexpected", "thermal sample extra",
        "cpuidle hlt", "cpuidle mwait", "cpuidle selftest", "cpuidle status extra",
        "lockup arm", "lockup arm 2", "lockup off", "lockup sample", "lockup status extra",
        "kprof unknown", "kprof start extra", "kprof raw extra", "sched",
        "sched stats extra", "perf reset", "perf snapshot extra", "serial-stats extra",
        "usb-perf reset", "usb-perf extra",
        "watch 138e6bf95 x", "unwatch 0", "hwbp clear", "hwbp clear 0 extra",
        "hwbp status extra", "hwbp trace 0 1 138e6bf95 edi32 8",
        "hwbp trace 0 1 138e6bf95 edi32 8 000102030405060708090a0b0c0d0e0f caller64:424 extra"
    };
    for (unsigned i = 0; i < sizeof(allow)/sizeof(allow[0]); i++) CHECK(admissible(allow[i]));
    for (unsigned i = 0; i < sizeof(deny)/sizeof(deny[0]); i++) CHECK(!admissible(deny[i]));
    CHECK(!shell_remote_independent_command(0, NULL));

    const char *loader[] = {"win32-loader", "win32-loader clear",
        "win32-loader flush", "win32-loader strict on", "win32-loader strict off"};
    const char *bad_loader[] = {"win32-loader unknown", "win32-loader strict",
        "win32-loader strict yes", "win32-loader strict on extra",
        "win32-loader clear extra", "win32-loader flush extra"};
    for (unsigned i = 0; i < sizeof(loader)/sizeof(loader[0]); i++) {
        char line[80], *argv[MAX_ARGS];
        memcpy(line, loader[i], strlen(loader[i])+1);
        int argc = parse_args(line, argv);
        CHECK(shell_remote_supported(argc, argv));
        CHECK(!shell_remote_independent_command(argc, argv));
        CHECK(!admissible(loader[i]));
    }
    for (unsigned i = 0; i < sizeof(bad_loader)/sizeof(bad_loader[0]); i++) {
        char line[80], *argv[MAX_ARGS];
        memcpy(line, bad_loader[i], strlen(bad_loader[i])+1);
        CHECK(!shell_remote_supported(parse_args(line, argv), argv));
    }

    current_task = 7;
    shell_command_lock(); shell_command_lock();
    CHECK(command_owner == 7 && command_depth == 2);
    redir_pos = 71;
    current_task = remote_owner = 9;
    redir_pos = 93;
    shell_command_lock(); shell_command_unlock();
    CHECK(command_owner == 7 && command_depth == 2 && !yields);
    CHECK(redir_pos == 93);
    current_task = 7;
    CHECK(redir_pos == 71 && !shell_remote_active());
    current_task = 9;
    CHECK(shell_remote_active());
    CHECK(!shell_command_try_lock() && command_depth == 2);
    current_task = 7;
    shell_command_unlock(); shell_command_unlock();
    CHECK(command_owner == -1 && command_depth == 0);
    current_task = 9;
    CHECK(shell_command_try_lock());
    CHECK(command_owner == 9 && command_depth == 1);
    shell_command_release();
    CHECK(command_owner == -1 && !yields);
    console_tests();
    reboot_tests();
    printf("Shell remote admission: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
