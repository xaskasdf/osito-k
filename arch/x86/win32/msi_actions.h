#ifndef OSITOK_MSI_ACTIONS_H
#define OSITOK_MSI_ACTIONS_H
#include "msi_package.h"

/* Walk the authored execution sequence in sequence order. The visitor must
 * implement each active standard action or return an error. Custom actions
 * are never silently skipped: active type 19 aborts with 1603; other active
 * custom action types return 120 until their execution contract exists.
 * This walker does not implement UI or termination (negative-sequence) actions. */
typedef uint32_t (*msi_action_visitor_t)(void *context, const char *action);
uint32_t msi_actions_walk(msi_db_t *database, msi_package_t *package,
    msi_action_visitor_t visitor, void *context);
#endif
