#ifndef INTEL_D3D9_OFFLINE_IOCTL_H
#define INTEL_D3D9_OFFLINE_IOCTL_H
#include <sys/ioccom.h>
/* Intentionally undefined. A dependency on Linux device queries must fail
 * linking, never turn into a successful or silently ignored operation.
 */
int ioctl(int, unsigned long, ...);
#endif
