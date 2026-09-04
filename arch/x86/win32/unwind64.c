#include "unwind64.h"
#include "compat32.h"
#include "pe.h"

extern DWORD win32_current_process_id(void);
extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);

#define UNWIND64_DYNAMIC_TABLES 128
#define UNWIND64_TABLE_FREE     0U
#define UNWIND64_TABLE_RESERVED 1U
#define UNWIND64_TABLE_ACTIVE   2U
#define UNWIND64_MAX_CHAIN_DEPTH 16U
#define UNWIND64_MAX_EPILOG_BYTES 64U
#define UNWIND64_MAX_DISPATCH_FRAMES 8192U
#define UNWIND64_DISPATCH_STATE_MAGIC 0x554E573634445350ULL

extern TEB *win64_current_teb(void);
extern BYTE win32_unwind64_wrapper_region_start[];
extern BYTE win32_unwind64_wrapper_region_end[];
extern BYTE win32_unwind64_call_language_handler_end[];
extern BYTE win32_unwind64_call_exception_handler_end[];
extern BYTE win32_unwind64_call_exception_filter_end[];
extern BYTE win32_unwind64_call_termination_handler_end[];
extern BYTE win32_unwind64_call_consolidate[];
extern BYTE win32_unwind64_call_consolidate_end[];
extern BYTE win32_unwind64_collision_handler_thunk[];
extern BYTE win32_unwind64_collision_handler_thunk_end[];
extern BYTE win32_unwind64_exception_bridge_thunk[];
extern BYTE win32_unwind64_exception_bridge_thunk_end[];
extern RUNTIME_FUNCTION win32_unwind64_language_runtime_function;
extern RUNTIME_FUNCTION win32_unwind64_exception_runtime_function;
extern RUNTIME_FUNCTION win32_unwind64_filter_runtime_function;
extern RUNTIME_FUNCTION win32_unwind64_termination_runtime_function;
extern RUNTIME_FUNCTION win32_unwind64_consolidate_runtime_function;
extern EXCEPTION_DISPOSITION NTAPI win32_unwind64_call_language_handler(
    PEXCEPTION_RECORD record, PVOID establisher_frame, PCONTEXT context,
    PDISPATCHER_CONTEXT dispatch, PEXCEPTION_ROUTINE handler);
extern EXCEPTION_DISPOSITION NTAPI win32_unwind64_call_exception_handler(
    PEXCEPTION_RECORD record, PVOID establisher_frame, PCONTEXT context,
    PDISPATCHER_CONTEXT dispatch, PEXCEPTION_ROUTINE handler,
    PCONTEXT frame_context);
extern LONG NTAPI win32_unwind64_call_exception_filter(
    PVOID handler, PEXCEPTION_POINTERS pointers, PVOID establisher_frame,
    PDISPATCHER_CONTEXT dispatch);
extern void NTAPI win32_unwind64_call_termination_handler(
    PVOID handler, BOOL abnormal, PVOID establisher_frame,
    PDISPATCHER_CONTEXT dispatch);

typedef struct {
    volatile DWORD state;
    DWORD owner_pid;
    PRUNTIME_FUNCTION table;
    DWORD count;
    ULONGLONG base;
    ULONGLONG range_start;
    ULONGLONG range_end;
} UNWIND64_DYNAMIC_TABLE;

typedef struct {
    DISPATCHER_CONTEXT Dispatch;
    ULONGLONG Magic;
    PCONTEXT FrameContext;
    PCONTEXT NextContext;
} UNWIND64_DISPATCH_STATE;

_Static_assert(__builtin_offsetof(UNWIND64_DISPATCH_STATE, Dispatch) == 0,
               "private dispatcher prefix changed");

static UNWIND64_DYNAMIC_TABLE dynamic_tables[UNWIND64_DYNAMIC_TABLES];

static NTSTATUS unwind64_invalid_target(const char *stage, ULONGLONG current,
                                        ULONGLONG frame,
                                        ULONGLONG target_frame)
{
    serial_puts("[UNWIND64] invalid target ");
    serial_puts(stage);
    serial_puts(" current=0x");
    serial_puthex(current, 16);
    serial_puts(" frame=0x");
    serial_puthex(frame, 16);
    serial_puts(" target=0x");
    serial_puthex(target_frame, 16);
    serial_puts("\n");
    return STATUS_INVALID_UNWIND_TARGET;
}

static DWORD unwind64_owner(void)
{
    DWORD owner = win32_current_process_id();
    return owner ? owner : 1;
}

static void unwind64_copy(void *destination, const void *source, SIZE_T size)
{
    BYTE *out = (BYTE *)destination;
    const BYTE *in = (const BYTE *)source;
    for (SIZE_T i = 0; i < size; i++) out[i] = in[i];
}

static BOOL unwind64_internal_range(ULONGLONG address, SIZE_T size)
{
    ULONGLONG start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_wrapper_region_start;
    ULONGLONG end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_wrapper_region_end;
    return size && address >= start && address < end &&
           (ULONGLONG)size <= end - address;
}

static BOOL unwind64_range_readable(ULONGLONG address, SIZE_T size)
{
    return unwind64_internal_range(address, size) ||
           win32_user_range_readable(
               (const void *)(ULONG_PTR)address, size, FALSE);
}

static BOOL unwind64_handler_executable(PVOID handler)
{
    ULONGLONG address = (ULONGLONG)(ULONG_PTR)handler;
    ULONGLONG start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_collision_handler_thunk;
    ULONGLONG end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_collision_handler_thunk_end;
    ULONGLONG bridge_start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_exception_bridge_thunk;
    ULONGLONG bridge_end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_exception_bridge_thunk_end;
    return (address >= start && address < end) ||
           (address >= bridge_start && address < bridge_end) ||
           win32_user_range_executable(handler, 1, FALSE);
}

static BOOL unwind64_add(ULONGLONG left, ULONGLONG right,
                         ULONGLONG *result)
{
    if (right > UINT64_MAX - left) return FALSE;
    if (result) *result = left + right;
    return TRUE;
}

static BOOL unwind64_add_signed(ULONGLONG value, LONGLONG displacement,
                                ULONGLONG *result)
{
    if (displacement >= 0)
        return unwind64_add(value, (ULONGLONG)displacement, result);

    ULONGLONG magnitude = (ULONGLONG)(-(displacement + 1)) + 1U;
    if (value < magnitude) return FALSE;
    if (result) *result = value - magnitude;
    return TRUE;
}

static BOOL unwind64_advance_rsp(PCONTEXT context, ULONGLONG amount)
{
    ULONGLONG next;
    if (!context || !unwind64_add(context->Rsp, amount, &next))
        return FALSE;
    context->Rsp = next;
    return TRUE;
}

static BOOL unwind64_integer_is_nonvolatile(BYTE reg)
{
    return reg == 3 || (reg >= 5 && reg <= 7) || reg >= 12;
}

static BOOL unwind64_xmm_is_nonvolatile(BYTE reg)
{
    return reg >= 6 && reg < 16;
}

static BOOL unwind64_range_inside(ULONGLONG base, ULONGLONG size,
                                  ULONGLONG address, ULONGLONG length)
{
    ULONGLONG end;
    ULONGLONG range_end;
    return length && unwind64_add(address, length, &end) &&
           unwind64_add(base, size, &range_end) &&
           address >= base && end <= range_end;
}

static BOOL unwind64_read(ULONGLONG address, void *value, SIZE_T size)
{
    if (!value || !unwind64_range_readable(address, size))
        return FALSE;
    unwind64_copy(value, (const void *)(ULONG_PTR)address, size);
    return TRUE;
}

static BOOL unwind64_dispatch_contexts(PDISPATCHER_CONTEXT dispatch,
                                       PCONTEXT *frame_context,
                                       PCONTEXT *next_context)
{
    UNWIND64_DISPATCH_STATE state;
    if (!dispatch ||
        !unwind64_read((ULONGLONG)(ULONG_PTR)dispatch, &state,
                       sizeof(state)) ||
        state.Magic != UNWIND64_DISPATCH_STATE_MAGIC ||
        !state.FrameContext || !state.NextContext)
        return FALSE;
    if (frame_context) *frame_context = state.FrameContext;
    if (next_context) *next_context = state.NextContext;
    return TRUE;
}

static ULONGLONG unwind64_context_control_pc(PCONTEXT context)
{
    if (!context || !context->Rip) return 0;
    return (context->ContextFlags & CONTEXT_UNWOUND_TO_CALL)
               ? context->Rip - 1U : context->Rip;
}

static BOOL unwind64_read_u8(ULONGLONG address, BYTE *value)
{
    return unwind64_read(address, value, sizeof(*value));
}

static BOOL unwind64_read_u16(ULONGLONG address, USHORT *value)
{
    return unwind64_read(address, value, sizeof(*value));
}

static BOOL unwind64_read_u32(ULONGLONG address, DWORD *value)
{
    return unwind64_read(address, value, sizeof(*value));
}

static BOOL unwind64_read_u64(ULONGLONG address, ULONGLONG *value)
{
    return unwind64_read(address, value, sizeof(*value));
}

static ULONGLONG *unwind64_integer_register(PCONTEXT context, BYTE reg)
{
    if (!context || reg >= 16) return NULL;
    return (&context->Rax) + reg;
}

static PM128A unwind64_xmm_register(PCONTEXT context, BYTE reg)
{
    if (!context || reg >= 16) return NULL;
    return (PM128A)((BYTE *)context + 0x1A0) + reg;
}

static BOOL unwind64_restore_integer(
    PCONTEXT context, PKNONVOLATILE_CONTEXT_POINTERS pointers,
    BYTE reg, ULONGLONG address)
{
    ULONGLONG value;
    ULONGLONG *destination = unwind64_integer_register(context, reg);
    if (!unwind64_integer_is_nonvolatile(reg) || !destination ||
        !unwind64_read_u64(address, &value))
        return FALSE;
    *destination = value;
    if (pointers) pointers->IntegerContext[reg] =
        (ULONGLONG *)(ULONG_PTR)address;
    return TRUE;
}

static BOOL unwind64_restore_xmm(
    PCONTEXT context, PKNONVOLATILE_CONTEXT_POINTERS pointers,
    BYTE reg, ULONGLONG address)
{
    M128A value;
    PM128A destination = unwind64_xmm_register(context, reg);
    if (!unwind64_xmm_is_nonvolatile(reg) || !destination ||
        !unwind64_read(address, &value, sizeof(value)))
        return FALSE;
    *destination = value;
    if (pointers) pointers->FloatingContext[reg] =
        (PM128A)(ULONG_PTR)address;
    return TRUE;
}

static DWORD unwind64_operation_slots(const UNWIND_CODE *code)
{
    if (!code) return 0;
    switch (code->UnwindOp) {
    case UWOP_PUSH_NONVOL:
    case UWOP_ALLOC_SMALL:
    case UWOP_SET_FPREG:
    case UWOP_PUSH_MACHFRAME:
        return 1;
    case UWOP_ALLOC_LARGE:
        return code->OpInfo == 0 ? 2 : (code->OpInfo == 1 ? 3 : 0);
    case UWOP_SAVE_NONVOL:
    case UWOP_SAVE_XMM128:
        return 2;
    case UWOP_SAVE_NONVOL_FAR:
    case UWOP_SAVE_XMM128_FAR:
        return 3;
    default:
        return 0;
    }
}

static BOOL unwind64_load_info(ULONGLONG image_base, DWORD unwind_rva,
                               PUNWIND_INFO *info_out,
                               ULONGLONG *trailer_out)
{
    ULONGLONG address;
    BYTE header[4];
    if (!unwind64_add(image_base, unwind_rva, &address) ||
        !unwind64_read(address, header, sizeof(header)))
        return FALSE;

    BYTE version = header[0] & 7U;
    BYTE flags = header[0] >> 3;
    BYTE code_count = header[2];
    if (version != 1 || (flags & ~(UNW_FLAG_EHANDLER |
                                   UNW_FLAG_UHANDLER |
                                   UNW_FLAG_CHAININFO)) ||
        ((flags & UNW_FLAG_CHAININFO) &&
         (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))))
        return FALSE;

    ULONGLONG code_bytes = (ULONGLONG)code_count * sizeof(UNWIND_CODE);
    ULONGLONG aligned_code_bytes = (code_bytes + 3U) & ~3ULL;
    ULONGLONG trailer;
    if (!unwind64_add(address, 4U + aligned_code_bytes, &trailer) ||
        !unwind64_range_readable(address, 4U + aligned_code_bytes))
        return FALSE;

    PUNWIND_INFO info = (PUNWIND_INFO)(ULONG_PTR)address;
    if ((info->FrameRegister &&
         !unwind64_integer_is_nonvolatile(info->FrameRegister)) ||
        (!info->FrameRegister && info->FrameOffset))
        return FALSE;

    DWORD i = 0;
    BYTE previous_offset = 0xFF;
    while (i < code_count) {
        UNWIND_CODE code = info->UnwindCode[i];
        DWORD slots = unwind64_operation_slots(&code);
        if (!slots || slots > (DWORD)code_count - i ||
            code.CodeOffset > info->SizeOfProlog ||
            code.CodeOffset > previous_offset)
            return FALSE;
        switch (code.UnwindOp) {
        case UWOP_PUSH_NONVOL:
        case UWOP_SAVE_NONVOL:
        case UWOP_SAVE_NONVOL_FAR:
            if (!unwind64_integer_is_nonvolatile(code.OpInfo)) return FALSE;
            break;
        case UWOP_SET_FPREG:
            if (code.OpInfo || !info->FrameRegister) return FALSE;
            break;
        case UWOP_SAVE_XMM128:
        case UWOP_SAVE_XMM128_FAR:
            if (!unwind64_xmm_is_nonvolatile(code.OpInfo)) return FALSE;
            break;
        case UWOP_PUSH_MACHFRAME:
            if (code.OpInfo > 1) return FALSE;
            break;
        default:
            break;
        }
        previous_offset = code.CodeOffset;
        i += slots;
    }

    *info_out = info;
    *trailer_out = trailer;
    return TRUE;
}

static BOOL unwind64_image_table(ULONGLONG control_pc,
                                 ULONGLONG *image_base_out,
                                 PRUNTIME_FUNCTION *table_out,
                                 DWORD *count_out)
{
    ULONGLONG image_base;
    ULONGLONG allocation_size;
    if (!pe_va_query_range(control_pc, &image_base, &allocation_size, NULL) ||
        control_pc < image_base ||
        !pe_va_range_contains(image_base, allocation_size) ||
        !unwind64_range_inside(image_base, allocation_size, image_base,
                               sizeof(IMAGE_DOS_HEADER)) ||
        !win32_user_range_readable((const void *)(ULONG_PTR)image_base,
                                   sizeof(IMAGE_DOS_HEADER), FALSE))
        return FALSE;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)(ULONG_PTR)image_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < (LONG)sizeof(*dos))
        return FALSE;

    ULONGLONG nt_address;
    if (!unwind64_add(image_base, (DWORD)dos->e_lfanew, &nt_address))
        return FALSE;
    const SIZE_T optional_directory_end =
        __builtin_offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
        (IMAGE_DIRECTORY_ENTRY_EXCEPTION + 1U) *
            sizeof(IMAGE_DATA_DIRECTORY);
    const SIZE_T nt_minimum = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                              optional_directory_end;
    if (!unwind64_range_inside(image_base, allocation_size, nt_address,
                               nt_minimum) ||
        !win32_user_range_readable((const void *)(ULONG_PTR)nt_address,
                                   nt_minimum, FALSE))
        return FALSE;

    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(ULONG_PTR)nt_address;
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader < optional_directory_end ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
        !nt->OptionalHeader.SizeOfImage ||
        nt->OptionalHeader.SizeOfImage > allocation_size ||
        control_pc - image_base >= nt->OptionalHeader.SizeOfImage)
        return FALSE;

    IMAGE_DATA_DIRECTORY directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.VirtualAddress && !directory.Size) {
        if (image_base_out) *image_base_out = image_base;
        if (table_out) *table_out = NULL;
        if (count_out) *count_out = 0;
        return TRUE;
    }
    if (!directory.VirtualAddress || !directory.Size ||
        directory.Size % sizeof(RUNTIME_FUNCTION))
        return FALSE;

    ULONGLONG table_address;
    if ((directory.VirtualAddress & 3U) ||
        !unwind64_add(image_base, directory.VirtualAddress, &table_address) ||
        !unwind64_range_inside(image_base,
                               nt->OptionalHeader.SizeOfImage,
                               table_address, directory.Size) ||
        !pe_va_range_contains(table_address, directory.Size) ||
        !win32_user_range_readable(
            (const void *)(ULONG_PTR)table_address, directory.Size, FALSE))
        return FALSE;

    if (image_base_out) *image_base_out = image_base;
    if (table_out)
        *table_out = (PRUNTIME_FUNCTION)(ULONG_PTR)table_address;
    if (count_out) *count_out = directory.Size / sizeof(RUNTIME_FUNCTION);
    return TRUE;
}

static PRUNTIME_FUNCTION unwind64_binary_search(
    PRUNTIME_FUNCTION table, DWORD count, ULONGLONG relative_pc)
{
    DWORD low = 0;
    DWORD high = count;
    while (low < high) {
        DWORD middle = low + (high - low) / 2;
        PRUNTIME_FUNCTION entry = &table[middle];
        if (relative_pc < entry->BeginAddress) {
            high = middle;
        } else if (relative_pc >= entry->EndAddress) {
            low = middle + 1;
        } else if (entry->BeginAddress < entry->EndAddress) {
            return entry;
        } else {
            return NULL;
        }
    }
    return NULL;
}

static PRUNTIME_FUNCTION unwind64_lookup_dynamic(ULONGLONG control_pc,
                                                  ULONGLONG *image_base)
{
    DWORD owner = unwind64_owner();
    for (DWORD i = 0; i < UNWIND64_DYNAMIC_TABLES; i++) {
        UNWIND64_DYNAMIC_TABLE *record = &dynamic_tables[i];
        if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
                UNWIND64_TABLE_ACTIVE ||
            record->owner_pid != owner ||
            control_pc < record->range_start ||
            control_pc >= record->range_end)
            continue;

        PRUNTIME_FUNCTION table = record->table;
        DWORD count = record->count;
        ULONGLONG base = record->base;
        if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
                UNWIND64_TABLE_ACTIVE ||
            !table || !count || control_pc < base ||
            !win32_user_range_readable(table,
                (SIZE_T)count * sizeof(RUNTIME_FUNCTION), FALSE))
            continue;

        PRUNTIME_FUNCTION found =
            unwind64_binary_search(table, count, control_pc - base);
        if (found) {
            if (image_base) *image_base = base;
            return found;
        }
    }
    return NULL;
}

BOOL win32_unwind64_add_function_table(PRUNTIME_FUNCTION function_table,
                                       DWORD entry_count,
                                       ULONGLONG base_address)
{
    if (g_compat32_mode || !function_table || !entry_count ||
        ((ULONG_PTR)function_table & 3U))
        return FALSE;

    SIZE_T bytes = (SIZE_T)entry_count * sizeof(RUNTIME_FUNCTION);
    if (!win32_user_range_readable(function_table, bytes, FALSE))
        return FALSE;

    DWORD previous_end = 0;
    for (DWORD i = 0; i < entry_count; i++) {
        RUNTIME_FUNCTION entry = function_table[i];
        ULONGLONG ignored;
        if (entry.BeginAddress >= entry.EndAddress ||
            (i && entry.BeginAddress < previous_end) ||
            !unwind64_add(base_address, entry.EndAddress, &ignored))
            return FALSE;
        previous_end = entry.EndAddress;
    }

    DWORD owner = unwind64_owner();
    for (DWORD i = 0; i < UNWIND64_DYNAMIC_TABLES; i++) {
        UNWIND64_DYNAMIC_TABLE *record = &dynamic_tables[i];
        if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) ==
                UNWIND64_TABLE_ACTIVE &&
            record->owner_pid == owner && record->table == function_table)
            return FALSE;
    }

    for (DWORD i = 0; i < UNWIND64_DYNAMIC_TABLES; i++) {
        UNWIND64_DYNAMIC_TABLE *record = &dynamic_tables[i];
        DWORD expected = UNWIND64_TABLE_FREE;
        if (!__atomic_compare_exchange_n(
                &record->state, &expected, UNWIND64_TABLE_RESERVED, FALSE,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;

        record->owner_pid = owner;
        record->table = function_table;
        record->count = entry_count;
        record->base = base_address;
        (void)unwind64_add(base_address, function_table[0].BeginAddress,
                           &record->range_start);
        (void)unwind64_add(
            base_address, function_table[entry_count - 1].EndAddress,
            &record->range_end);
        __atomic_store_n(&record->state, UNWIND64_TABLE_ACTIVE,
                         __ATOMIC_RELEASE);
        return TRUE;
    }
    return FALSE;
}

BOOL win32_unwind64_delete_function_table(PRUNTIME_FUNCTION function_table)
{
    if (!function_table) return FALSE;
    DWORD owner = unwind64_owner();
    for (DWORD i = 0; i < UNWIND64_DYNAMIC_TABLES; i++) {
        UNWIND64_DYNAMIC_TABLE *record = &dynamic_tables[i];
        if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
                UNWIND64_TABLE_ACTIVE ||
            record->owner_pid != owner || record->table != function_table)
            continue;
        DWORD expected = UNWIND64_TABLE_ACTIVE;
        if (!__atomic_compare_exchange_n(
                &record->state, &expected, UNWIND64_TABLE_RESERVED, FALSE,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        record->owner_pid = 0;
        record->table = NULL;
        record->count = 0;
        record->base = 0;
        record->range_start = 0;
        record->range_end = 0;
        __atomic_store_n(&record->state, UNWIND64_TABLE_FREE,
                         __ATOMIC_RELEASE);
        return TRUE;
    }
    return FALSE;
}

PRUNTIME_FUNCTION win32_unwind64_lookup_function_entry(
    ULONGLONG control_pc, ULONGLONG *image_base,
    PUNWIND_HISTORY_TABLE history_table)
{
    (void)history_table;
    if (image_base) *image_base = 0;
    if (g_compat32_mode || !control_pc) return NULL;

    ULONGLONG internal_base =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_wrapper_region_start;
    ULONGLONG language_start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_language_handler;
    ULONGLONG language_end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_language_handler_end;
    ULONGLONG exception_start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_exception_handler;
    ULONGLONG exception_end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_exception_handler_end;
    ULONGLONG filter_start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_exception_filter;
    ULONGLONG filter_end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_exception_filter_end;
    ULONGLONG termination_start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_termination_handler;
    ULONGLONG termination_end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_termination_handler_end;
    ULONGLONG consolidate_start =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_consolidate;
    ULONGLONG consolidate_end =
        (ULONGLONG)(ULONG_PTR)win32_unwind64_call_consolidate_end;
    if (control_pc >= language_start && control_pc < language_end) {
        if (image_base) *image_base = internal_base;
        return &win32_unwind64_language_runtime_function;
    }
    if (control_pc >= exception_start && control_pc < exception_end) {
        if (image_base) *image_base = internal_base;
        return &win32_unwind64_exception_runtime_function;
    }
    if (control_pc >= filter_start && control_pc < filter_end) {
        if (image_base) *image_base = internal_base;
        return &win32_unwind64_filter_runtime_function;
    }
    if (control_pc >= termination_start && control_pc < termination_end) {
        if (image_base) *image_base = internal_base;
        return &win32_unwind64_termination_runtime_function;
    }
    if (control_pc >= consolidate_start && control_pc < consolidate_end) {
        if (image_base) *image_base = internal_base;
        return &win32_unwind64_consolidate_runtime_function;
    }

    ULONGLONG static_base = 0;
    PRUNTIME_FUNCTION table = NULL;
    DWORD count = 0;
    if (unwind64_image_table(control_pc, &static_base, &table, &count)) {
        if (image_base) *image_base = static_base;
        if (table && count) {
            PRUNTIME_FUNCTION found = unwind64_binary_search(
                table, count, control_pc - static_base);
            if (found) return found;
        }
    }

    return unwind64_lookup_dynamic(control_pc, image_base);
}

static ULONGLONG unwind64_establisher_frame(PCONTEXT context,
                                            PUNWIND_INFO info,
                                            ULONGLONG code_offset)
{
    if (!info->FrameRegister)
        return context->Rsp;
    if (!unwind64_integer_is_nonvolatile(info->FrameRegister))
        return 0;
    ULONGLONG frame_offset = (ULONGLONG)info->FrameOffset * 16U;
    if (code_offset >= info->SizeOfProlog ||
        (info->Flags & UNW_FLAG_CHAININFO)) {
        ULONGLONG *frame =
            unwind64_integer_register(context, info->FrameRegister);
        return frame && *frame >= frame_offset ? *frame - frame_offset : 0;
    }

    DWORD i = 0;
    while (i < info->CountOfCodes) {
        UNWIND_CODE code = info->UnwindCode[i];
        DWORD slots = unwind64_operation_slots(&code);
        if (!slots || slots > info->CountOfCodes - i) break;
        if (code.CodeOffset <= code_offset &&
            code.UnwindOp == UWOP_SET_FPREG) {
            ULONGLONG *frame =
                unwind64_integer_register(context, info->FrameRegister);
            return frame && *frame >= frame_offset
                       ? *frame - frame_offset : 0;
        }
        i += slots;
    }
    return context->Rsp;
}

static BOOL unwind64_epilog_target_outside(
    ULONGLONG target, ULONGLONG function_start, ULONGLONG function_end)
{
    return target < function_start || target >= function_end;
}

static BOOL unwind64_try_epilog(
    ULONGLONG image_base, ULONGLONG control_pc,
    PRUNTIME_FUNCTION function_entry, BYTE frame_register,
    PCONTEXT context, PKNONVOLATILE_CONTEXT_POINTERS context_pointers)
{
    ULONGLONG function_start;
    ULONGLONG function_end;
    if (!unwind64_add(image_base, function_entry->BeginAddress,
                      &function_start) ||
        !unwind64_add(image_base, function_entry->EndAddress,
                      &function_end) ||
        control_pc < function_start || control_pc >= function_end)
        return FALSE;

    CONTEXT work = *context;
    KNONVOLATILE_CONTEXT_POINTERS pointers;
    if (context_pointers) pointers = *context_pointers;
    ULONGLONG cursor = control_pc;
    ULONGLONG scan_limit = function_end - cursor;
    if (scan_limit > UNWIND64_MAX_EPILOG_BYTES)
        scan_limit = UNWIND64_MAX_EPILOG_BYTES;
    ULONGLONG scanned = 0;
    BOOL unwound_to_call = FALSE;
    BYTE bytes[7];

    if (scan_limit >= 4 && unwind64_read(cursor, bytes, 4) &&
        bytes[0] == 0x48 && bytes[1] == 0x83 && bytes[2] == 0xC4) {
        if (!unwind64_advance_rsp(&work, bytes[3])) return FALSE;
        cursor += 4;
        scanned += 4;
    } else if (scan_limit >= 7 && unwind64_read(cursor, bytes, 7) &&
               bytes[0] == 0x48 && bytes[1] == 0x81 &&
               bytes[2] == 0xC4) {
        DWORD amount;
        unwind64_copy(&amount, &bytes[3], sizeof(amount));
        if (!unwind64_advance_rsp(&work, amount)) return FALSE;
        cursor += 7;
        scanned += 7;
    } else if (scan_limit >= 4 && unwind64_read(cursor, bytes, 4) &&
               (bytes[0] & 0xF8) == 0x48 && bytes[1] == 0x8D) {
        BYTE modrm = bytes[2];
        BYTE mode = modrm >> 6;
        BYTE destination = (modrm >> 3) & 7U;
        BYTE source = (modrm & 7U) + ((bytes[0] & 1U) ? 8U : 0U);
        if ((bytes[0] & 4U) || destination != 4 ||
            (modrm & 7U) == 4U ||
            source != frame_register || (mode != 1 && mode != 2))
            return FALSE;
        ULONGLONG *frame = unwind64_integer_register(&work, source);
        if (!frame) return FALSE;
        if (mode == 1) {
            if (!unwind64_add_signed(*frame, (int8_t)bytes[3],
                                     &work.Rsp))
                return FALSE;
            cursor += 4;
            scanned += 4;
        } else {
            if (scan_limit < 7 || !unwind64_read(cursor, bytes, 7))
                return FALSE;
            LONG displacement;
            unwind64_copy(&displacement, &bytes[3], sizeof(displacement));
            if (!unwind64_add_signed(*frame, displacement, &work.Rsp))
                return FALSE;
            cursor += 7;
            scanned += 7;
        }
    }

    while (scanned < scan_limit) {
        BYTE first;
        if (!unwind64_read_u8(cursor, &first)) return FALSE;
        BYTE reg;
        DWORD instruction_size;
        if ((first & 0xF8U) == 0x58U) {
            reg = first & 7U;
            instruction_size = 1;
        } else if ((first == 0x41 || first == 0x49) &&
                   scanned + 2 <= scan_limit) {
            BYTE second;
            if (!unwind64_read_u8(cursor + 1, &second) ||
                (second & 0xF8U) != 0x58U)
                break;
            reg = (second & 7U) + 8U;
            instruction_size = 2;
        } else {
            break;
        }
        if (!unwind64_restore_integer(
                &work, context_pointers ? &pointers : NULL, reg, work.Rsp))
            return FALSE;
        if (!unwind64_advance_rsp(&work, sizeof(ULONGLONG))) return FALSE;
        cursor += instruction_size;
        scanned += instruction_size;
    }

    ULONGLONG remaining = scan_limit - scanned;
    if (!remaining) return FALSE;
    BYTE opcode;
    if (!unwind64_read_u8(cursor, &opcode)) return FALSE;
    if (opcode == 0xC3) {
        if (!unwind64_read_u64(work.Rsp, &work.Rip)) return FALSE;
        if (!unwind64_advance_rsp(&work, sizeof(ULONGLONG))) return FALSE;
        unwound_to_call = TRUE;
    } else if (opcode == 0xC2) {
        USHORT amount;
        if (remaining < 3 || !unwind64_read_u16(cursor + 1, &amount) ||
            !unwind64_read_u64(work.Rsp, &work.Rip))
            return FALSE;
        if (!unwind64_advance_rsp(
                &work, sizeof(ULONGLONG) + (ULONGLONG)amount))
            return FALSE;
        unwound_to_call = TRUE;
    } else if (opcode == 0xF3) {
        BYTE next;
        if (remaining < 2 || !unwind64_read_u8(cursor + 1, &next) ||
            next != 0xC3 ||
            !unwind64_read_u64(work.Rsp, &work.Rip))
            return FALSE;
        if (!unwind64_advance_rsp(&work, sizeof(ULONGLONG))) return FALSE;
        unwound_to_call = TRUE;
    } else if (opcode == 0xE9 || opcode == 0xEB) {
        LONGLONG displacement;
        DWORD instruction_size;
        if (opcode == 0xE9) {
            LONG relative;
            if (remaining < 5 ||
                !unwind64_read(cursor + 1, &relative, sizeof(relative)))
                return FALSE;
            displacement = relative;
            instruction_size = 5;
        } else {
            BYTE relative;
            if (remaining < 2 ||
                !unwind64_read_u8(cursor + 1, &relative))
                return FALSE;
            displacement = (int8_t)relative;
            instruction_size = 2;
        }
        ULONGLONG target;
        if (!unwind64_add(cursor, instruction_size, &target) ||
            !unwind64_add_signed(target, displacement, &target))
            return FALSE;
        if (!unwind64_epilog_target_outside(target, function_start,
                                            function_end))
            return FALSE;
        work.Rip = target;
    } else {
        BYTE prefix = (opcode & 0xF0U) == 0x40U ? 1U : 0U;
        BYTE instruction[6];
        if (remaining < (ULONGLONG)prefix + 2U ||
            !unwind64_read(cursor, instruction, prefix + 2U))
            return FALSE;
        if (instruction[prefix] != 0xFF) return FALSE;
        BYTE modrm = instruction[prefix + 1];
        if ((modrm & 0x38U) != 0x20U) return FALSE;
        if ((modrm & 0xC0U) == 0xC0U) {
            BYTE reg = (modrm & 7U) +
                       ((prefix && (instruction[0] & 1U)) ? 8U : 0U);
            ULONGLONG *target = unwind64_integer_register(&work, reg);
            if (!target || !unwind64_epilog_target_outside(
                               *target, function_start, function_end))
                return FALSE;
            work.Rip = *target;
        } else if (!prefix && modrm == 0x25) {
            LONG displacement;
            ULONGLONG pointer_address;
            ULONGLONG target;
            if (remaining < sizeof(instruction) ||
                !unwind64_read(cursor, instruction, sizeof(instruction)))
                return FALSE;
            unwind64_copy(&displacement, &instruction[2],
                          sizeof(displacement));
            if (!unwind64_add(cursor, sizeof(instruction),
                              &pointer_address) ||
                !unwind64_add_signed(pointer_address, displacement,
                                     &pointer_address))
                return FALSE;
            if (!unwind64_read_u64(pointer_address, &target) ||
                !unwind64_epilog_target_outside(target, function_start,
                                                function_end))
                return FALSE;
            work.Rip = target;
        } else {
            return FALSE;
        }
    }

    if (unwound_to_call)
        work.ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
    else
        work.ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
    *context = work;
    if (context_pointers) *context_pointers = pointers;
    return TRUE;
}

static BOOL unwind64_process_codes(
    PUNWIND_INFO info, DWORD start,
    PCONTEXT context, PKNONVOLATILE_CONTEXT_POINTERS pointers,
    BOOL *machine_frame)
{
    DWORD i = start;
    while (i < info->CountOfCodes) {
        UNWIND_CODE code = info->UnwindCode[i];
        DWORD slots = unwind64_operation_slots(&code);
        if (!slots || slots > info->CountOfCodes - i) return FALSE;

        ULONGLONG address;
        DWORD offset;
        switch (code.UnwindOp) {
        case UWOP_PUSH_NONVOL:
            if (!unwind64_restore_integer(context, pointers, code.OpInfo,
                                          context->Rsp))
                return FALSE;
            if (!unwind64_advance_rsp(context, sizeof(ULONGLONG)))
                return FALSE;
            break;
        case UWOP_ALLOC_LARGE:
            if (code.OpInfo == 0) {
                if (!unwind64_advance_rsp(
                        context,
                        (ULONGLONG)info->UnwindCode[i + 1].FrameOffset * 8U))
                    return FALSE;
            } else {
                unwind64_copy(&offset, &info->UnwindCode[i + 1],
                              sizeof(offset));
                if (!unwind64_advance_rsp(context, offset)) return FALSE;
            }
            break;
        case UWOP_ALLOC_SMALL:
            if (!unwind64_advance_rsp(
                    context, (ULONGLONG)(code.OpInfo + 1U) * 8U))
                return FALSE;
            break;
        case UWOP_SET_FPREG: {
            ULONGLONG *frame =
                unwind64_integer_register(context, info->FrameRegister);
            ULONGLONG frame_offset =
                (ULONGLONG)info->FrameOffset * 16U;
            if (!frame ||
                !unwind64_integer_is_nonvolatile(info->FrameRegister) ||
                *frame < frame_offset)
                return FALSE;
            context->Rsp = *frame - frame_offset;
            break;
        }
        case UWOP_SAVE_NONVOL:
            offset = info->UnwindCode[i + 1].FrameOffset * 8U;
            if (!unwind64_add(context->Rsp, offset, &address) ||
                !unwind64_restore_integer(context, pointers, code.OpInfo,
                                          address))
                return FALSE;
            break;
        case UWOP_SAVE_NONVOL_FAR:
            unwind64_copy(&offset, &info->UnwindCode[i + 1],
                          sizeof(offset));
            if (!unwind64_add(context->Rsp, offset, &address) ||
                !unwind64_restore_integer(context, pointers, code.OpInfo,
                                          address))
                return FALSE;
            break;
        case UWOP_SAVE_XMM128:
            offset = info->UnwindCode[i + 1].FrameOffset * 16U;
            if (!unwind64_add(context->Rsp, offset, &address) ||
                !unwind64_restore_xmm(context, pointers, code.OpInfo,
                                      address))
                return FALSE;
            break;
        case UWOP_SAVE_XMM128_FAR:
            unwind64_copy(&offset, &info->UnwindCode[i + 1],
                          sizeof(offset));
            if (!unwind64_add(context->Rsp, offset, &address) ||
                !unwind64_restore_xmm(context, pointers, code.OpInfo,
                                      address))
                return FALSE;
            break;
        case UWOP_PUSH_MACHFRAME: {
            ULONGLONG stack;
            ULONGLONG return_stack_address;
            ULONGLONG return_stack;
            if (code.OpInfo > 1 ||
                !unwind64_add(context->Rsp,
                              code.OpInfo ? sizeof(ULONGLONG) : 0U,
                              &stack) ||
                !unwind64_add(stack, 0x18, &return_stack_address) ||
                !unwind64_read_u64(stack, &context->Rip) ||
                !unwind64_read_u64(return_stack_address, &return_stack))
                return FALSE;
            context->Rsp = return_stack;
            *machine_frame = TRUE;
            break;
        }
        default:
            return FALSE;
        }
        i += slots;
    }
    return TRUE;
}

PVOID win32_unwind64_virtual_unwind(
    DWORD handler_type, ULONGLONG image_base, ULONGLONG control_pc,
    PRUNTIME_FUNCTION function_entry, PCONTEXT context,
    PVOID *handler_data, ULONGLONG *establisher_frame,
    PKNONVOLATILE_CONTEXT_POINTERS context_pointers)
{
    if (handler_data) *handler_data = NULL;
    if (establisher_frame) *establisher_frame = context ? context->Rsp : 0;
    if (g_compat32_mode || !context) return NULL;

    CONTEXT work = *context;
    KNONVOLATILE_CONTEXT_POINTERS pointers;
    if (context_pointers) pointers = *context_pointers;

    if (!function_entry) {
        if (!unwind64_read_u64(work.Rsp, &work.Rip)) return NULL;
        if (establisher_frame) *establisher_frame = work.Rsp;
        if (!unwind64_advance_rsp(&work, sizeof(ULONGLONG))) return NULL;
        work.ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
        *context = work;
        return NULL;
    }
    if (!unwind64_range_readable(
            (ULONGLONG)(ULONG_PTR)function_entry,
            sizeof(*function_entry)))
        return NULL;

    RUNTIME_FUNCTION current = *function_entry;
    ULONGLONG begin;
    ULONGLONG end;
    if (current.BeginAddress >= current.EndAddress ||
        !unwind64_add(image_base, current.BeginAddress, &begin) ||
        !unwind64_add(image_base, current.EndAddress, &end) ||
        control_pc < begin || control_pc >= end)
        return NULL;

    PUNWIND_INFO info;
    ULONGLONG trailer;
    if (!unwind64_load_info(image_base, current.UnwindData, &info,
                            &trailer))
        return NULL;

    ULONGLONG code_offset = control_pc - begin;
    ULONGLONG frame = unwind64_establisher_frame(&work, info, code_offset);
    if (!frame) return NULL;
    if (establisher_frame) *establisher_frame = frame;

    if (code_offset > info->SizeOfProlog &&
        unwind64_try_epilog(image_base, control_pc, &current,
                            info->FrameRegister, &work,
                            context_pointers ? &pointers : NULL)) {
        *context = work;
        if (context_pointers) *context_pointers = pointers;
        return NULL;
    }

    DWORD start = 0;
    while (start < info->CountOfCodes &&
           info->UnwindCode[start].CodeOffset > code_offset) {
        DWORD slots = unwind64_operation_slots(&info->UnwindCode[start]);
        if (!slots || slots > info->CountOfCodes - start) return NULL;
        start += slots;
    }

    BOOL machine_frame = FALSE;
    DWORD chain_depth = 0;
    for (;;) {
        if (!unwind64_process_codes(
                info, start, &work,
                context_pointers ? &pointers : NULL, &machine_frame))
            return NULL;
        if (!(info->Flags & UNW_FLAG_CHAININFO)) break;
        if (++chain_depth > UNWIND64_MAX_CHAIN_DEPTH ||
            !unwind64_read(trailer, &current, sizeof(current)) ||
            !unwind64_load_info(image_base, current.UnwindData,
                                &info, &trailer))
            return NULL;
        start = 0;
    }

    if (!machine_frame) {
        if (!unwind64_read_u64(work.Rsp, &work.Rip)) return NULL;
        if (!unwind64_advance_rsp(&work, sizeof(ULONGLONG))) return NULL;
        work.ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
    } else {
        work.ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
    }

    PVOID language_handler = NULL;
    if (code_offset >= info->SizeOfProlog &&
        (info->Flags & (handler_type &
                        (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)))) {
        DWORD handler_rva;
        ULONGLONG handler_address;
        if (!unwind64_read_u32(trailer, &handler_rva) ||
            !unwind64_add(image_base, handler_rva, &handler_address))
            return NULL;
        language_handler = (PVOID)(ULONG_PTR)handler_address;
        if (handler_data) {
            ULONGLONG data_address;
            if (!unwind64_add(trailer, sizeof(DWORD), &data_address))
                return NULL;
            *handler_data = (PVOID)(ULONG_PTR)data_address;
        }
    }

    *context = work;
    if (context_pointers) *context_pointers = pointers;
    return language_handler;
}

static BOOL unwind64_stack_bounds(ULONGLONG *limit_out,
                                  ULONGLONG *base_out)
{
    TEB *teb = win64_current_teb();
    ULONGLONG limit = teb ? (ULONGLONG)(ULONG_PTR)teb->StackLimit : 0;
    ULONGLONG base = teb ? (ULONGLONG)(ULONG_PTR)teb->StackBase : 0;
    if (!limit || limit >= base) return FALSE;
    if (limit_out) *limit_out = limit;
    if (base_out) *base_out = base;
    return TRUE;
}

static BOOL unwind64_valid_stack_address(ULONGLONG address,
                                         ULONGLONG limit,
                                         ULONGLONG base)
{
    return !(address & (sizeof(ULONGLONG) - 1U)) &&
           address >= limit && address <= base;
}

static NTSTATUS unwind64_scope_table_validate(
    PDISPATCHER_CONTEXT dispatch, DWORD *count_out)
{
    if (!dispatch || !dispatch->HandlerData ||
        ((ULONG_PTR)dispatch->HandlerData & (sizeof(DWORD) - 1U)))
        return STATUS_BAD_FUNCTION_TABLE;

    DWORD count;
    ULONGLONG table_address =
        (ULONGLONG)(ULONG_PTR)dispatch->HandlerData;
    if (!unwind64_read_u32(table_address, &count))
        return STATUS_BAD_FUNCTION_TABLE;

    ULONGLONG table_size = sizeof(DWORD) +
        (ULONGLONG)count * sizeof(SCOPE_RECORD_AMD64);
    if (table_size > (ULONGLONG)(SIZE_T)-1 ||
        !win32_user_range_readable(dispatch->HandlerData,
                                   (SIZE_T)table_size, FALSE) ||
        dispatch->ScopeIndex > count)
        return STATUS_BAD_FUNCTION_TABLE;

    for (DWORD i = 0; i < count; i++) {
        SCOPE_RECORD_AMD64 scope;
        ULONGLONG record_address;
        ULONGLONG begin;
        ULONGLONG end;
        ULONGLONG target;
        if (!unwind64_add(table_address, sizeof(DWORD) +
                          (ULONGLONG)i * sizeof(scope), &record_address) ||
            !unwind64_read(record_address, &scope, sizeof(scope)) ||
            scope.BeginAddress >= scope.EndAddress ||
            !unwind64_add(dispatch->ImageBase, scope.BeginAddress, &begin) ||
            !unwind64_add(dispatch->ImageBase, scope.EndAddress, &end) ||
            !win32_user_range_executable((PVOID)(ULONG_PTR)begin, 1, FALSE) ||
            !win32_user_range_executable((PVOID)(ULONG_PTR)(end - 1U),
                                         1, FALSE))
            return STATUS_BAD_FUNCTION_TABLE;

        if (scope.JumpTarget) {
            if (!unwind64_add(dispatch->ImageBase, scope.JumpTarget,
                              &target) ||
                !win32_user_range_executable((PVOID)(ULONG_PTR)target,
                                             1, FALSE))
                return STATUS_BAD_FUNCTION_TABLE;
            if (scope.HandlerAddress != EXCEPTION_EXECUTE_HANDLER &&
                (!unwind64_add(dispatch->ImageBase, scope.HandlerAddress,
                               &target) ||
                 !win32_user_range_executable((PVOID)(ULONG_PTR)target,
                                              1, FALSE)))
                return STATUS_BAD_FUNCTION_TABLE;
        } else if (!unwind64_add(dispatch->ImageBase, scope.HandlerAddress,
                                 &target) ||
                   !win32_user_range_executable((PVOID)(ULONG_PTR)target,
                                                1, FALSE)) {
            return STATUS_BAD_FUNCTION_TABLE;
        }
    }

    if (count_out) *count_out = count;
    return STATUS_SUCCESS;
}

static BOOL unwind64_scope_record(PDISPATCHER_CONTEXT dispatch, DWORD index,
                                  PSCOPE_RECORD_AMD64 scope)
{
    ULONGLONG address;
    return dispatch && scope &&
           unwind64_add((ULONGLONG)(ULONG_PTR)dispatch->HandlerData,
                        sizeof(DWORD) +
                            (ULONGLONG)index * sizeof(*scope),
                        &address) &&
           unwind64_read(address, scope, sizeof(*scope));
}

EXCEPTION_DISPOSITION WINAPI win32_unwind64_collision_handler(
    PEXCEPTION_RECORD record, PVOID establisher_frame, PCONTEXT context,
    PDISPATCHER_CONTEXT dispatch)
{
    (void)record;
    (void)context;
    if (!establisher_frame || !dispatch || !dispatch->ContextRecord)
        return ExceptionContinueSearch;

    ULONGLONG slot;
    ULONGLONG original_address;
    DISPATCHER_CONTEXT original;
    CONTEXT original_context;
    if (!unwind64_add((ULONGLONG)(ULONG_PTR)establisher_frame, 0x20,
                      &slot) ||
        !unwind64_read_u64(slot, &original_address) || !original_address ||
        !unwind64_read(original_address, &original, sizeof(original)) ||
        !original.ContextRecord ||
        !unwind64_read((ULONGLONG)(ULONG_PTR)original.ContextRecord,
                       &original_context, sizeof(original_context)) ||
        !win32_user_range_writable(dispatch->ContextRecord,
                                   sizeof(*dispatch->ContextRecord), FALSE)) {
        dispatch->Fill0 = (DWORD)STATUS_BAD_STACK;
        return ExceptionContinueSearch;
    }

    ULONGLONG target_ip = dispatch->TargetIp;
    PCONTEXT destination = dispatch->ContextRecord;
    dispatch->ControlPc = original.ControlPc;
    dispatch->ImageBase = original.ImageBase;
    dispatch->FunctionEntry = original.FunctionEntry;
    dispatch->EstablisherFrame = original.EstablisherFrame;
    dispatch->TargetIp = target_ip;
    dispatch->ContextRecord = destination;
    dispatch->LanguageHandler = original.LanguageHandler;
    dispatch->HandlerData = original.HandlerData;
    dispatch->HistoryTable = original.HistoryTable;
    dispatch->ScopeIndex = original.ScopeIndex;
    dispatch->Fill0 = 0;
    *destination = original_context;
    destination->ContextFlags &= ~0x40U;
    return ExceptionCollidedUnwind;
}

EXCEPTION_DISPOSITION WINAPI win32_unwind64_exception_bridge_handler(
    PEXCEPTION_RECORD record, PVOID establisher_frame, PCONTEXT context,
    PDISPATCHER_CONTEXT dispatch)
{
    (void)context;
    if (!record || !establisher_frame || !dispatch ||
        !dispatch->ContextRecord)
        return ExceptionContinueSearch;

    ULONGLONG dispatch_slot;
    ULONGLONG original_address;
    DISPATCHER_CONTEXT original;
    CONTEXT source_context;
    PCONTEXT frame_context = NULL;
    PCONTEXT next_context = NULL;
    BOOL unwinding = (record->ExceptionFlags &
                      (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) != 0;
    if (!unwind64_add((ULONGLONG)(ULONG_PTR)establisher_frame, 0x20,
                      &dispatch_slot) ||
        !unwind64_read_u64(dispatch_slot, &original_address) ||
        !original_address ||
        !unwind64_read(original_address, &original, sizeof(original)) ||
        !original.ContextRecord ||
        !win32_user_range_writable(dispatch->ContextRecord,
                                   sizeof(*dispatch->ContextRecord), FALSE)) {
        dispatch->Fill0 = (DWORD)STATUS_BAD_STACK;
        return ExceptionContinueSearch;
    }

    (void)unwind64_dispatch_contexts(
        (PDISPATCHER_CONTEXT)(ULONG_PTR)original_address,
        &frame_context, &next_context);
    PCONTEXT source = unwinding ? frame_context : next_context;
    if (!source) source = original.ContextRecord;
    if (!unwind64_read((ULONGLONG)(ULONG_PTR)source, &source_context,
                       sizeof(source_context))) {
        dispatch->Fill0 = (DWORD)STATUS_BAD_STACK;
        return ExceptionContinueSearch;
    }

    PCONTEXT destination = dispatch->ContextRecord;
    if (!unwinding) {
        dispatch->EstablisherFrame = original.EstablisherFrame;
        dispatch->Fill0 = 0;
        *destination = source_context;
        destination->ContextFlags &= ~0x40U;
        return ExceptionNestedException;
    }

    ULONGLONG target_ip = dispatch->TargetIp;
    dispatch->ControlPc = original.ControlPc;
    dispatch->ImageBase = original.ImageBase;
    dispatch->FunctionEntry = original.FunctionEntry;
    dispatch->EstablisherFrame = original.EstablisherFrame;
    dispatch->TargetIp = target_ip;
    dispatch->ContextRecord = destination;
    dispatch->LanguageHandler = original.LanguageHandler;
    dispatch->HandlerData = original.HandlerData;
    dispatch->HistoryTable = original.HistoryTable;
    dispatch->ScopeIndex = original.ScopeIndex;
    dispatch->Fill0 = 0;
    *destination = source_context;
    destination->ContextFlags &= ~0x40U;
    return ExceptionCollidedUnwind;
}

NTSTATUS win32_unwind64_unwind_ex(
    PEXCEPTION_RECORD record, PCONTEXT context, PVOID target_frame_pointer,
    PVOID target_ip_pointer, PVOID return_value_pointer,
    PUNWIND_HISTORY_TABLE history)
{
    if (g_compat32_mode || !record || !context)
        return STATUS_INVALID_PARAMETER;

    ULONGLONG target_frame =
        (ULONGLONG)(ULONG_PTR)target_frame_pointer;
    ULONGLONG target_ip = (ULONGLONG)(ULONG_PTR)target_ip_pointer;
    ULONGLONG return_value =
        (ULONGLONG)(ULONG_PTR)return_value_pointer;
    BOOL exit_unwind = target_frame == 0;

    ULONGLONG stack_limit;
    ULONGLONG stack_base;
    if (!unwind64_stack_bounds(&stack_limit, &stack_base) ||
        !unwind64_valid_stack_address(context->Rsp, stack_limit, stack_base)) {
        record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
        return STATUS_BAD_STACK;
    }
    if (!exit_unwind &&
        (!unwind64_valid_stack_address(target_frame, stack_limit,
                                       stack_base) ||
         target_frame < context->Rsp || !target_ip ||
         !win32_user_range_executable(target_ip_pointer, 1, FALSE)))
        return unwind64_invalid_target("arguments", context->Rsp, target_ip,
                                       target_frame);

    CONTEXT walk = *context;
    record->ExceptionFlags |= EXCEPTION_UNWINDING;
    record->ExceptionFlags &= ~(EXCEPTION_EXIT_UNWIND |
                                EXCEPTION_TARGET_UNWIND |
                                EXCEPTION_COLLIDED_UNWIND);
    if (exit_unwind)
        record->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    DWORD collision_count = 0;
    for (DWORD depth = 0; depth < UNWIND64_MAX_DISPATCH_FRAMES; depth++) {
        if (!walk.Rip || walk.Rsp == stack_base) {
            if (exit_unwind) {
                walk.Rax = return_value;
                *context = walk;
                return STATUS_SUCCESS;
            }
            return unwind64_invalid_target("end", walk.Rsp, walk.Rip,
                                           target_frame);
        }
        if (!unwind64_valid_stack_address(walk.Rsp, stack_limit, stack_base)) {
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }

        CONTEXT before = walk;
        ULONGLONG control_pc = unwind64_context_control_pc(&before);
        ULONGLONG image_base = 0;
        PRUNTIME_FUNCTION function =
            win32_unwind64_lookup_function_entry(
                control_pc, &image_base, history);
        PVOID handler_data = NULL;
        ULONGLONG establisher_frame = 0;
        PVOID language_handler = win32_unwind64_virtual_unwind(
            UNW_FLAG_UHANDLER, image_base, control_pc, function, &walk,
            &handler_data, &establisher_frame, NULL);

        if (walk.Rip == before.Rip && walk.Rsp == before.Rsp)
            return STATUS_BAD_FUNCTION_TABLE;
        if (!unwind64_valid_stack_address(establisher_frame, stack_limit,
                                          stack_base) ||
            !unwind64_valid_stack_address(walk.Rsp, stack_limit,
                                          stack_base)) {
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }
        if (!exit_unwind && establisher_frame > target_frame)
            return unwind64_invalid_target("crossed", before.Rsp,
                                           establisher_frame, target_frame);

        before.Rax = return_value;

        UNWIND64_DISPATCH_STATE state = {
            .Dispatch = {
                .ControlPc = control_pc,
                .ImageBase = image_base,
                .FunctionEntry = function,
                .EstablisherFrame = establisher_frame,
                .TargetIp = target_ip,
                .ContextRecord = &before,
                .LanguageHandler = (PEXCEPTION_ROUTINE)language_handler,
                .HandlerData = handler_data,
                .HistoryTable = history,
                .ScopeIndex = 0,
                .Fill0 = 0,
            },
            .Magic = UNWIND64_DISPATCH_STATE_MAGIC,
            .FrameContext = &before,
            .NextContext = &walk,
        };
        PDISPATCHER_CONTEXT dispatch = &state.Dispatch;

invoke_unwind_handler:
        establisher_frame = dispatch->EstablisherFrame;
        language_handler = (PVOID)dispatch->LanguageHandler;
        dispatch->TargetIp = target_ip;
        dispatch->ContextRecord = &before;
        before.Rax = return_value;
        record->ExceptionFlags &= ~EXCEPTION_TARGET_UNWIND;
        if (!exit_unwind && establisher_frame == target_frame)
            record->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;

        if (language_handler) {
            if (!unwind64_handler_executable(language_handler))
                return STATUS_BAD_FUNCTION_TABLE;
            EXCEPTION_DISPOSITION disposition =
                win32_unwind64_call_language_handler(
                    record, (PVOID)(ULONG_PTR)establisher_frame,
                    &before, dispatch, dispatch->LanguageHandler);
            if (dispatch->Fill0)
                return (NTSTATUS)dispatch->Fill0;
            if (disposition == ExceptionCollidedUnwind) {
                if (++collision_count > UNWIND64_MAX_DISPATCH_FRAMES ||
                    dispatch->ContextRecord != &before ||
                    !dispatch->FunctionEntry || !dispatch->ControlPc ||
                    !dispatch->EstablisherFrame ||
                    !dispatch->LanguageHandler)
                    return STATUS_INVALID_DISPOSITION;

                walk = before;
                PVOID ignored_data = NULL;
                ULONGLONG collided_frame = 0;
                CONTEXT collided_before = walk;
                win32_unwind64_virtual_unwind(
                    UNW_FLAG_NHANDLER, dispatch->ImageBase,
                    dispatch->ControlPc, dispatch->FunctionEntry, &walk,
                    &ignored_data, &collided_frame, NULL);
                if ((walk.Rip == collided_before.Rip &&
                     walk.Rsp == collided_before.Rsp) ||
                    collided_frame != dispatch->EstablisherFrame ||
                    !unwind64_valid_stack_address(
                        dispatch->EstablisherFrame, stack_limit, stack_base) ||
                    !unwind64_valid_stack_address(
                        walk.Rsp, stack_limit, stack_base) ||
                    (!exit_unwind &&
                     dispatch->EstablisherFrame > target_frame))
                    return STATUS_BAD_STACK;

                record->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                goto invoke_unwind_handler;
            }
            if (disposition != ExceptionContinueSearch)
                return STATUS_INVALID_DISPOSITION;
            record->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
        }

        if (!exit_unwind && establisher_frame == target_frame) {
            before.Rip = target_ip;
            before.ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
            *context = before;
            return STATUS_SUCCESS;
        }

        walk.Rax = return_value;
        *context = walk;
    }

    record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
    return STATUS_BAD_STACK;
}

EXCEPTION_DISPOSITION WINAPI win32_unwind64_c_specific_handler(
    PEXCEPTION_RECORD record, PVOID establisher_frame, PCONTEXT context,
    PDISPATCHER_CONTEXT dispatch)
{
    if (g_compat32_mode || !record || !establisher_frame || !context ||
        !dispatch)
        return ExceptionContinueSearch;

    DWORD count;
    NTSTATUS status = unwind64_scope_table_validate(dispatch, &count);
    if (status != STATUS_SUCCESS) {
        dispatch->Fill0 = (DWORD)status;
        return ExceptionContinueSearch;
    }

    for (DWORD i = dispatch->ScopeIndex; i < count; i++) {
        SCOPE_RECORD_AMD64 scope;
        ULONGLONG begin;
        ULONGLONG end;
        ULONGLONG handler;
        if (!unwind64_scope_record(dispatch, i, &scope) ||
            !unwind64_add(dispatch->ImageBase, scope.BeginAddress, &begin) ||
            !unwind64_add(dispatch->ImageBase, scope.EndAddress, &end)) {
            dispatch->Fill0 = (DWORD)STATUS_BAD_FUNCTION_TABLE;
            return ExceptionContinueSearch;
        }
        if (dispatch->ControlPc < begin || dispatch->ControlPc >= end)
            continue;

        if (record->ExceptionFlags &
                (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
            if (scope.JumpTarget) continue;
            if ((record->ExceptionFlags & EXCEPTION_TARGET_UNWIND) &&
                dispatch->TargetIp >= begin && dispatch->TargetIp < end)
                break;
            if (!unwind64_add(dispatch->ImageBase, scope.HandlerAddress,
                              &handler) ||
                !unwind64_handler_executable(
                    (PVOID)(ULONG_PTR)handler)) {
                dispatch->Fill0 = (DWORD)STATUS_BAD_FUNCTION_TABLE;
                return ExceptionContinueSearch;
            }
            dispatch->ScopeIndex = i + 1;
            win32_unwind64_call_termination_handler(
                (PVOID)(ULONG_PTR)handler, TRUE, establisher_frame,
                dispatch);
            continue;
        }

        if (!scope.JumpTarget) continue;
        if (scope.HandlerAddress != EXCEPTION_EXECUTE_HANDLER) {
            if (!unwind64_add(dispatch->ImageBase, scope.HandlerAddress,
                              &handler) ||
                !unwind64_handler_executable(
                    (PVOID)(ULONG_PTR)handler)) {
                dispatch->Fill0 = (DWORD)STATUS_BAD_FUNCTION_TABLE;
                return ExceptionContinueSearch;
            }
            EXCEPTION_POINTERS pointers = {
                .ExceptionRecord = record,
                .ContextRecord = context,
            };
            LONG filter_result = win32_unwind64_call_exception_filter(
                (PVOID)(ULONG_PTR)handler, &pointers, establisher_frame,
                dispatch);
            if (filter_result == EXCEPTION_CONTINUE_EXECUTION)
                return ExceptionContinueExecution;
            if (filter_result == EXCEPTION_CONTINUE_SEARCH)
                continue;
            if (filter_result != EXCEPTION_EXECUTE_HANDLER) {
                dispatch->Fill0 = (DWORD)STATUS_INVALID_DISPOSITION;
                return ExceptionContinueSearch;
            }
        }

        ULONGLONG target_ip;
        if (!unwind64_add(dispatch->ImageBase, scope.JumpTarget,
                          &target_ip)) {
            dispatch->Fill0 = (DWORD)STATUS_BAD_FUNCTION_TABLE;
            return ExceptionContinueSearch;
        }
        status = win32_unwind64_unwind_ex(
            record, context, (PVOID)(ULONG_PTR)establisher_frame,
            (PVOID)(ULONG_PTR)target_ip,
            (PVOID)(ULONG_PTR)(ULONGLONG)record->ExceptionCode,
            dispatch->HistoryTable);
        if (status != STATUS_SUCCESS) {
            dispatch->Fill0 = (DWORD)status;
            return ExceptionContinueSearch;
        }
        dispatch->TargetIp = target_ip;
        return ExceptionContinueExecution;
    }

    return ExceptionContinueSearch;
}

NTSTATUS win32_unwind64_dispatch_exception(PEXCEPTION_RECORD record,
                                           PCONTEXT context)
{
    if (g_compat32_mode || !record || !context)
        return STATUS_INVALID_PARAMETER;

    ULONGLONG stack_limit;
    ULONGLONG stack_base;
    if (!unwind64_stack_bounds(&stack_limit, &stack_base) ||
        !unwind64_valid_stack_address(context->Rsp, stack_limit, stack_base)) {
        record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
        return STATUS_BAD_STACK;
    }

    CONTEXT walk = *context;
    UNWIND_HISTORY_TABLE history;
    UNWIND64_DISPATCH_STATE state;
    PDISPATCHER_CONTEXT dispatch = &state.Dispatch;
    BYTE *history_bytes = (BYTE *)&history;
    BYTE *dispatch_bytes = (BYTE *)&state;
    for (SIZE_T i = 0; i < sizeof(history); i++) history_bytes[i] = 0;
    for (SIZE_T i = 0; i < sizeof(state); i++) dispatch_bytes[i] = 0;
    dispatch->TargetIp = 0;
    dispatch->ContextRecord = &walk;
    dispatch->HistoryTable = &history;
    state.Magic = UNWIND64_DISPATCH_STATE_MAGIC;

    ULONGLONG nested_frame = 0;
    for (DWORD depth = 0; depth < UNWIND64_MAX_DISPATCH_FRAMES; depth++) {
        if (!walk.Rip || walk.Rsp == stack_base)
            return STATUS_UNHANDLED_EXCEPTION;
        if (!unwind64_valid_stack_address(walk.Rsp, stack_limit,
                                          stack_base)) {
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }

        CONTEXT before = walk;
        ULONGLONG control_pc = unwind64_context_control_pc(&before);
        ULONGLONG image_base = 0;
        PRUNTIME_FUNCTION function =
            win32_unwind64_lookup_function_entry(
                control_pc, &image_base, &history);
        PVOID handler_data = NULL;
        ULONGLONG establisher_frame = 0;
        PVOID language_handler = win32_unwind64_virtual_unwind(
            UNW_FLAG_EHANDLER, image_base, control_pc, function, &walk,
            &handler_data, &establisher_frame, NULL);

        if (walk.Rip == before.Rip && walk.Rsp == before.Rsp) {
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }
        if (!unwind64_valid_stack_address(establisher_frame, stack_limit,
                                          stack_base) ||
            !unwind64_valid_stack_address(walk.Rsp, stack_limit,
                                          stack_base)) {
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }

        dispatch->ControlPc = control_pc;
        dispatch->ImageBase = image_base;
        dispatch->FunctionEntry = function;
        dispatch->EstablisherFrame = establisher_frame;
        dispatch->TargetIp = 0;
        dispatch->LanguageHandler =
            (PEXCEPTION_ROUTINE)language_handler;
        dispatch->HandlerData = handler_data;
        dispatch->ScopeIndex = 0;
        dispatch->Fill0 = 0;
        state.FrameContext = &before;
        state.NextContext = &walk;

        if (nested_frame && establisher_frame > nested_frame) {
            nested_frame = 0;
            record->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
        }
        if (!language_handler) continue;
        if (!unwind64_handler_executable(language_handler))
            return STATUS_INVALID_DISPOSITION;

        EXCEPTION_DISPOSITION disposition =
            win32_unwind64_call_exception_handler(
                record, (PVOID)(ULONG_PTR)establisher_frame,
                context, dispatch, dispatch->LanguageHandler, &before);
        if (dispatch->Fill0)
            return (NTSTATUS)dispatch->Fill0;
        if (nested_frame == establisher_frame) {
            nested_frame = 0;
            record->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
        }

        switch (disposition) {
        case ExceptionContinueExecution:
            if (!dispatch->TargetIp &&
                (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE))
                return STATUS_NONCONTINUABLE_EXCEPTION;
            if (!unwind64_valid_stack_address(context->Rsp, stack_limit,
                                              stack_base) ||
                !context->Rip ||
                !win32_user_range_executable(
                    (const void *)(ULONG_PTR)context->Rip, 1, FALSE))
                return STATUS_INVALID_DISPOSITION;
            return STATUS_SUCCESS;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
            if (!dispatch->EstablisherFrame)
                return STATUS_INVALID_DISPOSITION;
            nested_frame = dispatch->EstablisherFrame;
            record->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            break;
        case ExceptionCollidedUnwind:
        default:
            return STATUS_INVALID_DISPOSITION;
        }
    }

    record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
    return STATUS_BAD_STACK;
}

void win32_unwind64_release_process(DWORD process_id)
{
    if (!process_id) return;
    for (DWORD i = 0; i < UNWIND64_DYNAMIC_TABLES; i++) {
        UNWIND64_DYNAMIC_TABLE *record = &dynamic_tables[i];
        if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
                UNWIND64_TABLE_ACTIVE ||
            record->owner_pid != process_id)
            continue;
        DWORD expected = UNWIND64_TABLE_ACTIVE;
        if (!__atomic_compare_exchange_n(
                &record->state, &expected, UNWIND64_TABLE_RESERVED, FALSE,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        record->owner_pid = 0;
        record->table = NULL;
        record->count = 0;
        record->base = 0;
        record->range_start = 0;
        record->range_end = 0;
        __atomic_store_n(&record->state, UNWIND64_TABLE_FREE,
                         __ATOMIC_RELEASE);
    }
}
