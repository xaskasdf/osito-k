/*
 * OsitoK x86-64 — USB Mass Storage Driver (Bulk-Only Transport)
 *
 * Reads sectors from USB flash drives via xHCI bulk transfers.
 * Implements BBB (Bulk-Only Transport) protocol:
 *   CBW (Command Block Wrapper) → Data → CSW (Command Status Wrapper)
 * SCSI commands: READ(10), INQUIRY, READ CAPACITY.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* xHCI bulk transfer (from xhci.c) */
extern int xhci_bulk_out(int dev_idx, const void *data, uint32_t len)
    __attribute__((weak));
extern int xhci_bulk_in(int dev_idx, void *data, uint32_t len, uint32_t *actual)
    __attribute__((weak));

/* ── USB Mass Storage BBB Protocol ───────────────────────────── */

#define CBW_SIGNATURE   0x43425355  /* "USBC" */
#define CSW_SIGNATURE   0x53425355  /* "USBS" */

typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;        /* 0x80 = Data-In (device → host) */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;      /* SCSI command length (6-16) */
    uint8_t  CBWCB[16];         /* SCSI CDB */
} cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;        /* 0=OK, 1=Failed, 2=Phase Error */
} csw_t;

/* SCSI Commands */
#define SCSI_INQUIRY              0x12
#define SCSI_READ_CAPACITY        0x25
#define SCSI_READ_10              0x28
#define SCSI_WRITE_10             0x2A
#define SCSI_SYNCHRONIZE_CACHE_10 0x35
#define USB_WRITE_DIAG_RECENT     8

/* ── Driver State ────────────────────────────────────────────── */

static struct {
    bool     ready;
    int      dev_idx;        /* xHCI device index */
    uint32_t block_count;    /* Total LBAs */
    uint32_t block_size;     /* Bytes per block (usually 512) */
    uint32_t cbw_tag;        /* Incrementing tag for CBW/CSW matching */
    char     vendor[9];
    char     product[17];
} usb_disk;

typedef struct {
    uint64_t lba;
    uint32_t count;
} usb_write_diag_entry_t;

static struct {
    uint64_t ops;
    uint64_t sectors;
    uint64_t first_lba;
    uint64_t last_lba;
    uint32_t first_count;
    uint32_t last_count;
    uint32_t recent_pos;
    uint32_t recent_count;
    usb_write_diag_entry_t recent[USB_WRITE_DIAG_RECENT];
} usb_write_diag;

static void usb_write_diag_record(uint64_t lba, uint32_t count)
{
    if (usb_write_diag.ops == 0) {
        usb_write_diag.first_lba = lba;
        usb_write_diag.first_count = count;
    }
    usb_write_diag.ops++;
    usb_write_diag.sectors += count;
    usb_write_diag.last_lba = lba;
    usb_write_diag.last_count = count;

    usb_write_diag.recent[usb_write_diag.recent_pos].lba = lba;
    usb_write_diag.recent[usb_write_diag.recent_pos].count = count;
    usb_write_diag.recent_pos =
        (usb_write_diag.recent_pos + 1) % USB_WRITE_DIAG_RECENT;
    if (usb_write_diag.recent_count < USB_WRITE_DIAG_RECENT)
        usb_write_diag.recent_count++;
}

static void usb_write_diag_reset(void)
{
    memset(&usb_write_diag, 0, sizeof(usb_write_diag));
}

static void usb_write_diag_print_summary(const char *reason)
{
    if (usb_write_diag.ops == 0)
        return;

    serial_puts("[USB-STOR] write summary");
    if (reason) {
        serial_puts(" reason=");
        serial_puts(reason);
    }
    serial_puts(" ops=");
    serial_putdec(usb_write_diag.ops);
    serial_puts(" sectors=");
    serial_putdec(usb_write_diag.sectors);
    serial_puts(" first=");
    serial_putdec(usb_write_diag.first_lba);
    serial_puts("+");
    serial_putdec(usb_write_diag.first_count);
    serial_puts(" last=");
    serial_putdec(usb_write_diag.last_lba);
    serial_puts("+");
    serial_putdec(usb_write_diag.last_count);
    serial_puts("\n");
}

void usb_storage_write_diag_flush(const char *reason)
{
    usb_write_diag_print_summary(reason);
    usb_write_diag_reset();
}

static void usb_write_diag_print_recent(void)
{
    uint32_t n = usb_write_diag.recent_count;
    uint32_t start = (usb_write_diag.recent_pos + USB_WRITE_DIAG_RECENT - n) %
                     USB_WRITE_DIAG_RECENT;

    for (uint32_t i = 0; i < n; i++) {
        usb_write_diag_entry_t *e =
            &usb_write_diag.recent[(start + i) % USB_WRITE_DIAG_RECENT];
        serial_puts("[USB-STOR] recent write lba=");
        serial_putdec(e->lba);
        serial_puts(" count=");
        serial_putdec(e->count);
        serial_puts("\n");
    }
}

/* ── SCSI Command Helpers ────────────────────────────────────── */

static int usb_scsi_cmd(const uint8_t *cdb, uint8_t cdb_len,
                        void *data, uint32_t data_len, bool data_in)
{
    if (!xhci_bulk_out || !xhci_bulk_in) return -1;

    /* Build CBW */
    cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = ++usb_disk.cbw_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = data_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    if (xhci_bulk_out(usb_disk.dev_idx, &cbw, sizeof(cbw)) < 0)
        return -1;

    /* Data phase (if any) */
    if (data_len > 0) {
        if (data_in) {
            uint32_t actual;
            if (xhci_bulk_in(usb_disk.dev_idx, data, data_len, &actual) < 0)
                return -1;
        } else {
            if (xhci_bulk_out(usb_disk.dev_idx, data, data_len) < 0)
                return -1;
        }
    }

    /* Receive CSW */
    csw_t csw;
    uint32_t csw_actual;
    if (xhci_bulk_in(usb_disk.dev_idx, &csw, sizeof(csw), &csw_actual) < 0) {
        serial_puts("[USB-STOR] CSW xhci_bulk_in failed\n");
        return -1;
    }

    if (csw.dCSWSignature != CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag) {
        serial_puts("[USB-STOR] CSW sig/tag mismatch sig=");
        serial_puthex(csw.dCSWSignature, 8);
        serial_puts(" tag=");
        serial_puthex(csw.dCSWTag, 8);
        serial_puts(" expected_tag=");
        serial_puthex(cbw.dCBWTag, 8);
        serial_puts("\n");
        return -1;
    }

    if (csw.bCSWStatus != 0) {
        serial_puts("[USB-STOR] CSW status=");
        serial_putdec(csw.bCSWStatus);
        serial_puts(" residue=");
        serial_putdec(csw.dCSWDataResidue);
        serial_puts(" cdb[0]=0x");
        serial_puthex(cdb[0], 2);
        serial_puts("\n");

        /* Status 1 = Command Failed; auto-fetch sense data via REQUEST
         * SENSE so we know WHY the device rejected. */
        if (csw.bCSWStatus == 1) {
            uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
            uint8_t sense[18];
            memset(sense, 0, sizeof(sense));
            cbw_t s_cbw;
            memset(&s_cbw, 0, sizeof(s_cbw));
            s_cbw.dCBWSignature = CBW_SIGNATURE;
            s_cbw.dCBWTag = ++usb_disk.cbw_tag;
            s_cbw.dCBWDataTransferLength = 18;
            s_cbw.bmCBWFlags = 0x80;
            s_cbw.bCBWCBLength = 6;
            memcpy(s_cbw.CBWCB, sense_cdb, 6);
            if (xhci_bulk_out(usb_disk.dev_idx, &s_cbw, sizeof(s_cbw)) >= 0) {
                uint32_t a;
                if (xhci_bulk_in(usb_disk.dev_idx, sense, 18, &a) >= 0) {
                    csw_t s_csw;
                    uint32_t sa;
                    xhci_bulk_in(usb_disk.dev_idx, &s_csw, sizeof(s_csw), &sa);
                    serial_puts("[USB-STOR] sense key=0x");
                    serial_puthex(sense[2] & 0xF, 1);
                    serial_puts(" asc=0x");
                    serial_puthex(sense[12], 2);
                    serial_puts(" ascq=0x");
                    serial_puthex(sense[13], 2);
                    serial_puts("\n");
                }
            }
        }
        return -1;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int usb_storage_init(int xhci_dev_idx)
{
    usb_disk.dev_idx = xhci_dev_idx;
    usb_disk.cbw_tag = 0;

    /* INQUIRY — get device identity */
    uint8_t inq_cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq_data[36];
    if (usb_scsi_cmd(inq_cdb, 6, inq_data, 36, true) < 0) {
        serial_puts("[USB-STOR] INQUIRY failed\n");
        return -1;
    }

    /* Extract vendor (8 bytes @ offset 8) and product (16 bytes @ offset 16) */
    memcpy(usb_disk.vendor, inq_data + 8, 8);
    usb_disk.vendor[8] = '\0';
    memcpy(usb_disk.product, inq_data + 16, 16);
    usb_disk.product[16] = '\0';

    /* READ CAPACITY — get disk size */
    uint8_t cap_cdb[10] = { SCSI_READ_CAPACITY, 0, 0,0,0,0, 0,0,0, 0 };
    uint8_t cap_data[8];
    if (usb_scsi_cmd(cap_cdb, 10, cap_data, 8, true) < 0) {
        serial_puts("[USB-STOR] READ CAPACITY failed\n");
        return -1;
    }

    usb_disk.block_count = ((uint32_t)cap_data[0] << 24) | ((uint32_t)cap_data[1] << 16) |
                           ((uint32_t)cap_data[2] << 8) | cap_data[3];
    usb_disk.block_size  = ((uint32_t)cap_data[4] << 24) | ((uint32_t)cap_data[5] << 16) |
                           ((uint32_t)cap_data[6] << 8) | cap_data[7];
    usb_disk.block_count++;  /* LBA is max LBA, count = max + 1 */

    usb_disk.ready = true;

    serial_puts("[USB-STOR] ");
    serial_puts(usb_disk.vendor);
    serial_puts(" ");
    serial_puts(usb_disk.product);
    serial_puts(": ");
    serial_putdec((uint64_t)usb_disk.block_count * usb_disk.block_size / (1024 * 1024));
    serial_puts(" MB (");
    serial_putdec(usb_disk.block_size);
    serial_puts(" bytes/sector)\n");
    return 0;
}

bool usb_storage_is_ready(void) { return usb_disk.ready; }
uint32_t usb_storage_block_size(void) { return usb_disk.block_size; }
uint32_t usb_storage_block_count(void) { return usb_disk.block_count; }

/* Read sectors from USB disk */
int usb_storage_read(uint64_t lba, uint32_t count, void *buf)
{
    if (!usb_disk.ready) return -1;

    /* SCSI READ(10) — max 128 sectors per command */
    uint8_t *dst = (uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = (count > 128) ? 128 : count;
        uint32_t xfer = chunk * usb_disk.block_size;

        uint8_t cdb[10];
        memset(cdb, 0, 10);
        cdb[0] = SCSI_READ_10;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);

        if (usb_scsi_cmd(cdb, 10, dst, xfer, true) < 0)
            return -1;

        dst += xfer;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/* Write sectors to USB disk via SCSI WRITE(10). Same shape as READ(10)
 * but data goes Bulk-OUT (host→device). Without this the kernel can't
 * persist OsitoFS updates (crash reports, file creates, etc.) when the
 * mounted disk is the boot USB. */
int usb_storage_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (!usb_disk.ready) {
        usb_write_diag_print_summary("not-ready");
        usb_write_diag_print_recent();
        usb_write_diag_reset();
        serial_puts("[USB-STOR] write: not ready\n");
        return -1;
    }

    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = (count > 128) ? 128 : count;
        uint32_t xfer  = chunk * usb_disk.block_size;

        uint8_t cdb[10];
        memset(cdb, 0, 10);
        cdb[0] = SCSI_WRITE_10;
        /* FUA (Force Unit Access) — byte 1 bit 3. Bypass the device's
         * write-back cache: data goes directly to the media before the
         * command completes. Required because most cheap USB controllers
         * silently reject SCSI SYNCHRONIZE_CACHE_10 (sense 0x5/0x24,
         * "Invalid Operation Code"), so an explicit flush has no effect
         * and pulled-USB-while-cache-is-dirty loses recent writes —
         * exactly the "I did dmesg > foo, file's not on disk" symptom. */
        cdb[1] = (1u << 3);
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);

        usb_write_diag_record(lba, chunk);
        if (usb_scsi_cmd(cdb, 10, (void *)(uintptr_t)src, xfer, false) < 0) {
            usb_write_diag_print_summary("error");
            usb_write_diag_print_recent();
            usb_write_diag_reset();
            serial_puts("[USB-STOR] WRITE_10 SCSI failed lba=");
            serial_putdec(lba);
            serial_puts("\n");
            return -1;
        }

        src   += xfer;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

/* SYNCHRONIZE CACHE — flush controller-side write cache to flash.
 * Run this before unplugging or after a series of writes to make
 * sure data is durable. */
int usb_storage_flush(void)
{
    if (!usb_disk.ready) return -1;
    uint8_t cdb[10] = { SCSI_SYNCHRONIZE_CACHE_10, 0, 0,0,0,0, 0,0,0, 0 };
    return usb_scsi_cmd(cdb, 10, NULL, 0, false);
}
