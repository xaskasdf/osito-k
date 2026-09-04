/* Raw Linux-syscall OSS playback smoke test for OsitoK. */

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_POLL 7
#define SYS_IOCTL 16
#define SYS_DUP 32
#define SYS_EXIT 60

#define O_RDONLY 0
#define O_WRONLY 1
#define POLLOUT 4

#define AFMT_QUERY 0x00000000
#define AFMT_MU_LAW 0x00000001
#define AFMT_S16_LE 0x00000010
#define SNDCTL_DSP_SYNC 0x00005001
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_GETCAPS 0x8004500F
#define DSP_CAP_TRIGGER 0x00001000

typedef struct {
    int fd;
    short events;
    short revents;
} pollfd_t;

static long syscall1(long number, long arg1)
{
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result) : "a"(number), "D"(arg1)
        : "rcx", "r11", "memory");
    return result;
}

static long syscall2(long number, long arg1, long arg2)
{
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result) : "a"(number), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory");
    return result;
}

static long syscall3(long number, long arg1, long arg2, long arg3)
{
    long result;
    __asm__ volatile ("syscall"
        : "=a"(result)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory");
    return result;
}

static void print(const char *text)
{
    long length = 0;
    while (text[length])
        length++;
    syscall3(SYS_WRITE, 1, (long)text, length);
}

static void fail(int code)
{
    char message[] = "OSS ELF syscall test: FAIL 00\n";
    message[27] = (char)('0' + code / 10);
    message[28] = (char)('0' + code % 10);
    print(message);
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

void _start(void)
{
    static const char dsp[] = "/dev/dsp";
    static const char audio[] = "/dev/audio";

#ifdef OSS_EXPECT_NO_DEVICE
    if (syscall3(SYS_OPEN, (long)dsp, O_WRONLY, 0) != -19)
        fail(14);
    if (syscall3(SYS_OPEN, (long)audio, O_WRONLY, 0) != -19)
        fail(15);

    print("OSS ELF no-device test: PASS\n");
    syscall1(SYS_EXIT, 0);
    for (;;) {}
#else
    static int16_t samples[480 * 2];

    long fd = syscall3(SYS_OPEN, (long)dsp, O_WRONLY, 0);
    if (fd < 0)
        fail(1);

    int format = AFMT_S16_LE;
    int rate = 48000;
    int channels = 2;
    int caps = 0;
    if (syscall3(SYS_IOCTL, fd, SNDCTL_DSP_SETFMT,
                 (long)&format) < 0 || format != AFMT_S16_LE)
        fail(2);
    if (syscall3(SYS_IOCTL, fd, SNDCTL_DSP_SPEED,
                 (long)&rate) < 0 || rate != 48000)
        fail(3);
    if (syscall3(SYS_IOCTL, fd, SNDCTL_DSP_CHANNELS,
                 (long)&channels) < 0 || channels != 2)
        fail(4);
    if (syscall3(SYS_IOCTL, fd, SNDCTL_DSP_GETCAPS,
                 (long)&caps) < 0 || !(caps & DSP_CAP_TRIGGER))
        fail(5);

    pollfd_t pollfd = { (int)fd, POLLOUT, 0 };
    if (syscall3(SYS_POLL, (long)&pollfd, 1, 0) != 1 ||
        !(pollfd.revents & POLLOUT))
        fail(6);

    long duplicate = syscall1(SYS_DUP, fd);
    if (duplicate < 0 || syscall1(SYS_CLOSE, fd) < 0)
        fail(7);
    for (int frame = 0; frame < 480; frame++) {
        int16_t sample = (frame / 60) & 1 ? 12000 : -12000;
        samples[frame * 2] = sample;
        samples[frame * 2 + 1] = sample;
    }
    if (syscall3(SYS_WRITE, duplicate, (long)samples,
                 sizeof(samples)) != (long)sizeof(samples))
        fail(8);
    if (syscall3(SYS_IOCTL, duplicate, SNDCTL_DSP_SYNC, 0) < 0)
        fail(9);
    if (syscall1(SYS_CLOSE, duplicate) < 0)
        fail(10);

    fd = syscall3(SYS_OPEN, (long)audio, O_WRONLY, 0);
    format = AFMT_QUERY;
    if (fd < 0 || syscall3(SYS_IOCTL, fd, SNDCTL_DSP_SETFMT,
                            (long)&format) < 0 || format != AFMT_MU_LAW)
        fail(11);
    if (syscall1(SYS_CLOSE, fd) < 0)
        fail(12);
    if (syscall3(SYS_OPEN, (long)dsp, O_RDONLY, 0) != -19)
        fail(13);

    print("OSS ELF syscall test: PASS\n");
    syscall1(SYS_EXIT, 0);
    for (;;) {}
#endif
}
