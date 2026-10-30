/* Real OpenSSH interop harness for the same freestanding protocol engine. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../lib/tinyssh/osito_ssh.h"
#ifdef OSITO_SYNC_TEST
#include "../kernel/file_sync.h"
extern void file_sync_host_init(const char *);
#endif
#ifdef OSITO_SCREEN_TEST
#include "../kernel/screen_capture.h"
extern void screen_capture_host_init(void);
#endif

static int fd = -1, entropy_fail;
static int entropy_remaining = -1;
static unsigned tx_calls;
static const char *session_fault;
static unsigned long long clock_offset;
static int fault_is(const char *name)
{ return session_fault && !strcmp(session_fault, name); }
static int receive(void *a, void *b, unsigned n)
{
    (void)a;
    if (fault_is("receive-eof")) return -1;
    if (fault_is("receive-timeout")) return 0;
    ssize_t r = recv(fd, b, n > 17 ? 17 : n, MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return r > 0 ? (int)r : -1;
}
static int transmit(void *a, const void *b, unsigned n)
{
    (void)a;
    if (fault_is("send-closed")) { errno = EPIPE; return -1; }
    if (fault_is("send-timeout")) return 0;
    if (++tx_calls % 7 == 0) return 0;
    return (int)send(fd, b, n > 37 ? 37 : n, MSG_NOSIGNAL);
}
static void wait_io(void *a)
{
    (void)a;
    if (fault_is("send-timeout") || fault_is("receive-timeout")) {
        clock_offset += 30001;
        return;
    }
    struct pollfd p = {fd, POLLIN, 0}; poll(&p, 1, 1);
}
static unsigned long long now(void *a)
{ (void)a; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000 + clock_offset; }
static int cancelled(void *a) { (void)a; return fault_is("cancelled"); }
static int random_data(void *a, void *p, unsigned n)
{
    (void)a;
    if (entropy_fail || entropy_remaining == 0 || fault_is("entropy")) return -1;
    if (entropy_remaining > 0) entropy_remaining--;
    return getrandom(p, n, 0) == (ssize_t)n ? 0 : -1;
}
static void message(void *a, const char *s) { (void)a; fprintf(stderr, "[SSHD] %s\n", s); }
static int command(void *a, char *s, osito_ssh_output_fn emit, void *out)
{
    (void)a;
    if (!strncmp(s, "echo ", 5)) { emit(out, s + 5, strlen(s + 5)); emit(out, "\n", 1); return 0; }
    if (!strcmp(s, "large")) {
        char data[1024]; memset(data, 'x', sizeof(data));
        for (unsigned i = 0; i < 128; i++) emit(out, data, sizeof(data));
        return 0;
    }
    if (!strcmp(s, "overflow")) {
        char data[1024]; memset(data, 'z', sizeof(data));
        for (unsigned i = 0; i < 1100; i++) emit(out, data, sizeof(data));
        return 0;
    }
    emit(out, "unsupported\n", 12); return 127;
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    osito_ssh_ops_t ops = {receive,transmit,wait_io,now,cancelled,random_data,message,command,0,0,0,0,0};
    osito_ssh_keys_t keys = {0};
    entropy_fail = 1;
    if (osito_ssh_keygen(&ops, keys.host) != -1) return 3;
    entropy_fail = 0;
    entropy_remaining = 1;
    if (osito_ssh_keygen(&ops, keys.host) != -1) return 14;
    for (unsigned i = 0; i < sizeof(keys.host); i++) if (keys.host[i]) return 15;
    entropy_remaining = -1;
    FILE *f = fopen(argv[2], "rb");
    if (f) { if (fread(keys.host, 1, sizeof(keys.host), f) != sizeof(keys.host)) return 4; fclose(f); }
    else {
        if (osito_ssh_keygen(&ops, keys.host)) return 5;
        f = fopen(argv[2], "wb"); if (!f) return 6;
        if (fwrite(keys.host, 1, sizeof(keys.host), f) != sizeof(keys.host)) return 7;
        fclose(f);
    }
    if (osito_ssh_keycheck(&ops, keys.host)) return 8;
    unsigned char saved = keys.host[32]; keys.host[32] ^= 1;
    if (osito_ssh_keycheck(&ops, keys.host) != -1) return 9;
    keys.host[32] = saved;
    f = fopen(argv[3], "rb"); if (!f) return 10;
    char text[16385]; unsigned n = (unsigned)fread(text, 1, sizeof(text), f); fclose(f);
    if (osito_ssh_authorize(&keys, text, n)) return 11;
    char public_key[82]; osito_ssh_public_key(keys.host, public_key);
    fputs(public_key, stderr);
    int server = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family=AF_INET,.sin_port=htons((unsigned short)atoi(argv[1])),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) || listen(server, 4)) return 12;
    puts("READY"); fflush(stdout);
#ifdef OSITO_SYNC_TEST
    file_sync_host_init(getenv("OSITO_SYNC_ROOT"));
    ops.file_request = file_sync_request;
    ops.file_reset = file_sync_reset;
#endif
#ifdef OSITO_SCREEN_TEST
    screen_capture_host_init();
    ops.screen_request = screen_capture_request;
    ops.screen_reset = screen_capture_reset;
#endif
    unsigned sessions = 0;
    for (;;) {
        fd = accept(server, 0, 0); if (fd < 0) return 13;
        session_fault = sessions++ == 0 ? getenv("OSITO_SSH_TEST_FAULT") : NULL;
        clock_offset = 0;
        int rc = osito_ssh_session(&ops, &keys);
        session_fault = NULL;
        clock_offset = 0;
        close(fd); fprintf(stderr, "SESSION %d\n", rc);
    }
}
