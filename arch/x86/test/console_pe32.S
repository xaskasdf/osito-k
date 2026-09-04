/* Freestanding PE32 regression for per-process console association state. */

    .text
    .globl _mainCRTStartup

_mainCRTStartup:
    pushl $-11
    calll *__imp__GetStdHandle@4
    cmpl $8, %eax
    jne _fail_initial_handle
    calll _check_attached_parameters
    testl %eax, %eax
    jz _fail_initial_parameters

    calll *__imp__AllocConsole@0
    testl %eax, %eax
    jnz _fail_duplicate_alloc
    calll *__imp__GetLastError@0
    cmpl $5, %eax
    jne _fail_duplicate_alloc_error

    calll *__imp__FreeConsole@0
    testl %eax, %eax
    jz _fail_free
    pushl $-11
    calll *__imp__GetStdHandle@4
    testl %eax, %eax
    jnz _fail_detached_handle
    calll _check_detached_parameters
    testl %eax, %eax
    jz _fail_detached_parameters

    calll *__imp__FreeConsole@0
    testl %eax, %eax
    jnz _fail_duplicate_free
    calll *__imp__GetLastError@0
    cmpl $87, %eax
    jne _fail_duplicate_free_error

    calll *__imp__AllocConsole@0
    testl %eax, %eax
    jz _fail_alloc
    calll _check_attached_parameters
    testl %eax, %eax
    jz _fail_reallocated_parameters

    pushl $0x1234
    pushl $-11
    calll *__imp__SetStdHandle@8
    testl %eax, %eax
    jz _fail_set_handle
    pushl $-11
    calll *__imp__GetStdHandle@4
    cmpl $0x1234, %eax
    jne _fail_get_redirected
    calll _check_redirected_parameters
    testl %eax, %eax
    jz _fail_redirected_parameters

    calll *__imp__FreeConsole@0
    testl %eax, %eax
    jz _fail_redirected_free
    pushl $-11
    calll *__imp__GetStdHandle@4
    cmpl $0x1234, %eax
    jne _fail_redirected_preservation
    calll _check_detached_redirected_parameters
    testl %eax, %eax
    jz _fail_detached_redirected_parameters

    calll *__imp__AllocConsole@0
    testl %eax, %eax
    jz _fail_final_alloc
    calll _check_attached_parameters
    testl %eax, %eax
    jz _fail_final_parameters

    pushl $5
    pushl $_atomic_value
    calll *__imp__InterlockedExchangeAdd@8
    cmpl $7, %eax
    jne _fail_exchange_add_return
    cmpl $12, _atomic_value
    jne _fail_exchange_add_value

    /* A module snapshot must start with the executable and expose the PE32
     * MODULEENTRY32 layout, including matching base/HMODULE values. */
    pushl $0
    pushl $0x8
    calll *__imp__CreateToolhelp32Snapshot@8
    cmpl $-1, %eax
    je _fail_snapshot_create
    movl %eax, _snapshot_handle

    movl $548, _module_entry
    pushl $_module_entry
    pushl %eax
    calll *__imp__Module32First@8
    testl %eax, %eax
    jz _fail_module_first
    cmpl $548, _module_entry
    jne _fail_module_size
    calll *__imp__GetCurrentProcessId@0
    cmpl %eax, _module_entry+8
    jne _fail_module_pid
    movl %fs:0x30, %ecx
    movl 0x8(%ecx), %ecx
    cmpl %ecx, _module_entry+20
    jne _fail_module_base
    cmpl %ecx, _module_entry+28
    jne _fail_module_handle
    cmpl $0, _module_entry+24
    je _fail_module_image_size
    cmpb $0, _module_entry+32
    je _fail_module_name
    cmpb $67, _module_entry+288
    jne _fail_module_path

    pushl _snapshot_handle
    calll *__imp__CloseHandle@4
    testl %eax, %eax
    jz _fail_snapshot_close

    /* MapViewOfFileEx must honor an available, allocation-granularity-aligned
     * address instead of silently selecting another mapping base. */
    pushl $0
    pushl $4096
    pushl $0
    pushl $4
    pushl $0
    pushl $-1
    calll *__imp__CreateFileMappingA@24
    testl %eax, %eax
    jz _fail_mapping_create
    movl %eax, _mapping_handle

    movl $0xffffffff, _handle_flags
    pushl $_handle_flags
    pushl _mapping_handle
    calll *__imp__GetHandleInformation@8
    testl %eax, %eax
    jz _fail_handle_info_default
    cmpl $0, _handle_flags
    jne _fail_handle_info_default

    pushl $1
    pushl $1
    pushl _mapping_handle
    calll *__imp__SetHandleInformation@12
    testl %eax, %eax
    jz _fail_handle_info_set
    pushl $_handle_flags
    pushl _mapping_handle
    calll *__imp__GetHandleInformation@8
    testl %eax, %eax
    jz _fail_handle_info_set
    cmpl $1, _handle_flags
    jne _fail_handle_info_set

    pushl $2
    pushl $2
    pushl _mapping_handle
    calll *__imp__SetHandleInformation@12
    testl %eax, %eax
    jz _fail_handle_info_protect
    pushl _mapping_handle
    calll *__imp__CloseHandle@4
    testl %eax, %eax
    jnz _fail_handle_info_protect
    calll *__imp__GetLastError@0
    cmpl $6, %eax
    jne _fail_handle_info_protect
    pushl $_handle_flags
    pushl _mapping_handle
    calll *__imp__GetHandleInformation@8
    testl %eax, %eax
    jz _fail_handle_info_protect
    cmpl $3, _handle_flags
    jne _fail_handle_info_protect

    pushl $0
    pushl $2
    pushl _mapping_handle
    calll *__imp__SetHandleInformation@12
    testl %eax, %eax
    jz _fail_handle_info_clear
    pushl $_handle_flags
    pushl _mapping_handle
    calll *__imp__GetHandleInformation@8
    testl %eax, %eax
    jz _fail_handle_info_clear
    cmpl $1, _handle_flags
    jne _fail_handle_info_clear

    movl $0x77777777, _handle_flags
    pushl $_handle_flags
    pushl $0x7ffffffc
    calll *__imp__GetHandleInformation@8
    testl %eax, %eax
    jnz _fail_handle_info_invalid
    calll *__imp__GetLastError@0
    cmpl $6, %eax
    jne _fail_handle_info_invalid
    cmpl $0x77777777, _handle_flags
    jne _fail_handle_info_invalid

    /* Thread contexts are not writable until the scheduler can transfer a
     * suspended PE register set. The contract must still distinguish bad
     * arguments, invalid handles, and an unsupported live-context update. */
    pushl $0
    pushl $-2
    calll *__imp__SetThreadContext@8
    testl %eax, %eax
    jnz _fail_thread_context_null
    calll *__imp__GetLastError@0
    cmpl $87, %eax
    jne _fail_thread_context_null

    pushl $_thread_context
    pushl $0x7ffffffc
    calll *__imp__SetThreadContext@8
    testl %eax, %eax
    jnz _fail_thread_context_invalid
    calll *__imp__GetLastError@0
    cmpl $6, %eax
    jne _fail_thread_context_invalid

    pushl $_thread_context
    pushl $-2
    calll *__imp__SetThreadContext@8
    testl %eax, %eax
    jnz _fail_thread_context_unsupported
    calll *__imp__GetLastError@0
    cmpl $50, %eax
    jne _fail_thread_context_unsupported

    pushl $0x50000000
    pushl $4096
    pushl $0
    pushl $0
    pushl $2
    pushl _mapping_handle
    calll *__imp__MapViewOfFileEx@24
    cmpl $0x50000000, %eax
    jne _fail_mapping_base
    movl $0x4D41504D, (%eax)
    cmpl $0x4D41504D, (%eax)
    jne _fail_mapping_write

    pushl %eax
    calll *__imp__UnmapViewOfFile@4
    testl %eax, %eax
    jz _fail_mapping_unmap
    pushl _mapping_handle
    calll *__imp__CloseHandle@4
    testl %eax, %eax
    jz _fail_mapping_close

    /* HotSpot checks UOI_FLAGS on the process window station before bringing
     * up AWT. The PE32 structure is three DWORDs and WinSta0 is interactive. */
    calll *__imp__GetProcessWindowStation@0
    testl %eax, %eax
    jz _fail_window_station
    movl %eax, _window_station
    movl $0, _user_object_needed
    pushl $_user_object_needed
    pushl $12
    pushl $_user_object_flags
    pushl $1
    pushl %eax
    calll *__imp__GetUserObjectInformationA@20
    testl %eax, %eax
    jz _fail_user_flags
    cmpl $12, _user_object_needed
    jne _fail_user_flags_size
    cmpl $0, _user_object_flags
    jne _fail_user_flags_inherit
    cmpl $0, _user_object_flags+4
    jne _fail_user_flags_reserved
    testl $1, _user_object_flags+8
    jz _fail_user_flags_visible

    /* A size probe must report the terminating NUL and fail with the normal
     * Win32 insufficient-buffer error. */
    movl $0, _user_object_needed
    pushl $_user_object_needed
    pushl $0
    pushl $0
    pushl $2
    pushl _window_station
    calll *__imp__GetUserObjectInformationA@20
    testl %eax, %eax
    jnz _fail_user_name_probe
    calll *__imp__GetLastError@0
    cmpl $122, %eax
    jne _fail_user_name_probe_error
    cmpl $8, _user_object_needed
    jne _fail_user_name_probe_size

    pushl $_user_object_needed
    pushl $32
    pushl $_user_object_name_a
    pushl $2
    pushl _window_station
    calll *__imp__GetUserObjectInformationA@20
    testl %eax, %eax
    jz _fail_user_name_a
    cmpl $0x536E6957, _user_object_name_a
    jne _fail_user_name_a
    cmpl $0x00306174, _user_object_name_a+4
    jne _fail_user_name_a

    pushl $_user_object_needed
    pushl $64
    pushl $_user_object_name_w
    pushl $2
    pushl _window_station
    calll *__imp__GetUserObjectInformationW@20
    testl %eax, %eax
    jz _fail_user_name_w
    cmpl $16, _user_object_needed
    jne _fail_user_name_w_size
    cmpw $87, _user_object_name_w
    jne _fail_user_name_w
    cmpw $48, _user_object_name_w+12
    jne _fail_user_name_w
    cmpw $0, _user_object_name_w+14
    jne _fail_user_name_w

    pushl $_user_object_needed
    pushl $32
    pushl $_user_object_type_a
    pushl $3
    pushl _window_station
    calll *__imp__GetUserObjectInformationA@20
    testl %eax, %eax
    jz _fail_user_type_a
    cmpl $14, _user_object_needed
    jne _fail_user_type_size
    cmpl $0x646E6957, _user_object_type_a
    jne _fail_user_type_a

    /* Exercise ACL layout and pointer-output behavior through PE32 thunks. */
    pushl $2
    pushl $64
    pushl $_test_acl
    calll *__imp__InitializeAcl@12
    testl %eax, %eax
    jz _fail_acl_initialize

    pushl $_world_sid
    pushl $0x1234
    pushl $2
    pushl $_test_acl
    calll *__imp__AddAccessAllowedAce@16
    testl %eax, %eax
    jz _fail_acl_add_allowed

    movl $0xA5A5A5A5, _ace_pointer_guard
    pushl $_ace_pointer
    pushl $0
    pushl $_test_acl
    calll *__imp__GetAce@12
    testl %eax, %eax
    jz _fail_acl_get_allowed
    leal _test_acl+8, %ecx
    cmpl %ecx, _ace_pointer
    jne _fail_acl_get_allowed_pointer
    cmpl $0xA5A5A5A5, _ace_pointer_guard
    jne _fail_acl_pointer_width
    movl _ace_pointer, %ecx
    cmpb $0, (%ecx)
    jne _fail_acl_get_allowed_data
    cmpw $20, 2(%ecx)
    jne _fail_acl_get_allowed_data
    cmpl $0x1234, 4(%ecx)
    jne _fail_acl_get_allowed_data

    pushl $2
    pushl $12
    pushl $_acl_size_info
    pushl $_test_acl
    calll *__imp__GetAclInformation@16
    testl %eax, %eax
    jz _fail_acl_size_info
    cmpl $1, _acl_size_info
    jne _fail_acl_size_info
    cmpl $28, _acl_size_info+4
    jne _fail_acl_size_info
    cmpl $36, _acl_size_info+8
    jne _fail_acl_size_info

    /* Generic AddAce inserts an opaque, structurally valid ACE at index 0. */
    pushl $20
    pushl $_deny_ace
    pushl $0
    pushl $2
    pushl $_test_acl
    calll *__imp__AddAce@20
    testl %eax, %eax
    jz _fail_acl_add_generic

    pushl $_ace_pointer
    pushl $0
    pushl $_test_acl
    calll *__imp__GetAce@12
    testl %eax, %eax
    jz _fail_acl_get_inserted
    movl _ace_pointer, %ecx
    cmpb $1, (%ecx)
    jne _fail_acl_get_inserted_data
    cmpl $0x5678, 4(%ecx)
    jne _fail_acl_get_inserted_data

    pushl $_ace_pointer
    pushl $1
    pushl $_test_acl
    calll *__imp__GetAce@12
    testl %eax, %eax
    jz _fail_acl_get_shifted
    leal _test_acl+28, %ecx
    cmpl %ecx, _ace_pointer
    jne _fail_acl_get_shifted
    cmpb $0, (%ecx)
    jne _fail_acl_get_shifted

    pushl $2
    pushl $12
    pushl $_acl_size_info
    pushl $_test_acl
    calll *__imp__GetAclInformation@16
    testl %eax, %eax
    jz _fail_acl_size_after_insert
    cmpl $2, _acl_size_info
    jne _fail_acl_size_after_insert
    cmpl $48, _acl_size_info+4
    jne _fail_acl_size_after_insert
    cmpl $16, _acl_size_info+8
    jne _fail_acl_size_after_insert

    /* SetFileSecurityA must persist a representable DACL instead of merely
     * reporting success. This self-relative descriptor grants read access to
     * Everyone, which maps exactly to mode 0444 on both OsitoFS versions. */
    pushl $_file_security_descriptor
    pushl $4
    pushl $_self_file_name
    calll *__imp__SetFileSecurityA@12
    testl %eax, %eax
    jz _fail_file_security_set

    pushl $_invalid_security_descriptor
    pushl $4
    pushl $_self_file_name
    calll *__imp__SetFileSecurityA@12
    testl %eax, %eax
    jnz _fail_file_security_invalid_descriptor
    calll *__imp__GetLastError@0
    cmpl $1338, %eax
    jne _fail_file_security_invalid_error

    pushl $_file_security_descriptor
    pushl $4
    pushl $_missing_file_name
    calll *__imp__SetFileSecurityA@12
    testl %eax, %eax
    jnz _fail_file_security_missing
    calll *__imp__GetLastError@0
    cmpl $2, %eax
    jne _fail_file_security_missing_error

    /* AllocateAndInitializeSid must publish a PE32-width pointer to a
     * standard SID allocation that FreeSid can release. */
    movl $0, _allocated_sid
    movl $0xA5A5A5A5, _allocated_sid_guard
    pushl $_allocated_sid
    pushl $0
    pushl $0
    pushl $0
    pushl $0
    pushl $0
    pushl $0
    pushl $545
    pushl $32
    pushl $2
    pushl $_nt_authority
    calll *__imp__AllocateAndInitializeSid@44
    testl %eax, %eax
    jz _fail_sid_allocate
    movl _allocated_sid, %ecx
    testl %ecx, %ecx
    jz _fail_sid_pointer
    cmpl $0xA5A5A5A5, _allocated_sid_guard
    jne _fail_sid_pointer_width
    cmpb $1, 0(%ecx)
    jne _fail_sid_layout
    cmpb $2, 1(%ecx)
    jne _fail_sid_layout
    cmpl $0, 2(%ecx)
    jne _fail_sid_layout
    cmpw $0x0500, 6(%ecx)
    jne _fail_sid_layout
    cmpl $32, 8(%ecx)
    jne _fail_sid_layout
    cmpl $545, 12(%ecx)
    jne _fail_sid_layout

    pushl %ecx
    calll *__imp__GetLengthSid@4
    cmpl $16, %eax
    jne _fail_sid_length

    pushl _allocated_sid
    calll *__imp__FreeSid@4
    testl %eax, %eax
    jnz _fail_sid_free

    /* Exercise the ANSI VERSION.dll surface through PE32 thunks. The
     * synthetic kernel32 version block is stable and contains fixed data. */
    movl $0xFFFFFFFF, _version_handle
    pushl $_version_handle
    pushl $_kernel32_module_name
    calll *__imp__GetFileVersionInfoSizeA@8
    testl %eax, %eax
    jz _fail_version_size
    cmpl $0, _version_handle
    jne _fail_version_handle
    cmpl $128, %eax
    ja _fail_version_size
    movl %eax, _version_size

    pushl $_version_blob
    pushl %eax
    pushl $0
    pushl $_kernel32_module_name
    calll *__imp__GetFileVersionInfoA@16
    testl %eax, %eax
    jz _fail_version_info
    cmpl $0xFEEF04BD, _version_blob
    jne _fail_version_data

    movl $0, _version_pointer
    movl $0xA5A5A5A5, _version_pointer_guard
    movl $0, _version_length
    pushl $_version_length
    pushl $_version_pointer
    pushl $_version_root_subblock
    pushl $_version_blob
    calll *__imp__VerQueryValueA@16
    testl %eax, %eax
    jz _fail_version_query
    leal _version_blob, %ecx
    cmpl %ecx, _version_pointer
    jne _fail_version_pointer
    cmpl $0xA5A5A5A5, _version_pointer_guard
    jne _fail_version_pointer_width
    cmpl $52, _version_length
    jne _fail_version_length

    /* SetFileSecurityA left this executable read-only. The MSVCR100 CRT view
     * must observe that mode, then _chmod must persist both transitions. */
    pushl $_crt_stat
    pushl $_self_file_name
    calll *__imp___stat64i32
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_stat_readonly
    movzwl _crt_stat+6, %eax
    cmpl $0x8100, %eax
    jne _fail_crt_mode_readonly

    pushl $0x80
    pushl $_self_file_name
    calll *__imp___chmod
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_chmod_write

    pushl $_crt_stat
    pushl $_self_file_name
    calll *__imp___stat64i32
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_stat_writable
    movzwl _crt_stat+6, %eax
    cmpl $0x8180, %eax
    jne _fail_crt_mode_writable

    pushl $0x100
    pushl $_self_file_name
    calll *__imp___chmod
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_chmod_restore

    pushl $_crt_stat
    pushl $_self_file_name
    calll *__imp___stat64i32
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_stat_restored
    movzwl _crt_stat+6, %eax
    cmpl $0x8100, %eax
    jne _fail_crt_mode_restored

    /* _unlink delegates to Win32 deletion while preserving the CRT errno
     * contract for read-only and missing paths. */
    pushl $0x80
    pushl $_unlink_test_name
    calll *__imp__SetFileAttributesA@8
    pushl $_unlink_test_name
    calll *__imp___unlink
    addl $4, %esp

    pushl $0x180
    pushl $0x301
    pushl $_unlink_test_name
    calll *__imp___open
    addl $12, %esp
    testl %eax, %eax
    js _fail_unlink_create
    pushl %eax
    calll *__imp___close
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_unlink_close

    pushl $1
    pushl $_unlink_test_name
    calll *__imp__SetFileAttributesA@8
    testl %eax, %eax
    jz _fail_unlink_readonly_set
    pushl $_unlink_test_name
    calll *__imp__GetFileAttributesA@4
    cmpl $1, %eax
    jne _fail_unlink_readonly_get

    pushl $_unlink_test_name
    calll *__imp___unlink
    addl $4, %esp
    cmpl $-1, %eax
    jne _fail_unlink_readonly_delete
    calll *__imp___errno
    cmpl $13, (%eax)
    jne _fail_unlink_readonly_errno

    pushl $0x80
    pushl $_unlink_test_name
    calll *__imp__SetFileAttributesA@8
    testl %eax, %eax
    jz _fail_unlink_writable_set
    pushl $_unlink_test_name
    calll *__imp___unlink
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_unlink_delete

    pushl $_unlink_test_name
    calll *__imp__GetFileAttributesA@4
    cmpl $-1, %eax
    jne _fail_unlink_still_exists
    pushl $_unlink_test_name
    calll *__imp___unlink
    addl $4, %esp
    cmpl $-1, %eax
    jne _fail_unlink_missing
    calll *__imp___errno
    cmpl $2, (%eax)
    jne _fail_unlink_missing_errno

    pushl $_unlink_test_name_w
    calll *__imp___wunlink
    addl $4, %esp
    cmpl $-1, %eax
    jne _fail_wunlink_missing
    calll *__imp___errno
    cmpl $2, (%eax)
    jne _fail_wunlink_errno

    pushl $_unlink_test_name
    calll *__imp__remove
    addl $4, %esp
    cmpl $-1, %eax
    jne _fail_remove_missing
    calll *__imp___errno
    cmpl $2, (%eax)
    jne _fail_remove_errno

    pushl $_unlink_test_name_w
    calll *__imp___wremove
    addl $4, %esp
    cmpl $-1, %eax
    jne _fail_wremove_missing
    calll *__imp___errno
    cmpl $2, (%eax)
    jne _fail_wremove_errno

    /* rename must preserve the MSVCRT no-replace rule while delegating the
     * actual metadata transaction to Win32/OsitoFS. */
    pushl $_rename_source
    calll *__imp___unlink
    addl $4, %esp
    pushl $_rename_target
    calll *__imp___unlink
    addl $4, %esp

    pushl $0x180
    pushl $0x301
    pushl $_rename_source
    calll *__imp___open
    addl $12, %esp
    testl %eax, %eax
    js _fail_rename_create
    pushl %eax
    calll *__imp___close
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_rename_close

    pushl $_rename_target
    pushl $_rename_source
    calll *__imp__rename
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_rename_move
    pushl $_rename_source
    calll *__imp__GetFileAttributesA@4
    cmpl $-1, %eax
    jne _fail_rename_source_exists
    pushl $_rename_target
    calll *__imp__GetFileAttributesA@4
    cmpl $-1, %eax
    je _fail_rename_target_missing

    pushl $0x180
    pushl $0x301
    pushl $_rename_source
    calll *__imp___open
    addl $12, %esp
    testl %eax, %eax
    js _fail_rename_collision_create
    pushl %eax
    calll *__imp___close
    addl $4, %esp

    pushl $_rename_target
    pushl $_rename_source
    calll *__imp__rename
    addl $8, %esp
    cmpl $-1, %eax
    jne _fail_rename_collision
    calll *__imp___errno
    cmpl $13, (%eax)
    jne _fail_rename_collision_errno

    pushl $_rename_source
    calll *__imp___unlink
    addl $4, %esp
    pushl $_rename_target
    calll *__imp___unlink
    addl $4, %esp
    pushl $_rename_target
    pushl $_rename_source
    calll *__imp__rename
    addl $8, %esp
    cmpl $-1, %eax
    jne _fail_rename_missing
    calll *__imp___errno
    cmpl $2, (%eax)
    jne _fail_rename_missing_errno

    pushl $_rename_target_w
    pushl $_rename_source_w
    calll *__imp___wrename
    addl $8, %esp
    cmpl $-1, %eax
    jne _fail_wrename_missing
    calll *__imp___errno
    cmpl $2, (%eax)
    jne _fail_wrename_errno

    calll *__imp___getdrive
    cmpl $3, %eax
    jne _fail_getdrive

    /* _wcserror returns the process CRT's message in a thread-owned wide
     * buffer and provides the canonical unknown-error fallback. */
    pushl $2
    calll *__imp___wcserror
    addl $4, %esp
    testl %eax, %eax
    jz _fail_wcserror
    cmpw $'N', 0(%eax)
    jne _fail_wcserror
    cmpw $' ', 4(%eax)
    jne _fail_wcserror
    cmpw $'y', 48(%eax)
    jne _fail_wcserror
    cmpw $0, 50(%eax)
    jne _fail_wcserror
    pushl $999
    calll *__imp___wcserror
    addl $4, %esp
    testl %eax, %eax
    jz _fail_wcserror
    cmpw $'U', 0(%eax)
    jne _fail_wcserror
    cmpw $' ', 14(%eax)
    jne _fail_wcserror
    cmpw $'r', 24(%eax)
    jne _fail_wcserror
    cmpw $0, 26(%eax)
    jne _fail_wcserror

    /* The default and explicit C: drive share the process CWD. Both narrow
     * and wide variants support caller-owned and CRT-allocated buffers. */
    pushl $260
    pushl $_getdcwd_buffer
    pushl $3
    calll *__imp___getdcwd
    addl $12, %esp
    cmpl $_getdcwd_buffer, %eax
    jne _fail_getdcwd
    cmpb $'C', 0(%eax)
    jne _fail_getdcwd
    cmpb $':', 1(%eax)
    jne _fail_getdcwd
    cmpb $'\\', 2(%eax)
    jne _fail_getdcwd

    pushl $260
    pushl $_wgetdcwd_buffer
    pushl $0
    calll *__imp___wgetdcwd
    addl $12, %esp
    cmpl $_wgetdcwd_buffer, %eax
    jne _fail_wgetdcwd
    cmpw $'C', 0(%eax)
    jne _fail_wgetdcwd
    cmpw $':', 2(%eax)
    jne _fail_wgetdcwd
    cmpw $'\\', 4(%eax)
    jne _fail_wgetdcwd

    pushl $260
    pushl $0
    pushl $3
    calll *__imp___wgetdcwd
    addl $12, %esp
    testl %eax, %eax
    jz _fail_wgetdcwd_alloc
    cmpw $'C', 0(%eax)
    jne _fail_wgetdcwd_alloc
    pushl %eax
    calll *__imp__free
    addl $4, %esp

    pushl $260
    pushl $_wgetdcwd_buffer
    pushl $4
    calll *__imp___wgetdcwd
    addl $12, %esp
    testl %eax, %eax
    jnz _fail_wgetdcwd_invalid
    calll *__imp___errno
    cmpl $22, (%eax)
    jne _fail_wgetdcwd_invalid

    pushl $2
    pushl $_wgetdcwd_buffer
    pushl $3
    calll *__imp___wgetdcwd
    addl $12, %esp
    testl %eax, %eax
    jnz _fail_wgetdcwd_range
    calll *__imp___errno
    cmpl $34, (%eax)
    jne _fail_wgetdcwd_range

    /* The active C locale has one-byte multibyte characters. Conversion
     * counts exclude the terminator, size queries ignore count, and a
     * destination that fills exactly is intentionally not terminated. */
    pushl $4
    pushl $_mbstowcs_source
    pushl $_mbstowcs_buffer
    calll *__imp__mbstowcs
    addl $12, %esp
    cmpl $2, %eax
    jne _fail_mbstowcs
    cmpw $'A', _mbstowcs_buffer
    jne _fail_mbstowcs
    cmpw $'b', _mbstowcs_buffer+2
    jne _fail_mbstowcs
    cmpw $0, _mbstowcs_buffer+4
    jne _fail_mbstowcs

    pushl $0
    pushl $_mbstowcs_source
    pushl $0
    calll *__imp__mbstowcs
    addl $12, %esp
    cmpl $2, %eax
    jne _fail_mbstowcs_size

    movw $0x7777, _mbstowcs_buffer+2
    pushl $1
    pushl $_mbstowcs_source
    pushl $_mbstowcs_buffer
    calll *__imp__mbstowcs
    addl $12, %esp
    cmpl $1, %eax
    jne _fail_mbstowcs_truncate
    cmpw $0x7777, _mbstowcs_buffer+2
    jne _fail_mbstowcs_truncate

    pushl $4
    pushl $_wcstombs_source
    pushl $_wcstombs_buffer
    calll *__imp__wcstombs
    addl $12, %esp
    cmpl $2, %eax
    jne _fail_wcstombs
    cmpb $'A', _wcstombs_buffer
    jne _fail_wcstombs
    cmpb $'b', _wcstombs_buffer+1
    jne _fail_wcstombs
    cmpb $0, _wcstombs_buffer+2
    jne _fail_wcstombs

    pushl $0
    pushl $_wcstombs_source
    pushl $0
    calll *__imp__wcstombs
    addl $12, %esp
    cmpl $2, %eax
    jne _fail_wcstombs_size

    pushl $4
    pushl $_wcstombs_invalid
    pushl $_wcstombs_buffer
    calll *__imp__wcstombs
    addl $12, %esp
    cmpl $-1, %eax
    jne _fail_wcstombs_invalid
    calll *__imp___errno
    cmpl $42, (%eax)
    jne _fail_wcstombs_invalid

    /* _vswprintf_c_l receives an i386 va_list, where 64-bit integers and
     * doubles consume two DWORDs. Exercise both so a slot-width regression
     * cannot silently shift every argument that follows. */
    pushl $_vswprintf_args
    pushl $0
    pushl $_vswprintf_format
    pushl $64
    pushl $_vswprintf_buffer
    calll *__imp___vswprintf_c_l
    addl $20, %esp
    cmpl $30, %eax
    jne _fail_vswprintf
    cld
    movl $_vswprintf_buffer, %esi
    movl $_vswprintf_expected, %edi
    movl $31, %ecx
    repe cmpsw
    jne _fail_vswprintf

    /* A null destination with a zero count is the conforming size query. */
    pushl $_vswprintf_args
    pushl $0
    pushl $_vswprintf_format
    pushl $0
    pushl $0
    calll *__imp___vswprintf_c_l
    addl $20, %esp
    cmpl $30, %eax
    jne _fail_vswprintf_size

    /* Truncation reports -1, keeps a terminator in the available buffer,
     * and must not write past the caller's count. */
    movw $0x7777, _vswprintf_buffer+8
    pushl $_vswprintf_args
    pushl $0
    pushl $_vswprintf_format
    pushl $4
    pushl $_vswprintf_buffer
    calll *__imp___vswprintf_c_l
    addl $20, %esp
    cmpl $-1, %eax
    jne _fail_vswprintf_truncate
    cmpw $'o', _vswprintf_buffer
    jne _fail_vswprintf_truncate
    cmpw $'k', _vswprintf_buffer+2
    jne _fail_vswprintf_truncate
    cmpw $' ', _vswprintf_buffer+4
    jne _fail_vswprintf_truncate
    cmpw $0, _vswprintf_buffer+6
    jne _fail_vswprintf_truncate
    cmpw $0x7777, _vswprintf_buffer+8
    jne _fail_vswprintf_truncate

    /* A non-null destination with count zero is invalid and remains intact. */
    movw $0x7777, _vswprintf_buffer
    pushl $_vswprintf_args
    pushl $0
    pushl $_vswprintf_format
    pushl $0
    pushl $_vswprintf_buffer
    calll *__imp___vswprintf_c_l
    addl $20, %esp
    cmpl $-1, %eax
    jne _fail_vswprintf_zero
    cmpw $0x7777, _vswprintf_buffer
    jne _fail_vswprintf_zero

    /* Legacy MinGW binaries import the 32-bit _timeb contract directly. */
    movl $0x5a5a5a5a, _ftime_guard
    pushl $_ftime_buffer
    calll *__imp___ftime
    addl $4, %esp
    cmpl $0, _ftime_buffer
    jle _fail_ftime_time
    movzwl _ftime_buffer+4, %eax
    cmpl $1000, %eax
    jae _fail_ftime_millis
    cmpw $0, _ftime_buffer+6
    jne _fail_ftime_zone
    cmpw $0, _ftime_buffer+8
    jne _fail_ftime_zone
    cmpl $0x5a5a5a5a, _ftime_guard
    jne _fail_ftime_guard

    /* _iob is a data export, not a function. Each PE32 FILE is the 32-byte
     * MSVCRT layout and indexes the same backing descriptors as __iob_func. */
    movl __imp___iob, %eax
    testl %eax, %eax
    jz _fail_iob_data
    cmpl $0, 16(%eax)
    jne _fail_iob_layout
    cmpl $1, 48(%eax)
    jne _fail_iob_layout
    testl $2, 44(%eax)
    jz _fail_iob_layout
    movl %eax, _iob_base

    pushl $17
    pushl $_snwprintf_word
    pushl $_snwprintf_format
    pushl $32
    pushl $_snwprintf_buffer
    calll *__imp___snwprintf
    addl $20, %esp
    cmpl $7, %eax
    jne _fail_snwprintf
    cld
    movl $_snwprintf_buffer, %esi
    movl $_snwprintf_expected, %edi
    movl $8, %ecx
    repe cmpsw
    jne _fail_snwprintf

    /* strtok keeps a destructive cursor in the calling CRT thread state. */
    pushl $_strtok_delimiters
    pushl $_strtok_buffer
    calll *__imp__strtok
    addl $8, %esp
    cmpl $_strtok_buffer+1, %eax
    jne _fail_strtok

    pushl $_strtok_delimiters
    pushl $0
    calll *__imp__strtok
    addl $8, %esp
    cmpl $_strtok_buffer+8, %eax
    jne _fail_strtok

    pushl $_strtok_delimiters
    pushl $0
    calll *__imp__strtok
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_strtok

    pushl $_strtok_restart_delimiter
    pushl $_strtok_restart
    calll *__imp__strtok
    addl $8, %esp
    cmpl $_strtok_restart, %eax
    jne _fail_strtok

    pushl $_strtok_restart_delimiter
    pushl $0
    calll *__imp__strtok
    addl $8, %esp
    cmpl $_strtok_restart+6, %eax
    jne _fail_strtok

    movl $0xC0000080, _generic_access_mask
    pushl $_generic_mapping
    pushl $_generic_access_mask
    calll *__imp__MapGenericMask@8
    cmpl $0x00060083, _generic_access_mask
    jne _fail_map_generic_mask

    pushl $7
    pushl $_fwprintf_format
    movl _iob_base, %eax
    addl $64, %eax
    pushl %eax
    calll *__imp__fwprintf
    addl $12, %esp
    cmpl $5, %eax
    jne _fail_fwprintf

    /* net.dll imports gethostbyaddr by ordinal 51. Exercise the same import
     * form and verify that the returned hostent uses PE32 pointer widths. */
    pushl $_wsa_data
    pushl $0x0202
    calll *__imp__WSAStartup@8
    testl %eax, %eax
    jnz _fail_wsa_startup

    pushl $2
    pushl $4
    pushl $_loopback_address
    calll *__imp__gethostbyaddr@12
    testl %eax, %eax
    jz _fail_gethostbyaddr
    movl (%eax), %ecx
    testl %ecx, %ecx
    jz _fail_hostent_layout
    cmpl $0x61636f6c, (%ecx)
    jne _fail_hostent_layout
    cmpw $2, 8(%eax)
    jne _fail_hostent_layout
    cmpw $4, 10(%eax)
    jne _fail_hostent_layout
    movl 12(%eax), %ecx
    testl %ecx, %ecx
    jz _fail_hostent_layout
    movl (%ecx), %ecx
    testl %ecx, %ecx
    jz _fail_hostent_layout
    cmpl $0x0100007f, (%ecx)
    jne _fail_hostent_layout

    pushl $_disconnect_buffer
    pushl $-1
    calll *__imp__WSASendDisconnect@8
    cmpl $-1, %eax
    jne _fail_wsa_disconnect
    calll *__imp__WSAGetLastError@0
    cmpl $10038, %eax
    jne _fail_wsa_disconnect_error
    calll *__imp__WSACleanup@0
    testl %eax, %eax
    jnz _fail_wsa_cleanup

    /* Handle classification and _fstat64 must use the live NT object rather
     * than treating every descriptor as a disk file. */
    pushl $8
    calll *__imp__GetFileType@4
    cmpl $2, %eax
    jne _fail_file_type_console

    pushl $0x7ffffffc
    calll *__imp__GetFileType@4
    testl %eax, %eax
    jnz _fail_file_type_invalid
    calll *__imp__GetLastError@0
    cmpl $6, %eax
    jne _fail_file_type_invalid_error

    pushl $0
    pushl $0
    pushl $_self_file_name
    calll *__imp___open
    addl $12, %esp
    testl %eax, %eax
    js _fail_crt_fstat_open
    movl %eax, _crt_fstat_fd

    pushl %eax
    calll *__imp___get_osfhandle
    addl $4, %esp
    cmpl $-1, %eax
    je _fail_crt_fstat_handle
    pushl %eax
    calll *__imp__GetFileType@4
    cmpl $1, %eax
    jne _fail_crt_fstat_type

    movl $0xa5a5a5a5, _crt_fstat_guard
    pushl $_crt_fstat
    pushl _crt_fstat_fd
    calll *__imp___fstat64
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_fstat_query
    cmpl $0xa5a5a5a5, _crt_fstat_guard
    jne _fail_crt_fstat_guard
    movzwl _crt_fstat+6, %eax
    cmpl $0x8100, %eax
    jne _fail_crt_fstat_mode
    movl _crt_stat+20, %eax
    cmpl %eax, _crt_fstat+24
    jne _fail_crt_fstat_size
    cmpl $0, _crt_fstat+28
    jne _fail_crt_fstat_size
    movl _crt_stat+24, %eax
    cmpl %eax, _crt_fstat+32
    jne _fail_crt_fstat_time
    movl _crt_stat+28, %eax
    cmpl %eax, _crt_fstat+36
    jne _fail_crt_fstat_time
    movl _crt_stat+32, %eax
    cmpl %eax, _crt_fstat+40
    jne _fail_crt_fstat_time
    movl _crt_stat+36, %eax
    cmpl %eax, _crt_fstat+44
    jne _fail_crt_fstat_time
    movl _crt_stat+40, %eax
    cmpl %eax, _crt_fstat+48
    jne _fail_crt_fstat_time
    movl _crt_stat+44, %eax
    cmpl %eax, _crt_fstat+52
    jne _fail_crt_fstat_time

    pushl $_crt_mode_wb
    pushl _crt_fstat_fd
    calll *__imp___fdopen
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_crt_fdopen_mismatch

    pushl $_crt_mode_rb
    pushl _crt_fstat_fd
    calll *__imp___fdopen
    addl $8, %esp
    testl %eax, %eax
    jz _fail_crt_fdopen_read
    pushl %eax
    calll *__imp__fclose
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_crt_fstat_close
    pushl $_crt_fstat
    pushl _crt_fstat_fd
    calll *__imp___fstat64
    addl $8, %esp
    cmpl $-1, %eax
    jne _fail_crt_fstat_closed

    /* MSVCR100 exposes timezone/daylight both as writable data imports and
     * through accessors. They must address the same process-local storage. */
    movl __imp___timezone, %eax
    testl %eax, %eax
    jz _fail_crt_timezone_data
    movl %eax, _crt_timezone_pointer
    movl (%eax), %ecx
    movl %ecx, _crt_timezone_original
    movl $0x12345678, (%eax)
    calll *__imp____timezone
    cmpl _crt_timezone_pointer, %eax
    jne _fail_crt_timezone_accessor
    cmpl $0x12345678, (%eax)
    jne _fail_crt_timezone_write
    movl _crt_timezone_original, %ecx
    movl %ecx, (%eax)

    movl __imp___daylight, %eax
    testl %eax, %eax
    jz _fail_crt_daylight_data
    movl %eax, _crt_daylight_pointer
    movl (%eax), %ecx
    movl %ecx, _crt_daylight_original
    movl $0x23456789, (%eax)
    calll *__imp____daylight
    cmpl _crt_daylight_pointer, %eax
    jne _fail_crt_daylight_accessor
    cmpl $0x23456789, (%eax)
    jne _fail_crt_daylight_write
    movl _crt_daylight_original, %ecx
    movl %ecx, (%eax)

    /* MSVCR100's C++ filter only delegates Microsoft C++ exceptions. With no
     * CRT signal mapping installed, both paths continue the SEH search. */
    pushl $0
    pushl $0xC0000005
    calll *__imp____CppXcptFilter
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_cpp_filter_non_cpp

    pushl $0
    pushl $0xE06D7363
    calll *__imp____CppXcptFilter
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_cpp_filter_cpp

    /* _CI intrinsics exchange arguments and results through the x87 stack;
     * exp(0) is exact and catches accidental routing through the C gateway. */
    fldl _ci_exp_zero
    calll *__imp___CIexp
    fstpl _ci_exp_result
    cmpl $0, _ci_exp_result
    jne _fail_ci_exp
    cmpl $0x3ff00000, _ci_exp_result+4
    jne _fail_ci_exp

    fldl _ci_log10_one
    calll *__imp___CIlog10
    fstpl _ci_log10_result
    cmpl $0, _ci_log10_result
    jne _fail_ci_log10
    cmpl $0, _ci_log10_result+4
    jne _fail_ci_log10

    fldl _ci_sqrt_four
    calll *__imp___CIsqrt
    fstpl _ci_sqrt_result
    cmpl $0, _ci_sqrt_result
    jne _fail_ci_sqrt
    cmpl $0x40000000, _ci_sqrt_result+4
    jne _fail_ci_sqrt

    /* strtod returns through ST(0) in the i386 CRT ABI and stores a 32-bit
     * pointer through endptr. An incomplete exponent must not be consumed. */
    movl $0, _strtod_end
    pushl $_strtod_end
    pushl $_strtod_input
    calll *__imp__strtod
    addl $8, %esp
    fstpl _strtod_result
    cmpl $0, _strtod_result
    jne _fail_strtod_value
    cmpl $0xC0938800, _strtod_result+4
    jne _fail_strtod_value
    leal _strtod_input+8, %eax
    cmpl %eax, _strtod_end
    jne _fail_strtod_end

    movl $0, _strtod_end
    pushl $_strtod_end
    pushl $_strtod_rollback_input
    calll *__imp__strtod
    addl $8, %esp
    fstpl _strtod_result
    cmpl $0, _strtod_result
    jne _fail_strtod_rollback_value
    cmpl $0x3FF00000, _strtod_result+4
    jne _fail_strtod_rollback_value
    leal _strtod_rollback_input+1, %eax
    cmpl %eax, _strtod_end
    jne _fail_strtod_rollback_end

    /* _ctime64 uses the application CRT buffer and the fixed-width MSVC
     * representation. Negative timestamps are outside its documented range. */
    pushl $_ctime64_epoch
    calll *__imp___ctime64
    addl $4, %esp
    testl %eax, %eax
    jz _fail_ctime64_value
    cmpl $0x20756854, 0(%eax)
    jne _fail_ctime64_value
    cmpl $0x206E614A, 4(%eax)
    jne _fail_ctime64_value
    cmpl $0x30203120, 8(%eax)
    jne _fail_ctime64_value
    cmpl $0x30303A30, 12(%eax)
    jne _fail_ctime64_value
    cmpl $0x2030303A, 16(%eax)
    jne _fail_ctime64_value
    cmpl $0x30373931, 20(%eax)
    jne _fail_ctime64_value
    cmpw $0x000A, 24(%eax)
    jne _fail_ctime64_value

    pushl $_ctime64_negative
    calll *__imp___ctime64
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_ctime64_negative
    calll *__imp___errno
    cmpl $22, (%eax)
    jne _fail_ctime64_errno

    /* _finite receives the IEEE-754 value as two cdecl stack DWORDs. */
    pushl $0x3ff00000
    pushl $0
    calll *__imp___finite
    addl $8, %esp
    cmpl $1, %eax
    jne _fail_finite_value

    pushl $0x7ff00000
    pushl $0
    calll *__imp___finite
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_finite_infinity

    pushl $0x7ff80000
    pushl $0
    calll *__imp___finite
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_finite_nan

    /* signal/raise state is process-local. Delivery resets a user handler to
     * SIG_DFL before entering the PE32 callback; SIG_IGN remains persistent. */
    movl $0, _signal_count
    movl $0, _signal_seen
    pushl $_signal_handler
    pushl $15
    calll *__imp__signal
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_signal_install

    pushl $15
    calll *__imp__raise
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_signal_raise
    cmpl $1, _signal_count
    jne _fail_signal_count
    cmpl $15, _signal_seen
    jne _fail_signal_argument

    pushl $1
    pushl $15
    calll *__imp__signal
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_signal_reset
    pushl $15
    calll *__imp__raise
    addl $4, %esp
    testl %eax, %eax
    jnz _fail_signal_ignore
    cmpl $1, _signal_count
    jne _fail_signal_ignore

    pushl $0
    pushl $15
    calll *__imp__signal
    addl $8, %esp
    cmpl $1, %eax
    jne _fail_signal_restore
    pushl $999
    calll *__imp__raise
    addl $4, %esp
    cmpl $-1, %eax
    jne _fail_signal_invalid

    /* _environ is a writable data export whose cell and PE32 pointer array
     * stay stable while _putenv_s refreshes the process environment. */
    movl __imp___environ, %eax
    testl %eax, %eax
    jz _fail_environ_cell
    pushl %eax
    calll *__imp____p__environ
    cmpl (%esp), %eax
    popl %ecx
    jne _fail_environ_accessor
    movl (%eax), %esi
    testl %esi, %esi
    jz _fail_environ_entries
    movl (%esi), %edi
    testl %edi, %edi
    jz _fail_environ_entries

    pushl $_environment_value
    pushl $_environment_name
    calll *__imp___putenv_s
    addl $8, %esp
    testl %eax, %eax
    jnz _fail_environ_update

    movl __imp___environ, %eax
    movl (%eax), %esi
1:
    movl (%esi), %edi
    testl %edi, %edi
    jz _fail_environ_refresh
    cmpl $0x5449534f, 0(%edi)
    jne 2f
    cmpl $0x45545f4f, 4(%edi)
    jne 2f
    cmpl $0x455f5453, 8(%edi)
    jne 2f
    cmpl $0x723d564e, 12(%edi)
    jne 2f
    cmpl $0x79646165, 16(%edi)
    jne 2f
    cmpb $0, 20(%edi)
    je 3f
2:
    addl $4, %esi
    jmp 1b
3:

    /* MSVCR100 RTTI names are tracked as a PE32 mem_ptr/next list. Build two
     * real heap nodes so the cleanup contract exercises pointer width,
     * ownership, root detachment, and repeat-call idempotence. */
    pushl $16
    calll *__imp__malloc
    addl $4, %esp
    testl %eax, %eax
    jz _fail_rtti_name1_alloc
    movl %eax, _rtti_name1

    pushl $8
    calll *__imp__malloc
    addl $4, %esp
    testl %eax, %eax
    jz _fail_rtti_node1_alloc
    movl %eax, _rtti_node1

    pushl $16
    calll *__imp__malloc
    addl $4, %esp
    testl %eax, %eax
    jz _fail_rtti_name2_alloc
    movl %eax, _rtti_name2

    pushl $8
    calll *__imp__malloc
    addl $4, %esp
    testl %eax, %eax
    jz _fail_rtti_node2_alloc
    movl %eax, _rtti_node2

    movl _rtti_node2, %ecx
    movl _rtti_name2, %eax
    movl %eax, 0(%ecx)
    movl $0, 4(%ecx)
    movl _rtti_node1, %edx
    movl _rtti_name1, %eax
    movl %eax, 0(%edx)
    movl %ecx, 4(%edx)
    movl $0x11223344, _rtti_root
    movl %edx, _rtti_root+4
    movl $0xA5A5A5A5, _rtti_root_guard

    pushl $_rtti_root
    calll *__imp____clean_type_info_names_internal
    addl $4, %esp
    cmpl $0, _rtti_root+4
    jne _fail_rtti_root_detach
    cmpl $0x11223344, _rtti_root
    jne _fail_rtti_root_data
    cmpl $0xA5A5A5A5, _rtti_root_guard
    jne _fail_rtti_root_guard

    pushl $_rtti_root
    calll *__imp____clean_type_info_names_internal
    addl $4, %esp
    cmpl $0, _rtti_root+4
    jne _fail_rtti_repeat_cleanup

    /* The internal method is thiscall: ECX owns the type_info object and no
     * stack argument is consumed. Its cached name must be released and reset. */
    pushl $16
    calll *__imp__malloc
    addl $4, %esp
    testl %eax, %eax
    jz _fail_rtti_dtor_name_alloc
    movl $0, _rtti_object
    movl %eax, _rtti_object+4
    movl $0x5A5A5A5A, _rtti_object_guard
    movl %esp, _rtti_stack_before
    leal _rtti_object, %ecx
    calll *"__imp_?_type_info_dtor_internal_method@type_info@@QAEXXZ"
    cmpl _rtti_stack_before, %esp
    jne _fail_rtti_dtor_stack
    cmpl $0, _rtti_object+4
    jne _fail_rtti_dtor_name
    cmpl $0x5A5A5A5A, _rtti_object_guard
    jne _fail_rtti_dtor_guard

    leal _rtti_object, %ecx
    calll *"__imp_?_type_info_dtor_internal_method@type_info@@QAEXXZ"
    cmpl _rtti_stack_before, %esp
    jne _fail_rtti_dtor_repeat

    pushl $0
    calll *__imp__ExitProcess@4

_signal_handler:
    movl 4(%esp), %eax
    movl %eax, _signal_seen
    incl _signal_count
    retl

_check_attached_parameters:
    movl %fs:0x30, %eax
    movl 0x10(%eax), %eax
    cmpl $3, 0x10(%eax)
    jne 1f
    cmpl $4, 0x18(%eax)
    jne 1f
    cmpl $8, 0x1c(%eax)
    jne 1f
    cmpl $12, 0x20(%eax)
    jne 1f
    movl $1, %eax
    retl
1:
    xorl %eax, %eax
    retl

_check_detached_parameters:
    movl %fs:0x30, %eax
    movl 0x10(%eax), %eax
    cmpl $0, 0x10(%eax)
    jne 1f
    cmpl $0, 0x18(%eax)
    jne 1f
    cmpl $0, 0x1c(%eax)
    jne 1f
    cmpl $0, 0x20(%eax)
    jne 1f
    movl $1, %eax
    retl
1:
    xorl %eax, %eax
    retl

_check_redirected_parameters:
    movl %fs:0x30, %eax
    movl 0x10(%eax), %eax
    cmpl $3, 0x10(%eax)
    jne 1f
    cmpl $0x1234, 0x1c(%eax)
    jne 1f
    movl $1, %eax
    retl
1:
    xorl %eax, %eax
    retl

_check_detached_redirected_parameters:
    movl %fs:0x30, %eax
    movl 0x10(%eax), %eax
    cmpl $0, 0x10(%eax)
    jne 1f
    cmpl $0x1234, 0x1c(%eax)
    jne 1f
    movl $1, %eax
    retl
1:
    xorl %eax, %eax
    retl

_fail_initial_handle:                  pushl $10; jmp _exit
_fail_initial_parameters:              pushl $11; jmp _exit
_fail_duplicate_alloc:                 pushl $12; jmp _exit
_fail_duplicate_alloc_error:           pushl $13; jmp _exit
_fail_free:                            pushl $14; jmp _exit
_fail_detached_handle:                 pushl $15; jmp _exit
_fail_detached_parameters:             pushl $16; jmp _exit
_fail_duplicate_free:                  pushl $17; jmp _exit
_fail_duplicate_free_error:            pushl $18; jmp _exit
_fail_alloc:                           pushl $19; jmp _exit
_fail_reallocated_parameters:          pushl $20; jmp _exit
_fail_set_handle:                      pushl $21; jmp _exit
_fail_get_redirected:                  pushl $22; jmp _exit
_fail_redirected_parameters:           pushl $23; jmp _exit
_fail_redirected_free:                 pushl $24; jmp _exit
_fail_redirected_preservation:         pushl $25; jmp _exit
_fail_detached_redirected_parameters:  pushl $26; jmp _exit
_fail_final_alloc:                     pushl $27; jmp _exit
_fail_final_parameters:                pushl $28; jmp _exit
_fail_exchange_add_return:             pushl $29; jmp _exit
_fail_exchange_add_value:              pushl $30; jmp _exit
_fail_snapshot_create:                 pushl $31; jmp _exit
_fail_module_first:                    pushl $32; jmp _exit
_fail_module_size:                     pushl $33; jmp _exit
_fail_module_pid:                      pushl $34; jmp _exit
_fail_module_base:                     pushl $35; jmp _exit
_fail_module_handle:                   pushl $36; jmp _exit
_fail_module_image_size:               pushl $37; jmp _exit
_fail_module_name:                     pushl $38; jmp _exit
_fail_module_path:                     pushl $39; jmp _exit
_fail_snapshot_close:                  pushl $40; jmp _exit
_fail_mapping_create:                  pushl $41; jmp _exit
_fail_mapping_base:                    pushl $42; jmp _exit
_fail_mapping_write:                   pushl $43; jmp _exit
_fail_mapping_unmap:                   pushl $44; jmp _exit
_fail_mapping_close:                   pushl $45; jmp _exit
_fail_window_station:                  pushl $46; jmp _exit
_fail_user_flags:                      pushl $47; jmp _exit
_fail_user_flags_size:                 pushl $48; jmp _exit
_fail_user_flags_inherit:              pushl $49; jmp _exit
_fail_user_flags_reserved:             pushl $50; jmp _exit
_fail_user_flags_visible:              pushl $51; jmp _exit
_fail_user_name_probe:                 pushl $52; jmp _exit
_fail_user_name_probe_error:           pushl $53; jmp _exit
_fail_user_name_probe_size:            pushl $54; jmp _exit
_fail_user_name_a:                     pushl $55; jmp _exit
_fail_user_name_w:                     pushl $56; jmp _exit
_fail_user_name_w_size:                pushl $57; jmp _exit
_fail_user_type_a:                     pushl $58; jmp _exit
_fail_user_type_size:                  pushl $59; jmp _exit
_fail_acl_initialize:                  pushl $60; jmp _exit
_fail_acl_add_allowed:                 pushl $61; jmp _exit
_fail_acl_get_allowed:                 pushl $62; jmp _exit
_fail_acl_get_allowed_pointer:         pushl $63; jmp _exit
_fail_acl_pointer_width:               pushl $64; jmp _exit
_fail_acl_get_allowed_data:            pushl $65; jmp _exit
_fail_acl_size_info:                   pushl $66; jmp _exit
_fail_acl_add_generic:                 pushl $67; jmp _exit
_fail_acl_get_inserted:                pushl $68; jmp _exit
_fail_acl_get_inserted_data:           pushl $69; jmp _exit
_fail_acl_get_shifted:                 pushl $70; jmp _exit
_fail_acl_size_after_insert:           pushl $71; jmp _exit
_fail_file_security_set:                pushl $72; jmp _exit
_fail_file_security_invalid_descriptor: pushl $73; jmp _exit
_fail_file_security_invalid_error:      pushl $74; jmp _exit
_fail_file_security_missing:            pushl $75; jmp _exit
_fail_file_security_missing_error:      pushl $76; jmp _exit
_fail_sid_allocate:                     pushl $77; jmp _exit
_fail_sid_pointer:                      pushl $78; jmp _exit
_fail_sid_pointer_width:                pushl $79; jmp _exit
_fail_sid_layout:                       pushl $80; jmp _exit
_fail_sid_length:                       pushl $81; jmp _exit
_fail_sid_free:                         pushl $82; jmp _exit
_fail_version_size:                     pushl $83; jmp _exit
_fail_version_handle:                   pushl $84; jmp _exit
_fail_version_info:                     pushl $85; jmp _exit
_fail_version_data:                     pushl $86; jmp _exit
_fail_version_query:                    pushl $87; jmp _exit
_fail_version_pointer:                  pushl $88; jmp _exit
_fail_version_pointer_width:            pushl $89; jmp _exit
_fail_version_length:                   pushl $90; jmp _exit
_fail_crt_stat_readonly:                pushl $91; jmp _exit
_fail_crt_mode_readonly:                pushl $92; jmp _exit
_fail_crt_chmod_write:                  pushl $93; jmp _exit
_fail_crt_stat_writable:                pushl $94; jmp _exit
_fail_crt_mode_writable:                pushl $95; jmp _exit
_fail_crt_chmod_restore:                pushl $96; jmp _exit
_fail_crt_stat_restored:                pushl $97; jmp _exit
_fail_crt_mode_restored:                pushl $98; jmp _exit
_fail_rtti_name1_alloc:                 pushl $99; jmp _exit
_fail_rtti_node1_alloc:                 pushl $100; jmp _exit
_fail_rtti_name2_alloc:                 pushl $101; jmp _exit
_fail_rtti_node2_alloc:                 pushl $102; jmp _exit
_fail_rtti_root_detach:                 pushl $103; jmp _exit
_fail_rtti_root_data:                   pushl $104; jmp _exit
_fail_rtti_root_guard:                  pushl $105; jmp _exit
_fail_rtti_repeat_cleanup:              pushl $106; jmp _exit
_fail_rtti_dtor_name_alloc:             pushl $107; jmp _exit
_fail_rtti_dtor_stack:                  pushl $108; jmp _exit
_fail_rtti_dtor_name:                   pushl $109; jmp _exit
_fail_rtti_dtor_guard:                  pushl $110; jmp _exit
_fail_rtti_dtor_repeat:                 pushl $111; jmp _exit
_fail_crt_timezone_data:                pushl $112; jmp _exit
_fail_crt_timezone_accessor:            pushl $113; jmp _exit
_fail_crt_timezone_write:               pushl $114; jmp _exit
_fail_crt_daylight_data:                pushl $115; jmp _exit
_fail_crt_daylight_accessor:            pushl $116; jmp _exit
_fail_crt_daylight_write:               pushl $117; jmp _exit
_fail_cpp_filter_non_cpp:                pushl $118; jmp _exit
_fail_cpp_filter_cpp:                    pushl $119; jmp _exit
_fail_ci_exp:                            pushl $120; jmp _exit
_fail_finite_value:                      pushl $121; jmp _exit
_fail_finite_infinity:                   pushl $122; jmp _exit
_fail_finite_nan:                        pushl $123; jmp _exit
_fail_file_type_console:                 pushl $124; jmp _exit
_fail_file_type_invalid:                 pushl $125; jmp _exit
_fail_file_type_invalid_error:           pushl $126; jmp _exit
_fail_crt_fstat_open:                    pushl $127; jmp _exit
_fail_crt_fstat_handle:                  pushl $128; jmp _exit
_fail_crt_fstat_type:                    pushl $129; jmp _exit
_fail_crt_fstat_query:                   pushl $130; jmp _exit
_fail_crt_fstat_guard:                   pushl $131; jmp _exit
_fail_crt_fstat_mode:                    pushl $132; jmp _exit
_fail_crt_fstat_size:                    pushl $133; jmp _exit
_fail_crt_fstat_time:                    pushl $134; jmp _exit
_fail_crt_fstat_close:                   pushl $135; jmp _exit
_fail_crt_fstat_closed:                  pushl $136; jmp _exit
_fail_crt_fdopen_mismatch:               pushl $137; jmp _exit
_fail_crt_fdopen_read:                   pushl $138; jmp _exit
_fail_signal_install:                    pushl $139; jmp _exit
_fail_signal_raise:                      pushl $140; jmp _exit
_fail_signal_count:                      pushl $141; jmp _exit
_fail_signal_argument:                   pushl $142; jmp _exit
_fail_signal_reset:                      pushl $143; jmp _exit
_fail_signal_ignore:                     pushl $144; jmp _exit
_fail_signal_restore:                    pushl $145; jmp _exit
_fail_signal_invalid:                    pushl $146; jmp _exit
_fail_environ_cell:                      pushl $147; jmp _exit
_fail_environ_accessor:                  pushl $148; jmp _exit
_fail_environ_entries:                   pushl $149; jmp _exit
_fail_environ_update:                    pushl $150; jmp _exit
_fail_environ_refresh:                   pushl $151; jmp _exit
_fail_ci_log10:                          pushl $152; jmp _exit
_fail_unlink_create:                     pushl $153; jmp _exit
_fail_unlink_close:                      pushl $154; jmp _exit
_fail_unlink_readonly_set:               pushl $155; jmp _exit
_fail_unlink_readonly_get:               pushl $156; jmp _exit
_fail_unlink_readonly_delete:            pushl $157; jmp _exit
_fail_unlink_readonly_errno:             pushl $158; jmp _exit
_fail_unlink_writable_set:               pushl $159; jmp _exit
_fail_unlink_delete:                     pushl $160; jmp _exit
_fail_unlink_still_exists:               pushl $161; jmp _exit
_fail_unlink_missing:                    pushl $162; jmp _exit
_fail_unlink_missing_errno:              pushl $163; jmp _exit
_fail_wunlink_missing:                   pushl $164; jmp _exit
_fail_wunlink_errno:                     pushl $165; jmp _exit
_fail_remove_missing:                    pushl $166; jmp _exit
_fail_remove_errno:                      pushl $167; jmp _exit
_fail_wremove_missing:                   pushl $168; jmp _exit
_fail_wremove_errno:                     pushl $169; jmp _exit
_fail_strtod_value:                      pushl $170; jmp _exit
_fail_strtod_end:                        pushl $171; jmp _exit
_fail_strtod_rollback_value:             pushl $172; jmp _exit
_fail_strtod_rollback_end:               pushl $173; jmp _exit
_fail_ctime64_value:                     pushl $174; jmp _exit
_fail_ctime64_negative:                  pushl $175; jmp _exit
_fail_ctime64_errno:                     pushl $176; jmp _exit
_fail_ci_sqrt:                           pushl $177; jmp _exit
_fail_rename_create:                     pushl $178; jmp _exit
_fail_rename_close:                      pushl $179; jmp _exit
_fail_rename_move:                       pushl $180; jmp _exit
_fail_rename_source_exists:              pushl $181; jmp _exit
_fail_rename_target_missing:             pushl $182; jmp _exit
_fail_rename_collision_create:           pushl $183; jmp _exit
_fail_rename_collision:                  pushl $184; jmp _exit
_fail_rename_collision_errno:            pushl $185; jmp _exit
_fail_rename_missing:                    pushl $186; jmp _exit
_fail_rename_missing_errno:              pushl $187; jmp _exit
_fail_wrename_missing:                   pushl $188; jmp _exit
_fail_wrename_errno:                     pushl $189; jmp _exit
_fail_getdrive:                          pushl $190; jmp _exit
_fail_wcserror:                          pushl $191; jmp _exit
_fail_getdcwd:                           pushl $192; jmp _exit
_fail_wgetdcwd:                          pushl $193; jmp _exit
_fail_wgetdcwd_alloc:                    pushl $194; jmp _exit
_fail_wgetdcwd_invalid:                  pushl $195; jmp _exit
_fail_wgetdcwd_range:                    pushl $196; jmp _exit
_fail_mbstowcs:                          pushl $197; jmp _exit
_fail_mbstowcs_size:                     pushl $198; jmp _exit
_fail_mbstowcs_truncate:                 pushl $199; jmp _exit
_fail_wcstombs:                          pushl $200; jmp _exit
_fail_wcstombs_size:                     pushl $201; jmp _exit
_fail_wcstombs_invalid:                  pushl $202; jmp _exit
_fail_vswprintf:                          pushl $203; jmp _exit
_fail_vswprintf_size:                     pushl $204; jmp _exit
_fail_vswprintf_truncate:                 pushl $205; jmp _exit
_fail_vswprintf_zero:                     pushl $206; jmp _exit
_fail_handle_info_default:                pushl $207; jmp _exit
_fail_handle_info_set:                    pushl $208; jmp _exit
_fail_handle_info_protect:                pushl $209; jmp _exit
_fail_handle_info_clear:                  pushl $210; jmp _exit
_fail_handle_info_invalid:                pushl $211; jmp _exit
_fail_thread_context_null:                pushl $212; jmp _exit
_fail_thread_context_invalid:             pushl $213; jmp _exit
_fail_thread_context_unsupported:         pushl $214; jmp _exit
_fail_ftime_time:                         pushl $215; jmp _exit
_fail_ftime_millis:                       pushl $216; jmp _exit
_fail_ftime_zone:                         pushl $217; jmp _exit
_fail_ftime_guard:                        pushl $218; jmp _exit
_fail_iob_data:                           pushl $219; jmp _exit
_fail_iob_layout:                         pushl $220; jmp _exit
_fail_snwprintf:                          pushl $221; jmp _exit
_fail_fwprintf:                           pushl $222; jmp _exit
_fail_wsa_startup:                        pushl $223; jmp _exit
_fail_gethostbyaddr:                      pushl $224; jmp _exit
_fail_hostent_layout:                     pushl $225; jmp _exit
_fail_wsa_disconnect:                     pushl $226; jmp _exit
_fail_wsa_disconnect_error:               pushl $227; jmp _exit
_fail_wsa_cleanup:                        pushl $228; jmp _exit
_fail_strtok:                             pushl $229; jmp _exit
_fail_map_generic_mask:                   pushl $230; jmp _exit
_exit:
    calll *__imp__ExitProcess@4
    int3

    .data
    .align 4
_atomic_value:
    .long 7
_world_sid:
    .byte 1, 1, 0, 0, 0, 0, 0, 1
    .long 0
_deny_ace:
    .byte 1, 0
    .short 20
    .long 0x5678
    .byte 1, 1, 0, 0, 0, 0, 0, 1
    .long 0
_self_file_name:
    .asciz "console_pe32.exe"
_crt_mode_rb:
    .asciz "rb"
_crt_mode_wb:
    .asciz "wb"
_environment_name:
    .asciz "OSITO_TEST_ENV"
_environment_value:
    .asciz "ready"
_unlink_test_name:
    .asciz "crt-unlink-test.tmp"
    .align 2
_unlink_test_name_w:
    .short 99, 114, 116, 45, 117, 110, 108, 105, 110, 107
    .short 45, 116, 101, 115, 116, 46, 116, 109, 112, 0
_rename_source:
    .asciz "crt-rename-source.tmp"
_rename_target:
    .asciz "crt-rename-target.tmp"
    .align 2
_rename_source_w:
    .short 99, 114, 116, 45, 114, 101, 110, 97, 109, 101
    .short 45, 115, 111, 117, 114, 99, 101, 46, 116, 109, 112, 0
_rename_target_w:
    .short 99, 114, 116, 45, 114, 101, 110, 97, 109, 101
    .short 45, 116, 97, 114, 103, 101, 116, 46, 116, 109, 112, 0
_missing_file_name:
    .asciz "missing-security-test.exe"
    .align 4
_file_security_descriptor:
    .byte 1, 0
    .short 0x8004
    .long 0, 0, 0, 20
_file_security_acl:
    .byte 2, 0
    .short 28
    .short 1, 0
    .byte 0, 0
    .short 20
    .long 0x80000000
    .byte 1, 1, 0, 0, 0, 0, 0, 1
    .long 0
_invalid_security_descriptor:
    .byte 2, 0
    .short 0
    .long 0, 0, 0, 0
_nt_authority:
    .byte 0, 0, 0, 0, 0, 5
_kernel32_module_name:
    .asciz "kernel32.dll"
_version_root_subblock:
    .asciz "\\"
    .align 8
_ci_exp_zero:
    .quad 0
_ci_log10_one:
    .quad 0x3ff0000000000000
_ci_sqrt_four:
    .quad 0x4010000000000000
_strtod_input:
    .asciz " -12.5e2tail"
_strtod_rollback_input:
    .asciz "1e+tail"
_strtok_buffer:
    .asciz ",alpha,,beta,"
_strtok_delimiters:
    .asciz ","
_strtok_restart:
    .asciz "gamma:delta"
_strtok_restart_delimiter:
    .asciz ":"
    .align 4
_generic_mapping:
    .long 0x00020001
    .long 0x00040002
    .long 0x00100020
    .long 0x000f003f
    .align 8
_ctime64_epoch:
    .quad 0
_ctime64_negative:
    .quad -1
_mbstowcs_source:
    .asciz "Ab"
    .align 2
_wcstombs_source:
    .short 'A', 'b', 0
_wcstombs_invalid:
    .short 0x0100, 0
    .align 4
_vswprintf_args:
    .long _vswprintf_word
    .long -42
    .long 0x55667788, 0x11223344
    .long 0x00000000, 0x40040000
    .long _vswprintf_narrow
    .align 2
_vswprintf_format:
    .short '%', 's', ' ', '%', 'd', ' ', '%', 'I', '6', '4', 'x', ' '
    .short '%', '.', '1', 'f', ' ', '%', 'h', 's', 0
_vswprintf_word:
    .short 'o', 'k', 0
_vswprintf_expected:
    .short 'o', 'k', ' ', '-', '4', '2', ' '
    .short '1', '1', '2', '2', '3', '3', '4', '4'
    .short '5', '5', '6', '6', '7', '7', '8', '8', ' '
    .short '2', '.', '5', ' ', 'n', 'b', 0
_vswprintf_narrow:
    .asciz "nb"
    .align 2
_snwprintf_format:
    .short '%', 's', ' ', '%', 'd', 0
_snwprintf_word:
    .short 'w', 'i', 'd', 'e', 0
_snwprintf_expected:
    .short 'w', 'i', 'd', 'e', ' ', '1', '7', 0
_fwprintf_format:
    .short 'f', 'w', ':', '%', 'd', '\n', 0
    .align 4
_loopback_address:
    .byte 127, 0, 0, 1
_disconnect_buffer:
    .long 0, 0

    .bss
    .align 4
_snapshot_handle:
    .long 0
_mapping_handle:
    .long 0
_handle_flags:
    .long 0
_thread_context:
    .space 16
_window_station:
    .long 0
_user_object_needed:
    .long 0
_user_object_flags:
    .space 12
_user_object_name_a:
    .space 32
_user_object_name_w:
    .space 64
_user_object_type_a:
    .space 32
_test_acl:
    .space 64
_ace_pointer:
    .long 0
_ace_pointer_guard:
    .long 0
_acl_size_info:
    .space 12
_allocated_sid:
    .long 0
_allocated_sid_guard:
    .long 0
_version_handle:
    .long 0
_version_size:
    .long 0
_version_pointer:
    .long 0
_version_pointer_guard:
    .long 0
_version_length:
    .long 0
_version_blob:
    .space 128
_crt_stat:
    .space 48
_crt_fstat_fd:
    .long 0
_crt_fstat:
    .space 56
_crt_fstat_guard:
    .long 0
_signal_count:
    .long 0
_signal_seen:
    .long 0
_crt_timezone_pointer:
    .long 0
_crt_timezone_original:
    .long 0
_crt_daylight_pointer:
    .long 0
_crt_daylight_original:
    .long 0
_ci_exp_result:
    .space 8
_ci_log10_result:
    .space 8
_ci_sqrt_result:
    .space 8
_strtod_result:
    .space 8
_strtod_end:
    .long 0
_getdcwd_buffer:
    .space 260
    .align 2
_wgetdcwd_buffer:
    .space 520
_mbstowcs_buffer:
    .space 8
_wcstombs_buffer:
    .space 4
    .align 2
_vswprintf_buffer:
    .space 128
_ftime_buffer:
    .space 12
_ftime_guard:
    .long 0
_iob_base:
    .long 0
_generic_access_mask:
    .long 0
    .align 2
_snwprintf_buffer:
    .space 64
    .align 4
_wsa_data:
    .space 400
_rtti_root:
    .space 8
_rtti_root_guard:
    .long 0
_rtti_name1:
    .long 0
_rtti_node1:
    .long 0
_rtti_name2:
    .long 0
_rtti_node2:
    .long 0
_rtti_object:
    .space 8
_rtti_object_guard:
    .long 0
_rtti_stack_before:
    .long 0
_module_entry:
    .space 548
