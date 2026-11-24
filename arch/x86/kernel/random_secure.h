#ifndef OSITO_RANDOM_SECURE_H
#define OSITO_RANDOM_SECURE_H
#include <stdint.h>
#include <stddef.h>

/* Checked hardware DRBG. Returns 0 on success, -1 on failure and clears the
 * entire output. No TSC/xorshift fallback, allocation, logging or shared state.
 * A zero-length request succeeds even without hardware support. */
int random_secure_bytes(void *out, size_t length);
#endif
