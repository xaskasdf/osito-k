/*
 * OsitoK — DOS Binary Loader
 *
 * Loads COM and MZ (EXE) DOS binaries into emulated 1MB memory.
 * COM: raw code loaded at PSP:0100h, all segments = PSP.
 * MZ:  relocatable EXE with header, segments, and relocation table.
 */

#include "cpu8086.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Forward declarations */
uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest);
int      dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size, uint16_t *max_avail);

/* ── Detect binary format ───────────────────────────────────────── */

int dos_detect_format(const uint8_t *data, uint64_t size)
{
    if (size < 2) return DOS_FMT_NONE;
    if ((data[0] == 'M' && data[1] == 'Z') ||
        (data[0] == 'Z' && data[1] == 'M'))
        return DOS_FMT_MZ;
    return DOS_FMT_NONE;  /* Caller checks .COM extension */
}

/* ── Build PSP at given segment ─────────────────────────────────── */

static void build_psp(dos_vm_t *vm, uint16_t psp_seg, uint16_t mem_top_seg,
                       const char *cmdline)
{
    uint32_t psp_addr = (uint32_t)psp_seg << 4;
    dos_psp_t *psp = (dos_psp_t *)(vm->mem + psp_addr);

    /* Zero the entire PSP */
    for (int i = 0; i < 256; i++)
        vm->mem[psp_addr + i] = 0;

    /* INT 20h at offset 0 (terminate instruction) */
    psp->int20 = 0x20CD;

    /* Memory size */
    psp->mem_top = mem_top_seg;

    /* Parent PSP = ourselves for now */
    psp->parent_psp = psp_seg;

    /* JFT: first 5 handles are stdin, stdout, stderr, stdaux, stdprn */
    for (int i = 0; i < 20; i++) psp->jft[i] = 0xFF;
    psp->jft[0] = 0;  /* stdin */
    psp->jft[1] = 1;  /* stdout */
    psp->jft[2] = 2;  /* stderr */
    psp->jft[3] = 3;  /* stdaux */
    psp->jft[4] = 4;  /* stdprn */
    psp->jft_size = 20;

    /* INT 21h dispatch at offset 0x50 */
    psp->dispatch[0] = 0xCD;  /* INT */
    psp->dispatch[1] = 0x21;
    psp->dispatch[2] = 0xCB;  /* RETF */

    /* Command tail */
    if (cmdline && cmdline[0]) {
        int len = 0;
        psp->cmd_tail[0] = ' ';
        for (len = 0; cmdline[len] && len < 125; len++)
            psp->cmd_tail[len + 1] = cmdline[len];
        psp->cmd_tail[len + 1] = 0x0D;
        psp->cmd_len = len + 1;
    } else {
        psp->cmd_tail[0] = 0x0D;
        psp->cmd_len = 0;
    }

    /* Environment segment (empty for now — single NUL byte) */
    /* We'll place a minimal env block just before the PSP */
    /* TODO: proper environment block */
}

/* ── Load COM binary ────────────────────────────────────────────── */

int dos_load_com(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                 const char *cmdline)
{
    if (size > 0xFF00) {
        serial_puts("[DOS] COM file too large (max 65280 bytes)\n");
        return -1;
    }

    /* Allocate maximum available memory */
    uint16_t largest = 0;
    uint16_t seg = dos_mem_alloc(vm, 0xFFFF, &largest);
    if (!seg) {
        /* Retry with largest available */
        seg = dos_mem_alloc(vm, largest, NULL);
        if (!seg) {
            serial_puts("[DOS] Failed to allocate memory for COM\n");
            return -1;
        }
    }

    /* PSP is at the allocated segment */
    uint16_t psp_seg = seg;
    uint16_t mem_top = seg + largest;
    vm->current_psp = psp_seg;

    build_psp(vm, psp_seg, mem_top, cmdline);

    /* Load COM data at PSP:0100h */
    uint32_t load_addr = dos_linear(psp_seg, 0x0100);
    for (uint64_t i = 0; i < size; i++)
        vm->mem[load_addr + i] = data[i];

    /* Set CPU state */
    cpu8086_state_t *cpu = vm->cpu;
    cpu->cs = psp_seg;
    cpu->ds = psp_seg;
    cpu->es = psp_seg;
    cpu->ss = psp_seg;
    cpu->ip = 0x0100;
    cpu->sp = 0xFFFE;

    /* Push a 0x0000 on stack (return to PSP:0000 = INT 20h) */
    cpu_push16(cpu, 0x0000);

    serial_puts("[DOS] COM loaded at ");
    serial_puthex(psp_seg, 4);
    serial_puts(":0100, ");
    serial_putdec(size);
    serial_puts(" bytes\n");

    return 0;
}

/* ── Load MZ EXE binary ─────────────────────────────────────────── */

int dos_load_mz(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                const char *cmdline)
{
    if (size < sizeof(mz_header_t)) {
        serial_puts("[DOS] MZ file too small\n");
        return -1;
    }

    const mz_header_t *hdr = (const mz_header_t *)data;

    /* Calculate load image size */
    uint32_t total_pages = hdr->e_cp;
    uint32_t image_size;
    if (hdr->e_cblp)
        image_size = (total_pages - 1) * 512 + hdr->e_cblp;
    else
        image_size = total_pages * 512;

    uint32_t header_size = (uint32_t)hdr->e_cparhdr * 16;
    uint32_t code_size = image_size - header_size;

    /* Paragraphs needed: code + minalloc + 16 (PSP) */
    uint32_t code_paras = (code_size + 15) / 16;
    uint32_t need_paras = code_paras + hdr->e_minalloc + 16;

    /* Try to allocate maxalloc, fall back to minalloc */
    uint32_t want_paras = code_paras + hdr->e_maxalloc + 16;
    if (want_paras > 0xFFFF) want_paras = 0xFFFF;

    uint16_t largest = 0;
    uint16_t seg = dos_mem_alloc(vm, (uint16_t)want_paras, &largest);
    if (!seg && largest >= need_paras) {
        seg = dos_mem_alloc(vm, largest, NULL);
    }
    if (!seg) {
        serial_puts("[DOS] Failed to allocate memory for MZ EXE\n");
        return -1;
    }

    uint16_t psp_seg = seg;
    vm->current_psp = psp_seg;

    /* Load segment = PSP + 16 paragraphs (256 bytes for PSP) */
    uint16_t load_seg = psp_seg + 0x10;

    build_psp(vm, psp_seg, seg + largest, cmdline);

    /* Copy code/data to load address */
    uint32_t load_addr = (uint32_t)load_seg << 4;
    const uint8_t *code_start = data + header_size;
    for (uint32_t i = 0; i < code_size && (load_addr + i) < DOS_MEM_SIZE; i++)
        vm->mem[load_addr + i] = code_start[i];

    /* Apply relocations */
    if (hdr->e_crlc > 0 && hdr->e_lfarlc > 0) {
        const mz_reloc_t *relocs = (const mz_reloc_t *)(data + hdr->e_lfarlc);
        for (int i = 0; i < hdr->e_crlc; i++) {
            uint32_t reloc_addr = dos_linear(load_seg + relocs[i].segment,
                                             relocs[i].offset);
            if (reloc_addr + 1 < DOS_MEM_SIZE) {
                uint16_t val = dos_mem_read16(vm, reloc_addr);
                val += load_seg;
                dos_mem_write16(vm, reloc_addr, val);
            }
        }
        serial_puts("[DOS] Applied ");
        serial_putdec(hdr->e_crlc);
        serial_puts(" relocations\n");
    }

    /* Set CPU state from MZ header */
    cpu8086_state_t *cpu = vm->cpu;
    cpu->cs = load_seg + hdr->e_cs;
    cpu->ip = hdr->e_ip;
    cpu->ss = load_seg + hdr->e_ss;
    cpu->sp = hdr->e_sp;
    cpu->ds = psp_seg;
    cpu->es = psp_seg;

    serial_puts("[DOS] MZ loaded at ");
    serial_puthex(load_seg, 4);
    serial_puts(", entry ");
    serial_puthex(cpu->cs, 4);
    serial_puts(":");
    serial_puthex(cpu->ip, 4);
    serial_puts(", ");
    serial_putdec(code_size);
    serial_puts(" bytes\n");

    return 0;
}
