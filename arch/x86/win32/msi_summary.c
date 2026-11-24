#include "msi_summary.h"
#include "kernel32_shim.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

static uint32_t summary_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t summary_text(const uint8_t *data, uint32_t available, uint32_t codepage, uint16_t **output)
{
    uint32_t type = summary_u32(data);
    if (type != 30 && type != 31) return 120;
    if (available < 8) return 13;
    uint32_t count = summary_u32(data + 4), width = type == 31 ? 2 : 1;
    if (!count || count > (available - 8) / width) return 13;
    const uint8_t *text = data + 8;
    if (text[(count-1)*width] || (width == 2 && text[(count-1)*width+1])) return 13;
    for (uint32_t i = 0; i + 1 < count; i++)
        if (!text[i*width] && (width == 1 || !text[i*width+1])) return 13;
    if (width == 2) {
        *output = kmalloc((uint64_t)count * 2);
        if (!*output) return 8;
        for (uint32_t i = 0; i < count; i++) (*output)[i] = text[i*2] | (uint16_t)text[i*2+1] << 8;
        return 0;
    }
    /* CodePageString with CP1200 has different length semantics. */
    if (codepage == 1200 || codepage == 1201) return 120;
    int units = MultiByteToWideChar(codepage, 0, (const char *)text, -1, NULL, 0);
    if (units <= 0 || units > 1048576) return 13;
    *output = kmalloc((uint64_t)(uint32_t)units * 2);
    if (!*output) return 8;
    if (MultiByteToWideChar(codepage, 0, (const char *)text, -1, *output, units) != units) {
        kfree(*output); *output = NULL; return 13;
    }
    return 0;
}
msi_summary_t *msi_summary_parse(const uint8_t *stream, uint32_t size, uint32_t *error)
{
    if (error) *error = 13;
    static const uint8_t fmtid[16] = {0xe0,0x85,0x9f,0xf2,0xf9,0x4f,0x68,0x10,0xab,0x91,0x08,0x00,0x2b,0x27,0xb3,0xd9};
    if (!stream || size < 56 || size > 2U * 1024 * 1024 || stream[0] != 0xfe || stream[1] != 0xff ||
        stream[2] > 1 || stream[3] || summary_u32(stream + 24) != 1 || memcmp(stream + 28, fmtid, 16)) return NULL;
    uint32_t offset = summary_u32(stream + 44);
    if (offset < 48 || (offset & 3) || offset > size - 8) return NULL;
    const uint8_t *set = stream + offset;
    uint32_t set_size = summary_u32(set), count = summary_u32(set + 4);
    if (set_size < 8 || set_size > size - offset || !count || count > 128 || count > (set_size - 8) / 8) return NULL;
    const uint8_t *properties[5] = {0}; uint32_t lengths[5] = {0};
    static const uint32_t wanted[5] = {1, 7, 9, 14, 15};
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = summary_u32(set + 8 + 8*i), at = summary_u32(set + 12 + 8*i);
        uint32_t next = i + 1 == count ? set_size : summary_u32(set + 20 + 8*i);
        if ((at & 3) || at < 8 + 8*count || at >= next || next > set_size || next - at < 4) return NULL;
        for (uint32_t j = 0; j < i; j++) if (summary_u32(set + 8 + 8*j) == id) return NULL;
        for (unsigned p = 0; p < 5; p++) if (id == wanted[p]) { properties[p] = set + at; lengths[p] = next - at; }
    }
    for (unsigned p = 0; p < 5; p++) if (!properties[p]) return NULL;
    if (lengths[0] < 8 || summary_u32(properties[0]) != 2 ||
        lengths[3] < 8 || summary_u32(properties[3]) != 3 ||
        lengths[4] < 8 || summary_u32(properties[4]) != 3) return NULL;
    msi_summary_t *summary = kmalloc(sizeof(*summary));
    if (!summary) { if (error) *error = 8; return NULL; }
    memset(summary, 0, sizeof(*summary));
    summary->codepage = (uint32_t)properties[0][4] | (uint32_t)properties[0][5] << 8;
    summary->installer_version = summary_u32(properties[3] + 4);
    summary->word_count = summary_u32(properties[4] + 4);
    uint32_t result = summary_text(properties[1], lengths[1], summary->codepage, &summary->platform);
    if (!result) result = summary_text(properties[2], lengths[2], summary->codepage, &summary->package_code);
    if (error) *error = result;
    if (result) { msi_summary_close(summary); return NULL; }
    return summary;
}
msi_summary_t *msi_summary_read(msi_db_t *database, uint32_t *error)
{
    uint32_t size = 0; uint8_t *data = msi_read_stream(database, "\005SummaryInformation", &size);
    if (!data) { if (error) *error = 1620; return NULL; }
    msi_summary_t *summary = msi_summary_parse(data, size, error); kfree(data); return summary;
}
void msi_summary_close(msi_summary_t *summary)
{
    if (!summary) return;
    if (summary->platform) kfree(summary->platform);
    if (summary->package_code) kfree(summary->package_code);
    kfree(summary);
}
