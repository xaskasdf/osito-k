/*
 * OsitoK Windows Compatibility Layer — msvcrt.dll Shim
 *
 * Provides the Microsoft C Runtime functions that CRT-linked
 * Windows executables need. Covers: memory, strings, stdio,
 * CRT init, math basics, and process control.
 */

#ifndef MSVCRT_SHIM_H
#define MSVCRT_SHIM_H

#include "nttypes.h"

/* ── CRT Initialization ────────────────────────────────────── */

typedef void (*_PVFV)(void);
typedef int  (*_PIFV)(void);

void WINAPI _initterm(_PVFV *pfbegin, _PVFV *pfend);
int  WINAPI _initterm_e(_PIFV *pfbegin, _PIFV *pfend);
int  WINAPI __getmainargs(int *argc, char ***argv, char ***env,
                          int do_wildcard, void *startinfo);
int  WINAPI __wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env,
                           int do_wildcard, void *startinfo);
int  WINAPI __crtGetShowWindowMode(void);
void WINAPI __set_app_type(int type);
int  WINAPI _set_new_mode(int mode);
PVOID WINAPI crt_set_new_handler(PVOID handler);

/* ── Memory ────────────────────────────────────────────────── */

PVOID WINAPI crt_malloc(SIZE_T size);
PVOID WINAPI crt_calloc(SIZE_T count, SIZE_T size);
PVOID WINAPI crt_realloc(PVOID ptr, SIZE_T size);
void  WINAPI crt_free(PVOID ptr);
SIZE_T WINAPI crt_msize(PVOID ptr);

/* ── String ────────────────────────────────────────────────── */

SIZE_T WINAPI crt_strlen(const char *s);
int    WINAPI crt_strcmp(const char *a, const char *b);
int    WINAPI crt_strncmp(const char *a, const char *b, SIZE_T n);
int    WINAPI crt_stricmp(const char *a, const char *b);
int    WINAPI crt_strnicmp(const char *a, const char *b, SIZE_T n);
char*  WINAPI crt_strcpy(char *dst, const char *src);
char*  WINAPI crt_strncpy(char *dst, const char *src, SIZE_T n);
int    WINAPI crt_strncpy_s(char *dst, SIZE_T dst_chars,
                            const char *src, SIZE_T count);
char*  WINAPI crt_strcat(char *dst, const char *src);
char*  WINAPI crt_strstr(const char *haystack, const char *needle);
char*  WINAPI crt_strchr(const char *s, int c);
char*  WINAPI crt_strrchr(const char *s, int c);
char*  WINAPI crt_strdup(const char *s);
SIZE_T WINAPI crt_strcspn(const char *s, const char *reject);
char*  WINAPI crt_strpbrk(const char *s, const char *accept);
char*  WINAPI crt_strtok(char *str, const char *delimiters);
char*  WINAPI crt_strtok_s(char *str, const char *delimiters,
                           char **context);

/* ── Memory ops ────────────────────────────────────────────── */

PVOID WINAPI crt_memcpy(PVOID dst, PCVOID src, SIZE_T n);
PVOID WINAPI crt_memset(PVOID dst, int c, SIZE_T n);
PVOID WINAPI crt_memmove(PVOID dst, PCVOID src, SIZE_T n);
int   WINAPI crt_memcmp(PCVOID a, PCVOID b, SIZE_T n);
PVOID WINAPI crt_memchr(PCVOID ptr, int value, SIZE_T n);

/* ── Format I/O ────────────────────────────────────────────── */

int WINAPI crt_printf(const char *fmt, ...);
int WINAPI crt_sprintf(char *buf, const char *fmt, ...);
int WINAPI crt_snprintf(char *buf, SIZE_T size, const char *fmt, ...);
int WINAPI crt_snwprintf(WCHAR *buf, SIZE_T size, const WCHAR *fmt, ...);
int WINAPI crt_snprintf_s(char *buf, SIZE_T size, SIZE_T count,
                          const char *fmt, ...);
int WINAPI crt_fprintf(PVOID stream, const char *fmt, ...);
int WINAPI crt_fwprintf(PVOID stream, const WCHAR *fmt, ...);
int WINAPI crt_sscanf(const char *buf, const char *fmt, ...);
int WINAPI crt_puts(const char *s);
int WINAPI crt_putchar(int c);
int WINAPI crt_stdio_common_vsprintf(uint64_t options, char *buffer,
                                     SIZE_T buffer_count,
                                     const char *format, PVOID locale,
                                     PVOID arg_list);
int WINAPI crt_stdio_common_vswprintf(uint64_t options, WCHAR *buffer,
                                      SIZE_T buffer_count,
                                      const WCHAR *format, PVOID locale,
                                      PVOID arg_list);
int WINAPI crt_vswprintf_c_l(WCHAR *buffer, SIZE_T buffer_count,
                              const WCHAR *format, PVOID locale,
                              PVOID arg_list);

/* ── stdio FILE* ───────────────────────────────────────────── */

typedef struct _CRT_FILE CRT_FILE;
typedef void (*crt_sighandler_t)(int);

CRT_FILE* WINAPI crt_fopen(const char *path, const char *mode);
CRT_FILE* WINAPI crt_fdopen(int fd, const char *mode);
SIZE_T    WINAPI crt_fread(PVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f);
SIZE_T    WINAPI crt_fwrite(PCVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f);
int       WINAPI crt_fclose(CRT_FILE *f);
int       WINAPI crt_fseek(CRT_FILE *f, long offset, int whence);
long      WINAPI crt_ftell(CRT_FILE *f);
int       WINAPI crt_fflush(CRT_FILE *f);
int       WINAPI crt_feof(CRT_FILE *f);
int       WINAPI crt_ferror(CRT_FILE *f);
int       WINAPI crt_fgetc(CRT_FILE *f);
int       WINAPI crt_fputc(int c, CRT_FILE *f);
char*     WINAPI crt_fgets(char *buf, int n, CRT_FILE *f);
int       WINAPI crt_fputs(const char *s, CRT_FILE *f);
int       WINAPI crt_ungetc(int c, CRT_FILE *f);
int       WINAPI crt_fileno(CRT_FILE *f);

/* Low-level Universal CRT descriptors. */
LONG_PTR  WINAPI crt_get_osfhandle(int fd);
int       WINAPI crt_open_osfhandle(LONG_PTR handle, int flags);
int       WINAPI crt_open(const char *path, int flags, int mode);
int       WINAPI crt_wopen(const WCHAR *path, int flags, int mode);
int       WINAPI crt_unlink(const char *path);
int       WINAPI crt_wunlink(const WCHAR *path);
int       WINAPI crt_rename(const char *old_path, const char *new_path);
int       WINAPI crt_wrename(const WCHAR *old_path, const WCHAR *new_path);
int       WINAPI crt_getdrive(void);
int       WINAPI crt_close(int fd);
int       WINAPI crt_read(int fd, PVOID buffer, unsigned int count);
int       WINAPI crt_write(int fd, PCVOID buffer, unsigned int count);
LONG      WINAPI crt_lseek(int fd, LONG offset, int origin);
LONGLONG  WINAPI crt_lseeki64(int fd, LONGLONG offset, int origin);
int       WINAPI crt_dup(int fd);
int       WINAPI crt_dup2(int source_fd, int target_fd);
int       WINAPI crt_commit(int fd);
int       WINAPI crt_isatty(int fd);
int       WINAPI crt_setmode(int fd, int mode);
int       WINAPI crt_chsize_s(int fd, ULONGLONG size);

/* stdio globals. Legacy MSVCRT returns the base of the three-element iob
 * array, while the Universal CRT resolves one stream by index. */
CRT_FILE* WINAPI crt_iob_func(void);
CRT_FILE* WINAPI crt_acrt_iob_func(unsigned int index);

/* ── Conversion ────────────────────────────────────────────── */

int       WINAPI crt_atoi(const char *s);
long      WINAPI crt_atol(const char *s);
double    WINAPI crt_atof(const char *s);
double    WINAPI crt_strtod(const char *s, char **endptr);
uint64_t  WINAPI crt_strtod_compat32(uint64_t s_arg, uint64_t endptr_arg);
SIZE_T    WINAPI crt_mbstowcs(WCHAR *destination, const char *source,
                              SIZE_T count);
SIZE_T    WINAPI crt_wcstombs(char *destination, const WCHAR *source,
                              SIZE_T count);
int       WINAPI crt_abs(int value);
long      WINAPI crt_strtol(const char *s, char **endptr, int base);
unsigned long WINAPI crt_strtoul(const char *s, char **endptr, int base);

/* ── Process ───────────────────────────────────────────────── */

void  WINAPI crt_exit(int code);
void  WINAPI crt_abort(void);
void  WINAPI crt__exit(int code);
int   WINAPI crt_getpid(void);
ULONG_PTR WINAPI crt_beginthreadex(PVOID security, unsigned stack_size,
                                    PVOID start_address, PVOID argument,
                                    unsigned init_flags, unsigned *thread_id);
void  WINAPI crt_endthreadex(unsigned exit_code) __attribute__((noreturn));
int   WINAPI crt_atexit(void (*func)(void));

/* Universal CRT process-startup support. The table argument is opaque here
 * because its pointer fields are 32 or 64 bits according to the calling PE. */
int   WINAPI crt_initialize_onexit_table(PVOID table);
int   WINAPI crt_register_onexit_function(PVOID table, PVOID function);
int   WINAPI crt_execute_onexit_table(PVOID table);
int   WINAPI crt_configure_narrow_argv(int mode);
int   WINAPI crt_initialize_narrow_environment(void);
char *WINAPI crt_get_narrow_winmain_command_line(void);
PVOID WINAPI crt_set_thread_local_invalid_parameter_handler(PVOID handler);
char* WINAPI crt_getenv(const char *name);
WCHAR* WINAPI crt_wgetenv(const WCHAR *name);
int   WINAPI crt_putenv(const char *assignment);
int   WINAPI crt_wputenv(const WCHAR *assignment);
int   WINAPI crt_putenv_s(const char *name, const char *value);
int   WINAPI crt_wputenv_s(const WCHAR *name, const WCHAR *value);
char*** WINAPI crt_p_environ(void);
WCHAR*** WINAPI crt_p_wenviron(void);
char* WINAPI crt_getcwd(char *buffer, int max_length);
WCHAR* WINAPI crt_wgetcwd(WCHAR *buffer, int max_length);
char* WINAPI crt_getdcwd(int drive, char *buffer, int max_length);
WCHAR* WINAPI crt_wgetdcwd(int drive, WCHAR *buffer, int max_length);
char* WINAPI crt_fullpath(char *absolute, const char *relative,
                          SIZE_T max_length);
WCHAR* WINAPI crt_wfullpath(WCHAR *absolute, const WCHAR *relative,
                            SIZE_T max_length);

/* Locale. OsitoK currently exposes the invariant C locale. */
char*  WINAPI crt_setlocale(int category, const char *locale);
WCHAR* WINAPI crt_wsetlocale(int category, const WCHAR *locale);
PVOID  WINAPI crt_localeconv(void);

/* ── ctype ─────────────────────────────────────────────────── */

int WINAPI crt_isalpha(int c);
int WINAPI crt_isdigit(int c);
int WINAPI crt_isalnum(int c);
int WINAPI crt_isspace(int c);
int WINAPI crt_isupper(int c);
int WINAPI crt_islower(int c);
int WINAPI crt_isprint(int c);
int WINAPI crt_toupper(int c);
int WINAPI crt_tolower(int c);
int WINAPI crt_iswctype(int c, int mask);
int WINAPI crt_iswalpha(int c);
int WINAPI crt_iswalnum(int c);
int WINAPI crt_iswdigit(int c);
int WINAPI crt_iswspace(int c);
int WINAPI crt_iswupper(int c);
int WINAPI crt_iswlower(int c);
int WINAPI crt_iswprint(int c);
int WINAPI crt_towupper(int c);
int WINAPI crt_towlower(int c);

/* ── Algorithm ─────────────────────────────────────────────── */

void WINAPI crt_qsort(PVOID base, SIZE_T nmemb, SIZE_T size,
                       int (WINAPI *compar)(PCVOID, PCVOID));
PVOID WINAPI crt_bsearch(PCVOID key, PCVOID base, SIZE_T nmemb,
                          SIZE_T size,
                          int (WINAPI *compar)(PCVOID, PCVOID));

/* ── Error ─────────────────────────────────────────────────── */

int*  WINAPI crt_errno(void);
ULONG* WINAPI crt_doserrno(void);
char** WINAPI crt_sys_errlist(void);
int*  WINAPI crt_sys_nerr(void);
char* WINAPI crt_strerror(int error);
WCHAR* WINAPI crt_wcserror(int error);
int   WINAPI crt_fpe_flt_rounds(void);

/* ── Time ──────────────────────────────────────────────────── */

typedef long crt_time_t;
typedef long crt_clock_t;

crt_time_t  WINAPI crt_time(crt_time_t *timer);
crt_clock_t WINAPI crt_clock(void);
void        WINAPI crt_ftime(PVOID result);
void        WINAPI crt_tzset(void);
LONG*       WINAPI crt_timezone(void);
int*        WINAPI crt_daylight(void);
int64_t     WINAPI crt_time64(int64_t *timer);
char*       WINAPI crt_ctime64(const int64_t *timer);
int         WINAPI crt_gmtime64_s(PVOID result, const int64_t *timer);
PVOID       WINAPI crt_localtime64(const int64_t *timer);
int         WINAPI crt_localtime64_s(PVOID result, const int64_t *timer);
int64_t     WINAPI crt_mktime64(PVOID tm);
SIZE_T      WINAPI crt_strftime(char *buffer, SIZE_T max_size,
                                const char *format, PCVOID tm);
void        WINAPI crt_sleep(unsigned long milliseconds);

/* ── SEH (Structured Exception Handling) ───────────────────── */

EXCEPTION_DISPOSITION WINAPI crt_except_handler3(
    PEXCEPTION_RECORD ExceptionRecord,
    PEH3_EXCEPTION_REGISTRATION EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext);

EXCEPTION_DISPOSITION WINAPI crt_except_handler4(
    PEXCEPTION_RECORD ExceptionRecord,
    PEH3_EXCEPTION_REGISTRATION EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext);

EXCEPTION_DISPOSITION WINAPI crt_except_handler4_common(
    ULONG *cookie,
    PVOID check_cookie,
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext);

int  WINAPI crt_XcptFilter(int code, PVOID pointers);
int  WINAPI crt_CppXcptFilter(int code, PVOID pointers);

/* PE32 non-local jumps are completed by compat32_dispatch, which has the
 * original i386 register file and caller stack. These functions are unique
 * resolver targets and must not be called as ordinary C implementations. */
int  WINAPI crt_compat32_setjmp_marker(PVOID environment);
int  WINAPI crt_compat32_setjmp3_marker(PVOID environment, int unwind_count);
void WINAPI crt_compat32_longjmp_marker(PVOID environment, int value);

/* ── Misc CRT internal ────────────────────────────────────── */

int   WINAPI crt_controlfp_s(unsigned int *old, unsigned int newval, unsigned int mask);
int   WINAPI crt_configthreadlocale(int type);
void  WINAPI crt_lock(int locknum);
void  WINAPI crt_unlock(int locknum);
int   WINAPI crt_crt_debugger_hook(int reserved);
void* WINAPI crt_encoded_null(void);
PVOID WINAPI crt_amsg_exit(int errnum);

/* ── C++ EH / UT99 required stubs ─────────────────────────── */

typedef void (*_onexit_t)(void);
typedef void (*_PVFV_DLL)(void);
typedef int  (*_UserMathErrFunc)(void);

void  WINAPI crt_type_info_dtor(PVOID _this);
void  WINAPI crt_type_info_dtor_internal(PVOID _this);
void  WINAPI crt_clean_type_info_names_internal(PVOID root_node);
void  WINAPI crt_std_type_info_destroy_list(PVOID list_head);
void  WINAPI crt_CxxThrowException(PVOID pExceptionObject, PVOID pThrowInfo);
EXCEPTION_DISPOSITION WINAPI crt_CxxFrameHandler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext);
uint32_t crt_find_cxx_func_info(uint32_t handler_addr);
_PVFV_DLL WINAPI crt_dllonexit(_PVFV_DLL func, _PVFV_DLL **pbegin, _PVFV_DLL **pend);
int*  WINAPI crt_p_commode(void);
int*  WINAPI crt_p_fmode(void);
int   WINAPI crt_set_fmode(int mode);
int   WINAPI crt_get_fmode(int *mode);
crt_sighandler_t WINAPI crt_signal(int sig, crt_sighandler_t handler);
int   WINAPI crt_raise(int sig);
void  msvcrt_release_process(DWORD process_id);
void  WINAPI crt_setusermatherr(_UserMathErrFunc handler);
char* WINAPI crt_acmdln(void);
int*  WINAPI crt_adjust_fdiv(void);
unsigned int WINAPI crt_controlfp(unsigned int newval, unsigned int mask);
long  WINAPI crt_ftol(double val);
_onexit_t WINAPI crt_onexit(_onexit_t func);
void  WINAPI crt_purecall(void);

/* ── UT99 Core.dll / Engine.dll required exports ──────────── */

void    WINAPI crt_terminate(void);
double  WINAPI crt_CIacos(double x);
double  WINAPI crt_CIexp(double x);
double  WINAPI crt_CIlog10(double x);
double  WINAPI crt_CIsqrt(double x);
double  WINAPI crt_CIfmod(double x, double y);
double  WINAPI crt_CIpow(double base, double exp);
int     WINAPI crt_finite(double x);
int     WINAPI crt_isnan(double x);
short   WINAPI crt_dclass(double x);
short   WINAPI crt_fdclass(float x);
int     WINAPI crt_stat(const char *path, PVOID buf);
int     WINAPI crt_wstat(const WCHAR *path, PVOID buf);
int     WINAPI crt_stat32(const char *path, PVOID buf);
int     WINAPI crt_stat32i64(const char *path, PVOID buf);
int     WINAPI crt_stat64i32(const char *path, PVOID buf);
int     WINAPI crt_stat64(const char *path, PVOID buf);
int     WINAPI crt_fstat64(int fd, PVOID buf);
int     WINAPI crt_chmod(const char *path, int mode);
int     WINAPI crt_wchmod(const WCHAR *path, int mode);
int     WINAPI crt_wstat32(const WCHAR *path, PVOID buf);
int     WINAPI crt_wstat32i64(const WCHAR *path, PVOID buf);
int     WINAPI crt_wstat64i32(const WCHAR *path, PVOID buf);
int     WINAPI crt_wstat64(const WCHAR *path, PVOID buf);
char*   WINAPI crt_strdate(char *buf);
char*   WINAPI crt_strtime(char *buf);
WCHAR*  WINAPI crt_wstrdate(WCHAR *buf);
WCHAR*  WINAPI crt_wstrtime(WCHAR *buf);
/* Note: actual signature takes ms_va_list — declared in .c file */
int     WINAPI crt_wcsicmp(const WCHAR *a, const WCHAR *b);
int     WINAPI crt_wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n);
WCHAR*  WINAPI crt_wcsupr(WCHAR *s);
int     WINAPI crt_wtoi(const WCHAR *s);
double  WINAPI crt_ceil(double x);
double  WINAPI crt_floor(double x);
double  WINAPI crt_fabs(double x);
double  WINAPI crt_sqrt(double x);
double  WINAPI crt_difftime(crt_time_t t1, crt_time_t t0);
PVOID   WINAPI crt_gmtime(const crt_time_t *timer);
crt_time_t WINAPI crt_mktime(PVOID tm);
int     WINAPI crt_rand(void);
void    WINAPI crt_srand(unsigned int seed);
char*   WINAPI crt_strncat(char *dst, const char *src, SIZE_T n);
SIZE_T  WINAPI crt_wcslen(const WCHAR *s);
SIZE_T  WINAPI crt_wcsnlen(const WCHAR *s, SIZE_T max_chars);
WCHAR*  WINAPI crt_wcsdup(const WCHAR *s);
WCHAR*  WINAPI crt_wcscpy(WCHAR *dst, const WCHAR *src);
int     WINAPI crt_wcscpy_s(WCHAR *dst, SIZE_T dst_chars,
                            const WCHAR *src);
WCHAR*  WINAPI crt_wcsncpy(WCHAR *dst, const WCHAR *src, SIZE_T n);
int     WINAPI crt_wcsncpy_s(WCHAR *dst, SIZE_T dst_chars,
                             const WCHAR *src, SIZE_T count);
WCHAR*  WINAPI crt_wcscat(WCHAR *dst, const WCHAR *src);
int     WINAPI crt_wcscat_s(WCHAR *dst, SIZE_T dst_chars,
                            const WCHAR *src);
int     WINAPI crt_wcscmp(const WCHAR *a, const WCHAR *b);
int     WINAPI crt_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n);
int     WINAPI crt_wcscoll(const WCHAR *a, const WCHAR *b);
SIZE_T  WINAPI crt_wcsxfrm(WCHAR *dst, const WCHAR *src, SIZE_T dst_chars);
WCHAR*  WINAPI crt_wcschr(const WCHAR *s, WCHAR c);
WCHAR*  WINAPI crt_wcsrchr(const WCHAR *s, WCHAR c);
WCHAR*  WINAPI crt_wcsstr(const WCHAR *haystack, const WCHAR *needle);
WCHAR*  WINAPI crt_wcstok_s(WCHAR *str, const WCHAR *delimiters,
                            WCHAR **context);
unsigned long WINAPI crt_wcstoul(const WCHAR *s, WCHAR **endptr, int base);

/* ── Shim resolution ──────────────────────────────────────── */

PVOID msvcrt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
PVOID msvcrt_shim_init(void);

#endif /* MSVCRT_SHIM_H */
