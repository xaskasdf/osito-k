/* Identical, off-screen DIB probes for native Windows and Osito PE32/PE64. */
typedef unsigned int UINT, DWORD;
typedef unsigned short WORD;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define COPY 0x00CC0020u
typedef struct {
    DWORD size; LONG width, height; WORD planes, bpp;
    DWORD compression, image_size; LONG xppm, yppm; DWORD used, important;
} HEADER;
typedef struct { HEADER header; DWORD masks[3]; } INFO;
typedef struct {
    LONG type, width, height, pitch; WORD planes, bpp; void *bits;
} BITMAP;
typedef struct { BITMAP bitmap; HEADER header; DWORD masks[3]; HANDLE section; DWORD offset; } DIBSECTION;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL CreateFileMappingA(HANDLE, void *, DWORD, DWORD, DWORD, const char *);
API BOOL CALL CloseHandle(HANDLE);
API HANDLE CALL CreateCompatibleDC(HANDLE);
API HANDLE CALL CreateDIBSection(HANDLE, const INFO *, UINT, void **, HANDLE, DWORD);
API HANDLE CALL SelectObject(HANDLE, HANDLE);
API BOOL CALL DeleteObject(HANDLE);
API BOOL CALL DeleteDC(HANDLE);
API int CALL GetObjectA(HANDLE, int, void *);
API int CALL GetDIBits(HANDLE, HANDLE, UINT, UINT, void *, INFO *, UINT);
API BOOL CALL BitBlt(HANDLE, int, int, int, int, HANDLE, int, int, DWORD);
API BOOL CALL StretchBlt(HANDLE, int, int, int, int, HANDLE, int, int, int, int, DWORD);
API int CALL StretchDIBits(HANDLE, int, int, int, int, int, int, int, int, const void *, const INFO *, UINT, DWORD);
API int CALL SetDIBitsToDevice(HANDLE, int, int, DWORD, DWORD, int, int, UINT, UINT, const void *, const INFO *, UINT);
API BOOL CALL PatBlt(HANDLE, int, int, int, int, DWORD);
API HANDLE CALL CreateSolidBrush(DWORD);
API BOOL CALL GdiFlush(void);

void *memset(void *p, int c, __SIZE_TYPE__ n)
{
    unsigned char *b = p; while (n--) *b++ = (unsigned char)c; return p;
}
static UINT checks, failures;
static void report(const char *s)
{
    DWORD n = 0, written; while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void hex(DWORD value)
{
    char text[9];
    for (unsigned i = 0; i < 8; i++) text[i] = "0123456789ABCDEF"[(value >> (28 - 4*i)) & 15];
    text[8] = 0; report(text);
}
static void check(BOOL ok, const char *name)
{
    checks++; if (!ok) { failures++; report("FAIL "); report(name); report("\n"); }
}
static DWORD expand(DWORD pixel, DWORD mask)
{
    unsigned shift = 0; while (!(mask & 1)) { mask >>= 1; shift++; }
    DWORD v = (pixel >> shift) & mask;
    return (v * 255 + mask / 2) / mask;
}
static DWORD unpack(DWORD p, const DWORD *mask)
{
    return (expand(p, mask[0]) << 16) | (expand(p, mask[1]) << 8) | expand(p, mask[2]);
}
static INFO info(int width, int height, unsigned bpp, DWORD compression, const DWORD *masks)
{
    INFO i = {{40, width, height, 1, (WORD)bpp, compression, 0, 0, 0, 0, 0}, {0,0,0}};
    if (masks) for (int c = 0; c < 3; c++) i.masks[c] = masks[c];
    return i;
}
static void format_case(unsigned bpp, DWORD compression, const DWORD *masks, int height)
{
    report("FORMAT "); hex(bpp); report(" "); hex(compression); report(" ");
    for (int c = 0; c < 3; c++) { hex(masks[c]); report(" "); } report("\n");
    HANDLE src = CreateCompatibleDC(0), dst = CreateCompatibleDC(0);
    INFO si = info(4, height, bpp, compression, masks), di = info(4, -2, 32, 0, 0);
    void *source = 0; DWORD *pixels = 0;
    HANDLE sb = CreateDIBSection(src, &si, 0, &source, 0, 0);
    HANDLE db = CreateDIBSection(dst, &di, 0, (void **)&pixels, 0, 0);
    check(src && dst && sb && db && source && pixels, "DIB allocation");
    if (!sb || !db) ExitProcess(2);
    DIBSECTION ds; memset(&ds, 0x55, sizeof(ds));
    int result = GetObjectA(sb, sizeof(ds), &ds);
    report("OBJECT bytes="); hex(result); report(" height="); hex(ds.header.height);
    report(" compression="); hex(ds.header.compression); report(" masks=");
    for (int c = 0; c < 3; c++) { hex(ds.masks[c]); report(" "); } report("\n");
    check(result == sizeof(ds), "GetObject DIBSECTION ABI size");
    check(ds.bitmap.bits == source && ds.bitmap.bpp == bpp && ds.bitmap.height == 2,
          "DIBSECTION BITMAP describes pixels");
    check(ds.header.size == 40 && ds.header.width == 4 && ds.header.bpp == bpp &&
          ds.header.compression == (bpp == 16 ? 3 : compression), "DIBSECTION normalized format");
    if (compression == 3 || bpp == 16)
        check(ds.masks[0] == masks[0] && ds.masks[1] == masks[1] && ds.masks[2] == masks[2],
              "DIBSECTION preserves masks");
    HANDLE old_s = SelectObject(src, sb), old_d = SelectObject(dst, db);
    DWORD values[8] = {masks[0], masks[1], masks[2], 0, masks[0]|masks[1]|masks[2], 0, masks[0], masks[2]};
    for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 4; x++) {
        unsigned at = (height < 0 ? y : 1-y) * 4 + x;
        if (bpp == 16) ((WORD *)source)[at] = (WORD)values[y*4+x];
        else ((DWORD *)source)[at] = values[y*4+x];
    }
    memset(pixels, 0x55, 32);
    check(BitBlt(dst, 0, 0, 4, 2, src, 0, 0, COPY), "BitBlt conversion succeeds");
    GdiFlush();
    for (unsigned i = 0; i < 8; i++) {
        check((pixels[i] & 0xFFFFFF) == unpack(values[i], masks), "BitBlt primary colors and row order");
        if ((pixels[i] & 0xFFFFFF) != unpack(values[i], masks)) {
            report("pixel "); hex(i); report(" got="); hex(pixels[i]); report("\n");
        }
    }
    HANDLE red = CreateSolidBrush(255), old_brush = SelectObject(src, red);
    check(PatBlt(src, 0, 0, 1, 1, 0x00F00021), "write to masked DIB");
    GdiFlush();
    unsigned at = height < 0 ? 0 : 4;
    DWORD sample = bpp == 16 ? ((WORD *)source)[at] : ((DWORD *)source)[at];
    check((sample & (masks[0]|masks[1]|masks[2])) == masks[0],
          "write uses the selected DIB masks");
    SelectObject(src, old_brush); DeleteObject(red);
    memset(pixels, 0x55, 32);
    check(StretchDIBits(dst, 0, 0, 4, 2, 0, 0, 4, 2, source, &si, 0, COPY) != 0,
          "StretchDIBits accepts same header");
    GdiFlush();
    check((pixels[0] & 0xFFFFFF) == 0xFF0000 && (pixels[1] & 0xFFFFFF) == 0x00FF00,
          "StretchDIBits and selected DIB agree");
    SelectObject(src, old_s);
    INFO output = info(4, -2, 32, 0, 0);
    DWORD converted[8]; memset(converted, 0x55, sizeof(converted));
    check(GetDIBits(dst, sb, 0, 2, converted, &output, 0) == 2, "GetDIBits converts to top-down RGB32");
    check(output.header.height == -2 && (converted[0] & 0xFFFFFF) == 0xFF0000 &&
          (converted[1] & 0xFFFFFF) == 0x00FF00, "GetDIBits honors requested orientation and colors");
    SelectObject(dst, old_d);
    check(DeleteObject(sb) && DeleteObject(db) && DeleteDC(src) && DeleteDC(dst), "DIB resources released");
}

static void cross_format(void)
{
    static const DWORD rgb555[3] = {0x7C00,0x3E0,0x1F}, rgb565[3] = {0xF800,0x7E0,0x1F};
    HANDLE a = CreateCompatibleDC(0), b = CreateCompatibleDC(0);
    INFO ai = info(4,-1,16,0,0), bi = info(4,-1,16,3,rgb565);
    WORD *ap, *bp;
    HANDLE ab = CreateDIBSection(a,&ai,0,(void **)&ap,0,0);
    HANDLE bb = CreateDIBSection(b,&bi,0,(void **)&bp,0,0);
    if (!ab || !bb) ExitProcess(2);
    HANDLE ao = SelectObject(a,ab), bo = SelectObject(b,bb);
    for (int i=0;i<3;i++) ap[i] = (WORD)rgb555[i]; ap[3]=0x7FFF;
    check(BitBlt(b,0,0,4,1,a,0,0,COPY), "same depth conversion succeeds"); GdiFlush();
    check(bp[0]==0xF800 && bp[1]==0x7E0 && bp[2]==0x1F && bp[3]==0xFFFF,
          "same depth different masks is not a byte copy");
    bp[0]=bp[1]=bp[2]=bp[3]=0;
    check(StretchBlt(b,0,0,4,1,a,0,0,2,1,COPY), "scaled memory blit"); GdiFlush();
    check(bp[0]==0xF800 && bp[1]==0xF800 && bp[2]==0x7E0 && bp[3]==0x7E0,
          "scaled memory blit converts colors");
    SelectObject(a,ao); SelectObject(b,bo); DeleteObject(ab); DeleteObject(bb); DeleteDC(a); DeleteDC(b);
}

static void shared_dib(void)
{
    HANDLE mapping = CreateFileMappingA((HANDLE)(UPTR)-1,0,4,0,4096,0);
    check(mapping != 0, "mapping allocated");
    INFO i = info(3,-2,16,0,0); void *bits = 0;
    HANDLE bitmap = CreateDIBSection(0,&i,0,&bits,mapping,128);
    check(bitmap && bits, "section-backed DIB created");
    if (bitmap) {
        DIBSECTION ds; memset(&ds,0,sizeof(ds));
        check(GetObjectA(bitmap,sizeof(ds),&ds)==sizeof(ds) && ds.bitmap.bits==bits &&
              ds.section==mapping && ds.offset==128, "section-backed metadata and ABI");
        DeleteObject(bitmap);
    }
    CloseHandle(mapping);
}

static void channel_ramp(void)
{
    static const DWORD masks[3] = {0xF800, 0x7E0, 0x1F};
    HANDLE a = CreateCompatibleDC(0), b = CreateCompatibleDC(0);
    INFO ai = info(256,-1,16,3,masks), bi = info(256,-1,32,0,0);
    WORD *ap; DWORD *bp;
    HANDLE ab = CreateDIBSection(a,&ai,0,(void **)&ap,0,0);
    HANDLE bb = CreateDIBSection(b,&bi,0,(void **)&bp,0,0);
    if (!ab || !bb) ExitProcess(2);
    HANDLE ao = SelectObject(a,ab), bo = SelectObject(b,bb);
    for (unsigned i=0;i<256;i++) ap[i]=(WORD)(((i&31)<<11)|((i&63)<<5)|(i&31));
    check(BitBlt(b,0,0,256,1,a,0,0,COPY), "expand channel ramp"); GdiFlush();
    for (unsigned i=0;i<256;i++) {
        DWORD r=((i&31)<<3)|((i&31)>>2), g=((i&63)<<2)|((i&63)>>4);
        check((bp[i]&0xFFFFFF)==((r<<16)|(g<<8)|r), "channel expansion replicates bits");
        bp[i]=(i<<16)|(i<<8)|i;
    }
    check(BitBlt(a,0,0,256,1,b,0,0,COPY), "reduce channel ramp"); GdiFlush();
    for (unsigned i=0;i<256;i++)
        check(ap[i]==(WORD)(((i>>3)<<11)|((i>>2)<<5)|(i>>3)), "channel reduction truncates bits");
    SelectObject(a,ao); SelectObject(b,bo); DeleteObject(ab); DeleteObject(bb); DeleteDC(a); DeleteDC(b);
}

static void unscaled_rows(unsigned source_bpp, unsigned dest_bpp, int source_bottom, int dest_bottom)
{
    unsigned char source[24], *pixels = 0;
    unsigned source_bytes = source_bpp / 8, dest_bytes = dest_bpp / 8;
    unsigned pitch = (5 * dest_bytes + 3) & ~3u;
    INFO si = info(3, source_bottom ? 2 : -2, source_bpp, 0, 0);
    INFO di = info(5, dest_bottom ? 3 : -3, dest_bpp, 0, 0);
    HANDLE dc = CreateCompatibleDC(0);
    HANDLE bitmap = CreateDIBSection(dc, &di, 0, (void **)&pixels, 0, 0);
    check(dc && bitmap && pixels, "unscaled row destination allocated");
    if (!dc || !bitmap || !pixels) ExitProcess(2);
    HANDLE old = SelectObject(dc, bitmap);
    memset(source, 0xc7, sizeof(source));
    for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 3; x++) {
        DWORD color = (y + 1) * 0x302010 + x * 0x010307;
        unsigned at = (source_bottom ? 1 - y : y) * 12 + x * source_bytes;
        for (unsigned c = 0; c < 3; c++) source[at + c] = (unsigned char)(color >> (c * 8));
        if (source_bytes == 4) source[at + 3] = (unsigned char)(x * 64);
    }
    for (unsigned api = 0; api < 2; api++) {
        memset(pixels, 0x55, pitch * 3);
        int rows = api ? SetDIBitsToDevice(dc, 1, 1, 3, 2, 0, 0, 0, 2, source, &si, 0)
                       : StretchDIBits(dc, 1, 1, 3, 2, 0, 0, 3, 2, source, &si, 0, COPY);
        check(rows == 2, "unscaled DIB reports two rows");
        GdiFlush();
        for (unsigned y = 0; y < 3; y++) for (unsigned byte = 0; byte < pitch; byte++) {
            unsigned x = byte / dest_bytes, channel = byte % dest_bytes;
            unsigned char expected = 0x55;
            if (y >= 1 && x >= 1 && x < 4) {
                /* RGB is portable; the Osito host suite checks opaque coverage. */
                if (channel == 3) continue;
                DWORD color = y * 0x302010 + (x - 1) * 0x010307;
                expected = (unsigned char)(color >> (channel * 8));
            }
            unsigned at = (dest_bottom ? 2 - y : y) * pitch + byte;
            check(pixels[at] == expected, "unscaled DIB colors, orientation and untouched padding");
        }
    }
    SelectObject(dc, old);
    check(DeleteObject(bitmap) && DeleteDC(dc), "unscaled DIB resources released");
}

void mainCRTStartup(void)
{
    static const DWORD rgb555[3]={0x7C00,0x3E0,0x1F}, rgb565[3]={0xF800,0x7E0,0x1F};
    static const DWORD bgr565[3]={0x1F,0x7E0,0xF800}, bgr888[3]={0xFF,0xFF00,0xFF0000};
    static const DWORD rgb888[3]={0xFF0000,0xFF00,0xFF};
    format_case(16,0,rgb555,-2);
    format_case(16,0,rgb555,2);
    format_case(16,3,rgb555,-2);
    format_case(16,3,rgb565,-2);
    format_case(16,3,bgr565,-2);
    format_case(32,3,bgr888,-2);
    format_case(32,0,rgb888,-2);
    cross_format();
    shared_dib();
    channel_ramp();
    for (unsigned sb = 24; sb <= 32; sb += 8)
        for (unsigned db = 24; db <= 32; db += 8)
            for (int source_bottom = 0; source_bottom < 2; source_bottom++)
                for (int dest_bottom = 0; dest_bottom < 2; dest_bottom++)
                    unscaled_rows(sb, db, source_bottom, dest_bottom);
    report("[GDI-DIB] checks=0x"); hex(checks); report(" failures=0x"); hex(failures);
    report(failures ? " FAIL\n" : " PASS\n");
    ExitProcess(failures ? 3 : 0);
}
