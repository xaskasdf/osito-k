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
#define SCSI_INQUIRY        0x12
#define SCSI_READ_CAPACITY  0x25
#define SCSI_READ_10        0x28

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
    if (xhci_bulk_in(usb_disk.dev_idx, &csw, sizeof(csw), &csw_actual) < 0)
        return -1;

    if (csw.dCSWSignature != CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag)
        return -1;

    return (csw.bCSWStatus == 0) ? 0 : -1;
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
