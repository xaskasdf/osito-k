/*
 * OsitoK — Win32 Compatibility Layer Initialization
 *
 * Sets up everything needed to run PE32 (i386) Windows executables:
 *   1. GDT 32-bit code/data segments for compatibility mode
 *   2. IDT vector 0x2E → int2e_stub (compat32 dispatch)
 *   3. All DLL shim initialization
 *   4. win32_exec() — load PE from OsitoFS and run it
 *
 * Integration:
 *   Call win32_init() from main.c after idt_init() and osfs2_mount().
 *   Add "winexec" command in shell.c → win32_exec(filename).
 */

#include "nttypes.h"
#include "pe.h"
#include "compat32.h"
#include "wdbg.h"

/* ── Kernel interfaces (weak-safe externs) ────────────────────── */

extern void  serial_puts(const char *s);
extern void  serial_puthex(uint64_t val, int digits);
extern void  serial_putdec(uint64_t val);
extern void  fb_puts(const char *s);
extern void  fb_puts_color(const char *s, uint32_t color);

/* OsitoFS */
extern int   osfs2_is_mounted(void);
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* Memory */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   strncmp(const char *a, const char *b, uint64_t n);

/* winexec engine */
extern int winexec_run(const uint8_t *file_data, uint64_t file_size);

/* INT 0x2E assembly stub (int2e_stub.S) */
extern void int2e_stub(void);

/* ── GDT access (from idt.c) ─────────────────────────────────── */
/*
 * The kernel GDT is a static array in idt.c.
 * We access it via weak externs to add 32-bit compat mode entries.
 */

extern uint64_t kernel_gdt[] __attribute__((weak));

/* Kernel GDTR — we need to reload it after adding entries */
extern struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} kernel_gdtr __attribute__((weak));

/* ── IDT access (from idt.c) ─────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} win32_idt_entry_t;

/* The IDT array from idt.c — 256 entries */
extern win32_idt_entry_t idt[] __attribute__((weak));

/* ── GDT segment descriptors ─────────────────────────────────── */
/*
 * Current kernel GDT layout (set by gdt_init in idt.c):
 *   Index 5 (0x28): 64-bit code  — SYSCALL CS
 *   Index 6 (0x30): 64-bit data  — SYSCALL SS
 *   Index 7 (0x38): 64-bit code  — IDT gate CS
 *
 * We add:
 *   Index 8 (0x40): 32-bit code  — L=0, D=1 (compat mode)
 *   Index 9 (0x48): 32-bit data  — L=0, D=1
 *
 * Descriptor encoding (low→high):
 *   [15:0]  Limit[15:0]
 *   [31:16] Base[15:0]
 *   [39:32] Base[23:16]
 *   [47:40] Access (P=1, DPL=00, S=1, Type)
 *   [51:48] Limit[19:16]
 *   [55:52] Flags (G=1, D/B, L, AVL)
 *   [63:56] Base[31:24]
 *
 * 32-bit code (L=0, D=1, G=1): flags=0xC → 0x00CF9A000000FFFF
 * 32-bit data (L=0, D=1, G=1): flags=0xC → 0x00CF92000000FFFF
 */

#define GDT_ENTRY_CODE32  0x00CF9A000000FFFFULL  /* execute/read, ring 0 */
#define GDT_ENTRY_DATA32  0x00CF92000000FFFFULL  /* read/write,   ring 0 */

#define GDT_INDEX_CODE32  8   /* selector 0x40 */
#define GDT_INDEX_DATA32  9   /* selector 0x48 */

static void gdt_add_compat_segments(void)
{
    if (!kernel_gdt) {
        serial_puts("[WIN32] WARNING: kernel_gdt not found, cannot add compat segments\n");
        return;
    }

    /* Write 32-bit code and data descriptors */
    kernel_gdt[GDT_INDEX_CODE32] = GDT_ENTRY_CODE32;
    kernel_gdt[GDT_INDEX_DATA32] = GDT_ENTRY_DATA32;

    /* Extend the GDT limit if needed (must cover index 9 = 10 entries) */
    uint16_t needed_limit = (uint16_t)(10 * 8 - 1);  /* 79 bytes */
    if (kernel_gdtr.limit < needed_limit) {
        kernel_gdtr.limit = needed_limit;
        __asm__ volatile ("lgdt %0" : : "m"(kernel_gdtr));
    }

    serial_puts("[WIN32] GDT compat segments: CODE32=0x40, DATA32=0x48\n");
}

/* ── IDT: install INT 0x2E handler ───────────────────────────── */

static void idt_install_int2e(void)
{
    if (!idt) {
        serial_puts("[WIN32] WARNING: IDT not found, cannot install INT 0x2E\n");
        return;
    }

    /* Read current CS for the IDT gate selector */
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    uint64_t addr = (uint64_t)int2e_stub;

    int vec = 0x2E;
    idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].selector    = cs;
    idt[vec].ist         = 1;     /* IST1: dedicated stack (avoids user stack pollution) */
    idt[vec].type_attr   = 0x8F;  /* present, DPL=0, 64-bit trap gate (preserves IF) */
    idt[vec].reserved    = 0;

    serial_puts("[WIN32] IDT vector 0x2E → int2e_stub at 0x");
    serial_puthex(addr, 16);
    serial_puts("\n");
}

/* ── Win32 subsystem initialization ──────────────────────────── */

static int win32_initialized = 0;

void win32_init(void)
{
    if (win32_initialized) return;

    serial_puts("\n[WIN32] Initializing Windows compatibility layer...\n");

    /* Step 1: Add 32-bit GDT segments for compat mode */
    gdt_add_compat_segments();

    /* Step 2: Install INT 0x2E handler for compat32 dispatch */
    idt_install_int2e();

    /* Step 3: Initialize Win32 debug toolkit (modules, default hooks) */
    wdbg_init();

    win32_initialized = 1;

    serial_puts("[WIN32] Ready. Use 'winexec <file.exe>' to run PE executables.\n");
    fb_puts(" Win32 compat layer ready\n");
}

/* ── Current PE name (for GetModuleFileName) ─────────────────── */

char win32_exe_name[64] = "program.exe";
char win32_image_path[260] = "program.exe";
#define WIN32_COMMAND_LINE_CAP 4096
char win32_command_line[WIN32_COMMAND_LINE_CAP] = "program.exe";
static char win32_relaunch_command_line[WIN32_COMMAND_LINE_CAP];

/* Set by ShellExecuteA/CreateProcessA when the guest launches an .exe (UT99
 * re-launches itself to apply a video-mode/color-depth change). win32_exec
 * loops: when the current PE exits with this set, it reloads + re-runs the
 * same EXE — a minimal "process re-exec" so the relaunch isn't a dead exit. */
int  g_win32_relaunch = 0;

BOOL win32_request_relaunch(const char *application,
                            const char *command_line)
{
    const char *source = command_line && command_line[0]
                       ? command_line : application;
    if (!source || !source[0]) return FALSE;

    int out = 0;
    BOOL quote_application = (!command_line || !command_line[0]);
    if (quote_application) {
        for (int i = 0; source[i]; i++) {
            if (source[i] == ' ' || source[i] == '\t') {
                if (out >= WIN32_COMMAND_LINE_CAP - 1) return FALSE;
                win32_relaunch_command_line[out++] = '"';
                break;
            }
        }
    }

    for (int i = 0; source[i]; i++) {
        if (out >= WIN32_COMMAND_LINE_CAP - 1) return FALSE;
        win32_relaunch_command_line[out++] = source[i];
    }
    if (quote_application && out && win32_relaunch_command_line[0] == '"') {
        if (out >= WIN32_COMMAND_LINE_CAP - 1) return FALSE;
        win32_relaunch_command_line[out++] = '"';
    }
    win32_relaunch_command_line[out] = 0;
    __atomic_store_n(&g_win32_relaunch, 1, __ATOMIC_RELEASE);
    return TRUE;
}

/* ── Load and execute a PE from OsitoFS ──────────────────────── */

int win32_exec_args(const char *filename, int argc, const char **argv)
{
    if (!win32_initialized) {
        serial_puts("[WIN32] Not initialized — call win32_init() first\n");
        return -1;
    }

    if (!osfs2_is_mounted()) {
        serial_puts("[WIN32] No filesystem mounted\n");
        return -1;
    }

    int cmd_pos = 0;
    for (int arg = 0; arg < argc &&
                            cmd_pos < WIN32_COMMAND_LINE_CAP - 1; arg++) {
        int quoted = argv[arg][0] == 0;
        for (int i = 0; argv[arg][i]; i++) {
            if (argv[arg][i] == ' ' || argv[arg][i] == '\t') {
                quoted = 1;
                break;
            }
        }
        if (arg > 0) win32_command_line[cmd_pos++] = ' ';
        if (quoted && cmd_pos < WIN32_COMMAND_LINE_CAP - 1)
            win32_command_line[cmd_pos++] = '"';
        for (int i = 0; argv[arg][i] &&
                            cmd_pos < WIN32_COMMAND_LINE_CAP - 1; i++)
            win32_command_line[cmd_pos++] = argv[arg][i];
        if (quoted && cmd_pos < WIN32_COMMAND_LINE_CAP - 1)
            win32_command_line[cmd_pos++] = '"';
    }
    win32_command_line[cmd_pos] = 0;
    serial_puts("[WIN32] Command line: ");
    serial_puts(win32_command_line);
    serial_puts("\n");

    int result = -1;
  relaunch:
    __atomic_store_n(&g_win32_relaunch, 0, __ATOMIC_RELEASE);
    win32_relaunch_command_line[0] = 0;

    /* Find file on OsitoFS */
    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[WIN32] File not found: ");
        serial_puts(filename);
        serial_puts("\n");
        return -1;
    }

    uint64_t size = osfs2_file_size(file);
    if (size < 64) {
        serial_puts("[WIN32] File too small to be a PE\n");
        return -1;
    }

    serial_puts("[WIN32] Loading ");
    serial_puts(filename);
    serial_puts(" (");
    serial_putdec(size);
    serial_puts(" bytes)\n");

    /* Allocate buffer and read entire PE file */
    uint64_t pages = (size + 0xFFF) / 4096;
    uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
    if (!buf) {
        serial_puts("[WIN32] Failed to allocate read buffer\n");
        return -1;
    }

    int rd = osfs2_read(file, 0, buf, size);
    if (rd < 0) {
        serial_puts("[WIN32] Failed to read file\n");
        mem_free_pages(buf, pages);
        return -1;
    }

    /* Set the PE name for GetModuleFileName */
    {
        int path_len = 0;
        while (filename[path_len] && path_len < 259) {
            win32_image_path[path_len] = filename[path_len];
            path_len++;
        }
        win32_image_path[path_len] = 0;

        const char *exe_name = filename;
        if (strncmp(exe_name, "System\\", 7) == 0) exe_name += 7;
        int i;
        for (i = 0; exe_name[i] && i < 63; i++)
            win32_exe_name[i] = exe_name[i];
        win32_exe_name[i] = 0;
    }

    /* Hand off to the PE execution engine */
    result = winexec_run(buf, size);

    /* Free the file buffer (PE image was copied by pe_load) */
    mem_free_pages(buf, pages);

    serial_puts("[WIN32] Execution finished, exit code = ");
    serial_putdec((uint64_t)(uint32_t)result);
    serial_puts("\n");

    /* Minimal process re-exec: UT99 relaunches itself (ShellExecute/CreateProcess
     * of its own .exe) to apply a video-mode/color-depth change, then ExitProcess.
     * Without this the relaunch is a dead exit to the shell. Reload + re-run the
     * same EXE. NOTE: win32 global state (PE/DLL VA mappings, FName, GMalloc,
     * surfaces) is only partially reset by winexec_run's *_shim_init — this is a
     * debug attempt to see how far a naive re-exec gets. */
    if (__atomic_load_n(&g_win32_relaunch, __ATOMIC_ACQUIRE)) {
        if (win32_relaunch_command_line[0]) {
            int i = 0;
            while (win32_relaunch_command_line[i] &&
                   i < WIN32_COMMAND_LINE_CAP - 1) {
                win32_command_line[i] = win32_relaunch_command_line[i];
                i++;
            }
            win32_command_line[i] = 0;
        }
        serial_puts("[WIN32] === RE-EXEC requested — relaunching ");
        serial_puts(filename);
        serial_puts(" ===\n");
        goto relaunch;
    }

    return result;
}

int win32_exec(const char *filename)
{
    const char *argv[] = { filename };
    return win32_exec_args(filename, 1, argv);
}

/* ── Install an MSI/MSIX package from OsitoFS ─────────────────── */

extern int installer_run_buffer(const uint8_t *data, uint32_t len,
                                const char *pkg_name);

int win32_install(const char *filename)
{
    if (!win32_initialized) win32_init();

    if (!osfs2_is_mounted()) {
        serial_puts("[WIN32] No filesystem mounted\n");
        return -1;
    }

    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[WIN32] File not found: ");
        serial_puts(filename);
        serial_puts("\n");
        return -1;
    }

    uint64_t size = osfs2_file_size(file);
    if (size < 8) {
        serial_puts("[WIN32] File too small to be a package\n");
        return -1;
    }

    serial_puts("[WIN32] Installing ");
    serial_puts(filename);
    serial_puts(" (");
    serial_putdec(size);
    serial_puts(" bytes)\n");

    uint64_t pages = (size + 0xFFF) / 4096;
    uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
    if (!buf) {
        serial_puts("[WIN32] Failed to allocate read buffer\n");
        return -1;
    }

    int rd = osfs2_read(file, 0, buf, size);
    if (rd < 0) {
        serial_puts("[WIN32] Failed to read file\n");
        mem_free_pages(buf, pages);
        return -1;
    }

    /* package name = filename basename without extension */
    char pkg[64];
    {
        const char *base = filename;
        for (const char *p = filename; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        int i = 0;
        for (; base[i] && base[i] != '.' && i < 63; i++) pkg[i] = base[i];
        pkg[i] = 0;
    }

    int result = installer_run_buffer(buf, (uint32_t)size, pkg);

    mem_free_pages(buf, pages);

    serial_puts("[WIN32] Install finished, status = ");
    serial_putdec((uint64_t)(uint32_t)result);
    serial_puts("\n");
    return result;
}
