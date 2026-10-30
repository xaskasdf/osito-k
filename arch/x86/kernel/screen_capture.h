#ifndef OSITO_SCREEN_CAPTURE_H
#define OSITO_SCREEN_CAPTURE_H

#define SCREEN_BLOCK 32768u
#define SCREEN_FRAME (SCREEN_BLOCK + 32u)
#define SCREEN_MAX_PIXELS (16u * 1024u * 1024u)
#define SCREEN_MAX_DIMENSION 16384u
#define SCREEN_METADATA 72u
#define SCREEN_CAPTURE_TIMEOUT_MS 30000u
#define SCREEN_CAPTURE_SLICE_MS 4u

enum {
    SCREEN_OK, SCREEN_UNAVAILABLE, SCREEN_INVALID, SCREEN_NOMEM,
    SCREEN_UNSUPPORTED, SCREEN_BUSY, SCREEN_CHANGED,
    SCREEN_CANCELLED, SCREEN_TIMEOUT
};

typedef struct {
    const volatile unsigned *pixels;
    unsigned long long bytes;
    unsigned width, height, pitch, pixel_format; /* GOP 0 RGBX, 1 BGRX */
} screen_view_t;

typedef struct {
    int (*view)(screen_view_t *);
    void *(*allocate)(unsigned long long);
    void (*release)(void *);
    unsigned long long (*now_ms)(void);
    int (*cancelled)(void);
    void (*yield)(void);
} screen_capture_ops_t;

void screen_capture_init(const screen_capture_ops_t *);
void screen_capture_reset(void *);
int screen_capture_request(void *, const unsigned char *, unsigned,
                           unsigned char *, unsigned, int *);
extern const screen_capture_ops_t screen_capture_gop;

#endif
