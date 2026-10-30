#ifndef OSITO_SSH_CORE_H
#define OSITO_SSH_CORE_H

#define OSITO_SSH_HOST_KEY_BYTES 96
#define OSITO_SSH_MAX_KEYS 32
#define OSITO_SSH_COMMAND_MAX 32768

typedef struct {
    unsigned char host[OSITO_SSH_HOST_KEY_BYTES]; /* public[32], secret[64] */
    char authorized[OSITO_SSH_MAX_KEYS][69];
    unsigned key_count;
} osito_ssh_keys_t;

typedef void (*osito_ssh_output_fn)(void *, const char *, unsigned long);
typedef struct {
    /* recv/send: bytes, 0 would-block, -1 EOF/error. */
    int (*recv)(void *, void *, unsigned);
    int (*send)(void *, const void *, unsigned);
    void (*wait)(void *);
    unsigned long long (*now_ms)(void *);
    int (*cancelled)(void *);
    int (*random)(void *, void *, unsigned);
    void (*log)(void *, const char *);
    int (*command)(void *, char *, osito_ssh_output_fn, void *);
    void *arg;
    /* Complete bounded request/response, without terminal conversion/capture. */
    int (*file_request)(void *, const unsigned char *, unsigned,
                        unsigned char *, unsigned, int *);
    void (*file_reset)(void *);
    int (*screen_request)(void *, const unsigned char *, unsigned,
                          unsigned char *, unsigned, int *);
    void (*screen_reset)(void *);
} osito_ssh_ops_t;

int osito_ssh_keygen(const osito_ssh_ops_t *, unsigned char *host);
int osito_ssh_keycheck(const osito_ssh_ops_t *, const unsigned char *host);
int osito_ssh_authorize(osito_ssh_keys_t *, const char *text, unsigned len);
void osito_ssh_public_key(const unsigned char *host, char line[82]);
/* Serialized by the service. Errors close this connection, not the OS. */
int osito_ssh_session(const osito_ssh_ops_t *, const osito_ssh_keys_t *);

#endif
