#ifndef OSITO_SURFACE_ALPHA_H
#define OSITO_SURFACE_ALPHA_H

/* Native 0xAARRGGBB surfaces; per-pixel input is premultiplied. */
typedef struct {
    unsigned int color_key;
    unsigned char opacity;
    unsigned char per_pixel;
    unsigned char color_key_enabled;
    unsigned char input_transparent;
} surface_alpha_t;

static inline unsigned surface_alpha_coverage(unsigned pixel,
                                               const surface_alpha_t *alpha)
{
    if (alpha->color_key_enabled &&
        (pixel & 0xffffffu) == alpha->color_key)
        return 0;
    unsigned coverage = alpha->per_pixel ? pixel >> 24 : 255;
    return (coverage * alpha->opacity + 127) / 255;
}

static inline unsigned surface_alpha_blend(unsigned dst, unsigned src,
                                            const surface_alpha_t *alpha)
{
    unsigned coverage = surface_alpha_coverage(src, alpha);
    if (!coverage) return dst;
    unsigned result = 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned source = (((src >> shift) & 255) * alpha->opacity + 127) / 255;
        unsigned target = (((dst >> shift) & 255) * (255 - coverage) + 127) / 255;
        unsigned channel = source + target;
        result |= (channel > 255 ? 255 : channel) << shift;
    }
    return result;
}

#endif
