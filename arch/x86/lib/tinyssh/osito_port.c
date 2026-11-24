/* TinySSH OS boundary. The protocol/crypto in upstream/ remains unchanged. */
#include "osito_ssh.h"
#include "../../kernel/file_sync.h"
#include "packet.h"
#include "packetparser.h"
#include "ssh.h"
#include "byte.h"
#include "str.h"
#include "purge.h"
#include "bug.h"
#include "numtostr.h"
#include "subprocess.h"
#include <unistd.h>
#include <pwd.h>
#include <poll.h>

#ifdef OSITO_SSH_HOST_TEST
#include <setjmp.h>
static jmp_buf failure;
#define save_failure() setjmp(failure)
#define restore_failure() longjmp(failure, 1)
#else
extern int kern_setjmp(unsigned long long *) __attribute__((returns_twice));
extern void kern_longjmp(unsigned long long *, int) __attribute__((noreturn));
static unsigned long long failure[9];
#define save_failure() kern_setjmp(failure)
#define restore_failure() kern_longjmp(failure, 1)
#endif

static const osito_ssh_ops_t *ops;
static const osito_ssh_keys_t *keys;
static unsigned long long deadline, transferred;
static unsigned long long received, sent;
static const char *session_phase, *failure_reason;
static unsigned char scratch1[65536], scratch2[65536];
static struct buf b1, b2;
struct channel channel;
int tssh_errno;
int loglevel = 1;

#define OUTPUT_CAPACITY (1024u * 1024u)
static char output[OUTPUT_CAPACITY];
static unsigned output_pos, output_len;
static int output_overflow, exit_status, done, pending, interactive;
static char command[OSITO_SSH_COMMAND_MAX];
static unsigned command_len;
static int line_overflow, last_cr, escape;
static int binary;
static const char sync_subsystem[] = "osito-sync-v1";
static const char screen_subsystem[] = "osito-screen-v1";

static int text_equal(const char *a, const char *b)
{ return str_equaln(a, str_len(a), b); }

void global_die(int code)
{
    (void)code;
    restore_failure();
}

static void check_cancel(void)
{
    if (!ops || ops->cancelled(ops->arg)) failure_reason = "cancelled";
    else if (ops->now_ms(ops->arg) >= deadline) failure_reason = "deadline";
    else if (!binary && transferred >= (1ULL << 30)) failure_reason = "traffic-limit";
    else return;
    global_die(111);
}

void randombytes(void *buf, long long len)
{
    if (!ops || len < 0 || len > 65536 ||
        ops->random(ops->arg, buf, (unsigned)len) != 0) {
        failure_reason = "entropy";
        global_die(111);
    }
}

void trymlock(void *p, long long n) { (void)p; (void)n; }
void trymunlock(void *p, long long n) { (void)p; (void)n; }
char *log_string(void) { return (char *)"OsitoK"; }

void log_9_(int level, int unused, const char *file, unsigned long long line,
            const char *a, const char *b, const char *c, const char *d,
            const char *e, const char *f, const char *g, const char *h,
            const char *i)
{
    (void)level; (void)unused; (void)file; (void)line;
    const char *parts[] = {a,b,c,d,e,f,g,h,i};
    char text[256];
    unsigned n = 0;
    for (unsigned k = 0; k < 9; k++) {
        const char *p = parts[k];
        while (p && *p && n + 1 < sizeof(text)) {
            unsigned char ch = (unsigned char)*p++;
            text[n++] = ch >= 32 && ch <= 126 ? ch : '?';
        }
    }
    text[n] = 0;
    if (ops && ops->log) ops->log(ops->arg, text);
}

ssize_t tssh_read(int fd, void *buf, size_t len)
{
    check_cancel();
    if (fd != 0 || len > 65536) { errno = EBADF; return -1; }
    int n = ops->recv(ops->arg, buf, (unsigned)len);
    if (!n) { errno = EAGAIN; return -1; }
    if (n < 0) { failure_reason = "read-closed"; return 0; }
    transferred += (unsigned)n;
    received += (unsigned)n;
    return n;
}

ssize_t tssh_write(int fd, const void *buf, size_t len)
{
    check_cancel();
    if (fd != 1 || len > 1024 * 1024) { errno = EBADF; return -1; }
    int n = ops->send(ops->arg, buf, (unsigned)len);
    if (!n) { errno = EAGAIN; return -1; }
    if (n < 0) { failure_reason = "write-closed"; errno = EPIPE; return -1; }
    transferred += (unsigned)n;
    sent += (unsigned)n;
    return n;
}

int writeall(int fd, const void *buf, long long len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        long n = tssh_write(fd, p, (size_t)len);
        if (n < 0 && errno == EAGAIN) {
            ops->wait(ops->arg);
            continue;
        }
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int tssh_poll(struct pollfd *fds, unsigned long n, int timeout)
{
    (void)timeout;
    check_cancel();
    ops->wait(ops->arg);
    for (unsigned long i = 0; i < n; i++) fds[i].revents = fds[i].events;
    return (int)n;
}

int getln(int fd, void *buf, long long capacity)
{
    unsigned char *p = buf;
    for (long long i = 0; i + 1 < capacity;) {
        long n = tssh_read(fd, p + i, 1);
        if (!n) return 0;
        if (n < 0) {
            if (errno != EAGAIN) return -1;
            ops->wait(ops->arg);
            continue;
        }
        if (!p[i]) return -1;
        if (p[i++] == '\n') { p[i] = 0; return 1; }
    }
    return -1;
}

/* Never enable the upstream optional "none" authentication branch. */
uid_t tssh_geteuid(void) { return 0; }
struct passwd *tssh_getpwnam(const char *name) { (void)name; return 0; }

int subprocess_auth(const char *user, const char *algorithm, const char *key)
{
    if (!keys || !text_equal(user, "osito") ||
        !text_equal(algorithm, "ssh-ed25519") || str_len(key) != 68) return -1;
    unsigned found = 0;
    for (unsigned i = 0; i < keys->key_count; i++)
        found |= (unsigned)byte_isequal(key, 68, keys->authorized[i]);
    return found ? 0 : -1;
}

int subprocess_sign(unsigned char *sig, long long capacity, const char *dir,
                    unsigned char *hash, long long len)
{
    (void)dir;
    unsigned char signed_hash[128];
    unsigned long long n = 0;
    if (!keys || capacity != 64 || len < 0 || len > 64) return -1;
    int result = crypto_sign_ed25519(signed_hash, &n, hash, len, keys->host + 32);
    if (!result && n == (unsigned long long)len + 64)
        byte_copy(sig, 64, signed_hash);
    else result = -1;
    purge(signed_hash, sizeof(signed_hash));
    return result;
}

void osito_ssh_public_key(const unsigned char *host, char line[82])
{
    struct buf b = {(unsigned char *)line, 0, 82};
    buf_puts(&b, "ssh-ed25519 ");
    ed25519_putsignpkbase64(&b, host);
    buf_putnum8(&b, '\n');
    line[b.len] = 0;
}

static int key_consistent(const unsigned char *host)
{
    static const unsigned char message[] = "OsitoK host key consistency";
    unsigned char signed_message[128], recovered[128];
    unsigned long long n = 0, m = 0;
    int ok = byte_isequal(host, 32, host + 64) &&
        crypto_sign_ed25519(signed_message, &n, message, sizeof(message), host + 32) == 0 &&
        crypto_sign_ed25519_open(recovered, &m, signed_message, n, host) == 0 &&
        m == sizeof(message) && byte_isequal(recovered, m, message);
    purge(signed_message, sizeof(signed_message));
    purge(recovered, sizeof(recovered));
    return ok ? 0 : -1;
}

int osito_ssh_keycheck(const osito_ssh_ops_t *io, const unsigned char *host)
{
    ops = io;
    volatile int result = -1;
    if (save_failure() == 0) result = key_consistent(host);
    ops = 0;
    return result;
}

int osito_ssh_keygen(const osito_ssh_ops_t *io, unsigned char *host)
{
    ops = io;
    volatile int result = -1;
    if (save_failure() == 0) {
        result = crypto_sign_ed25519_keypair(host, host + 32);
        if (!result) result = key_consistent(host);
    } else result = -1;
    if (result) purge(host, OSITO_SSH_HOST_KEY_BYTES);
    ops = 0;
    return result;
}

static int base64_digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int osito_ssh_authorize(osito_ssh_keys_t *k, const char *text, unsigned len)
{
    static const unsigned char prefix[19] = {
        0,0,0,11,'s','s','h','-','e','d','2','5','5','1','9',0,0,0,32
    };
    k->key_count = 0;
    purge(k->authorized, sizeof(k->authorized));
    if (!text || len > 16384) return -1;
    for (unsigned pos = 0; pos < len;) {
        unsigned end = pos;
        while (end < len && text[end] != '\n') {
            if (!text[end]) goto invalid;
            end++;
        }
        unsigned start = pos;
        pos = end < len ? end + 1 : end;
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) start++;
        if (start == end || text[start] == '#') continue;
        if (end - start < 80 || !byte_isequal(text + start, 12, "ssh-ed25519 ") ||
            k->key_count == OSITO_SSH_MAX_KEYS) goto invalid;
        start += 12;
        unsigned char blob[51];
        for (unsigned j = 0; j < 17; j++) {
            unsigned value = 0;
            for (unsigned d = 0; d < 4; d++) {
                int v = base64_digit((unsigned char)text[start + 4 * j + d]);
                if (v < 0) goto invalid;
                value = (value << 6) | (unsigned)v;
            }
            blob[j * 3] = value >> 16;
            blob[j * 3 + 1] = value >> 8;
            blob[j * 3 + 2] = value;
        }
        if (!byte_isequal(blob, 19, prefix)) goto invalid;
        unsigned tail = start + 68;
        if (tail < end && text[tail] != ' ' && text[tail] != '\t' && text[tail] != '\r') goto invalid;
        byte_copy(k->authorized[k->key_count++], 68, text + start);
    }
    return k->key_count ? 0 : -1;
invalid:
    purge(k->authorized, sizeof(k->authorized));
    k->key_count = 0;
    return -1;
}

static void console_output(void *arg, const char *data, unsigned long len)
{
    (void)arg;
    /* The command callback must always return normally to release its I/O. */
    if (len > OUTPUT_CAPACITY - 128 - output_len) {
        output_overflow = 1;
        return;
    }
    byte_copy(output + output_len, len, data);
    output_len += len;
}

static void console_text(const char *s) { console_output(0, s, str_len(s)); }

int channel_open(const char *user, crypto_uint32 id, crypto_uint32 window,
                 crypto_uint32 maxpacket, crypto_uint32 *localwindow)
{
    if (channel.maxpacket || channel.pid || !text_equal(user, "osito")) return 0;
    channel.id = id;
    channel.maxpacket = maxpacket;
    channel.remotewindow = window;
    channel.localwindow = *localwindow = CHANNEL_BUFSIZE;
    return 1;
}

int channel_openterminal(const char *name, crypto_uint32 a, crypto_uint32 b,
                         crypto_uint32 x, crypto_uint32 y)
{
    if (!channel.maxpacket || channel.pid || channel.flagterminal) return 0;
    if (!str_copyn(channel.termname, sizeof(channel.termname), name)) return 0;
    channel.flagterminal = 1;
    channel.a = a; channel.b = b; channel.x = x; channel.y = y;
    return 1;
}
void channel_ptyresize(crypto_uint32 a, crypto_uint32 b, crypto_uint32 x, crypto_uint32 y)
{
    channel.a = a; channel.b = b; channel.x = x; channel.y = y;
}
const char *channel_subsystem_get(const char *s)
{
    if (channel.flagterminal) return 0;
    if (ops->file_request && ops->file_reset && text_equal(s, sync_subsystem))
        return sync_subsystem;
    if (ops->screen_request && ops->screen_reset && text_equal(s, screen_subsystem))
        return screen_subsystem;
    return 0;
}

int channel_exec(const char *cmd)
{
    if (!channel.maxpacket || channel.pid) return 0;
    channel.pid = 1;
    binary = cmd == sync_subsystem ? 1 : cmd == screen_subsystem ? 2 : 0;
    if (binary) return 1;
    interactive = cmd == 0;
    if (cmd) {
        if (!str_copyn(command, sizeof(command), cmd)) return 0;
        pending = 1;
    } else {
        console_text("OsitoK remote console\r\nosito> ");
    }
    return 1;
}

void channel_put(unsigned char *buf, long long len)
{
    if (!channel.maxpacket || !channel.pid || len < 0 ||
        (unsigned long long)len > channel.localwindow || channel.remoteeof) bug_proto();
    if (done || (!interactive && !binary)) return;
    if (len > CHANNEL_BUFSIZE - channel.len0) bug_proto();
    byte_copy(channel.buf0 + channel.len0, len, buf);
    channel.len0 += len;
    channel.localwindow -= len;
}
void channel_putextended(long long len)
{
    if (!channel.maxpacket || !channel.pid || len < 0 ||
        (unsigned long long)len > channel.localwindow) bug_proto();
    channel.localwindow -= len;
}
void channel_puteof(void) { channel.remoteeof = 1; }
long long channel_getlen0(void) { return channel.len0; }
crypto_uint32 channel_getid(void) { return channel.id; }
crypto_uint32 channel_getlocalwindow(void) { return channel.localwindow; }
void channel_incrementlocalwindow(crypto_uint32 n)
{
    if (n > 0xffffffffU - channel.localwindow) bug_proto();
    channel.localwindow += n;
}
void channel_incrementremotewindow(crypto_uint32 n)
{
    if (n > 0xffffffffU - channel.remotewindow) bug_proto();
    channel.remotewindow += n;
}
long long channel_read(unsigned char *buf, long long len)
{
    unsigned n = output_len - output_pos;
    if (len < 0) bug_inval();
    if ((unsigned long long)len < n) n = (unsigned)len;
    if (channel.remotewindow < n) n = channel.remotewindow;
    byte_copy(buf, n, output + output_pos);
    purge(output + output_pos, n);
    output_pos += n;
    channel.remotewindow -= n;
    if (output_pos == output_len) output_pos = output_len = 0;
    return n;
}
long long channel_extendedread(unsigned char *buf, long long n) { (void)buf; (void)n; return 0; }

static void execute_command(void)
{
    if (interactive && (text_equal(command, "exit") || text_equal(command, "logout"))) {
        done = 1;
    } else if (!line_overflow) {
        exit_status = ops->command(ops->arg, command, console_output, 0);
    } else {
        console_text("Command too long; discarded.\r\n");
        exit_status = 2;
    }
    if (output_overflow) {
        static const char error[] = "\r\nsshd: output limit exceeded (1 MiB)\r\n";
        byte_copy(output + output_len, sizeof(error) - 1, error);
        output_len += sizeof(error) - 1;
        exit_status = 1;
        done = 1;
    }
    purge(command, sizeof(command));
    command_len = 0;
    line_overflow = 0;
    pending = 0;
    if (!interactive) done = 1;
    else if (!done) console_text("osito> ");
}

static void console_input(void)
{
    if (pending) { execute_command(); return; }
    if (!interactive || done || output_len) return;
    long long consumed = 0;
    while (consumed < channel.len0 && !done) {
        unsigned char c = channel.buf0[consumed++];
        if (escape) {
            if (escape == 1 && (c == '[' || c == 'O')) escape = 2;
            else if (escape == 1 || (c >= 0x40 && c <= 0x7e)) escape = 0;
            continue;
        }
        if (c == 27) { escape = 1; continue; }
        if (c == '\n' && last_cr) { last_cr = 0; continue; }
        last_cr = c == '\r';
        if (c == '\r' || c == '\n') {
            if (channel.flagterminal) console_text("\r\n");
            command[command_len] = 0;
            execute_command();
            break;
        }
        if (c == 3 || c == 21) {
            purge(command, sizeof(command)); command_len = 0; line_overflow = 0;
            if (channel.flagterminal) console_text("^C\r\nosito> ");
            continue;
        }
        if (c == 4 && !command_len) { done = 1; break; }
        if (c == 8 || c == 127) {
            if (command_len) {
                command[--command_len] = 0;
                if (channel.flagterminal) console_text("\b \b");
            }
            continue;
        }
        if (c < 32 && c != '\t') continue;
        if (command_len + 1 >= sizeof(command)) { line_overflow = 1; continue; }
        command[command_len++] = (char)c;
        if (channel.flagterminal) console_output(0, (const char *)&c, 1);
    }
    byte_copy(channel.buf0, channel.len0 - consumed, channel.buf0 + consumed);
    channel.len0 -= consumed;
    purge(channel.buf0 + channel.len0, consumed);
    if (channel.remoteeof && !channel.len0 && !done) {
        if (command_len) { command[command_len] = 0; execute_command(); }
        done = 1;
    }
}

static void binary_input(void)
{
    if (done || output_len) return;
    if (channel.len0 >= 4) {
        const unsigned char *p = channel.buf0;
        unsigned n = (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
                     (unsigned)p[2] << 8 | p[3];
        if (!n || n > FILE_SYNC_FRAME) { done = 1; exit_status = 2; return; }
        if ((unsigned long long)channel.len0 >= n + 4) {
            int finished = 0;
            int (*request)(void *, const unsigned char *, unsigned,
                            unsigned char *, unsigned, int *) =
                binary == 2 ? ops->screen_request : ops->file_request;
            int count = request(ops->arg, p + 4, n,
                    (unsigned char *)output + 4, FILE_SYNC_FRAME, &finished);
            if (count < 1 || count > (int)FILE_SYNC_FRAME) {
                done = 1; exit_status = 1; return;
            }
            output[0] = (unsigned)count >> 24;
            output[1] = (unsigned)count >> 16;
            output[2] = (unsigned)count >> 8;
            output[3] = count;
            output_len = (unsigned)count + 4;
            channel.len0 -= n + 4;
            byte_copy(channel.buf0, channel.len0, channel.buf0 + n + 4);
            purge(channel.buf0 + channel.len0, n + 4);
            done = finished;
            return;
        }
    }
    if (channel.remoteeof) { done = 1; exit_status = 1; }
}

int osito_ssh_session(const osito_ssh_ops_t *io, const osito_ssh_keys_t *authorized)
{
    ops = io; keys = authorized;
    volatile int result = -1;
    session_phase = "init";
    failure_reason = "protocol";
    received = sent = 0;
    binary = 0;
    if (save_failure() != 0) goto cleanup;
    if (ops->file_reset) ops->file_reset(ops->arg);
    if (ops->screen_reset) ops->screen_reset(ops->arg);
    deadline = ops->now_ms(ops->arg) + 30000;
    transferred = 0;
    output_pos = output_len = command_len = 0;
    done = pending = interactive = output_overflow = exit_status = 0;
    line_overflow = escape = last_cr = 0;
    errno = 0;
    purge(&channel, sizeof(channel));
    buf_init(&b1, scratch1, sizeof(scratch1));
    buf_init(&b2, scratch2, sizeof(scratch2));
    packet_init();
    sshcrypto_init();
    sshcrypto_kex_flags = 0;
    for (unsigned i = 0; sshcrypto_keys[i].name; i++) {
        sshcrypto_keys[i].sign_flagserver = sshcrypto_keys[i].sign_flagclient = 1;
        byte_copy(sshcrypto_keys[i].sign_publickey, 32, keys->host);
    }
    for (unsigned i = 0; sshcrypto_kexs[i].name; i++) sshcrypto_kexs[i].flagenabled = 1;
    for (unsigned i = 0; sshcrypto_ciphers[i].name; i++) sshcrypto_ciphers[i].flagenabled = 1;
    session_phase = "banner-send";
    if (!packet_hello_send()) goto cleanup;
    session_phase = "banner-receive";
    if (!packet_hello_receive()) goto cleanup;
    session_phase = "kex-send";
    if (!packet_kex_send()) goto cleanup;
    session_phase = "kex-receive";
    if (!packet_kex_receive()) goto cleanup;
rekey:
    session_phase = "key-exchange";
    deadline = ops->now_ms(ops->arg) + 30000;
    if (packet.flagkeys &&
        (!packet_kex_receive_rekey(&b1) || !packet_kex_send())) goto cleanup;
    if (!packet_kexdh(0, &b1, &b2)) goto cleanup;
    packet.flagkeys = 1;
    if (!packet.flagauthorized) {
        session_phase = "authentication";
        if (!packet_auth(&b1, &b2, 0)) goto cleanup;
        packet.flagauthorized = 1;
        if (ops->log) ops->log(ops->arg, "public key authenticated: osito");
    }
    deadline = ops->now_ms(ops->arg) + 600000;
    for (;;) {
        session_phase = "channel";
        check_cancel();
        if (!packet_sendall()) goto cleanup;
        if (packet.flagchanneleofreceived) { result = 0; break; }
        if (!packet_get(&b1, 0)) goto cleanup;
        if (b1.len) {
            deadline = ops->now_ms(ops->arg) + 600000;
            switch (b1.buf[0]) {
            case SSH_MSG_GLOBAL_REQUEST: packet_global_request(&b1); break;
            case SSH_MSG_CHANNEL_OPEN: packet_channel_open(&b1, &b2); break;
            case SSH_MSG_CHANNEL_REQUEST: packet_channel_request(&b1, &b2, 0); break;
            case SSH_MSG_CHANNEL_DATA: packet_channel_recv_data(&b1); break;
            case SSH_MSG_CHANNEL_EXTENDED_DATA: packet_channel_recv_extendeddata(&b1); break;
            case SSH_MSG_CHANNEL_WINDOW_ADJUST: packet_channel_recv_windowadjust(&b1); break;
            case SSH_MSG_CHANNEL_EOF: packet_channel_recv_eof(&b1); break;
            case SSH_MSG_CHANNEL_CLOSE: packet_channel_recv_close(&b1); break;
            case SSH_MSG_KEXINIT: goto rekey;
            default: packet_unimplemented(&b1); break;
            }
        } else {
            if (!packet_recv()) goto cleanup;
        }
        if (!packet.flagclosesent && channel.pid) {
            session_phase = binary == 2 ? "screen" : binary ? "file" : "console";
            if (binary) binary_input();
            else console_input();
            session_phase = "channel";
            packet_channel_send_windowadjust(&b2);
            if (output_len && channel.remotewindow) packet_channel_send_data(&b2);
            if (done && !output_len) {
                if (!packet_channel_send_close(&b2, 0, exit_status)) goto cleanup;
                deadline = ops->now_ms(ops->arg) + 5000;
            }
        }
        if (!b1.len && !packet_sendisready()) ops->wait(ops->arg);
    }
cleanup:
    if (result < 0 && ops->log) {
        /* Static stage/reason and transport counts only, never session data. */
        char rx[NUMTOSTR_LEN], tx[NUMTOSTR_LEN];
        log_9_(1, 0, __FILE__, __LINE__, "session failed phase=", session_phase,
                " reason=", failure_reason, " rx=", numtostr(rx, received),
                " tx=", numtostr(tx, sent), 0);
    }
    if (ops->file_reset) ops->file_reset(ops->arg);
    if (ops->screen_reset) ops->screen_reset(ops->arg);
    packet_purge();
    purge(&channel, sizeof(channel));
    purge(scratch1, sizeof(scratch1)); purge(scratch2, sizeof(scratch2));
    purge(command, sizeof(command)); purge(output, sizeof(output));
    for (unsigned i = 0; sshcrypto_keys[i].name; i++)
        purge(sshcrypto_keys[i].sign_publickey, sizeof(sshcrypto_keys[i].sign_publickey));
    ops = 0; keys = 0;
    return result;
}
