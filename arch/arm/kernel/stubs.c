/*
 * stubs.c -- Stub/redirect functions for x86-specific features on ARM64
 *
 * proc_exec: redirects to proc_run (ARM process model).
 * syscall_capture: now implemented in syscall.c (removed from stubs).
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "../drivers/gpu.h"

/* GPU device global (populated by PCI scan if GPU found) */
gpu_device_t gpu_dev;

/* GPU PCI accessor — returns pointer to gpu_dev if populated, else NULL */
gpu_device_t *pci_get_gpu(void)
{
    if (gpu_dev.vendor_id != 0)
        return &gpu_dev;
    return (gpu_device_t *)0;
}

/* Process execution — redirect to ARM's proc_run */
extern int proc_run(const char *path, int argc, char **argv);
int proc_exec(const char *path, const char **argv, const char **envp)
{
    (void)envp;
    char *only[2] = { (char *)path, (char *)0 };
    int argc = 0;
    while (argv && argv[argc])
        argc++;
    return argc ? proc_run(path, argc, (char **)argv) : proc_run(path, 1, only);
}
