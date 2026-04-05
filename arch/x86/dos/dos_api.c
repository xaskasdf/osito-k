/*
 * OsitoK — DOS INT 21h API Services
 *
 * Implements the core DOS API functions accessed via INT 21h.
 * AH register selects the function. Maps file operations to OsitoFS.
 *
 * Phase 1: console I/O (01h-0Ch), version (30h), exit (4Ch)
 * Phase 2: file I/O (3Ch-42h), memory (48h-4Ah)
 */

#include "cpu8086.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void serial_putchar(char c);

/* Console output — bridges to OsitoK's framebuffer */
extern void fb_putchar(char c);
extern int  kb_has_input(void);
extern char kb_getchar(void);

/* OsitoFS */
extern int      osfs2_is_mounted(void);
extern void    *osfs2_find(const char *name);
extern int      osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* DOS memory manager */
extern uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest);
extern int      dos_mem_free(dos_vm_t *vm, uint16_t segment);
extern int      dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size,
                               uint16_t *max_avail);

/* ── Helper: read ASCIIZ string from DOS memory ────────────────── */

static void dos_read_asciiz(dos_vm_t *vm, uint16_t seg, uint16_t off,
                            char *buf, int maxlen)
{
    for (int i = 0; i < maxlen - 1; i++) {
        uint8_t ch = dos_mem_read8(vm, dos_linear(seg, off + i));
        if (ch == 0) { buf[i] = 0; return; }
        buf[i] = ch;
    }
    buf[maxlen - 1] = 0;
}

/* ── Helper: convert DOS path to OsitoFS path ──────────────────── */

static const char *dos_path_to_osfs(const char *path, char *buf, int buflen)
{
    int j = 0;

    /* Skip drive letter if present (e.g., "C:\") */
    if (path[1] == ':') path += 2;
    /* Skip leading backslash */
    if (*path == '\\' || *path == '/') path++;

    for (int i = 0; path[i] && j < buflen - 1; i++) {
        buf[j++] = (path[i] == '\\') ? '/' : path[i];
    }
    buf[j] = 0;
    return buf;
}

/* ── Helper: write char to console (handles CR/LF) ─────────────── */

static void dos_putchar(dos_vm_t *vm, char ch)
{
    (void)vm;
    fb_putchar(ch);
    serial_putchar(ch);  /* echo to serial for debugging */
}

/* ── INT 21h function dispatch ──────────────────────────────────── */

void dos_int21_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t ah = cpu->ah;

    switch (ah) {

    /* ── AH=00h: Terminate ──────────────────────────────────────── */
    case 0x00:
        cpu->running = false;
        cpu->exit_code = 0;
        break;

    /* ── AH=01h: Read char with echo ────────────────────────────── */
    case 0x01: {
        char ch;
        while (!kb_has_input()) { /* spin */ }
        ch = kb_getchar();
        dos_putchar(vm, ch);
        cpu->al = (uint8_t)ch;
        break;
    }

    /* ── AH=02h: Write character ────────────────────────────────── */
    case 0x02:
        dos_putchar(vm, (char)cpu->dl);
        break;

    /* ── AH=06h: Direct console I/O ─────────────────────────────── */
    case 0x06:
        if (cpu->dl == 0xFF) {
            /* Input */
            if (kb_has_input()) {
                cpu->al = (uint8_t)kb_getchar();
                cpu->flags &= ~FLAG_ZF;
            } else {
                cpu->al = 0;
                cpu->flags |= FLAG_ZF;
            }
        } else {
            /* Output */
            dos_putchar(vm, (char)cpu->dl);
        }
        break;

    /* ── AH=07h/08h: Read char without echo ─────────────────────── */
    case 0x07:
    case 0x08: {
        while (!kb_has_input()) { /* spin */ }
        cpu->al = (uint8_t)kb_getchar();
        break;
    }

    /* ── AH=09h: Write string ($ terminated) ────────────────────── */
    case 0x09: {
        uint16_t off = cpu->dx;
        while (1) {
            uint8_t ch = dos_mem_read8(vm, dos_linear(cpu->ds, off));
            if (ch == '$') break;
            dos_putchar(vm, (char)ch);
            off++;
        }
        cpu->al = '$';
        break;
    }

    /* ── AH=0Ah: Buffered input ─────────────────────────────────── */
    case 0x0A: {
        uint32_t buf_addr = dos_linear(cpu->ds, cpu->dx);
        uint8_t max_len = dos_mem_read8(vm, buf_addr);
        uint8_t count = 0;
        for (;;) {
            while (!kb_has_input()) { /* spin */ }
            char ch = kb_getchar();
            if (ch == '\r' || ch == '\n') {
                dos_putchar(vm, '\r');
                dos_putchar(vm, '\n');
                break;
            }
            if (ch == 8 && count > 0) {  /* backspace */
                count--;
                dos_putchar(vm, '\b');
                dos_putchar(vm, ' ');
                dos_putchar(vm, '\b');
                continue;
            }
            if (count < max_len - 1) {
                dos_mem_write8(vm, buf_addr + 2 + count, (uint8_t)ch);
                dos_putchar(vm, ch);
                count++;
            }
        }
        dos_mem_write8(vm, buf_addr + 2 + count, 0x0D);
        dos_mem_write8(vm, buf_addr + 1, count);
        break;
    }

    /* ── AH=0Bh: Check stdin status ─────────────────────────────── */
    case 0x0B:
        cpu->al = kb_has_input() ? 0xFF : 0x00;
        break;

    /* ── AH=0Ch: Flush input + call function ────────────────────── */
    case 0x0C:
        /* Flush keyboard buffer */
        while (kb_has_input()) kb_getchar();
        /* Re-dispatch with AL as function */
        if (cpu->al == 0x01 || cpu->al == 0x06 || cpu->al == 0x07 ||
            cpu->al == 0x08 || cpu->al == 0x0A) {
            cpu->ah = cpu->al;
            dos_int21_dispatch(vm);
        }
        break;

    /* ── AH=19h: Get current drive ──────────────────────────────── */
    case 0x19:
        cpu->al = vm->current_drive;  /* 2 = C: */
        break;

    /* ── AH=1Ah: Set DTA ────────────────────────────────────────── */
    case 0x1A:
        vm->dta_seg = cpu->ds;
        vm->dta_off = cpu->dx;
        break;

    /* ── AH=25h: Set interrupt vector ───────────────────────────── */
    case 0x25: {
        uint32_t ivt_addr = (uint32_t)cpu->al * 4;
        dos_mem_write16(vm, ivt_addr, cpu->dx);
        dos_mem_write16(vm, ivt_addr + 2, cpu->ds);
        break;
    }

    /* ── AH=2Fh: Get DTA ────────────────────────────────────────── */
    case 0x2F:
        cpu->es = vm->dta_seg;
        cpu->bx = vm->dta_off;
        break;

    /* ── AH=30h: Get DOS version ────────────────────────────────── */
    case 0x30:
        cpu->al = 6;   /* major: DOS 6.22 */
        cpu->ah = 22;  /* minor */
        cpu->bx = 0;   /* OEM */
        cpu->cx = 0;
        break;

    /* ── AH=35h: Get interrupt vector ───────────────────────────── */
    case 0x35: {
        uint32_t ivt_addr = (uint32_t)cpu->al * 4;
        cpu->bx = dos_mem_read16(vm, ivt_addr);
        cpu->es = dos_mem_read16(vm, ivt_addr + 2);
        break;
    }

    /* ── AH=3Ch: Create file ────────────────────────────────────── */
    case 0x3C: {
        /* TODO Phase 2: file creation */
        cpu->flags |= FLAG_CF;
        cpu->ax = 5;  /* Access denied */
        break;
    }

    /* ── AH=3Dh: Open file ──────────────────────────────────────── */
    case 0x3D: {
        char path[128], ospath[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        dos_path_to_osfs(path, ospath, sizeof(ospath));

        serial_puts("[DOS] Open DS=");
        serial_puthex(cpu->ds, 4);
        serial_puts(" DX=");
        serial_puthex(cpu->dx, 4);
        serial_puts(" -> '");
        serial_puts(path);
        serial_puts("' osfs='");
        serial_puts(ospath);
        serial_puts("' #");
        serial_putdec(cpu->insn_count);

        /* Hex dump at DS:DX-4 to DS:DX+20 */
        {
            uint32_t a = dos_linear(cpu->ds, cpu->dx);
            serial_puts(" @");
            serial_puthex(a, 8);
            serial_puts(" [-4..+20]:");
            for (int i = -4; i < 20; i++) {
                serial_puts(" ");
                serial_puthex(dos_mem_read8(vm, a + i), 2);
            }
        }

        void *file = osfs2_find(ospath);
        if (!file) {
            serial_puts(" NOT FOUND\n");
            cpu->flags |= FLAG_CF;
            cpu->ax = 2;  /* File not found */
            break;
        }
        serial_puts(" OK\n");

        /* Find free handle */
        int h = -1;
        for (int i = 5; i < DOS_MAX_HANDLES; i++) {
            if (!vm->handles[i].open) { h = i; break; }
        }
        if (h < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = 4;  /* Too many open files */
            break;
        }

        vm->handles[h].open = true;
        vm->handles[h].osfs_file = file;
        vm->handles[h].position = 0;
        vm->handles[h].file_size = (uint32_t)osfs2_file_size(file);
        vm->handles[h].mode = cpu->al & 0x03;
        vm->handles[h].is_device = false;

        cpu->flags &= ~FLAG_CF;
        cpu->ax = h;
        break;
    }

    /* ── AH=3Eh: Close file ─────────────────────────────────────── */
    case 0x3E: {
        uint16_t h = cpu->bx;
        if (h >= DOS_MAX_HANDLES || !vm->handles[h].open) {
            cpu->flags |= FLAG_CF;
            cpu->ax = 6;  /* Invalid handle */
            break;
        }
        vm->handles[h].open = false;
        vm->handles[h].osfs_file = 0;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=3Fh: Read file ──────────────────────────────────────── */
    case 0x3F: {
        uint16_t h = cpu->bx;
        uint16_t count = cpu->cx;

        /* Handle stdin */
        if (h == 0) {
            uint32_t buf = dos_linear(cpu->ds, cpu->dx);
            uint16_t i = 0;
            while (i < count) {
                while (!kb_has_input()) { /* spin */ }
                char ch = kb_getchar();
                dos_mem_write8(vm, buf + i, (uint8_t)ch);
                i++;
                if (ch == '\r' || ch == '\n') break;
            }
            cpu->ax = i;
            cpu->flags &= ~FLAG_CF;
            break;
        }

        if (h >= DOS_MAX_HANDLES || !vm->handles[h].open) {
            cpu->flags |= FLAG_CF;
            cpu->ax = 6;
            break;
        }

        dos_handle_t *fh = &vm->handles[h];
        uint32_t to_read = count;
        if (fh->position + to_read > fh->file_size)
            to_read = fh->file_size - fh->position;

        /* Read into a temp buffer, then copy to DOS memory */
        uint32_t buf = dos_linear(cpu->ds, cpu->dx);
        uint8_t tmp[512];
        uint32_t total = 0;
        while (total < to_read) {
            uint32_t chunk = to_read - total;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            int rd = osfs2_read(fh->osfs_file, fh->position, tmp, chunk);
            if (rd <= 0) break;
            for (int i = 0; i < rd; i++)
                dos_mem_write8(vm, buf + total + i, tmp[i]);
            total += rd;
            fh->position += rd;
        }

        cpu->ax = total;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=40h: Write file ─────────────────────────────────────── */
    case 0x40: {
        uint16_t h = cpu->bx;
        uint16_t count = cpu->cx;

        /* Handle stdout/stderr */
        if (h == 1 || h == 2) {
            for (uint16_t i = 0; i < count; i++) {
                uint8_t ch = dos_mem_read8(vm, dos_linear(cpu->ds, cpu->dx + i));
                dos_putchar(vm, (char)ch);
            }
            cpu->ax = count;
            cpu->flags &= ~FLAG_CF;
            break;
        }

        /* TODO Phase 2: write to file */
        cpu->flags |= FLAG_CF;
        cpu->ax = 5;
        break;
    }

    /* ── AH=42h: Seek (lseek) ──────────────────────────────────── */
    case 0x42: {
        uint16_t h = cpu->bx;
        if (h >= DOS_MAX_HANDLES || !vm->handles[h].open) {
            cpu->flags |= FLAG_CF;
            cpu->ax = 6;
            break;
        }

        dos_handle_t *fh = &vm->handles[h];
        int32_t offset = (int32_t)((uint32_t)cpu->cx << 16 | cpu->dx);

        switch (cpu->al) {
        case 0: fh->position = offset; break;                    /* SEEK_SET */
        case 1: fh->position = (int32_t)fh->position + offset; break; /* SEEK_CUR */
        case 2: fh->position = (int32_t)fh->file_size + offset; break; /* SEEK_END */
        }

        cpu->dx = (uint16_t)(fh->position >> 16);
        cpu->ax = (uint16_t)(fh->position & 0xFFFF);
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=44h: IOCTL ──────────────────────────────────────────── */
    case 0x44: {
        uint16_t h = cpu->bx;
        if (cpu->al == 0x00) {
            /* Get device info */
            if (h <= 2) {
                cpu->dx = 0x80D3;  /* character device, stdin/stdout */
            } else {
                cpu->dx = 0x0000;  /* disk file */
            }
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->flags |= FLAG_CF;
            cpu->ax = 1;  /* invalid function */
        }
        break;
    }

    /* ── AH=36h: Get disk free space ─────────────────────────────── */
    case 0x36:
        cpu->ax = 64;     /* sectors per cluster */
        cpu->bx = 1024;   /* available clusters */
        cpu->cx = 512;    /* bytes per sector */
        cpu->dx = 2048;   /* total clusters */
        break;

    /* ── AH=38h: Get country info ──────────────────────────────── */
    case 0x38:
        cpu->flags &= ~FLAG_CF;
        cpu->bx = 1;  /* country code: USA */
        break;

    /* ── AH=47h: Get current directory ──────────────────────────── */
    case 0x47: {
        uint32_t buf = dos_linear(cpu->ds, cpu->si);
        dos_mem_write8(vm, buf, 0);  /* root directory */
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=48h: Allocate memory ────────────────────────────────── */
    case 0x48: {
        uint16_t largest = 0;
        uint16_t seg = dos_mem_alloc(vm, cpu->bx, &largest);
        if (seg) {
            cpu->ax = seg;
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->bx = largest;
            cpu->ax = 8;  /* insufficient memory */
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    /* ── AH=49h: Free memory ────────────────────────────────────── */
    case 0x49:
        if (dos_mem_free(vm, cpu->es) == 0) {
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->ax = 9;  /* invalid memory block */
            cpu->flags |= FLAG_CF;
        }
        break;

    /* ── AH=4Ah: Resize memory ──────────────────────────────────── */
    case 0x4A: {
        uint16_t max_avail = 0;
        if (dos_mem_resize(vm, cpu->es, cpu->bx, &max_avail) == 0) {
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->bx = max_avail;
            cpu->ax = 8;
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    /* ── AH=4Ch: Exit with return code ──────────────────────────── */
    case 0x4C:
        serial_puts("[DOS] Exit(");
        serial_puthex(cpu->al, 2);
        serial_puts(") at #");
        serial_putdec(cpu->insn_count);
        serial_puts(" CS:EIP=");
        serial_puthex(cpu->cs, 4);
        serial_puts(":");
        serial_puthex(cpu->eip, 8);
        serial_puts(cpu->protected_mode ? " [PM]\n" : " [RM]\n");
        cpu->running = false;
        cpu->exit_code = cpu->al;
        break;

    /* ── AH=62h: Get PSP ────────────────────────────────────────── */
    case 0x62:
        cpu->bx = vm->current_psp;
        break;

    /* ── Default: unhandled ─────────────────────────────────────── */
    default:
        serial_puts("[DOS] Unhandled INT 21h AH=");
        serial_puthex(ah, 2);
        serial_puts("\n");
        cpu->flags |= FLAG_CF;
        cpu->ax = 1;  /* invalid function */
        break;
    }
}
