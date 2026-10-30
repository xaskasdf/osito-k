#include "osversion.h"

typedef struct {
    const unsigned char *data;
    unsigned count, width, big_endian;
} OSVERSION_XML;

static unsigned osversion_xml_char(const OSVERSION_XML *xml, unsigned pos)
{
    if (pos >= xml->count) return 0;
    const unsigned char *p = xml->data + pos * xml->width;
    if (xml->width == 1) return p[0];
    return xml->big_endian ? ((unsigned)p[0] << 8) | p[1] :
                            ((unsigned)p[1] << 8) | p[0];
}

static int osversion_xml_equal(const char *a, const char *b, int fold)
{
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (fold) {
            if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
            if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        }
        if (x != y) return 0;
    }
    return *a == *b;
}

unsigned osversion_manifest_level(const void *data, unsigned size)
{
    /* This is a bounded reader for the manifest compatibility subtree, not
     * an activation-context/XML engine. Do not interpret comments, CDATA,
     * unrelated attributes or another namespace as supportedOS declarations.
     * Unsupported DTDs and malformed/truncated resources keep the legacy cap. */
    if (!data || !size || size > 1024 * 1024) return 62;
    const unsigned char *bytes = data;
    OSVERSION_XML xml = {bytes, size, 1, 0};
    if (size >= 2 && ((bytes[0] == 0xff && bytes[1] == 0xfe) ||
                      (bytes[0] == 0xfe && bytes[1] == 0xff))) {
        xml.big_endian = bytes[0] == 0xfe;
        xml.data += 2; size -= 2; xml.width = 2;
    } else if (size >= 2 && ((bytes[0] == '<' && !bytes[1]) ||
                             (!bytes[0] && bytes[1] == '<'))) {
        xml.width = 2; xml.big_endian = !bytes[0];
    } else if (size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
        xml.data += 3; size -= 3;
    }
    if (size % xml.width) return 62;
    xml.count = size / xml.width;
    struct { char name[64]; unsigned stage, ns_start; } stack[24];
    struct { char prefix[64]; unsigned kind; } namespaces[48];
    unsigned depth = 0, ns_count = 0, roots = 0, level = 62, pos = 0;
#define XC(i) osversion_xml_char(&xml, (i))
#define XSPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\r' || (c) == '\n')
#define XNAME(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || \
                 ((c) >= '0' && (c) <= '9') || (c) == ':' || (c) == '_' || (c) == '-' || (c) == '.')
    while (pos < xml.count) {
        unsigned ch = XC(pos++);
        if (!ch) return 62;
        if (ch != '<') {
            if (!depth && !XSPACE(ch)) return 62;
            continue;
        }
        if (XC(pos) == '?') {
            pos++;
            while (pos < xml.count && !(XC(pos) == '?' && XC(pos+1) == '>')) pos++;
            if (pos == xml.count) return 62;
            pos += 2; continue;
        }
        if (XC(pos) == '!') {
            unsigned comment = XC(pos+1) == '-' && XC(pos+2) == '-';
            const char *cdata = "[CDATA[";
            unsigned is_cdata = 1;
            for (unsigned i = 0; cdata[i]; i++) if (XC(pos+1+i) != (unsigned)cdata[i]) is_cdata = 0;
            if (!comment && !is_cdata) return 62;
            if (is_cdata && !depth) return 62;
            pos += comment ? 3 : 8;
            unsigned end = comment ? '-' : ']';
            while (pos < xml.count && !(XC(pos) == end && XC(pos+1) == end && XC(pos+2) == '>')) pos++;
            if (pos == xml.count) return 62;
            pos += 3; continue;
        }
        unsigned closing = XC(pos) == '/';
        if (closing) pos++;
        char name[64]; unsigned n = 0;
        while (XNAME(XC(pos))) {
            if (n == sizeof(name)-1) return 62;
            name[n++] = (char)XC(pos++);
        }
        name[n] = 0;
        if (!n) return 62;
        if (closing) {
            while (XSPACE(XC(pos))) pos++;
            if (XC(pos++) != '>' || !depth || !osversion_xml_equal(name, stack[depth-1].name, 0)) return 62;
            ns_count = stack[--depth].ns_start;
            continue;
        }
        if (depth == 24 || (!depth && roots++)) return 62;
        unsigned ns_start = ns_count, id_level = 62, self_close = 0;
        for (;;) {
            unsigned spaces = 0;
            while (XSPACE(XC(pos))) { pos++; spaces++; }
            if (XC(pos) == '/' && XC(pos+1) == '>') { pos += 2; self_close = 1; break; }
            if (XC(pos) == '>') { pos++; break; }
            if (!spaces) return 62;
            char attr[64], value[128]; n = 0;
            while (XNAME(XC(pos))) {
                if (n == sizeof(attr)-1) return 62;
                attr[n++] = (char)XC(pos++);
            }
            attr[n] = 0;
            if (!n) return 62;
            while (XSPACE(XC(pos))) pos++;
            if (XC(pos++) != '=') return 62;
            while (XSPACE(XC(pos))) pos++;
            unsigned quote = XC(pos++);
            if (quote != '\'' && quote != '"') return 62;
            n = 0; unsigned overflow = 0;
            while ((ch = XC(pos)) != quote) {
                if (!ch || ch == '<') return 62;
                if (n < sizeof(value)-1 && ch < 128) value[n++] = (char)ch;
                else overflow = 1;
                pos++;
            }
            pos++; value[n] = 0;
            if (overflow) value[0] = 0;
            if (osversion_xml_equal(attr, "Id", 0)) {
                if (osversion_xml_equal(value, "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}", 1)) id_level = 100;
                else if (osversion_xml_equal(value, "{1f676c76-80e1-4239-95bb-83d0f6d0da78}", 1)) id_level = 63;
            }
            unsigned is_default = osversion_xml_equal(attr, "xmlns", 0);
            unsigned is_prefix = attr[0] == 'x' && attr[1] == 'm' && attr[2] == 'l' && attr[3] == 'n' && attr[4] == 's' && attr[5] == ':';
            if (is_default || is_prefix) {
                if (ns_count == 48) return 62;
                const char *prefix = is_default ? "" : attr + 6;
                n = 0; do { namespaces[ns_count].prefix[n] = prefix[n]; } while (prefix[n++]);
                namespaces[ns_count].kind = osversion_xml_equal(value, "urn:schemas-microsoft-com:asm.v1", 0) ? 1 :
                    osversion_xml_equal(value, "urn:schemas-microsoft-com:compatibility.v1", 0) ? 2 : 0;
                ns_count++;
            }
        }
        char prefix[64]; n = 0;
        const char *local = name;
        for (unsigned i = 0; name[i]; i++) if (name[i] == ':') {
            if (local != name) return 62;
            local = name + i + 1;
            for (unsigned j = 0; j < i; j++) prefix[n++] = name[j];
        }
        prefix[n] = 0;
        unsigned ns = 0;
        for (unsigned i = ns_count; i; i--) if (osversion_xml_equal(prefix, namespaces[i-1].prefix, 0)) {
            ns = namespaces[i-1].kind; break;
        }
        unsigned parent = depth ? stack[depth-1].stage : 0, stage = 0;
        if (!depth && ns == 1 && osversion_xml_equal(local, "assembly", 0)) stage = 1;
        if (parent == 1 && ns == 2 && osversion_xml_equal(local, "compatibility", 0)) stage = 2;
        if (parent == 2 && ns == 2 && osversion_xml_equal(local, "application", 0)) stage = 3;
        if (parent == 3 && ns == 2 && osversion_xml_equal(local, "supportedOS", 0) && id_level > level) level = id_level;
        if (self_close) ns_count = ns_start;
        else {
            n = 0; do { stack[depth].name[n] = name[n]; } while (name[n++]);
            stack[depth].stage = stage; stack[depth].ns_start = ns_start; depth++;
        }
    }
#undef XC
#undef XSPACE
#undef XNAME
    return !depth && roots == 1 ? level : 62;
}
