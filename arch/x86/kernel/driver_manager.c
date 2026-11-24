#include "driver_manager.h"

int driver_service_claim(driver_service_t *service)
{
    unsigned expected = 0;
    return __atomic_compare_exchange_n(&service->busy, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void driver_service_release(driver_service_t *service)
{
    __atomic_store_n(&service->busy, 0, __ATOMIC_RELEASE);
}

void driver_manager_tick(driver_service_t *services, unsigned count, unsigned long long now)
{
    for (unsigned i = 0; i < count; i++) {
        driver_service_t *s = &services[i];
        if (!driver_service_claim(s)) continue;
        if (!s->paused && s->step && now >= s->next_ms) {
            s->retry_ms = 1000;
            s->state = s->step(s);
            unsigned long long finished = s->clock_ms ? s->clock_ms() : now;
            if (finished < now) finished = now;
            /* No callback can accidentally create an unbounded retry loop. */
            s->next_ms = finished + (s->retry_ms < 100 ? 100 : s->retry_ms);
        }
        driver_service_release(s);
    }
}

const char *driver_state_name(enum driver_state state)
{
    static const char *const names[] = {"detected", "absent", "waiting-filesystem",
        "waiting-firmware", "waiting-network", "starting", "waiting-config",
        "connecting", "ready", "disabled", "failed", "quarantined"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "invalid";
}
