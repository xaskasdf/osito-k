#define VNET_HOST_TEST
#include "../drivers/virtio_net.c"
#include <threads.h>

extern int printf(const char *, ...);
extern void exit(int);
extern void *aligned_alloc(size_t, size_t);
extern void free(void *);

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
} host_desc_t;
typedef struct { uint16_t flags, idx, ring[256]; } host_avail_t;
typedef struct { uint32_t id, len; } host_used_elem_t;
typedef struct { uint16_t flags, idx; host_used_elem_t ring[256]; } host_used_t;
static void *allocations[16];
static unsigned allocation_count, checks;
static uint8_t config[64];
static uint16_t notify[4];
static _Thread_local bool irq_disabled;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
void serial_putdec(uint64_t n) { (void)n; }
void *mem_alloc_aligned(uint64_t bytes, uint64_t align)
{
    CHECK(allocation_count < 16);
    void *p = aligned_alloc(align, (bytes + align - 1) & ~(align - 1));
    CHECK(p != NULL);
    allocations[allocation_count++] = p;
    return (void *)((uintptr_t)p - KERNEL_VBASE);
}

uint64_t vnet_test_irq_save(void)
{
    uint64_t flags = irq_disabled ? 0 : 1ULL << 9;
    irq_disabled = true;
    return flags;
}
void vnet_test_irq_restore(uint64_t flags)
{ irq_disabled = !(flags & (1ULL << 9)); }

static void setup(uint16_t size)
{
    memset(&vnet, 0, sizeof(vnet));
    CHECK(virtqueue_init(&vnet.txq, size) == 0);
    vnet.tx_buf = PHYS_TO_VIRT(mem_alloc_aligned(size * VNET_BUF_SIZE, 16));
    vnet.common_cfg = config;
    vnet.notify_base = (void *)notify;
    config[CFG_DEVICE_STATUS] = 15;
    vnet.ready = true;
}

static host_desc_t *descriptor(unsigned index)
{ return &((host_desc_t *)vnet.txq.desc)[index]; }
static void complete(unsigned id)
{
    host_used_t *used = vnet.txq.used;
    used->ring[used->idx % vnet.txq.size] = (host_used_elem_t){id, 0};
    used->idx++;
}

static void queue_full(void)
{
    setup(4);
    for (unsigned i = 0; i < 4; i++)
        CHECK(virtqueue_add_buf(&vnet.txq, 0x1000 + i * 2048, 100, 0) == (int)i);
    CHECK(virtqueue_add_buf(&vnet.txq, 0x9000, 101, 0) == -1);
    CHECK(((host_avail_t *)vnet.txq.avail)->idx == 4);
    complete(2);
    CHECK(virtqueue_get_used(&vnet.txq, NULL) == 2);
    CHECK(virtqueue_add_buf(&vnet.txq, 0x9000, 101, 0) == 2);
    CHECK(virtqueue_add_buf(&vnet.txq, 0xa000, 102, 0) == -1);
}

static void tx_owned(void)
{
    setup(4);
    uint8_t a[100], b[100];
    memset(a, 'A', sizeof(a)); memset(b, 'B', sizeof(b));
    CHECK(virtio_net_send(a, sizeof(a)) == 0);
    CHECK(virtio_net_send(b, sizeof(b)) == 0);
    CHECK(descriptor(0)->addr != descriptor(1)->addr);
    CHECK(!memcmp((void *)(uintptr_t)(descriptor(0)->addr + VNET_HDR_SIZE), a, sizeof(a)));
    CHECK(!memcmp((void *)(uintptr_t)(descriptor(1)->addr + VNET_HDR_SIZE), b, sizeof(b)));
    CHECK(!irq_disabled);
    irq_disabled = true;
    complete(1);
    CHECK(virtio_net_send(a, sizeof(a)) == 0 && irq_disabled);
    CHECK(descriptor(1)->len == sizeof(a) + VNET_HDR_SIZE);
    CHECK(((host_avail_t *)vnet.txq.avail)->ring[2] == 1);
    irq_disabled = false;
}

static void tx_full(void)
{
    setup(4);
    uint8_t bytes[100];
    memset(bytes, 'C', sizeof(bytes));
    for (unsigned i = 0; i < 4; i++) CHECK(virtio_net_send(bytes, sizeof(bytes)) == 0);
    host_desc_t saved[4]; memcpy(saved, vnet.txq.desc, sizeof(saved));
    CHECK(virtio_net_send(bytes, sizeof(bytes)) == -1);
    CHECK(!memcmp(saved, vnet.txq.desc, sizeof(saved)));
    CHECK(((host_avail_t *)vnet.txq.avail)->idx == 4 && !irq_disabled);
    complete(3); complete(0);
    CHECK(virtio_net_send(bytes, sizeof(bytes)) == 0);
    CHECK(virtio_net_send(bytes, sizeof(bytes)) == 0);
    CHECK(virtio_net_send(bytes, sizeof(bytes)) == -1);
}

static int sender(void *arg)
{
    unsigned char tag = (unsigned char)(uintptr_t)arg;
    uint8_t bytes[100]; memset(bytes, tag, sizeof(bytes));
    for (unsigned i = 0; i < 64; i++) {
        if (virtio_net_send(bytes, sizeof(bytes)) != 0 || irq_disabled) return 1;
        thrd_yield();
    }
    return 0;
}

static void concurrent(void)
{
    setup(256);
    host_avail_t *avail = vnet.txq.avail;
    host_used_t *used = vnet.txq.used;
    for (unsigned round = 0; round < 300; round++) {
        thrd_t threads[4];
        for (unsigned i = 0; i < 4; i++) CHECK(thrd_create(&threads[i], sender, (void *)(uintptr_t)(i + 1)) == thrd_success);
        for (unsigned i = 0; i < 4; i++) {
            int result;
            CHECK(thrd_join(threads[i], &result) == thrd_success && !result);
        }
        CHECK((uint16_t)(avail->idx - used->idx) == 256);
        bool seen[256] = {0};
        unsigned tags[5] = {0};
        for (unsigned i = 0; i < 256; i++) {
            unsigned id = avail->ring[(used->idx + i) % 256];
            CHECK(id < 256 && !seen[id]); seen[id] = true;
            host_desc_t *d = descriptor(id);
            CHECK(d->flags == 0 && d->len == VNET_HDR_SIZE + 100);
            uint8_t *data = (void *)(uintptr_t)d->addr;
            for (unsigned j = 0; j < VNET_HDR_SIZE; j++) CHECK(data[j] == 0);
            unsigned tag = data[VNET_HDR_SIZE]; CHECK(tag >= 1 && tag <= 4);
            tags[tag]++;
            for (unsigned j = VNET_HDR_SIZE; j < d->len; j++) CHECK(data[j] == tag);
        }
        for (unsigned i = 1; i <= 4; i++) CHECK(tags[i] == 64);
        for (unsigned i = 0; i < 256; i++) complete(avail->ring[used->idx % 256]);
    }
}

static void device_reset(void)
{
    setup(4);
    config[CFG_DEVICE_STATUS] |= 0x40;
    CHECK(!virtio_net_is_ready());
    const uint8_t bytes[60] = {0};
    CHECK(virtio_net_send(bytes, sizeof(bytes)) == -1);
    CHECK(((host_avail_t *)vnet.txq.avail)->idx == 0);
    config[CFG_DEVICE_STATUS] = 15;
    CHECK(!virtio_net_is_ready());
}

static void queue_size(void)
{
    memset(&vnet, 0, sizeof(vnet));
    vnet.common_cfg = config;
    w16(config, CFG_QUEUE_SIZE, 1024);
    CHECK(setup_queue(&vnet.txq, 1, &vnet.tx_notify_off) == 0);
    CHECK(vnet.txq.size == 256 && r16(config, CFG_QUEUE_SIZE) == 256);
    CHECK(r16(config, CFG_QUEUE_ENABLE) == 1);
    CHECK(vnet.txq.free_head == 0);
    CHECK(descriptor(255)->next == 256);
    w16(config, CFG_QUEUE_SIZE, 0);
    CHECK(setup_queue(&vnet.rxq, 0, &vnet.rx_notify_off) == -1);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "queue")) queue_full();
    else if (!strcmp(argv[1], "owned")) tx_owned();
    else if (!strcmp(argv[1], "full")) tx_full();
    else if (!strcmp(argv[1], "concurrent")) concurrent();
    else if (!strcmp(argv[1], "reset")) device_reset();
    else if (!strcmp(argv[1], "size")) queue_size();
    else return 2;
    for (unsigned i = 0; i < allocation_count; i++) free(allocations[i]);
    printf("PASS %s: %u checks\n", argv[1], checks);
    return 0;
}
