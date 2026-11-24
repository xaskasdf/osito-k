/* Host checks of the real decoder. PCI snapshots are synthetic, not P1 reads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_ggtt.h"

#define MB (1024ULL * 1024)
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static intel_ggtt_config_t make_config(void)
{
    intel_ggtt_config_t c = {
        .host_id = 0x3ec48086, .igd_id = 0x3e9b8086, .command = 2,
        .ggc = 0x0281, /* 64 MB data, 4 MB table, locked */
        .bdsm = 0xbc000001, .bgsm = 0xbbc00001, .tolud = 0xc0000001,
        .bar0_lo = 0x40000004, .bar0_hi = 0x60,
        .bar2_lo = 0x0000000c, .bar2_hi = 0x40,
        .msac = 1 /* 256 MB aperture */
    };
    return c;
}

static void expect_error(intel_ggtt_config_t c, enum intel_ggtt_status error)
{
    intel_ggtt_layout_t out, saved;
    memset(&out, 0xa5, sizeof(out));
    saved = out;
    CHECK(intel_ggtt_decode(&c, &out) == error);
    CHECK(memcmp(&out, &saved, sizeof(out)) == 0);
}

static void test_layouts(void)
{
    /* Independent transcription of GGC.GMS legal encodings (MB). Zero entries
     * except index 0 are reserved. In particular, FF does NOT mean 64 MB. */
    static const unsigned sizes[256] = {
        [1]=32, [2]=64, [3]=96, [4]=128, [5]=160, [6]=192, [7]=224, [8]=256,
        [9]=288, [10]=320, [11]=352, [12]=384, [13]=416, [14]=448,
        [15]=480, [16]=512, [0x20]=1024, [0x30]=1536, [0x40]=2048,
        [0xf0]=4, [0xf1]=8, [0xf2]=12, [0xf3]=16, [0xf4]=20, [0xf5]=24,
        [0xf6]=28, [0xf7]=32, [0xf8]=36, [0xf9]=40, [0xfa]=44,
        [0xfb]=48, [0xfc]=52, [0xfd]=56, [0xfe]=60
    };
    static const unsigned table_mb[4] = {0, 2, 4, 8};
    for (unsigned gms = 0; gms < 256; gms++) {
        for (unsigned ggms = 0; ggms < 4; ggms++) {
            intel_ggtt_config_t c = make_config();
            c.ggc = (gms << 8) | (ggms << 6) | 7;
            c.bdsm = (uint32_t)(0xc0000000ULL - sizes[gms] * MB) | 1;
            c.bgsm = (uint32_t)((c.bdsm & ~1u) - table_mb[ggms] * MB) | 1;
            if (gms && !sizes[gms]) {
                expect_error(c, INTEL_GGTT_BAD_GGC);
                continue;
            }
            if (!ggms) {
                expect_error(c, INTEL_GGTT_DISABLED);
                continue;
            }
            intel_ggtt_layout_t v;
            CHECK(intel_ggtt_decode(&c, &v) == INTEL_GGTT_OK);
            CHECK(v.stolen_bytes == sizes[gms] * MB);
            CHECK(v.stolen_base + v.stolen_bytes == 0xc0000000);
            CHECK(v.table_stolen_base + v.ggtt_bytes == v.stolen_base);
            CHECK(v.ggtt_bytes == table_mb[ggms] * MB);
            CHECK(v.gpu_va_bytes == table_mb[ggms] * 512 * MB);
            CHECK(v.ggtt_mmio_base == 0x6040800000ULL);
        }
    }
}

static void test_apertures(void)
{
    static const unsigned sizes[32] = {
        [0]=128, [1]=256, [3]=512, [7]=1024, [15]=2048, [31]=4096
    };
    for (unsigned msac = 0; msac < 256; msac++) {
        intel_ggtt_config_t c = make_config();
        c.msac = (uint8_t)msac;
        if (!sizes[msac & 31]) {
            expect_error(c, INTEL_GGTT_BAD_APERTURE);
            continue;
        }
        intel_ggtt_layout_t v;
        CHECK(intel_ggtt_decode(&c, &v) == INTEL_GGTT_OK);
        CHECK(v.aperture_base == 0x4000000000ULL);
        CHECK(v.aperture_bytes == sizes[msac & 31] * MB);
        uint64_t offset = 99;
        CHECK(intel_ggtt_aperture_offset(&v, v.aperture_base, 1, &offset));
        CHECK(offset == 0);
        /* A 4 GB BAR with a 2 GB GGTT cannot translate its full aperture. */
        if (v.aperture_bytes > v.gpu_va_bytes) {
            CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base,
                                              v.aperture_bytes, &offset));
            CHECK(intel_ggtt_aperture_offset(&v, v.aperture_base,
                                             v.gpu_va_bytes, &offset));
        }
    }
}

static void test_ranges(void)
{
    intel_ggtt_config_t c = make_config();
    intel_ggtt_layout_t v;
    CHECK(intel_ggtt_decode(&c, &v) == INTEL_GGTT_OK);
    uint64_t offset = 123;
    CHECK(intel_ggtt_aperture_offset(&v, 0x4000000000ULL, 7680 * 1080, &offset));
    CHECK(offset == 0);
    CHECK(intel_ggtt_aperture_offset(&v, v.aperture_base + v.aperture_bytes - 1,
                                     1, &offset));
    CHECK(offset == v.aperture_bytes - 1);
    uint64_t before = offset;
    CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base - 1, 1, &offset));
    CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base, 0, &offset));
    CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base + v.aperture_bytes,
                                      1, &offset));
    CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base + v.aperture_bytes - 1,
                                      2, &offset));
    CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base + 1, UINT64_MAX, &offset));
    CHECK(!intel_ggtt_aperture_offset(&v, UINT64_MAX, 1, &offset));
    CHECK(!intel_ggtt_aperture_offset(NULL, v.aperture_base, 1, &offset));
    CHECK(!intel_ggtt_aperture_offset(&v, v.aperture_base, 1, NULL));
    CHECK(offset == before);

    uint32_t reg = 99;
    CHECK(intel_ggtt_pte_offset(&v, 0, &reg));
    CHECK(reg == 0x800000);
    CHECK(intel_ggtt_pte_offset(&v, 4095, &reg));
    CHECK(reg == 0x800000);
    CHECK(intel_ggtt_pte_offset(&v, 4096, &reg));
    CHECK(reg == 0x800008);
    CHECK(intel_ggtt_pte_offset(&v, v.gpu_va_bytes - 1, &reg));
    CHECK(reg == 0xbffff8);
    uint32_t saved = reg;
    CHECK(!intel_ggtt_pte_offset(&v, v.gpu_va_bytes, &reg));
    CHECK(!intel_ggtt_pte_offset(&v, UINT64_MAX, &reg));
    CHECK(!intel_ggtt_pte_offset(NULL, 0, &reg));
    CHECK(!intel_ggtt_pte_offset(&v, 0, NULL));
    CHECK(reg == saved);
    /* Even a corrupted layout cannot direct the read outside the BAR. */
    v.ggtt_bytes = UINT64_MAX;
    v.gpu_va_bytes = UINT64_MAX;
    CHECK(!intel_ggtt_pte_offset(&v, 4ULL * 1024 * MB, &reg));
    CHECK(!intel_ggtt_pte_offset(&v, UINT64_MAX - 1, &reg));
    CHECK(reg == saved);
}

static void test_ptes(void)
{
    uint64_t phys = 99;
    CHECK(!intel_ggtt_decode_pte(0, 0, &phys));
    CHECK(!intel_ggtt_decode_pte(0xbc000002, 0, &phys));
    CHECK(phys == 99);
    CHECK(!intel_ggtt_decode_pte(1, 0, NULL));
    CHECK(intel_ggtt_decode_pte(1, 0, &phys));
    CHECK(phys == 0); /* Present page zero is valid in the format. */
    for (unsigned offset = 0; offset < 4096; offset++) {
        CHECK(intel_ggtt_decode_pte(0xbc123001, 0x12345000 + offset, &phys));
        CHECK(phys == 0xbc123000 + offset);
        /* Ignored upper and lower bits do not become address bits. */
        CHECK(intel_ggtt_decode_pte(0xffffffffffabcdefULL,
                                     0xf0000000 + offset, &phys));
        CHECK(phys == 0x7fffabc000ULL + offset);
    }
}

static void test_invalid(void)
{
    intel_ggtt_config_t c = make_config();
    intel_ggtt_layout_t v;
    CHECK(intel_ggtt_decode(NULL, &v) == INTEL_GGTT_BAD_ARGUMENT);
    CHECK(intel_ggtt_decode(&c, NULL) == INTEL_GGTT_BAD_ARGUMENT);
#define BAD(field, value, reason) do { c = make_config(); c.field = (value); \
    expect_error(c, (reason)); } while (0)
    BAD(host_id, 0xffffffff, INTEL_GGTT_BAD_DEVICE);
    BAD(igd_id, 0xffffffff, INTEL_GGTT_BAD_DEVICE);
    BAD(command, 4, INTEL_GGTT_MEMORY_DISABLED);
    BAD(bar0_lo, 0x4000000c, INTEL_GGTT_BAD_BAR);
    BAD(bar0_lo, 0x40000005, INTEL_GGTT_BAD_BAR);
    BAD(bar0_lo, 0x40001004, INTEL_GGTT_BAD_BAR);
    BAD(bar0_hi, 0x80, INTEL_GGTT_BAD_BAR);
    BAD(bar2_lo, 0x04, INTEL_GGTT_BAD_BAR);
    BAD(bar2_lo, 0x0d, INTEL_GGTT_BAD_BAR);
    BAD(bar2_lo, 0x0800000c, INTEL_GGTT_BAD_BAR);
    BAD(bar2_hi, 0x80, INTEL_GGTT_BAD_BAR);
    BAD(bdsm, 0xffffffff, INTEL_GGTT_BAD_STOLEN);
    BAD(bgsm, 0xffffffff, INTEL_GGTT_BAD_STOLEN);
    BAD(tolud, 0xffffffff, INTEL_GGTT_BAD_STOLEN);
    BAD(bdsm, 0xbd000001, INTEL_GGTT_BAD_STOLEN);
    BAD(bgsm, 0xbb800001, INTEL_GGTT_BAD_STOLEN);
    BAD(tolud, 0xb0000001, INTEL_GGTT_BAD_STOLEN);
    BAD(tolud, 0, INTEL_GGTT_BAD_STOLEN);
    BAD(ggc, 0x0289, INTEL_GGTT_BAD_GGC);
    c = make_config(); c.bar0_lo = 4; c.bar0_hi = 0;
    expect_error(c, INTEL_GGTT_BAD_BAR);
    c = make_config(); c.bar2_lo = 12; c.bar2_hi = 0;
    expect_error(c, INTEL_GGTT_BAD_BAR);
    c = make_config(); c.bar2_lo = 0x4000000c; c.bar2_hi = 0x60;
    expect_error(c, INTEL_GGTT_BAD_BAR); /* overlaps MMIO */
    c = make_config(); c.bar0_lo = 0xb0000004; c.bar0_hi = 0;
    expect_error(c, INTEL_GGTT_BAD_STOLEN); /* overlaps low DRAM */
    c = make_config(); c.bar2_lo = 0xb000000c; c.bar2_hi = 0;
    expect_error(c, INTEL_GGTT_BAD_STOLEN);
    c = make_config(); c.bdsm = 0x00200001; c.bgsm = 0xffe00001;
    c.tolud = 0x04200001;
    expect_error(c, INTEL_GGTT_BAD_STOLEN); /* 32-bit wrap is not adjacency */
    c = make_config(); c.bar0_hi = 0x7f; c.bar0_lo = 0xff000004;
    c.bar2_hi = 0x7e; c.bar2_lo = 0xf000000c;
    CHECK(intel_ggtt_decode(&c, &v) == INTEL_GGTT_OK); /* exact last MMIO BAR */
#undef BAD
}

int main(void)
{
    test_layouts();
    test_apertures();
    test_ranges();
    test_ptes();
    test_invalid();
    printf("Intel GGTT: %u checks, 0 failures (synthetic PCI/PTE data)\n", checks);
    return 0;
}
