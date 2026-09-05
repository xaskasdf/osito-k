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

/* INT 21h/AH=58h memory allocation strategies. The upper-memory bits are
 * retained even when no UMB chain is installed so callers can round-trip the
 * DOS contract without changing the conventional-memory fit policy. */
#define DOS_ALLOC_FIRST_FIT       0x00
#define DOS_ALLOC_BEST_FIT        0x01
#define DOS_ALLOC_LAST_FIT        0x02
#define DOS_ALLOC_UMB_ONLY        0x40
#define DOS_ALLOC_UMB_FIRST       0x80
#define DOS_ALLOC_FIT_MASK        0x3F

/* DOS error values shared by the loader and INT 21h services. */
enum {
    DOS_ERR_INVALID_FUNCTION = 1,
    DOS_ERR_FILE_NOT_FOUND = 2,
    DOS_ERR_PATH_NOT_FOUND = 3,
    DOS_ERR_TOO_MANY_OPEN_FILES = 4,
    DOS_ERR_ACCESS_DENIED = 5,
    DOS_ERR_INVALID_HANDLE = 6,
    DOS_ERR_NOT_ENOUGH_MEMORY = 8,
    DOS_ERR_INVALID_BLOCK = 9,
    DOS_ERR_BAD_ENVIRONMENT = 10,
    DOS_ERR_BAD_FORMAT = 11,
    DOS_ERR_INVALID_ACCESS = 12,
    DOS_ERR_INVALID_DATA = 13,
    DOS_ERR_INVALID_DRIVE = 15,
    DOS_ERR_NO_MORE_FILES = 18,
    DOS_ERR_NOT_READY = 21,
    DOS_ERR_SHARING_VIOLATION = 32,
    DOS_ERR_FILE_EXISTS = 80,
    DOS_ERR_CANNOT_MAKE = 82
};

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

#define DOS_PSP_JFT_ENTRIES  20u
#define DOS_MAX_JFT_ENTRIES  0xFFFEu
#define DOS_MAX_SFT_ENTRIES  255u
#define DOS_SFT_INVALID      0xFFu

enum {
    DOS_DEVICE_NONE = 0,
    DOS_DEVICE_CON,
    DOS_DEVICE_AUX,
    DOS_DEVICE_PRN,
    DOS_DEVICE_NUL
};

typedef struct {
    bool     used;
    uint16_t ref_count;
    void    *osfs_file;     /* OsitoFS file handle */
    uint32_t position;      /* current seek position */
    uint32_t file_size;     /* cached file size */
    uint16_t open_mode;     /* DOS access/share/inheritance/open flags */
    uint16_t io_flags;      /* shared SFT IOCTL flags, not driver attributes */
    uint16_t owner_psp;     /* PSP that created this SFT entry */
    bool     is_device;     /* true for DOS character devices */
    uint8_t  device_kind;
} dos_sft_entry_t;

typedef struct {
    uint8_t sft_index;      /* process JFT entry, 0xFF when closed */
} dos_handle_t;

/* ── Forward declarations ───────────────────────────────────────── */

struct cpu8086_state;
struct dos_exec_context;
struct dos_vcpi_state;

#define DOS_EMS_PAGE_FRAME_BASE 0x000E0000u
#define DOS_EMS_PAGE_SIZE       0x00004000u
#define DOS_EMS_FRAME_PAGES     4u

#define DOS_MAX_SEARCHES        8u
#define DOS_SEARCH_PATH_MAX     128u

typedef struct {
    bool             used;
    bool             use_osfs3;
    uint16_t         token;
    uint16_t         attributes;
    uint32_t         serial;
    char             directory[DOS_SEARCH_PATH_MAX];
    char             pattern[DOS_SEARCH_PATH_MAX];
} dos_search_t;

/* DPMI state (full definition in dos_dpmi.h, included after dos_vm_t) */
#include "dos_dpmi.h"

/* VBE video memory occupies a guest-physical aperture after system RAM.  It
 * is mapped into native DPMI address spaces but excluded from every DOS/XMS
 * memory-size report and from the DPMI allocation pool. */
#define DOS_VBE_WINDOW_BASE       0x000A0000u
#define DOS_VBE_WINDOW_SIZE       0x00010000u
#define DOS_VBE_FB_BASE           DOS_TOTAL_MEM
#define DOS_VBE_FB_SIZE           (4u * 1024u * 1024u)
#define DOS_VM_ADDRESS_SPACE_SIZE (DOS_TOTAL_MEM + DOS_VBE_FB_SIZE)

/* ── DOS Virtual Machine ────────────────────────────────────────── */

typedef struct {
    uint64_t fs_base, gs_base;
    uint16_t fs, gs;
    bool saved;
} dos_host_tls_t;

typedef struct dos_vm {
    struct cpu8086_state *cpu;
    uint8_t         *mem;               /* kernel direct-map view of guest RAM */
    uint32_t         total_mem_size;    /* actual allocated size */
    uint32_t         system_mem_size;   /* RAM visible to DOS/DPMI clients */

    /* DOS state */
    uint16_t         current_psp;       /* segment of current PSP */
    uint8_t          current_drive;     /* 0=A:, 2=C: */
    char             current_dir[64];   /* current directory */
    uint16_t         dta_seg;           /* Disk Transfer Area segment */
    uint16_t         dta_off;           /* Disk Transfer Area offset */
    uint8_t          allocation_strategy; /* INT 21h/AH=58h policy */
    uint8_t          uppermem_link;     /* zero until a UMB arena exists */
    uint8_t          indos_count;       /* published through INT 21h/AH=34h */
    bool             ctrl_break_enabled;
    uint16_t         extended_error;    /* last INT 21h carry error */
    uint8_t          extended_error_action;
    uint8_t          extended_error_class;
    uint8_t          extended_error_locus;
    uint16_t         extended_error_segment;
    uint16_t         extended_error_offset;
    uint32_t         temp_file_serial;
    uint8_t          last_return_code;  /* INT 21h/AH=4Dh low byte */
    uint8_t          last_return_type;  /* 0 normal, 1 Ctrl+Break/fault */
    uint8_t          termination_type;
    bool             process_terminated;
    uint16_t         exec_depth;        /* nested INT 21h/AH=4Bh calls */
    struct dos_exec_context *exec_context; /* pending AH=4B01 process chain */
    uint32_t         software_int_return_flags;
    uint8_t          software_int_frame_bytes;
    dos_search_t     searches[DOS_MAX_SEARCHES];
    uint16_t         next_search_token;
    uint32_t         next_search_serial;

    /* File handles. Before a PSP exists, the bootstrap JFT supplies the five
     * standard handles. Once loaded, the PSP's JFT is authoritative. */
    dos_handle_t     bootstrap_jft[DOS_PSP_JFT_ENTRIES];
    dos_sft_entry_t  sft[DOS_MAX_SFT_ENTRIES];
    uint16_t         jft_external_segment;
    uint16_t         jft_external_psp;
    bool             jft_active;

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
    uint8_t          vga_render_page;
    uint8_t          vga_render_cursor_row;
    uint8_t          vga_render_cursor_col;
    uint16_t         vga_render_mouse_x;
    uint16_t         vga_render_mouse_y;
    bool             vga_render_valid;
    bool             vga_render_mouse_visible;
    uint8_t          video_diag_count;

    /* VBE 2.0 display state. Framebuffer bytes live at DOS_VBE_FB_BASE;
     * windowed clients reach the selected 64 KB bank through A000:0000. */
    uint16_t         vbe_mode;
    uint16_t         vbe_width;
    uint16_t         vbe_height;
    uint16_t         vbe_pitch;
    uint16_t         vbe_bank;
    uint16_t         vbe_display_x;
    uint16_t         vbe_display_y;
    uint8_t          vbe_bpp;
    uint8_t          vbe_bytes_per_pixel;
    uint8_t          vbe_dac_width;
    bool             vbe_active;
    bool             vbe_linear;
    bool             vbe_no_clear;

    /* Keyboard buffer */
    uint16_t         kb_buffer[16];     /* circular buffer (scancode<<8 | ascii) */
    uint8_t          kb_head;
    uint8_t          kb_tail;
    uint8_t          console_scan_pending;
    uint8_t          console_column;   /* DOS cooked-output column, modulo 256 */
    uint8_t          console_line[129]; /* 127 edited bytes, CR, LF */
    uint16_t         console_line_count;
    uint16_t         console_line_position;

    /* Timer */
    uint32_t         bios_ticks;        /* INT 1Ah tick count */
    uint64_t         start_ticks;       /* kernel tick at VM start */
    uint64_t         last_timer_tick;   /* kernel tick of last INT 8 delivery */
    bool             timer_irq_pending; /* latched IRQ0 while IF is clear */

    /* DOS/BIOS civil clock. A guest-set clock advances from a monotonic
     * snapshot without changing the kernel's global wall clock. */
    uint64_t         clock_base_centiseconds;
    uint64_t         clock_base_ticks;
    bool             clock_override;
    uint8_t          clock_daylight;

    /* Mouse state (INT 33h) */
    uint16_t         mouse_x, mouse_y;
    uint16_t         mouse_buttons;
    uint16_t         mouse_min_x, mouse_max_x;
    uint16_t         mouse_min_y, mouse_max_y;
    int16_t          mouse_cursor_flag;
    bool             mouse_visible;
    bool             mouse_initialized;
    int32_t          mouse_host_offset_x;
    int32_t          mouse_host_offset_y;
    int64_t          mouse_motion_x;
    int64_t          mouse_motion_y;
    uint64_t         mouse_press_count[3];
    uint64_t         mouse_release_count[3];
    uint64_t         mouse_press_observed[3];
    uint64_t         mouse_release_observed[3];
    uint16_t         mouse_press_x[3];
    uint16_t         mouse_press_y[3];
    uint16_t         mouse_release_x[3];
    uint16_t         mouse_release_y[3];
    uint32_t         mouse_diag_mask;

    /* DPMI host state */
    dpmi_state_t     dpmi;

    /* EMS/VCPI owns extended pages through the shared DPMI page allocator.
     * A zero frame base means that the corresponding 16 KB window is
     * unmapped. */
    struct dos_vcpi_state *vcpi;
    uint32_t         ems_frame_bases[DOS_EMS_FRAME_PAGES];

    /* JIT/DBT state (NULL if not initialized) */
    void            *jit;           /* jit_state_t* — forward ref avoids circular include */

    /* Per-VM legacy audio frontend (Sound Blaster DSP + ISA DMA). */
    void            *audio;

    /* Per-VM ISA chipset state (PIT, PIC, keyboard controller, and VGA I/O). */
    void            *io;

    /* Native protected-mode session ownership. Every run gets its own CR3
     * and restores the host descriptor tables before releasing guest RAM. */
    uint64_t         mem_pages;
    uint64_t         native_cr3;
    void            *native_gdt;
    void            *native_ldt;
    dos_host_tls_t   native_host_tls;
    uint8_t          native_saved_idt[12][16];
    uint64_t         native_resume_jmpbuf[9];
    bool             native_idt_saved;
    bool             native_ready;
    bool             native_active;
    bool             native_resume_armed;
    uint8_t          native_dispatch_depth;

    /* A DPMI real-mode service can temporarily re-enter the interpreter.
     * The nested run stops before fetching the host-owned return address. */
    uint16_t         interpreter_stop_cs;
    uint32_t         interpreter_stop_ip;
    uint16_t         interpreter_stop_psp;
    bool             interpreter_stop_protected;
    bool             interpreter_stop_active;
    bool             interpreter_stop_reached;
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
