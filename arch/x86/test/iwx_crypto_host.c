/* Separate hosted TU: OpenSSL supplies the simulated AP's crypto, never the
 * kernel. Keep libc/OpenSSL headers out of the freestanding driver harness. */
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdlib.h>

unsigned iwx_test_wrap(const unsigned char key[16], const unsigned char *plain,
                       unsigned n, unsigned char *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) abort();
    EVP_CIPHER_CTX_set_flags(c, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    int length = 0, tail = 0;
    if (EVP_EncryptInit_ex(c, EVP_aes_128_wrap(), NULL, key, NULL) != 1 ||
        EVP_EncryptUpdate(c, out, &length, plain, n) != 1 ||
        EVP_EncryptFinal_ex(c, out + length, &tail) != 1) abort();
    EVP_CIPHER_CTX_free(c);
    return length + tail;
}

void iwx_test_hmac(const unsigned char key[16], const unsigned char *p,
                    unsigned n, unsigned char mac[20])
{
    unsigned length = 0;
    if (!HMAC(EVP_sha1(), key, 16, p, n, mac, &length) || length != 20) abort();
}
