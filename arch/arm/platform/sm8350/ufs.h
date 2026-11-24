/*
 * ufs.h -- the phone's UFS storage (ufs.c)
 */

#ifndef SM8350_UFS_H
#define SM8350_UFS_H

/* Mount the OsitoFS on userdata, if it has one (platform_probe_devices) */
void ufs_mount_at_boot(void);

#endif
