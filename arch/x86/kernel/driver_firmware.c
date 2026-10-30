#include "driver_firmware.h"
#include "driver_store.h"
#include "crypto.h"
#include "http.h"
#include "net.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern uint8_t *net_get_ip_ptr(void);
extern uint64_t idt_get_monotonic_ns(void);

#define FW_BASE "/kernel-firmware/linux-firmware/-/raw/797d34e622b2262ca0777e98fd40b1d29034169d/"
const driver_firmware_t driver_ax200_firmware = {
    "iwlwifi-cc-a0-77.ucode", "gitlab.com", FW_BASE "intel/iwlwifi/iwlwifi-cc-a0-77.ucode",
    "94f5fb915f2074f98059365e11cca313e25a6fa3b7cd6772e80b3ba5edfb966b", 4 * 1024 * 1024
};
const driver_firmware_t driver_ax200_license = {
    "LICENCE.iwlwifi_firmware", "gitlab.com", FW_BASE "LICENSES/LICENCE.iwlwifi_firmware",
    "16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5", 16384
};

static bool digest_matches(const void *data, unsigned size, const char *expected)
{
    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    sha256(data, size, digest);
    if (strlen(expected) != 64) return false;
    for (unsigned i = 0; i < 32; i++)
        if (hex[digest[i] >> 4] != expected[2 * i] || hex[digest[i] & 15] != expected[2 * i + 1]) return false;
    return true;
}

typedef struct { uint8_t *data; unsigned size, capacity; uint64_t deadline, epoch; } download_t;
static int download_body(const void *data, uint32_t size, void *arg)
{
    download_t *d = arg;
    if (size > d->capacity - d->size || idt_get_monotonic_ns() > d->deadline ||
        net_link_epoch() != d->epoch) return -1;
    memcpy(d->data + d->size, data, size); d->size += size;
    return 0;
}

int driver_firmware_get(const driver_firmware_t *fw, void **out, unsigned *size, const char **error)
{
    *out = NULL; *size = 0; *error = NULL;
    uint8_t *data = kmalloc(fw->max_size);
    if (!data) { *error = "firmware allocation failed"; return -1; }
    unsigned n = 0;
    int rc = driver_file_read(fw->file, data, fw->max_size, &n, false);
    if (rc < 0) { *error = "firmware file is invalid or unreadable"; goto fail; }
    if (rc == 1) {
        uint8_t *ip = net_get_ip_ptr();
        if (!net_link_ready() || !ip[0] || (ip[0] == 169 && ip[1] == 254)) {
            *error = "firmware missing; copy it or connect another network adapter"; goto fail;
        }
        http_session_t *http = kmalloc(sizeof(*http));
        http_response_t *response = kmalloc(sizeof(*response));
        rc = -1;
        *error = "firmware HTTPS download failed or client busy";
        if (http && response && !http_open(http, fw->host)) {
            download_t d = {data, 0, fw->max_size,
                idt_get_monotonic_ns() + 120000000000ULL, net_link_epoch()};
            const char *headers[] = {"Accept-Encoding: identity", "Connection: close", NULL};
            if (!http_request(http, "GET", fw->path, fw->host, headers, NULL, 0, response) &&
                response->status_code == 200 && response->content_length <= fw->max_size &&
                http_read_body(http, response, download_body, &d) >= 0) {
                n = d.size; rc = 0;
            }
            http_close(http);
        }
        kfree(http); kfree(response);
        if (rc) goto fail;
        if (!digest_matches(data, n, fw->sha256)) {
            rc = -1; *error = "downloaded firmware SHA-256 mismatch"; goto fail;
        }
        if (driver_file_publish(fw->file, data, n, false)) {
            rc = -1; *error = "cannot publish verified firmware"; goto fail;
        }
    }
    if (!digest_matches(data, n, fw->sha256)) {
        rc = -1; *error = "local firmware SHA-256 mismatch (file preserved)"; goto fail;
    }
    *out = data; *size = n; *error = NULL; return 0;
fail:
    kfree(data); return rc;
}
