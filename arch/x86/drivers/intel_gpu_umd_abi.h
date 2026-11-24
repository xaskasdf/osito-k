/* Osito-K Intel UMD/KMD private buffer description, not a Direct3D ABI.
 * The original Microsoft runtime passes this opaque data from our native
 * UMD. No addresses or native cookies cross this boundary. Version 1 creates
 * owned linear buffers only; surfaces/imported/shared layouts need explicit
 * future contracts rather than interpreting arbitrary driver-private bytes. */
#ifndef OSITOK_INTEL_GPU_UMD_ABI_H
#define OSITOK_INTEL_GPU_UMD_ABI_H
#define INTEL_UMD_ADAPTER_MAGIC 0x414b4749u
#define INTEL_UMD_ADAPTER_VERSION 1u
#define INTEL_UMD_CAP_COPY32 1u
#define INTEL_UMD_CAP_RCS_COPY32 2u
typedef struct {
    unsigned int magic,version,bytes,capabilities;
    unsigned int vendor,device,copy_node,reserved;
} intel_umd_adapter_description_t;
_Static_assert(sizeof(intel_umd_adapter_description_t)==32,"native adapter description ABI");
#define INTEL_UMD_BUFFER_MAGIC 0x4d4b4749u
#define INTEL_UMD_BUFFER_VERSION 1u
typedef struct {
    unsigned int magic,version;
    unsigned long long bytes;
    unsigned int flags,reserved;
} intel_umd_buffer_description_t;
_Static_assert(sizeof(intel_umd_buffer_description_t)==24,"native buffer description ABI");
#define INTEL_UMD_CONTEXT_MAGIC 0x58434749u
#define INTEL_UMD_CONTEXT_VERSION 1u
#define INTEL_UMD_NODE_RENDER 0u
#define INTEL_UMD_NODE_COPY 1u
#define INTEL_UMD_CONTEXT_MAX_COMMAND_BYTES 65536u
#define INTEL_UMD_CONTEXT_MAX_ALLOCATIONS 64u
#define INTEL_UMD_CONTEXT_MAX_PATCHES 64u
typedef struct {
    unsigned int magic,version,command_bytes,allocation_count,patch_count,reserved;
} intel_umd_context_description_t;
_Static_assert(sizeof(intel_umd_context_description_t)==24,"native context description ABI");
/* Versioned native UMD command, not raw Intel instructions. Exactly one linear
 * 32bpp copy, origin zero. Two standard allocation/patch-list entries identify
 * distinct source (read) and destination (write) resources. Patches name the
 * zero placeholders relative to the staging buffer, use slot 0/1 respectively,
 * and have driver/allocation/split offsets zero. Kernel assigns GPU addresses.
 * Other layouts, offsets, operations and batching require explicit versions. */
#define INTEL_UMD_COMMAND_MAGIC 0x50434749u
#define INTEL_UMD_COMMAND_VERSION 1u
/* COPY32 version 2 gives reserved[0..3] pixel coordinates sx,sy,dx,dy.
 * Width/height describe the copied rectangle. Addresses and patch offsets
 * remain zero-only; distinct entire allocations stay owned until retirement.
 * Version 1 retains its zero-origin, zero-reserved contract. */
#define INTEL_UMD_COPY_RECT_VERSION 2u
#define INTEL_UMD_COPY32 1u
#define INTEL_UMD_DRAW32 2u
typedef struct {
    unsigned int magic,version,opcode,bytes;
    unsigned int width,height,source_pitch,destination_pitch;
    unsigned long long source,destination;
    unsigned int reserved[4];
} intel_umd_copy32_t;
_Static_assert(sizeof(intel_umd_copy32_t)==64,"native copy command ABI");
_Static_assert(__builtin_offsetof(intel_umd_copy32_t,source)==32,"source patch offset");
_Static_assert(__builtin_offsetof(intel_umd_copy32_t,destination)==40,"destination patch offset");
/* Screen-space float4 triangle list/strip/fan, BGRA8 target and scissor.
 * Three distinct allocations: vertices/read, target/write, program/read.
 * Exactly three patches, slots 0/1/2 at byte offsets 80/88/96. All placeholders
 * and other patch fields are zero. Native instructions are validated under
 * exclusive ownership; only the kernel constructs state and GPU addresses. */
typedef struct {
    unsigned int magic,version,opcode,bytes;
    unsigned int destination_width,destination_height,destination_pitch,reserved0;
    unsigned int x,y,width,height;
    unsigned int vertex_offset,vertex_stride,vertex_count,topology;
    unsigned int program_bytes,dispatch_mask,reserved1[2];
    unsigned long long vertices,destination,program;
    unsigned int reserved2[6];
} intel_umd_draw32_t;
_Static_assert(sizeof(intel_umd_draw32_t)==128,"native draw command ABI");
_Static_assert(__builtin_offsetof(intel_umd_draw32_t,vertices)==80,"vertex patch offset");
_Static_assert(__builtin_offsetof(intel_umd_draw32_t,destination)==88,"draw destination patch offset");
_Static_assert(__builtin_offsetof(intel_umd_draw32_t,program)==96,"program patch offset");
/* Version 2 snapshots packed float constants immediately after the unchanged
 * draw header. Only 136 + parameter_count*4 bytes are transmitted. */
#define INTEL_UMD_DRAW_CONSTANTS_VERSION 2u
#define INTEL_UMD_MAX_PARAMETERS 512u
typedef struct {
    intel_umd_draw32_t draw;
    unsigned int parameter_count,reserved;
    unsigned int values[INTEL_UMD_MAX_PARAMETERS];
} intel_umd_draw_constants_t;
_Static_assert(__builtin_offsetof(intel_umd_draw_constants_t,values)==136,"constant payload offset");
/* Version 3 repeats one primitive with an owned list of scissors in ONE job.
 * `reserved` becomes rectangle_count (1..32); rectangles follow the USED
 * constants, not the fixed-capacity values array. No partial list admission.
 * The explicit limit fits the existing 8 KiB command/state allocation. */
#define INTEL_UMD_DRAW_RECTANGLES_VERSION 3u
#define INTEL_UMD_MAX_RECTANGLES 32u
typedef struct { unsigned int x,y,width,height; } intel_umd_rectangle_t;
typedef struct {
    intel_umd_draw_constants_t constants;
    intel_umd_rectangle_t storage[INTEL_UMD_MAX_RECTANGLES];
} intel_umd_draw_rectangles_t;
_Static_assert(sizeof(intel_umd_rectangle_t)==16,"native rectangle ABI");
/* Version 4 carries single-target blending in reserved2[0..3]. Its 136-byte
 * prefix always includes parameter_count and rectangle_count (both may be
 * zero), followed by used constants and rectangles. Factors/operations use
 * D3D9 enum values, never native register encodings. Other reserved words and
 * all address placeholders retain their original zero-only contract. */
#define INTEL_UMD_DRAW_BLEND_VERSION 4u
#define INTEL_UMD_BLEND_VALID 0x80000000u
#define INTEL_UMD_BLEND_ENABLE 1u
#define INTEL_UMD_BLEND_XRGB 2u
#define INTEL_UMD_BLEND_WRITE_ALL 0xf0u
typedef struct { unsigned int flags,rgb,alpha,factor; } intel_umd_blend_t;
_Static_assert(sizeof(intel_umd_blend_t)==16,"native blend snapshot ABI");
/* DRAW32 version 5 snapshots sampling state and TEXCOORD float4 offsets.
 * The first 136 bytes keep the draw/count prefix. Fixed texture slots follow;
 * used constants and rectangles start at values (not after its capacity).
 * The list contains UNIQUE allocations: three geometry/code roles plus up to
 * 16 images. Patches use roles 0..2 at the old offsets and 3+s at texture[s].
 * Several active sampler patches may name the same image allocation. All
 * address placeholders remain zero; no texture may alias geometry or code.
 * Inactive slots, unused texcoord offsets and reserved fields must be zero.
 * Layouts are reconstructed from dimensions/format/pitch, never supplied as
 * native surface state. Sampler enums/Q8 fields match the owned Gen9 subset. */
#define INTEL_UMD_DRAW_SAMPLED_VERSION 5u
#define INTEL_UMD_DRAW_VERTEX_VERSION 6u
#define INTEL_UMD_MAX_TEXTURES 16u
#define INTEL_UMD_MAX_RESOURCES (5u+INTEL_UMD_MAX_TEXTURES)
typedef struct {
    unsigned int min_filter,mag_filter,mip_filter;
    unsigned int address_u,address_v,address_w;
    int lod_bias;
    unsigned int min_lod,max_lod,border_argb;
} intel_umd_sampler_t;
typedef struct {
    unsigned long long image;
    unsigned int width,height,levels,format,pitch,first,count,srgb;
    intel_umd_sampler_t sampler;
} intel_umd_texture_t;
typedef struct {
    intel_umd_draw32_t draw;
    unsigned int parameter_count,rectangle_count,bound_mask;
    union { unsigned int reserved; unsigned int position_offset; };
    /* V5: byte offsets relative to POSITIONT, with implicit float4 fetch.
     * V6: low 16 bits = byte offset in the bound vertex stream, high 16 bits
     * = D3DDECLTYPE (0..16). POSITIONT's stream offset is position_offset.
     * Unused entries must be zero in both versions. Native GPU encodings
     * never cross this interface. The immutable packet footprint is unchanged. */
    unsigned int texcoord_offset[8];
    intel_umd_texture_t textures[INTEL_UMD_MAX_TEXTURES];
    unsigned int values[INTEL_UMD_MAX_PARAMETERS];
    intel_umd_rectangle_t storage[INTEL_UMD_MAX_RECTANGLES];
} intel_umd_draw_sampled_t;
_Static_assert(sizeof(intel_umd_sampler_t)==40,"sampler snapshot ABI");
_Static_assert(sizeof(intel_umd_texture_t)==80,"texture snapshot ABI");
_Static_assert(__builtin_offsetof(intel_umd_draw_sampled_t,textures)==176,"texture patch offset");
_Static_assert(__builtin_offsetof(intel_umd_draw_sampled_t,values)==1456,"sampled payload offset");
_Static_assert(sizeof(intel_umd_draw_sampled_t)==4016,"sampled capacity ABI");
/* V7 adds an independent native vertex shader and D3D input declaration.
 * Roles 0..18 preserve V5/V6; role 19 patches the zero-only VS placeholder.
 * Unique leases are geometry/target/PS/VS, then images. Packed input elements
 * use byte offset in low 16 bits and D3DDECLTYPE in high 16 bits, indexed by
 * shader vN. Legacy POSITIONT/texcoord fields must be zero. The payload is
 * used PS constants, used VS constants, then rectangles, without padding. */
#define INTEL_UMD_DRAW_VERTEX_SHADER_VERSION 7u
#define INTEL_UMD_VERTEX_PROGRAM_ROLE 19u
typedef struct {
    intel_umd_draw32_t draw;
    unsigned int parameter_count,rectangle_count,bound_mask,reserved;
    unsigned int texcoord_offset[8];
    intel_umd_texture_t textures[INTEL_UMD_MAX_TEXTURES];
    unsigned long long vertex_program;
    unsigned int vertex_program_bytes,vertex_parameter_count;
    unsigned int viewport_x,viewport_y,viewport_width,viewport_height;
    unsigned int attributes[16];
    unsigned int values[2*INTEL_UMD_MAX_PARAMETERS];
    intel_umd_rectangle_t storage[INTEL_UMD_MAX_RECTANGLES];
} intel_umd_draw_vertex_shader_t;
_Static_assert(__builtin_offsetof(intel_umd_draw_vertex_shader_t,vertex_program)==1456,"VS patch offset");
_Static_assert(__builtin_offsetof(intel_umd_draw_vertex_shader_t,values)==1552,"VS payload offset");
_Static_assert(sizeof(intel_umd_draw_vertex_shader_t)==6160,"VS packet capacity");
/* V8 preserves the V7 fixed prefix, then adds independent indices at role 20.
 * vertex_program_bytes==0 selects typed POSITIONT (the V6 declaration fields);
 * all V7-only VS fields must then be zero. Nonzero selects V7 vertex semantics.
 * Unique leases: vertices, target, PS, optional VS, indices, then images.
 * vertex_count bounds accessible vertex entries; index_count is the count of
 * original R16/R32 entries. The GPU applies signed base_vertex to each entry.
 * Parameter arrays and rectangles follow the USED prefix, without padding. */
#define INTEL_UMD_DRAW_INDEXED_VERSION 8u
#define INTEL_UMD_INDEX_ROLE 20u
typedef struct {
    intel_umd_draw32_t draw;
    unsigned int parameter_count,rectangle_count,bound_mask,position_offset;
    unsigned int texcoord_offset[8];
    intel_umd_texture_t textures[INTEL_UMD_MAX_TEXTURES];
    unsigned long long vertex_program;
    unsigned int vertex_program_bytes,vertex_parameter_count;
    unsigned int viewport_x,viewport_y,viewport_width,viewport_height;
    unsigned int attributes[16];
    unsigned long long indices;
    unsigned int index_offset,index_count,index_stride;
    int base_vertex;
    unsigned int min_index,num_vertices;
    unsigned int values[2*INTEL_UMD_MAX_PARAMETERS];
    intel_umd_rectangle_t storage[INTEL_UMD_MAX_RECTANGLES];
} intel_umd_draw_indexed_t;
_Static_assert(__builtin_offsetof(intel_umd_draw_indexed_t,vertex_program)==1456,"indexed VS prefix");
_Static_assert(__builtin_offsetof(intel_umd_draw_indexed_t,indices)==1552,"index patch offset");
_Static_assert(__builtin_offsetof(intel_umd_draw_indexed_t,values)==1584,"indexed payload offset");
_Static_assert(sizeof(intel_umd_draw_indexed_t)==6192,"indexed packet capacity");
#endif
