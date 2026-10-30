#ifndef OSITOK_MSI_INSTALL_H
#define OSITOK_MSI_INSTALL_H
#include "nttypes.h"
BOOL msi_install_deferred_current(void);
BOOL msi_install_deferred_for_kernel_pid(int32_t pid);
BOOL msi_install_scope_enter(void);
void msi_install_scope_leave(void);
int msi_install_cancelled(void *context);
#endif
