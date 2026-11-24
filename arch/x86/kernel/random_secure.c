#include "random_secure.h"

#ifdef RANDOM_SECURE_HOST_TEST
extern int random_secure_supported(void);
extern int random_secure_word(uint64_t *word);
#else
#include "../include/cpu_features.h"
static int random_secure_supported(void) { return cpu_features.rdrand; }
static int random_secure_word(uint64_t *word)
{
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*word), "=qm"(ok) : : "cc");
    return ok;
}
#endif

int random_secure_bytes(void *out, size_t length)
{
    if (!length) return 0;
    if (!out) return -1;
    unsigned char *p = out;
    if (!random_secure_supported()) goto failed;
    for (size_t pos = 0; pos < length;) {
        uint64_t word = 0;
        unsigned attempt;
        for (attempt = 0; attempt < 10; attempt++)
            if (random_secure_word(&word)) break;
        if (attempt == 10) goto failed;
        for (unsigned i = 0; i < 8 && pos < length; i++, pos++) {
            p[pos] = (unsigned char)word;
            word >>= 8;
        }
        *(volatile uint64_t *)&word = 0;
    }
    return 0;
failed:
    for (size_t i = 0; i < length; i++) ((volatile unsigned char *)out)[i] = 0;
    return -1;
}
