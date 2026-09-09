/*
 * OsitoK — DOS Binary Loader
 *
 * Loads COM and MZ (EXE) DOS binaries into emulated 1MB memory.
 * COM: raw code loaded at PSP:0100h, all segments = PSP.
 * MZ:  relocatable EXE with header, segments, and relocation table.
 */

#include "cpu8086.h"
#include "dos_audio.h"
#include "dos_loader.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Forward declarations */
uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest);
int      dos_mem_free(dos_vm_t *vm, uint16_t segment);
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

static uint16_t dos_block_size(const dos_vm_t *vm, uint16_t segment)
{
    const dos_mcb_t *mcb = (const dos_mcb_t *)(vm->mem +
                                               ((uint32_t)(segment - 1) << 4));
    return mcb->size;
}

static void dos_set_block_owner(dos_vm_t *vm, uint16_t segment,
                                uint16_t owner, const char *name)
{
    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem +
                                   ((uint32_t)(segment - 1) << 4));
    mcb->owner = owner;
    for (int i = 0; i < 8; i++) mcb->name[i] = 0;
    if (!name) return;

    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    for (int i = 0; i < 8 && base[i] && base[i] != '.'; i++) {
        char ch = base[i];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        mcb->name[i] = ch;
    }
}

static bool dos_env_append_char(char *buffer, uint32_t capacity,
                                uint32_t *length, char value)
{
    if (*length + 1U >= capacity)
        return false;
    buffer[(*length)++] = value;
    return true;
}

static bool dos_env_append_text(char *buffer, uint32_t capacity,
                                uint32_t *length, const char *text)
{
    while (*text) {
        if (!dos_env_append_char(buffer, capacity, length, *text++))
            return false;
    }
    return true;
}

static bool dos_env_append_number(char *buffer, uint32_t capacity,
                                  uint32_t *length, uint32_t value,
                                  uint32_t base)
{
    static const char digits[] = "0123456789ABCDEF";
    char reversed[8];
    uint32_t count = 0;

    do {
        reversed[count++] = digits[value % base];
        value /= base;
    } while (value && count < sizeof(reversed));

    while (count) {
        if (!dos_env_append_char(buffer, capacity, length,
                                 reversed[--count]))
            return false;
    }
    return true;
}

static bool dos_build_blaster_variable(const dos_vm_t *vm, char *buffer,
                                       uint32_t capacity,
                                       uint32_t *length_out)
{
    dos_audio_resources_t resources;
    if (!buffer || !length_out ||
        !dos_audio_get_resources(vm, &resources))
        return false;

    uint32_t length = 0;
    if (!dos_env_append_text(buffer, capacity, &length, "BLASTER=A") ||
        !dos_env_append_number(buffer, capacity, &length,
                               resources.base_port, 16) ||
        !dos_env_append_text(buffer, capacity, &length, " I") ||
        !dos_env_append_number(buffer, capacity, &length, resources.irq, 10) ||
        !dos_env_append_text(buffer, capacity, &length, " D") ||
        !dos_env_append_number(buffer, capacity, &length, resources.dma8, 10) ||
        !dos_env_append_text(buffer, capacity, &length, " H") ||
        !dos_env_append_number(buffer, capacity, &length,
                               resources.dma16, 10) ||
        !dos_env_append_text(buffer, capacity, &length, " T") ||
        !dos_env_append_number(buffer, capacity, &length,
                               resources.card_type, 10))
        return false;

    buffer[length] = 0;
    *length_out = length;
    return true;
}

static uint16_t build_default_environment(dos_vm_t *vm,
                                          const char *progname)
{
    if (!progname) progname = "PROGRAM.EXE";
    char blaster_buffer[48];
    uint32_t blaster_len = 0;
    const char *blaster = dos_build_blaster_variable(
        vm, blaster_buffer, sizeof(blaster_buffer), &blaster_len)
        ? blaster_buffer : NULL;
    uint32_t name_len = 0;
    while (progname[name_len] && name_len < 0xFFF0u) name_len++;
    if (progname[name_len]) return 0;

    uint32_t environment_bytes = blaster ? blaster_len + 1U : 1U;
    uint32_t bytes = environment_bytes + 1U + 2U + name_len + 1U;
    uint16_t paragraphs = (uint16_t)((bytes + 15) >> 4);
    uint16_t env_seg = dos_mem_alloc(vm, paragraphs, NULL);
    if (!env_seg) return 0;

    uint32_t address = (uint32_t)env_seg << 4;
    for (uint32_t i = 0; i < ((uint32_t)paragraphs << 4); i++)
        vm->mem[address + i] = 0;

    uint32_t offset = 0;
    if (blaster) {
        for (uint32_t i = 0; i < blaster_len; i++)
            vm->mem[address + offset++] = (uint8_t)blaster[i];
        offset++;
    } else {
        offset++;
    }
    offset++;
    dos_mem_write16(vm, address + offset, 1);
    offset += 2U;
    for (uint32_t i = 0; i < name_len; i++)
        vm->mem[address + offset + i] = (uint8_t)progname[i];
    return env_seg;
}

static int build_copied_environment(dos_vm_t *vm, uint16_t source_segment,
                                    const char *progname,
                                    uint16_t *environment_out)
{
    enum { DOS_ENVIRONMENT_MAX = 0x8000 };
    if (!vm || !vm->mem || !environment_out)
        return DOS_ERR_BAD_ENVIRONMENT;
    *environment_out = 0;
    if (!progname) progname = "PROGRAM.EXE";

    uint32_t name_length = 0;
    while (progname[name_length] && name_length < DOS_ENVIRONMENT_MAX)
        name_length++;
    if (progname[name_length]) return DOS_ERR_BAD_ENVIRONMENT;

    uint32_t source_address = (uint32_t)source_segment << 4;
    uint32_t environment_length = 2;
    if (source_segment) {
        if (source_address >= vm->total_mem_size)
            return DOS_ERR_BAD_ENVIRONMENT;
        uint32_t available = vm->total_mem_size - source_address;
        uint32_t limit = available < DOS_ENVIRONMENT_MAX
                       ? available : DOS_ENVIRONMENT_MAX;
        bool terminated = false;
        for (uint32_t i = 0; i + 1u < limit; i++) {
            if (dos_mem_read8(vm, source_address + i) == 0 &&
                dos_mem_read8(vm, source_address + i + 1u) == 0) {
                environment_length = i + 2u;
                terminated = true;
                break;
            }
        }
        if (!terminated) return DOS_ERR_BAD_ENVIRONMENT;
    }

    uint32_t bytes = environment_length + 2u + name_length + 1u;
    if (bytes > 0xFFFF0u) return DOS_ERR_BAD_ENVIRONMENT;
    uint16_t paragraphs = (uint16_t)((bytes + 15u) >> 4);
    uint16_t environment = dos_mem_alloc(vm, paragraphs, NULL);
    if (!environment) return DOS_ERR_NOT_ENOUGH_MEMORY;

    uint32_t destination = (uint32_t)environment << 4;
    uint32_t allocation_bytes = (uint32_t)paragraphs << 4;
    for (uint32_t i = 0; i < allocation_bytes; i++)
        vm->mem[destination + i] = 0;
    if (source_segment) {
        for (uint32_t i = 0; i < environment_length; i++)
            vm->mem[destination + i] =
                dos_mem_read8(vm, source_address + i);
    }
    dos_mem_write16(vm, destination + environment_length, 1u);
    uint32_t name_address = destination + environment_length + 2u;
    for (uint32_t i = 0; i < name_length; i++)
        vm->mem[name_address + i] = (uint8_t)progname[i];
    *environment_out = environment;
    return 0;
}

static int build_process_environment(dos_vm_t *vm, const char *progname,
                                     const dos_load_spec_t *spec,
                                     uint16_t *environment_out)
{
    if (spec && spec->copy_environment)
        return build_copied_environment(vm, spec->environment_source,
                                        progname, environment_out);
    *environment_out = build_default_environment(vm, progname);
    return *environment_out ? 0 : DOS_ERR_NOT_ENOUGH_MEMORY;
}

void dos_psp_initialize_system_fields(dos_vm_t *vm, dos_psp_t *psp,
                                      uint16_t psp_segment,
                                      uint16_t mem_top)
{
    psp->int20 = 0x20CDu;
    psp->mem_top = mem_top;

    uint16_t allocation = (uint16_t)(mem_top - psp_segment);
    if (allocation > 0x0FFFu) allocation = 0x0FFFu;
    uint16_t cpm_paragraphs = allocation >= 0x0010u
                            ? (uint16_t)(allocation - 0x0010u) : 0u;
    uint16_t cpm_offset = (uint16_t)(cpm_paragraphs << 4);
    uint16_t cpm_segment = (uint16_t)(0x000Cu - cpm_paragraphs);
    psp->dos_call[0] = 0x9Au;
    psp->dos_call[1] = (uint8_t)cpm_offset;
    psp->dos_call[2] = (uint8_t)(cpm_offset >> 8);
    psp->dos_call[3] = (uint8_t)cpm_segment;
    psp->dos_call[4] = (uint8_t)(cpm_segment >> 8);

    psp->old_int22 = dos_mem_read32(vm, 0x22u * 4u);
    psp->old_int23 = dos_mem_read32(vm, 0x23u * 4u);
    psp->old_int24 = dos_mem_read32(vm, 0x24u * 4u);
    psp->jft_size = DOS_PSP_JFT_ENTRIES;
    psp->jft_ptr = ((uint32_t)psp_segment << 16) |
                   __builtin_offsetof(dos_psp_t, jft);

    for (unsigned i = 0; i < 4u; i++) psp->reserved2[i] = 0xFFu;
    psp->dispatch[0] = 0xCDu;
    psp->dispatch[1] = 0x21u;
    psp->dispatch[2] = 0xCBu;
}

static void build_psp(dos_vm_t *vm, uint16_t psp_seg, uint16_t mem_top_seg,
                      uint16_t env_seg, const dos_load_spec_t *spec)
{
    uint32_t psp_addr = (uint32_t)psp_seg << 4;
    dos_psp_t *psp = (dos_psp_t *)(vm->mem + psp_addr);

    /* Zero the entire PSP */
    for (int i = 0; i < 256; i++)
        vm->mem[psp_addr + i] = 0;

    dos_psp_initialize_system_fields(vm, psp, psp_seg, mem_top_seg);
    if (spec && spec->has_termination_address) {
        psp->old_int22 = ((uint32_t)spec->termination_segment << 16) |
                         spec->termination_offset;
    }
    psp->parent_psp = spec && spec->parent_psp
                    ? spec->parent_psp : psp_seg;

    /* The PSP owns a JFT, not independent file state. Copy the kernel's
     * JFT-to-SFT mapping so duplicated standard handles remain shared. */
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
        psp->jft[i] = spec && spec->jft
                    ? spec->jft[i] : vm->bootstrap_jft[i].sft_index;
    vm->jft_external_segment = 0;
    vm->jft_external_psp = 0;
    vm->jft_active = true;
    psp->env_seg = env_seg;

    if (spec && spec->fcb1) {
        for (unsigned i = 0; i < 16u; i++) psp->fcb1[i] = spec->fcb1[i];
    }
    if (spec && spec->fcb2) {
        for (unsigned i = 0; i < 16u; i++) psp->fcb2[i] = spec->fcb2[i];
    }

    if (spec && spec->command_tail) {
        uint8_t *tail = &psp->cmd_len;
        for (unsigned i = 0; i < 128u; i++) tail[i] = spec->command_tail[i];
    } else if (spec && spec->command_line && spec->command_line[0]) {
        int len = 0;
        psp->cmd_tail[0] = ' ';
        for (len = 0; spec->command_line[len] && len < 125; len++)
            psp->cmd_tail[len + 1] = spec->command_line[len];
        psp->cmd_tail[len + 1] = 0x0D;
        psp->cmd_len = len + 1;
    } else {
        psp->cmd_tail[0] = 0x0D;
        psp->cmd_len = 0;
    }

}

/* ── Load COM binary ────────────────────────────────────────────── */

static uint16_t dos_initial_fcb_ax(const dos_load_spec_t *spec)
{
    uint8_t first = spec && spec->fcb1 ? spec->fcb1[0] : 0;
    uint8_t second = spec && spec->fcb2 ? spec->fcb2[0] : 0;
    uint8_t low = first <= 26u ? 0u : 0xFFu;
    uint8_t high = second <= 26u ? 0u : 0xFFu;
    return (uint16_t)low | ((uint16_t)high << 8);
}

static void dos_initialize_process_cpu(dos_vm_t *vm,
                                       const dos_load_spec_t *spec,
                                       uint16_t psp_seg, uint16_t cs,
                                       uint16_t ip, uint16_t ss,
                                       uint16_t sp)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (spec && spec->initialize_cpu) {
        cpu8086_init(cpu, vm);
        uint16_t fcb_ax = dos_initial_fcb_ax(spec);
        cpu->ax = fcb_ax;
        cpu->bx = fcb_ax;
        cpu->cx = 0x00FFu;
        cpu->dx = psp_seg;
        cpu->si = ip;
        cpu->di = sp;
        cpu->bp = 0x091Eu;
        cpu->flags = FLAG_IF | FLAGS_FIXED;
    }
    cpu8086_reset_real_cs(cpu, cs);
    cpu->ds = psp_seg;
    cpu->es = psp_seg;
    cpu->ss = ss;
    cpu8086_sync_data(cpu);
    cpu->ip = ip;
    cpu->sp = sp;
    cpu->halted = false;
    cpu->running = true;
    cpu->exit_code = 0;
}

int dos_load_com_process(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                         const char *progname,
                         const dos_load_spec_t *spec,
                         uint16_t *psp_out)
{
    if (psp_out) *psp_out = 0;
    if (!vm || !vm->mem || !vm->cpu || !data || !size || size > 0xFF00u) {
        serial_puts("[DOS] COM file too large (max 65280 bytes)\n");
        return DOS_ERR_BAD_FORMAT;
    }
    if (spec && spec->command_tail && spec->command_tail[0] > 126u)
        return DOS_ERR_BAD_ENVIRONMENT;

    uint16_t original_psp = vm->current_psp;
    if (!vm->current_psp) vm->current_psp = 0x0008;

    uint16_t env_seg = 0;
    int error = build_process_environment(vm, progname, spec, &env_seg);
    if (error) {
        serial_puts("[DOS] Failed to allocate environment\n");
        vm->current_psp = original_psp;
        return error;
    }

    /* Allocate maximum available memory after reserving the environment. */
    uint16_t largest = 0;
    uint16_t seg = dos_mem_alloc(vm, 0xFFFF, &largest);
    if (!seg) {
        uint32_t required_bytes = 0x100u + (uint32_t)size;
        uint16_t required = (uint16_t)((required_bytes + 15u) >> 4);
        if (largest < required) {
            dos_mem_free(vm, env_seg);
            vm->current_psp = original_psp;
            return DOS_ERR_NOT_ENOUGH_MEMORY;
        }
        seg = dos_mem_alloc(vm, largest, NULL);
        if (!seg) {
            serial_puts("[DOS] Failed to allocate memory for COM\n");
            dos_mem_free(vm, env_seg);
            vm->current_psp = original_psp;
            return DOS_ERR_NOT_ENOUGH_MEMORY;
        }
    }

    /* PSP is at the allocated segment */
    uint16_t psp_seg = seg;
    uint16_t mem_top = (uint16_t)(seg + dos_block_size(vm, seg));
    vm->current_psp = psp_seg;
    dos_set_block_owner(vm, env_seg, psp_seg, "ENV");
    dos_set_block_owner(vm, psp_seg, psp_seg, progname);

    build_psp(vm, psp_seg, mem_top, env_seg, spec);

    /* Load COM data at PSP:0100h */
    uint32_t load_addr = dos_linear(psp_seg, 0x0100);
    for (uint64_t i = 0; i < size; i++)
        vm->mem[load_addr + i] = data[i];

    /* Set CPU state */
    cpu8086_state_t *cpu = vm->cpu;
    dos_initialize_process_cpu(vm, spec, psp_seg, psp_seg, 0x0100,
                               psp_seg, 0xFFFE);

    /* Push a 0x0000 on stack (return to PSP:0000 = INT 20h) */
    cpu_push16(cpu, 0x0000);

    serial_puts("[DOS] COM loaded at ");
    serial_puthex(psp_seg, 4);
    serial_puts(":0100, ");
    serial_putdec(size);
    serial_puts(" bytes\n");

    if (psp_out) *psp_out = psp_seg;
    return 0;
}

int dos_load_com(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                 const char *progname, const char *cmdline)
{
    dos_load_spec_t spec = {0};
    spec.command_line = cmdline;
    spec.initialize_cpu = true;
    int error = dos_load_com_process(vm, data, size, progname, &spec, NULL);
    return error ? -1 : 0;
}

/* ── Load MZ EXE binary ─────────────────────────────────────────── */

static int dos_parse_mz(const uint8_t *data, uint64_t size,
                        uint32_t *header_size_out,
                        uint32_t *code_size_out)
{
    if (!data || size < sizeof(mz_header_t)) return DOS_ERR_BAD_FORMAT;

    const mz_header_t *hdr = (const mz_header_t *)data;
    if ((hdr->e_magic != MZ_MAGIC && hdr->e_magic != ZM_MAGIC) ||
        !hdr->e_cp || hdr->e_cblp > 511u)
        return DOS_ERR_BAD_FORMAT;

    uint64_t image_size;
    if (hdr->e_cblp)
        image_size = ((uint64_t)hdr->e_cp - 1u) * 512u + hdr->e_cblp;
    else
        image_size = (uint64_t)hdr->e_cp * 512u;

    uint32_t header_size = (uint32_t)hdr->e_cparhdr * 16;
    if (header_size < sizeof(mz_header_t) || header_size > image_size ||
        image_size > size || image_size > UINT32_MAX)
        return DOS_ERR_BAD_FORMAT;
    uint32_t code_size = (uint32_t)image_size - header_size;

    uint64_t relocation_end = (uint64_t)hdr->e_lfarlc +
                              (uint64_t)hdr->e_crlc * sizeof(mz_reloc_t);
    if (hdr->e_crlc && (hdr->e_lfarlc < sizeof(mz_header_t) ||
                        relocation_end > header_size))
        return DOS_ERR_BAD_FORMAT;
    const mz_reloc_t *relocations =
        (const mz_reloc_t *)(data + hdr->e_lfarlc);
    for (uint32_t i = 0; i < hdr->e_crlc; i++) {
        uint64_t target = (uint64_t)relocations[i].segment * 16u +
                          relocations[i].offset;
        if (target + 2u > code_size) return DOS_ERR_BAD_FORMAT;
    }

    *header_size_out = header_size;
    *code_size_out = code_size;
    return 0;
}

static int dos_validate_mz_process(const uint8_t *data, uint64_t size,
                                   uint32_t *header_size_out,
                                   uint32_t *code_size_out,
                                   uint32_t *minimum_paragraphs_out,
                                   uint32_t *wanted_paragraphs_out)
{
    uint32_t header_size, code_size;
    int error = dos_parse_mz(data, size, &header_size, &code_size);
    if (error) return error;

    const mz_header_t *hdr = (const mz_header_t *)data;
    uint32_t code_paras = (code_size + 15u) / 16u;
    uint32_t need_paras = code_paras + hdr->e_minalloc + 16;
    if (need_paras > 0xFFFFu) return DOS_ERR_NOT_ENOUGH_MEMORY;

    uint64_t wanted = (uint64_t)code_paras + hdr->e_maxalloc + 16u;
    uint32_t want_paras = wanted > 0xFFFFu ? 0xFFFFu : (uint32_t)wanted;
    if (want_paras < need_paras) want_paras = need_paras;

    uint64_t entry = (uint64_t)hdr->e_cs * 16u + hdr->e_ip;
    if (entry >= code_size) return DOS_ERR_BAD_FORMAT;

    *header_size_out = header_size;
    *code_size_out = code_size;
    *minimum_paragraphs_out = need_paras;
    *wanted_paragraphs_out = want_paras;
    return 0;
}

int dos_load_overlay(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                     uint16_t load_segment, uint16_t relocation_factor)
{
    if (!vm || !vm->mem || !data || !size) return DOS_ERR_BAD_FORMAT;

    uint64_t load_address = (uint64_t)load_segment << 4;
    if (load_address >= vm->total_mem_size)
        return DOS_ERR_NOT_ENOUGH_MEMORY;

    if (dos_detect_format(data, size) != DOS_FMT_MZ) {
        uint32_t copy_size = size > 0xFFFEu ? 0xFFFEu : (uint32_t)size;
        if (copy_size > vm->total_mem_size - load_address)
            return DOS_ERR_NOT_ENOUGH_MEMORY;
        for (uint32_t i = 0; i < copy_size; i++)
            vm->mem[load_address + i] = data[i];
        return 0;
    }

    uint32_t header_size, code_size;
    int error = dos_parse_mz(data, size, &header_size, &code_size);
    if (error) return error;
    if (code_size > vm->total_mem_size - load_address)
        return DOS_ERR_NOT_ENOUGH_MEMORY;

    for (uint32_t i = 0; i < code_size; i++)
        vm->mem[load_address + i] = data[header_size + i];

    const mz_header_t *hdr = (const mz_header_t *)data;
    const mz_reloc_t *relocs = (const mz_reloc_t *)(data + hdr->e_lfarlc);
    for (uint32_t i = 0; i < hdr->e_crlc; i++) {
        uint32_t target = (uint32_t)relocs[i].segment * 16u +
                          relocs[i].offset;
        uint32_t address = (uint32_t)load_address + target;
        dos_mem_write16(vm, address,
                        (uint16_t)(dos_mem_read16(vm, address) +
                                   relocation_factor));
    }
    return 0;
}

int dos_load_mz_process(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                        const char *progname,
                        const dos_load_spec_t *spec,
                        uint16_t *psp_out)
{
    if (psp_out) *psp_out = 0;
    if (!vm || !vm->mem || !vm->cpu) return DOS_ERR_BAD_FORMAT;
    if (spec && spec->command_tail && spec->command_tail[0] > 126u)
        return DOS_ERR_BAD_ENVIRONMENT;

    uint32_t header_size, code_size, need_paras, want_paras;
    int error = dos_validate_mz_process(data, size, &header_size, &code_size,
                                        &need_paras, &want_paras);
    if (error) {
        serial_puts("[DOS] Invalid MZ image\n");
        return error;
    }
    const mz_header_t *hdr = (const mz_header_t *)data;

    uint16_t original_psp = vm->current_psp;
    if (!vm->current_psp) vm->current_psp = 0x0008;
    uint16_t env_seg = 0;
    error = build_process_environment(vm, progname, spec, &env_seg);
    if (error) {
        serial_puts("[DOS] Failed to allocate environment\n");
        vm->current_psp = original_psp;
        return error;
    }

    uint16_t largest = 0;
    uint16_t seg = dos_mem_alloc(vm, (uint16_t)want_paras, &largest);
    if (!seg && largest >= need_paras) {
        seg = dos_mem_alloc(vm, largest, NULL);
    }
    if (!seg) {
        serial_puts("[DOS] Failed to allocate memory for MZ EXE\n");
        dos_mem_free(vm, env_seg);
        vm->current_psp = original_psp;
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    }

    uint32_t block_bytes = (uint32_t)dos_block_size(vm, seg) << 4;
    uint64_t stack = 0x100u + (uint64_t)hdr->e_ss * 16u + hdr->e_sp;
    if (stack > block_bytes) {
        dos_mem_free(vm, seg);
        dos_mem_free(vm, env_seg);
        vm->current_psp = original_psp;
        return DOS_ERR_BAD_FORMAT;
    }

    uint16_t psp_seg = seg;
    vm->current_psp = psp_seg;

    dos_set_block_owner(vm, env_seg, psp_seg, "ENV");
    dos_set_block_owner(vm, psp_seg, psp_seg, progname);

    /* Load segment = PSP + 16 paragraphs (256 bytes for PSP) */
    uint16_t load_seg = psp_seg + 0x10;

    build_psp(vm, psp_seg,
              (uint16_t)(seg + dos_block_size(vm, seg)), env_seg, spec);

    /* Copy code/data to load address */
    uint32_t load_addr = (uint32_t)load_seg << 4;
    const uint8_t *code_start = data + header_size;
    for (uint32_t i = 0; i < code_size; i++)
        vm->mem[load_addr + i] = code_start[i];

    /* Apply relocations */
    if (hdr->e_crlc > 0 && hdr->e_lfarlc > 0) {
        const mz_reloc_t *relocs = (const mz_reloc_t *)(data + hdr->e_lfarlc);
        for (uint32_t i = 0; i < hdr->e_crlc; i++) {
            uint32_t reloc_addr = dos_linear(load_seg + relocs[i].segment,
                                             relocs[i].offset);
            uint16_t val = dos_mem_read16(vm, reloc_addr);
            val += load_seg;
            dos_mem_write16(vm, reloc_addr, val);
        }
        serial_puts("[DOS] Applied ");
        serial_putdec(hdr->e_crlc);
        serial_puts(" relocations\n");
    }

    /* Set CPU state from MZ header */
    cpu8086_state_t *cpu = vm->cpu;
    dos_initialize_process_cpu(vm, spec, psp_seg,
                               (uint16_t)(load_seg + hdr->e_cs), hdr->e_ip,
                               (uint16_t)(load_seg + hdr->e_ss), hdr->e_sp);

    serial_puts("[DOS] MZ loaded at ");
    serial_puthex(load_seg, 4);
    serial_puts(", entry ");
    serial_puthex(cpu->cs, 4);
    serial_puts(":");
    serial_puthex(cpu->ip, 4);
    serial_puts(", ");
    serial_putdec(code_size);
    serial_puts(" bytes\n");

    if (psp_out) *psp_out = psp_seg;
    return 0;
}

int dos_load_mz(dos_vm_t *vm, const uint8_t *data, uint64_t size,
                const char *progname, const char *cmdline)
{
    dos_load_spec_t spec = {0};
    spec.command_line = cmdline;
    spec.initialize_cpu = true;
    int error = dos_load_mz_process(vm, data, size, progname, &spec, NULL);
    return error ? -1 : 0;
}
