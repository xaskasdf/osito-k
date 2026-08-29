/* System certificate root store and issuer index. */

#include "certmgr.h"
#include "crl.h"
#include "crypto.h"
#include "ocsp.h"
#include "x509.h"
#include "../fs/vfs.h"
#include "../include/paging.h"

extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern bool osfs2_is_mounted(void);
extern int sched_spawn(const char *name, void (*entry)(void));
extern void sched_yield(void);
extern int sched_sleep_ticks(uint64_t ticks);

#define CERTMGR_STORE_PATH       "tls/ca-roots.bin"
#define CERTMGR_MAGIC_SIZE       8U
#define CERTMGR_HEADER_SIZE      56U
#define CERTMGR_VERSION          2U
#define CERTMGR_ENTRY_HEADER     44U
#define CERTMGR_MAX_STORE_SIZE   (4U * 1024U * 1024U)
#define CERTMGR_MAX_ROOTS        256U
#define CERTMGR_MAX_CERT_SIZE    16384U
#define CERTMGR_MAX_TRUST_OIDS   64U
#define CERTMGR_MAX_TRUST_BYTES  4096U
#define CERTMGR_BUCKETS          256U
#define CERTMGR_REVOCATION_SLOTS 64U
#define CERTMGR_REVOCATION_FAILURE_SLOTS 16U
#define CERTMGR_REVOCATION_QUEUE_SLOTS 16U
#define CERTMGR_REVOCATION_RETRY_SECONDS 30U

#define CERTMGR_UNINITIALIZED    0
#define CERTMGR_READY            1
#define CERTMGR_FAILED          -1

typedef struct {
    const uint8_t *der;
    uint32_t der_len;
    const uint8_t *fingerprint;
    const uint8_t *trust_oids;
    uint32_t trust_oids_len;
    uint32_t trust_oid_count;
    uint8_t trust_all;
    const uint8_t *subject_name;
    uint32_t subject_name_len;
    const uint8_t *subject_key_id;
    uint32_t subject_key_id_len;
    int16_t next_subject;
    int16_t next_key_id;
    int16_t next_fingerprint;
} certmgr_root_t;

typedef struct {
    uint8_t fingerprint[32];
    uint32_t valid_until;
    uint32_t last_use;
    int8_t status;
    uint8_t valid;
} certmgr_revocation_entry_t;

typedef struct {
    uint8_t fingerprint[32];
    uint32_t retry_after;
    uint32_t last_use;
    uint8_t valid;
} certmgr_revocation_failure_t;

typedef struct {
    uint8_t fingerprint[32];
    uint8_t issuer_fingerprint[32];
    uint8_t *cert;
    uint8_t *issuer;
    uint32_t cert_len;
    uint32_t issuer_len;
    uint32_t validation_time;
    uint8_t state;
} certmgr_revocation_request_t;

enum {
    CERTMGR_REQUEST_FREE = 0,
    CERTMGR_REQUEST_PENDING = 1,
    CERTMGR_REQUEST_RUNNING = 2,
};

static const uint8_t certmgr_magic[CERTMGR_MAGIC_SIZE] = {
    'O', 'S', 'I', 'T', 'O', 'C', 'A', 0
};

static certmgr_root_t roots[CERTMGR_MAX_ROOTS];
static int16_t subject_heads[CERTMGR_BUCKETS];
static int16_t key_id_heads[CERTMGR_BUCKETS];
static int16_t fingerprint_heads[CERTMGR_BUCKETS];
static uint32_t root_count;
static uint8_t *store_data;
static uint32_t store_pages;
static volatile uint32_t manager_lock;
static volatile int manager_state;
static certmgr_revocation_entry_t revocation_cache[CERTMGR_REVOCATION_SLOTS];
static certmgr_revocation_failure_t
    revocation_failures[CERTMGR_REVOCATION_FAILURE_SLOTS];
static certmgr_revocation_request_t
    revocation_queue[CERTMGR_REVOCATION_QUEUE_SLOTS];
static volatile uint32_t revocation_lock;
static volatile uint32_t revocation_queue_lock;
static volatile int revocation_worker_state;
static uint32_t revocation_generation;

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t index_hash(const uint8_t *data, uint32_t len)
{
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash & (CERTMGR_BUCKETS - 1U);
}

static void certmgr_lock(void)
{
    while (__sync_lock_test_and_set(&manager_lock, 1))
        __asm__ volatile ("pause");
}

static void certmgr_unlock(void)
{
    __sync_lock_release(&manager_lock);
}

static void certmgr_revocation_lock(void)
{
    while (__sync_lock_test_and_set(&revocation_lock, 1))
        __asm__ volatile ("pause");
}

static void certmgr_revocation_unlock(void)
{
    __sync_lock_release(&revocation_lock);
}

static void certmgr_revocation_queue_lock(void)
{
    while (__sync_lock_test_and_set(&revocation_queue_lock, 1))
        __asm__ volatile ("pause");
}

static void certmgr_revocation_queue_unlock(void)
{
    __sync_lock_release(&revocation_queue_lock);
}

static void release_store(void)
{
    if (!store_data) return;
    mem_free_pages((void *)kvirt_to_phys(store_data), store_pages);
    store_data = NULL;
    store_pages = 0;
}

static void reset_indexes(void)
{
    root_count = 0;
    for (uint32_t i = 0; i < CERTMGR_BUCKETS; i++) {
        subject_heads[i] = -1;
        key_id_heads[i] = -1;
        fingerprint_heads[i] = -1;
    }
}

static int load_failed(const char *reason)
{
    release_store();
    reset_indexes();
    manager_state = CERTMGR_FAILED;
    serial_puts("[CERTMGR] root store unavailable: ");
    serial_puts(reason);
    serial_puts("\n");
    return -1;
}

static int load_deferred(void)
{
    release_store();
    reset_indexes();
    manager_state = CERTMGR_UNINITIALIZED;
    return -1;
}

static void index_root(uint32_t index)
{
    certmgr_root_t *root = &roots[index];
    uint32_t bucket = index_hash(root->subject_name, root->subject_name_len);
    root->next_subject = subject_heads[bucket];
    subject_heads[bucket] = (int16_t)index;

    if (root->subject_key_id_len) {
        bucket = index_hash(root->subject_key_id, root->subject_key_id_len);
        root->next_key_id = key_id_heads[bucket];
        key_id_heads[bucket] = (int16_t)index;
    } else {
        root->next_key_id = -1;
    }

    bucket = index_hash(root->fingerprint, 32);
    root->next_fingerprint = fingerprint_heads[bucket];
    fingerprint_heads[bucket] = (int16_t)index;
}

static int trust_oid_blob_valid(const uint8_t *data, uint32_t len,
                                uint32_t count)
{
    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t start = offset;
        int saw_dot = 0;
        while (offset < len && data[offset]) {
            uint8_t c = data[offset++];
            if (c == '.') saw_dot = 1;
            else if (c < '0' || c > '9') return 0;
        }
        if (offset == start || offset >= len || !saw_dot) return 0;
        offset++;
    }
    return offset == len;
}

static int load_store_locked(void)
{
    vfs_node_t node;
    if (!vfs_find(CERTMGR_STORE_PATH, VFS_MODE_POSIX, &node)) {
        if (!osfs2_is_mounted()) return load_deferred();
        return load_failed("tls/ca-roots.bin not found");
    }
    if (node.size < CERTMGR_HEADER_SIZE ||
        node.size > CERTMGR_MAX_STORE_SIZE)
        return load_failed("invalid file size");

    store_pages = (uint32_t)((node.size + 4095U) / 4096U);
    void *physical = mem_alloc_pages(store_pages);
    if (!physical) return load_failed("out of memory");
    store_data = (uint8_t *)PHYS_TO_VIRT(physical);
    int amount = vfs_read(&node, 0, store_data, node.size);
    if (amount < 0 || (uint64_t)amount != node.size)
        return load_failed("short read");

    if (!bytes_equal(store_data, certmgr_magic, CERTMGR_MAGIC_SIZE) ||
        read_le32(store_data + 8) != CERTMGR_VERSION ||
        read_le32(store_data + 12) != CERTMGR_HEADER_SIZE)
        return load_failed("bad header");

    uint32_t declared_count = read_le32(store_data + 16);
    uint32_t payload_size = read_le32(store_data + 20);
    if (!declared_count || declared_count > CERTMGR_MAX_ROOTS ||
        payload_size != (uint32_t)node.size - CERTMGR_HEADER_SIZE)
        return load_failed("invalid manifest");

    uint8_t payload_hash[32];
    sha256(store_data + CERTMGR_HEADER_SIZE, payload_size, payload_hash);
    if (!bytes_equal(payload_hash, store_data + 24, 32))
        return load_failed("payload digest mismatch");

    reset_indexes();
    uint32_t offset = CERTMGR_HEADER_SIZE;
    for (uint32_t i = 0; i < declared_count; i++) {
        if (offset > (uint32_t)node.size ||
            (uint32_t)node.size - offset < CERTMGR_ENTRY_HEADER)
            return load_failed("truncated entry");
        uint32_t der_len = read_le32(store_data + offset);
        uint32_t trust_count = read_le32(store_data + offset + 4);
        uint32_t trust_len = read_le32(store_data + offset + 8);
        if (der_len < 4 || der_len > CERTMGR_MAX_CERT_SIZE ||
            trust_len > CERTMGR_MAX_TRUST_BYTES ||
            trust_len > (uint32_t)node.size - offset - CERTMGR_ENTRY_HEADER ||
            der_len > (uint32_t)node.size - offset -
                      CERTMGR_ENTRY_HEADER - trust_len)
            return load_failed("invalid certificate length");

        int trust_all = trust_count == UINT32_MAX;
        if ((trust_all && trust_len != 0) ||
            (!trust_all &&
             (trust_count > CERTMGR_MAX_TRUST_OIDS ||
              !trust_oid_blob_valid(store_data + offset +
                                    CERTMGR_ENTRY_HEADER,
                                    trust_len, trust_count))))
            return load_failed("invalid trust policy");

        certmgr_root_t *root = &roots[i];
        root->fingerprint = store_data + offset + 12;
        root->trust_all = (uint8_t)trust_all;
        root->trust_oid_count = trust_all ? 0 : trust_count;
        root->trust_oids = store_data + offset + CERTMGR_ENTRY_HEADER;
        root->trust_oids_len = trust_len;
        root->der = root->trust_oids + trust_len;
        root->der_len = der_len;
        uint8_t digest[32];
        sha256(root->der, root->der_len, digest);
        if (!bytes_equal(digest, root->fingerprint, 32))
            return load_failed("certificate digest mismatch");
        if (x509_get_subject_name_der(root->der, root->der_len,
                                      &root->subject_name,
                                      &root->subject_name_len) < 0)
            return load_failed("malformed root certificate");
        if (x509_get_subject_key_id(root->der, root->der_len,
                                    &root->subject_key_id,
                                    &root->subject_key_id_len) < 0) {
            root->subject_key_id = NULL;
            root->subject_key_id_len = 0;
        }
        index_root(i);
        root_count++;
        offset = (offset + CERTMGR_ENTRY_HEADER + trust_len +
                  der_len + 3U) & ~3U;
    }
    if (offset != (uint32_t)node.size)
        return load_failed("trailing bundle data");

    manager_state = CERTMGR_READY;
    serial_puts("[CERTMGR] loaded ");
    serial_putdec(root_count);
    serial_puts(" system roots (bytes=");
    serial_putdec(node.size);
    serial_puts(")\n");
    return (int)root_count;
}

int certmgr_load_system_roots(void)
{
    if (manager_state == CERTMGR_READY) return (int)root_count;
    if (manager_state == CERTMGR_FAILED) return -1;
    certmgr_lock();
    int result;
    if (manager_state == CERTMGR_UNINITIALIZED)
        result = load_store_locked();
    else if (manager_state == CERTMGR_READY)
        result = (int)root_count;
    else
        result = -1;
    certmgr_unlock();
    return result;
}

int certmgr_root_count(void)
{
    return certmgr_load_system_roots() < 0 ? 0 : (int)root_count;
}

int certmgr_is_trust_anchor(const uint8_t *cert, uint32_t cert_len)
{
    return certmgr_check_trust_anchor(cert, cert_len, NULL, 0, 1, NULL);
}

static int stored_oid_equal(const uint8_t *stored, uint32_t stored_len,
                            const char *requested)
{
    uint32_t i = 0;
    while (i < stored_len && requested[i] &&
           stored[i] == (uint8_t)requested[i])
        i++;
    return i == stored_len && requested[i] == '\0';
}

static int root_has_usage(const certmgr_root_t *root, const char *oid)
{
    uint32_t offset = 0;
    for (uint32_t i = 0; i < root->trust_oid_count; i++) {
        uint32_t start = offset;
        while (offset < root->trust_oids_len && root->trust_oids[offset])
            offset++;
        if (stored_oid_equal(root->trust_oids + start, offset - start, oid))
            return 1;
        offset++;
    }
    return 0;
}

static int root_allows_usages(const certmgr_root_t *root,
                              const char *const *usage_oids,
                              uint32_t usage_count, int match_all)
{
    if (!usage_count || root->trust_all) return 1;
    for (uint32_t i = 0; i < usage_count; i++) {
        int found = usage_oids[i] && root_has_usage(root, usage_oids[i]);
        if (match_all && !found) return 0;
        if (!match_all && found) return 1;
    }
    return match_all ? 1 : 0;
}

int certmgr_check_trust_anchor(const uint8_t *cert, uint32_t cert_len,
                               const char *const *usage_oids,
                               uint32_t usage_count, int match_all,
                               int *usage_allowed)
{
    if (usage_allowed) *usage_allowed = 0;
    if (usage_count && !usage_oids) return 0;
    if (!cert || !cert_len || certmgr_load_system_roots() < 0) return 0;
    uint8_t digest[32];
    sha256(cert, cert_len, digest);
    int16_t index = fingerprint_heads[index_hash(digest, sizeof digest)];
    while (index >= 0) {
        certmgr_root_t *root = &roots[index];
        if (bytes_equal(digest, root->fingerprint, sizeof digest) &&
            cert_len == root->der_len &&
            bytes_equal(cert, root->der, cert_len)) {
            if (usage_allowed)
                *usage_allowed = root_allows_usages(root, usage_oids,
                                                    usage_count, match_all);
            return 1;
        }
        index = root->next_fingerprint;
    }
    return 0;
}

int certmgr_issuer_iter_begin(certmgr_issuer_iter_t *iter,
                              const uint8_t *child, uint32_t child_len)
{
    if (!iter || !child || certmgr_load_system_roots() < 0) return -1;
    if (x509_get_issuer_name_der(child, child_len, &iter->issuer_name,
                                 &iter->issuer_name_len) < 0)
        return -1;
    if (x509_get_authority_key_id(child, child_len,
                                  &iter->authority_key_id,
                                  &iter->authority_key_id_len) == 0) {
        iter->phase = 0;
        iter->next_index = key_id_heads[index_hash(iter->authority_key_id,
                                                   iter->authority_key_id_len)];
    } else {
        iter->authority_key_id = NULL;
        iter->authority_key_id_len = 0;
        iter->phase = 1;
        iter->next_index = subject_heads[index_hash(iter->issuer_name,
                                                    iter->issuer_name_len)];
    }
    return 0;
}

int certmgr_issuer_iter_next(certmgr_issuer_iter_t *iter,
                             const uint8_t **cert, uint32_t *cert_len)
{
    if (!iter || !cert || !cert_len || manager_state != CERTMGR_READY)
        return 0;
    for (;;) {
        if (iter->next_index < 0) {
            if (iter->phase != 0) return 0;
            iter->phase = 1;
            iter->next_index = subject_heads[index_hash(iter->issuer_name,
                                                        iter->issuer_name_len)];
            continue;
        }

        certmgr_root_t *root = &roots[iter->next_index];
        iter->next_index = iter->phase == 0
            ? root->next_key_id : root->next_subject;
        if (root->subject_name_len != iter->issuer_name_len ||
            !bytes_equal(root->subject_name, iter->issuer_name,
                         iter->issuer_name_len))
            continue;

        if (iter->phase == 0) {
            if (root->subject_key_id_len != iter->authority_key_id_len ||
                !bytes_equal(root->subject_key_id, iter->authority_key_id,
                             iter->authority_key_id_len))
                continue;
        } else if (iter->authority_key_id_len && root->subject_key_id_len) {
            /* A matching SKI was already yielded in phase zero; a mismatch
             * is a different key under the same X.500 name. */
            continue;
        }

        *cert = root->der;
        *cert_len = root->der_len;
        return 1;
    }
}

int certmgr_revocation_lookup(const uint8_t *cert, uint32_t cert_len,
                              uint32_t validation_time,
                              uint32_t *valid_until)
{
    if (valid_until) *valid_until = 0;
    if (!cert || !cert_len || !validation_time)
        return CERTMGR_REVOCATION_UNAVAILABLE;

    uint8_t fingerprint[32];
    sha256(cert, cert_len, fingerprint);
    int result = CERTMGR_REVOCATION_UNAVAILABLE;

    certmgr_revocation_lock();
    for (uint32_t i = 0; i < CERTMGR_REVOCATION_SLOTS; i++) {
        certmgr_revocation_entry_t *entry = &revocation_cache[i];
        if (!entry->valid ||
            !bytes_equal(entry->fingerprint, fingerprint,
                         sizeof(fingerprint)))
            continue;
        if (!entry->valid_until || validation_time > entry->valid_until) {
            entry->valid = 0;
            break;
        }
        entry->last_use = ++revocation_generation;
        result = entry->status;
        if (valid_until) *valid_until = entry->valid_until;
        break;
    }
    certmgr_revocation_unlock();
    return result;
}

void certmgr_revocation_store(const uint8_t *cert, uint32_t cert_len,
                              certmgr_revocation_status_t status,
                              uint32_t valid_until)
{
    if (!cert || !cert_len || !valid_until ||
        status < CERTMGR_REVOCATION_GOOD ||
        status > CERTMGR_REVOCATION_UNKNOWN)
        return;

    uint8_t fingerprint[32];
    sha256(cert, cert_len, fingerprint);

    certmgr_revocation_lock();
    uint32_t slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (uint32_t i = 0; i < CERTMGR_REVOCATION_SLOTS; i++) {
        certmgr_revocation_entry_t *entry = &revocation_cache[i];
        if (entry->valid &&
            bytes_equal(entry->fingerprint, fingerprint,
                        sizeof(fingerprint))) {
            slot = i;
            oldest = 0;
            break;
        }
        if (!entry->valid) {
            slot = i;
            oldest = 0;
            break;
        }
        if (entry->last_use < oldest) {
            oldest = entry->last_use;
            slot = i;
        }
    }

    certmgr_revocation_entry_t *entry = &revocation_cache[slot];
    for (uint32_t i = 0; i < sizeof(fingerprint); i++)
        entry->fingerprint[i] = fingerprint[i];
    entry->valid_until = valid_until;
    entry->last_use = ++revocation_generation;
    entry->status = (int8_t)status;
    entry->valid = 1;
    certmgr_revocation_unlock();
}

static int revocation_failure_lookup(const uint8_t fingerprint[32],
                                     uint32_t validation_time)
{
    int found = 0;
    certmgr_revocation_lock();
    for (uint32_t i = 0; i < CERTMGR_REVOCATION_FAILURE_SLOTS; i++) {
        certmgr_revocation_failure_t *entry = &revocation_failures[i];
        if (!entry->valid ||
            !bytes_equal(entry->fingerprint, fingerprint, 32))
            continue;
        if (!validation_time || validation_time >= entry->retry_after) {
            entry->valid = 0;
            break;
        }
        entry->last_use = ++revocation_generation;
        found = 1;
        break;
    }
    certmgr_revocation_unlock();
    return found;
}

static void revocation_failure_update(const uint8_t fingerprint[32],
                                      uint32_t validation_time, int failed)
{
    certmgr_revocation_lock();
    uint32_t slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (uint32_t i = 0; i < CERTMGR_REVOCATION_FAILURE_SLOTS; i++) {
        certmgr_revocation_failure_t *entry = &revocation_failures[i];
        if (entry->valid &&
            bytes_equal(entry->fingerprint, fingerprint, 32)) {
            if (!failed) entry->valid = 0;
            slot = i;
            oldest = 0;
            break;
        }
        if (!entry->valid) {
            slot = i;
            oldest = 0;
            if (!failed) break;
        } else if (entry->last_use < oldest) {
            oldest = entry->last_use;
            slot = i;
        }
    }
    if (failed) {
        certmgr_revocation_failure_t *entry = &revocation_failures[slot];
        for (uint32_t i = 0; i < 32; i++)
            entry->fingerprint[i] = fingerprint[i];
        entry->retry_after = validation_time +
                             CERTMGR_REVOCATION_RETRY_SECONDS;
        entry->last_use = ++revocation_generation;
        entry->valid = 1;
    }
    certmgr_revocation_unlock();
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

static int revocation_queue_claim(uint32_t *slot_out)
{
    int found = 0;
    certmgr_revocation_queue_lock();
    for (uint32_t i = 0; i < CERTMGR_REVOCATION_QUEUE_SLOTS; i++) {
        if (revocation_queue[i].state != CERTMGR_REQUEST_PENDING)
            continue;
        revocation_queue[i].state = CERTMGR_REQUEST_RUNNING;
        *slot_out = i;
        found = 1;
        break;
    }
    certmgr_revocation_queue_unlock();
    return found;
}

static void revocation_queue_complete(uint32_t slot)
{
    uint8_t *allocation;
    certmgr_revocation_queue_lock();
    allocation = revocation_queue[slot].cert;
    revocation_queue[slot].cert = NULL;
    revocation_queue[slot].issuer = NULL;
    revocation_queue[slot].cert_len = 0;
    revocation_queue[slot].issuer_len = 0;
    revocation_queue[slot].validation_time = 0;
    revocation_queue[slot].state = CERTMGR_REQUEST_FREE;
    certmgr_revocation_queue_unlock();
    kfree(allocation);
}

static void certmgr_revocation_worker(void)
{
    for (;;) {
        uint32_t slot;
        if (!revocation_queue_claim(&slot)) {
            if (sched_sleep_ticks(1) < 0) sched_yield();
            continue;
        }

        certmgr_revocation_request_t *request = &revocation_queue[slot];
        int status = certmgr_revocation_lookup(
            request->cert, request->cert_len, request->validation_time, NULL);
        if (status == CERTMGR_REVOCATION_UNAVAILABLE &&
            !revocation_failure_lookup(request->fingerprint,
                                       request->validation_time)) {
            int ocsp_status = ocsp_check(
                request->cert, request->cert_len,
                request->issuer, request->issuer_len);
            if (ocsp_status == OCSP_GOOD || ocsp_status == OCSP_REVOKED) {
                status = ocsp_status;
            } else {
                int crl_status = crl_check_revoked_with_issuer(
                    request->cert, request->cert_len,
                    request->issuer, request->issuer_len);
                if (crl_status == CRL_GOOD || crl_status == CRL_REVOKED)
                    status = crl_status;
                else if (ocsp_status == OCSP_UNKNOWN)
                    status = CERTMGR_REVOCATION_UNKNOWN;
            }

            revocation_failure_update(
                request->fingerprint, request->validation_time,
                status == CERTMGR_REVOCATION_UNAVAILABLE);
        }
        revocation_queue_complete(slot);
    }
}

static int certmgr_revocation_worker_ensure(void)
{
    int state = __atomic_load_n(&revocation_worker_state, __ATOMIC_ACQUIRE);
    if (state == 2) return 1;
    if (state != 0 ||
        !__sync_bool_compare_and_swap(&revocation_worker_state, 0, 1))
        return __atomic_load_n(&revocation_worker_state,
                               __ATOMIC_ACQUIRE) == 2;

    if (sched_spawn("cert-revoke", certmgr_revocation_worker) < 0) {
        __atomic_store_n(&revocation_worker_state, 0, __ATOMIC_RELEASE);
        serial_puts("[CERTMGR] revocation worker unavailable\n");
        return 0;
    }
    __atomic_store_n(&revocation_worker_state, 2, __ATOMIC_RELEASE);
    return 1;
}

static int revocation_queue_submit(const uint8_t fingerprint[32],
                                   const uint8_t *cert, uint32_t cert_len,
                                   const uint8_t *issuer, uint32_t issuer_len,
                                   uint32_t validation_time)
{
    if (cert_len > CERTMGR_MAX_CERT_SIZE ||
        issuer_len > CERTMGR_MAX_CERT_SIZE ||
        !certmgr_revocation_worker_ensure())
        return 0;

    uint64_t allocation_size = (uint64_t)cert_len + issuer_len;
    uint8_t *allocation = (uint8_t *)kmalloc(allocation_size);
    if (!allocation) return 0;
    copy_bytes(allocation, cert, cert_len);
    copy_bytes(allocation + cert_len, issuer, issuer_len);

    uint8_t issuer_fingerprint[32];
    sha256(issuer, issuer_len, issuer_fingerprint);
    int free_slot = -1;

    certmgr_revocation_queue_lock();
    for (uint32_t i = 0; i < CERTMGR_REVOCATION_QUEUE_SLOTS; i++) {
        certmgr_revocation_request_t *request = &revocation_queue[i];
        if (request->state == CERTMGR_REQUEST_FREE) {
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (bytes_equal(request->fingerprint, fingerprint, 32) &&
            bytes_equal(request->issuer_fingerprint,
                        issuer_fingerprint, 32)) {
            certmgr_revocation_queue_unlock();
            kfree(allocation);
            return 1;
        }
    }

    if (free_slot >= 0) {
        certmgr_revocation_request_t *request =
            &revocation_queue[(uint32_t)free_slot];
        copy_bytes(request->fingerprint, fingerprint, 32);
        copy_bytes(request->issuer_fingerprint, issuer_fingerprint, 32);
        request->cert = allocation;
        request->issuer = allocation + cert_len;
        request->cert_len = cert_len;
        request->issuer_len = issuer_len;
        request->validation_time = validation_time;
        request->state = CERTMGR_REQUEST_PENDING;
    }
    certmgr_revocation_queue_unlock();

    if (free_slot < 0) {
        kfree(allocation);
        return 0;
    }
    return 1;
}

int certmgr_revocation_resolve(const uint8_t *cert, uint32_t cert_len,
                               const uint8_t *issuer, uint32_t issuer_len,
                               uint32_t validation_time, int allow_network,
                               int *network_attempted)
{
    if (network_attempted) *network_attempted = 0;
    int status = certmgr_revocation_lookup(cert, cert_len, validation_time,
                                           NULL);
    if (status != CERTMGR_REVOCATION_UNAVAILABLE || !allow_network ||
        !cert || !cert_len || !issuer || !issuer_len)
        return status;

    uint8_t fingerprint[32];
    sha256(cert, cert_len, fingerprint);
    if (revocation_failure_lookup(fingerprint, validation_time)) {
        if (network_attempted) *network_attempted = 1;
        return CERTMGR_REVOCATION_UNAVAILABLE;
    }

    status = certmgr_revocation_lookup(cert, cert_len, validation_time, NULL);
    if (status != CERTMGR_REVOCATION_UNAVAILABLE) return status;
    if (revocation_failure_lookup(fingerprint, validation_time)) {
        if (network_attempted) *network_attempted = 1;
        return CERTMGR_REVOCATION_UNAVAILABLE;
    }

    if (network_attempted) *network_attempted = 1;
    (void)revocation_queue_submit(fingerprint, cert, cert_len,
                                  issuer, issuer_len, validation_time);
    return CERTMGR_REVOCATION_UNAVAILABLE;
}
