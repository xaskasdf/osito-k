#include "../fs/boot_kernel.c"
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);

static unsigned checks, failures, allocations;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
void *kmalloc(uint64_t n) { void *p=malloc(n); if(p) allocations++; return p; }
void kfree(void *p) { if(p) { allocations--; free(p); } }

typedef struct {
    uint8_t *disk, *pristine;
    uint64_t bytes, base, esp_bytes, data_offset, fat_off, root, efi, boot, data;
    uint32_t bps, fat_sectors, clusters, width, old_size;
    unsigned calls, fail_at, reads, writes, flushes;
    bool after_write, permanent, corrupt;
} fixture_t;
static fixture_t f;
static uint8_t payload[14000];
static bool fail_io(void) { f.calls++; return f.fail_at && (f.calls==f.fail_at || (f.permanent && f.calls>=f.fail_at)); }
static int rd(void *ctx, uint64_t off, void *buf, uint64_t n)
{
    CHECK(ctx==&f); f.reads++;
    if (off>f.bytes || n>f.bytes-off || fail_io()) return -1;
    memcpy(buf,f.disk+off,n); return 0;
}
static int wr(void *ctx, uint64_t off, const void *buf, uint64_t n)
{
    CHECK(ctx==&f); f.writes++;
    CHECK(off>=f.base && off+n<=f.base+f.esp_bytes);
    if (off>f.bytes || n>f.bytes-off) return -1;
    bool failure=fail_io();
    if (!failure || f.after_write) memcpy(f.disk+off,buf,n);
    if (f.corrupt && n && !failure) f.disk[off]^=0x40;
    return failure ? -1 : 0;
}
static int flush(void *ctx) { CHECK(ctx==&f); f.flushes++; return fail_io() ? -1 : 0; }
static boot_kernel_io_t io(void) { return (boot_kernel_io_t){&f,f.bps,f.bytes/f.bps,rd,wr,flush}; }
static uint64_t cluster(uint32_t c) { return f.data+(uint64_t)(c-2)*f.bps; }
static void fat_entry(uint32_t c, uint32_t value)
{
    for (unsigned copy=0;copy<2;copy++) {
        uint8_t *p=f.disk+f.fat_off+(uint64_t)copy*f.fat_sectors*f.bps+c*f.width;
        if(f.width==4) p32(p,value); else p16(p,value);
    }
}
static void entry(uint64_t off, const char *name, uint8_t attr, uint32_t c, uint32_t size)
{
    uint8_t *p=f.disk+off; memcpy(p,name,11); p[11]=attr;
    p16(p+26,c); if(f.width==4) p16(p+20,c>>16); p32(p+28,size);
}
static void gpt_crc(void)
{
    gpt_header_t *h=(void *)(f.disk+f.bps);
    h->partition_entries_crc32=crc32(f.disk+2*f.bps,128*128);
    h->header_crc32=0; h->header_crc32=crc32(h,h->header_size);
}
static void setup(unsigned width, unsigned bps)
{
    memset(&f,0,sizeof(f));
    f.width=width; f.bps=bps; f.clusters=width==4 ? 66000 : 5000;
    f.fat_sectors=((f.clusters+2)*width+bps-1)/bps;
    unsigned reserved=width==4 ? 32 : 1, roots=width==4 ? 0 : 512;
    uint32_t data_sector=reserved+2*f.fat_sectors+(roots*32+bps-1)/bps;
    f.base=64ULL*bps; f.esp_bytes=(uint64_t)(data_sector+f.clusters)*bps;
    f.data_offset=f.base+f.esp_bytes+128*bps;
    f.bytes=f.data_offset+128*bps;
    f.disk=malloc(f.bytes); f.pristine=malloc(f.bytes);
    CHECK(f.disk && f.pristine); memset(f.disk,0,f.bytes);
    gpt_header_t *h=(void *)(f.disk+bps);
    memcpy(h->signature,"EFI PART",8); h->revision=0x10000; h->header_size=sizeof(*h);
    h->current_lba=1; h->backup_lba=f.bytes/bps-1;
    h->first_usable_lba=64; h->last_usable_lba=h->backup_lba-33;
    h->partition_entry_lba=2; h->num_partition_entries=128; h->partition_entry_size=128;
    gpt_entry_t *g=(void *)(f.disk+2*bps);
    g[0].type_guid=(gpt_guid_t){0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b}};
    g[0].unique_guid.d1=1; g[0].first_lba=f.base/bps; g[0].last_lba=(f.base+f.esp_bytes)/bps-1;
    g[1].type_guid.d1=123; g[1].unique_guid.d1=2;
    g[1].first_lba=f.data_offset/bps; g[1].last_lba=h->last_usable_lba;
    gpt_crc();
    uint8_t *b=f.disk+f.base; b[0]=0xeb; b[510]=0x55; b[511]=0xaa;
    p16(b+11,bps); b[13]=1; p16(b+14,reserved); b[16]=2; p16(b+17,roots);
    p32(b+32,data_sector+f.clusters);
    if(width==4) {
        p32(b+36,f.fat_sectors); p32(b+44,2); p16(b+48,1); p16(b+50,6);
        for(unsigned i=1;i<=7;i+=6) {
            uint8_t *info=b+i*bps; p32(info,0x41615252); p32(info+484,0x61417272);
            p32(info+488,f.clusters-7); p32(info+492,9); p32(info+508,0xaa550000);
        }
    } else p16(b+22,f.fat_sectors);
    f.fat_off=f.base+(uint64_t)reserved*bps;
    f.data=f.base+(uint64_t)data_sector*bps;
    f.root=width==4 ? cluster(2) : f.base+(uint64_t)(reserved+2*f.fat_sectors)*bps;
    f.efi=cluster(3); f.boot=cluster(4); f.old_size=bps+123;
    fat_entry(0,0x0ffffff8); fat_entry(1,0x0fffffff);
    if(width==4) fat_entry(2,0x0fffffff);
    for(unsigned c=3;c<=8;c++) fat_entry(c,c==5 ? 8 : 0x0fffffff);
    entry(f.root,"EFI        ",16,3,0);
    entry(f.efi,".          ",16,3,0); entry(f.efi+32,"..         ",16,width==4?2:0,0);
    entry(f.efi+64,"BOOT       ",16,4,0);
    entry(f.boot,".          ",16,4,0); entry(f.boot+32,"..         ",16,3,0);
    entry(f.boot+64,"KERNEL  ELF",32,5,f.old_size);
    entry(f.boot+96,"BOOTX64 EFI",32,6,128); entry(f.boot+128,"OTHER   BIN",32,7,100);
    memset(f.disk+cluster(5),0x41,bps); memset(f.disk+cluster(8),0x41,123);
    memset(f.disk+cluster(6),0x4d,128); memset(f.disk+cluster(7),0x78,100);
    memset(payload,0x5a,sizeof(payload));
    memcpy(f.pristine,f.disk,f.bytes);
}
static void reset(void)
{
    memcpy(f.disk,f.pristine,f.bytes); f.calls=f.reads=f.writes=f.flushes=f.fail_at=0;
    f.after_write=f.permanent=f.corrupt=false;
}
static boot_kernel_t *open_fixture(void)
{
    boot_kernel_t *s=NULL; boot_kernel_io_t x=io();
    const char *e=boot_kernel_open(&x,f.data_offset,&s);
    if(e) printf("open: %s\n",e);
    CHECK(!e && s); return s;
}
static void intact(void)
{
    CHECK(equal(f.disk,f.pristine,f.base));
    CHECK(equal(f.disk+f.base+f.esp_bytes,f.pristine+f.base+f.esp_bytes,f.bytes-f.base-f.esp_bytes));
    CHECK(equal(f.disk+cluster(6),f.pristine+cluster(6),f.bps));
    CHECK(equal(f.disk+cluster(7),f.pristine+cluster(7),f.bps));
}
static void current_valid(void)
{
    /* Even a persistent I/O outage must leave old or new complete data, not
     * a directory pointing at a freed or partially written chain. */
    uint8_t *e=f.disk+f.boot+64;
    uint32_t c=u16(e+26)|(f.width==4 ? (uint32_t)u16(e+20)<<16 : 0), size=u32(e+28);
    CHECK(size==f.old_size || size==2*f.bps+211);
    uint8_t expected=size==f.old_size?0x41:0x5a;
    unsigned loops=0;
    while(size && ++loops<8) {
        CHECK(c>=2 && c<f.clusters+2); if(c<2 || c>=f.clusters+2) break;
        unsigned n=size<f.bps?size:f.bps;
        bool match=true; for(unsigned i=0;i<n;i++) if(f.disk[cluster(c)+i]!=expected) match=false;
        CHECK(match);
        uint8_t *p=f.disk+f.fat_off+c*f.width;
        c=f.width==4 ? u32(p)&0xfffffff : u16(p); size-=n;
    }
    CHECK(size==0); intact();
}
static void success(void)
{
    boot_kernel_t *s=open_fixture(); if(!s) return;
    uint8_t old[9000]; CHECK(!boot_kernel_read(s,old)); CHECK(old[0]==0x41 && old[f.old_size-1]==0x41);
    unsigned size=2*f.bps+211;
    CHECK(!boot_kernel_replace(s,payload,size)); boot_kernel_close(s);
    current_valid(); s=open_fixture();
    if(s) { CHECK(boot_kernel_size(s)==size); CHECK(!boot_kernel_read(s,old)); CHECK(equal(old,payload,size)); boot_kernel_close(s); }
    /* No accumulating old chains after repeated updates of different sizes. */
    for(unsigned i=0;i<4;i++) {
        s=open_fixture(); if(!s) break;
        CHECK(!boot_kernel_replace(s,payload,size-i*33)); boot_kernel_close(s);
    }
    CHECK(allocations==0);
}
static void failures_at_every_io(void)
{
    reset(); boot_kernel_t *s=open_fixture(); unsigned begin=f.calls;
    CHECK(!boot_kernel_replace(s,payload,2*f.bps+211)); unsigned operations=f.calls-begin;
    boot_kernel_close(s);
    for(unsigned mode=0;mode<3;mode++) for(unsigned n=1;n<=operations;n++) {
        reset(); s=open_fixture(); f.fail_at=f.calls+n;
        f.after_write=mode!=0; f.permanent=mode==2;
        const char *error=boot_kernel_replace(s,payload,2*f.bps+211);
        CHECK(error!=NULL); boot_kernel_close(s); current_valid(); CHECK(!allocations);
    }
    reset(); s=open_fixture(); f.corrupt=true;
    CHECK(boot_kernel_replace(s,payload,2*f.bps+211)!=NULL); boot_kernel_close(s);
    CHECK(u16(f.disk+f.boot+64+26)==5); CHECK(!allocations);
}
static void reject(void)
{
    boot_kernel_t *s=NULL; boot_kernel_io_t x=io();
    CHECK(boot_kernel_open(&x,f.data_offset,&s)!=NULL); CHECK(!s); CHECK(!f.writes); CHECK(!allocations);
}
static void malformed(void)
{
    reset(); f.disk[f.bps+16]^=1; reject();
    reset(); f.disk[2*f.bps]^=1; reject();
    reset(); ((gpt_header_t *)(f.disk+f.bps))->header_size=5000; reject();
    reset(); ((gpt_entry_t *)(f.disk+2*f.bps))[1].first_lba=f.base/f.bps; gpt_crc(); reject();
    reset(); ((gpt_entry_t *)(f.disk+2*f.bps))[0].attributes=1ULL<<60; gpt_crc(); reject();
    reset(); f.disk[f.base+13]=3; reject();
    reset(); f.disk[f.fat_off+f.fat_sectors*f.bps+40]^=1; reject();
    reset(); fat_entry(5,5); reject();
    reset(); entry(f.boot+128,"OTHER   BIN",32,5,f.old_size); reject();
    reset(); fat_entry(5,0); reject();
    reset(); p32(f.disk+f.boot+64+28,BOOT_KERNEL_MAX+1); reject();
    reset(); f.disk[f.boot+64+11]=1; reject();
    reset(); f.disk[f.boot+96]='X'; reject();
    reset(); f.data_offset+=f.bps; reject(); f.data_offset-=f.bps;
    reset(); boot_kernel_t *s=open_fixture();
    for(unsigned c=2;c<s->clusters+2;c++) if(!next(s,c)) set_next(s,c,0x0fffffff);
    CHECK(boot_kernel_replace(s,payload,2*f.bps+211)!=NULL); CHECK(!f.writes); boot_kernel_close(s);
    reset(); s=open_fixture(); CHECK(boot_kernel_replace(s,payload,0)!=NULL); CHECK(!f.writes); boot_kernel_close(s);
    if(f.width==4) {
        reset(); memset(f.disk+f.base+7*f.bps,0,f.bps); s=open_fixture();
        CHECK(!boot_kernel_replace(s,payload,2*f.bps+211)); boot_kernel_close(s);
        CHECK(u32(f.disk+f.base+7*f.bps)==0); current_valid();
    }
}
int main(void)
{
    for(unsigned width=2;width<=4;width+=2) for(unsigned bps=512;bps<=4096;bps*=8) {
        setup(width,bps); success(); malformed();
        if(bps==512) failures_at_every_io();
        free(f.disk); free(f.pristine);
    }
    printf("boot kernel: %u checks, %u failures\n",checks,failures); return failures?1:0;
}
