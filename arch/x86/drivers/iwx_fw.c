/* Bounds-checked Intel TLV inspection. No device writes or DMA allocation. */
#include "iwx.h"
#include "iwxreg.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void radio_abi_id(unsigned index, uint8_t *group, uint8_t *opcode)
{
    static const uint8_t ids[IWX_ABI_COUNT][2] = {
        {1, IWX_TX_ANT_CFG}, {1, IWX_BT_CONFIG}, {2, IWX_SOC_CONFIG},
        {5, IWX_DQA_ENABLE}, {4, IWX_TEMP_THRESHOLDS}, {1, IWX_DEVICE_POWER},
        {1, IWX_MCC_UPDATE}, {1, IWX_SCAN_CONFIG}, {1, IWX_SCAN_REQUEST},
        {1, IWX_SCAN_ABORT}, {1, IWX_BEACON_FILTER}, {0, IWX_RX_MPDU}, {1, IWX_LTR_CONFIG},
        {1, IWX_PHY_CONTEXT}, {5, IWX_RLC_CONFIG}, {1, IWX_MAC_CONTEXT},
        {1, IWX_BINDING_CONTEXT}, {1, IWX_ADD_STATION}, {1, IWX_TX_FRAME},
        {5, IWX_SCD_QUEUE_CONFIG}, {3, IWX_SESSION_PROTECTION}, {3, IWX_SESSION_NOTIFICATION},
        {1, IWX_ADD_STA_KEY}
    };
    *group = ids[index][0];
    *opcode = ids[index][1];
}

static const char *parse(const void *data, uint32_t size, iwx_fw_info_t *info,
                         iwx_fw_plan_t *plan)
{
    if (!info) return "missing output";
    memset(info, 0, sizeof(*info));
    if (!data || size < 88 || size > IWX_FW_MAX_SIZE)
        return "firmware size outside 88 bytes..4 MiB";
    const uint8_t *p = data;
    if (le32(p) != 0 || le32(p + 4) != IWX_TLV_MAGIC)
        return "not an Intel TLV .ucode image (Windows .dat/.sys not supported)";

    iwx_fw_info_t f = {0};
    unsigned group = 0, seen = 0, nvm_seen = 0;
    unsigned api_seen = 0, capa_seen = 0;
    uint32_t paging_css = 0;
    for (uint32_t pos = 88; pos < size;) {
        if (size - pos < 8) return "truncated TLV header";
        uint32_t type = le32(p + pos), len = le32(p + pos + 4);
        pos += 8;
        if (len > size - pos) return "truncated TLV body";
        uint32_t padded = (len + 3U) & ~3U;
        if (padded > size - pos) return "truncated TLV padding";
        const uint8_t *v = p + pos;
        f.tlvs++;
        unsigned bit = 0;
        switch (type) {
        case IWX_TLV_FW_VERSION:
            bit = 1;
            if (len != 12) return "invalid firmware version TLV";
            for (int i = 0; i < 3; i++) f.version[i] = le32(v + i * 4);
            if (f.version[0] != 77) return "firmware API not 77";
            break;
        case IWX_TLV_PHY_SKU:
            bit = 2;
            if (len != 4) return "invalid PHY SKU TLV";
            f.phy_config = le32(v);
            break;
        case IWX_TLV_NUM_CPU:
            bit = 4;
            if (len != 4 || le32(v) != 2) return "expected two firmware CPUs";
            break;
        case IWX_TLV_PAGING:
            bit = 8;
            if (len != 4 || (le32(v) & 4095) || le32(v) > 1024U * 1024U)
                return "invalid paging size";
            f.paging_size = le32(v);
            break;
        case IWX_TLV_API_CHANGES:
        case IWX_TLV_CAPABILITIES: {
            bool api = type == IWX_TLV_API_CHANGES;
            if (len != 8) return "invalid firmware API/capability flags TLV";
            unsigned word = le32(v), limit = api ? 3 : 4;
            unsigned *flags_seen = api ? &api_seen : &capa_seen;
            if (word < limit) {
                if (*flags_seen & (1U << word)) return "duplicate firmware API/capability word";
                *flags_seen |= 1U << word;
                if (plan) {
                    (api ? plan->radio.api : plan->radio.capa)[word] = le32(v + 4);
                    if (api && word == 1) plan->nvm_v4 = (le32(v + 4) & (1U << 16)) != 0;
                }
            }
            break;
        }
        case IWX_TLV_SCAN_CHANNELS:
            bit = 32;
            if (len != 4 || !le32(v) || le32(v) > 256)
                return "invalid scan channel count TLV";
            if (plan) plan->radio.scan_channels = le32(v);
            break;
        case IWX_TLV_CMD_VERSIONS:
            bit = 16;
            if (!len || len % 4) return "invalid command versions TLV";
            f.commands = len / 4;
            if (plan) {
                bool found = false;
                for (uint32_t i = 0; i < len; i += 4) {
                    for (unsigned a = 0; a < IWX_ABI_COUNT; a++) {
                        uint8_t g, op;
                        radio_abi_id(a, &g, &op);
                        if (v[i] != op || v[i + 1] != g) continue;
                        if (plan->radio.seen & (1U << a)) return "duplicate radio command ABI";
                        plan->radio.seen |= 1U << a;
                        plan->radio.command[a] = v[i + 2];
                        plan->radio.notification[a] = v[i + 3];
                    }
                    if (v[i + 1] == IWX_NVM_GROUP &&
                        (v[i] == IWX_NVM_GET || v[i] == IWX_NVM_COMPLETE)) {
                        unsigned nbit = v[i] == IWX_NVM_GET ? 1 : 2;
                        if (nvm_seen & nbit) return "duplicate NVM command ABI";
                        nvm_seen |= nbit;
                        if (nbit == 1) {
                            plan->nvm_get_version = v[i + 2];
                            plan->nvm_version = v[i + 3];
                        } else plan->nvm_complete_version = v[i + 2];
                    }
                    if (v[i] != 1 || v[i + 1] != 0) continue;
                    if (found) return "duplicate ALIVE notification ABI";
                    found = true;
                    plan->alive_version = v[i + 3];
                }
                if (!found || plan->alive_version < 4 || plan->alive_version > 7)
                    return "unsupported/missing ALIVE notification ABI";
            }
            break;
        case IWX_TLV_SEC_RT: {
            if (len < 4) return "missing section destination";
            uint32_t dest = le32(v), bytes = len - 4;
            if (dest == IWX_CPU_SEPARATOR || dest == IWX_PAGING_SEPARATOR) {
                if ((len != 4 && len != 8) || (len == 8 && le32(v + 4)))
                    return "invalid section separator";
                if (dest == IWX_CPU_SEPARATOR && group == 0 && f.sections[0])
                    group = 1;
                else if (dest == IWX_PAGING_SEPARATOR && group == 1 && f.sections[1])
                    group = 2;
                else return "out-of-order section separator";
                break;
            }
            if (!bytes || bytes > IWX_MAX_SECTION_SIZE ||
                (uint64_t)dest + bytes > 0x100000000ULL)
                return "invalid section extent";
            if (f.sections[group] == IWX_MAX_DRAM_ENTRIES)
                return "too many firmware sections";
            if (group == 2) {
                if (!f.sections[2]) paging_css = bytes;
                else if (bytes & 4095) return "unaligned paging section";
            }
            if (plan) {
                plan->section[group][f.sections[group]].offset = pos + 4;
                plan->section[group][f.sections[group]].length = bytes;
            }
            f.sections[group]++;
            f.bytes[group] += bytes;
            break;
        }
        default:
            /* Optional/debug TLVs are opaque, but their bounds still matter. */
            break;
        }
        if (seen & bit) return "duplicate singleton TLV";
        seen |= bit;
        pos += padded;
    }
    if ((seen & 23) != 23 || !f.sections[0] || !f.sections[1])
        return "missing runtime firmware metadata/sections";
    if (f.paging_size) {
        if (group != 2 || f.sections[2] < 2 ||
            f.bytes[2] - paging_css != f.paging_size)
            return "paging sections do not match declared size";
    } else if (group == 2) return "paging sections without paging size";
    *info = f;
    if (plan) plan->radio.phy_config = f.phy_config;
    return NULL;
}

const char *iwx_fw_inspect(const void *data, uint32_t size, iwx_fw_info_t *info)
{
    return parse(data, size, info, NULL);
}

const char *iwx_fw_plan(const void *data, uint32_t size, iwx_fw_plan_t *plan)
{
    if (!plan) return "missing firmware plan";
    memset(plan, 0, sizeof(*plan));
    const char *err = parse(data, size, &plan->info, plan);
    if (err) memset(plan, 0, sizeof(*plan));
    return err;
}
