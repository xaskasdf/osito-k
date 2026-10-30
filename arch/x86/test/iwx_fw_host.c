#include "../drivers/iwx_fw.c"

extern int printf(const char *format, ...);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static uint8_t image[IWX_FW_MAX_SIZE + 16], saved[32768];
static uint32_t end, nonpaged_end;

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = v >> (i * 8);
}
static uint32_t tlv(uint32_t type, uint32_t len)
{
    uint32_t pos = end;
    put32(image + end, type); put32(image + end + 4, len);
    end += 8 + ((len + 3) & ~3U);
    memset(image + pos + 8, 0, end - pos - 8);
    return pos + 8;
}
static void singleton(uint32_t type, uint32_t value)
{
    put32(image + tlv(type, 4), value);
}
static void section(uint32_t dest, uint32_t len)
{
    put32(image + tlv(IWX_TLV_SEC_RT, len + 4), dest);
}
static void fixture(void)
{
    memset(image, 0, 88);
    end = 88;
    put32(image + 4, IWX_TLV_MAGIC);
    put32(image + tlv(IWX_TLV_FW_VERSION, 12), 77);
    singleton(IWX_TLV_PHY_SKU, 0x330018);
    singleton(IWX_TLV_NUM_CPU, 2);
    singleton(IWX_TLV_CMD_VERSIONS, 0x06630001);
    section(0x1000, 32);
    section(IWX_CPU_SEPARATOR, 4);
    section(0x8000, 32);
    nonpaged_end = end;
    singleton(IWX_TLV_PAGING, 4096);
    section(IWX_PAGING_SEPARATOR, 4);
    section(0, 32);
    section(0x1000000, 4096);
}
static bool invalid(uint32_t n)
{
    iwx_fw_info_t info, zero = {0};
    memset(&info, 0xa5, sizeof(info));
    const char *err = iwx_fw_inspect(image, n, &info);
    CHECK(err == NULL || memcmp(&info, &zero, sizeof(info)) == 0);
    iwx_fw_plan_t plan, empty = {0};
    memset(&plan, 0xa5, sizeof(plan));
    const char *planerr = iwx_fw_plan(image, n, &plan);
    CHECK(!planerr || memcmp(&plan, &empty, sizeof(plan)) == 0);
    if (!planerr) {
        CHECK(!err && plan.alive_version >= 4 && plan.alive_version <= 7);
        CHECK(memcmp(&plan.info, &info, sizeof(info)) == 0);
        for (unsigned g = 0; g < 3; g++) {
            CHECK(plan.info.sections[g] <= 64);
            for (unsigned s = 0; s < plan.info.sections[g]; s++) {
                CHECK(plan.section[g][s].offset >= 4 && plan.section[g][s].offset <= n);
                CHECK(plan.section[g][s].length &&
                      plan.section[g][s].length <= n - plan.section[g][s].offset);
            }
        }
    }
    return err != NULL;
}

static void test_nvm_abi(void)
{
    iwx_fw_plan_t p;
    for (unsigned version = 3; version <= 4; version++) {
        fixture();
        unsigned cmd = 0;
        for (unsigned pos = 88; pos < end; pos += 8 + le32(image + pos + 4)) {
            if (le32(image + pos) == IWX_TLV_CMD_VERSIONS) { cmd = pos; break; }
        }
        CHECK(cmd != 0);
        for (unsigned i = end; i > cmd + 12; i--) image[i + 7] = image[i - 1];
        put32(image + cmd + 4, 12);
        put32(image + cmd + 12, (version << 24) | 0x00010c02);
        put32(image + cmd + 16, 0x00010c00);
        end += 8;
        unsigned api = tlv(IWX_TLV_API_CHANGES, 8);
        put32(image + api, 1);
        put32(image + api + 4, version == 4 ? 1U << 16 : 0);
        CHECK(iwx_fw_plan(image, end, &p) == NULL);
        CHECK(p.nvm_version == version && p.nvm_get_version == 1 && p.nvm_complete_version == 1);
        CHECK(p.nvm_v4 == (version == 4) && p.alive_version == 6);
        put32(image + cmd + 16, (version << 24) | 0x00010c02);
        CHECK(iwx_fw_plan(image, end, &p) != NULL && !p.nvm_version && !p.nvm_v4);
        put32(image + cmd + 16, 0x00010c00);
        put32(image + tlv(IWX_TLV_API_CHANGES, 8), 1);
        CHECK(invalid(end));
    }
    fixture(); tlv(IWX_TLV_API_CHANGES, 4);
    CHECK(invalid(end));
    fixture(); tlv(IWX_TLV_API_CHANGES, 12);
    CHECK(invalid(end));
    fixture();
    put32(image + tlv(IWX_TLV_API_CHANGES, 8), UINT32_MAX);
    CHECK(iwx_fw_plan(image, end, &p) == NULL && !p.nvm_v4);
}

int main(int argc, char **argv)
{
    iwx_fw_info_t info;
    fixture();
    CHECK(iwx_fw_inspect(image, end, &info) == NULL);
    CHECK(info.sections[0] == 1 && info.sections[1] == 1 && info.sections[2] == 2);
    CHECK(info.paging_size == 4096 && info.bytes[2] == 4128);
    iwx_fw_plan_t plan;
    CHECK(iwx_fw_plan(image, end, &plan) == NULL && plan.alive_version == 6);
    CHECK(le32(image + plan.section[0][0].offset - 4) == 0x1000);
    CHECK(le32(image + plan.section[1][0].offset - 4) == 0x8000);
    CHECK(plan.section[0][0].length == 32 && plan.section[2][1].length == 4096);
    CHECK(iwx_fw_plan(image, end, NULL) != NULL);
    uint32_t valid_size = end;
    memcpy(saved, image, end);
    for (uint32_t i = 0; i < valid_size; i++) {
        /* A valid non-paged prefix is allowed before its PAGING TLV. */
        const char *err = iwx_fw_inspect(image, i, &info);
        CHECK(err != NULL || (i == nonpaged_end && info.paging_size == 0));
    }
    for (unsigned i = 0; i < 8; i++) {
        image[i] ^= 0xff;
        CHECK(invalid(end));
        image[i] ^= 0xff;
    }
    CHECK(iwx_fw_inspect(NULL, end, &info) != NULL);
    CHECK(iwx_fw_inspect(image, end, NULL) != NULL);
    CHECK(invalid(IWX_FW_MAX_SIZE + 1));
    put32(image + 92, UINT32_MAX);
    CHECK(invalid(end));
    memcpy(image, saved, valid_size);
    for (uint32_t pos = 88; pos < valid_size;) {
        uint32_t len = le32(saved + pos + 4);
        put32(image + pos + 4, UINT32_MAX);
        CHECK(invalid(valid_size));
        memcpy(image, saved, valid_size);
        pos += 8 + ((len + 3) & ~3U);
    }
    fixture(); singleton(IWX_TLV_NUM_CPU, 2);
    CHECK(invalid(end));
    fixture(); section(IWX_CPU_SEPARATOR, 4);
    CHECK(invalid(end));
    fixture(); section(0xfffffff0, 32);
    CHECK(invalid(end));
    fixture(); section(0x2000000, 32769);
    CHECK(invalid(end));
    fixture(); section(0x2000000, 1);
    CHECK(invalid(end));
    fixture(); tlv(0x7fffffff, 3);
    CHECK(iwx_fw_inspect(image, end, &info) == NULL);
    CHECK(invalid(end - 1)); /* Missing alignment byte. */
    fixture();
    for (unsigned i = 0; i < 64; i++) section(0x2000000 + i * 4096, 4096);
    CHECK(invalid(end));
    fixture(); put32(image + 96, 78);
    CHECK(invalid(end));

    for (unsigned version = 0; version < 10; version++) {
        fixture();
        for (unsigned pos = 88; pos < end; pos += 8 + le32(image + pos + 4)) {
            if (le32(image + pos) != IWX_TLV_CMD_VERSIONS) continue;
            image[pos + 11] = version;
        }
        CHECK((iwx_fw_plan(image, end, &plan) == NULL) == (version >= 4 && version <= 7));
    }
    fixture();
    for (unsigned pos = 88; pos < end; pos += 8 + le32(image + pos + 4)) {
        if (le32(image + pos) != IWX_TLV_CMD_VERSIONS) continue;
        image[pos + 9] = 1; /* Wrong group is not ALIVE. */
    }
    CHECK(iwx_fw_plan(image, end, &plan) != NULL);
    fixture();
    for (unsigned pos = 88; pos < end; pos += 8 + le32(image + pos + 4)) {
        if (le32(image + pos) != IWX_TLV_CMD_VERSIONS) continue;
        for (unsigned i = end; i > pos + 12; i--) image[i + 3] = image[i - 1];
        put32(image + pos + 4, 8);
        put32(image + pos + 12, 0x06630001);
        end += 4;
        break;
    }
    CHECK(iwx_fw_plan(image, end, &plan) != NULL);

    /* Deterministic mutation exercise of TLV offsets/lengths and short inputs. */
    uint32_t random = 0x91837;
    for (unsigned i = 0; i < 20000; i++) {
        memcpy(image, saved, valid_size);
        random = random * 1664525 + 1013904223;
        unsigned pos = random % valid_size;
        image[pos] ^= (random >> 16) | 1;
        random = random * 1664525 + 1013904223;
        (void)invalid(random % (valid_size + 1));
    }

    test_nvm_abi();
    if (argc > 1) {
        int fd = open(argv[1], 0);
        CHECK(fd >= 0);
        if (fd >= 0) {
            unsigned n = 0;
            long got;
            while (n < sizeof(image) && (got = read(fd, image + n, sizeof(image) - n)) > 0)
                n += got;
            close(fd);
            const char *err = iwx_fw_inspect(image, n, &info);
            CHECK(err == NULL);
            CHECK(iwx_fw_plan(image, n, &plan) == NULL && plan.alive_version == 6);
            CHECK((plan.radio.seen & (1U << IWX_ABI_STA_KEY)) && plan.radio.command[IWX_ABI_STA_KEY] == 3);
            CHECK((plan.radio.api[0] & (1U << 29)) && (plan.radio.api[1] & (1U << 9)));
            CHECK(plan.nvm_v4 && plan.nvm_version == 4 &&
                  plan.nvm_get_version == 1 && plan.nvm_complete_version == 1);
            if (err) printf("firmware error: %s\n", err);
            else printf("firmware %u.%08x.%u: %u/%u/%u sections, %u paging bytes, %u commands\n",
                info.version[0], info.version[1], info.version[2],
                info.sections[0], info.sections[1], info.sections[2],
                info.paging_size, info.commands);
        }
    }
    printf("iwx firmware: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
