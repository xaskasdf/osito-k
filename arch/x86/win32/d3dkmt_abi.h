/* Public Windows graphics-kernel ABI, adapter discovery subset.
 *
 * These are OS/driver interface declarations, not a Direct3D implementation.
 * Windows uses native-size pointers here; PE32 calls need explicit marshalling
 * in the 64-bit kernel. KMT handles and LUIDs retain their size in both ABIs.
 * Kept independent of Windows SDK headers for the freestanding PE probes.
 */
#ifndef OSITO_D3DKMT_ABI_H
#define OSITO_D3DKMT_ABI_H

typedef unsigned int OKMT_U32;
typedef int OKMT_S32;
typedef unsigned short OKMT_WCHAR;
typedef unsigned long long OKMT_U64;
typedef OKMT_U32 OKMT_HANDLE;
typedef struct { OKMT_U32 low; OKMT_S32 high; } OKMT_LUID;
typedef struct { OKMT_HANDLE context; OKMT_S32 priority; } OKMT_CONTEXT_PRIORITY;

typedef struct {
    OKMT_HANDLE device;
    const OKMT_HANDLE *resources,*allocations;
    OKMT_U32 count,priority,flags;
} OKMT_OFFER_ALLOCATIONS;
typedef struct {
    OKMT_HANDLE device;
    const OKMT_HANDLE *resources,*allocations;
    OKMT_U32 *discarded;
    OKMT_U32 count;
} OKMT_RECLAIM_ALLOCATIONS;
typedef struct {
    OKMT_HANDLE queue;
    OKMT_U32 count;
    const OKMT_HANDLE *resources,*allocations;
    OKMT_U32 *discarded;
    OKMT_U64 fence __attribute__((aligned(8)));
} OKMT_RECLAIM_ALLOCATIONS2;

typedef struct {
    OKMT_HANDLE adapter;
    OKMT_LUID luid;
    OKMT_U32 sources;
    OKMT_S32 precise_present_regions;
} OKMT_ADAPTER_INFO;

#define OKMT_MAX_ENUM_ADAPTERS 16u
typedef struct {
    OKMT_U32 count;
    OKMT_ADAPTER_INFO adapters[OKMT_MAX_ENUM_ADAPTERS];
} OKMT_ENUM_ADAPTERS;

typedef struct {
    OKMT_U32 count;
    OKMT_ADAPTER_INFO *adapters;
} OKMT_ENUM_ADAPTERS2;

typedef struct {
    OKMT_HANDLE adapter;
    OKMT_U32 type;
    void *data;
    OKMT_U32 bytes;
} OKMT_QUERY_ADAPTER_INFO;

typedef struct {
    OKMT_LUID luid;
    OKMT_HANDLE adapter;
} OKMT_OPEN_ADAPTER_FROM_LUID;

typedef struct { OKMT_HANDLE adapter; } OKMT_CLOSE_ADAPTER;

/* Modern display-mode ABI: flags occupy two DWORDs, not a C bitfield.
 * The fixed 44-byte entry is shared by PE32/PE64; only the list pointer changes. */
typedef struct {
    OKMT_U32 width,height,format,integer_refresh;
    OKMT_U32 refresh_numerator,refresh_denominator;
    OKMT_U32 scanline_order,orientation,fixed_output,flags[2];
} OKMT_DISPLAY_MODE;
typedef struct {
    OKMT_HANDLE adapter;
    OKMT_U32 source;
    OKMT_DISPLAY_MODE *modes;
    OKMT_U32 count;
} OKMT_GET_DISPLAY_MODE_LIST;

/* Fixed 16-byte PE32/PE64 ABI. BOOLEAN is one byte; never overwrite padding. */
typedef struct {
    OKMT_HANDLE adapter;
    OKMT_U32 source;
    unsigned char in_vertical_blank, padding[3];
    OKMT_U32 scanline;
} OKMT_GET_SCANLINE;
enum { OKMT_SYNC_MUTEX=1, OKMT_SYNC_SEMAPHORE=2, OKMT_SYNC_FENCE=3, OKMT_SYNC_MONITORED=5 };
#define OKMT_MAX_SYNC_OBJECTS 32u
#define OKMT_MAX_BROADCAST_CONTEXTS 64u
typedef struct {
    OKMT_U32 type;
    union { OKMT_U32 initial; struct { OKMT_U32 maximum,initial; } semaphore; OKMT_U32 reserved[16]; } data;
} OKMT_SYNC_INFO;
typedef struct {
    OKMT_U32 type,flags;
    union {
        OKMT_U32 initial;
        struct { OKMT_U32 maximum,initial; } semaphore;
        OKMT_U64 value __attribute__((aligned(8)));
        struct {
            OKMT_U64 initial __attribute__((aligned(8)));
            void *cpu_address;
            OKMT_U64 gpu_address __attribute__((aligned(8)));
            OKMT_U32 affinity,padding;
        } monitored;
        OKMT_U64 reserved[8] __attribute__((aligned(8)));
    } data;
    OKMT_HANDLE shared;
} OKMT_SYNC_INFO2;
typedef struct { OKMT_HANDLE device; OKMT_SYNC_INFO info; OKMT_HANDLE object; } OKMT_CREATE_SYNC;
typedef struct { OKMT_HANDLE device; OKMT_SYNC_INFO2 info; OKMT_HANDLE object; } OKMT_CREATE_SYNC2;
typedef struct { OKMT_HANDLE object; } OKMT_DESTROY_SYNC;
typedef struct { OKMT_HANDLE context; OKMT_U32 count; OKMT_HANDLE objects[32]; } OKMT_WAIT_SYNC;
typedef struct {
    OKMT_HANDLE context; OKMT_U32 count; OKMT_HANDLE objects[32];
    OKMT_U64 values[8] __attribute__((aligned(8)));
} OKMT_WAIT_SYNC2;
typedef struct { OKMT_HANDLE context; OKMT_U32 count; OKMT_HANDLE objects[32]; OKMT_U32 flags; } OKMT_SIGNAL_SYNC;
typedef struct {
    OKMT_HANDLE context; OKMT_U32 count; OKMT_HANDLE objects[32]; OKMT_U32 flags,broadcast_count;
    OKMT_HANDLE broadcast[64];
    OKMT_U64 values[8] __attribute__((aligned(8)));
} OKMT_SIGNAL_SYNC2;

typedef struct {
    OKMT_HANDLE device; OKMT_U32 count;
    const OKMT_HANDLE *objects;
    const OKMT_U64 *values;
    OKMT_U32 flags;
} OKMT_SIGNAL_SYNC_CPU;
typedef struct {
    OKMT_HANDLE device; OKMT_U32 count;
    const OKMT_HANDLE *objects;
    const OKMT_U64 *values;
    void *event;
    OKMT_U32 flags;
} OKMT_WAIT_SYNC_CPU;
typedef struct {
    OKMT_HANDLE context; OKMT_U32 count;
    const OKMT_HANDLE *objects;
    union {
        const OKMT_U64 *values;
        OKMT_U64 value __attribute__((aligned(8)));
        OKMT_U64 reserved[8] __attribute__((aligned(8)));
    } data;
} OKMT_SYNC_GPU;
typedef struct {
    OKMT_U32 count;
    const OKMT_HANDLE *objects;
    OKMT_U32 flags,context_count;
    const OKMT_HANDLE *contexts;
    union {
        OKMT_U64 value;
        void *event;
        const OKMT_U64 *values;
        OKMT_U64 reserved[8];
    } data;
} OKMT_SIGNAL_SYNC_GPU2;
typedef struct {
    void *dc;
    OKMT_HANDLE adapter;
    OKMT_LUID luid;
    OKMT_U32 source;
} OKMT_OPEN_ADAPTER_FROM_HDC;
typedef struct {
    OKMT_WCHAR name[32];
    OKMT_HANDLE adapter;
    OKMT_LUID luid;
    OKMT_U32 source;
} OKMT_OPEN_ADAPTER_FROM_GDI_NAME;
typedef struct {
    const OKMT_WCHAR *name;
    OKMT_HANDLE adapter;
    OKMT_LUID luid;
} OKMT_OPEN_ADAPTER_FROM_DEVICE_NAME;
typedef struct {
    union { OKMT_HANDLE adapter; void *kernel_adapter; } object;
    OKMT_U32 flags;
    OKMT_HANDLE device;
    void *command_buffer;
    OKMT_U32 command_bytes;
    void *allocation_list;
    OKMT_U32 allocation_count;
    void *patch_list;
    OKMT_U32 patch_count;
} OKMT_CREATE_DEVICE;
typedef struct { OKMT_HANDLE device; } OKMT_DESTROY_DEVICE;
typedef struct {
    OKMT_HANDLE device;
    OKMT_U32 node,engine,flags;
    void *private_data;
    OKMT_U32 private_bytes,hint;
    OKMT_HANDLE context;
    void *command_buffer;
    OKMT_U32 command_bytes;
    void *allocation_list;
    OKMT_U32 allocation_count;
    void *patch_list;
    OKMT_U32 patch_count;
    OKMT_U64 gpu_commands;
} OKMT_CREATE_CONTEXT;
typedef struct { OKMT_HANDLE context; } OKMT_DESTROY_CONTEXT;

typedef struct {
    OKMT_HANDLE context;
    OKMT_U32 command_offset,command_length,allocation_count,patch_count;
    void *commands;
    OKMT_U32 command_bytes;
    void *allocations;
    OKMT_U32 allocation_capacity;
    void *patches;
    OKMT_U32 patch_capacity,flags;
    OKMT_U64 present_token __attribute__((aligned(8)));
    OKMT_U32 broadcast_count;
    OKMT_HANDLE broadcast[64];
    OKMT_U32 queued;
    OKMT_U64 gpu_commands __attribute__((aligned(8)));
    void *private_data;
    OKMT_U32 private_bytes;
} OKMT_RENDER;
_Static_assert(sizeof(OKMT_RENDER)==(sizeof(void *)==8 ? 368 : 336),"render ABI");
typedef struct { OKMT_HANDLE allocation; OKMT_U32 flags; } OKMT_ALLOCATION_LIST;
typedef struct {
    OKMT_U32 allocation_index,slot,driver_id,allocation_offset,patch_offset,split_offset;
} OKMT_PATCH_LOCATION;
_Static_assert(sizeof(OKMT_ALLOCATION_LIST)==8,"allocation list element ABI");
_Static_assert(sizeof(OKMT_PATCH_LOCATION)==24,"patch list element ABI");
typedef struct {
    OKMT_HANDLE device,allocation;
    OKMT_U32 private_data,page_count;
    const OKMT_U32 *pages;
    void *data;
    OKMT_U32 flags;
    OKMT_U64 gpu_address;
} OKMT_LOCK;
typedef struct {
    OKMT_HANDLE device;
    OKMT_U32 count;
    const OKMT_HANDLE *allocations;
} OKMT_UNLOCK;
typedef struct {
    OKMT_HANDLE device,allocation;
    OKMT_U32 flags;
    void *data;
} OKMT_LOCK2;
typedef struct { OKMT_HANDLE device,allocation; } OKMT_UNLOCK2;

typedef struct {
    OKMT_HANDLE device;
    OKMT_S32 priority;
    OKMT_HANDLE queue,sync;
    void *cpu_address;
    OKMT_U32 physical_index;
} OKMT_CREATE_PAGING_QUEUE;
typedef struct { OKMT_HANDLE queue; } OKMT_DESTROY_PAGING_QUEUE;

typedef struct {
    OKMT_HANDLE queue;
    OKMT_U32 count;
    const OKMT_HANDLE *allocations;
    const OKMT_U32 *priorities;
    OKMT_U32 flags;
    OKMT_U64 fence __attribute__((aligned(8)));
    OKMT_U64 trim;
} OKMT_MAKE_RESIDENT;
typedef struct {
    OKMT_HANDLE device;
    OKMT_U32 count;
    const OKMT_HANDLE *allocations;
    OKMT_U32 flags;
    OKMT_U64 trim __attribute__((aligned(8)));
} OKMT_EVICT;

enum {
    OKMT_LOCK_READ_ONLY=1u, OKMT_LOCK_WRITE_ONLY=2u,
    OKMT_LOCK_DO_NOT_WAIT=4u, OKMT_LOCK_IGNORE_SYNC=8u,
    OKMT_LOCK_ENTIRE=16u, OKMT_LOCK_DO_NOT_EVICT=32u,
    OKMT_LOCK_ACQUIRE_APERTURE=64u, OKMT_LOCK_DISCARD=128u,
    OKMT_LOCK_NO_REFERENCE=256u, OKMT_LOCK_ALTERNATE_VA=512u,
    OKMT_LOCK_IGNORE_READ_SYNC=1024u
};
typedef struct {
    OKMT_HANDLE allocation;
    const void *system_memory;
    void *private_data;
    OKMT_U32 private_bytes,source,flags;
} OKMT_ALLOCATION_INFO;
typedef struct {
    OKMT_HANDLE allocation;
    const void *system_memory;
    void *private_data;
    OKMT_U32 private_bytes,source,flags;
    OKMT_U64 gpu_address;
    __UINTPTR_TYPE__ priority,reserved[5];
} OKMT_ALLOCATION_INFO2;
typedef struct {
    OKMT_HANDLE device,resource,global_share;
    const void *runtime_data;
    OKMT_U32 runtime_bytes;
    const void *private_data;
    OKMT_U32 private_bytes,count;
    void *allocations;
    OKMT_U32 flags;
    void *runtime_resource;
} OKMT_CREATE_ALLOCATION;
typedef struct {
    OKMT_HANDLE device,resource;
    const OKMT_HANDLE *allocations;
    OKMT_U32 count;
} OKMT_DESTROY_ALLOCATION;
typedef struct {
    OKMT_HANDLE device,resource;
    const OKMT_HANDLE *allocations;
    OKMT_U32 count,flags;
} OKMT_DESTROY_ALLOCATION2;
typedef struct { OKMT_U32 version; OKMT_WCHAR name[260]; } OKMT_UMD_FILENAME;
typedef struct {
    OKMT_U32 physical_index;
    OKMT_U32 vendor, device, subvendor, subsystem, revision, bus_type;
} OKMT_QUERY_DEVICE_IDS;

enum {
    OKMT_QUERY_PRIVATE_DRIVER_DATA = 0,
    OKMT_QUERY_UMD_FILENAME = 1,
    OKMT_QUERY_DRIVER_VERSION = 13,
    /* Value 14 is reserved; this enum is not a consecutive list. */
    OKMT_QUERY_ADAPTER_TYPE = 15,
    OKMT_QUERY_DEVICE_IDENTIFIERS = 31,
    OKMT_UMD_DX9 = 0,
    OKMT_ADAPTER_RENDER = 1u << 0,
    OKMT_ADAPTER_DISPLAY = 1u << 1,
    OKMT_ADAPTER_SOFTWARE = 1u << 2,
    OKMT_ADAPTER_PROTOTYPE = 1u << 12
};

/* SetAllocationPriority and QueryAllocationResidency share this layout. */
typedef struct {
    OKMT_HANDLE device,resource;
    const OKMT_HANDLE *allocations;
    OKMT_U32 count;
    OKMT_U32 *values;
} OKMT_ALLOCATION_PROPERTIES;
enum { OKMT_RESIDENT_GPU=1, OKMT_RESIDENT_SHARED=2, OKMT_NOT_RESIDENT=3 };
_Static_assert(sizeof(OKMT_ALLOCATION_PROPERTIES)==(sizeof(void *)==8 ? 32 : 20),
    "allocation properties ABI");
_Static_assert(sizeof(OKMT_HANDLE) == 4, "KMT handle is always 32 bits");
_Static_assert(sizeof(OKMT_LUID) == 8, "LUID ABI");
_Static_assert(sizeof(OKMT_ADAPTER_INFO) == 20, "adapter info ABI");
_Static_assert(sizeof(OKMT_OPEN_ADAPTER_FROM_LUID) == 12, "open adapter ABI");
_Static_assert(sizeof(OKMT_CLOSE_ADAPTER) == 4, "close adapter ABI");
_Static_assert(sizeof(OKMT_CREATE_DEVICE) == (sizeof(void *)==8 ? 64 : 36), "create device ABI");
_Static_assert(__builtin_offsetof(OKMT_CREATE_DEVICE,device)==sizeof(void *)+4, "device output ABI");
_Static_assert(sizeof(OKMT_DESTROY_DEVICE)==4, "destroy device ABI");
_Static_assert(sizeof(OKMT_LOCK)==(sizeof(void *)==8 ? 48 : 40), "lock ABI");
_Static_assert(__builtin_offsetof(OKMT_LOCK,data)==16+sizeof(void *), "lock output ABI");
_Static_assert(sizeof(OKMT_UNLOCK)==(sizeof(void *)==8 ? 16 : 12), "unlock ABI");
_Static_assert(sizeof(OKMT_ALLOCATION_INFO)==(sizeof(void *)==8 ? 40 : 24), "allocation info ABI");
_Static_assert(sizeof(OKMT_ALLOCATION_INFO2)==(sizeof(void *)==8 ? 96 : 56), "allocation info2 ABI");
_Static_assert(sizeof(OKMT_CREATE_ALLOCATION)==(sizeof(void *)==8 ? 72 : 44), "create allocation ABI");
_Static_assert(sizeof(OKMT_DESTROY_ALLOCATION)==(sizeof(void *)==8 ? 24 : 16), "destroy allocation ABI");
_Static_assert(sizeof(OKMT_DESTROY_ALLOCATION2)==(sizeof(void *)==8 ? 24 : 20), "destroy allocation2 ABI");
_Static_assert(sizeof(OKMT_UMD_FILENAME) == 524, "UMD filename ABI");
_Static_assert(sizeof(OKMT_QUERY_DEVICE_IDS) == 28, "device IDs ABI");
_Static_assert(sizeof(OKMT_ENUM_ADAPTERS2) == (sizeof(void *) == 8 ? 16 : 8),
               "enum adapters native pointer ABI");
_Static_assert(sizeof(OKMT_QUERY_ADAPTER_INFO) == (sizeof(void *) == 8 ? 24 : 16),
               "query adapter native pointer ABI");
_Static_assert(__builtin_offsetof(OKMT_ENUM_ADAPTERS2, adapters) == sizeof(void *),
               "enum adapters pointer offset");
_Static_assert(__builtin_offsetof(OKMT_QUERY_ADAPTER_INFO, data) == 8,
               "query adapter pointer offset");
_Static_assert(__builtin_offsetof(OKMT_QUERY_ADAPTER_INFO, bytes) == 8 + sizeof(void *),
               "query adapter size offset");

/* Execution member of GETDEVICESTATE. Other union members have independent
 * contracts and are not captured or overwritten by this query. */
typedef struct { OKMT_HANDLE device; OKMT_U32 type,state; } OKMT_DEVICE_EXECUTION;
enum { OKMT_DEVICE_STATE_EXECUTION=1, OKMT_EXECUTION_ACTIVE=1,
    OKMT_EXECUTION_RESET=2, OKMT_EXECUTION_HUNG=3, OKMT_EXECUTION_STOPPED=4,
    OKMT_EXECUTION_OUT_OF_MEMORY=5, OKMT_EXECUTION_DMA_FAULT=6,
    OKMT_EXECUTION_DMA_PAGE_FAULT=7 };
_Static_assert(sizeof(OKMT_DEVICE_EXECUTION)==12,"device execution prefix ABI");
typedef struct { OKMT_HANDLE device; OKMT_U32 reason; } OKMT_MARK_DEVICE_ERROR;
#define OKMT_DEVICE_ERROR_GENERIC 0x80000000u
#define OKMT_DEVICE_ERROR_DRIVER 0x80000006u
_Static_assert(sizeof(OKMT_MARK_DEVICE_ERROR)==8,"device error ABI");

#endif
