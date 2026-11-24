#include "../include/display_source.h"

static unsigned source_lock;
static uint64_t source_serial;
static display_source_change_t *source_change;
static display_source_owner_t *source_owner;
static uint64_t source_owner_serial;
static display_source_mode_t source_mode;

static uint64_t source_enter(void)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&source_lock,1,__ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return flags;
}
static void source_leave(uint64_t flags)
{
    __atomic_store_n(&source_lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
static bool source_mode_valid(const display_source_mode_t *m)
{
    return m && m->base && m->width && m->height && m->pitch>=m->width &&
        (uint64_t)m->pitch<=UINT64_MAX/4/m->height;
}
static bool source_mode_equal(const display_source_mode_t *a,const display_source_mode_t *b)
{
    return a->base==b->base && a->width==b->width && a->height==b->height && a->pitch==b->pitch;
}
bool display_source_begin(display_source_change_t *change,const display_source_mode_t *preserved)
{
    if (!change || change->self || (preserved && !source_mode_valid(preserved))) return false;
    uint64_t flags=source_enter();
    bool ok=!source_change && source_serial!=UINT64_MAX &&
        (!source_owner || (preserved && source_mode_equal(preserved,&source_mode)));
    if (ok) {
        source_change=change;
        *change=(display_source_change_t){change,++source_serial};
    }
    source_leave(flags);return ok;
}
bool display_source_end(display_source_change_t *change)
{
    if (!change) return false;
    uint64_t flags=source_enter();
    bool ok=source_change==change && change->self==change && change->serial==source_serial;
    if (ok) { source_change=NULL;*change=(display_source_change_t){0}; }
    source_leave(flags);return ok;
}
bool display_source_adopt(display_source_change_t *change,display_source_owner_t *owner,
    const display_source_mode_t *mode)
{
    if (!change || !owner || owner->self || !source_mode_valid(mode)) return false;
    uint64_t flags=source_enter();
    bool ok=!source_owner && source_change==change && change->self==change && change->serial==source_serial;
    if (ok) {
        source_owner=owner;source_owner_serial=source_serial;source_mode=*mode;
        *owner=(display_source_owner_t){owner,source_serial};
        source_change=NULL;*change=(display_source_change_t){0};
    }
    source_leave(flags);return ok;
}
bool display_source_mode(const display_source_owner_t *owner,display_source_mode_t *out)
{
    if (!owner || !out) return false;
    uint64_t flags=source_enter();
    bool ok=source_owner==owner && owner->self==owner && owner->serial==source_owner_serial;
    if (ok) *out=source_mode;
    source_leave(flags);return ok;
}
bool display_source_release(display_source_owner_t *owner)
{
    if (!owner) return false;
    uint64_t flags=source_enter();
    bool ok=!source_change && source_owner==owner && owner->self==owner && owner->serial==source_owner_serial;
    if (ok) {
        source_owner=NULL;source_owner_serial=0;source_mode=(display_source_mode_t){0};
        *owner=(display_source_owner_t){0};
    }
    source_leave(flags);return ok;
}
