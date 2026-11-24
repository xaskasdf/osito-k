#include "../kernel/kexec_image.h"

extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static struct {
    kexec_elf_header_t header;
    kexec_elf_program_t program[17];
    uint8_t bytes[4096];
} file;

static void valid_file(unsigned count)
{
    memset(&file, 0, sizeof(file));
    memcpy(file.header.ident, "\177ELF\2\1\1", 7);
    file.header.type = 2; file.header.machine = 62; file.header.version = 1;
    file.header.ehsize = sizeof(file.header);
    file.header.phentsize = sizeof(file.program[0]);
    file.header.phoff = __builtin_offsetof(__typeof__(file), program);
    file.header.phnum = count;
    file.header.entry = KEXEC_DIRECT_BASE + 0x2000000;
    for (unsigned i = 0; i < count; i++) {
        file.program[i] = (kexec_elf_program_t){
            .type = 1, .flags = i ? 6 : 5, .offset = 2048 + i * 16,
            .vaddr = KEXEC_DIRECT_BASE + 0x2000000 + i * 0x20000,
            .paddr = 0x2000000 + i * 0x20000, .filesz = 16, .memsz = 0x20000, .align = 1};
    }
}

static void test_elf(void)
{
    kexec_image_t plan;
    valid_file(2);
    CHECK(kexec_parse_image(&file, sizeof(file), &plan) == NULL);
    CHECK(plan.count == 2 && plan.low == 0x2000000 && plan.high == 0x2040000);
    CHECK(plan.staging_bytes == 8192);
    file.program[1].filesz = 0; file.program[1].memsz = 160 * 1024 * 1024;
    CHECK(kexec_parse_image(&file, sizeof(file), &plan) == NULL);
    CHECK(plan.staging_bytes == 4096); /* Large BSS does not need a live staging copy. */
    valid_file(16);
    CHECK(kexec_parse_image(&file, sizeof(file), &plan) == NULL && plan.count == 16);
    for (unsigned fault = 0; fault < 27; fault++) {
        valid_file(2);
        uint64_t size = sizeof(file);
        switch (fault) {
        case 0: size = sizeof(file.header) - 1; break;
        case 1: file.header.ident[0] = 0; break;
        case 2: file.header.ident[4] = 1; break;
        case 3: file.header.ident[5] = 2; break;
        case 4: file.header.type = 3; break;
        case 5: file.header.machine = 3; break;
        case 6: file.header.phoff = UINT64_MAX - 8; break;
        case 7: file.header.phnum = 129; break;
        case 8: file.header.phnum = 0; break;
        case 9: file.header.phentsize--; break;
        case 10: file.program[0].offset = UINT64_MAX - 4; break;
        case 11: file.program[0].filesz = UINT64_MAX; break;
        case 12: file.program[0].filesz = sizeof(file); break;
        case 13: file.program[0].memsz = 8; break;
        case 14: file.program[0].paddr = KEXEC_PHYS_LIMIT - 4096; break;
        case 15: file.program[0].memsz = UINT64_MAX; break;
        case 16: file.program[0].paddr = 0x7000; break;
        case 17: file.program[0].vaddr++; break;
        case 18: file.program[0].align = 3; break;
        case 19: file.program[0].align = 4096; break;
        case 20: file.program[1].paddr = file.program[0].paddr;
                 file.program[1].vaddr = file.program[0].vaddr; break;
        case 21: file.header.entry += 16; break; /* Entry in zero-fill, not code. */
        case 22: file.program[0].flags = 6; break;
        case 23: file.header.entry = 0x2000000; break;
        case 24: valid_file(17); break;
        case 25: file.header.phoff = size - sizeof(file.program[0]); break;
        case 26: file.header.ehsize--; break;
        }
        CHECK(kexec_parse_image(&file, size, &plan) != NULL);
    }
    valid_file(1);
    CHECK(kexec_parse_image(NULL, sizeof(file), &plan) != NULL);
    file.program[0].offset = 4096; file.program[0].align = 4096;
    CHECK(kexec_parse_image(&file, sizeof(file), &plan) == NULL);
    CHECK(kexec_overlap(128ULL << 20, 7ULL << 20, 32ULL << 20, 155ULL << 20));
    CHECK(!kexec_overlap(4096, 4096, 8192, 4096));
    CHECK(kexec_overlap(UINT64_MAX - 3, 8, 4096, 4096));
    CHECK(!kexec_overlap(4096, 0, 4096, 4096));
}

static void test_ram(void)
{
    struct { kexec_memory_descriptor_t d; uint64_t extension; } map[3] = {
        {{.type = 7, .physical = 0x3000000, .pages = 0x1000}, 0},
        {{.type = 2, .physical = 0x2000000, .pages = 0x1000}, 0},
        {{.type = 11, .physical = 0x4000000, .pages = 0x1000}, 0}
    };
    boot_info_t b = {.mmap_addr = (uint64_t)map, .mmap_size = sizeof(map), .mmap_desc_size = sizeof(map[0])};
    CHECK(kexec_destination_is_ram(&b, 0x2000000, 0x2000000));
    CHECK(!kexec_destination_is_ram(&b, 0x2000000, 0x2000001));
    CHECK(!kexec_destination_is_ram(&b, 0x1fff000, 0x2000));
    CHECK(!kexec_destination_is_ram(&b, KEXEC_PHYS_LIMIT - 4096, 8192));
    CHECK(!kexec_destination_is_ram(&b, 0x2000000, 0));
    map[0].d.physical += 4096;
    CHECK(!kexec_destination_is_ram(&b, 0x2000000, 0x2000000));
    map[0].d.physical -= 4096;
    map[2].d.physical = 0x2800000; /* Reserved descriptor wins even if firmware map overlaps. */
    CHECK(!kexec_destination_is_ram(&b, 0x2000000, 0x1000000));
    map[2].d.physical = 0x4000000;
    for (unsigned type = 0; type <= 14; type++) {
        map[1].d.type = type;
        CHECK(kexec_destination_is_ram(&b, 0x2000000, 4096) ==
              (type == 1 || type == 2 || type == 3 || type == 4 || type == 7));
    }
    map[1].d.type = 7; map[1].d.pages = UINT64_MAX;
    CHECK(!kexec_destination_is_ram(&b, 0x2000000, 4096));
    map[1].d.pages = 0x1000;
    b.mmap_size--; CHECK(!kexec_destination_is_ram(&b, 0x2000000, 4096));
    b.mmap_size++; b.mmap_desc_size = 32;
    CHECK(!kexec_destination_is_ram(&b, 0x2000000, 4096));
}

static void test_boot_abi(void)
{
    size_t v2_size = __builtin_offsetof(boot_info_t, handoff_data_base);
    void *old = malloc(v2_size);
    boot_info_t b = {.magic = BOOT_INFO_MAGIC, .version = 2, .fb_base = 0x4000000000ULL};
    boot_info_t copy;
    memcpy(old, &b, v2_size);
    CHECK(boot_info_import(&copy, old)); /* ASan guards exact-size v2 input. */
    CHECK(copy.fb_base == b.fb_base && copy.handoff_data_size == 0 && copy.handoff_stack_size == 0);
    free(old);
    b.version = 3; b.handoff_stack_base = 0x8000000; b.handoff_stack_size = 0x100000;
    CHECK(boot_info_import(&copy, &b) && copy.handoff_stack_size == b.handoff_stack_size);
    b.version = 4; CHECK(!boot_info_import(&copy, &b));
    b.version = 1; CHECK(!boot_info_import(&copy, &b));
    b.version = 3; b.magic = 0; CHECK(!boot_info_import(&copy, &b));
    CHECK(!boot_info_import(&copy, NULL));
}

int main(void)
{
    test_elf(); test_ram(); test_boot_abi();
    printf("kexec image/ownership: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
