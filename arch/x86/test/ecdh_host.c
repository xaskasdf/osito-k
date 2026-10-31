/* ECDHE on P-256 and P-384 (kernel/ecdh.c over BearSSL): two parties agree
 * on the secret, the sizes are TLS's, and points off the curve are refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/ecdh.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("ECDH FAIL line=%d: %s\n", __LINE__, #x); } } while (0)

static unsigned long long state = 0x9E3779B97F4A7C15ULL;

void random_bytes(void *buf, unsigned len)
{
    unsigned char *p = buf;
    while (len--) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        *p++ = (unsigned char)state;
    }
}

int main(void)
{
    static const unsigned short groups[] = { 0x0017, 0x0018 };
    CHECK(ecdh_scalar_len(0x001D) == 0 && ecdh_point_len(0x001D) == 0);
    for (unsigned g = 0; g < 2; g++) {
        unsigned short group = groups[g];
        unsigned slen = ecdh_scalar_len(group), plen = ecdh_point_len(group);
        CHECK(slen == (group == 0x0017 ? 32u : 48u) && plen == 2 * slen + 1);
        for (int round = 0; round < 50; round++) {
            unsigned char a[ECDH_MAX_SCALAR], b[ECDH_MAX_SCALAR];
            unsigned char pa[ECDH_MAX_POINT], pb[ECDH_MAX_POINT];
            unsigned char sa[ECDH_MAX_SCALAR], sb[ECDH_MAX_SCALAR];
            CHECK(ecdh_keygen(group, a, pa) == 0 && ecdh_keygen(group, b, pb) == 0);
            CHECK(pa[0] == 0x04 && memcmp(pa, pb, plen) != 0);
            CHECK(ecdh_shared(group, a, pb, plen, sa) == 0);
            CHECK(ecdh_shared(group, b, pa, plen, sb) == 0);
            CHECK(memcmp(sa, sb, slen) == 0);
            /* A short point, a compressed one, one off the curve */
            CHECK(ecdh_shared(group, a, pb, plen - 1, sa) < 0);
            pb[0] = 0x02;
            CHECK(ecdh_shared(group, a, pb, plen, sa) < 0);
            pb[0] = 0x04;
            pb[plen - 1] ^= 1;
            CHECK(ecdh_shared(group, a, pb, plen, sa) < 0);
        }
    }
    unsigned char k[ECDH_MAX_SCALAR], p[ECDH_MAX_POINT];
    CHECK(ecdh_keygen(0x0019, k, p) < 0);                 /* P-521: not offered */
    printf("ECDH: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
