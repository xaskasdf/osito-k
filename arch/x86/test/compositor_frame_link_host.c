/* Exercise a redraw request through a separate translation unit, like USER32. */
#define main compositor_overlay_main
#include "compositor_cursor_test.c"
#undef main

extern int compositor_frame_external_request(void);

int main(void)
{
    if (compositor_overlay_main()) return 1;
    __atomic_store_n(&comp_frame_dirty, false, __ATOMIC_RELEASE);
    CHECK(compositor_frame_external_request() == 1);
    CHECK(__atomic_exchange_n(&comp_frame_dirty, false, __ATOMIC_ACQ_REL));
    CHECK(!__atomic_load_n(&comp_frame_dirty, __ATOMIC_ACQUIRE));
    /* Several external notifications coalesce into one pending frame. */
    for (unsigned i = 0; i < 3; i++) CHECK(compositor_frame_external_request() == 1);
    CHECK(__atomic_exchange_n(&comp_frame_dirty, false, __ATOMIC_ACQ_REL));
    CHECK(!__atomic_exchange_n(&comp_frame_dirty, false, __ATOMIC_ACQ_REL));
    printf("compositor frame link: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
