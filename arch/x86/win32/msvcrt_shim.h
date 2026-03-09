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
void WINAPI __set_app_type(int type);
int  WINAPI _set_new_mode(int mode);

/* ── Memory ────────────────────────────────────────────────── */

PVOID WINAPI crt_malloc(SIZE_T size);
PVOID WINAPI crt_calloc(SIZE_T count, SIZE_T size);
PVOID WINAPI crt_realloc(PVOID ptr, SIZE_T size);
void  WINAPI crt_free(PVOID ptr);

/* ── String ────────────────────────────────────────────────── */

SIZE_T WINAPI crt_strlen(const char *s);
int    WINAPI crt_strcmp(const char *a, const char *b);
int    WINAPI crt_strncmp(const char *a, const char *b, SIZE_T n);
int    WINAPI crt_stricmp(const char *a, const char *b);
int    WINAPI crt_strnicmp(const char *a, const char *b, SIZE_T n);
char*  WINAPI crt_strcpy(char *dst, const char *src);
char*  WINAPI crt_strncpy(char *dst, const char *src, SIZE_T n);
char*  WINAPI crt_strcat(char *dst, const char *src);
char*  WINAPI crt_strstr(const char *haystack, const char *needle);
char*  WINAPI crt_strchr(const char *s, int c);
char*  WINAPI crt_strrchr(const char *s, int c);

/* ── Memory ops ────────────────────────────────────────────── */

PVOID WINAPI crt_memcpy(PVOID dst, PCVOID src, SIZE_T n);
PVOID WINAPI crt_memset(PVOID dst, int c, SIZE_T n);
PVOID WINAPI crt_memmove(PVOID dst, PCVOID src, SIZE_T n);
int   WINAPI crt_memcmp(PCVOID a, PCVOID b, SIZE_T n);

/* ── Format I/O ────────────────────────────────────────────── */

int WINAPI crt_printf(const char *fmt, ...);
int WINAPI crt_sprintf(char *buf, const char *fmt, ...);
int WINAPI crt_snprintf(char *buf, SIZE_T size, const char *fmt, ...);
int WINAPI crt_fprintf(PVOID stream, const char *fmt, ...);
int WINAPI crt_sscanf(const char *buf, const char *fmt, ...);
int WINAPI crt_puts(const char *s);
int WINAPI crt_putchar(int c);

/* ── stdio FILE* ───────────────────────────────────────────── */

typedef struct _CRT_FILE CRT_FILE;

CRT_FILE* WINAPI crt_fopen(const char *path, const char *mode);
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

/* stdio globals */
CRT_FILE* WINAPI crt_iob_func(int index);

/* ── Conversion ────────────────────────────────────────────── */

int       WINAPI crt_atoi(const char *s);
long      WINAPI crt_atol(const char *s);
double    WINAPI crt_atof(const char *s);
long      WINAPI crt_strtol(const char *s, char **endptr, int base);
unsigned long WINAPI crt_strtoul(const char *s, char **endptr, int base);

/* ── Process ───────────────────────────────────────────────── */

void  WINAPI crt_exit(int code);
void  WINAPI crt_abort(void);
void  WINAPI crt__exit(int code);
int   WINAPI crt_atexit(void (*func)(void));

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

/* ── Algorithm ─────────────────────────────────────────────── */

void WINAPI crt_qsort(PVOID base, SIZE_T nmemb, SIZE_T size,
                       int (WINAPI *compar)(PCVOID, PCVOID));
PVOID WINAPI crt_bsearch(PCVOID key, PCVOID base, SIZE_T nmemb,
                          SIZE_T size,
                          int (WINAPI *compar)(PCVOID, PCVOID));

/* ── Error ─────────────────────────────────────────────────── */

int*  WINAPI crt_errno(void);

/* ── Time ──────────────────────────────────────────────────── */

typedef long crt_time_t;
typedef long crt_clock_t;

crt_time_t  WINAPI crt_time(crt_time_t *timer);
crt_clock_t WINAPI crt_clock(void);

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

int  WINAPI crt_XcptFilter(int code, PVOID pointers);

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
void  WINAPI crt_CxxThrowException(PVOID pExceptionObject, PVOID pThrowInfo);
EXCEPTION_DISPOSITION WINAPI crt_CxxFrameHandler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext);
_PVFV_DLL WINAPI crt_dllonexit(_PVFV_DLL func, _PVFV_DLL **pbegin, _PVFV_DLL **pend);
int*  WINAPI crt_p_commode(void);
int*  WINAPI crt_p_fmode(void);
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
double  WINAPI crt_CIfmod(double x, double y);
double  WINAPI crt_CIpow(double base, double exp);
int     WINAPI crt_isnan(double x);
int     WINAPI crt_stat(const char *path, PVOID buf);
int     WINAPI crt_wstat(const WCHAR *path, PVOID buf);
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
double  WINAPI crt_difftime(crt_time_t t1, crt_time_t t0);
PVOID   WINAPI crt_gmtime(const crt_time_t *timer);
crt_time_t WINAPI crt_mktime(PVOID tm);
int     WINAPI crt_rand(void);
void    WINAPI crt_srand(unsigned int seed);
char*   WINAPI crt_strncat(char *dst, const char *src, SIZE_T n);
SIZE_T  WINAPI crt_wcslen(const WCHAR *s);
WCHAR*  WINAPI crt_wcscpy(WCHAR *dst, const WCHAR *src);
WCHAR*  WINAPI crt_wcsncpy(WCHAR *dst, const WCHAR *src, SIZE_T n);
WCHAR*  WINAPI crt_wcscat(WCHAR *dst, const WCHAR *src);
int     WINAPI crt_wcscmp(const WCHAR *a, const WCHAR *b);
int     WINAPI crt_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n);
WCHAR*  WINAPI crt_wcschr(const WCHAR *s, WCHAR c);
WCHAR*  WINAPI crt_wcsstr(const WCHAR *haystack, const WCHAR *needle);
unsigned long WINAPI crt_wcstoul(const WCHAR *s, WCHAR **endptr, int base);

/* ── Shim resolution ──────────────────────────────────────── */

PVOID msvcrt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
PVOID msvcrt_shim_init(void);

#endif /* MSVCRT_SHIM_H */
