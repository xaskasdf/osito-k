/* W4.8 — minimal dlfcn.h shim. */
#ifndef OSITO_DLFCN_H
#define OSITO_DLFCN_H 1
#ifdef __cplusplus
#include_next <dlfcn.h>
#else

#define RTLD_LAZY    0x1
#define RTLD_NOW     0x2
#define RTLD_GLOBAL  0x100
#define RTLD_LOCAL   0x000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

typedef struct {
    const char *dli_fname;
    void       *dli_fbase;
    const char *dli_sname;
    void       *dli_saddr;
} Dl_info;

extern void *dlopen(const char *file, int mode);
extern int   dlclose(void *handle);
extern void *dlsym(void *handle, const char *name);
extern char *dlerror(void);
extern int   dladdr(const void *addr, Dl_info *info);

#endif /* __cplusplus */
#endif
