#include "kernel_update.h"
#include "kexec.h"
#include "kexec_image.h"
#include "http.h"
#include "random_secure.h"
#include "../fs/boot_kernel.h"

extern void *kmalloc(uint64_t);
extern void kfree(void *);
extern void sh_puts(const char *);
extern bool osfs2_is_mounted(void);
extern uint64_t osfs2_partition_start(void);
extern void *osfs2_find(const char *);
extern void *osfs2_create(const char *, uint64_t);
extern int osfs2_read(void *, uint64_t, void *, uint64_t);
extern int osfs2_write(void *, uint64_t, const void *, uint64_t);
extern uint64_t osfs2_file_size(void *);
extern uint64_t osfs2_file_revision(void *);
extern int osfs2_file_retain(void *);
extern void osfs2_file_release(void *);
extern int osfs2_rename(const char *, const char *, bool);
extern int osfs2_delete(const char *);
extern int disk_active(void);
extern bool disk_is_usb(void);
extern bool blkdev_can_write(int);
extern const char *blkdev_name(int);
extern uint32_t disk_lba_size(void);
extern uint64_t disk_lba_count(void);
extern int disk_read_bytes(uint64_t, void *, uint64_t);
extern int disk_write_bytes(uint64_t, const void *, uint64_t);
extern int disk_flush(void);

static bool same(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x=a, *y=b;
    while (n--) if (*x++!=*y++) return false;
    return true;
}
static void usage(void)
{
    sh_puts("Usage: kupdate [host] [/path] [--channel name] [--no-kexec]\n"
            "       kupdate --file /kernel.elf [--no-kexec]\n"
            "       kupdate --status\n"
            "Updates OsitoFS and EFI/BOOT/kernel.elf on the same USB.\n");
}
static int pinned_read(void *p, uint64_t off, void *buf, uint64_t n)
{ return disk_active()==*(int *)p ? disk_read_bytes(off,buf,n) : -1; }
static int pinned_write(void *p, uint64_t off, const void *buf, uint64_t n)
{ return disk_active()==*(int *)p ? disk_write_bytes(off,buf,n) : -1; }
static int pinned_flush(void *p)
{ return disk_active()==*(int *)p ? disk_flush() : -1; }

static const char *read_local(const char *path, void **data, uint32_t *size)
{
    void *file=osfs2_find(path);
    if (!file || osfs2_file_retain(file)<0) return "local kernel not found/available";
    uint64_t bytes=osfs2_file_size(file), revision=osfs2_file_revision(file);
    const char *error="invalid local kernel size or concurrent write";
    if (!bytes || bytes>BOOT_KERNEL_MAX || (revision&1)) goto done;
    *data=kmalloc(bytes);
    if (!*data) { error="out of memory (local kernel)"; goto done; }
    error="local kernel read failed/changed";
    if (osfs2_read(file,0,*data,bytes)!=(int)bytes || osfs2_file_revision(file)!=revision) goto done;
    *size=bytes; error=NULL;
done:
    osfs2_file_release(file); return error;
}
static const char *download(const char *host, const char *path, void **data, uint32_t *size)
{
    http_session_t *session=kmalloc(sizeof(*session));
    http_response_t *response=kmalloc(sizeof(*response));
    const char *error="out of memory (HTTPS)";
    if (!session || !response) goto done;
    memset(session,0,sizeof(*session));
    sh_puts("kupdate: GET https://"); sh_puts(host); sh_puts(path); sh_puts("\n");
    error="HTTPS connection failed";
    if (http_open(session,host)<0) goto done;
    const char *headers[]={"Connection: close","User-Agent: OsitoK/1.0 (kupdate)",NULL};
    error="HTTP request failed (requires status 200)";
    if (http_request(session,"GET",path,host,headers,NULL,0,response)<0 || response->status_code!=200) goto close;
    const char *length=http_get_header(response,"Content-Length");
    error="invalid/missing Content-Length or unsupported transfer encoding";
    if (!length || !*length || response->chunked || http_get_header(response,"Transfer-Encoding")) goto close;
    uint32_t n=0;
    for (const char *p=length;*p;p++) {
        if (*p<'0' || *p>'9' || n>(BOOT_KERNEL_MAX-(unsigned)(*p-'0'))/10) goto close;
        n=n*10+(unsigned)(*p-'0');
    }
    if (!n || n!=response->content_length) goto close;
    *data=kmalloc(n);
    if (!*data) { error="out of memory (download)"; goto close; }
    error="kernel download incomplete";
    if (http_read_body_full(session,response,*data,n)!=(int)n) goto close;
    *size=n; error=NULL;
close:
    http_close(session);
done:
    kfree(response); kfree(session); return error;
}

/* Publish only after a separate file is fully written, flushed and read back.
 * Shell mutations are excluded by the exclusive remote-command lock. */
static const char *save_verified(const char *path, const void *data, uint32_t size)
{
    uint8_t random[8], block[4096];
    char temporary[40]="/.kupdate-";
    if (random_secure_bytes(random,sizeof(random))) return "cannot generate staging name";
    static const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<8;i++) { temporary[10+2*i]=hex[random[i]>>4]; temporary[11+2*i]=hex[random[i]&15]; }
    memcpy(temporary+26,".part",6);
    if (osfs2_find(temporary)) return "staging name collision";
    void *f=osfs2_create(temporary,size);
    if (!f) return "cannot create staged kernel";
    if (osfs2_file_retain(f)<0) { osfs2_delete(temporary); return "cannot retain staged kernel"; }
    const char *error="staged kernel write/flush failed";
    if (osfs2_write(f,0,data,size)<0 || disk_flush()) goto done;
    for (uint32_t off=0;off<size;) {
        uint32_t n=size-off; if (n>sizeof(block)) n=sizeof(block);
        error="staged kernel readback mismatch";
        if (osfs2_read(f,off,block,n)!=(int)n || !same(block,(const uint8_t *)data+off,n)) goto done;
        off+=n;
    }
    error="staged kernel rename failed";
    if (osfs2_rename(temporary,path,true)<0) goto done;
    osfs2_file_release(f);
    if (disk_flush()) return "OsitoFS publication flush failed; no hotreload";
    return NULL;
done:
    osfs2_file_release(f); osfs2_delete(temporary); return error;
}

void kernel_update_command(int argc, char *argv[])
{
    const char *host="wasm.naranjositos.tech", *path="/k/x86_64/stable/kernel.elf", *file=NULL;
    char channel_path[128];
    bool reboot=true, host_set=false, path_set=false, status=false;
    for (int i=1;i<argc;i++) {
        const char *a=argv[i];
        if (!strcmp(a,"--no-kexec")) reboot=false;
        else if (!strcmp(a,"--status")) status=true;
        else if (!strcmp(a,"--file") && i+1<argc && !file) file=argv[++i];
        else if (!strcmp(a,"--channel") && i+1<argc && !path_set) {
            a=argv[++i]; unsigned n=0;
            while (a[n]) {
                char c=a[n++];
                if (n>64 || !((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                             (c>='0' && c<='9') || c=='-' || c=='_')) { usage(); return; }
            }
            if (!n) { usage(); return; }
            static const char prefix[]="/k/x86_64/", suffix[]="/kernel.elf";
            memcpy(channel_path,prefix,sizeof(prefix)-1);
            memcpy(channel_path+sizeof(prefix)-1,a,n);
            memcpy(channel_path+sizeof(prefix)-1+n,suffix,sizeof(suffix));
            path=channel_path; path_set=true;
        } else if (a[0]=='/' && !path_set) { path=a; path_set=true; }
        else if (a[0] && a[0]!='-' && !host_set) { host=a; host_set=true; }
        else { usage(); return; }
    }
    if ((file && (host_set || path_set || !*file || file[0]=='-')) ||
        (status && argc!=2)) { usage(); return; }
    const char *error=NULL;
    void *data=NULL, *old=NULL;
    boot_kernel_t *boot=NULL;
    uint32_t size=0;
    int device=disk_active();
    if (!osfs2_is_mounted() || !disk_is_usb() || !blkdev_can_write(device)) {
        sh_puts("kupdate: requires writable OsitoFS on USB; no other disk will be modified\n"); return;
    }
    boot_kernel_io_t io={&device,disk_lba_size(),disk_lba_count(),pinned_read,pinned_write,pinned_flush};
    error=boot_kernel_open(&io,osfs2_partition_start(),&boot);
    if (error) goto done;
    sh_puts("kupdate: boot target "); sh_puts(blkdev_name(device)); sh_puts(" EFI/BOOT/kernel.elf\n");
    uint32_t old_size=boot_kernel_size(boot);
    old=kmalloc(old_size);
    if (!old) { error="out of memory (previous EFI kernel)"; goto done; }
    error=boot_kernel_read(boot,old);
    if (error) goto done;
    kexec_image_t plan;
    error=kexec_parse_image(old,old_size,&plan);
    if (error) goto done;
    if (status) { sh_puts("kupdate: existing EFI kernel valid; no writes performed\n"); goto done; }
    error=file ? read_local(file,&data,&size) : download(host,path,&data,&size);
    if (error) goto done;
    error=kexec_parse_image(data,size,&plan);
    if (error) goto done;
    bool unchanged=old_size==size && same(old,data,size);
    if (!unchanged) {
        sh_puts("kupdate: saving previous EFI kernel -> /kernel.previous.elf\n");
        error=save_verified("/kernel.previous.elf",old,old_size);
        if (error) goto done;
    }
    error=save_verified("/kernel.elf",data,size);
    if (error) goto done;
    sh_puts("kupdate: OsitoFS kernel verified\n");
    if (!unchanged) {
        sh_puts("kupdate: writing and verifying EFI kernel...\n");
        error=boot_kernel_replace(boot,data,size);
        if (error) goto done;
    }
    sh_puts(unchanged ? "kupdate: EFI kernel already current\n" : "kupdate: EFI kernel verified; cold boot updated\n");
done:
    boot_kernel_close(boot); kfree(old); kfree(data);
    if (error) { sh_puts("kupdate: "); sh_puts(error); sh_puts("; kexec cancelled\n"); }
    else if (!status && reboot) kexec_command("/kernel.elf");
    else if (!status) sh_puts("kupdate: --no-kexec; persistent update complete\n");
}
