/* Bounded binary RPC carried by an authenticated SSH subsystem, not a shell. */
#include "file_sync.h"
#include "crypto.h"

static const file_sync_ops_t *fs;
static bool reader, writer, listing, greeted;
static uint64_t read_size, write_size, written;
static sha256_ctx write_hash;
static unsigned char block[FILE_SYNC_BLOCK];

static uint64_t get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
           (unsigned)p[2] << 8 | p[3];
}

static void put64(unsigned char *p, uint64_t v)
{
    for (unsigned i = 8; i; v >>= 8) p[--i] = (unsigned char)v;
}

static bool path_arg(const unsigned char *p, unsigned n, char *path)
{
    if (!n || n >= FILE_SYNC_PATH) return false;
    if (n == 1 && p[0] == '.') { strcpy(path, "/"); return true; }
    unsigned start = p[0] == '/' ? 1 : 0;
    for (unsigned i = start; i <= n; i++) {
        if (i < n && (!p[i] || p[i] < 32 || p[i] == 127 ||
                      p[i] == '\\' || p[i] == ':')) return false;
        if (i == n || p[i] == '/') {
            unsigned len = i - start;
            if ((!len && n != 1) || len > 255 ||
                (len == 1 && p[start] == '.') ||
                (len == 2 && p[start] == '.' && p[start + 1] == '.'))
                return false;
            start = i + 1;
        }
    }
    memcpy(path, p, n);
    path[n] = 0;
    return true;
}

void file_sync_reset(void *arg)
{
    (void)arg;
    if (fs) {
        if (reader) fs->close();
        if (writer) fs->abort();
    }
    reader = writer = listing = greeted = false;
    read_size = write_size = written = 0;
    memset(&write_hash, 0, sizeof(write_hash));
    memset(block, 0, sizeof(block));
}

void file_sync_init(const file_sync_ops_t *ops)
{
    file_sync_reset(NULL);
    fs = ops;
}

static int append(const void *data, unsigned n)
{
    if (!writer || !n || n > FILE_SYNC_BLOCK || n > write_size - written)
        return SYNC_INVALID;
    int rc = fs->write(written, data, n);
    if (!rc) rc = fs->readback(written, block, n);
    if (rc) {
        fs->abort(); writer = false;
        return rc;
    }
    sha256_update(&write_hash, block, n);
    written += n;
    return SYNC_OK;
}

int file_sync_request(void *arg, const unsigned char *p, unsigned n,
                      unsigned char *out, unsigned capacity, int *finished)
{
    (void)arg;
    char path[FILE_SYNC_PATH];
    unsigned kind = 0, bytes = 0;
    uint64_t size = 0, offset = 0;
    int rc = SYNC_INVALID;
    *finished = 0;
    if (!fs || !p || !n || n > FILE_SYNC_FRAME || capacity < FILE_SYNC_FRAME)
        return -1;
    unsigned op = *p++; n--;
    if (!greeted && op != 'V') goto result;
    switch (op) {
    case 'V':
        if (n || greeted) break;
        greeted = true;
        memcpy(out + 1, "OSYNC001", 8);
        out[9] = 0; out[10] = 0; out[11] = 0x80; out[12] = 0;
        bytes = 12; rc = SYNC_OK;
        break;
    case 'S':
        if (!path_arg(p, n, path)) break;
        rc = fs->stat(path, &kind, &size);
        if (!rc) { out[1] = kind; put64(out + 2, size); bytes = 9; }
        break;
    case 'L':
        if (!path_arg(p, n, path)) break;
        rc = fs->list(path); listing = !rc;
        break;
    case 'N':
        if (n || !listing) break;
        rc = fs->next(path, sizeof(path), &kind);
        if (!rc) {
            unsigned len = (unsigned)strlen(path);
            if (!len || len > 255) { rc = SYNC_IO; break; }
            out[1] = kind; memcpy(out + 2, path, len); bytes = len + 1;
        } else listing = false;
        break;
    case 'O':
        if (reader) { rc = SYNC_BUSY; break; }
        if (!path_arg(p, n, path)) break;
        rc = fs->open(path, &read_size);
        if (!rc) { reader = true; put64(out + 1, read_size); bytes = 8; }
        break;
    case 'H': case 'R': case 'C':
        if (n != 12 || !reader) break;
        offset = get64(p); bytes = get32(p + 8);
        if (!bytes || bytes > FILE_SYNC_BLOCK || offset > read_size ||
            bytes > read_size - offset) { bytes = 0; break; }
        rc = fs->read(offset, block, bytes);
        if (rc) break;
        if (op == 'H') { sha256(block, bytes, out + 1); bytes = 32; }
        else if (op == 'R') memcpy(out + 1, block, bytes);
        else { rc = append(block, bytes); bytes = 0; }
        break;
    case 'X':
        if (n || !reader) break;
        rc = fs->read(0, block, 0);
        fs->close(); reader = false;
        break;
    case 'B':
        if (writer) { rc = SYNC_BUSY; break; }
        if (n < 9 || !path_arg(p + 8, n - 8, path)) break;
        size = get64(p);
        /* OsitoFS3's block count is 32-bit. Avoid size-rounding overflow. */
        if (size > 0xffffffffULL * 65536) break;
        rc = fs->begin(path, size);
        if (!rc) {
            writer = true; write_size = size; written = 0;
            sha256_init(&write_hash);
        }
        break;
    case 'W': rc = append(p, n); break;
    case 'F': {
        if (n != 32 || !writer || written != write_size) break;
        unsigned char digest[32];
        sha256_final(&write_hash, digest);
        rc = memcmp(p, digest, 32) ? SYNC_HASH : fs->commit();
        if (rc) fs->abort();
        writer = false;
        memset(&write_hash, 0, sizeof(write_hash));
        break;
    }
    case 'A':
        if (n) break;
        if (writer) fs->abort();
        writer = false; rc = SYNC_OK;
        break;
    case 'M':
        if (path_arg(p, n, path)) rc = fs->mkdir(path);
        break;
    case 'Q':
        if (n) break;
        file_sync_reset(NULL); *finished = 1; rc = SYNC_OK;
        break;
    }
result:
    out[0] = (unsigned char)rc;
    return rc ? 1 : (int)bytes + 1;
}
