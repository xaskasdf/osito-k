#include "wlan_profiles.h"
#include "../include/types.h"
_Static_assert(sizeof(wlan_profiles_t) == WLAN_PROFILES_BYTES, "profile wire layout");

int wlan_profiles_valid(const wlan_profiles_t *p, unsigned n)
{
    if (n != sizeof(*p) || p->version != 1 || p->enabled > 1 ||
        p->count > WLAN_PROFILE_MAX || p->reserved) return 0;
    for (unsigned i = 0; i < WLAN_PROFILE_MAX; i++) {
        const wlan_profile_t *s = &p->network[i];
        if (i >= p->count) {
            const unsigned char *b = (const unsigned char *)s;
            for (unsigned j = 0; j < sizeof(*s); j++) if (b[j]) return 0;
            continue;
        }
        if (!s->length || s->length > 32 || s->secure > 1 || s->reserved ||
            (s->secure ? s->group_cipher != 2 && s->group_cipher != 4 : s->group_cipher != 0)) return 0;
        for (unsigned j = s->length; j < 32; j++) if (s->ssid[j]) return 0;
        if (!s->secure) for (unsigned j = 0; j < 32; j++) if (s->pmk[j]) return 0;
        for (unsigned j = 0; j < i; j++)
            if (wlan_profiles_match(&p->network[j], s->ssid, s->length,
                                    s->secure, s->group_cipher)) return 0;
    }
    return 1;
}

int wlan_profiles_match(const wlan_profile_t *p, const unsigned char *ssid,
                        unsigned n, int secure, unsigned group)
{
    return n && n <= 32 && p->length == n && p->secure == !!secure &&
           p->group_cipher == group && !memcmp(p->ssid, ssid, n);
}

int wlan_profiles_remember(wlan_profiles_t *p, const wlan_profile_t *s)
{
    unsigned at = p->count;
    for (unsigned i = 0; i < p->count; i++)
        if (wlan_profiles_match(&p->network[i], s->ssid, s->length, s->secure, s->group_cipher)) { at = i; break; }
    if (at == WLAN_PROFILE_MAX) return -1; /* Never silently evict a trusted network. */
    if (at == p->count) p->count++;
    for (unsigned i = at; i; i--) p->network[i] = p->network[i - 1];
    p->network[0] = *s; /* Last successful explicit selection has priority. */
    return 0;
}

void wlan_profiles_forget(wlan_profiles_t *p, unsigned index)
{
    if (index >= p->count) return;
    for (unsigned i = index; i + 1 < p->count; i++) p->network[i] = p->network[i + 1];
    volatile unsigned char *last = (volatile unsigned char *)&p->network[--p->count];
    for (unsigned i = 0; i < sizeof(wlan_profile_t); i++) last[i] = 0;
}
