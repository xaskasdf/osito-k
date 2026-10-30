#ifndef OSITO_NTJOB_H
#define OSITO_NTJOB_H

#include "nttypes.h"

/* One reference per numeric handle entry, not per inherited owner. */
void nt_job_handle_retain(PVOID object);
void nt_job_handle_release(PVOID object);
BOOL nt_job_process_create(ULONG pid);
void nt_job_process_exit(ULONG pid, NTSTATUS status);
NTSTATUS nt_job_inherit(ULONG parent, ULONG child, DWORD creation_flags);
BOOL nt_job_charge_commit(ULONG pid, SIZE_T bytes);
void nt_job_return_commit(ULONG pid, SIZE_T bytes);

#define NT_JOB_UI_HANDLES         0x0001U
#define NT_JOB_UI_READCLIPBOARD   0x0002U
#define NT_JOB_UI_WRITECLIPBOARD  0x0004U
#define NT_JOB_UI_SYSTEMPARAMETERS 0x0008U
#define NT_JOB_UI_DISPLAYSETTINGS 0x0010U
#define NT_JOB_UI_GLOBALATOMS     0x0020U
#define NT_JOB_UI_DESKTOP         0x0040U
#define NT_JOB_UI_EXITWINDOWS     0x0080U
DWORD nt_job_ui_restrictions(ULONG pid);
enum nt_job_user_kind {
    NT_JOB_USER_WINDOW = 1, NT_JOB_USER_MENU, NT_JOB_USER_ICON, NT_JOB_USER_HOOK,
    NT_JOB_USER_INPUT_CONTEXT
};
BOOL nt_job_user_access(ULONG caller, ULONG owner, UINT kind, ULONG_PTR value);
BOOL nt_job_user_same_domain(ULONG caller, ULONG owner);
BOOL nt_job_user_grant(HANDLE job, ULONG owner, UINT kind, ULONG_PTR value, BOOL grant);
void nt_job_user_destroy(UINT kind, ULONG_PTR value);
/* Zero selects the global table. Nonzero IDs survive UI toggles and are never
 * recycled; the job's last reference releases its private atom entries. */
ULONGLONG nt_job_global_atom_namespace(ULONG pid);

HANDLE WINAPI CreateJobObjectW(PVOID attributes, PCWSTR name);
HANDLE WINAPI CreateJobObjectA(PVOID attributes, PCSTR name);
BOOL WINAPI SetInformationJobObject(HANDLE job, DWORD info_class,
                                    PCVOID info, DWORD size);
BOOL WINAPI QueryInformationJobObject(HANDLE job, DWORD info_class,
                                      PVOID info, DWORD size, DWORD *returned);
BOOL WINAPI AssignProcessToJobObject(HANDLE job, HANDLE process);
BOOL WINAPI IsProcessInJob(HANDLE process, HANDLE job, BOOL *result);
BOOL WINAPI TerminateJobObject(HANDLE job, UINT exit_code);

#endif
