#include "driver_store.h"
#include "../fs/ositofs3.h"
#include "crypto.h"
#include "random_secure.h"

extern int disk_flush(void);
extern void *kmalloc(size_t);
extern void kfree(void *);

static void erase(void *p, unsigned n)
{
    volatile unsigned char *s = p;
    while (n--) *s++ = 0;
}

int driver_file_read(const char *path, void *data, unsigned capacity,
                     unsigned *size, bool private_file)
{
    if (!osfs3_is_mounted()) return -1;
    void *file = osfs3_find_ci(path);
    if (!file) return osfs3_resolve_path_ci(path) ? -1 : 1;
    if (osfs3_file_retain(file)) return -1;
    uint64_t revision = osfs3_file_revision(file), n = osfs3_file_size(file);
    uint16_t mode = 0;
    int rc = -1;
    if (!(revision & 1) && !osfs3_file_get_mode(file, &mode) &&
        (mode & 0170000) == 0100000 && (!private_file || !(mode & 0077)) &&
        n <= capacity && osfs3_read_file(file, 0, data, n) >= 0 &&
        osfs3_file_revision(file) == revision) { *size = (unsigned)n; rc = 0; }
    osfs3_file_release(file);
    return rc;
}

int driver_file_publish(const char *path, const void *data, unsigned size, bool replace)
{
    if (!osfs3_is_mounted()) return -1;
    unsigned char nonce[8];
    if (random_secure_bytes(nonce, sizeof(nonce))) return -1;
    char stage[40] = ".driver-stage-";
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; i++) {
        stage[14 + 2 * i] = hex[nonce[i] >> 4];
        stage[15 + 2 * i] = hex[nonce[i] & 15];
    }
    stage[30] = 0;
    void *file = osfs3_create(stage, size);
    if (!file) return -1;
    if (osfs3_file_retain(file)) { osfs3_delete(stage); return -1; }
    int rc = -1;
    if (!osfs3_file_set_mode(file, 0600) &&
        osfs3_write_data(file, 0, data, size) >= 0 && !disk_flush()) {
        unsigned char verify[1024];
        bool ok = true;
        for (unsigned off = 0; off < size;) {
            unsigned n = size - off;
            if (n > sizeof(verify)) n = sizeof(verify);
            if (osfs3_read_file(file, off, verify, n) < 0 ||
                memcmp(verify, (const unsigned char *)data + off, n)) { ok = false; break; }
            off += n;
        }
        erase(verify, sizeof(verify));
        if (ok && !osfs3_rename(stage, path, replace)) rc = disk_flush();
    }
    osfs3_file_release(file);
    if (osfs3_find_ci(stage) == file) osfs3_delete(stage);
    return rc;
}

static bool config_path(const char *id, char path[48])
{
    unsigned n = 0;
    while (id[n]) {
        if (n >= 24 || !((id[n] >= 'a' && id[n] <= 'z') ||
            (id[n] >= '0' && id[n] <= '9') || id[n] == '-')) return false;
        n++;
    }
    if (!n) return false;
    memcpy(path, ".driver-", 8); memcpy(path + 8, id, n); memcpy(path + 8 + n, ".conf", 6);
    return true;
}

int driver_config_read(const char *id, void *data, unsigned capacity, unsigned *size)
{
    char path[48];
    if (!config_path(id, path) || capacity > DRIVER_CONFIG_MAX) return -1;
    unsigned char *blob = kmalloc(DRIVER_CONFIG_MAX + 40);
    if (!blob) return -1;
    unsigned n = 0;
    int rc = driver_file_read(path, blob, DRIVER_CONFIG_MAX + 40, &n, true);
    if (!rc) {
        unsigned char hash[32];
        rc = -1;
        if (n >= 40 && n - 40 <= capacity && !memcmp(blob, "ODCFG001", 8)) {
            sha256(blob + 40, n - 40, hash);
            if (!memcmp(hash, blob + 8, 32)) {
                memcpy(data, blob + 40, n - 40); *size = n - 40; rc = 0;
            }
        }
    }
    erase(blob, DRIVER_CONFIG_MAX + 40); kfree(blob);
    return rc;
}

int driver_config_write(const char *id, const void *data, unsigned size)
{
    char path[48];
    if (!config_path(id, path) || size > DRIVER_CONFIG_MAX) return -1;
    unsigned char *blob = kmalloc(DRIVER_CONFIG_MAX + 40);
    if (!blob) return -1;
    unsigned previous = 0;
    /* Do not silently overwrite a corrupt or foreign-version configuration. */
    int rc = driver_config_read(id, blob, DRIVER_CONFIG_MAX, &previous);
    if (rc >= 0) {
        memcpy(blob, "ODCFG001", 8); sha256(data, size, blob + 8);
        memcpy(blob + 40, data, size);
        rc = driver_file_publish(path, blob, size + 40, true);
    }
    erase(blob, DRIVER_CONFIG_MAX + 40); kfree(blob);
    return rc;
}
