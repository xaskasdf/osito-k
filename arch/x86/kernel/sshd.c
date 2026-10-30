/* Opt-in SSH maintenance service. Authentication/protocol: TinySSH (0BSD). */
#include "sshd.h"
#include "net.h"
#include "random_secure.h"
#include "shell_remote.h"
#include "file_sync.h"
#include "screen_capture.h"
#include "driver_manager.h"
#include "../lib/tinyssh/osito_ssh.h"

extern void serial_puts(const char *);
extern void boot_diag_mark(const char *);
extern void sh_puts(const char *);
extern uint64_t idt_get_monotonic_ns(void);
extern void sched_yield(void);
extern int sched_spawn(const char *, void (*)(void));
extern bool sched_is_enabled(void);
extern bool osfs2_is_mounted(void);
extern void *osfs2_find(const char *);
extern void *osfs2_create(const char *, uint64_t);
extern uint64_t osfs2_file_size(void *);
extern int osfs2_read(void *, uint64_t, void *, uint64_t);
extern int osfs2_write(void *, uint64_t, const void *, uint64_t);
extern int osfs2_rename(const char *, const char *, bool);
extern int osfs2_get_mode(const void *, uint16_t *);
extern int osfs2_set_mode(void *, uint16_t);
extern int disk_flush(void);

#define HOST_KEY ".ssh_host_ed25519"
#define STAGED_KEY ".ssh_host_ed25519.new"
#define AUTH_KEYS ".ssh_authorized_keys"

/* 0 stopped, 1 starting, 2 running, 3 stopping. Only worker releases live I/O. */
static unsigned service_state;
static int listener = -1, connection = -1;
static uint16_t listen_port;
static uint64_t link_epoch;
static osito_ssh_keys_t credentials;
static char authorized_text[16385];
static bool automatic_suspended;
static screen_capture_ops_t session_capture_ops;

static void erase(void *p, size_t n)
{
    volatile unsigned char *s = p;
    while (n--) *s++ = 0;
}

static int ssh_recv(void *arg, void *p, unsigned n)
{ (void)arg; return net_tcp_recv(connection, p, n); }
static int ssh_send(void *arg, const void *p, unsigned n)
{ (void)arg; return net_tcp_send(connection, p, n); }
static void ssh_wait(void *arg)
{ (void)arg; net_poll(); sched_yield(); }
static unsigned long long ssh_clock(void *arg)
{ (void)arg; return idt_get_monotonic_ns() / 1000000; }
static int ssh_cancelled(void *arg)
{
    (void)arg;
    return __atomic_load_n(&service_state, __ATOMIC_ACQUIRE) != 2 ||
           net_link_epoch() != link_epoch;
}
static int ssh_random(void *arg, void *p, unsigned n)
{ (void)arg; return random_secure_bytes(p, n); }
static int ssh_capture_cancelled(void)
{
    return ssh_cancelled(NULL) || net_tcp_state(connection) != TCP_ESTABLISHED;
}
static void ssh_log(void *arg, const char *s)
{ (void)arg; serial_puts("[SSHD] "); serial_puts(s); serial_puts("\n"); }
static int ssh_command(void *arg, char *line, osito_ssh_output_fn out, void *context)
{ (void)arg; return shell_remote_execute(line, out, context); }
static const osito_ssh_ops_t ssh_ops = {
    ssh_recv, ssh_send, ssh_wait, ssh_clock, ssh_cancelled, ssh_random,
    ssh_log, ssh_command, NULL, file_sync_request, file_sync_reset,
    screen_capture_request, screen_capture_reset
};

static const char *load_host_key(bool create)
{
    if (!osfs2_is_mounted()) return "no filesystem mounted";
    void *file = osfs2_find(HOST_KEY);
    if (!file && create) {
        if (osfs2_find(STAGED_KEY)) return "unfinished host key file exists; inspect .ssh_host_ed25519.new";
        if (osito_ssh_keygen(&ssh_ops, credentials.host)) return "secure entropy/key generation failed";
        file = osfs2_create(STAGED_KEY, sizeof(credentials.host));
        if (!file || osfs2_set_mode(file, 0600) < 0 ||
            osfs2_write(file, 0, credentials.host, sizeof(credentials.host)) < 0 ||
            disk_flush() < 0) return "cannot persist host key";
        unsigned char verify[OSITO_SSH_HOST_KEY_BYTES];
        bool ok = osfs2_read(file, 0, verify, sizeof(verify)) >= 0 &&
                  memcmp(verify, credentials.host, sizeof(verify)) == 0;
        erase(verify, sizeof(verify));
        if (!ok) return "host key readback failed";
        if (osfs2_rename(STAGED_KEY, HOST_KEY, false) < 0 || disk_flush() < 0)
            return "cannot commit host key";
        file = osfs2_find(HOST_KEY);
    }
    if (!file) return "no host key; use sshd keygen";
    uint16_t mode;
    if (osfs2_get_mode(file, &mode) < 0 || (mode & 0170000) != 0100000 ||
        (mode & 0077)) return "host key must be a regular file with mode 600";
    if (osfs2_file_size(file) != sizeof(credentials.host) ||
        osfs2_read(file, 0, credentials.host, sizeof(credentials.host)) < 0 ||
        osito_ssh_keycheck(&ssh_ops, credentials.host)) return "invalid host key (not replaced)";
    return NULL;
}

static const char *load_authorized_keys(void)
{
    void *file = osfs2_find(AUTH_KEYS);
    if (!file) return "missing .ssh_authorized_keys (OpenSSH Ed25519 public keys)";
    uint16_t mode;
    if (osfs2_get_mode(file, &mode) < 0 || (mode & 0170000) != 0100000)
        return "authorized keys must be a regular file";
    uint64_t len = osfs2_file_size(file);
    if (!len || len > 16384 || osfs2_read(file, 0, authorized_text, len) < 0 ||
        osito_ssh_authorize(&credentials, authorized_text, (unsigned)len))
        return "invalid authorized keys; only plain ssh-ed25519 lines supported";
    erase(authorized_text, sizeof(authorized_text));
    return NULL;
}

static void ssh_worker(void)
{
    file_sync_init(&file_sync_osfs);
    session_capture_ops = screen_capture_gop;
    session_capture_ops.cancelled = ssh_capture_cancelled;
    screen_capture_init(&session_capture_ops);
    while (__atomic_load_n(&service_state, __ATOMIC_ACQUIRE) == 2 &&
           net_link_epoch() == link_epoch) {
        int conn = net_tcp_accept(listener, 10);
        if (conn < 0) continue;
        __atomic_store_n(&connection, conn, __ATOMIC_RELEASE);
        net_tcp_set_keepalive(conn, 1, 3000, 1000);
        ssh_log(NULL, "connection accepted");
        int result = osito_ssh_session(&ssh_ops, &credentials);
        if (result < 0) {
            net_tcp_abort(conn);
            net_tcp_release(conn);
        } else net_tcp_close_timeout(conn, 100);
        __atomic_store_n(&connection, -1, __ATOMIC_RELEASE);
        ssh_log(NULL, result < 0 ? "connection closed (error/cancel/timeout)" : "session closed");
    }
    net_tcp_stop_listen(listener);
    listener = -1;
    erase(&credentials, sizeof(credentials));
    ssh_log(NULL, "stopped");
    __atomic_store_n(&service_state, 0, __ATOMIC_RELEASE);
}

static bool claim_service(void)
{
    unsigned expected = 0;
    return __atomic_compare_exchange_n(&service_state, &expected, 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void print_number(unsigned n)
{
    char text[12]; unsigned i = sizeof(text) - 1;
    text[i] = 0;
    do { text[--i] = '0' + n % 10; n /= 10; } while (n);
    sh_puts(text + i);
}

static const char *start_service(uint16_t port, bool create_key)
{
    if (!port || !claim_service()) return "service busy";
    boot_diag_mark("sshd-start");
    const char *error = load_host_key(create_key);
    if (!error) error = sched_is_enabled() ? load_authorized_keys() : "scheduler is disabled";
    if (!error) {
        boot_diag_mark("sshd-keys-ready");
        link_epoch = net_link_epoch();
        listener = net_tcp_listen(port);
        if (listener < 0) error = "cannot listen";
        else {
            listen_port = port;
            net_tcp_set_listen_backlog(listener, 1);
            __atomic_store_n(&service_state, 2, __ATOMIC_RELEASE);
            if (sched_spawn("sshd", ssh_worker) >= 0) {
                boot_diag_mark("sshd-ready");
                return NULL;
            }
            net_tcp_stop_listen(listener); listener = -1;
            error = "cannot create SSH worker";
        }
    }
    erase(&credentials, sizeof(credentials));
    erase(authorized_text, sizeof(authorized_text));
    __atomic_store_n(&service_state, 0, __ATOMIC_RELEASE);
    ssh_log(NULL, error);
    boot_diag_mark("sshd-start-failed");
    return error;
}

const char *sshd_ensure_running(uint16_t port)
{
    if (__atomic_load_n(&automatic_suspended, __ATOMIC_ACQUIRE)) return "manually stopped for this boot";
    if (__atomic_load_n(&service_state, __ATOMIC_ACQUIRE) == 2) return NULL;
    /* Never regenerate identity or silently replace keys during auto-reconnect. */
    return start_service(port, false);
}

void sshd_command(int argc, char **argv)
{
    const char *command = argc > 1 ? argv[1] : "status";
    if (!strcmp(command, "autostart") && (argc == 3 || argc == 4) &&
        (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        unsigned port = 22;
        if (argc == 4) {
            port = 0;
            for (const char *p = argv[3]; *p; p++) {
                if (*p < '0' || *p > '9' || port > 6553) { port = 0; break; }
                port = port * 10 + (unsigned)(*p - '0');
            }
        }
        if (!port || port > 65535 || driver_sshd_autostart(!strcmp(argv[2], "on"), port))
            sh_puts("sshd: cannot persist autostart policy\n");
        else {
            __atomic_store_n(&automatic_suspended, false, __ATOMIC_RELEASE);
            sh_puts("sshd: autostart policy saved; existing host/authorized keys required\n");
        }
        return;
    }
    if (!strcmp(command, "status") && argc <= 2) {
        unsigned state = __atomic_load_n(&service_state, __ATOMIC_ACQUIRE);
        sh_puts(state == 2 ? "sshd: listening" : state ? "sshd: starting/stopping" : "sshd: stopped");
        if (state == 2) {
            sh_puts(" on TCP "); print_number(listen_port);
            sh_puts(__atomic_load_n(&connection, __ATOMIC_ACQUIRE) >= 0 ? ", session active" : ", idle");
        }
        sh_puts("\n");
        return;
    }
    if (!strcmp(command, "stop") && argc == 2) {
        __atomic_store_n(&automatic_suspended, true, __ATOMIC_RELEASE);
        unsigned expected = 2;
        bool stopping = __atomic_compare_exchange_n(&service_state, &expected, 3,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        sh_puts(stopping ? "sshd: stopping\n" : "sshd: not running\n");
        return;
    }
    bool keygen = !strcmp(command, "keygen") && argc == 2;
    bool key = !strcmp(command, "key") && argc == 2;
    bool start = !strcmp(command, "start") && (argc == 2 || argc == 3);
    unsigned port = 22;
    if (start && argc == 3) {
        port = 0;
        const char *p = argv[2];
        if (!*p) start = false;
        while (*p) {
            if (*p < '0' || *p > '9' || port > 6553) { start = false; break; }
            port = port * 10 + (unsigned)(*p++ - '0');
        }
        if (!port || port > 65535) start = false;
    }
    if (!keygen && !key && !start) {
        sh_puts("sshd status\nsshd keygen\nsshd key\nsshd start [port]\nsshd stop\nsshd autostart on [port]\nsshd autostart off\n");
        return;
    }
    if (start) {
        const char *error = start_service((uint16_t)port, true);
        if (error) { sh_puts("sshd: "); sh_puts(error); sh_puts("\n"); }
        else {
            __atomic_store_n(&automatic_suspended, false, __ATOMIC_RELEASE);
            sh_puts("sshd: listening on TCP "); print_number(port);
            sh_puts("; user osito, public key only\n");
        }
        return;
    }
    if (!claim_service()) { sh_puts("sshd: stop the service before changing/reading keys\n"); return; }
    const char *error = load_host_key(keygen || start);
    if (!error && (keygen || key)) {
        char public_key[82]; osito_ssh_public_key(credentials.host, public_key);
        sh_puts(public_key);
    }
    if (error) { sh_puts("sshd: "); sh_puts(error); sh_puts("\n"); }
    erase(&credentials, sizeof(credentials));
    erase(authorized_text, sizeof(authorized_text));
    __atomic_store_n(&service_state, 0, __ATOMIC_RELEASE);
}
