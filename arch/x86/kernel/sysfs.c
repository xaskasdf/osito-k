/*
 * OsitoK x86-64 — sysfs (/sys) Virtual Filesystem
 *
 * Exposes device and kernel information as readable files.
 * /sys/class/block/  — block devices
 * /sys/class/net/    — network interfaces
 * /sys/devices/      — device tree
 * /sys/kernel/       — kernel parameters
 *
 * Read-only; generates content on open like procfs.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern uint64_t mem_get_free(void);
extern uint64_t mem_get_total(void);

/* Block device info */
extern int blkdev_count(void) __attribute__((weak));
extern const char *blkdev_name(int idx) __attribute__((weak));
extern uint64_t blkdev_size_mb(int idx) __attribute__((weak));

/* Network info */
extern void net_get_mac(uint8_t mac[6]) __attribute__((weak));
extern uint8_t *net_get_ip_ptr(void) __attribute__((weak));

/* ── sysfs Entry Generation ──────────────────────────────────── */

#define SYSFS_BUF_SIZE 2048

/* Generate content for a /sys path.
 * Returns bytes written to buf, or -1 if path not found. */
int sysfs_read(const char *path, char *buf, int max_len)
{
    if (!path || !buf || max_len < 16) return -1;
    int p = 0;

    /* Helper macros */
    #define SB_STR(s) do { const char *_s = (s); \
        while (*_s && p < max_len - 1) buf[p++] = *_s++; } while(0)
    #define SB_DEC(v) do { char _t[20]; int _n = 0; uint64_t _v = (v); \
        if (_v == 0) { if (p < max_len-1) buf[p++] = '0'; } \
        else { while (_v) { _t[_n++] = '0' + _v % 10; _v /= 10; } \
               for (int _i = _n-1; _i >= 0 && p < max_len-1; _i--) \
                   buf[p++] = _t[_i]; } } while(0)
    #define SB_NL() do { if (p < max_len-1) buf[p++] = '\n'; } while(0)

    /* /sys/class/block/ — list block devices */
    if (path[5] == 'c' && path[10] == 'b') {  /* /sys/class/block */
        if (blkdev_count) {
            int n = blkdev_count();
            for (int i = 0; i < n; i++) {
                const char *name = blkdev_name ? blkdev_name(i) : "?";
                SB_STR(name); SB_STR(" ");
                SB_DEC(blkdev_size_mb ? blkdev_size_mb(i) : 0);
                SB_STR("MB"); SB_NL();
            }
        } else {
            SB_STR("(no block device layer)\n");
        }
    }
    /* /sys/class/net/ — network interfaces */
    else if (path[5] == 'c' && path[10] == 'n') {
        SB_STR("eth0: ");
        if (net_get_ip_ptr) {
            uint8_t *ip = net_get_ip_ptr();
            SB_DEC(ip[0]); SB_STR("."); SB_DEC(ip[1]); SB_STR(".");
            SB_DEC(ip[2]); SB_STR("."); SB_DEC(ip[3]);
        } else {
            SB_STR("0.0.0.0");
        }
        SB_NL();
        if (net_get_mac) {
            uint8_t mac[6]; net_get_mac(mac);
            SB_STR("  HW: ");
            for (int i = 0; i < 6; i++) {
                if (i > 0) SB_STR(":");
                char h[3]; h[0] = "0123456789abcdef"[(mac[i]>>4)&0xF];
                h[1] = "0123456789abcdef"[mac[i]&0xF]; h[2] = 0;
                SB_STR(h);
            }
            SB_NL();
        }
    }
    /* /sys/kernel/hostname */
    else if (path[5] == 'k' && path[11] == 'h') {
        extern const char *ns_get_hostname(uint32_t) __attribute__((weak));
        const char *h = (ns_get_hostname) ? ns_get_hostname(0) : "osito-k";
        SB_STR(h); SB_NL();
    }
    /* /sys/kernel/version */
    else if (path[5] == 'k' && path[11] == 'v') {
        SB_STR("OsitoK 1.0 bare-metal x86_64"); SB_NL();
    }
    /* /sys/kernel/uptime */
    else if (path[5] == 'k' && path[11] == 'u') {
        SB_DEC(idt_get_ticks() / 100); SB_NL();
    }
    /* /sys/devices/system/cpu/online */
    else if (path[5] == 'd') {
        extern uint32_t smp_get_cpu_count(void) __attribute__((weak));
        uint32_t cpus = smp_get_cpu_count ? smp_get_cpu_count() : 1;
        SB_STR("0-"); SB_DEC(cpus - 1); SB_NL();
    }
    else {
        return -1;  /* Not found */
    }

    buf[p] = '\0';
    return p;

    #undef SB_STR
    #undef SB_DEC
    #undef SB_NL
}
