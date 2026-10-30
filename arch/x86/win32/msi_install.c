/* An installer must undo its journal before a termination checkpoint unwinds
 * its host stack. This scope only defers delivery on the installing thread;
 * other applications, cancellation requests and scheduling remain live. */
#include "msi_install.h"
extern int32_t proc_current_pid(void);
extern BOOL win32_termination_requested(void);
static int32_t msi_install_owner;
BOOL msi_install_deferred_for_kernel_pid(int32_t pid)
{
    return pid > 0 && __atomic_load_n(&msi_install_owner, __ATOMIC_ACQUIRE) == pid;
}
BOOL msi_install_deferred_current(void)
{
    int32_t owner = __atomic_load_n(&msi_install_owner, __ATOMIC_ACQUIRE);
    return owner > 0 && owner == proc_current_pid();
}
BOOL msi_install_scope_enter(void)
{
    int32_t owner = proc_current_pid(), empty = 0;
    return owner > 0 && __atomic_compare_exchange_n(&msi_install_owner, &empty, owner,
        FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
void msi_install_scope_leave(void)
{
    int32_t owner = proc_current_pid();
    (void)__atomic_compare_exchange_n(&msi_install_owner, &owner, 0,
        FALSE, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
int msi_install_cancelled(void *context)
{
    (void)context;
    return msi_install_deferred_current() && win32_termination_requested();
}
