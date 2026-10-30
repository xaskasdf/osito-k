/* In-memory NT file backend for differential tests of the production shim. */
#include "variant_host_types.h"
#include "../win32/imagehlp_shim.h"
#include "../win32/kernel32_shim.h"
#include "../win32/ntdll_shim.h"
#include "../win32/handle.h"
HANDLE_TABLE g_handle_table;
static BYTE storage[2*1024*1024];
static ULONGLONG file_size,file_position;
static DWORD access_mask,last_error;
static int mode,fail_at,io_calls;
int *proc_win32_compat32_mode_slot(void){return &mode;}
DWORD win32_current_process_id(void){return 1;}
DWORD WINAPI GetLastError(void){return last_error;}
void WINAPI SetLastError(DWORD error){last_error=error;}
int win32_user_range_readable(const void *p,SIZE_T bytes,BOOL compat)
{return (uintptr_t)p>4096&&bytes<sizeof(storage)&&compat==mode;}
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{return (uintptr_t)p>4096&&bytes<sizeof(storage)&&compat==mode;}
NTSTATUS handle_snapshot_for_process(PHANDLE_TABLE table,HANDLE h,ULONG pid,HANDLE_OBJECT_SNAPSHOT *out)
{
    if(table!=&g_handle_table||h!=(HANDLE)(ULONG_PTR)7||pid!=1)return STATUS_INVALID_HANDLE;
    memset(out,0,sizeof(*out));out->type=OBJ_TYPE_FILE;out->access=access_mask;out->file_flags=FILE_OBJ_DISK_FILE;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtQueryInformationFile(HANDLE h,PIO_STATUS_BLOCK io,PVOID out,ULONG bytes,FILE_INFORMATION_CLASS info)
{
    (void)h;(void)bytes;io->Status=STATUS_SUCCESS;
    if(info==FileStandardInformation){memset(out,0,sizeof(FILE_STANDARD_INFORMATION));((FILE_STANDARD_INFORMATION *)out)->EndOfFile.QuadPart=(LONGLONG)file_size;}
    else if(info==FilePositionInformation)((FILE_POSITION_INFORMATION *)out)->CurrentByteOffset.QuadPart=(LONGLONG)file_position;
    else return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtSetInformationFile(HANDLE h,PIO_STATUS_BLOCK io,PVOID input,ULONG bytes,FILE_INFORMATION_CLASS info)
{
    (void)h;(void)bytes;io->Status=STATUS_SUCCESS;
    LONGLONG value=((LARGE_INTEGER *)input)->QuadPart;if(value<0)return STATUS_INVALID_PARAMETER;
    if(info==FilePositionInformation)file_position=(ULONGLONG)value;
    else if(info==FileEndOfFileInformation){
        if(!(access_mask&2)||(ULONGLONG)value>sizeof(storage))return STATUS_ACCESS_DENIED;
        if((ULONGLONG)value>file_size)memset(storage+file_size,0,(size_t)((ULONGLONG)value-file_size));
        file_size=(ULONGLONG)value;
    }else return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtReadFile(HANDLE h,HANDLE e,PVOID apc,PVOID ctx,PIO_STATUS_BLOCK io,PVOID out,ULONG n,PLARGE_INTEGER offset,PVOID key)
{
    (void)h;(void)e;(void)apc;(void)ctx;(void)key;io->Information=0;
    if(++io_calls==fail_at)return STATUS_UNSUCCESSFUL;
    if(!(access_mask&1)||offset->QuadPart<0||(ULONGLONG)offset->QuadPart>file_size||n>file_size-(ULONGLONG)offset->QuadPart)return STATUS_ACCESS_DENIED;
    memcpy(out,storage+offset->QuadPart,n);file_position=(ULONGLONG)offset->QuadPart+n;io->Information=n;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtWriteFile(HANDLE h,HANDLE e,PVOID apc,PVOID ctx,PIO_STATUS_BLOCK io,PVOID input,ULONG n,PLARGE_INTEGER offset,PVOID key)
{
    (void)h;(void)e;(void)apc;(void)ctx;(void)key;io->Information=0;
    if(++io_calls==fail_at)return STATUS_UNSUCCESSFUL;
    if(!(access_mask&2)||offset->QuadPart<0||(ULONGLONG)offset->QuadPart>sizeof(storage)||n>sizeof(storage)-(ULONGLONG)offset->QuadPart)return STATUS_ACCESS_DENIED;
    memcpy(storage+offset->QuadPart,input,n);file_position=(ULONGLONG)offset->QuadPart+n;
    if(file_position>file_size)file_size=file_position;
    io->Information=n;return STATUS_SUCCESS;
}
void host_reset(const BYTE *bytes,unsigned length,unsigned access,unsigned compat)
{memset(storage,0,sizeof(storage));memcpy(storage,bytes,length);file_size=length;file_position=17;access_mask=access;mode=compat;last_error=0;io_calls=fail_at=0;}
unsigned host_size(void){return (unsigned)file_size;}
BYTE *host_bytes(void){return storage;}
unsigned long long host_position(void){return file_position;}
unsigned host_error(void){return last_error;}
void host_error_set(unsigned value){last_error=value;}
void host_position_set(unsigned value){file_position=value;}
int host_add(void *cert,DWORD *index,int invalid)
{return imagehlp_ImageAddCertificate((HANDLE)(ULONG_PTR)(invalid?99:7),cert,index);}
int host_enum(unsigned filter,DWORD *count,DWORD *indices,unsigned capacity,int invalid)
{return imagehlp_ImageEnumerateCertificates((HANDLE)(ULONG_PTR)(invalid?99:7),(WORD)filter,count,indices,capacity);}
int host_remove(unsigned index,int invalid)
{return imagehlp_ImageRemoveCertificate((HANDLE)(ULONG_PTR)(invalid?99:7),index);}
void host_fail_at(unsigned call){fail_at=(int)call;io_calls=0;}
