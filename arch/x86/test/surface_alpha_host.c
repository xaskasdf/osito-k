#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/surface_alpha.h"

static unsigned checks;

static void expect(uint32_t actual, uint32_t wanted, const char *label)
{
    checks++;
    if (actual != wanted) {
        fprintf(stderr, "%s: got %08x, expected %08x\n", label, actual, wanted);
        exit(1);
    }
}

static unsigned scale(unsigned value, unsigned alpha)
{
    return (unsigned)((double)value * alpha / 255.0 + 0.5);
}

static uint32_t reference(uint32_t dst, uint32_t src,
                          const surface_alpha_t *alpha)
{
    if (alpha->color_key_enabled && (src & 0xffffffu) == alpha->color_key)
        return dst;
    unsigned coverage = scale(alpha->per_pixel ? src >> 24 : 255, alpha->opacity);
    if (!coverage) return dst;
    uint32_t result = 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned value = scale((src >> shift) & 255, alpha->opacity) +
                         scale((dst >> shift) & 255, 255 - coverage);
        if (value > 255) value = 255;
        result |= value << shift;
    }
    return result;
}

int main(void)
{
    surface_alpha_t config = {.opacity = 255};
    expect(surface_alpha_blend(0xff102030, 0x00040506, &config),
           0xff040506, "opaque ignores stored alpha");
    config.opacity = 0;
    expect(surface_alpha_blend(0x12345678, 0xffabcdef, &config),
           0x12345678, "zero opacity preserves all destination bits");
    config = (surface_alpha_t){.opacity = 255, .per_pixel = 1};
    expect(surface_alpha_blend(0xff204060, 0x80008000, &config),
           0xff10a030, "premultiplied green over background");
    expect(surface_alpha_blend(0xff010203, 0x00ffffff, &config),
           0xff010203, "zero coverage cannot expose invalid RGB");
    expect(surface_alpha_blend(0xffffffff, 0x01ffffff, &config),
           0xffffffff, "invalid premultiplication saturates without wrap");

    const uint32_t targets[] = {0, 0xffffffffu, 0xff204060u, 0xff0102feu};
    for (unsigned global = 0; global < 256; global++) {
        for (unsigned pixel = 0; pixel < 256; pixel++) {
            config.opacity = (unsigned char)global;
            uint32_t src = (pixel << 24) | (pixel << 16) |
                           ((pixel / 2) << 8) | (pixel / 3);
            expect(surface_alpha_coverage(src, &config), scale(pixel, global),
                   "all combined alpha pairs");
            for (unsigned n = 0; n < sizeof(targets) / sizeof(targets[0]); n++)
                expect(surface_alpha_blend(targets[n], src, &config),
                       reference(targets[n], src, &config), "per-pixel blend matrix");
            config.color_key = src & 0xffffffu;
            config.color_key_enabled = 1;
            expect(surface_alpha_coverage(src, &config), 0, "key overrides alpha");
            expect(surface_alpha_blend(0x87654321, src, &config), 0x87654321,
                   "key preserves destination");
            config.color_key_enabled = 0;
            config.per_pixel = 0;
            expect(surface_alpha_coverage(src, &config), global,
                   "constant alpha ignores pixel alpha");
            expect(surface_alpha_blend(0xff204060, src, &config),
                   reference(0xff204060, src, &config), "constant blend matrix");
            config.per_pixel = 1;
        }
    }
    config = (surface_alpha_t){.opacity = 255, .input_transparent = 1};
    expect(surface_alpha_blend(0xff010203, 0xffa0b0c0, &config),
           0xffa0b0c0, "input transparency does not alter presentation");
    config.color_key_enabled = 1;
    config.color_key = 0x010203;
    expect(surface_alpha_blend(0, 0xff010204, &config), 0xff010204,
           "color key is exact, not approximate");
    printf("surface-alpha: %u checks passed\n", checks);
    return 0;
}
