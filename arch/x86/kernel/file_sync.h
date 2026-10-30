#ifndef OSITO_FILE_SYNC_H
#define OSITO_FILE_SYNC_H

#define FILE_SYNC_BLOCK 32768u
#define FILE_SYNC_FRAME (FILE_SYNC_BLOCK + 32u)
#define FILE_SYNC_PATH 260u

enum { SYNC_OK, SYNC_MISSING, SYNC_END, SYNC_INVALID, SYNC_IO,
       SYNC_CHANGED, SYNC_BUSY, SYNC_HASH };

/* One authenticated session owns this state. Callbacks never unwind through it. */
typedef struct {
    int (*stat)(const char *, unsigned *, unsigned long long *);
    int (*list)(const char *);
    int (*next)(char *, unsigned, unsigned *);
    int (*open)(const char *, unsigned long long *);
    int (*read)(unsigned long long, void *, unsigned);
    void (*close)(void);
    int (*begin)(const char *, unsigned long long);
    int (*write)(unsigned long long, const void *, unsigned);
    int (*readback)(unsigned long long, void *, unsigned);
    int (*commit)(void);
    void (*abort)(void);
    int (*mkdir)(const char *);
} file_sync_ops_t;

void file_sync_init(const file_sync_ops_t *);
void file_sync_reset(void *);
int file_sync_request(void *, const unsigned char *, unsigned,
                      unsigned char *, unsigned, int *);
extern const file_sync_ops_t file_sync_osfs;

#endif
