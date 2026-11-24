#ifndef OSITOK_BOOT_TRACE_H
#define OSITOK_BOOT_TRACE_H

/* Two hex digits identify the stage; the remaining digits carry EFI status.
 * No globals, allocator, UART or firmware calls are needed before BSS/IDT. */
static inline void boot_trace(unsigned long long base, unsigned width,
                              unsigned height, unsigned pitch, unsigned stage,
                              unsigned long long status)
{
    static const unsigned char hex[16][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}
    };
    if (!base || width < 228 || height < 20 || pitch < width)
        return;
    volatile unsigned *pixels = (volatile unsigned *)(__UINTPTR_TYPE__)base;
    unsigned x0 = width - 228, y0 = height - 20;
    for (unsigned y = 0; y < 20; y++) {
        for (unsigned x = 0; x < 228; x++) {
            unsigned col = x / 12, on = 0;
            if (col != 2 && y >= 3 && y < 17 && x % 12 < 10) {
                unsigned digit = col < 2 ? (stage >> ((1 - col) * 4)) & 15 :
                    (unsigned)(status >> ((18 - col) * 4)) & 15;
                on = hex[digit][(y - 3) / 2] & (1u << (4 - (x % 12) / 2));
            }
            pixels[(unsigned long long)(y0 + y) * pitch + x0 + x] =
                on ? 0x00ffffffu : 0x00202020u;
        }
    }
}

#endif
