/* Production primary-scanout selection. PCI responses are explicit fixtures;
 * no real PCI, MMIO, display initialization or rendering is performed. */
#include "../kernel/display.c"
extern int printf(const char *, ...);
extern void abort(void);
#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL identity %d: %s\n", __LINE__, #x); abort(); } } while (0)
static unsigned checks, reads, intel_names;
static bool virtio_ready, virtio_vga, missing_gpu, missing_intel;
static unsigned fail_reg, stale_id, class_override;
static pci_dev_t virtio = {.bus=2,.dev=3,.vendor_id=0x1af4,.device_id=0x1050};
static gpu_device_t nvidia = {.pci_bus=1,.pci_dev=1,.vendor_id=0x10de,.device_id=0x1f91};
static intel_gfx_state_t intel = {.bus=0,.dev=2,.vendor_id=0x8086,.device_id=0x3e9b};

bool virtio_gpu_ready(void) { return virtio_ready; }
bool virtio_gpu_is_vga_compatible(void) { return virtio_vga; }
pci_dev_t *pci_get_virtio_gpu(void) { return missing_gpu ? NULL : &virtio; }
gpu_device_t *pci_get_gpu(void) { return missing_gpu ? NULL : &nvidia; }
const intel_gfx_state_t *intel_gfx_get_state(void) { return missing_intel ? NULL : &intel; }
const char *intel_gfx_device_name(uint16_t id)
{ CHECK(id==0x3e9b); intel_names++; return "Intel fixture"; }
bool pci_diag_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg, uint32_t *out)
{
    CHECK(!func && (reg==0 || reg==8)); reads++;
    if (reg==fail_reg) return false;
    CHECK((bus==0 && dev==2) || (bus==1 && dev==1) || (bus==2 && dev==3));
    uint32_t id = bus==0 ? 0x3e9b8086 : bus==1 ? 0x1f9110de : 0x10501af4;
    *out = reg==0 ? id ^ stale_id : class_override;
    return true;
}

int main(void)
{
    display_identity_t out;
    fail_reg=UINT32_MAX; class_override=0x030000a1;
    display_get_primary_identity(NULL);
    CHECK(!reads);
    /* Mere device presence, ready render engines or a secondary virtio
     * surface must never turn into the primary desktop's PCI identity. */
    for (unsigned mask=0;mask<64;mask++) {
        disp.gpu_scanout=(mask&1)!=0;
        virtio_ready=(mask&2)!=0; virtio_vga=(mask&4)!=0;
        intel.ready=(mask&8)!=0; intel.gop_scanout_retained=(mask&16)!=0;
        disp.virtio_scanout=(mask&32)!=0;
        reads=0;
        display_get_primary_identity(&out);
        if (mask&1) {
            CHECK(out.vendor==0x10de && out.device==0x1f91);
            CHECK(!strcmp(out.description,"NVIDIA Display"));
        } else if ((mask&6)==6) {
            CHECK(out.vendor==0x1af4 && out.device==0x1050);
            CHECK(!strcmp(out.description,"VirtIO GPU Display"));
        } else if ((mask&24)==24) {
            CHECK(out.vendor==0x8086 && out.device==0x3e9b);
            CHECK(!strcmp(out.description,"Intel fixture"));
        } else {
            CHECK(!out.vendor && !out.device && !out.vga_compatible && !reads);
            CHECK(!strcmp(out.description,"UEFI GOP Display"));
        }
        CHECK(out.vendor ? reads==2 && out.vga_compatible : reads==0);
    }
    /* Stop exposing stale PCI values when config access fails, a device is
     * replaced, the provider disappears, or the endpoint is not a display. */
    for (unsigned owner=0;owner<3;owner++) for (unsigned bad=0;bad<6;bad++) {
        disp.gpu_scanout=owner==0; virtio_ready=virtio_vga=owner==1;
        intel.ready=intel.gop_scanout_retained=true;
        fail_reg=bad==0 ? 0 : bad==1 ? 8 : UINT32_MAX;
        stale_id=bad==2 ? 1 : 0;
        class_override=bad==3 ? 0x02000000 : 0x03020000;
        missing_gpu=missing_intel=bad==4;
        memset(&out,0x5a,sizeof(out));
        display_get_primary_identity(&out);
        CHECK(!out.vga_compatible);
        if (bad<5) CHECK(!out.vendor && !out.device);
        else CHECK(out.vendor && out.device);
        CHECK(out.description && out.description[0]);
    }
    CHECK(intel_names>0);
    printf("Display identity: %u checks, 0 failures\n",checks);
    return 0;
}
