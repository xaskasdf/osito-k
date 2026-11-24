/* D3D9 declaration types -> Gen9 native vertex fetch. No CPU conversion. */
#ifndef OSITOK_INTEL_GEN9_VERTEX_H
#define OSITOK_INTEL_GEN9_VERTEX_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint32_t bytes,format,control; } intel_gen9_vertex_format_t;
static inline bool intel_gen9_vertex_format(uint32_t type,intel_gen9_vertex_format_t *out)
{
    /* SURFACE_FORMAT encodings and VF support: Intel PRM / Mesa ISL table.
     * Component controls: source=1, zero=2, float-one=3. D3DCOLOR fetch
     * swizzles BGRA storage to RGBA; packed DEC3 ignores the upper two bits. */
    static const uint16_t format[17]={216,133,64,0,192,245,246,147,199,205,129,204,128,243,197,208,132};
    static const uint8_t bytes[17]={4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
    static const uint8_t components[17]={1,2,3,4,4,4,2,4,4,2,4,2,4,3,3,2,4};
    if (type>=17 || !out) return false;
    uint32_t control=0;
    for (unsigned i=0;i<4;i++) control|=(i<components[type] ? 1u : i==3 ? 3u : 2u)<<(28-i*4);
    *out=(intel_gen9_vertex_format_t){bytes[type],format[type],control};
    return true;
}
#endif
