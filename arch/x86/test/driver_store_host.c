#include "../kernel/driver_store.c"
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
static unsigned failures, checks, flushes, nonce, fail_write, fail_flush;
static bool mounted = true;
typedef struct { char path[80]; uint8_t *data; unsigned size, refs; uint16_t mode; uint64_t revision; } file_t;
static file_t files[16];
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
bool osfs3_is_mounted(void) { return mounted; }
void *osfs3_find_ci(const char *s)
{ for (unsigned i = 0; i < 16; i++) if (files[i].path[0] && !strcmp(files[i].path, s)) return &files[i]; return NULL; }
uint32_t osfs3_resolve_path_ci(const char *s) { return osfs3_find_ci(s) != NULL; }
int osfs3_file_retain(void *p) { ((file_t *)p)->refs++; return 0; }
void osfs3_file_release(void *p) { CHECK(((file_t *)p)->refs > 0); ((file_t *)p)->refs--; }
uint64_t osfs3_file_size(const void *p) { return ((const file_t *)p)->size; }
uint64_t osfs3_file_revision(const void *p) { return ((const file_t *)p)->revision; }
int osfs3_file_get_mode(const void *p, uint16_t *m) { *m = ((const file_t *)p)->mode; return 0; }
int osfs3_file_set_mode(void *p, uint16_t m) { ((file_t *)p)->mode = 0100000 | m; return 0; }
int osfs3_read_file(const void *p, uint64_t off, void *b, uint64_t n)
{ const file_t *f = p; if (off > f->size || n > f->size - off) return -1; memcpy(b, f->data + off, n); return 0; }
void *osfs3_create(const char *name, uint64_t size)
{
    if (osfs3_find_ci(name) || strlen(name) >= 80) return NULL;
    for (unsigned i = 0; i < 16; i++) if (!files[i].path[0]) {
        file_t *f = &files[i]; strcpy(f->path, name); f->data = malloc(size + 1); f->size = size;
        f->mode = 0100644; f->revision = 2; f->refs = 0; return f;
    }
    return NULL;
}
int osfs3_write_data(void *p, uint64_t off, const void *b, uint64_t n)
{ file_t *f = p; if (fail_write || off > f->size || n > f->size - off) return -1; memcpy(f->data + off, b, n); f->revision += 2; return 0; }
int osfs3_delete(const char *name)
{ file_t *f = osfs3_find_ci(name); if (!f || f->refs) return -1; free(f->data); memset(f, 0, sizeof(*f)); return 0; }
int osfs3_rename(const char *old, const char *dest, bool replace)
{
    file_t *f = osfs3_find_ci(old), *target = osfs3_find_ci(dest);
    if (!f || (target && (!replace || target->refs))) return -1;
    if (target) osfs3_delete(dest);
    strcpy(f->path, dest); return 0;
}
int random_secure_bytes(void *p, size_t n) { memset(p, ++nonce, n); return 0; }
int disk_flush(void) { flushes++; return fail_flush ? -1 : 0; }
static unsigned count_files(void) { unsigned n = 0; for (unsigned i = 0; i < 16; i++) n += !!files[i].path[0]; return n; }
int main(void)
{
    uint8_t data[4096], out[4096]; unsigned n;
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = i;
    CHECK(driver_config_read("wlan", out, sizeof(out), &n) == 1);
    CHECK(!driver_config_write("wlan", data, sizeof(data)) && flushes == 2);
    file_t *f = osfs3_find_ci(".driver-wlan.conf"); CHECK(f && f->mode == 0100600);
    CHECK(!driver_config_read("wlan", out, sizeof(out), &n) && n == sizeof(data) && !memcmp(data, out, n));
    CHECK(driver_config_read("wlan", out, 2, &n) < 0);
    f->mode = 0100644; CHECK(driver_config_read("wlan", out, sizeof(out), &n) < 0); f->mode = 0100600;
    f->data[44] ^= 1;
    CHECK(driver_config_read("wlan", out, sizeof(out), &n) < 0);
    CHECK(driver_config_write("wlan", data, sizeof(data)) < 0 && count_files() == 1);
    f->data[44] ^= 1;
    fail_write = 1; CHECK(driver_config_write("wlan", "new", 3) < 0 && count_files() == 1); fail_write = 0;
    fail_flush = 1; CHECK(driver_config_write("wlan", "new", 3) < 0 && count_files() == 1); fail_flush = 0;
    CHECK(!driver_config_read("wlan", out, sizeof(out), &n) && !memcmp(data, out, n));
    CHECK(!driver_config_write("wlan", "new", 3) && count_files() == 1);
    CHECK(!driver_config_read("wlan", out, sizeof(out), &n) && n == 3 && !memcmp(out, "new", 3));
    CHECK(driver_config_write("../wlan", data, 10) < 0 && driver_config_write("wlan", data, 4097) < 0);
    mounted = false; CHECK(driver_config_write("sshd", "x", 1) < 0 && count_files() == 1);
    mounted = true;
    CHECK(!driver_file_publish("firmware", data, sizeof(data), false));
    CHECK(driver_file_publish("firmware", "bad", 3, false) < 0);
    CHECK(!driver_file_read("firmware", out, sizeof(out), &n, false) && n == sizeof(data));
    for (unsigned i = 0; i < 16; i++) { CHECK(!files[i].refs); if (files[i].path[0]) osfs3_delete(files[i].path); }
    printf("driver-store: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
