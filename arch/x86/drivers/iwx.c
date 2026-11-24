/* AX200 transport bring-up. Hardware sequence adapted from OpenBSD if_iwx.c.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Author: Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2007-2010 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * The Intel BSD notice for the register/sequence reference is in iwxreg.h.
 */
#include "iwx.h"
#include "iwxreg.h"
#include "iwx_scan.h"
#include "iwx_link.h"
#include "iwx_data.h"
#include "../kernel/wpa2.h"
#include "../include/paging.h"

extern int paging_map_mmio(uint64_t phys, uint64_t size);
extern uint64_t idt_get_monotonic_ns(void);
extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);
extern int sched_spawn(const char *name, void (*entry)(void));
extern int sched_sleep_ticks(uint64_t ticks);
extern void sched_yield(void);
extern bool sched_is_enabled(void);

#ifdef IWX_HOST_TEST
extern uint32_t iwx_test_read(uint64_t addr);
extern void iwx_test_write(uint64_t addr, uint32_t value);
extern void iwx_test_write8(uint64_t addr, uint8_t value);
extern void *iwx_test_dma_ptr(uint64_t phys);
#define csr_read(d, reg) iwx_test_read((d)->bar0 + (reg))
#define csr_write(d, reg, val) iwx_test_write((d)->bar0 + (reg), (val))
#define csr_write8(d, reg, val) iwx_test_write8((d)->bar0 + (reg), (val))
#define dma_ptr(phys) iwx_test_dma_ptr(phys)
#else
#define csr_read(d, reg) mmio_read32(PHYS_TO_VIRT((d)->bar0 + (reg)))
#define csr_write(d, reg, val) mmio_write32(PHYS_TO_VIRT((d)->bar0 + (reg)), (val))
#define csr_write8(d, reg, val) (*(volatile uint8_t *)PHYS_TO_VIRT((d)->bar0 + (reg)) = (val))
#define dma_ptr(phys) PHYS_TO_VIRT(phys)
#endif

#define POLL_LIMIT 1000000U
static uint8_t probe_lock;

/* Published allocations stay pinned until BOTH master-stop and PCI BME-clear
 * are confirmed. A failed stop quarantines the whole transport, even at ALIVE. */
static struct {
    struct { uint64_t phys; unsigned pages; } allocation[IWX_RX_COUNT + 198];
    unsigned count, bytes;
    uint64_t context, free_ring, used_ring, status, command, rx[IWX_RX_COUNT];
    bool exposed, consumed[IWX_RX_COUNT];
    unsigned read;
    bool streaming, owned[IWX_RX_COUNT];
    unsigned free_write, published, outstanding;
    uint64_t completed, reposted, eligible[IWX_RX_COUNT];
    uint64_t command_data;
    uint64_t tx_descriptors, tx_byte_count, tx_page, tx_started;
    unsigned tx_queue, tx_index;
    bool tx_enabled, tx_pending, tx_data;
    uint32_t tx_wpa_token;
    unsigned command_index, response_size;
    bool command_pending, command_done, init_waiting;
    uint8_t response[512];
    iwx_nvm_report_t *nvm;
    pci_dev_t device;
    uint64_t bar0;
} transport;
static uint64_t retained_bytes;
static wpa2_michael_t michael;
static struct {
    bool active, worker_started;
    iwx_probe_report_t report;
    iwx_status_t status;
    iwx_radio_profile_t profile;
    iwx_scan_status_t scan;
    iwx_bss_t bss[IWX_SCAN_BSS_MAX];
    uint64_t scan_started, regulatory_revision;
    bool mcc_pending;
    uint16_t pending_mcc;
    uint8_t pending_mcc_source;
    iwx_link_t link;
    iwx_data_t data;
    wpa2_t wpa;
    struct { unsigned size; uint8_t data[WPA2_EAPOL_MAX]; } eapol[4];
    unsigned eapol_read, eapol_count;
    bool connect_requested, network_up, tkip_deauth_sent;
    struct { unsigned size; uint8_t data[IWX_ETH_MAX]; } rx_data[16], tx_data[16];
    unsigned rx_read, rx_count, tx_read, tx_count;
} runtime;

static void rx_worker(void);
static bool scan_notification(const uint8_t *h, unsigned payload, iwx_probe_report_t *r);
static bool link_notification(const uint8_t *h, unsigned payload, iwx_probe_report_t *r, uint32_t flags);
static bool link_pump(void);

static void security_reset(void)
{
    __atomic_store_n(&runtime.network_up, false, __ATOMIC_RELEASE);
    runtime.connect_requested = false;
    runtime.tkip_deauth_sent = false;
    wpa2_clear(runtime.rx_data, sizeof(runtime.rx_data));
    wpa2_clear(runtime.tx_data, sizeof(runtime.tx_data));
    runtime.rx_read = runtime.rx_count = runtime.tx_read = runtime.tx_count = 0;
    wpa2_reset(&runtime.wpa);
    wpa2_clear(runtime.eapol, sizeof(runtime.eapol));
    runtime.eapol_read = runtime.eapol_count = 0;
    memset(&runtime.data, 0, sizeof(runtime.data));
    runtime.link.status.port_open = false;
}

typedef struct {
    iwx_progress_fn emit;
    void *arg;
    bool mac_held;
} boot_progress_t;

static bool progress_mark(boot_progress_t *p, iwx_probe_report_t *r, const char *phase)
{
    if (r->progress_failed_at) return false;
    if (!p || !p->emit) return true;
    if (p->mac_held) { r->progress_deferred++; return true; }
    if (r->progress_count < 64) {
        r->progress_count++;
        if (p->emit(p->arg, phase, r)) return true;
    }
    r->progress_failed_at = phase;
    p->emit = NULL;
    return false;
}

static void snapshot_pcie(const pci_dev_t *d, iwx_boot_snapshot_t *s);
static void trace_checkpoint(const pci_dev_t *d, iwx_probe_report_t *r,
                             const char *phase, bool mapped);

static bool inaccessible(uint32_t value)
{
    return value == 0xffffffffU || (value & 0xfffffff0U) == 0xa5a5a5a0U;
}

/* AX200/22000 has a 20-bit PRPH bus address, unlike AX210's 24-bit bus.
 * Callers retain ownership of the required MAC-access/power handshake. */
static uint32_t prph_read(iwx_probe_report_t *r, uint32_t reg)
{
    csr_write(r, IWX_PRPH_RADDR, (reg & IWX_PRPH_ADDR_MASK_22000) | IWX_PRPH_DWORD);
    return csr_read(r, IWX_PRPH_RDAT);
}

static void prph_write(iwx_probe_report_t *r, uint32_t reg, uint32_t value)
{
    csr_write(r, IWX_PRPH_WADDR, (reg & IWX_PRPH_ADDR_MASK_22000) | IWX_PRPH_DWORD);
    csr_write(r, IWX_PRPH_WDAT, value);
}

static bool csr_modify(iwx_probe_report_t *r, unsigned reg,
                       uint32_t clear, uint32_t set)
{
    uint32_t v = csr_read(r, reg);
    if (inaccessible(v)) { r->error = "MMIO inaccessible"; return false; }
    csr_write(r, reg, (v & ~clear) | set);
    return true;
}

static bool delay_us(iwx_probe_report_t *r, unsigned us)
{
    uint64_t start = idt_get_monotonic_ns();
    for (unsigned n = 0; n < POLL_LIMIT; n++) {
        if (idt_get_monotonic_ns() - start >= (uint64_t)us * 1000) return true;
        __asm__ volatile ("pause");
    }
    r->error = "timer/poll budget exhausted";
    return false;
}

static bool wait_bits(iwx_probe_report_t *r, unsigned reg, uint32_t mask,
                      uint32_t value, unsigned us)
{
    uint64_t start = idt_get_monotonic_ns();
    for (unsigned n = 0; n < POLL_LIMIT; n++) {
        uint32_t v = csr_read(r, reg);
        if (inaccessible(v)) { r->error = "MMIO inaccessible"; return false; }
        if ((v & mask) == value) return true;
        if (idt_get_monotonic_ns() - start >= (uint64_t)us * 1000) {
            r->error = "register wait timed out";
            return false;
        }
        __asm__ volatile ("pause");
    }
    r->error = "timer/poll budget exhausted";
    return false;
}

static uint16_t cfg16(const pci_dev_t *d, unsigned reg)
{
    return pci_cfg_read16(d->bus, d->dev, d->func, reg);
}

static uint32_t cfg32(const pci_dev_t *d, unsigned reg)
{
    return pci_cfg_read32(d->bus, d->dev, d->func, reg);
}

static void config16(const pci_dev_t *d, unsigned reg, uint16_t v)
{
    pci_cfg_write16(d->bus, d->dev, d->func, reg, v);
}

/* Validate the complete conventional capability chain before changing power
 * or interrupt delivery. Bounds/cycle checks prevent writes through bad links. */
static bool prepare_pci(const pci_dev_t *d, iwx_probe_report_t *r)
{
    unsigned pm = 0, msi = 0, msix = 0;
    uint64_t visited = 0;
    unsigned cap = (cfg16(d, 6) & 0x10) ? cfg32(d, 0x34) & 0xff : 0;
    while (cap) {
        if (cap < 0x40 || cap > 0xfc || (cap & 3)) goto bad_cap;
        uint64_t bit = 1ULL << ((cap - 0x40) / 4);
        if (visited & bit) goto bad_cap;
        visited |= bit;
        uint16_t header = cfg16(d, cap);
        if ((header & 0xff) == 1) {
            if (pm || cap > 0xf8) goto bad_cap;
            pm = cap;
        } else if ((header & 0xff) == 5) {
            if (msi) goto bad_cap;
            msi = cap;
        } else if ((header & 0xff) == 0x11) {
            if (msix) goto bad_cap;
            msix = cap;
        }
        cap = header >> 8;
    }
    r->command_before = cfg16(d, 4);
    if (r->command_before == 0xffff) {
        r->error = "PCI device disappeared";
        return false;
    }
    config16(d, 4, (r->command_before & ~4U) | 0x400U);
    if (msi) config16(d, msi + 2, cfg16(d, msi + 2) & ~1U);
    if (msix) config16(d, msix + 2, (cfg16(d, msix + 2) & ~0x8000U) | 0x4000U);
    if (pm && (cfg16(d, pm + 4) & 3)) {
        /* PME_Status is W1C: do not echo it. D3hot -> D0 needs recovery time. */
        config16(d, pm + 4, cfg16(d, pm + 4) & ~0x8003U);
        if (!delay_us(r, 10000)) return false;
        if (cfg16(d, pm + 4) & 3) {
            r->error = "PCI power state did not reach D0";
            return false;
        }
    }
    /* D3hot without No_Soft_Reset can reset PCI Command. Do not restore a
     * saved bus-master bit, even if firmware/another OS had enabled it. */
    uint16_t command = cfg16(d, 4);
    if (command == 0xffff || cfg32(d, 0) != 0x27238086U) {
        r->error = "PCI device disappeared during power transition";
        return false;
    }
    config16(d, 4, (command & ~4U) | 0x402U);
    if (msi) config16(d, msi + 2, cfg16(d, msi + 2) & ~1U);
    if (msix) config16(d, msix + 2, (cfg16(d, msix + 2) & ~0x8000U) | 0x4000U);
    if ((msi && (cfg16(d, msi + 2) & 1)) ||
        (msix && (cfg16(d, msix + 2) & 0x8000))) {
        r->error = "PCI message interrupts could not be disabled";
        return false;
    }
    r->command_after = cfg16(d, 4);
    if ((r->command_after & 0x406) != 0x402) {
        r->error = "PCI memory/interrupt/DMA command did not latch";
        return false;
    }
    return true;
bad_cap:
    r->error = "malformed PCI capability chain";
    return false;
}

static bool prepare_card(iwx_probe_report_t *r)
{
    if (!csr_modify(r, IWX_HW_IF_CONFIG, 0, IWX_NIC_READY)) return false;
    if (!wait_bits(r, IWX_HW_IF_CONFIG, IWX_NIC_READY, IWX_NIC_READY, 50)) {
        if (strcmp(r->error, "register wait timed out") != 0) return false;
        r->error = NULL;
        if (!csr_modify(r, IWX_DBG_LINK_PWR, 0, IWX_LINK_PWR_DISABLED) ||
            !delay_us(r, 1000) ||
            !csr_modify(r, IWX_HW_IF_CONFIG, 0, IWX_PREPARE)) return false;
        for (unsigned n = 0; n < 750; n++) {
            if (!csr_modify(r, IWX_HW_IF_CONFIG, 0, IWX_NIC_READY)) return false;
            uint32_t v = csr_read(r, IWX_HW_IF_CONFIG);
            if (inaccessible(v)) { r->error = "MMIO inaccessible"; return false; }
            if (v & IWX_NIC_READY)
                return csr_modify(r, IWX_MBOX_SET, 0, IWX_OS_ALIVE);
            if (!delay_us(r, 200)) return false;
        }
        r->error = "NIC ownership timed out";
        return false;
    }
    return csr_modify(r, IWX_MBOX_SET, 0, IWX_OS_ALIVE);
}

static bool clear_persistence(iwx_probe_report_t *r)
{
    uint32_t hpm = prph_read(r, IWX_HPM_DEBUG);
    /* The power-gated PRPH sentinel is expected before APM init. */
    if (hpm == 0xa5a5a5a0) return true;
    if (inaccessible(hpm)) { r->error = "PRPH inaccessible"; return false; }
    if (hpm & IWX_PERSISTENCE) {
        uint32_t wprot = prph_read(r, IWX_PRPH_WPROT);
        if (inaccessible(wprot) || (wprot & IWX_WFPM_ACCESS)) {
            r->error = "firmware persistence is write-protected";
            return false;
        }
        prph_write(r, IWX_HPM_DEBUG, hpm & ~IWX_PERSISTENCE);
    }
    return true;
}

static bool stop_card(iwx_probe_report_t *r)
{
    csr_write(r, IWX_INT_MASK, 0);
    if (!csr_modify(r, IWX_RESET, 0, IWX_STOP_MASTER) ||
        !wait_bits(r, IWX_RESET, IWX_MASTER_DISABLED, IWX_MASTER_DISABLED, 100000))
        return false;
    r->reset = csr_read(r, IWX_RESET);
    if (!csr_modify(r, IWX_RESET, 0, IWX_SW_RESET) || !delay_us(r, 5000) ||
        !csr_modify(r, IWX_GP_CNTRL, IWX_INIT_DONE, 0)) return false;
    csr_write(r, IWX_INT_MASK, 0);
    return true;
}

static bool start_card(const pci_dev_t *d, iwx_probe_report_t *r, bool *mapped,
                       boot_progress_t *progress)
{
    r->stage = "PCI detection";
    if (!d || d->vendor_id != 0x8086 || d->device_id != 0x2723 ||
        d->class_code != 2 || d->subclass != 0x80) {
        r->error = "no supported AX200 (8086:2723)";
        goto out;
    }
    if (cfg32(d, 0) != 0x27238086U) {
        r->error = "PCI identity changed/disappeared";
        goto out;
    }
    r->stage = "PCI power/config";
    if (!prepare_pci(d, r)) goto out;
    if (!progress_mark(progress, r, "wlan-pci-ready")) return false;
    uint32_t lo = cfg32(d, 0x10);
    uint32_t type = (lo >> 1) & 3;
    r->bar0 = lo & ~15ULL;
    if (type == 2) r->bar0 |= (uint64_t)cfg32(d, 0x14) << 32;
    if (lo == 0xffffffff || (lo & 1) || (type != 0 && type != 2) ||
        r->bar0 < 0x100000 || (r->bar0 & 4095) || r->bar0 > (1ULL << 39) - 8192) {
        r->error = "invalid AX200 memory BAR0";
        goto out;
    }
    r->stage = "MMIO mapping";
    if (paging_map_mmio(r->bar0, 8192) != 0 ||
        paging_translate_in_cr3(paging_get_kernel_cr3(),
                                (uint64_t)PHYS_TO_VIRT(r->bar0)) != r->bar0 ||
        paging_translate_in_cr3(paging_get_kernel_cr3(),
                        (uint64_t)PHYS_TO_VIRT(r->bar0 + 4096)) != r->bar0 + 4096) {
        r->error = "cannot map AX200 register pages";
        goto out;
    }
    r->hw_rev = csr_read(r, IWX_HW_REV);
    if (!r->hw_rev || inaccessible(r->hw_rev)) {
        r->error = "AX200 registers inaccessible";
        goto out;
    }
    *mapped = true;
    if (!progress_mark(progress, r, "wlan-mmio-ready")) return false;
    r->hw_rf_id = csr_read(r, IWX_HW_RF_ID);
    r->interrupt = csr_read(r, IWX_INT);
    if (r->trace_count) trace_checkpoint(d, r, "before-card-reset", true);
    csr_write(r, IWX_INT_MASK, 0);
    r->stage = "NIC ownership";
    if (!prepare_card(r)) goto out;
    r->stage = "firmware persistence";
    if (!clear_persistence(r)) goto out;
    r->stage = "software reset";
    if (!csr_modify(r, IWX_RESET, 0, IWX_SW_RESET) || !delay_us(r, 5000)) goto out;
    r->stage = "MAC clock";
    if (!csr_modify(r, IWX_GIO_CHICKEN, 0, IWX_L1A_NO_L0S_RX) ||
        !csr_modify(r, IWX_DBG_HPET, 0, IWX_HPET_MAX) ||
        !csr_modify(r, IWX_HW_IF_CONFIG, 0, IWX_HAP_WAKE_L1A) ||
        !csr_modify(r, IWX_GIO, 0, IWX_L0S_DISABLED) ||
        !csr_modify(r, IWX_GP_CNTRL, 0, IWX_INIT_DONE) ||
        !wait_bits(r, IWX_GP_CNTRL, IWX_CLOCK_READY, IWX_CLOCK_READY, 25000)) goto out;
    r->clock_ready = true;
    r->gp_cntrl = csr_read(r, IWX_GP_CNTRL);
    if (inaccessible(r->gp_cntrl)) {
        r->error = "radio status inaccessible";
        goto out;
    }
    r->rfkill_known = true;
    r->rfkill = !(r->gp_cntrl & IWX_RF_ENABLED);
    r->stage = "probe complete";
out:
    return r->error == NULL;
}

static void finish_probe(const pci_dev_t *d, iwx_probe_report_t *r, bool mapped)
{
    if (mapped) {
        const char *error = r->error;
        const char *stage = r->stage;
        r->stage = "device stop";
        r->stopped = stop_card(r);
        if (!r->stopped) r->stop_error = r->error;
        if (error) r->error = error;
        if (error || r->stopped) r->stage = stage;
    }
    if (d) r->command_after = cfg16(d, 4);
}

static bool claim(iwx_probe_report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->stage = "driver ownership";
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) {
        r->error = "WLAN diagnostic or kernel handoff is active";
        return false;
    }
    if (transport.count) {
        r->error = runtime.active ? "WLAN transport active; run wlan stop first" :
                                   "DMA quarantined; run wlan stop before retrying";
        if (!runtime.active) r->retained_bytes = transport.bytes;
        __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

int iwx_probe(iwx_probe_report_t *r)
{
    if (!r || !claim(r)) return -1;
    const pci_dev_t *d = pci_get_wlan();
    bool mapped = false;
    start_card(d, r, &mapped, NULL);
    finish_probe(d, r, mapped);
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return r->error ? -1 : 0;
}

static uint64_t dma_alloc(unsigned bytes)
{
    unsigned pages = (bytes + 4095U) / 4096U;
    if (transport.count == sizeof(transport.allocation) / sizeof(transport.allocation[0]))
        return 0;
    uint64_t phys = (uint64_t)mem_alloc_pages(pages);
    if (!phys) return 0;
    /* Conservative 32-bit DMA aperture, also excludes 4-GiB-crossing blocks. */
    if ((phys & 4095) || phys >= 0x100000000ULL ||
        (uint64_t)pages * 4096 > 0x100000000ULL - phys) goto fail;
    for (unsigned i = 0; i < pages; i++) {
        uint64_t p = phys + (uint64_t)i * 4096;
        if (paging_translate_in_cr3(paging_get_kernel_cr3(),
                                   (uint64_t)PHYS_TO_VIRT(p)) != p) goto fail;
    }
    transport.allocation[transport.count].phys = phys;
    transport.allocation[transport.count++].pages = pages;
    transport.bytes += pages * 4096;
    memset(dma_ptr(phys), 0, pages * 4096);
    return phys;
fail:
    mem_free_pages((void *)phys, pages);
    return 0;
}

static void dma_release(void)
{
    /* Called only after verified DMA quiescence (or before publication).
     * A timed-out key command must remain pinned until this point. */
    if (transport.command_data) wpa2_clear(dma_ptr(transport.command_data), 4096);
    if (transport.tx_page) wpa2_clear(dma_ptr(transport.tx_page), 4096);
    while (transport.count) {
        unsigned i = --transport.count;
        mem_free_pages((void *)transport.allocation[i].phys, transport.allocation[i].pages);
    }
    memset(&transport, 0, sizeof(transport));
    __atomic_store_n(&retained_bytes, 0, __ATOMIC_RELEASE);
}

static bool alloc_transport(const void *data, const iwx_fw_plan_t *p,
                            iwx_probe_report_t *r)
{
    transport.context = dma_alloc(sizeof(iwx_context_t));
    transport.free_ring = dma_alloc(IWX_RX_COUNT * 8);
    transport.used_ring = dma_alloc(IWX_RX_COUNT * 4);
    transport.status = dma_alloc(12);
    transport.command = dma_alloc(IWX_CMD_COUNT * sizeof(iwx_tfh_tfd_t));
    if (!transport.context || !transport.free_ring || !transport.used_ring ||
        !transport.status || !transport.command) return false;
    if (transport.nvm) {
        transport.command_data = dma_alloc(4096);
        if (!transport.command_data) return false;
    }
    for (unsigned i = 0; i < IWX_RX_COUNT; i++) {
        transport.rx[i] = dma_alloc(4096);
        if (!transport.rx[i]) return false;
        /* Every possible packet-aligned location starts invalid. */
        uint32_t *buf = dma_ptr(transport.rx[i]);
        for (unsigned n = 0; n < 4096 / 4; n += 16) buf[n] = IWX_RX_INVALID;
    }
    iwx_context_t *c = dma_ptr(transport.context);
    c->mac_id = r->hw_rev;
    c->size_dwords = sizeof(*c) / 4;
    c->control_flags = (1U << 8) | (9U << 4) | (4U << 9);
    c->free_rbd = transport.free_ring;
    c->used_rbd = transport.used_ring;
    c->status = transport.status;
    c->cmd_queue = transport.command;
    c->cmd_queue_size = 5; /* log2(256 entries) - 3 */
    for (unsigned group = 0; group < 3; group++) {
        for (unsigned i = 0; i < p->info.sections[group]; i++) {
            uint64_t phys = dma_alloc(p->section[group][i].length);
            if (!phys) return false;
            memcpy(dma_ptr(phys), (const uint8_t *)data + p->section[group][i].offset,
                   p->section[group][i].length);
            if (group == 0) c->lmac[i] = phys;
            else if (group == 1) c->umac[i] = phys;
            else c->paging[i] = phys;
        }
    }
    return true;
}

static bool launch_firmware(const pci_dev_t *d, const iwx_fw_plan_t *p,
                            iwx_probe_report_t *r, boot_progress_t *progress)
{
    uint32_t phy = p->info.phy_config;
    uint32_t hwif = ((r->hw_rev & 3) << 2) | ((phy & 3) << 10) |
                   (((phy >> 2) & 3) << 14) | (((phy >> 4) & 3) << 12);
    if (!csr_modify(r, IWX_HW_IF_CONFIG, 0x0000ff0f, hwif) ||
        !csr_modify(r, IWX_SHADOW_CTRL, 0, 0x800fffff)) return false;
    csr_write8(r, IWX_INT_COALESCING, 0x40);
    csr_write8(r, IWX_INT_PERIODIC, 0);
    csr_write(r, IWX_UCODE_GP1_CLR, 2);
    csr_write(r, IWX_UCODE_GP1_CLR, 4);
    csr_write(r, IWX_INT, 0xffffffff);
    csr_write(r, IWX_FH_INT, 0xffffffff);
    if (progress) progress->mac_held = true;
    if (!csr_modify(r, IWX_GP_CNTRL, 0, IWX_MAC_ACCESS) ||
        !delay_us(r, 2) || !wait_bits(r, IWX_GP_CNTRL,
                IWX_CLOCK_READY | IWX_GOING_SLEEP, IWX_CLOCK_READY, 150000)) return false;

    transport.device = *d;
    transport.bar0 = r->bar0;
    /* Even a failed BME readback must take the verified-stop cleanup path. */
    transport.exposed = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16_t command = cfg16(d, 4);
    if (command == 0xffff || cfg32(d, 0) != 0x27238086U) {
        r->error = "PCI device disappeared before DMA start";
        return false;
    }
    trace_checkpoint(d, r, "before-bme", true);
    config16(d, 4, command | 0x406);
    command = cfg16(d, 4);
    trace_checkpoint(d, r, "after-bme", true);
    if (command == 0xffff || (command & 0x406) != 0x406) {
        r->error = "PCI bus mastering did not latch";
        return false;
    }
    csr_write(r, IWX_CTXT_INFO_BA, (uint32_t)transport.context);
    csr_write(r, IWX_CTXT_INFO_BA + 4, transport.context >> 32);
    trace_checkpoint(d, r, "after-context", true);
    /* LTR snoop/non-snoop scale fields start at bits 10/26 (1.024 us units). */
    csr_write(r, IWX_LTR_LONG, (1U << 31) | (2U << 26) | (250U << 16) |
                             (1U << 15) | (2U << 10) | 250U);
    trace_checkpoint(d, r, "before-cpu-run", true);
    prph_write(r, IWX_CPU_INIT_RUN, 1);
    trace_checkpoint(d, r, "after-cpu-run", true);
    bool released = csr_modify(r, IWX_GP_CNTRL, IWX_MAC_ACCESS, 0);
    unsigned previous = r->trace_count;
    trace_checkpoint(d, r, "after-mac-release", true);
    if (progress && released && r->trace_count > previous) {
        const iwx_boot_trace_sample_t *t = &r->trace[previous];
        if (t->csr_valid && !(t->gp_cntrl & IWX_MAC_ACCESS)) progress->mac_held = false;
    }
    return released;
}

static uint32_t rx_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rx_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool command_response(const uint8_t *h, unsigned payload,
                              iwx_probe_report_t *r)
{
    iwx_nvm_report_t *n = transport.nvm;
    if (h[0] == 2 && h[1] == 0) {
        r->error = "firmware command error notification";
        return false;
    }
    if (h[0] == IWX_INIT_COMPLETE && h[1] == 0) {
        n->init_notifications++;
        n->init_sequence = rx_le16(h + 2);
        n->init_payload_bytes = payload;
        /* Bit 15 identifies a firmware event; the remaining sequence bits
         * are not an echoed command queue/index. INIT has no decoded body. */
        if (!(h[3] & 0x80)) {
            r->error = "INIT_COMPLETE missing firmware-origin flag";
            return false;
        }
        if (!transport.init_waiting || n->init_complete || n->commands_sent < 2) {
            r->error = "duplicate/out-of-phase INIT_COMPLETE notification";
            return false;
        }
        n->init_complete = true;
        return true;
    }
    if (h[3] & 0x80) return true; /* Unsolicited notifications never complete a command. */
    if (!transport.command_pending || transport.command_done || h[3] != 0 ||
        h[2] != n->last_index || h[0] != n->last_opcode ||
        (h[1] & ~IWX_CMD_FAILED) != n->last_group) {
        r->error = "unexpected firmware command response sequence/ID";
        return false;
    }
    if (h[1] & IWX_CMD_FAILED) {
        r->error = "firmware rejected command";
        return false;
    }
    if (payload > sizeof(transport.response)) {
        r->error = "firmware command response exceeds buffer";
        return false;
    }
    memcpy(transport.response, h + 4, payload);
    transport.response_size = n->response_bytes = payload;
    transport.command_done = true;
    n->commands_completed++;
    return true;
}

static bool parse_rx(const uint8_t *buf, iwx_probe_report_t *r)
{
    for (unsigned pos = 0; pos + 8 <= 4096;) {
        uint32_t flags = rx_le32(buf + pos);
        if (flags == IWX_RX_INVALID) return true;
        if (flags & 0x003f0000U) {
            r->error = "RX notification from unconfigured queue";
            return false;
        }
        unsigned len = (flags & 0x3fff) + 4;
        if (len < 8 || len > 4096 - pos) {
            r->error = "invalid RX packet extent";
            return false;
        }
        const uint8_t *h = buf + pos + 4;
        r->rx_packets++;
        bool tx = runtime.active && h[0] == IWX_TX_FRAME && h[1] == 0;
        if (transport.nvm && !tx && !command_response(h, len - 8, r)) return false;
        if (runtime.active && !scan_notification(h, len - 8, r)) return false;
        if (runtime.active && !link_notification(h, len - 8, r, flags)) return false;
        if (h[0] == 1 && h[1] == 0) {
            unsigned expected = r->alive_version == 4 ? 116 :
                                r->alive_version == 5 ? 128 : 144;
            if (!(h[3] & 0x80) || len != expected + 8 || r->alive) {
                r->error = "invalid ALIVE notification header/length";
                return false;
            }
            const uint8_t *a = h + 4;
            r->alive_status = (uint16_t)a[0] | ((uint16_t)a[1] << 8);
            if (r->alive_status != 0xcafe) {
                r->error = "firmware reported ALIVE failure";
                return false;
            }
            r->lmac_major = rx_le32(a + 4);
            r->lmac_minor = rx_le32(a + 8);
            r->umac_major = rx_le32(a + 100);
            r->umac_minor = rx_le32(a + 104);
            r->alive = true;
        }
        pos += (len + 63U) & ~63U;
    }
    return true;
}

enum rx_wait { WAIT_ALIVE, WAIT_COMMAND, WAIT_INIT };

static bool drain_rx(iwx_probe_report_t *r, boot_progress_t *progress,
                      bool *rx_seen, unsigned budget)
{
    unsigned producer = *(volatile uint16_t *)dma_ptr(transport.status) & 0xfff;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    unsigned distance = (producer - transport.read) & (IWX_RX_COUNT - 1);
    if ((!transport.streaming && (producer < transport.read || producer > IWX_RX_COUNT - 8)) ||
        (transport.streaming && (producer >= IWX_RX_COUNT || distance > transport.outstanding))) {
        r->error = "invalid RX completion cursor";
        return false;
    }
    uint64_t snapshot_end = transport.completed + distance;
    volatile uint32_t *used = dma_ptr(transport.used_ring);
    if (rx_seen && !*rx_seen && transport.read != producer) {
        *rx_seen = true;
        if (!progress_mark(progress, r, "wlan-rx-first")) return false;
    }
    while (transport.read != producer && budget--) {
        unsigned id = used[transport.read] & 0xfff;
        if (!id || id > IWX_RX_COUNT - 8 ||
            (transport.streaming ? (!transport.owned[id - 1] ||
                                      transport.completed < transport.eligible[id - 1]) :
                                     transport.consumed[id - 1])) {
            r->error = "invalid/duplicate RX buffer ID";
            return false;
        }
        transport.consumed[id - 1] = true;
        transport.owned[id - 1] = false;
        uint32_t *buf = dma_ptr(transport.rx[id - 1]);
        if (!parse_rx((const uint8_t *)buf, r)) return false;
        transport.read = (transport.read + 1) & (IWX_RX_COUNT - 1);
        transport.completed++;
        if (transport.streaming) {
            transport.outstanding--;
            for (unsigned n = 0; n < 4096 / 4; n += 16) buf[n] = IWX_RX_INVALID;
            uint64_t *free_ring = dma_ptr(transport.free_ring);
            free_ring[transport.free_write] = transport.rx[id - 1] | id;
            transport.free_write = (transport.free_write + 1) & (IWX_RX_COUNT - 1);
        }
    }
    if (transport.streaming) {
        unsigned publish = transport.free_write & ~7U;
        if (publish != transport.published) {
            uint64_t *free_ring = dma_ptr(transport.free_ring);
            /* Do not permit reuse to disguise a duplicate in the already
             * sampled completion backlog, including across budgeted polls. */
            while (transport.published != publish) {
                unsigned id = free_ring[transport.published] & 0xfff;
                transport.eligible[id - 1] = snapshot_end;
                transport.owned[id - 1] = true;
                transport.outstanding++;
                transport.reposted++;
                transport.published = (transport.published + 1) & (IWX_RX_COUNT - 1);
            }
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            csr_write(r, IWX_RX_WIDX, publish);
        }
    }
    return true;
}

static bool poll_rx(iwx_probe_report_t *r, boot_progress_t *progress,
                     bool first, bool *rx_seen, unsigned budget)
{
    bool rfh_started = false;
    uint32_t cause = csr_read(r, IWX_INT), fh = csr_read(r, IWX_FH_INT);
    r->interrupt |= cause;
    r->gp_cntrl = csr_read(r, IWX_GP_CNTRL);
    if (first) trace_checkpoint(&transport.device, r, "first-poll", true);
    if (inaccessible(cause) || inaccessible(fh) || inaccessible(r->gp_cntrl)) {
        r->error = "MMIO inaccessible during firmware operation";
        return false;
    }
    r->rfkill = !(r->gp_cntrl & IWX_RF_ENABLED);
    if (r->rfkill || (cause & IWX_INT_CT_KILL)) {
        r->error = "radio/thermal switch blocked firmware operation";
        return false;
    }
    if ((cause & IWX_INT_HW_ERR) || (fh & IWX_FH_ERROR)) {
        r->error = "hardware/DMA error interrupt";
        return false;
    }
    if (cause & IWX_INT_SW_ERR) {
        r->error = "firmware software error interrupt";
        return false;
    }
    if ((cause & IWX_INT_ALIVE) && !r->rfh_ready) {
        trace_checkpoint(&transport.device, r, "rfh-alive-interrupt", true);
        uint64_t *free_ring = dma_ptr(transport.free_ring);
        for (unsigned i = 0; i < IWX_RX_COUNT; i++) {
            free_ring[i] = transport.rx[i] | (i + 1);
            transport.owned[i] = i < IWX_RX_COUNT - 8;
        }
        transport.free_write = transport.published = transport.outstanding = IWX_RX_COUNT - 8;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        csr_write(r, IWX_RX_WIDX, 8);
        /* Keep a full/empty gap. Only the opt-in runtime recycles buffers. */
        csr_write(r, IWX_RX_WIDX, IWX_RX_COUNT - 8);
        r->rfh_ready = true;
        rfh_started = true;
        r->stage = "firmware ALIVE notification";
    }
    if (cause) csr_write(r, IWX_INT, cause);
    if (fh) csr_write(r, IWX_FH_INT, fh);
    if (rfh_started && !progress_mark(progress, r, "wlan-rfh-ready")) return false;
    return !r->rfh_ready || drain_rx(r, progress, rx_seen, budget);
}

static bool wait_rx(iwx_probe_report_t *r, boot_progress_t *progress, enum rx_wait what)
{
    bool rx_seen = false;
    unsigned limit = what == WAIT_ALIVE ? 50000 : what == WAIT_INIT ? 20000 : 10000;
    uint64_t start = idt_get_monotonic_ns();
    for (unsigned iteration = 0; iteration < limit; iteration++) {
        if (what == WAIT_ALIVE) r->poll_count++;
        else transport.nvm->command_polls++;
        if (!poll_rx(r, progress, iteration == 0 && what == WAIT_ALIVE,
                     &rx_seen, transport.streaming ? 64 : IWX_RX_COUNT)) return false;
        if (r->rfh_ready) {
            if ((what == WAIT_ALIVE && r->alive) ||
                (what == WAIT_COMMAND && transport.command_done) ||
                (what == WAIT_INIT && transport.nvm->init_complete)) return true;
            if (!transport.streaming && transport.read == IWX_RX_COUNT - 8) {
                r->error = "diagnostic RX buffer budget exhausted";
                return false;
            }
        }
        if (idt_get_monotonic_ns() - start >= (uint64_t)limit * 100000ULL) break;
        if (!delay_us(r, 100)) return false;
    }
    r->error = what == WAIT_COMMAND ? "firmware command response timed out" :
               what == WAIT_INIT ? "firmware INIT_COMPLETE timed out" :
               r->rfh_ready ? "firmware ALIVE notification timed out" :
                              "firmware RFH initialization timed out";
    return false;
}

static bool wait_alive(iwx_probe_report_t *r, boot_progress_t *progress)
{
    return wait_rx(r, progress, WAIT_ALIVE);
}

static bool send_command_version(iwx_probe_report_t *r, uint8_t group, uint8_t opcode,
                                 const void *payload, unsigned size, uint8_t version)
{
    iwx_nvm_report_t *n = transport.nvm;
    if (r->error) return false;
    if (!n || !r->alive || !r->rfh_ready || !transport.command_data ||
        transport.command_pending || transport.command_index >= IWX_CMD_COUNT ||
        !group || (group & IWX_CMD_FAILED) || size > 4096 - sizeof(iwx_cmd_header_t) ||
        (size && !payload)) {
        r->error = "invalid firmware command state/size";
        return false;
    }
    iwx_cmd_header_t *h = dma_ptr(transport.command_data);
    memset(h, 0, 4096);
    h->opcode = opcode;
    h->group = group;
    h->index = transport.command_index;
    h->length = size;
    h->version = version;
    if (size) memcpy(h + 1, payload, size);
    iwx_tfh_tfd_t *t = (iwx_tfh_tfd_t *)dma_ptr(transport.command) + h->index;
    memset(t, 0, sizeof(*t));
    unsigned total = sizeof(*h) + size;
    t->num_tbs = total > IWX_FIRST_TB_SIZE ? 2 : 1;
    t->tb[0].address = transport.command_data;
    t->tb[0].length = total > IWX_FIRST_TB_SIZE ? IWX_FIRST_TB_SIZE : total;
    if (t->num_tbs == 2) {
        t->tb[1].address = transport.command_data + IWX_FIRST_TB_SIZE;
        t->tb[1].length = total - IWX_FIRST_TB_SIZE;
    }
    n->last_group = group;
    n->last_opcode = opcode;
    n->last_index = h->index;
    n->response_bytes = transport.response_size = 0;
    transport.command_done = false;
    transport.command_pending = true;
    n->commands_sent++;
    /* One outstanding command; never reuse its buffer until a matching RX
     * completion. Any timeout aborts the session through verified shutdown. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned next = (transport.command_index + 1) & (IWX_CMD_COUNT - 1);
    transport.command_index = transport.streaming ? next : transport.command_index + 1;
    csr_write(r, IWX_HBUS_TARG_WRPTR, next);
    if (!wait_rx(r, NULL, WAIT_COMMAND)) return false;
    transport.command_pending = false;
    return true;
}

static bool send_command(iwx_probe_report_t *r, uint8_t group, uint8_t opcode,
                         const void *payload, unsigned size)
{
    return send_command_version(r, group, opcode, payload, size, 0);
}

static bool decode_nvm(iwx_nvm_report_t *n, const uint8_t *p, unsigned len)
{
    unsigned expected = n->response_version == 4 ? 468 : 128;
    if ((n->response_version != 3 && n->response_version != 4) || len != expected)
        return false;
    unsigned channels = n->response_version == 4 ? rx_le32(p + 24) : 51;
    uint32_t tx = rx_le32(p + 12), rx = rx_le32(p + 16), lar = rx_le32(p + 20);
    if (!channels || channels > IWX_NVM_CHANNELS || !tx || !rx ||
        (tx & ~3U) || (rx & ~3U) || lar > 1) return false;
    n->general_flags = rx_le32(p);
    n->nvm_version = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    n->board_type = p[6];
    n->n_hw_addrs = p[7];
    n->mac_sku = rx_le32(p + 8);
    n->tx_chains = tx;
    n->rx_chains = rx;
    n->lar_enabled = lar;
    n->n_channels = channels;
    for (unsigned i = 0; i < channels; i++)
        n->channel_profile[i] = n->response_version == 4 ? rx_le32(p + 28 + i * 4) :
                               (uint32_t)p[24 + i * 2] | ((uint32_t)p[25 + i * 2] << 8);
    return true;
}

static bool valid_mac(const uint8_t *a)
{
    static const uint8_t zero[6], reserved[6] = {2, 0xcc, 0xaa, 0xff, 0xee, 0};
    return !(a[0] & 1) && memcmp(a, zero, 6) && memcmp(a, reserved, 6);
}

static bool read_mac(iwx_probe_report_t *r, iwx_nvm_report_t *n, boot_progress_t *progress)
{
    bool ok = false;
    progress->mac_held = true;
    if (!csr_modify(r, IWX_GP_CNTRL, 0, IWX_MAC_ACCESS) || !delay_us(r, 2) ||
        !wait_bits(r, IWX_GP_CNTRL, IWX_CLOCK_READY | IWX_GOING_SLEEP,
                    IWX_CLOCK_READY, 150000)) goto release;
    for (unsigned otp = 0; otp < 2; otp++) {
        uint32_t lo = csr_read(r, otp ? IWX_MAC_OTP0 : IWX_MAC_STRAP0);
        uint32_t hi = csr_read(r, otp ? IWX_MAC_OTP1 : IWX_MAC_STRAP1);
        uint8_t a[6] = {lo >> 24, lo >> 16, lo >> 8, lo, hi >> 8, hi};
        if (!inaccessible(lo) && !inaccessible(hi) && valid_mac(a)) {
            memcpy(n->mac, a, 6);
            n->mac_from_otp = otp != 0;
            ok = true;
            break;
        }
    }
    if (!ok) r->error = "no valid hardware MAC in strap/OTP";
release:;
    const char *error = r->error;
    bool released = csr_modify(r, IWX_GP_CNTRL, IWX_MAC_ACCESS, 0);
    uint32_t gp = csr_read(r, IWX_GP_CNTRL);
    if (released && !inaccessible(gp) && !(gp & IWX_MAC_ACCESS)) progress->mac_held = false;
    else { ok = false; r->error = "MAC access release not confirmed"; }
    if (error) r->error = error;
    return ok;
}

static bool initialize_nvm(iwx_probe_report_t *r, iwx_nvm_report_t *n,
                           boot_progress_t *progress)
{
    uint32_t flags = 2, reserved = 0;
    r->stage = "firmware init configuration";
    if (!progress_mark(progress, r, "wlan-init-config") ||
        !send_command(r, IWX_SYSTEM_GROUP, IWX_INIT_EXTENDED, &flags, sizeof(flags))) return false;
    r->stage = "firmware NVM access complete";
    if (!progress_mark(progress, r, "wlan-nvm-complete")) return false;
    transport.init_waiting = true;
    bool ready = send_command(r, IWX_NVM_GROUP, IWX_NVM_COMPLETE, &reserved, sizeof(reserved));
    if (ready) {
        r->stage = "firmware INIT_COMPLETE notification";
        ready = progress_mark(progress, r, "wlan-init-wait") && wait_rx(r, NULL, WAIT_INIT);
    }
    transport.init_waiting = false;
    if (!ready) return false;
    r->stage = "firmware NVM read";
    if (!progress_mark(progress, r, "wlan-nvm-get") ||
        !send_command(r, IWX_NVM_GROUP, IWX_NVM_GET, &reserved, sizeof(reserved))) return false;
    if (!decode_nvm(n, transport.response, transport.response_size)) {
        r->error = "invalid NVM response length/capabilities";
        return false;
    }
    r->stage = "hardware MAC read";
    if (!progress_mark(progress, r, "wlan-nvm-mac") || !read_mac(r, n, progress)) return false;
    n->valid = true;
    r->stage = "firmware NVM validated";
    return progress_mark(progress, r, "wlan-nvm-ready");
}

static bool stop_transport(iwx_probe_report_t *r)
{
    const pci_dev_t *d = &transport.device;
    r->bar0 = transport.bar0;
    r->stage = "DMA shutdown";
    uint32_t lo = cfg32(d, 0x10);
    uint64_t bar = lo & ~15ULL;
    if ((lo & 6) == 4) bar |= (uint64_t)cfg32(d, 0x14) << 32;
    if (cfg32(d, 0) != 0x27238086U || bar != r->bar0 || (lo & 1)) {
        r->error = "cannot verify DMA owner PCI identity/BAR";
        return false;
    }
    bool stopped = stop_card(r);
    uint16_t command = cfg16(d, 4);
    if (command != 0xffff) config16(d, 4, (command & ~4U) | 0x400);
    r->command_after = cfg16(d, 4);
    if (r->command_after == 0xffff || (r->command_after & 4)) {
        r->error = "PCI bus mastering disable not confirmed";
        return false;
    }
    return stopped;
}

static void snapshot_pcie(const pci_dev_t *d, iwx_boot_snapshot_t *s)
{
    uint64_t visited = 0;
    unsigned pm = 0;
    unsigned cap = (s->pci_command_status & (1U << 20)) ? cfg32(d, 0x34) & 0xff : 0;
    while (cap) {
        if (cap < 0x40 || cap > 0xfc || (cap & 3)) break;
        uint64_t bit = 1ULL << ((cap - 0x40) / 4);
        if (visited & bit) break;
        visited |= bit;
        uint16_t header = cfg16(d, cap);
        if ((header & 0xff) == 0x10) {
            if (s->pcie_offset || cap > 0xec) break;
            s->pcie_offset = cap;
        } else if ((header & 0xff) == 1) {
            if (pm || cap > 0xf8) break;
            pm = cap;
        }
        cap = header >> 8;
    }
    if (cap) { s->pci_error = "malformed PCI capability chain at snapshot"; return; }
    if (pm) {
        s->pm_offset = pm;
        s->pm_control = cfg16(d, pm + 4);
        s->pm_valid = s->pm_control != 0xffff;
    }
    if (!s->pcie_offset) { s->pci_error = "no PCIe capability"; return; }
    unsigned p = s->pcie_offset;
    s->pcie_flags = cfg16(d, p + 2);
    /* AX200 is an endpoint with link registers, not an RC integrated endpoint. */
    if (!(s->pcie_flags & 15) || (s->pcie_flags & 0xf0)) {
        s->pci_error = "unexpected PCIe capability version/type";
        return;
    }
    s->pcie_device_control = cfg16(d, p + 8);
    s->pcie_device_status = cfg16(d, p + 10);
    s->pcie_link_control = cfg16(d, p + 16);
    s->pcie_link_status = cfg16(d, p + 18);
    if (s->pcie_device_control == 0xffff || s->pcie_device_status == 0xffff ||
        s->pcie_link_control == 0xffff || s->pcie_link_status == 0xffff) {
        s->pci_error = "PCIe registers inaccessible";
        return;
    }
    s->pcie_valid = true;
}

/* Only bounded config/CSR reads: no PRPH access, W1C, allocation or output.
 * Samples are not atomic; elapsed time includes any preemption between reads. */
static void trace_checkpoint(const pci_dev_t *d, iwx_probe_report_t *r,
                             const char *phase, bool mapped)
{
    if (!d || d->vendor_id != 0x8086 || d->device_id != 0x2723 ||
        d->class_code != 2 || d->subclass != 0x80) return;
    if (r->trace_count >= IWX_BOOT_TRACE_SAMPLES) {
        r->trace_dropped++;
        return;
    }
    uint64_t now = idt_get_monotonic_ns();
    if (!r->trace_count) r->trace_start_ns = now;
    iwx_boot_trace_sample_t *t = &r->trace[r->trace_count++];
    t->phase = phase;
    t->elapsed_us = (now - r->trace_start_ns) / 1000;
    t->context_expected = transport.context;
    t->dma_bytes = transport.bytes;
    t->rx_read = transport.read;
    if (transport.status)
        t->rx_producer = *(volatile uint16_t *)dma_ptr(transport.status);
    t->pci_id = cfg32(d, 0);
    t->pci_command_status = cfg32(d, 4);
    if (t->pci_id != 0x27238086U || t->pci_command_status == 0xffffffff) {
        t->note = "PCI identity unavailable; register reads skipped";
        return;
    }
    t->pci_valid = true;
    iwx_boot_snapshot_t pci = {.pci_command_status = t->pci_command_status};
    snapshot_pcie(d, &pci);
    t->pcie_valid = pci.pcie_valid;
    t->pcie_device_control = pci.pcie_device_control;
    t->pcie_device_status = pci.pcie_device_status;
    t->pcie_link_control = pci.pcie_link_control;
    t->pcie_link_status = pci.pcie_link_status;
    t->pm_valid = pci.pm_valid;
    t->pm_control = pci.pm_control;
    t->note = pci.pci_error;
    if (!mapped) return;
    uint32_t lo = cfg32(d, 0x10);
    uint64_t bar = lo & ~15ULL;
    if ((lo & 6) == 4) bar |= (uint64_t)cfg32(d, 0x14) << 32;
    if (!(t->pci_command_status & 2) || (lo & 1) || bar != r->bar0 ||
        (t->pm_valid && (t->pm_control & 3))) {
        t->note = "PCI BAR/decode/power changed; CSR reads skipped";
        return;
    }
    t->csr_sampled = true;
    t->interrupt = csr_read(r, IWX_INT);
    t->fh_interrupt = csr_read(r, IWX_FH_INT);
    t->gp_cntrl = csr_read(r, IWX_GP_CNTRL);
    t->reset = csr_read(r, IWX_RESET);
    if (inaccessible(t->interrupt) || inaccessible(t->fh_interrupt) ||
        inaccessible(t->gp_cntrl) || inaccessible(t->reset)) {
        t->note = "CSR inaccessible; remaining reads skipped";
        return;
    }
    t->hw_if = csr_read(r, IWX_HW_IF_CONFIG);
    t->int_mask = csr_read(r, IWX_INT_MASK);
    uint32_t low = csr_read(r, IWX_CTXT_INFO_BA);
    uint32_t high = csr_read(r, IWX_CTXT_INFO_BA + 4);
    t->context_observed = low | ((uint64_t)high << 32);
    t->csr_valid = !inaccessible(t->hw_if) && !inaccessible(t->int_mask) &&
                   !inaccessible(low) && !inaccessible(high);
    if (!t->csr_valid) t->note = "CSR inaccessible during sample";
}

/* Copy evidence while allocations are pinned and before stop/reset or W1C.
 * Do not print, allocate, acknowledge errors or change the original failure. */
static void snapshot_transport(iwx_probe_report_t *r, boot_progress_t *progress)
{
    static const struct { const char *name; uint32_t address; } registers[] = {
        {"RFH_RXF_DMA_CFG", IWX_RFH_RXF_DMA_CFG},
        {"RFH_GEN_CFG", IWX_RFH_GEN_CFG},
        {"RFH_GEN_STATUS", IWX_RFH_GEN_STATUS},
        {"FH_TX_STATUS", IWX_FH_TX_STATUS},
        {"FH_TX_ERROR", IWX_FH_TX_ERROR},
        {"SB_CPU1_STATUS", IWX_SB_CPU1_STATUS},
        {"SB_CPU2_STATUS", IWX_SB_CPU2_STATUS},
        {"UMAC_CPU1_STATUS", IWX_UMAC_CPU1_STATUS},
        {"UMAC_CPU2_STATUS", IWX_UMAC_CPU2_STATUS},
        {"UCODE_LOAD_STATUS", IWX_UCODE_LOAD_STATUS},
    };
    _Static_assert(sizeof(registers) / sizeof(registers[0]) == IWX_BOOT_PRPH_SAMPLES,
                   "boot snapshot capacity");
    iwx_boot_snapshot_t *s = &r->fault;
    if (s->captured) return;
    s->captured = true;
    s->context_expected = transport.context;
    s->free_ring = transport.free_ring;
    s->used_ring = transport.used_ring;
    s->status = transport.status;
    s->dma_bytes = transport.bytes;
    s->rx_read = transport.read;
    s->rx_producer = *(volatile uint16_t *)dma_ptr(transport.status);
    const iwx_context_t *c = dma_ptr(transport.context);
    s->context_flags = c->control_flags;
    s->context_mac = c->mac_id;
    s->context_dwords = c->size_dwords;
    s->lmac = c->lmac[0]; s->umac = c->umac[0]; s->paging = c->paging[0];

    const pci_dev_t *d = &transport.device;
    s->pci_id = cfg32(d, 0);
    s->pci_command_status = cfg32(d, 4);
    uint32_t lo = cfg32(d, 0x10);
    uint64_t bar = lo & ~15ULL;
    if ((lo & 6) == 4) bar |= (uint64_t)cfg32(d, 0x14) << 32;
    if (s->pci_id != 0x27238086U || s->pci_command_status == 0xffffffff ||
        !(s->pci_command_status & 2) || (lo & 1) || bar != transport.bar0) {
        s->pci_error = "PCI identity/BAR/memory decode changed; MMIO snapshot skipped";
        return;
    }
    s->pci_valid = true;
    snapshot_pcie(d, s);
    s->interrupt = csr_read(r, IWX_INT);
    s->fh_interrupt = csr_read(r, IWX_FH_INT);
    s->gp_cntrl = csr_read(r, IWX_GP_CNTRL);
    s->reset = csr_read(r, IWX_RESET);
    s->hw_if = csr_read(r, IWX_HW_IF_CONFIG);
    s->int_mask = csr_read(r, IWX_INT_MASK);
    s->context_observed = csr_read(r, IWX_CTXT_INFO_BA);
    s->context_observed |= (uint64_t)csr_read(r, IWX_CTXT_INFO_BA + 4) << 32;
    if (inaccessible(s->interrupt) || inaccessible(s->fh_interrupt) ||
        inaccessible(s->gp_cntrl) || inaccessible(s->reset)) {
        s->prph_error = "CSR inaccessible; peripheral snapshot skipped";
        return;
    }
    s->csr_valid = true;

    /* A separate report keeps optional diagnostic timeouts out of r->error.
     * Access-request is the only control bit touched; restore its prior state. */
    iwx_probe_report_t access = {.bar0 = r->bar0};
    if (progress) progress->mac_held = true;
    bool acquired = csr_modify(&access, IWX_GP_CNTRL, 0, IWX_MAC_ACCESS);
    if (acquired && delay_us(&access, 2) && wait_bits(&access, IWX_GP_CNTRL,
            IWX_CLOCK_READY | IWX_GOING_SLEEP, IWX_CLOCK_READY, 150000)) {
        for (unsigned i = 0; i < IWX_BOOT_PRPH_SAMPLES; i++) {
            s->prph[i].name = registers[i].name;
            s->prph[i].address = registers[i].address;
            s->prph[i].value = prph_read(r, registers[i].address);
            s->prph[i].valid = !inaccessible(s->prph[i].value);
            s->prph_count++;
        }
    }
    s->prph_error = access.error;
    if (acquired && !(s->gp_cntrl & IWX_MAC_ACCESS)) {
        bool released = csr_modify(&access, IWX_GP_CNTRL, IWX_MAC_ACCESS, 0);
        if (released && progress && progress->emit) {
            uint32_t gp = csr_read(r, IWX_GP_CNTRL);
            if (!inaccessible(gp) && !(gp & IWX_MAC_ACCESS)) progress->mac_held = false;
        }
        if (!released && !s->prph_error) s->prph_error = access.error;
    }
}

static void finish_transport(const pci_dev_t *d, iwx_probe_report_t *r, bool mapped,
                             boot_progress_t *progress)
{
    /* Observer failure is independent of the hardware failure and never
     * prevents snapshots, verified stop or retention of DMA-reachable pages. */
    progress_mark(progress, r, "wlan-cleanup");
    if (r->trace_count) trace_checkpoint(d, r, "before-stop", mapped);
    progress_mark(progress, r, "wlan-platform-snapshot");
    dma_diag_capture(&r->platform, 1);
    if (!transport.exposed) {
        progress_mark(progress, r, "wlan-release-unpublished");
        dma_release();
        finish_probe(d, r, mapped);
        if (r->stopped && progress) progress->mac_held = false;
        return;
    }
    const char *error = r->error, *stage = r->stage;
    if (error) {
        progress_mark(progress, r, "wlan-error-snapshot");
        snapshot_transport(r, progress);
    }
    progress_mark(progress, r, "wlan-dma-stop");
    r->error = NULL;
    r->stopped = stop_transport(r);
    r->stop_error = r->stopped ? NULL : r->error;
    if (r->stopped) {
        if (progress) progress->mac_held = false;
        progress_mark(progress, r, "wlan-dma-release");
        dma_release();
    } else {
        r->retained_bytes = transport.bytes;
        __atomic_store_n(&retained_bytes, transport.bytes, __ATOMIC_RELEASE);
    }
    if (error) r->error = error;
    if (error || r->stopped) r->stage = stage;
}

static void runtime_snapshot(const iwx_probe_report_t *r)
{
    runtime.status.rx_buffers = transport.completed;
    runtime.status.rx_reposted = transport.reposted;
    runtime.status.rx_read = transport.read;
    runtime.status.rx_write = transport.published;
    runtime.status.rx_outstanding = transport.outstanding;
    runtime.status.rx_pending = (transport.free_write - transport.published) & (IWX_RX_COUNT - 1);
    runtime.status.rx_packets = r->rx_packets;
    if (transport.nvm) runtime.status.nvm = *transport.nvm;
}

static int fw_run(const void *data, uint32_t size, iwx_probe_report_t *r,
                   iwx_nvm_report_t *nvm, iwx_progress_fn observer, void *arg,
                   bool keep_active)
{
    if (nvm) memset(nvm, 0, sizeof(*nvm));
    if (!r || !claim(r)) return -1;
    transport.nvm = nvm;
    transport.streaming = keep_active;
    if (keep_active) {
        runtime.status = (iwx_status_t){.generation = runtime.status.generation + 1};
        memset(&runtime.profile, 0, sizeof(runtime.profile));
        memset(&runtime.scan, 0, sizeof(runtime.scan));
        memset(runtime.bss, 0, sizeof(runtime.bss));
        memset(&runtime.link, 0, sizeof(runtime.link));
        security_reset();
        runtime.regulatory_revision = 0;
        runtime.mcc_pending = false;
    }
    boot_progress_t progress = {.emit = observer, .arg = arg};
    bool mapped = false;
    const pci_dev_t *d = pci_get_wlan();
    iwx_fw_plan_t plan;
    if (keep_active && !sched_is_enabled()) {
        r->error = "persistent WLAN requires the scheduler";
        goto out;
    }
    r->stage = "firmware validation";
    if (!progress_mark(&progress, r, "wlan-fw-validate")) goto out;
    r->error = iwx_fw_plan(data, size, &plan);
    if (r->error) goto out;
    r->alive_version = plan.alive_version;
    if (nvm) {
        nvm->attempted = true;
        nvm->response_version = plan.nvm_version;
        if (plan.nvm_get_version != 1 || plan.nvm_complete_version != 1 ||
            plan.nvm_version != (plan.nvm_v4 ? 4 : 3)) {
            r->error = "unsupported/missing NVM command/API versions";
            goto out;
        }
    }
    if (!progress_mark(&progress, r, "wlan-platform-prepare")) goto out;
    dma_diag_prepare(d, &r->platform);
    trace_checkpoint(d, r, "before-prepare", false);
    if (!progress_mark(&progress, r, "wlan-card-prepare")) goto out;
    if (!start_card(d, r, &mapped, &progress)) goto out;
    trace_checkpoint(d, r, "after-prepare", mapped);
    if (!progress_mark(&progress, r, "wlan-card-ready")) goto out;
    if (r->rfkill) {
        r->stage = "radio switch";
        r->error = "radio switch blocked";
        goto out;
    }
    r->stage = "DMA allocation";
    if (!progress_mark(&progress, r, "wlan-dma-allocate")) goto out;
    if (!alloc_transport(data, &plan, r)) {
        r->error = "cannot allocate mapped DMA pages below 4 GiB";
        goto out;
    }
    trace_checkpoint(d, r, "dma-ready", mapped);
    r->stage = "PCI DMA path preparation";
    if (!progress_mark(&progress, r, "wlan-bridge-enable")) goto out;
    r->error = pci_prepare_dma_path(d, &r->dma_path);
    if (r->error) goto out;
    if (!progress_mark(&progress, r, "wlan-bridge-ready")) goto out;
    r->stage = "firmware context/start";
    if (!progress_mark(&progress, r, "wlan-launch")) goto out;
    if (!launch_firmware(d, &plan, r, &progress)) goto out;
    r->stage = "firmware RFH initialization";
    if (!progress_mark(&progress, r, "wlan-wait-alive")) goto out;
    if (wait_alive(r, &progress)) {
        r->stage = "firmware ALIVE confirmed";
        trace_checkpoint(d, r, "alive-confirmed", mapped);
        if (progress_mark(&progress, r, "wlan-alive") && nvm)
            initialize_nvm(r, nvm, &progress);
    }
    if (keep_active && !r->error && !r->progress_failed_at && nvm->valid) {
        r->stage = "persistent firmware transport (not associated)";
        r->command_after = cfg16(d, 4);
        if (r->command_after == 0xffff || (r->command_after & 0x406) != 0x406) {
            r->error = "PCI DMA ownership lost during firmware initialization";
            goto out;
        }
        if (!progress_mark(&progress, r, "wlan-runtime-ready")) goto out;
        if (!runtime.worker_started) {
            if (sched_spawn("wlan-rx", rx_worker) < 0) {
                r->error = "cannot start WLAN RX worker";
                goto out;
            }
            runtime.worker_started = true;
        }
        runtime_snapshot(r);
        runtime.profile = plan.radio;
        runtime.report = *r;
        transport.nvm = &runtime.status.nvm;
        runtime.status.state = IWX_ACTIVE;
        runtime.active = true;
        __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
        return 0;
    }
out:
    if (keep_active) runtime_snapshot(r);
    finish_transport(d, r, mapped, &progress);
    progress_mark(&progress, r, "wlan-finished");
    if (!r->error && r->progress_failed_at) {
        r->stage = r->progress_failed_at;
        r->error = "persistent progress checkpoint failed";
    }
    /* A quarantined transport may outlive this caller's report storage. */
    transport.nvm = NULL;
    if (keep_active) {
        runtime.report = *r;
        runtime.status.state = transport.count ? IWX_QUARANTINED : IWX_FAILED;
        runtime.status.error = r->error;
        runtime.status.stop_error = r->stop_error;
    }
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return r->error ? -1 : 0;
}

int iwx_fw_boot_traced(const void *data, uint32_t size, iwx_probe_report_t *r,
                       iwx_progress_fn observer, void *arg)
{
    return fw_run(data, size, r, NULL, observer, arg, false);
}

int iwx_fw_boot(const void *data, uint32_t size, iwx_probe_report_t *r)
{
    return iwx_fw_boot_traced(data, size, r, NULL, NULL);
}

int iwx_fw_nvm(const void *data, uint32_t size, iwx_probe_report_t *r,
               iwx_nvm_report_t *nvm, iwx_progress_fn observer, void *arg)
{
    if (!nvm) {
        if (r) { memset(r, 0, sizeof(*r)); r->error = "missing NVM report"; }
        return -1;
    }
    return fw_run(data, size, r, nvm, observer, arg, false);
}

int iwx_start(const void *data, uint32_t size, iwx_probe_report_t *r,
              iwx_nvm_report_t *nvm, iwx_progress_fn observer, void *arg)
{
    if (!nvm) {
        if (r) { memset(r, 0, sizeof(*r)); r->error = "missing NVM report"; }
        return -1;
    }
    return fw_run(data, size, r, nvm, observer, arg, true);
}

int iwx_get_status(iwx_status_t *status)
{
    if (!status) return -1;
    memset(status, 0, sizeof(*status));
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    *status = runtime.status;
    status->worker_started = runtime.worker_started;
    status->dma_bytes = transport.bytes;
    if (!runtime.active && transport.count) status->state = IWX_QUARANTINED;
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return 0;
}

static bool scan_busy(void)
{
    return runtime.scan.state == IWX_SCAN_RUNNING || runtime.scan.state == IWX_SCAN_ABORTING;
}

static bool scan_notification(const uint8_t *h, unsigned payload, iwx_probe_report_t *r)
{
    if (!(h[3] & 0x80)) return true;
    iwx_scan_status_t *s = &runtime.scan;
    const uint8_t *p = h + 4;
    if (h[1] == 0 && h[0] == IWX_MCC_CHANGED) {
        if (payload != 4) {
            r->error = "invalid regulatory change notification";
            return false;
        }
        s->regulatory.valid = false;
        runtime.pending_mcc = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        runtime.pending_mcc_source = p[2];
        runtime.mcc_pending = true;
        runtime.regulatory_revision++;
    } else if (h[1] == 0 && h[0] == IWX_RX_MPDU) {
        if (s->state != IWX_SCAN_RUNNING || !s->regulatory.valid) return true;
        iwx_bss_t b;
        int rc = iwx_scan_decode_mpdu(&b, p, payload, &s->regulatory);
        if (rc > 0) iwx_scan_cache(s, runtime.bss, &b);
        else if (rc < 0) s->malformed++;
        else s->ignored++;
    } else if (h[1] == 0 && h[0] == IWX_SCAN_COMPLETE_NOTIF) {
        if (payload != 16) {
            r->error = "invalid scan completion notification";
            return false;
        }
        s->notifications++;
        if (!scan_busy() || rx_le32(p) != 0) { s->ignored++; return true; }
        s->completion_status = p[6];
        s->ebs_status = p[7];
        if (p[6] != 1 && p[6] != 2) {
            r->error = "unknown scan completion status";
            return false;
        }
        s->state = (p[6] == 1 && !s->error) ? IWX_SCAN_COMPLETE : IWX_SCAN_ABORTED;
    }
    return true;
}

static bool radio_command(uint8_t group, uint8_t opcode, const void *data, unsigned length)
{
    return send_command(&runtime.report, group, opcode, data, length);
}

static bool radio_regulatory(void);

static bool radio_prepare(void)
{
    iwx_probe_report_t *r = &runtime.report;
    const iwx_radio_profile_t *p = &runtime.profile;
    const iwx_nvm_report_t *n = &runtime.status.nvm;
    r->stage = "scan radio configuration";
    uint32_t tx = ((p->phy_config >> 16) & 15) & n->tx_chains;
    uint32_t rx = ((p->phy_config >> 20) & 15) & n->rx_chains;
    uint32_t bt[2] = {3, 0}; /* Wi-Fi coexistence mode, no optional modules. */
    uint32_t soc[2] = {1, 0}; /* Discrete AX200, no integrated crystal delay. */
    uint32_t queue = 0, thresholds[5] = {0}, power = 0;
    if (!radio_command(1, IWX_TX_ANT_CFG, &tx, sizeof(tx)) ||
        !radio_command(1, IWX_BT_CONFIG, bt, sizeof(bt)) ||
        !radio_command(2, IWX_SOC_CONFIG, soc, sizeof(soc))) return false;
    if (iwx_radio_capability(p, IWX_CAPA_DQA) &&
        !radio_command(IWX_DATA_GROUP, IWX_DQA_ENABLE, &queue, sizeof(queue))) return false;
    iwx_boot_snapshot_t pci = {.pci_command_status = cfg32(&transport.device, 4)};
    snapshot_pcie(&transport.device, &pci);
    if (!pci.pcie_valid || pci.pcie_offset > 0xd4) {
        r->error = "cannot read PCIe latency configuration";
        return false;
    }
    if ((pci.pcie_flags & 15) >= 2) {
        uint16_t ctl2 = cfg16(&transport.device, pci.pcie_offset + 0x28);
        if (ctl2 == 0xffff) { r->error = "PCIe latency configuration inaccessible"; return false; }
        if (ctl2 & (1U << 10)) {
            uint32_t ltr[8] = {1};
            if (!radio_command(1, IWX_LTR_CONFIG, ltr, sizeof(ltr))) return false;
        }
    }
    if (!radio_command(4, IWX_TEMP_THRESHOLDS, thresholds, sizeof(thresholds)) ||
        !radio_command(1, IWX_DEVICE_POWER, &power, sizeof(power))) return false;
    if (!radio_regulatory()) return false;
    struct __attribute__((packed)) { uint8_t cam, promiscuous, broadcast, reserved; uint32_t tx, rx; }
        scan_config = {.tx = tx, .rx = rx};
    uint32_t beacon_filter[15] = {0};
    if (!radio_command(1, IWX_SCAN_CONFIG, &scan_config, sizeof(scan_config)) ||
        !radio_command(1, IWX_BEACON_FILTER, beacon_filter, sizeof(beacon_filter))) return false;
    runtime.scan.radio_ready = true;
    return true;
}

static bool radio_regulatory(void)
{
    runtime.report.stage = "query firmware regulatory profile";
    /* GET_CURRENT with ZZ asks for the firmware/NVM profile, never a guessed
     * country. CHUB asks the host to apply its supplied MCC/source; merely
     * rereading the previous country would not acknowledge that transition. */
    struct __attribute__((packed)) { uint16_t mcc; uint8_t source, reserved; uint32_t tail[6]; }
        query = {.mcc = 0x5a5a, .source = 0x10};
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        query.mcc = runtime.mcc_pending ? runtime.pending_mcc : 0x5a5a;
        query.source = runtime.mcc_pending ? runtime.pending_mcc_source : 0x10;
        uint64_t revision = runtime.regulatory_revision;
        if (!radio_command(1, IWX_MCC_UPDATE, &query, sizeof(query))) return false;
        if (revision != runtime.regulatory_revision) continue;
        runtime.report.error = iwx_mcc_decode(&runtime.scan.regulatory, &runtime.status.nvm,
                                               transport.response, transport.response_size);
        if (!runtime.report.error) runtime.mcc_pending = false;
        return runtime.report.error == NULL;
    }
    runtime.report.error = "regulatory profile changed during both queries";
    return false;
}

static void runtime_fail(void)
{
    iwx_probe_report_t *r = &runtime.report;
    if (runtime.link.status.state != IWX_LINK_IDLE) {
        runtime.link.status.state = IWX_LINK_FAILED;
        runtime.link.status.port_open = false;
        if (!runtime.link.status.error) runtime.link.status.error = r->error;
    }
    security_reset();
    runtime_snapshot(r);
    runtime.active = false;
    runtime.scan.radio_ready = runtime.scan.regulatory.valid = false;
    if (scan_busy()) runtime.scan.state = IWX_SCAN_FAILED;
    if (!runtime.scan.error) runtime.scan.error = r->error;
    finish_transport(&transport.device, r, true, NULL);
    transport.nvm = NULL;
    runtime.status.state = transport.count ? IWX_QUARANTINED : IWX_FAILED;
    runtime.status.error = r->error;
    runtime.status.stop_error = r->stop_error;
}

static bool scan_abort_locked(const char *reason)
{
    runtime.scan.state = IWX_SCAN_ABORTING;
    runtime.scan.error = reason;
    runtime.scan_started = idt_get_monotonic_ns();
    uint32_t abort[2] = {0}; /* UID zero, flags reserved. */
    if (!radio_command(1, IWX_SCAN_ABORT, abort, sizeof(abort))) return false;
    if (transport.response_size != 4 || rx_le32(transport.response) > 2) {
        runtime.report.error = "invalid scan abort response";
        return false;
    }
    /* NOT_FOUND means the firmware no longer owns this slot. Any preceding
     * completion in the same RX batch has already been consumed. */
    if (rx_le32(transport.response) == 2 && scan_busy()) runtime.scan.state = IWX_SCAN_ABORTED;
    return true;
}

const char *iwx_scan_start(void)
{
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return "WLAN transport busy";
    const char *err = NULL;
    if (!runtime.active) err = "run wlan start <firmware> before scanning";
    else if (scan_busy()) err = "scan already in progress";
    else if (runtime.link.status.state != IWX_LINK_IDLE) err = "stop/restart WLAN before scanning another link";
    else err = iwx_scan_profile_check(&runtime.profile, &runtime.status.nvm);
    if (err) goto out;
    /* Drain completed traffic before reusing the single firmware scan slot. */
    if (!poll_rx(&runtime.report, NULL, false, NULL, IWX_RX_COUNT)) goto failed;
    runtime.scan.error = NULL;
    if (!runtime.scan.radio_ready) {
        if (!radio_prepare()) goto failed;
    } else if (!radio_regulatory()) goto failed;
    iwx_scan_request_t request;
    err = iwx_scan_build(&request, &runtime.scan.regulatory, &runtime.status.nvm);
    if (err) goto out;
    iwx_regulatory_t reg = runtime.scan.regulatory;
    uint64_t generation = runtime.scan.generation + 1;
    runtime.scan = (iwx_scan_status_t){.radio_ready = true, .regulatory = reg,
                                      .generation = generation, .state = IWX_SCAN_RUNNING};
    memset(runtime.bss, 0, sizeof(runtime.bss));
    runtime.scan_started = idt_get_monotonic_ns();
    runtime.report.stage = "passive scan request";
    if (!radio_command(1, IWX_SCAN_REQUEST, &request, sizeof(request))) goto failed;
    runtime_snapshot(&runtime.report);
    goto out;
failed:
    err = runtime.report.error;
    runtime.scan.state = IWX_SCAN_FAILED;
    runtime_fail();
out:
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return err;
}

const char *iwx_scan_abort(void)
{
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return "WLAN transport busy";
    const char *err = NULL;
    if (!runtime.active || !scan_busy()) err = "no scan in progress";
    else if (runtime.scan.state == IWX_SCAN_RUNNING && !scan_abort_locked("scan cancelled")) {
        err = runtime.report.error;
        runtime_fail();
    }
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return err;
}

int iwx_scan_results(iwx_scan_status_t *status, iwx_bss_t *bss, unsigned capacity)
{
    if (!status || (capacity && !bss)) return -1;
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    *status = runtime.scan;
    unsigned count = capacity < runtime.scan.count ? capacity : runtime.scan.count;
    if (count) memcpy(bss, runtime.bss, count * sizeof(*bss));
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return count;
}

static void link_diag_increment(unsigned *count)
{
    if (*count != (unsigned)-1) ++*count;
}

static void link_rx_trace(iwx_link_status_t *s, iwx_rx_sample_t *d)
{
    if ((unsigned)d->reason < IWX_RX_REASON_COUNT) link_diag_increment(&s->rx_reasons[d->reason]);
    if (s->port_open && (d->frame_control & 0xff) == 8 && (d->addresses & 2) &&
        (d->reason != IWX_RX_ACCEPTED || (d->addresses & 32) ||
         (d->udp_source == 67 && d->udp_destination == 68) ||
         (d->udp_source == 68 && d->udp_destination == 67))) {
        /* Preserve the first data-plane failures/bootstrap frames independently
         * of handshake evidence. Normal traffic cannot evict either trace. */
        if (s->data_trace_count < IWX_LINK_TRACE_MAX) {
            d->time_ns = idt_get_monotonic_ns();
            s->data_trace[s->data_trace_count++] = *d;
        } else link_diag_increment(&s->data_trace_skipped);
    }
    /* Keep only data from the selected AP while the controlled port is closed.
     * Beacons and unrelated traffic cannot evict handshake evidence. */
    if (s->port_open || (d->frame_control & 0x0c) != 8 || !(d->addresses & 2)) return;
    d->time_ns = idt_get_monotonic_ns();
    s->rx_trace[s->rx_trace_next] = *d;
    s->rx_trace_next = (s->rx_trace_next + 1) % IWX_LINK_TRACE_MAX;
    if (s->rx_trace_count < IWX_LINK_TRACE_MAX) s->rx_trace_count++;
    else link_diag_increment(&s->rx_trace_overwritten);
}

static bool link_notification(const uint8_t *h, unsigned size, iwx_probe_report_t *r, uint32_t flags)
{
    iwx_link_t *l = &runtime.link;
    const uint8_t *p = h + 4;
    if (h[0] == IWX_TX_FRAME && h[1] == 0) {
        /* TX_RSP v7: one non-aggregated frame plus the scheduler read cursor.
         * This is not a command ACK. No byte becomes reusable on a bad cursor. */
        if (!transport.tx_enabled || !transport.tx_pending ||
            h[3] != (transport.tx_queue & IWX_TX_SEQUENCE_QUEUE_MASK) ||
            h[2] != transport.tx_index || size != 48 || p[0] != 1 ||
            rx_le16(p + 36) != transport.tx_queue ||
            (rx_le32(p + 44) & 255) != ((transport.tx_index + 1) & 255)) {
            r->error = "invalid/stale link TX completion";
            return false;
        }
        unsigned status = p[40];
        if (transport.tx_data && status != 2)
            iwx_link_rate_feedback(l, status == 1, p[3]);
        if (transport.tx_wpa_token && l->status.eapol_tx_count) {
            unsigned i = (l->status.eapol_tx_next + IWX_LINK_TRACE_MAX - 1) % IWX_LINK_TRACE_MAX;
            iwx_tx_sample_t *d = &l->status.eapol_tx[i];
            d->completed = true; d->completed_ns = idt_get_monotonic_ns();
            d->status = rx_le16(p + 40); d->completion_rate = rx_le32(p + 4);
            d->bt_kills = p[1]; d->rts_failures = p[2]; d->ack_failures = p[3];
        }
        l->status.tx_completed++;
        transport.tx_pending = false;
        transport.tx_index = (transport.tx_index + 1) & 255;
        if (transport.tx_wpa_token) {
            int rc = wpa2_tx_complete(&runtime.wpa, transport.tx_wpa_token, status == 1 || status == 2);
            transport.tx_wpa_token = 0;
            runtime.link.status.port_open = runtime.wpa.authorized;
            if (rc) { r->error = "invalid/failed EAPOL TX completion"; return false; }
            if (runtime.wpa.authorized)
                __atomic_store_n(&runtime.network_up, true, __ATOMIC_RELEASE);
        }
        if (status != 1 && status != 2) {
            l->status.tx_failed++;
            if (transport.tx_data) return true;
            r->error = "management frame was not acknowledged";
            return false;
        }
    } else if (h[0] == IWX_SESSION_NOTIFICATION && h[1] == IWX_MAC_GROUP) {
        if (!(h[3] & 0x80) || size != 16 || rx_le32(p + 4) > 1 || rx_le32(p + 8) > 1) {
            r->error = "invalid session protection notification";
            return false;
        }
        if (rx_le32(p) || rx_le32(p + 12) || l->status.state < IWX_LINK_SESSION ||
            l->status.state > IWX_LINK_ASSOCIATED) return true;
        if (!rx_le32(p + 4)) {
            r->error = "firmware could not protect association session";
            return false;
        }
        if (rx_le32(p + 8)) l->session_started = true;
        else l->session_ended = true;
    } else if (h[0] == IWX_RX_MPDU && h[1] == 0 &&
               l->status.state >= IWX_LINK_AUTH && l->status.state <= IWX_LINK_ASSOCIATED) {
        if (!(h[3] & 0x80)) { r->error = "link MPDU missing firmware-origin flag"; return false; }
        enum iwx_link_state before = l->status.state;
        int rc = iwx_link_receive(l, p, size);
        if (rc < 0) l->status.rx_malformed++;
        else if (!rc) l->status.rx_ignored++;
        if (before != l->status.state) l->deadline = idt_get_monotonic_ns() + 1000000000ULL;
        if (runtime.connect_requested && l->status.state >= IWX_LINK_CONFIGURE &&
            l->status.state <= IWX_LINK_ASSOCIATED) {
            iwx_data_rx_t data;
            iwx_rx_sample_t diagnostic;
            int received = iwx_data_receive_diag(&runtime.data, l, p, size, &data, &diagnostic, flags);
            link_rx_trace(&l->status, &diagnostic);
            if (diagnostic.reason == IWX_RX_MIC && wpa2_michael_failure(&runtime.wpa, &michael,
                                                                          idt_get_monotonic_ns()) == 2) {
                l->status.port_open = false;
                __atomic_store_n(&runtime.network_up, false, __ATOMIC_RELEASE);
            }
            if (received > 0 && data.eapol && runtime.wpa.state != WPA2_OFF) {
                unsigned length = data.length - 14;
                if (length <= WPA2_EAPOL_MAX && runtime.eapol_count < 4) {
                    unsigned i = (runtime.eapol_read + runtime.eapol_count++) & 3;
                    runtime.eapol[i].size = length;
                    memcpy(runtime.eapol[i].data, data.ethernet + 14, length);
                } else {
                    l->status.eapol_dropped++;
                    l->status.eapol_ingress_dropped++;
                }
            } else if (received > 0 && !data.eapol && runtime.network_up) {
                if (runtime.rx_count < 16) {
                    unsigned i = (runtime.rx_read + runtime.rx_count++) & 15;
                    runtime.rx_data[i].size = data.length;
                    memcpy(runtime.rx_data[i].data, data.ethernet, data.length);
                    l->status.data_rx++;
                } else l->status.data_rx_dropped++;
            }
        }
    }
    return true;
}

static bool link_prepare(void)
{
    iwx_link_t *l = &runtime.link;
    iwx_probe_report_t *r = &runtime.report;
    transport.tx_descriptors = dma_alloc(256 * sizeof(iwx_tfh_tfd_t));
    transport.tx_byte_count = dma_alloc(4096);
    transport.tx_page = dma_alloc(4096);
    if (!transport.tx_descriptors || !transport.tx_byte_count || !transport.tx_page) {
        r->error = "cannot allocate management TX DMA";
        return false;
    }
    /* One firmware MAC/PHY/station, legacy 20 MHz and SISO. No HT/QoS or
     * aggregation is advertised to the AP before those paths exist. */
    iwx_link_phy_t phy = {.action = 1, .channel = l->ap.channel, .band = l->ap.channel <= 14};
    r->stage = "association PHY context";
    if (!radio_command(1, IWX_PHY_CONTEXT, &phy, sizeof(phy))) return false;
    unsigned rx = ((runtime.profile.phy_config >> 20) & 3) & runtime.status.nvm.rx_chains;
    uint32_t rlc[8] = {0, (rx << 1) | (1U << 10) | (1U << 12)};
    r->stage = "association receive chains";
    if (!send_command_version(r, 5, IWX_RLC_CONFIG, rlc, sizeof(rlc), 2)) return false;
    iwx_link_mac_t mac;
    iwx_link_mac_build(&mac, l, false);
    r->stage = "association MAC context";
    if (!radio_command(1, IWX_MAC_CONTEXT, &mac, sizeof(mac))) return false;
    iwx_link_binding_t binding = {.action = 1, .macs = {0, UINT32_MAX, UINT32_MAX}};
    r->stage = "association MAC/PHY binding";
    if (!radio_command(1, IWX_BINDING_CONTEXT, &binding, sizeof(binding))) return false;
    if (transport.response_size != 4 || rx_le32(transport.response) != 0) {
        r->error = "firmware rejected MAC/PHY binding"; return false;
    }
    iwx_link_station_t station = {.mask = (3U << 26) | (3U << 28)};
    memcpy(station.address, l->ap.bssid, 6);
    r->stage = "association AP station";
    if (!radio_command(1, IWX_ADD_STATION, &station, sizeof(station))) return false;
    if (transport.response_size != 4 || rx_le32(transport.response) != 1) {
        r->error = "firmware rejected AP station"; return false;
    }
    iwx_link_queue_t queue = {.sta_mask = 1, .tid = 15, .cb_size = 5,
        .byte_count = transport.tx_byte_count, .descriptors = transport.tx_descriptors};
    r->stage = "association management TX queue";
    if (!radio_command(5, IWX_SCD_QUEUE_CONFIG, &queue, sizeof(queue))) return false;
    l->status.queue_response = true;
    l->status.queue_bytes = transport.response_size;
    if (transport.response_size != 8) {
        r->error = "invalid management queue allocation response size"; return false;
    }
    l->status.queue_id = rx_le16(transport.response);
    l->status.queue_flags = rx_le16(transport.response + 2);
    l->status.queue_write_pointer = rx_le16(transport.response + 4);
    l->status.queue_reserved = rx_le16(transport.response + 6);
    /* Match Intel's gen2 allocator: retain payload flags for diagnostics,
     * not as command status. command_response() already checked CMD_FAILED
     * in the header; the P1 returns payload flags=1 with an assigned queue. */
    if (!l->status.queue_id || l->status.queue_id >= IWX_TX_QUEUE_COUNT) {
        r->error = "invalid or command-owned management queue ID"; return false;
    }
    /* TVQM assigns the full queue ID and initial scheduler sequence. The
     * 256-entry DMA ring uses its low eight bits, including on TX completion. */
    transport.tx_queue = l->status.queue_id;
    transport.tx_index = l->status.queue_write_pointer & 255;
    transport.tx_enabled = true;
    if (!iwx_link_tx_allowed(&runtime.scan.regulatory, l->ap.channel)) {
        r->error = "regulatory permission changed during association setup"; return false;
    }
    l->status.state = IWX_LINK_SESSION;
    l->deadline = idt_get_monotonic_ns() + 1000000000ULL;
    uint32_t session[6] = {0, 1, 0, 3000};
    r->stage = "association session protection";
    return radio_command(IWX_MAC_GROUP, IWX_SESSION_PROTECTION, session, sizeof(session));
}

static bool link_transmit(const uint8_t *frame, unsigned length, uint32_t wpa_token, bool data)
{
    iwx_link_t *l = &runtime.link;
    iwx_probe_report_t *r = &runtime.report;
    if (!transport.tx_enabled || transport.tx_pending) { r->error = "link TX queue unavailable"; return false; }
    iwx_tfh_tfd_t *t = (iwx_tfh_tfd_t *)dma_ptr(transport.tx_descriptors) + transport.tx_index;
    uint16_t bc;
    uint32_t rate = data ? iwx_link_data_rate(l) : l->rate_flags;
    r->error = iwx_link_tx_build(t, dma_ptr(transport.tx_page), transport.tx_page,
                               transport.tx_queue, transport.tx_index, rate, frame, length, &bc);
    if (r->error) return false;
    uint16_t *counts = dma_ptr(transport.tx_byte_count);
    counts[transport.tx_index] = bc;
    if (transport.tx_index < 64) counts[transport.tx_index + 256] = bc;
    transport.tx_pending = true;
    transport.tx_wpa_token = wpa_token;
    transport.tx_data = data;
    transport.tx_started = idt_get_monotonic_ns();
    if (wpa_token) {
        const uint8_t *page = dma_ptr(transport.tx_page);
        iwx_link_status_t *s = &l->status;
        iwx_tx_sample_t *d = &s->eapol_tx[s->eapol_tx_next];
        memset(d, 0, sizeof(*d));
        d->submitted_ns = transport.tx_started;
        d->queue = transport.tx_queue; d->index = transport.tx_index;
        d->kind = runtime.wpa.inflight_kind;
        d->frame_bytes = rx_le16(page + 4); d->byte_count = bc;
        d->command_flags = rx_le32(page + 8); d->rate = rx_le32(page + 20);
        d->frame_control = rx_le16(page + 24);
        for (unsigned i = 0; i < 3; i++) d->tb_bytes[i] = t->tb[i].length;
        d->dma_match = t->num_tbs == 3 && t->tb[0].length == 20 && t->tb[1].length == 28 &&
            t->tb[2].length == length - 24 && !memcmp(page + 24, frame, 24) &&
            !memcmp(page + 64, frame + 24, length - 24);
        if (length >= 39) { d->version = page[72]; d->key_info = ((uint16_t)page[77] << 8) | page[78]; }
        s->eapol_tx_next = (s->eapol_tx_next + 1) % IWX_LINK_TRACE_MAX;
        if (s->eapol_tx_count < IWX_LINK_TRACE_MAX) s->eapol_tx_count++;
        else link_diag_increment(&s->eapol_tx_overwritten);
    }
    l->status.tx_sent++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    csr_write(r, IWX_HBUS_TARG_WRPTR, (transport.tx_queue << 16) | ((transport.tx_index + 1) & 255));
    return true;
}

static bool security_key(unsigned slot, enum wpa2_cipher cipher, const uint8_t *key, unsigned key_len)
{
    iwx_key_cmd_t cmd;
    iwx_probe_report_t *r = &runtime.report;
    if (iwx_key_build(&cmd, slot, cipher, key, key_len)) { r->error = "invalid WLAN key/slot"; return false; }
    r->stage = cipher == WPA2_CIPHER_TKIP ? "install TKIP group key" : "install CCMP key";
    bool ok = radio_command(1, IWX_ADD_STA_KEY, &cmd, sizeof(cmd));
    wpa2_clear(&cmd, sizeof(cmd));
    if (!ok) return false;
    /* The exact command ACK returned ownership of this DMA buffer. */
    wpa2_clear(dma_ptr(transport.command_data), 4096);
    if (transport.response_size != 4 || rx_le32(transport.response) != 1) {
        r->error = "firmware rejected WLAN key"; return false;
    }
    return true;
}

static bool security_pump(void)
{
    wpa2_t *s = &runtime.wpa;
    iwx_link_t *l = &runtime.link;
    iwx_probe_report_t *r = &runtime.report;
    if (s->state == WPA2_OFF) return true;
    /* RX callbacks only copy bounded EAPOL frames. Crypto and key commands run
     * here, never recursively inside a command's response-draining loop. */
    for (unsigned budget = 0; budget < 4 && runtime.eapol_count; budget++) {
        unsigned i = runtime.eapol_read;
        int rc = wpa2_receive(s, l->ap.bssid, runtime.eapol[i].data,
                             runtime.eapol[i].size, idt_get_monotonic_ns());
        wpa2_clear(&runtime.eapol[i], sizeof(runtime.eapol[i]));
        runtime.eapol_read = (i + 1) & 3;
        runtime.eapol_count--;
        if (rc < 0) goto failed;
        l->status.pmkid_mismatches = s->pmkid_mismatches;
        if (rc) l->status.eapol_received++;
        else {
            l->status.eapol_dropped++;
            l->status.eapol_last_drop = s->last_drop;
        }
    }
    if (wpa2_tick(s, idt_get_monotonic_ns())) goto failed;
    l->status.port_open = s->authorized;
    if (transport.tx_pending) return true;
    if (s->countermeasures && !s->mic_reports && !s->inflight_tx) {
        if (runtime.tkip_deauth_sent) {
            r->error = "TKIP MIC countermeasures: association blocked for 60 seconds";
            return false;
        }
        uint8_t frame[26] = {0xc0};
        memcpy(frame + 4, l->ap.bssid, 6); memcpy(frame + 10, l->local, 6);
        memcpy(frame + 16, l->ap.bssid, 6);
        uint16_t seq = (l->sequence++ & 4095) << 4;
        frame[22] = seq; frame[23] = seq >> 8; frame[24] = 14;
        runtime.tkip_deauth_sent = true;
        return link_transmit(frame, sizeof(frame), 0, false);
    }
    if (s->install_pending && !s->countermeasures) {
        wpa2_keys_t keys = s->keys;
        bool ok = (!keys.set_pairwise || security_key(0, WPA2_CIPHER_CCMP, keys.pairwise, 16)) &&
                  (!keys.set_group || security_key(keys.group_id + 1, keys.group_cipher, keys.group,
                                                   keys.group_cipher == WPA2_CIPHER_TKIP ? 32 : 16));
        /* Commands can drain disconnect/regulatory events as well as ACKs. */
        ok = ok && l->status.state == IWX_LINK_ASSOCIATED &&
             iwx_link_tx_allowed(&runtime.scan.regulatory, l->ap.channel);
        if (ok) {
            static const uint8_t zero[6] = {0};
            if (keys.set_pairwise) ok = iwx_data_key_commit(&runtime.data, 0, keys.generation, WPA2_CIPHER_CCMP, zero) == 1;
            if (ok && keys.set_group)
                ok = iwx_data_key_commit(&runtime.data, keys.group_id + 1, keys.generation, keys.group_cipher, keys.rsc) == 1;
        }
        int rc = wpa2_keys_installed(s, keys.generation, ok);
        wpa2_clear(&keys, sizeof(keys));
        if (rc) goto failed;
        l->status.key_installs++;
    }
    uint8_t ethernet[14 + 121], frame[14 + 121 + 18];
    uint32_t token = 0;
    bool protect;
    int n = wpa2_take_tx(s, ethernet + 14, sizeof(ethernet) - 14, &token, &protect, idt_get_monotonic_ns());
    if (n < 0) goto failed;
    if (!n) return true;
    memcpy(ethernet, l->ap.bssid, 6); memcpy(ethernet + 6, l->local, 6);
    ethernet[12] = 0x88; ethernet[13] = 0x8e;
    n = iwx_data_frame(&runtime.data, l, ethernet, n + 14, protect, frame, sizeof(frame));
    bool sent = n > 0 && link_transmit(frame, n, token, false);
    wpa2_clear(ethernet, sizeof(ethernet)); wpa2_clear(frame, sizeof(frame));
    if (!sent) { if (!r->error) r->error = "cannot transmit EAPOL frame"; return false; }
    return true;
failed:
    l->status.port_open = false;
    if (!r->error) r->error = s->error ? s->error : "WPA2 state transition failed";
    return false;
}

static bool data_pump(void)
{
    if (transport.tx_pending || !runtime.link.status.port_open || !runtime.tx_count) return true;
    unsigned i = runtime.tx_read;
    uint8_t frame[IWX_DATA_MAX];
    int n = iwx_data_frame(&runtime.data, &runtime.link, runtime.tx_data[i].data,
                           runtime.tx_data[i].size, runtime.link.status.rsn, frame, sizeof(frame));
    bool ok = n > 0 && link_transmit(frame, n, 0, true);
    wpa2_clear(frame, sizeof(frame));
    if (!ok) {
        if (!runtime.report.error) runtime.report.error = "cannot encode Ethernet frame";
        return false;
    }
    wpa2_clear(&runtime.tx_data[i], sizeof(runtime.tx_data[i]));
    runtime.tx_read = (i + 1) & 15;
    runtime.tx_count--;
    return true;
}

static bool link_pump(void)
{
    iwx_link_t *l = &runtime.link;
    iwx_probe_report_t *r = &runtime.report;
    if (l->status.state == IWX_LINK_IDLE || l->status.state == IWX_LINK_PREPARING) return true;
    if (l->status.state == IWX_LINK_FAILED) { r->error = l->status.error; return false; }
    if (!iwx_link_tx_allowed(&runtime.scan.regulatory, l->ap.channel)) {
        r->error = "regulatory permission changed while linked"; return false;
    }
    uint64_t now = idt_get_monotonic_ns();
    if (transport.tx_pending && now - transport.tx_started >= 1000000000ULL) {
        r->error = "management TX completion timed out"; return false;
    }
    if (l->status.state == IWX_LINK_ASSOCIATED) return security_pump() && data_pump();
    if (now >= l->deadline || (l->session_ended && l->status.state < IWX_LINK_CONFIGURE)) {
        r->error = "802.11 association/session timed out"; return false;
    }
    if (l->status.state == IWX_LINK_SESSION && l->session_started) {
        l->status.state = IWX_LINK_AUTH;
        l->deadline = now + 1000000000ULL;
    }
    if (transport.tx_pending) return true;
    if (l->status.state == IWX_LINK_CONFIGURE) {
        iwx_link_mac_t mac;
        iwx_link_mac_build(&mac, l, true);
        r->stage = "record AP association (data port closed)";
        if (!radio_command(1, IWX_MAC_CONTEXT, &mac, sizeof(mac))) return false;
        if (l->status.state == IWX_LINK_FAILED) { r->error = l->status.error; return false; }
        if (!iwx_link_tx_allowed(&runtime.scan.regulatory, l->ap.channel)) {
            r->error = "regulatory permission changed during association commit"; return false;
        }
        l->status.state = IWX_LINK_ASSOCIATED;
        if (runtime.connect_requested && !l->status.rsn) {
            l->status.port_open = true;
            __atomic_store_n(&runtime.network_up, true, __ATOMIC_RELEASE);
        }
        return true;
    }
    if ((l->status.state != IWX_LINK_AUTH && l->status.state != IWX_LINK_ASSOC) || l->request_sent) return true;
    uint8_t frame[256];
    unsigned len = iwx_link_request(l, frame);
    if (!len || !transport.tx_enabled) { r->error = "invalid management transmit state"; return false; }
    /* One in-flight management buffer. Reuse requires an exact TX completion,
     * never the command queue ACK nor an over-the-air AP response. */
    return link_transmit(frame, len, 0, false);
}

static const char *associate(const uint8_t bssid[6], bool rsn, const uint8_t *pmk,
                             bool connect, const iwx_bss_t *expected)
{
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return "WLAN transport busy";
    const char *err = NULL;
    if (!runtime.active) err = "run wlan start and wlan scan before association";
    else if (!bssid) err = "missing AP BSSID";
    else if (scan_busy()) err = "wait for scan completion before association";
    else if (runtime.link.status.state != IWX_LINK_IDLE) err = "stop/restart WLAN before another association";
    else if (idt_get_monotonic_ns() < michael.blocked_until) err = "TKIP MIC countermeasures: wait 60 seconds before association";
    else err = iwx_link_profile_check(&runtime.profile);
    if (!err && pmk) err = iwx_key_profile_check(&runtime.profile);
    if (err) goto out;
    const iwx_bss_t *ap = NULL;
    for (unsigned i = 0; i < runtime.scan.count; i++) if (!memcmp(runtime.bss[i].bssid, bssid, 6)) {
        if (ap) { err = "ambiguous BSSID on multiple channels; rescan"; goto out; }
        ap = &runtime.bss[i];
    }
    if (!ap || !runtime.scan.radio_ready) { err = "BSSID not found in configured radio scan"; goto out; }
    if (rsn) {
        enum wpa2_cipher group;
        err = wpa2_rsn_select(ap->rsn_ie, ap->rsn_len, &group);
        if (err) goto out;
        if (group == WPA2_CIPHER_TKIP && (!iwx_radio_api(&runtime.profile, 29) ||
                                         !iwx_radio_api(&runtime.profile, 41))) {
            err = "TKIP group requires firmware MIC keys and deprecated TTAK API";
            goto out;
        }
    }
    if (expected && (ap->channel != expected->channel || ap->ssid_len != expected->ssid_len ||
        memcmp(ap->ssid, expected->ssid, ap->ssid_len) || ap->rsn != expected->rsn ||
        ap->rsn_len != expected->rsn_len || memcmp(ap->rsn_ie, expected->rsn_ie, ap->rsn_len))) {
        err = "network observation changed; scan and retry";
        goto out;
    }
    if (!poll_rx(&runtime.report, NULL, false, NULL, IWX_RX_COUNT) || !radio_regulatory()) goto failed;
    err = iwx_link_select(&runtime.link, ap, &runtime.status.nvm, &runtime.profile,
                           &runtime.scan.regulatory, rsn);
    if (err) goto out;
    if (pmk && wpa2_start(&runtime.wpa, pmk, runtime.link.local, ap->bssid,
                          ap->rsn_ie, ap->rsn_len, idt_get_monotonic_ns())) {
        err = runtime.wpa.error ? runtime.wpa.error : "cannot initialize WPA2";
        security_reset();
        memset(&runtime.link, 0, sizeof(runtime.link));
        goto out;
    }
    runtime.link.status.generation = runtime.status.generation;
    runtime.connect_requested = connect;
    if (!link_prepare() || !link_pump()) goto failed;
    runtime_snapshot(&runtime.report);
    goto out;
failed:
    err = runtime.report.error;
    runtime_fail();
out:
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return err;
}

const char *iwx_associate(const uint8_t bssid[6], bool rsn)
{
    return associate(bssid, rsn, NULL, false, NULL);
}

const char *iwx_connect_psk(const uint8_t bssid[6], const uint8_t pmk[32])
{
    if (!pmk) return "missing WPA2 PMK";
    return associate(bssid, true, pmk, true, NULL);
}

const char *iwx_connect_open(const uint8_t bssid[6])
{
    return associate(bssid, false, NULL, true, NULL);
}

const char *iwx_connect_network(const iwx_bss_t *network, const uint8_t *pmk)
{
    if (!network || network->ssid_len > sizeof(network->ssid)) return "invalid network snapshot";
    return associate(network->bssid, pmk != NULL, pmk, true, network);
}

int iwx_send(const void *data, uint32_t len)
{
    if (!data || len < 14 || len > IWX_ETH_MAX ||
        __atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    const uint8_t *p = data;
    int rc = -1;
    if (runtime.network_up && runtime.link.status.port_open && runtime.tx_count < 16 &&
        !memcmp(p + 6, runtime.link.local, 6) && p[12] >= 6 &&
        !(p[12] == 0x88 && p[13] == 0x8e)) {
        unsigned i = (runtime.tx_read + runtime.tx_count++) & 15;
        runtime.tx_data[i].size = len;
        memcpy(runtime.tx_data[i].data, p, len);
        runtime.link.status.data_tx++;
        rc = 0;
    } else runtime.link.status.data_tx_dropped++;
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return rc;
}

int iwx_recv(void *buf, uint32_t *len)
{
    if (!buf || !len || __atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    int rc = -1;
    if (runtime.network_up && runtime.link.status.port_open && runtime.rx_count) {
        unsigned i = runtime.rx_read;
        *len = runtime.rx_data[i].size;
        memcpy(buf, runtime.rx_data[i].data, *len);
        wpa2_clear(&runtime.rx_data[i], sizeof(runtime.rx_data[i]));
        runtime.rx_read = (i + 1) & 15;
        runtime.rx_count--;
        rc = 0;
    }
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return rc;
}

int iwx_read_link(nic_link_state_t *state)
{
    if (!state || __atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    *state = (nic_link_state_t){.generation = runtime.status.generation, .up = runtime.network_up};
    if (runtime.status.nvm.valid) memcpy(state->mac, runtime.status.nvm.mac, 6);
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return 0;
}

void iwx_get_mac(uint8_t mac[6])
{
    nic_link_state_t state;
    memset(mac, 0, 6);
    if (!iwx_read_link(&state)) memcpy(mac, state.mac, 6);
}

bool iwx_link_up(void)
{
    return __atomic_load_n(&runtime.network_up, __ATOMIC_ACQUIRE);
}

int iwx_link_status(iwx_link_status_t *status)
{
    if (!status || __atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    *status = runtime.link.status;
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return 0;
}

int iwx_poll(void)
{
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return 0;
    int result = 0;
    if (runtime.active) {
        uint64_t before = transport.completed;
        iwx_probe_report_t *r = &runtime.report;
        runtime.status.polls++;
        bool ok = !r->error && poll_rx(r, NULL, false, NULL, 64);
        if (ok && scan_busy()) {
            uint64_t elapsed = idt_get_monotonic_ns() - runtime.scan_started;
            if (runtime.scan.state == IWX_SCAN_RUNNING &&
                (!runtime.scan.regulatory.valid || elapsed >= 30000000000ULL)) {
                ok = scan_abort_locked(runtime.scan.regulatory.valid ?
                    "scan timed out" : "regulatory profile changed during scan");
            } else if (runtime.scan.state == IWX_SCAN_ABORTING && elapsed >= 2000000000ULL) {
                r->error = "scan abort completion timed out";
                ok = false;
            }
        }
        if (ok) ok = link_pump();
        runtime_snapshot(r);
        if (ok) result = (int)(transport.completed - before);
        else {
            r->stage = "runtime RX failure";
            runtime_fail();
            result = -1;
        }
    }
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return result;
}

static void rx_worker(void)
{
    for (;;) {
        /* One outstanding DMA frame must not imply one frame per timer tick.
         * Poll completions briefly under load; keep every burst preemptible and
         * bounded even if firmware stalls. No register polling in IRQ context. */
        uint64_t deadline = idt_get_monotonic_ns() + 1000000;
        bool busy = false;
        do {
            iwx_poll();
            if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) break;
            busy = runtime.active && (transport.tx_pending || runtime.tx_count);
            __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
            if (!busy) break;
            __asm__ volatile ("pause" ::: "memory");
        } while (idt_get_monotonic_ns() < deadline);
        if (busy) sched_yield();
        else if (sched_sleep_ticks(1) < 0) sched_yield();
    }
}

uint64_t iwx_retained_bytes(void)
{
    return __atomic_load_n(&retained_bytes, __ATOMIC_ACQUIRE);
}

static int stop_locked(iwx_probe_report_t *r)
{
    if (runtime.active) runtime_snapshot(&runtime.report);
    if (scan_busy()) {
        runtime.scan.state = IWX_SCAN_ABORTED;
        runtime.scan.error = "WLAN stopped";
    }
    runtime.scan.radio_ready = runtime.scan.regulatory.valid = false;
    if (runtime.link.status.state != IWX_LINK_IDLE) runtime.link.status.state = IWX_LINK_STOPPED;
    runtime.link.status.port_open = false;
    security_reset();
    runtime.active = false;
    memset(r, 0, sizeof(*r));
    r->stage = "no pending WLAN DMA";
    r->stopped = true;
    if (transport.count) {
        r->stopped = stop_transport(r);
        if (r->stopped) dma_release();
        else {
            r->stop_error = r->error;
            r->retained_bytes = transport.bytes;
            __atomic_store_n(&retained_bytes, transport.bytes, __ATOMIC_RELEASE);
        }
    }
    transport.nvm = NULL;
    runtime.status.state = transport.count ? IWX_QUARANTINED : IWX_OFF;
    runtime.status.error = r->error;
    runtime.status.stop_error = r->stop_error;
    return r->stopped ? 0 : -1;
}

int iwx_stop(iwx_probe_report_t *r)
{
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    r->stage = "driver ownership";
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) {
        r->error = "WLAN diagnostic or kernel handoff is active";
        return -1;
    }
    int rc = stop_locked(r);
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
    return rc;
}

int iwx_handoff_begin(void)
{
    if (__atomic_test_and_set(&probe_lock, __ATOMIC_ACQUIRE)) return -1;
    iwx_probe_report_t r;
    if (stop_locked(&r)) {
        __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
        return -1;
    }
    return 0;
}

void iwx_handoff_end(void)
{
    __atomic_clear(&probe_lock, __ATOMIC_RELEASE);
}
