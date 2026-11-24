/* Bounded GPT/FAT boot-file replacement, not a second general FAT mount.
 * New data and both FAT copies are verified before publishing one directory
 * entry. The old chain is only reclaimed after that entry is durable/read back.
 * FAT has no journal: a device that tears sectors or lies about flush can still
 * require repair. The caller saves a separate recovery copy in OsitoFS first. */
#include "boot_kernel.h"
#include "gpt.h"

extern void *kmalloc(uint64_t);
extern void kfree(void *);

#define MAX_FAT_BYTES (16U * 1024 * 1024)
#define MAX_DIRECTORY_ENTRIES 65536U
#define IO_BYTES 65536U

struct boot_kernel {
    boot_kernel_io_t io;
    uint64_t base, bytes, entry_offset;
    uint32_t bps, spc, reserved, fats, fat_sectors, clusters, data_sector;
    uint32_t root_entries, root_sector, root_cluster, fat_bytes, entry_count;
    uint32_t fsinfo[2];
    unsigned width;
    uint8_t *fat, *original, *seen, *scratch;
    uint8_t entry[32];
    bool loader_found, kernel_found;
};

static uint16_t u16(const void *p) { const uint8_t *b=p; return b[0] | b[1]<<8; }
static uint32_t u32(const void *p) { const uint8_t *b=p; return u16(b) | (uint32_t)u16(b+2)<<16; }
static void p16(void *p, uint16_t x) { uint8_t *b=p; b[0]=x; b[1]=x>>8; }
static void p32(void *p, uint32_t x) { uint8_t *b=p; p16(b,x); p16(b+2,x>>16); }
static bool equal(const void *a, const void *b, uint64_t n)
{
    const uint8_t *x=a, *y=b;
    while (n--) if (*x++ != *y++) return false;
    return true;
}
static uint32_t crc32(const void *p, uint32_t n)
{
    const uint8_t *b=p; uint32_t c=~0U;
    while (n--) { c ^= *b++; for (unsigned i=0;i<8;i++) c=(c>>1)^(0xedb88320U & (0U-(c&1))); }
    return ~c;
}
static const char *partition(boot_kernel_t *s, uint64_t data_offset)
{
    uint8_t block[4096];
    const boot_kernel_io_t *io=&s->io;
    if ((io->sector_size != 512 && io->sector_size != 4096) || io->sectors < 68 ||
        io->sectors > UINT64_MAX/io->sector_size || data_offset % io->sector_size)
        return "unsupported disk geometry";
    if (io->read(io->context,io->sector_size,block,io->sector_size)) return "GPT read failed";
    gpt_header_t h; memcpy(&h,block,sizeof(h));
    if (!equal(h.signature,"EFI PART",8) || h.revision != 0x10000 || h.reserved ||
        h.header_size < sizeof(h) || h.header_size > io->sector_size ||
        h.current_lba != 1 || h.backup_lba != io->sectors-1 ||
        h.first_usable_lba >= h.last_usable_lba || h.last_usable_lba >= h.backup_lba ||
        !h.num_partition_entries || h.num_partition_entries > 128 ||
        h.partition_entry_size < 128 || h.partition_entry_size > 1024 ||
        (h.partition_entry_size & (h.partition_entry_size-1))) return "invalid GPT layout";
    p32(block+16,0);
    if (crc32(block,h.header_size) != h.header_crc32) return "GPT header CRC mismatch";
    uint32_t table_bytes=h.num_partition_entries*h.partition_entry_size;
    uint64_t table_sectors=(table_bytes+io->sector_size-1)/io->sector_size;
    if (h.partition_entry_lba < 2 || h.partition_entry_lba >= h.first_usable_lba ||
        table_sectors > h.first_usable_lba-h.partition_entry_lba ||
        table_sectors > h.backup_lba-h.last_usable_lba-1) return "GPT table out of bounds";
    uint8_t *table=kmalloc(table_bytes);
    if (!table) return "out of memory (GPT)";
    const char *error="GPT entries read/CRC failed";
    if (io->read(io->context,h.partition_entry_lba*io->sector_size,table,table_bytes) ||
        crc32(table,table_bytes) != h.partition_entries_crc32) goto done;
    static const gpt_guid_t esp={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b}};
    static const gpt_guid_t zero={0};
    unsigned matches=0, data_matches=0;
    for (unsigned i=0;i<h.num_partition_entries;i++) {
        gpt_entry_t e; memcpy(&e,table+i*h.partition_entry_size,sizeof(e));
        if (equal(&e.type_guid,&zero,sizeof(zero))) continue;
        error="invalid or overlapping GPT partitions";
        if (e.first_lba < h.first_usable_lba || e.last_lba > h.last_usable_lba ||
            e.first_lba > e.last_lba || equal(&e.unique_guid,&zero,sizeof(zero))) goto done;
        for (unsigned j=0;j<i;j++) {
            gpt_entry_t other; memcpy(&other,table+j*h.partition_entry_size,sizeof(other));
            if (equal(&other.type_guid,&zero,sizeof(zero))) continue;
            if (e.first_lba <= other.last_lba && other.first_lba <= e.last_lba) goto done;
        }
        if (e.first_lba == data_offset/io->sector_size && !equal(&e.type_guid,&esp,sizeof(esp))) data_matches++;
        if (equal(&e.type_guid,&esp,sizeof(esp))) {
            /* Bit 1 excludes firmware block I/O; bit 60 conventionally read-only. */
            if (e.attributes & ((1ULL<<1)|(1ULL<<60))) { error="ESP is not writable/firmware-visible"; goto done; }
            matches++; s->base=e.first_lba*io->sector_size;
            s->bytes=(e.last_lba-e.first_lba+1)*io->sector_size;
        }
    }
    error=matches == 1 && data_matches == 1 ? NULL : "need one ESP on the mounted OsitoFS disk";
done:
    kfree(table); return error;
}
static int read_at(boot_kernel_t *s, uint64_t off, void *p, uint64_t n)
{
    if (off > s->bytes || n > s->bytes-off) return -1;
    return s->io.read(s->io.context,s->base+off,p,n);
}
static int write_at(boot_kernel_t *s, uint64_t off, const void *p, uint64_t n)
{
    if (off > s->bytes || n > s->bytes-off) return -1;
    return s->io.write(s->io.context,s->base+off,p,n);
}
static uint32_t next(boot_kernel_t *s, uint32_t cluster)
{
    const uint8_t *p=s->fat+(uint64_t)cluster*s->width;
    return s->width==4 ? u32(p)&0x0fffffff : u16(p);
}
static void set_next(boot_kernel_t *s, uint32_t cluster, uint32_t value)
{
    uint8_t *p=s->fat+(uint64_t)cluster*s->width;
    if (s->width==4) p32(p,(u32(p)&0xf0000000)|(value&0x0fffffff));
    else p16(p,value);
}
static bool eoc(boot_kernel_t *s, uint32_t c) { return c >= (s->width==4 ? 0x0ffffff8U : 0xfff8U); }
static bool valid(boot_kernel_t *s, uint32_t c) { return c >= 2 && c < s->clusters+2; }
static uint64_t cluster_offset(boot_kernel_t *s, uint32_t c)
{ return ((uint64_t)s->data_sector+(uint64_t)(c-2)*s->spc)*s->bps; }
static uint32_t first_cluster(boot_kernel_t *s, const uint8_t *e)
{ return u16(e+26) | (s->width==4 ? (uint32_t)u16(e+20)<<16 : 0); }

/* Mark every reachable chain, rejecting cycles and cross-links before writes. */
static int mark_chain(boot_kernel_t *s, uint32_t c, uint32_t size, bool directory)
{
    uint64_t capacity=0;
    if (!c && !directory && !size) return 0;
    while (valid(s,c)) {
        uint8_t bit=1U<<(c&7);
        if (s->seen[c/8]&bit) return -1;
        s->seen[c/8]|=bit;
        capacity+=(uint64_t)s->spc*s->bps;
        c=next(s,c);
        if (eoc(s,c)) return directory || (capacity >= size && capacity-size < (uint64_t)s->spc*s->bps) ? 0 : -1;
    }
    return -1;
}
static const char *directory(boot_kernel_t *s, uint32_t cluster, unsigned depth, unsigned path)
{
    if (depth > 16) return "ESP directory depth limit";
    bool fixed=cluster==0 && s->width==2 && depth==0;
    if (!fixed && mark_chain(s,cluster,0,true)) return "invalid/cross-linked ESP directory";
    uint32_t sectors=fixed ? (s->root_entries*32+s->bps-1)/s->bps : s->spc;
    uint8_t block[4096];
    do {
        uint64_t off=fixed ? (uint64_t)s->root_sector*s->bps : cluster_offset(s,cluster);
        for (uint32_t sector=0;sector<sectors;sector++) {
            if (read_at(s,off+(uint64_t)sector*s->bps,block,s->bps)) return "ESP directory read failed";
            for (unsigned pos=0;pos<s->bps;pos+=32) {
                uint8_t *e=block+pos;
                if (++s->entry_count > MAX_DIRECTORY_ENTRIES) return "ESP directory entry limit";
                if (!e[0]) return NULL;
                if (e[0]==0xe5 || e[11]==0x0f || (e[11]&8)) continue;
                if (equal(e,".          ",11) || equal(e,"..         ",11)) continue;
                uint32_t c=first_cluster(s,e), size=u32(e+28);
                if (e[11]&16) {
                    unsigned child=3;
                    if (path==0 && equal(e,"EFI        ",11)) child=1;
                    if (path==1 && equal(e,"BOOT       ",11)) child=2;
                    const char *error=directory(s,c,depth+1,child);
                    if (error) return error;
                } else {
                    if (mark_chain(s,c,size,false)) return "invalid/cross-linked ESP file";
                    if (path==2 && equal(e,"BOOTX64 EFI",11) && size) s->loader_found=true;
                    if (path==2 && equal(e,"KERNEL  ELF",11)) {
                        if (s->kernel_found || (e[11]&1) || !size || size > BOOT_KERNEL_MAX)
                            return "invalid/duplicate/read-only EFI kernel";
                        s->kernel_found=true; memcpy(s->entry,e,32);
                        s->entry_offset=off+(uint64_t)sector*s->bps+pos;
                    }
                }
            }
        }
        if (fixed) break;
        cluster=next(s,cluster);
    } while (valid(s,cluster));
    return NULL;
}

const char *boot_kernel_open(const boot_kernel_io_t *io, uint64_t data_offset, boot_kernel_t **out)
{
    *out=NULL;
    if (!io || !io->read || !io->write || !io->flush) return "missing boot disk I/O";
    boot_kernel_t *s=kmalloc(sizeof(*s));
    if (!s) return "out of memory (boot disk)";
    memset(s,0,sizeof(*s)); s->io=*io;
    const char *error=partition(s,data_offset);
    if (error) goto fail;
    uint8_t b[4096];
    error="invalid FAT boot sector";
    if (read_at(s,0,b,512) || b[510]!=0x55 || b[511]!=0xaa) goto fail;
    s->bps=u16(b+11); s->spc=b[13]; s->reserved=u16(b+14); s->fats=b[16];
    s->root_entries=u16(b+17);
    uint32_t total=u16(b+19); if (!total) total=u32(b+32);
    s->fat_sectors=u16(b+22); if (!s->fat_sectors) s->fat_sectors=u32(b+36);
    if (s->bps != io->sector_size || !s->spc || s->spc>128 || (s->spc&(s->spc-1)) ||
        !s->reserved || !s->fats || s->fats>2 || !s->fat_sectors ||
        (uint64_t)total*s->bps > s->bytes || (uint64_t)s->spc*s->bps > 65536 ||
        (uint64_t)s->fat_sectors*s->bps > MAX_FAT_BYTES) goto fail;
    uint64_t data=(uint64_t)s->reserved+s->fats*(uint64_t)s->fat_sectors+(s->root_entries*32+s->bps-1)/s->bps;
    if (data >= total) goto fail;
    s->data_sector=data; s->clusters=(total-data)/s->spc;
    if (s->clusters < 4085 || s->clusters >= 0x0ffffff0) { error="FAT12/oversized ESP not supported"; goto fail; }
    s->width=s->clusters < 65525 ? 2 : 4;
    s->fat_bytes=s->fat_sectors*s->bps;
    if ((uint64_t)(s->clusters+2)*s->width > s->fat_bytes) goto fail;
    if (s->width==4) {
        /* Only mirrored, version-zero FAT32 volumes are modified. */
        if (s->root_entries || u16(b+22) || u16(b+42) || u16(b+40)) goto fail;
        s->root_cluster=u32(b+44);
        if (!valid(s,s->root_cluster)) goto fail;
        uint32_t info=u16(b+48), backup=u16(b+50);
        if (info && info!=0xffff) {
            if (info >= s->reserved) goto fail;
            s->fsinfo[0]=info;
            if (backup && backup!=0xffff) {
                if (backup >= s->reserved || info+backup >= s->reserved) goto fail;
                s->fsinfo[1]=info+backup;
            }
        }
    } else {
        if (!u16(b+22) || !s->root_entries || (s->root_entries*32)%s->bps) goto fail;
        s->root_sector=s->reserved+s->fats*s->fat_sectors;
    }
    s->fat=kmalloc(s->fat_bytes); s->original=kmalloc(s->fat_bytes);
    s->seen=kmalloc((s->clusters+9)/8); s->scratch=kmalloc(IO_BYTES);
    error="out of memory (FAT)";
    if (!s->fat || !s->original || !s->seen || !s->scratch) goto fail;
    error="FAT read or mirror mismatch";
    if (read_at(s,(uint64_t)s->reserved*s->bps,s->fat,s->fat_bytes)) goto fail;
    for (unsigned copy=1;copy<s->fats;copy++) {
        if (read_at(s,((uint64_t)s->reserved+copy*s->fat_sectors)*s->bps,s->original,s->fat_bytes) ||
            !equal(s->fat,s->original,s->fat_bytes)) goto fail;
    }
    memcpy(s->original,s->fat,s->fat_bytes); memset(s->seen,0,(s->clusters+9)/8);
    error=directory(s,s->root_cluster,0,0);
    if (error) goto fail;
    if (!s->kernel_found || !s->loader_found) { error="EFI/BOOT Osito-K loader/kernel not found"; goto fail; }
    *out=s; return NULL;
fail:
    boot_kernel_close(s); return error;
}
uint32_t boot_kernel_size(const boot_kernel_t *s) { return u32(s->entry+28); }
const char *boot_kernel_read(boot_kernel_t *s, void *buffer)
{
    uint8_t *p=buffer; uint32_t left=boot_kernel_size(s), c=first_cluster(s,s->entry);
    while (left) {
        if (!valid(s,c)) return "invalid EFI kernel chain";
        uint32_t cb=s->bps*s->spc, run=1;
        while (run<IO_BYTES/cb && (uint64_t)run*cb<left &&
               valid(s,c+run) && next(s,c+run-1)==c+run) run++;
        uint32_t n=run*cb; if (n>left) n=left;
        if (read_at(s,cluster_offset(s,c),p,n)) return "EFI kernel read failed";
        p+=n; left-=n; c=next(s,c+run-1);
    }
    return NULL;
}
static int verify_at(boot_kernel_t *s, uint64_t off, const void *p, uint32_t n)
{
    const uint8_t *b=p;
    while (n) {
        uint32_t part=n>IO_BYTES ? IO_BYTES : n;
        if (read_at(s,off,s->scratch,part) || !equal(s->scratch,b,part)) return -1;
        off+=part; b+=part; n-=part;
    }
    return 0;
}
static int fat_write(boot_kernel_t *s, const uint8_t *image)
{
    for (unsigned copy=0;copy<s->fats;copy++) {
        uint64_t base=((uint64_t)s->reserved+copy*s->fat_sectors)*s->bps;
        for (uint32_t off=0;off<s->fat_bytes;off+=s->bps) {
            if (equal(s->fat+off,s->original+off,s->bps)) continue;
            if (write_at(s,base+off,image+off,s->bps)) return -1;
        }
    }
    if (s->io.flush(s->io.context)) return -1;
    for (unsigned copy=0;copy<s->fats;copy++) {
        uint64_t base=((uint64_t)s->reserved+copy*s->fat_sectors)*s->bps;
        for (uint32_t off=0;off<s->fat_bytes;off+=s->bps) {
            if (equal(s->fat+off,s->original+off,s->bps)) continue;
            if (verify_at(s,base+off,image+off,s->bps)) return -1;
        }
    }
    return 0;
}
static int invalidate_free_count(boot_kernel_t *s)
{
    uint8_t block[4096];
    for (unsigned i=0;i<2;i++) if (s->fsinfo[i]) {
        uint64_t off=(uint64_t)s->fsinfo[i]*s->bps;
        if (read_at(s,off,block,s->bps)) return -1;
        /* Invalid FSInfo is ignored by FAT readers; some formatters omit the
         * backup entirely. Never turn an absent structure into valid metadata. */
        if (u32(block)!=0x41615252 || u32(block+484)!=0x61417272 ||
            u32(block+508)!=0xaa550000) continue;
        p32(block+488,UINT32_MAX); p32(block+492,UINT32_MAX);
        if (write_at(s,off,block,s->bps) || s->io.flush(s->io.context) ||
            verify_at(s,off,block,s->bps)) return -1;
    }
    return 0;
}
static uint32_t contiguous_run(const uint32_t *chain, uint32_t count, uint32_t start, uint32_t cb)
{
    uint32_t run=1;
    while (start+run<count && run<IO_BYTES/cb && chain[start+run]==chain[start]+run) run++;
    return run;
}
const char *boot_kernel_replace(boot_kernel_t *s, const void *image, uint32_t size)
{
    if (!image || !size || size>BOOT_KERNEL_MAX) return "invalid kernel size";
    uint32_t cb=s->bps*s->spc;
    if (!cb || cb>65536) return "invalid cluster size";
    uint32_t count=1+(size-1)/cb;
    uint32_t *chain=kmalloc((uint64_t)count*sizeof(*chain));
    if (!chain) return "out of memory (new kernel chain)";
    const char *error="ESP has insufficient free space";
    uint32_t found=0;
    for (uint32_t c=2;c<s->clusters+2 && found<count;c++) if (!next(s,c)) chain[found++]=c;
    if (found!=count) goto done;
    for (uint32_t i=0;i<count;i++) set_next(s,chain[i],i+1<count ? chain[i+1] : 0x0fffffff);
    const uint8_t *data=image;
    for (uint32_t i=0, off=0;i<count;) {
        uint32_t run=contiguous_run(chain,count,i,cb), span=run*cb;
        uint32_t n=size-off; if (n>span) n=span;
        memset(s->scratch,0,span); memcpy(s->scratch,data+off,n);
        error="new EFI kernel write failed (old entry retained)";
        if (write_at(s,cluster_offset(s,chain[i]),s->scratch,span)) goto done;
        off+=n; i+=run;
    }
    error="new EFI kernel flush/readback failed (old entry retained)";
    if (s->io.flush(s->io.context)) goto done;
    for (uint32_t i=0,off=0;i<count;) {
        uint32_t run=contiguous_run(chain,count,i,cb), span=run*cb;
        uint32_t n=size-off; if (n>span) n=span;
        if (verify_at(s,cluster_offset(s,chain[i]),data+off,n)) goto done;
        off+=n; i+=run;
    }
    error="FAT32 FSInfo update failed (old entry retained)";
    if (invalidate_free_count(s)) goto done;
    if (fat_write(s,s->fat)) {
        error=fat_write(s,s->original) ? "FAT update/rollback failed; repair ESP before retry" : "FAT update failed, rolled back";
        goto done;
    }
    uint8_t before[4096], after[4096];
    uint64_t dir=s->entry_offset/s->bps*s->bps;
    unsigned pos=s->entry_offset%s->bps;
    if (read_at(s,dir,before,s->bps) || !equal(before+pos,s->entry,32)) {
        error=fat_write(s,s->original) ? "directory changed; FAT rollback failed" : "directory changed; update cancelled";
        goto done;
    }
    memcpy(after,before,s->bps);
    p16(after+pos+26,chain[0]);
    if (s->width==4) p16(after+pos+20,chain[0]>>16);
    p32(after+pos+28,size); after[pos+11]|=0x20;
    if (write_at(s,dir,after,s->bps) || s->io.flush(s->io.context) || verify_at(s,dir,after,s->bps)) {
        /* A failed write may still have reached the device. Restore/verify the
         * directory before releasing any newly published clusters. */
        error="EFI publication uncertain; both chains retained, no hotreload";
        if (!write_at(s,dir,before,s->bps) && !s->io.flush(s->io.context) && !verify_at(s,dir,before,s->bps))
            error=fat_write(s,s->original) ? "EFI entry restored; FAT rollback failed" : "EFI publication failed, rolled back";
        goto done;
    }
    memcpy(s->original,s->fat,s->fat_bytes);
    uint32_t c=first_cluster(s,s->entry);
    while (valid(s,c)) { uint32_t following=next(s,c); set_next(s,c,0); c=following; }
    memcpy(s->entry,after+pos,32);
    error=fat_write(s,s->fat) ? "EFI kernel installed; old-chain cleanup failed, no hotreload" : NULL;
    if (!error) memcpy(s->original,s->fat,s->fat_bytes);
done:
    kfree(chain); return error;
}
void boot_kernel_close(boot_kernel_t *s)
{
    if (!s) return;
    kfree(s->fat); kfree(s->original); kfree(s->seen); kfree(s->scratch); kfree(s);
}
