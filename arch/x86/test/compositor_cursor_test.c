/* Check the production overlay without a display server or host input. */
#include "../kernel/compositor.c"

extern int printf(const char *, ...);
static int32_t test_x, test_y;
static uint32_t hidden_owner, queried_owner;
static int failures, checks;
static uint32_t pixels[64 * 64];

void input_get_cursor(int32_t *x, int32_t *y)
{
    *x = test_x;
    *y = test_y;
}

bool user32_cursor_overlay_visible(uint32_t id)
{
    queried_owner = id;
    return id != hidden_owner;
}

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void check_overlay(const window_t *owner, bool visible)
{
    const uint32_t background = 0xFF316247;
    for (unsigned i = 0; i < 64 * 64; i++) pixels[i] = background;
    queried_owner = 0;
    draw_cursor_overlay(pixels, 64, 64, 64, owner);
    unsigned changed = 0;
    for (unsigned i = 0; i < 64 * 64; i++)
        if (pixels[i] != background) changed++;
    CHECK((changed != 0) == visible);
    CHECK(queried_owner ==
          (owner && (owner->flags & WND_USER32) ? owner->id : 0));
}

int main(void)
{
    compositor_init_cursor();
    window_t owner = { .id = 7, .flags = WND_ACTIVE | WND_VISIBLE | WND_USER32 };
    hidden_owner = owner.id;
    for (int edge = 0; edge < 2; edge++) {
        test_x = test_y = edge ? 60 : 8;
        check_overlay(&owner, false);
        owner.flags |= WND_FULLSCREEN;
        check_overlay(&owner, false);
        owner.id = 8;
        check_overlay(&owner, true);
        owner.id = 7;
        owner.flags &= (uint8_t)~WND_USER32;
        check_overlay(&owner, true);
        check_overlay(NULL, true);
        owner.flags = WND_ACTIVE | WND_VISIBLE | WND_USER32;
    }
    hidden_owner = 0;
    check_overlay(&owner, true);
    printf("compositor cursor: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
