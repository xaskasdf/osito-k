#include "../include/types.h"
#define OSITO_RANDOM_SECURE_H
int random_secure_bytes(void *, size_t);
#include "../kernel/kernel_update.c"
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
static unsigned checks, failures, allocations, boots, publications, creates, flush_calls, renames;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static struct file { char name[64]; uint8_t data[1024]; unsigned size, refs, revision; } files[12];
static uint8_t old_elf[512], next_elf[513], boot_file[1024];
static unsigned boot_size, fail_write, fail_flush, fail_rename, writes, read_mode;
static bool usb=true, esp_failure, chunked, te, short_body;
static int http_status;
static const char *http_length;
static char output[8192], requested_path[256];

void *kmalloc(uint64_t n) { void *p=malloc(n); if(p) allocations++; return p; }
void kfree(void *p) { if(p) { allocations--; free(p); } }
void sh_puts(const char *s) { unsigned n=strlen(output); while(*s && n+1<sizeof(output)) output[n++]=*s++; output[n]=0; }
bool osfs2_is_mounted(void) { return true; }
uint64_t osfs2_partition_start(void) { return 129*1024*1024; }
void *osfs2_find(const char *path) { for(unsigned i=0;i<12;i++) if(files[i].name[0] && !strcmp(files[i].name,path)) return &files[i]; return NULL; }
void *osfs2_create(const char *name, uint64_t size)
{
    CHECK(size<=1024); creates++;
    for(unsigned i=0;i<12;i++) if(!files[i].name[0]) {
        struct file *f=&files[i]; strcpy(f->name,name); f->size=size; memset(f->data,0,size); return f;
    }
    return NULL;
}
int osfs2_read(void *file, uint64_t off, void *buf, uint64_t n)
{
    struct file *f=file; CHECK(off+n<=f->size); memcpy(buf,f->data+off,n);
    if(read_mode==1 && !strcmp(f->name,"/next.elf")) return n-1;
    if(read_mode==2 && f->name[1]=='.') ((uint8_t *)buf)[0]^=1;
    if(read_mode==3 && !strcmp(f->name,"/next.elf")) f->revision+=2;
    return n;
}
int osfs2_write(void *file, uint64_t off, const void *buf, uint64_t n)
{
    struct file *f=file; writes++;
    if(writes==fail_write) return -1;
    CHECK(off+n<=f->size); memcpy(f->data+off,buf,n); f->revision+=2; return 0;
}
uint64_t osfs2_file_size(void *file) { return ((struct file *)file)->size; }
uint64_t osfs2_file_revision(void *file) { return ((struct file *)file)->revision; }
int osfs2_file_retain(void *file) { ((struct file *)file)->refs++; return 0; }
void osfs2_file_release(void *file) { struct file *f=file; CHECK(f->refs>0); f->refs--; }
int osfs2_rename(const char *from, const char *to, bool replace)
{
    renames++; if(renames==fail_rename) return -1;
    struct file *f=osfs2_find(from), *old=osfs2_find(to); CHECK(f && replace);
    if(old) { CHECK(!old->refs); old->name[0]=0; }
    strcpy(f->name,to); return 0;
}
int osfs2_delete(const char *name) { struct file *f=osfs2_find(name); if(f) { CHECK(!f->refs); f->name[0]=0; } return 0; }
int disk_active(void) { return 0; }
bool disk_is_usb(void) { return usb; }
bool blkdev_can_write(int d) { return d==0; }
const char *blkdev_name(int d) { CHECK(d==0); return "usb0"; }
uint32_t disk_lba_size(void) { return 512; }
uint64_t disk_lba_count(void) { return 1000000; }
int disk_read_bytes(uint64_t o, void *b, uint64_t n) { (void)o; (void)b; (void)n; return 0; }
int disk_write_bytes(uint64_t o, const void *b, uint64_t n) { (void)o; (void)b; (void)n; return 0; }
int disk_flush(void) { return ++flush_calls==fail_flush ? -1 : 0; }
int random_secure_bytes(void *p, size_t n) { static uint8_t nonce; memset(p,++nonce,n); return 0; }
void kexec_command(const char *path) { CHECK(!strcmp(path,"/kernel.elf")); boots++; }
int http_open(http_session_t *s, const char *host) { (void)s; CHECK(host && *host); return 0; }
int http_request(http_session_t *s, const char *method, const char *path, const char *host,
                 const char *const *headers, const void *body, uint32_t len, http_response_t *r)
{
    (void)s; (void)host; (void)headers; (void)body; (void)len;
    CHECK(!strcmp(method,"GET")); strcpy(requested_path,path);
    memset(r,0,sizeof(*r)); r->status_code=http_status; r->content_length=sizeof(next_elf); r->chunked=chunked;
    return 0;
}
const char *http_get_header(const http_response_t *r, const char *name)
{ (void)r; return !strcmp(name,"Content-Length") ? http_length : te ? "identity" : NULL; }
int http_read_body_full(http_session_t *s, const http_response_t *r, void *buf, uint32_t cap)
{ (void)s; (void)r; CHECK(cap==sizeof(next_elf)); memcpy(buf,next_elf,cap); return short_body?(int)cap-1:(int)cap; }
void http_close(http_session_t *s) { (void)s; }
const char *boot_kernel_open(const boot_kernel_io_t *io, uint64_t data, boot_kernel_t **out)
{ CHECK(io->sector_size==512 && data==129*1024*1024); *out=(boot_kernel_t *)boot_file; return NULL; }
uint32_t boot_kernel_size(const boot_kernel_t *s) { CHECK(s==(void *)boot_file); return boot_size; }
const char *boot_kernel_read(boot_kernel_t *s, void *p) { CHECK(s==(void *)boot_file); memcpy(p,boot_file,boot_size); return NULL; }
const char *boot_kernel_replace(boot_kernel_t *s, const void *p, uint32_t n)
{ CHECK(s==(void *)boot_file); publications++; if(esp_failure) return "injected ESP failure"; memcpy(boot_file,p,n); boot_size=n; return NULL; }
void boot_kernel_close(boot_kernel_t *s) { (void)s; }

static void reset(void)
{
    CHECK(!allocations); memset(files,0,sizeof(files)); memset(old_elf,0,sizeof(old_elf));
    kexec_elf_header_t *h=(void *)old_elf; kexec_elf_program_t *p=(void *)(old_elf+sizeof(*h));
    memcpy(h->ident,"\177ELF\2\1\1",7); h->type=2; h->machine=62; h->version=1;
    h->ehsize=sizeof(*h); h->phentsize=sizeof(*p); h->phoff=sizeof(*h); h->phnum=1;
    h->entry=KEXEC_DIRECT_BASE+0x2000000;
    *p=(kexec_elf_program_t){.type=1,.flags=5,.offset=256,.vaddr=h->entry,.paddr=0x2000000,.filesz=128,.memsz=4096,.align=1};
    memcpy(next_elf,old_elf,sizeof(old_elf)); next_elf[512]=123;
    memcpy(boot_file,old_elf,sizeof(old_elf)); boot_size=sizeof(old_elf);
    struct file *f=osfs2_create("/next.elf",sizeof(next_elf)); memcpy(f->data,next_elf,sizeof(next_elf));
    f=osfs2_create("/kernel.elf",sizeof(old_elf)); memcpy(f->data,old_elf,sizeof(old_elf));
    creates=boots=publications=flush_calls=renames=writes=read_mode=0;
    fail_write=fail_flush=fail_rename=0; output[0]=requested_path[0]=0;
    usb=true; esp_failure=chunked=te=short_body=false;
    http_status=200; http_length="513";
}
static void run(bool noexec)
{
    char *args[]={"kupdate","--file","/next.elf","--no-kexec"};
    kernel_update_command(noexec?4:3,args);
    CHECK(!allocations); for(unsigned i=0;i<12;i++) CHECK(!files[i].refs);
}
static void cancelled(void) { CHECK(!boots && !publications); CHECK(boot_size==sizeof(old_elf)); }
int main(void)
{
    reset(); run(true); CHECK(publications==1 && !boots); CHECK(same(boot_file,next_elf,sizeof(next_elf)));
    struct file *backup=osfs2_find("/kernel.previous.elf"); CHECK(backup && same(backup->data,old_elf,sizeof(old_elf)));
    run(false); CHECK(boots==1 && publications==1); CHECK(same(backup->data,old_elf,sizeof(old_elf)));
    reset(); run(false); CHECK(boots==1 && publications==1);
    reset(); char *status[]={"kupdate","--status"}; kernel_update_command(2,status); CHECK(!creates && !writes && !boots && !allocations);
    reset(); usb=false; run(false); cancelled(); CHECK(!creates);
    reset(); files[0].data[0]=0; run(false); cancelled(); CHECK(!creates);
    reset(); files[0].size=1; run(false); cancelled(); CHECK(!creates);
    for(unsigned m=1;m<=3;m++) { reset(); read_mode=m; run(false); cancelled(); }
    for(unsigned n=1;n<=2;n++) { reset(); fail_write=n; run(false); cancelled(); }
    for(unsigned n=1;n<=2;n++) { reset(); fail_rename=n; run(false); cancelled(); }
    for(unsigned n=1;n<=4;n++) { reset(); fail_flush=n; run(false); cancelled(); }
    reset(); esp_failure=true; run(false); CHECK(!boots && publications==1 && boot_size==sizeof(old_elf));
    char *http[]={"kupdate","--no-kexec"};
    const char *bad_lengths[]={NULL,"","0","1","513x","-1","4294967809","33554433"};
    for(unsigned i=0;i<sizeof(bad_lengths)/sizeof(*bad_lengths);i++) {
        reset(); http_length=bad_lengths[i]; kernel_update_command(2,http); cancelled(); CHECK(!allocations);
    }
    reset(); http_status=404; kernel_update_command(2,http); cancelled();
    reset(); chunked=true; kernel_update_command(2,http); cancelled();
    reset(); te=true; kernel_update_command(2,http); cancelled();
    reset(); short_body=true; kernel_update_command(2,http); cancelled();
    reset(); kernel_update_command(2,http); CHECK(publications==1 && !boots);
    reset(); char *channel[]={"kupdate","--channel","testing","--no-kexec"}; kernel_update_command(4,channel);
    CHECK(!strcmp(requested_path,"/k/x86_64/testing/kernel.elf") && publications==1);
    reset(); channel[2]="../../wrong"; kernel_update_command(4,channel); cancelled();
    reset(); char *bad[]={"kupdate","--file","/next.elf","--unknown"}; kernel_update_command(4,bad); cancelled();
    reset(); bad[3]="host"; kernel_update_command(4,bad); cancelled();
    CHECK(!allocations);
    printf("kernel update: %u checks, %u failures\n",checks,failures); return failures?1:0;
}
