/* Compilation of Linux UAPI types for the offline Intel PCI metadata tables.
 * These request constants are never sent to an OS. The final compiler link
 * must discard Linux device-query code: no ioctl implementation is provided.
 */
#ifndef INTEL_D3D9_OFFLINE_IOCCOM_H
#define INTEL_D3D9_OFFLINE_IOCCOM_H
#define _IO(group, number) (((group) << 8) | (number))
#define _IOR(group, number, type) (_IO(group, number) | (sizeof(type) << 16) | (2u << 30))
#define _IOW(group, number, type) (_IO(group, number) | (sizeof(type) << 16) | (1u << 30))
#define _IOWR(group, number, type) (_IO(group, number) | (sizeof(type) << 16) | (3u << 30))
#endif
