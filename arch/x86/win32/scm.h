/*
 * OsitoK Windows Compatibility Layer - Service Control Manager.
 */

#ifndef WIN32_SCM_H
#define WIN32_SCM_H

#include "nttypes.h"

HANDLE WINAPI OpenSCManagerW(PCWSTR machine_name, PCWSTR database_name,
                             DWORD desired_access);
HANDLE WINAPI OpenServiceW(HANDLE manager, PCWSTR service_name,
                           DWORD desired_access);
HANDLE WINAPI CreateServiceW(HANDLE manager, PCWSTR service_name,
                             PCWSTR display_name, DWORD desired_access,
                             DWORD service_type, DWORD start_type,
                             DWORD error_control, PCWSTR binary_path,
                             PCWSTR load_order_group, DWORD *tag_id,
                             PCWSTR dependencies, PCWSTR service_start_name,
                             PCWSTR password);
BOOL WINAPI ChangeServiceConfigW(HANDLE service, DWORD service_type,
                                 DWORD start_type, DWORD error_control,
                                 PCWSTR binary_path, PCWSTR load_order_group,
                                 DWORD *tag_id, PCWSTR dependencies,
                                 PCWSTR service_start_name, PCWSTR password,
                                 PCWSTR display_name);
BOOL WINAPI ChangeServiceConfig2W(HANDLE service, DWORD info_level,
                                  PVOID info);
BOOL WINAPI DeleteService(HANDLE service);
BOOL WINAPI StartServiceW(HANDLE service, DWORD argc, PVOID argv);
BOOL WINAPI ControlService(HANDLE service, DWORD control, PVOID status);
BOOL WINAPI QueryServiceStatus(HANDLE service, PVOID status);
BOOL WINAPI QueryServiceStatusEx(HANDLE service, DWORD info_level,
                                 BYTE *buffer, DWORD buffer_size,
                                 DWORD *bytes_needed);
BOOL WINAPI QueryServiceConfigW(HANDLE service, BYTE *buffer,
                                DWORD buffer_size, DWORD *bytes_needed);
BOOL WINAPI CloseServiceHandle(HANDLE object);
BOOL WINAPI StartServiceCtrlDispatcherW(PVOID service_table);
HANDLE WINAPI RegisterServiceCtrlHandlerW(PCWSTR service_name,
                                          PVOID handler);
HANDLE WINAPI RegisterServiceCtrlHandlerExW(PCWSTR service_name,
                                            PVOID handler, PVOID context);
BOOL WINAPI SetServiceStatus(HANDLE status_handle, PVOID status);
BOOL WINAPI QueryServiceObjectSecurity(HANDLE service, DWORD security_info,
                                       PVOID descriptor, DWORD buffer_size,
                                       DWORD *bytes_needed);
BOOL WINAPI SetServiceObjectSecurity(HANDLE service, DWORD security_info,
                                     PVOID descriptor);

HANDLE WINAPI RegisterEventSourceW_scm(PCWSTR server_name, PCWSTR source_name);
BOOL WINAPI DeregisterEventSource_scm(HANDLE event_log);
BOOL WINAPI ReportEventW_scm(HANDLE event_log, WORD type, WORD category,
                             DWORD event_id, PVOID user_sid,
                             WORD string_count, DWORD data_size,
                             PVOID strings, PVOID raw_data);
HANDLE WINAPI OpenEventLogA_scm(PCSTR server_name, PCSTR source_name);
BOOL WINAPI ReadEventLogW_scm(HANDLE event_log, DWORD read_flags,
                              DWORD record_offset, PVOID buffer,
                              DWORD bytes_to_read, DWORD *bytes_read,
                              DWORD *minimum_bytes_needed);
BOOL WINAPI CloseEventLog_scm(HANDLE event_log);

DWORD advapi32_service_release_process(DWORD process_id);

#endif /* WIN32_SCM_H */
