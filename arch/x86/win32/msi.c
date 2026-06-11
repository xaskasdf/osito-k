/*
 * OsitoK Win32 Layer — MSI database reader + native installer. See msi.h.
 *
 * Pipeline: OLE2 streams -> de-mangle names -> string pool (_StringData/
 * _StringPool) -> column metadata (_Columns) -> typed column-major row
 * access -> Directory tree resolution -> file extraction (embedded CAB or
 * uncompressed stream) -> Registry-table application.
 */

#include "msi.h"
#include "ole2.h"
#include "cab.h"
#include "installer.h"
#include "advapi32_shim.h"   /* DWORD, REG_SZ/REG_DWORD, advapi32_reg_install_set */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

#define MSI_MAX_TABLES 96
#define MSI_MAX_COLS   40
#define MSI_NAME       72
#define MSITYPE_STRING 0x0800

typedef struct {
    char     name[MSI_NAME];
    uint16_t type;       /* decoded column type word */
    uint8_t  width;      /* persisted cell width in bytes */
} msi_col_t;

typedef struct {
    char      name[MSI_NAME];
    int       ncols;
    msi_col_t cols[MSI_MAX_COLS];
    int       ole_index; /* directory index of the data stream, -1 if none */
} msi_table_t;

struct msi_db {
    ole2_file_t *ole;
    char     *strbuf;     /* concatenated null-terminated strings */
    uint32_t *str_start;  /* offset of string id within strbuf */
    uint32_t  nstrings;
    int       strref;     /* string-reference width: 2 or 3 */
    msi_table_t tables[MSI_MAX_TABLES];
    int       ntables;
};

/* Loaded (read) data stream for a table. */
typedef struct {
    msi_table_t *meta;
    uint8_t     *data;
    uint32_t     size;
    uint32_t     nrows;
} msi_loaded_t;

/* ── small string helpers ────────────────────────────────────── */

static int meq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static uint32_t mstrlen(const char *s)
{
    uint32_t n = 0; while (s[n]) n++; return n;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ── stream-name de-mangling (MSI 0x3800/0x4800 base, 0x4840 table marker) */

static const char MIME[65] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";

static void demangle(const uint16_t *in, char *out, int cap)
{
    int n = 0;
    uint16_t ch;
    while ((ch = *in++)) {
        if (ch == 0x4840) {            /* table marker — drop */
            continue;
        } else if (ch >= 0x3800 && ch < 0x4800) {
            uint16_t v = ch - 0x3800;
            if (n < cap - 1) out[n++] = MIME[v & 0x3f];
            if (n < cap - 1) out[n++] = MIME[(v >> 6) & 0x3f];
        } else if (ch >= 0x4800 && ch < 0x4840) {
            uint16_t v = ch - 0x4800;
            if (n < cap - 1) out[n++] = MIME[v & 0x3f];
        } else {
            if (n < cap - 1) out[n++] = (char)(ch & 0xff);
        }
    }
    out[n] = 0;
}

static int find_stream(ole2_file_t *ole, const char *target)
{
    uint16_t raw[64];
    char name[MSI_NAME];
    uint32_t sz; int type;
    uint32_t n = ole2_count(ole);
    for (uint32_t i = 0; i < n; i++) {
        if (!ole2_enum(ole, (int)i, raw, 64, &sz, &type)) continue;
        if (type != 2) continue;
        demangle(raw, name, sizeof name);
        if (meq(name, target)) return (int)i;
    }
    return -1;
}

const char *msi_string(msi_db_t *db, uint32_t id)
{
    if (!db || id >= db->nstrings) return "";
    return db->strbuf + db->str_start[id];
}

/* ── string pool ─────────────────────────────────────────────── */

static int build_strings(msi_db_t *db)
{
    int si = find_stream(db->ole, "_StringData");
    int pi = find_stream(db->ole, "_StringPool");
    if (si < 0 || pi < 0) { serial_puts("[MSI] no string pool\n"); return -1; }

    uint32_t dsz = 0, psz = 0;
    uint8_t *sdata = ole2_read_index(db->ole, si, &dsz);
    uint8_t *pool  = ole2_read_index(db->ole, pi, &psz);
    if (!pool) { if (sdata) kfree(sdata); return -1; }

    uint32_t nent = psz / 4;       /* 4-byte entries (refcount, length) */
    uint32_t codepage = (nent >= 1)
        ? ((uint32_t)rd16(pool) | ((uint32_t)rd16(pool + 2) << 16)) : 0;
    db->strref = (codepage & 0x80000000u) ? 3 : 2;

    /* Pass 1: count strings (id 0 = "") and total byte length.            */
    /* Follows the Wine/libmsi overflow rule: a size==0,refs!=0 entry is a  */
    /* high-word marker for the next entry's 32-bit length.                 */
    uint32_t nstr = 1, totlen = 0, i = 1;
    while (i < nent) {
        uint16_t len  = rd16(pool + i * 4);       /* word0 = length */
        uint16_t refs = rd16(pool + i * 4 + 2);   /* word1 = refcount */
        uint32_t L = len;
        if (len == 0 && refs != 0 && i + 1 < nent) {
            /* overflow marker: 32-bit length = next.len | (refs << 16) */
            L = (uint32_t)rd16(pool + (i + 1) * 4) | ((uint32_t)refs << 16);
            i++;
        }
        totlen += L;
        nstr++; i++;
    }

    db->nstrings  = nstr;
    db->str_start = (uint32_t *)kmalloc((uint64_t)nstr * 4);
    db->strbuf    = (char *)kmalloc(totlen + nstr + 1);
    if (!db->str_start || !db->strbuf) {
        if (sdata) kfree(sdata); kfree(pool); return -1;
    }

    /* Pass 2: materialize. */
    uint32_t w = 0, off = 0, n = 0;
    db->str_start[n++] = w; db->strbuf[w++] = 0;   /* id 0 = "" */
    i = 1;
    while (i < nent && n < nstr) {
        uint16_t len  = rd16(pool + i * 4);       /* word0 = length */
        uint16_t refs = rd16(pool + i * 4 + 2);   /* word1 = refcount */
        uint32_t L = len;
        if (len == 0 && refs != 0 && i + 1 < nent) {
            L = (uint32_t)rd16(pool + (i + 1) * 4) | ((uint32_t)refs << 16);
            i++;
        }
        db->str_start[n] = w;
        if (sdata && off + L <= dsz) memcpy(db->strbuf + w, sdata + off, L);
        w += L; db->strbuf[w++] = 0; off += L;
        n++; i++;
    }

    if (sdata) kfree(sdata);
    kfree(pool);
    serial_puts("[MSI] string pool: "); serial_putdec(db->nstrings);
    serial_puts(" strings, strref="); serial_putdec(db->strref);
    serial_puts("\n");
    return 0;
}

/* ── table metadata from _Columns ────────────────────────────── */

static msi_table_t *get_or_add_table(msi_db_t *db, const char *name)
{
    for (int i = 0; i < db->ntables; i++)
        if (meq(db->tables[i].name, name)) return &db->tables[i];
    if (db->ntables >= MSI_MAX_TABLES) return 0;
    msi_table_t *t = &db->tables[db->ntables++];
    int j = 0; for (const char *p = name; *p && j < MSI_NAME - 1; p++) t->name[j++] = *p;
    t->name[j] = 0;
    t->ncols = 0;
    t->ole_index = -1;
    return t;
}

static uint8_t width_for_type(msi_db_t *db, uint16_t type)
{
    if (type & MSITYPE_STRING) return (uint8_t)db->strref;
    uint8_t w = (uint8_t)(type & 0xff);
    if (w != 2 && w != 4) w = 2;
    return w;
}

static int build_columns(msi_db_t *db)
{
    int ci = find_stream(db->ole, "_Columns");
    if (ci < 0) { serial_puts("[MSI] no _Columns\n"); return -1; }
    uint32_t size = 0;
    uint8_t *col = ole2_read_index(db->ole, ci, &size);
    if (!col) return -1;

    int sr = db->strref;
    uint32_t roww = (uint32_t)sr + 2 + (uint32_t)sr + 2;
    uint32_t nrows = roww ? size / roww : 0;

    /* column-major bases */
    uint32_t b_table = 0;
    uint32_t b_num   = nrows * sr;
    uint32_t b_name  = nrows * (sr + 2);
    uint32_t b_type  = nrows * (sr + 2 + sr);

    for (uint32_t r = 0; r < nrows; r++) {
        uint32_t tid = 0, nid = 0, num = 0, typ = 0;
        for (int k = 0; k < sr; k++) tid |= (uint32_t)col[b_table + r*sr + k] << (8*k);
        num = (uint32_t)col[b_num + r*2] | ((uint32_t)col[b_num + r*2 + 1] << 8);
        for (int k = 0; k < sr; k++) nid |= (uint32_t)col[b_name + r*sr + k] << (8*k);
        typ = (uint32_t)col[b_type + r*2] | ((uint32_t)col[b_type + r*2 + 1] << 8);

        const char *tname = msi_string(db, tid);
        const char *cname = msi_string(db, nid);
        if (num == 0) continue;                       /* null column index */
        uint16_t type   = (uint16_t)(typ - 0x8000);   /* int unbias (2-byte) */
        int      colnum = (int)(num - 0x8000);        /* Number is a biased int */
        int pos = colnum - 1;
        if (pos < 0 || pos >= MSI_MAX_COLS) continue;

        msi_table_t *t = get_or_add_table(db, tname);
        if (!t) continue;
        int j = 0; for (const char *p = cname; *p && j < MSI_NAME - 1; p++) t->cols[pos].name[j++] = *p;
        t->cols[pos].name[j] = 0;
        t->cols[pos].type  = type;
        t->cols[pos].width = width_for_type(db, type);
        if (pos + 1 > t->ncols) t->ncols = pos + 1;
    }

    serial_puts("[MSI] parsed "); serial_putdec((uint64_t)db->ntables);
    serial_puts(" tables\n");

    /* bind each table to its data stream */
    for (int i = 0; i < db->ntables; i++)
        db->tables[i].ole_index = find_stream(db->ole, db->tables[i].name);

    kfree(col);
    return 0;
}

/* ── open / close ────────────────────────────────────────────── */

msi_db_t *msi_open(const uint8_t *data, uint32_t len)
{
    ole2_file_t *ole = ole2_open(data, len);
    if (!ole) { serial_puts("[MSI] not a compound file\n"); return 0; }

    msi_db_t *db = (msi_db_t *)kmalloc(sizeof(*db));
    if (!db) { ole2_close(ole); return 0; }
    memset(db, 0, sizeof(*db));
    db->ole = ole;
    db->strref = 2;

    if (build_strings(db) != 0) { msi_close(db); return 0; }
    if (build_columns(db) != 0) { msi_close(db); return 0; }
    return db;
}

void msi_close(msi_db_t *db)
{
    if (!db) return;
    if (db->ole)       ole2_close(db->ole);
    if (db->strbuf)    kfree(db->strbuf);
    if (db->str_start) kfree(db->str_start);
    kfree(db);
}

/* ── typed table access ──────────────────────────────────────── */

static msi_table_t *tbl(msi_db_t *db, const char *name)
{
    for (int i = 0; i < db->ntables; i++)
        if (meq(db->tables[i].name, name)) return &db->tables[i];
    return 0;
}

static int load_table(msi_db_t *db, const char *name, msi_loaded_t *lt)
{
    memset(lt, 0, sizeof(*lt));
    msi_table_t *t = tbl(db, name);
    if (!t || t->ole_index < 0) return -1;
    lt->meta = t;
    lt->data = ole2_read_index(db->ole, t->ole_index, &lt->size);
    if (!lt->data) return -1;
    uint32_t roww = 0;
    for (int i = 0; i < t->ncols; i++) roww += t->cols[i].width;
    lt->nrows = roww ? lt->size / roww : 0;
    return 0;
}

static void free_table(msi_loaded_t *lt)
{
    if (lt->data) { kfree(lt->data); lt->data = 0; }
}

static int col_off(msi_loaded_t *lt, const char *cname,
                   uint32_t *base, uint8_t *w)
{
    uint32_t b = 0;
    for (int i = 0; i < lt->meta->ncols; i++) {
        if (meq(lt->meta->cols[i].name, cname)) { *base = b; *w = lt->meta->cols[i].width; return 1; }
        b += lt->nrows * lt->meta->cols[i].width;
    }
    return 0;
}

static uint32_t raw_cell(msi_loaded_t *lt, uint32_t row, const char *cname, uint8_t *wout)
{
    uint32_t base; uint8_t w;
    if (!col_off(lt, cname, &base, &w)) { if (wout) *wout = 0; return 0; }
    const uint8_t *p = lt->data + base + row * w;
    uint32_t v = 0;
    for (int i = 0; i < w; i++) v |= (uint32_t)p[i] << (8 * i);
    if (wout) *wout = w;
    return v;
}

static const char *cell_s(msi_db_t *db, msi_loaded_t *lt, uint32_t row, const char *cname)
{
    uint8_t w;
    return msi_string(db, raw_cell(lt, row, cname, &w));
}

static int32_t cell_i(msi_loaded_t *lt, uint32_t row, const char *cname)
{
    uint8_t w;
    uint32_t raw = raw_cell(lt, row, cname, &w);
    if (raw == 0) return 0;                       /* null */
    if (w == 2) return (int32_t)(raw - 0x8000u);
    return (int32_t)(raw - 0x80000000u);
}

/* ── Directory tree resolution ───────────────────────────────── */

/* Extract the long target name from a DefaultDir value
 * ("short|long[:srcshort|srclong]"). */
static void def_long(const char *defdir, char *out, int cap)
{
    char tmp[256];
    int j = 0;
    for (const char *p = defdir; *p && *p != ':' && j < 255; p++) tmp[j++] = *p;
    tmp[j] = 0;
    const char *name = tmp;
    for (char *p = tmp; *p; p++) if (*p == '|') name = p + 1;
    int k = 0;
    for (const char *p = name; *p && k < cap - 1; p++) out[k++] = *p;
    out[k] = 0;
}

static int find_row_str(msi_db_t *db, msi_loaded_t *lt, const char *col, const char *key)
{
    for (uint32_t r = 0; r < lt->nrows; r++)
        if (meq(cell_s(db, lt, r, col), key)) return (int)r;
    return -1;
}

static void resolve_dir(msi_db_t *db, msi_loaded_t *dir,
                        const char *key, char *out, int cap)
{
    out[0] = 0;
    if (!key || !*key) return;
    if (meq(key, "TARGETDIR") || meq(key, "SourceDir")) return;  /* root */

    int r = find_row_str(db, dir, "Directory", key);
    if (r < 0) return;

    const char *parent = cell_s(db, dir, (uint32_t)r, "Directory_Parent");
    const char *defdir = cell_s(db, dir, (uint32_t)r, "DefaultDir");
    char self[256];
    def_long(defdir, self, sizeof self);

    char pbuf[512]; pbuf[0] = 0;
    if (parent && *parent && !meq(parent, key))
        resolve_dir(db, dir, parent, pbuf, sizeof pbuf);

    int pos = 0;
    for (const char *p = pbuf; *p && pos < cap - 1; p++) out[pos++] = *p;
    if (self[0] && !meq(self, ".")) {
        if (pos > 0 && pos < cap - 1) out[pos++] = '\\';
        for (const char *p = self; *p && pos < cap - 1; p++) out[pos++] = *p;
    }
    out[pos] = 0;
}

static void join_path(const char *dir, const char *name, char *out, int cap)
{
    int pos = 0;
    for (const char *p = dir; *p && pos < cap - 1; p++) out[pos++] = *p;
    if (pos > 0 && out[pos - 1] != '\\' && pos < cap - 1) out[pos++] = '\\';
    for (const char *p = name; *p && pos < cap - 1; p++) out[pos++] = *p;
    out[pos] = 0;
}

/* ── Registry table application ───────────────────────────────── */

static long parse_int(const char *s, int base)
{
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    return neg ? -v : v;
}

static void apply_registry(int32_t root, const char *key,
                           const char *name, const char *val)
{
    const char *pfx;
    switch (root) {
        case 0:  pfx = "hkcr"; break;
        case 1:  pfx = "hkcu"; break;
        case 2:  pfx = "hklm"; break;
        case 3:  pfx = "hku";  break;
        default: pfx = "hklm"; break;   /* -1 = ALLUSERS-dependent → machine */
    }

    char path[300];
    int pos = 0;
    for (const char *p = pfx; *p && pos < 299; p++) path[pos++] = *p;
    if (key && *key) {
        if (pos < 299) path[pos++] = '\\';
        for (const char *p = key; *p && pos < 299; p++) {
            char c = (*p == '/') ? '\\' : *p;
            path[pos++] = lc(c);
        }
    }
    path[pos] = 0;

    const char *vname = name ? name : "";
    if ((vname[0] == '+' || vname[0] == '-' || vname[0] == '*') && vname[1] == 0)
        vname = "";

    if (!val || !*val) return;   /* key-create marker only — nothing to store */

    if (val[0] == '#' && val[1] != '#') {
        const char *q = val + 1;
        int base = 10;
        if (*q == 'x' || *q == 'X') { base = 16; q++; }
        DWORD dw = (DWORD)parse_int(q, base);
        advapi32_reg_install_set(path, vname, REG_DWORD, &dw, 4);
    } else {
        const char *s = val;
        if (s[0] == '#' && s[1] == '#') s++;   /* ## escape → literal # */
        advapi32_reg_install_set(path, vname, REG_SZ, s, mstrlen(s) + 1);
    }
}

/* ── file payload extraction ─────────────────────────────────── */

typedef struct {
    char           cab_name[MSI_NAME];
    cab_archive_t *cab;
    uint8_t       *cab_buf;
} cab_cache_t;

static const char *media_cab_for_seq(msi_db_t *db, msi_loaded_t *media, int32_t seq)
{
    const char *best = 0;
    int32_t best_ls = 0x7fffffff;
    for (uint32_t r = 0; r < media->nrows; r++) {
        int32_t ls = cell_i(media, r, "LastSequence");
        if (seq <= ls && ls < best_ls) { best_ls = ls; best = cell_s(db, media, r, "Cabinet"); }
    }
    return best;
}

/* Open (and cache) the cabinet named `name` ("#internal" → ole2 stream). */
static cab_archive_t *open_cab(msi_db_t *db, cab_cache_t *cc, const char *name)
{
    if (!name || !*name) return 0;
    if (cc->cab && meq(cc->cab_name, name)) return cc->cab;

    /* drop previous */
    if (cc->cab)     { cab_close(cc->cab); cc->cab = 0; }
    if (cc->cab_buf) { kfree(cc->cab_buf); cc->cab_buf = 0; }
    cc->cab_name[0] = 0;

    if (name[0] != '#') {
        serial_puts("[MSI] external cabinet unsupported: ");
        serial_puts(name); serial_puts("\n");
        return 0;
    }
    int idx = find_stream(db->ole, name + 1);
    if (idx < 0) { serial_puts("[MSI] cab stream not found\n"); return 0; }
    uint32_t csz = 0;
    cc->cab_buf = ole2_read_index(db->ole, idx, &csz);
    if (!cc->cab_buf) return 0;
    cc->cab = cab_open(cc->cab_buf, csz);
    if (!cc->cab) { kfree(cc->cab_buf); cc->cab_buf = 0; return 0; }
    int j = 0; for (const char *p = name; *p && j < MSI_NAME - 1; p++) cc->cab_name[j++] = *p;
    cc->cab_name[j] = 0;
    return cc->cab;
}

/* ── install ─────────────────────────────────────────────────── */

int msi_install(msi_db_t *db, const char *pkg_name)
{
    if (!db) return -1;

    msi_loaded_t dir, comp, file, media, reg;
    int has_dir   = load_table(db, "Directory", &dir)   == 0;
    int has_comp  = load_table(db, "Component", &comp)   == 0;
    int has_file  = load_table(db, "File",      &file)   == 0;
    int has_media = load_table(db, "Media",     &media)  == 0;
    int has_reg   = load_table(db, "Registry",  &reg)    == 0;

    installer_manifest_begin(pkg_name);

    int written = 0;
    cab_cache_t cc; memset(&cc, 0, sizeof cc);

    if (has_file && has_comp && has_dir) {
        for (uint32_t r = 0; r < file.nrows; r++) {
            const char *fkey  = cell_s(db, &file, r, "File");
            const char *comp_ = cell_s(db, &file, r, "Component_");
            const char *fname = cell_s(db, &file, r, "FileName");
            int32_t seq       = cell_i(&file, r, "Sequence");

            /* component → directory key → resolved path */
            int cr = find_row_str(db, &comp, "Component", comp_);
            const char *dirkey = (cr >= 0) ? cell_s(db, &comp, (uint32_t)cr, "Directory_") : "";
            char dirpath[512];
            resolve_dir(db, &dir, dirkey, dirpath, sizeof dirpath);

            char longname[256];
            def_long(fname, longname, sizeof longname);
            char target[768];
            join_path(dirpath, longname, target, sizeof target);

            /* fetch bytes: from cabinet, else uncompressed stream */
            uint32_t bsz = 0;
            uint8_t *bytes = 0;
            const char *cabname = has_media ? media_cab_for_seq(db, &media, seq) : 0;
            if (cabname) {
                cab_archive_t *cab = open_cab(db, &cc, cabname);
                if (cab) bytes = cab_extract(cab, fkey, &bsz);
            }
            if (!bytes) {
                int sidx = find_stream(db->ole, fkey);   /* uncompressed */
                if (sidx >= 0) bytes = ole2_read_index(db->ole, sidx, &bsz);
            }

            if (!bytes) {
                serial_puts("[MSI] missing payload for "); serial_puts(fkey); serial_puts("\n");
                continue;
            }

            serial_puts("[MSI] install "); serial_puts(target);
            serial_puts(" ("); serial_putdec(bsz); serial_puts(" bytes)\n");
            if (installer_write_file(target, bytes, bsz) == 0) {
                installer_manifest_add(target);
                written++;
            }
            kfree(bytes);
        }
    } else {
        serial_puts("[MSI] no File/Component/Directory tables — nothing to extract\n");
    }

    if (cc.cab)     cab_close(cc.cab);
    if (cc.cab_buf) kfree(cc.cab_buf);

    if (has_reg) {
        for (uint32_t r = 0; r < reg.nrows; r++) {
            int32_t root    = cell_i(&reg, r, "Root");
            const char *key = cell_s(db, &reg, r, "Key");
            const char *nm  = cell_s(db, &reg, r, "Name");
            const char *val = cell_s(db, &reg, r, "Value");
            serial_puts("[MSI] registry root="); serial_putdec((uint64_t)(uint32_t)root);
            serial_puts(" "); serial_puts(key); serial_puts("\n");
            apply_registry(root, key, nm, val);
        }
    }

    installer_manifest_commit();

    if (has_dir)   free_table(&dir);
    if (has_comp)  free_table(&comp);
    if (has_file)  free_table(&file);
    if (has_media) free_table(&media);
    if (has_reg)   free_table(&reg);

    serial_puts("[MSI] install complete: "); serial_putdec((uint64_t)written);
    serial_puts(" files\n");
    return written;
}
