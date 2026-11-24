/* OsitoK W4.3: minimal stub of drm-uapi/drm_fourcc.h.
 *
 * Mesa's zink driver references DRM_FORMAT_* constants and the
 * fourcc_mod_code() macro for VK_EXT_image_drm_format_modifier
 * support.  We don't have a DRM-KMS stack and don't intend to
 * negotiate per-format modifier lists with the kernel — the
 * compositor accepts the linear / invalid sentinels and that's it.
 *
 * No transitive #include of <linux/types.h> — we use C standard ints. */
#ifndef OSITOK_DRM_FOURCC_H
#define OSITOK_DRM_FOURCC_H 1

#include <stdint.h>

#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                                 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Common formats zink expects to know about — kept minimal. */
#define DRM_FORMAT_R8         fourcc_code('R', '8', ' ', ' ')
#define DRM_FORMAT_R16        fourcc_code('R', '1', '6', ' ')
#define DRM_FORMAT_RG88       fourcc_code('R', 'G', '8', '8')
#define DRM_FORMAT_GR88       fourcc_code('G', 'R', '8', '8')
#define DRM_FORMAT_RGB565     fourcc_code('R', 'G', '1', '6')
#define DRM_FORMAT_BGR565     fourcc_code('B', 'G', '1', '6')
#define DRM_FORMAT_RGBA8888   fourcc_code('R', 'A', '2', '4')
#define DRM_FORMAT_BGRA8888   fourcc_code('B', 'A', '2', '4')
#define DRM_FORMAT_ARGB8888   fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888   fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_XRGB8888   fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_XBGR8888   fourcc_code('X', 'B', '2', '4')

/* Modifier sentinels — the only values our stack ever uses. */
#define fourcc_mod_code(vendor, val) ((((uint64_t)(vendor)) << 56) | ((val) & 0x00ffffffffffffffULL))
#define DRM_FORMAT_MOD_VENDOR_NONE 0
#define DRM_FORMAT_MOD_NONE        0
#define DRM_FORMAT_MOD_LINEAR      0
#define DRM_FORMAT_MOD_INVALID     ((((uint64_t)0xff) << 56) | ((1ULL << 56) - 1))

#endif /* OSITOK_DRM_FOURCC_H */
