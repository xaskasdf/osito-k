/* Host regression harness for the kernel OCSP and CRL validators. */

#include "../../arch/x86/kernel/certmgr.h"
#include "../../arch/x86/kernel/crl.h"
#include "../../arch/x86/kernel/ocsp.h"
#include "../../arch/x86/kernel/x509.h"
#include "../../arch/x86/fs/vfs.h"

extern void *malloc(size_t size);
extern void free(void *ptr);
extern int printf(const char *format, ...);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buffer, size_t count);
extern long lseek(int fd, long offset, int whence);
extern int close(int fd);
extern long time(void *result);

#define SEEK_END 2
#define SEEK_SET 0

typedef struct {
    uint8_t *data;
    uint32_t len;
} fixture_t;

static int failures;

static fixture_t load_fixture(const char *path)
{
    fixture_t result = { 0 };
    int fd = open(path, 0);
    if (fd < 0) return result;
    long length = lseek(fd, 0, SEEK_END);
    if (length <= 0 || length > (long)UINT32_MAX ||
        lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return result;
    }
    result.data = (uint8_t *)malloc((size_t)length);
    if (!result.data) {
        close(fd);
        return result;
    }
    long amount = read(fd, result.data, (size_t)length);
    close(fd);
    if (amount != length) {
        free(result.data);
        result.data = NULL;
        return result;
    }
    result.len = (uint32_t)length;
    return result;
}

static void expect_status(const char *name, int actual, int expected)
{
    if (actual == expected) {
        printf("PASS %-30s status=%d\n", name, actual);
        return;
    }
    printf("FAIL %-30s status=%d expected=%d\n",
           name, actual, expected);
    failures++;
}

static void expect_crldp(const char *name, const fixture_t *cert,
                         const char *expected)
{
    char actual[512];
    int length = x509_get_crldp_url(cert->data, cert->len,
                                    actual, sizeof(actual));
    uint32_t expected_length = 0;
    while (expected[expected_length]) expected_length++;
    int matches = length == (int)expected_length;
    for (uint32_t i = 0; matches && i < expected_length; i++)
        matches = actual[i] == expected[i];
    if (matches) {
        printf("PASS %-30s url=%s\n", name, actual);
        return;
    }
    printf("FAIL %-30s length=%d url=%s expected=%s\n",
           name, length, length > 0 ? actual : "<none>", expected);
    failures++;
}

void serial_puts(const char *text)
{
    printf("%s", text);
}

void serial_putdec(uint64_t value)
{
    (void)value;
}

void serial_puthex(uint64_t value, int digits)
{
    (void)value;
    (void)digits;
}

void *kmalloc(uint64_t size)
{
    return malloc((size_t)size);
}

void kfree(void *ptr)
{
    free(ptr);
}

uint32_t ntp_get_utc(void)
{
    return (uint32_t)time(NULL);
}

int http_plain_get(const char *url, uint8_t *out, uint32_t capacity)
{
    (void)url;
    (void)out;
    (void)capacity;
    return -1;
}

int http_plain_post(const char *url, const char *content_type,
                    const uint8_t *body, uint32_t body_len,
                    uint8_t *out, uint32_t out_capacity)
{
    (void)url;
    (void)content_type;
    (void)body;
    (void)body_len;
    (void)out;
    (void)out_capacity;
    return -1;
}

int certmgr_revocation_lookup(const uint8_t *cert, uint32_t cert_len,
                              uint32_t validation_time,
                              uint32_t *valid_until)
{
    (void)cert;
    (void)cert_len;
    (void)validation_time;
    (void)valid_until;
    return CERTMGR_REVOCATION_UNAVAILABLE;
}

void certmgr_revocation_store(const uint8_t *cert, uint32_t cert_len,
                              certmgr_revocation_status_t status,
                              uint32_t valid_until)
{
    (void)cert;
    (void)cert_len;
    (void)status;
    (void)valid_until;
}

bool vfs_find(const char *path, int mode, vfs_node_t *out_node)
{
    (void)path;
    (void)mode;
    (void)out_node;
    return false;
}

int vfs_read(vfs_node_t *node, uint64_t offset, void *buffer, uint64_t len)
{
    (void)node;
    (void)offset;
    (void)buffer;
    (void)len;
    return -1;
}

bool osfs2_is_mounted(void) { return false; }
void *osfs2_create(const char *name, uint64_t size)
{
    (void)name;
    (void)size;
    return NULL;
}
int osfs2_write(void *file, uint64_t offset,
                const void *buffer, uint64_t len)
{
    (void)file;
    (void)offset;
    (void)buffer;
    (void)len;
    return -1;
}
int osfs2_delete(const char *name) { (void)name; return -1; }
bool osfs3_is_mounted(void) { return false; }
void *osfs3_create(const char *path, uint64_t size)
{
    (void)path;
    (void)size;
    return NULL;
}
int osfs3_write(void *file, uint64_t offset,
                const void *buffer, uint64_t len)
{
    (void)file;
    (void)offset;
    (void)buffer;
    (void)len;
    return -1;
}
int osfs3_truncate(void *file, uint64_t size)
{
    (void)file;
    (void)size;
    return -1;
}
int osfs3_rename(const char *from, const char *to, bool replace)
{
    (void)from;
    (void)to;
    (void)replace;
    return -1;
}
int osfs3_delete(const char *path) { (void)path; return -1; }

int main(int argc, char **argv)
{
    if (argc != 8 && argc != 12) {
        printf("usage: %s issuer.der leaf.der other.der good.ocsp.der "
               "revoked.ocsp.der good.crl.der revoked.crl.der "
               "[live-leaf.der live-issuer.der live.crl.der crldp-url]\n",
               argv[0]);
        return 2;
    }

    fixture_t issuer = load_fixture(argv[1]);
    fixture_t leaf = load_fixture(argv[2]);
    fixture_t other = load_fixture(argv[3]);
    fixture_t good_ocsp = load_fixture(argv[4]);
    fixture_t revoked_ocsp = load_fixture(argv[5]);
    fixture_t good_crl = load_fixture(argv[6]);
    fixture_t revoked_crl = load_fixture(argv[7]);
    fixture_t *all[] = {
        &issuer, &leaf, &other, &good_ocsp,
        &revoked_ocsp, &good_crl, &revoked_crl,
    };
    for (uint32_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (!all[i]->data) {
            printf("FAIL unable to load fixture argument %u\n", i + 1);
            return 2;
        }
    }

    uint32_t now = (uint32_t)time(NULL);
    uint32_t valid_until = 0;
    expect_crldp("CRL distribution point", &leaf,
                 "http://crl.invalid/test.crl");
    expect_status("OCSP delegated good",
        ocsp_validate_response(good_ocsp.data, good_ocsp.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now, &valid_until), OCSP_GOOD);
    if (valid_until <= now) {
        printf("FAIL OCSP expiry not in the future\n");
        failures++;
    }
    expect_status("OCSP revoked",
        ocsp_validate_response(revoked_ocsp.data, revoked_ocsp.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now, &valid_until), OCSP_REVOKED);
    expect_status("OCSP mismatched CertID",
        ocsp_validate_response(good_ocsp.data, good_ocsp.len,
                               other.data, other.len, issuer.data, issuer.len,
                               now, &valid_until), OCSP_ERROR);
    expect_status("OCSP expired response",
        ocsp_validate_response(good_ocsp.data, good_ocsp.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now + 3U * 86400U, &valid_until), OCSP_ERROR);

    uint8_t *tampered = (uint8_t *)malloc(good_ocsp.len);
    memcpy(tampered, good_ocsp.data, good_ocsp.len);
    tampered[good_ocsp.len - 1] ^= 0x01;
    expect_status("OCSP tampered signature",
        ocsp_validate_response(tampered, good_ocsp.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now, &valid_until), OCSP_ERROR);
    free(tampered);

    expect_status("CRL good",
        crl_validate_and_check(good_crl.data, good_crl.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now, &valid_until), CRL_GOOD);
    expect_status("CRL revoked",
        crl_validate_and_check(revoked_crl.data, revoked_crl.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now, &valid_until), CRL_REVOKED);
    expect_status("CRL wrong issuer relation",
        crl_validate_and_check(good_crl.data, good_crl.len,
                               leaf.data, leaf.len, other.data, other.len,
                               now, &valid_until), CRL_ERROR);
    expect_status("CRL expired list",
        crl_validate_and_check(good_crl.data, good_crl.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now + 10U * 86400U, &valid_until), CRL_ERROR);

    tampered = (uint8_t *)malloc(good_crl.len);
    memcpy(tampered, good_crl.data, good_crl.len);
    tampered[good_crl.len - 1] ^= 0x01;
    expect_status("CRL tampered signature",
        crl_validate_and_check(tampered, good_crl.len,
                               leaf.data, leaf.len, issuer.data, issuer.len,
                               now, &valid_until), CRL_ERROR);
    free(tampered);

    fixture_t live_leaf = { 0 };
    fixture_t live_issuer = { 0 };
    fixture_t live_crl = { 0 };
    if (argc == 12) {
        live_leaf = load_fixture(argv[8]);
        live_issuer = load_fixture(argv[9]);
        live_crl = load_fixture(argv[10]);
        if (!live_leaf.data || !live_issuer.data || !live_crl.data) {
            printf("FAIL unable to load live CRL fixture\n");
            failures++;
        } else {
            expect_crldp("live CRL distribution point", &live_leaf,
                         argv[11]);
            expect_status("live CRL authenticated good",
                crl_validate_and_check(live_crl.data, live_crl.len,
                                       live_leaf.data, live_leaf.len,
                                       live_issuer.data, live_issuer.len,
                                       now, &valid_until), CRL_GOOD);
        }
    }

    for (uint32_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        free(all[i]->data);
    free(live_leaf.data);
    free(live_issuer.data);
    free(live_crl.data);
    printf("revocation fixture tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
