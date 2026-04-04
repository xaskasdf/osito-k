/*
 * OsitoK — DOS 16-bit Compatibility Layer Types
 *
 * Shared data structures for the DOS emulation subsystem:
 *   - MZ executable header
 *   - Program Segment Prefix (PSP)
 *   - Memory Control Block (MCB)
 *   - DOS Virtual Machine state (dos_vm_t)
 */

#ifndef DOS_TYPES_H
#define DOS_TYPES_H

#include "../include/types.h"

/* ── Emulated memory layout ─────────────────────────────────────── */

#define DOS_MEM_SIZE      (1024 * 1024 + 65536)   /* 1MB + 64KB HMA */

#define DOS_IVT_BASE      0x00000   /* Interrupt Vector Table */
#define DOS_IVT_SIZE      0x00400   /* 256 entries * 4 bytes */
#define DOS_BDA_BASE      0x00400   /* BIOS Data Area */
#define DOS_BDA_SIZE      0x00100
#define DOS_DATA_BASE     0x00500   /* DOS internal data */
#define DOS_CONV_BASE     0x00600   /* Start of conventional memory */
#define DOS_CONV_TOP      0xA0000   /* 640KB boundary */
#define DOS_VRAM_BASE     0xB8000   /* VGA text buffer */
#define DOS_VRAM_SIZE     0x08000   /* 32KB video RAM */
#define DOS_ROM_BASE      0xF0000   /* ROM area for IRET stubs */

/* ── MZ (DOS EXE) header ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t e_magic;       /* 'MZ' (0x5A4D) */
    uint16_t e_cblp;        /* bytes on last 512-byte page */
    uint16_t e_cp;          /* pages in file (512 bytes each) */
    uint16_t e_crlc;        /* relocation entry count */
    uint16_t e_cparhdr;     /* header size in paragraphs (16 bytes) */
    uint16_t e_minalloc;    /* minimum extra paragraphs needed */
    uint16_t e_maxalloc;    /* maximum extra paragraphs requested */
    uint16_t e_ss;          /* initial SS (relative to load segment) */
    uint16_t e_sp;          /* initial SP */
    uint16_t e_csum;        /* checksum (usually ignored) */
    uint16_t e_ip;          /* initial IP */
    uint16_t e_cs;          /* initial CS (relative to load segment) */
    uint16_t e_lfarlc;      /* offset of relocation table */
    uint16_t e_ovno;        /* overlay number */
} mz_header_t;

#define MZ_MAGIC  0x5A4D   /* 'MZ' */
#define ZM_MAGIC  0x4D5A   /* 'ZM' (alternate) */

/* MZ relocation entry */
typedef struct __attribute__((packed)) {
    uint16_t offset;
    uint16_t segment;
} mz_reloc_t;

/* ── Program Segment Prefix (256 bytes) ─────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t int20;         /* INT 20h instruction (0x20CD) */
    uint16_t mem_top;       /* segment past end of allocated memory */
    uint8_t  reserved1;
    uint8_t  dos_call[5];   /* FAR CALL to DOS entry point */
    uint32_t old_int22;     /* saved terminate address (INT 22h) */
    uint32_t old_int23;     /* saved Ctrl+Break address (INT 23h) */
    uint32_t old_int24;     /* saved critical error address (INT 24h) */
    uint16_t parent_psp;    /* parent PSP segment */
    uint8_t  jft[20];       /* Job File Table */
    uint16_t env_seg;       /* environment segment */
    uint32_t last_ss_sp;    /* saved SS:SP */
    uint16_t jft_size;      /* JFT entry count */
    uint32_t jft_ptr;       /* FAR pointer to JFT */
    uint8_t  reserved2[24];
    uint8_t  dispatch[3];   /* INT 21h / RETF */
    uint8_t  reserved3[9];
    uint8_t  fcb1[16];      /* default FCB 1 */
    uint8_t  fcb2[20];      /* default FCB 2 */
    uint8_t  cmd_len;       /* command tail length */
    uint8_t  cmd_tail[127]; /* command tail (space + args + 0x0D) */
} dos_psp_t;

/* ── Memory Control Block (16 bytes) ────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  type;          /* 'M' = more blocks, 'Z' = last block */
    uint16_t owner;         /* PSP segment of owner (0 = free) */
    uint16_t size;          /* block size in paragraphs (16 bytes) */
    uint8_t  reserved[3];
    char     name[8];       /* program name (DOS 4.0+) */
} dos_mcb_t;

/* ── DOS file handle ────────────────────────────────────────────── */

#define DOS_MAX_HANDLES  20

typedef struct {
    bool     open;
    void    *osfs_file;     /* OsitoFS file handle */
    uint32_t position;      /* current seek position */
    uint32_t file_size;     /* cached file size */
    uint8_t  mode;          /* 0=read, 1=write, 2=rw */
    bool     is_device;     /* true for CON, AUX, PRN */
} dos_handle_t;

/* ── Forward declarations ───────────────────────────────────────── */

struct cpu8086_state;

/* DPMI state (full definition in dos_dpmi.h, included after dos_vm_t) */
#include "dos_dpmi.h"

/* ── DOS Virtual Machine ────────────────────────────────────────── */

typedef struct dos_vm {
    struct cpu8086_state *cpu;
    uint8_t         *mem;               /* emulated memory (up to 16MB) */
    uint32_t         total_mem_size;    /* actual allocated size */

    /* DOS state */
    uint16_t         current_psp;       /* segment of current PSP */
    uint8_t          current_drive;     /* 0=A:, 2=C: */
    char             current_dir[64];   /* current directory */
    uint16_t         dta_seg;           /* Disk Transfer Area segment */
    uint16_t         dta_off;           /* Disk Transfer Area offset */

    /* File handles */
    dos_handle_t     handles[DOS_MAX_HANDLES];

    /* Memory control blocks */
    uint16_t         first_mcb;         /* segment of first MCB */

    /* VGA text mode state */
    uint8_t          vga_mode;          /* current video mode (0x03) */
    uint8_t          vga_page;          /* active display page */
    uint8_t          cursor_row;
    uint8_t          cursor_col;
    uint8_t          cursor_start;      /* cursor shape start line */
    uint8_t          cursor_end;        /* cursor shape end line */
    uint8_t          text_attr;         /* default text attribute */
    uint8_t          vga_dirty[250];    /* dirty bits: 80*25/8 = 250 bytes */

    /* Keyboard buffer */
    uint16_t         kb_buffer[16];     /* circular buffer (scancode<<8 | ascii) */
    uint8_t          kb_head;
    uint8_t          kb_tail;

    /* Timer */
    uint32_t         bios_ticks;        /* INT 1Ah tick count */
    uint64_t         start_ticks;       /* kernel tick at VM start */
    uint64_t         last_timer_tick;   /* kernel tick of last INT 8 delivery */

    /* Mouse state (INT 33h) */
    uint16_t         mouse_x, mouse_y;
    uint16_t         mouse_buttons;
    bool             mouse_visible;

    /* DPMI host state */
    dpmi_state_t     dpmi;

    /* JIT/DBT state (NULL if not initialized) */
    void            *jit;           /* jit_state_t* — forward ref avoids circular include */
} dos_vm_t;

/* ── CGA color palette ──────────────────────────────────────────── */

static const uint32_t cga_colors[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,  /* black, blue, green, cyan */
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,  /* red, magenta, brown, light gray */
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,  /* dark gray, lt blue, lt green, lt cyan */
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF   /* lt red, lt magenta, yellow, white */
};

/* ── DOS binary format detection ────────────────────────────────── */

#define DOS_FMT_NONE  0
#define DOS_FMT_COM   1
#define DOS_FMT_MZ    2

#endif /* DOS_TYPES_H */
