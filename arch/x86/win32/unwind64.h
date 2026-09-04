#ifndef WIN32_UNWIND64_H
#define WIN32_UNWIND64_H

#include "nttypes.h"

BOOL win32_unwind64_add_function_table(PRUNTIME_FUNCTION function_table,
                                       DWORD entry_count,
                                       ULONGLONG base_address);
BOOL win32_unwind64_delete_function_table(PRUNTIME_FUNCTION function_table);
PRUNTIME_FUNCTION win32_unwind64_lookup_function_entry(
    ULONGLONG control_pc, ULONGLONG *image_base,
    PUNWIND_HISTORY_TABLE history_table);
PVOID win32_unwind64_virtual_unwind(
    DWORD handler_type, ULONGLONG image_base, ULONGLONG control_pc,
    PRUNTIME_FUNCTION function_entry, PCONTEXT context,
    PVOID *handler_data, ULONGLONG *establisher_frame,
    PKNONVOLATILE_CONTEXT_POINTERS context_pointers);
NTSTATUS win32_unwind64_dispatch_exception(PEXCEPTION_RECORD record,
                                           PCONTEXT context);
NTSTATUS win32_unwind64_unwind_ex(
    PEXCEPTION_RECORD record, PCONTEXT context, PVOID target_frame,
    PVOID target_ip, PVOID return_value,
    PUNWIND_HISTORY_TABLE history_table);
EXCEPTION_DISPOSITION WINAPI win32_unwind64_c_specific_handler(
    PEXCEPTION_RECORD record, PVOID establisher_frame, PCONTEXT context,
    PDISPATCHER_CONTEXT dispatcher);
void win32_unwind64_release_process(DWORD process_id);

#endif
