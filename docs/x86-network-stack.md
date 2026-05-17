# OsitoK x86-64 — Network Stack Notes (2026-05)

Sesión de bring-up de red en hardware real (ASUS B450-F + Ryzen 5800X +
Intel I211 8086:1539, conectado por cable USB-Ethernet directo a Mac).
Quedaron varios fixes y herramientas; este documento los junta para que
no haya que re-investigar.

---

## 1. Shell pipes

Commit: `a692452`.

Parser top-level en `arch/x86/kernel/shell.c::shell_pipeline_dispatch` (~L1853).
Splittea la línea en `|` (skip dentro de quotes), corre cada etapa, captura
su stdout en buffer heap de 1 MB, y lo pasa como `sh_stdin_buf` a la
siguiente. Hasta 8 etapas. La última imprime/redirige normal.

`grep`, `head`, `tail` aceptan stdin: invocados sin file arg (o con `-`)
leen del buffer. `cat` aún requiere file (no migrado todavía).

```
osito> dmesg | grep PCI
osito> dmesg | grep I211 | tail 5
osito> dmesg | grep ISR= | head 20
```

---

## 2. `nic_stats` shell command

Commits: `437353c`, `2d96b04`.

Snapshot live del estado del I211 (sin esperar al TX-kick log capeado a
64 entries):

```
osito> nic_stats
[I211 stats] ISR=0x0000001E RX_ISR=0x0000001E IMS=0x00000084 ICR=0x00000080
              RDH=0x005A RDT=0x0034 GPRC=0x00000025 irq_pending=1
```

- ISR/RX_ISR: contadores SW (suben en `i211_isr()`).
- IMS/ICR: registros HW. Si IMS pierde RXT0 = chip enmascarado, no más IRQs.
- RDH/RDT: HW write head vs SW tail. RDT=RDH = ring drained.
- GPRC: Good Packets Received counter (read-clear).

Implementación: `i211_print_stats()` en `arch/x86/drivers/i211.c`.

---

## 3. PCI MSI capability — ECAM aligned read fix

Commit: `a692452`.

`arch/x86/kernel/pci.c:pci_enable_msi` antes leía Status register con
`pci_read32(0x06)` (offset 0x06 unaligned a dword). MMIO en ECAM (UC
memory) requiere reads naturalmente alineados (Intel SDM §11.3) — el
unaligned devolvía 0, abortando con `[PCI] No capability list`.

**Fix**: leer dword aligned en 0x04 (cmd[15:0] + status[31:16]) y testear
bit 20 (= bit 4 del status word) para "Capabilities List present".

Verificación: `dmesg | grep "MSI enabled"` debería mostrar
`[PCI] MSI enabled: vector 40 for 4:0.0`.

---

## 4. I211 RX descriptor RDT off-by-one

Commit: `a692452`.

`arch/x86/drivers/i211.c:i211_recv` antes escribía `RDT = old_tail`
(el índice del slot que acabábamos de re-armar). Datasheet I210/I211
§7.1.6 dice:

> RDT points one descriptor BEYOND the last that HW may use.

Linux igb hace `writel(next_to_use, RDT)` = `(last_re-armed + 1) % SIZE`.
Estábamos lagueando 1 slot, lo que excluía el slot recién re-armado del
pool de HW hasta el siguiente consume.

**Fix**: `i211_write(I211_RDT0, nic.rx_tail);` (post-increment value).

---

## 5. I211 GPIE programming + MSI delivery fix

Commit: pendiente (este sprint).

**Síntoma observado**: MSI vector 40 dispara ~22 veces durante boot
(suficiente para que DHCP/APIPA terminen) y después se queda silencioso
aunque el HW siga recibiendo paquetes (RDH avanza, RDT laguea, GPRC sube).
`nic_stats` (que internamente lee `I211_ICR`) **desbloquea temporalmente**
la cadena.

**Investigación** (3 agents en paralelo + Linux igb source + I210
datasheet §7.3): el registro **GPIE** (0x00028) nunca se programaba. Linux
igb tiene este comentario famoso en `igb_configure_msix()`:

> Turn on MSI-X capability first, or our settings won't stick.
> And it will take days to debug.

**Fix** (`arch/x86/drivers/i211.c::i211_enable_interrupts`):

```c
i211_write(I211_GPIE, I211_GPIE_PBA | I211_GPIE_EIAME | I211_GPIE_NSICR);
```

- **EIAME** (bit 30, "Extended Interrupt Auto-Mask Enable"): sin este
  bit el chip ignora el re-arm del IMS que `net_poll` →
  `i211_rx_irq_reenable` hace tras drenar el ring, y subsiguientes
  transiciones de RXT0 no generan MSI.
- **PBA** (bit 31): permite write-back del Pending Bit Array, inocuo en
  MSI mode.
- **NSICR** (bit 0): explicit "read clears ICR" — es el default pero
  conviene fijarlo.
- **MSIX_MODE** (bit 4): intencionalmente OFF (usamos MSI legacy).

Definiciones de registros agregadas en `arch/x86/drivers/i211.h`:
GPIE, EIAC, EIAM, IAM (los últimos tres por completitud, no se usan en
MSI legacy).

**Verificación**:
1. `dmesg | head` debería mostrar `[I211] Interrupts enabled (MSI vec 40, GPIE=0x...)`
2. Bajo carga sostenida (`ping -c 100 -i 0.05 <osito-ip>` desde el peer),
   `nic_stats` antes vs después de los pings: ISR/RX_ISR debe subir
   proporcional a # paquetes (no stuck en ~0x16 como sin el fix).

---

## 6. `sched_tick` always-poll fallback

Commit: `a692452`.

Workaround temporal mientras el bug del MSI estaba abierto:
`arch/x86/kernel/process.c::sched_tick` llama `net_poll()` incondicionalmente
cada APIC tick (en lugar de solo si `irq_pending`). Esto bypassa el MSI
y mantiene la red drenando vía polling.

`net_poll` tiene un guard de reentrancia (`in_net_poll`) y retorna
inmediatamente si el ring está vacío (1 MMIO read del DD bit), así que
el costo es bajo.

**TODO post-fix**: revertir a la versión condicional (solo cuando
`net_nic_irq_pending() || net_has_active_waiters()`) para recuperar el
beneficio NAPI de no consumir CPU drainando un ring vacío.

---

## 7. OFTP — UDP file transfer protocol

Commit: `cc3e578` + NAK retry.

Workaround para el bug del reply-path (ver §9): transferencia de
archivos OsitoK ↔ Mac via UDP, OsitoK-iniciado.

**Servidor Mac**: `tools/oftp-server.py` (Python 3, ~150 LOC).
```
python3 tools/oftp-server.py 0.0.0.0 7779 /Users/pc/osito-k/arch/x86/build
```

**Cliente OsitoK**: `arch/x86/kernel/shell.c::cmd_kdownload`. Listener UDP
en :7780. Re-tries vía NAK si stall > 500 ms (hasta 5 attempts).

```
osito> kdownload <server-ip> 7779 kernel.elf test.elf
osito> kdownload <server-ip> 7779 kernel.elf --kexec
```

**Wire format**:

| Packet | Direction | Layout |
|--------|-----------|--------|
| REQ    | OsitoK→Server | `OFTQ` (4 B) + filename (60 B null-padded) |
| DAT    | Server→OsitoK | `OFTD` + total(BE32) + offset(BE32) + cklen(BE32) + data(≤1400 B) |
| NAK    | OsitoK→Server | `OFTN` + offset(BE32) — "resend from offset" |
| EOF    | Server→OsitoK | DAT con cklen=0, offset==total |
| ERR    | Server→OsitoK | `OFTE` + ASCII msg |

Pacing: 3 ms entre chunks (configurable). Throughput observado ~2 MB/s
con kernel.elf de 1.2 MB.

---

## 8. `kupdate` stub — futuro web flow

Commit: `cc3e578`.

`arch/x86/kernel/shell.c::cmd_kupdate` actualmente imprime stub. Plan
implementación: encadenar primitivas existentes:

1. `dns_resolve("naranjositos.tech")` → IP
2. `tls13_connect(ip, 443)` → TLS context
3. `http_get(ctx, "/k/x86_64/stable/kernel.elf")` → bytes
4. `osfs2_write` → guardar
5. `cmd_kexec("kernel.elf")` → bootear

Bloqueado en: kernel necesita salida a Internet (actualmente solo cable
directo a Mac, sin gateway). Siguiente milestone H7+ (red con router/AP).

---

## 9.5 SG path en `net_udp_send` (open, disabled)

Commits relevantes:
- `99cda9b` — IFCS/PAYLEN only-on-last (per Intel §7.2.2.2.4)
- `0ec8516` — re-enable attempt #1
- `9f7255f` — kvirt_to_phys translation
- `2398adf` — heap-alloc kupload pkt (descarta stack-source UB)
- `6bdcbc7` — re-enable attempt #2 con tx_pkt 64-byte aligned
- `b5bd22f` — **re-disable definitivo tras 4 intentos fallidos**

**Symptom**: `kupload --dmesg` con SG path activo envía 100% NUL bytes
al server, aunque `kupload: src[0..31]` confirma que `klog_read` puso
ASCII real en el buffer source.

**Bugs encontrados y fixeados (no resuelven el síntoma)**:
1. `VIRT_TO_PHYS` macro underflows para lower-half identity addresses.
   Switched a `kvirt_to_phys()` (paging.h:46). No cambio.
2. `i211_send_sg` setea `IFCS` y `PAYLEN` per-fragment. Per Intel
   I210/I211 datasheet §7.2.2.2.4, IFCS es 1 CRC por paquete y PAYLEN
   es total post-L2 length, ambos solo en el último data descriptor.
   Fixed en `99cda9b`. No cambio el síntoma.
3. `tx_pkt` era plain `static uint8_t[]` (1-byte aligned). Force
   64-byte alignment (`__attribute__((aligned(64)))`) por hipótesis
   de DMA align (commit `6bdcbc7`). NO ayudó: upload sigue 100% NUL.
4. kupload buffer movido stack→heap (`2398adf`) para descartar UB en
   el source side. No cambio.

**Causa residual sospechada**: hay configuración chip-side adicional
que falta (header-split mode? context-only desc?), o una combinación
de campos del descriptor que el chip no acepta en modo MSI con
chained data descs. Linux igb tiene un path más elaborado que
necesitaríamos replicar 1:1.

**Estado actual** (commit `b5bd22f`, 2026-05-16): SG path disabled
en `net_udp_send` (línea ~860).  Todos los UDP pasan por `memcpy →
tx_pkt → i211_send` single-buffer.  Performance suficiente para
tráfico kernel-class.  El path memcpy es 100% correcto y entrega
`kupload --dmesg` con contenido real.

**Próximo experimento**:
- Capturar TX en cable con un NIC sniffer externo mientras enviamos
  via SG, ver si los frames realmente salen del chip o se quedan
  internos
- Comparar bit-a-bit con un capture de Linux igb en el mismo HW
- Replicar el descriptor flow exacto de `igb_xmit_frame` de Linux,
  incluyendo el context descriptor opcional y el `tx_buffer_info`
  unmap-tracking (probablemente no necesario funcionalmente pero
  bueno como referencia)

---

## 9.6 kexec'd kernel #UD diagnostics (instrumentation landed)

Commit relevante: `b5bd22f`.

**Background**: el path `kdownload --kexec` (descargar un kernel.elf
nuevo via OFTP y bootearlo sin reset HW) crashea pre-shell con `#UD
at RIP=0xffff800002064fa2`, una dirección que cae **mid-instruction**
de `gpu_init` (la primera instrucción de gpu_init es un `movabs` de
10 bytes en `fa0..fa9`).  Mid-instruction RIP en `#UD` significa que
el CPU NO ejecutó linealmente desde gpu_init+0 — alguien hizo
CALL/JMP a `fa2` directo (return mismatch, fn-ptr corruption, o
indirect call con valor garbage).

**Instrumentación añadida** (no es un fix, es diagnóstico):

1. `arch/x86/kernel/kexec_tramp.S` — emite `'T'` (0x54) directo a
   COM1 (`outb $0x3f8`) después de `rep movsb` de todos los
   segmentos, antes del `call *%r13` a kernel_entry.  Distingue
   "tramp completó copy, jump-to-kernel falló" de "tramp se
   autoextinguió mid-copy".  Polls 0x3FD bit 0x20 (THR-empty).
2. `arch/x86/kernel/main.c::kernel_entry` — añade probe
   `[KEXEC-PATH] bss-zeroed` después del zero-BSS, para bracketar
   el gap entre la primera probe (`kernel_entry`) y `serial_init`.
   Las probes anteriores existían en `pre-gpu_init` y
   `post-gpu_init`.
3. `arch/x86/kernel/idt.c` panic dump — para excepciones con
   `CS==0x08` (kernel code), añade dump de **CR3, SS, y 4 quadwords
   desde [RSP]**.  El return-slot quadword es la pista crítica para
   identificar indirect-call corruption vs stack corruption.

**Validación**: smoke-test boot directo en QEMU (`b5bd22f`) confirma
que la instrumentación no rompió el path normal (`serial.log`:
`Serial initialized`, virtio-net up, shell prompt, no `EXCEPTION`).
El path kexec'd se ejercita cuando alguien dispare `kexec` desde el
shell o `kdownload --kexec` desde la red.

**Hipótesis abiertas** (a confirmar con la próxima traza):
- (A) HW state stale: el new kernel re-inicia GPU/xHCI/NVMe asumiendo
  un estado limpio, pero el old kernel los dejó en mid-state.
- (B) IDT stale: vectores del old kernel siguen apuntando a código
  ya sobreescrito por la copy del new kernel.  Cualquier IRQ
  spurious cae a bytes random.
- (C) CR3 stale: paging tables del old kernel siguen activas hasta
  que el new kernel corra `paging_init`.  Las "indirect calls"
  podrían estar resolviendo via PTEs viejos.

---

## 9. Reply-path TX bug (open, workaround documentado)

Commits diagnósticos: `437353c`, `b7c2225`, `2d96b04`.

**Symptom**:
- `osito> ping <Mac-IP>` funciona perfecto (4/4 received en tcpdump,
  latencia ~150 µs).
- Pero **ARP/ICMP REPLIES** desde el path `net_poll →
  handle_arp/handle_icmp → eth_send → i211_send` no llegan al wire.

**Confirmado vía instrumentación** (TX-buf hex dump + olinfo_status):
- Eth header bien formado (dst=Mac MAC, src=OsitoK MAC, ethertype correcto)
- Descriptor escrito correctamente (PAYLEN, EOP, IFCS, RS, DEXT)
- HW reporta `olinfo_post.DD=1` y `GPTC` incrementa = chip dice
  "buen packet transmitido"
- Pero `tcpdump -i en5 -p` (promiscuous) en Mac NO ve los frames

**Kernel-side exonerado** (audit 2026-05-15, revisando los 3 commits
diagnósticos): cada paso del path está verificado byte-a-byte:

| Paso | Evidencia | Estado |
|---|---|---|
| RX del paquete | RDH avanza | ✅ |
| `nic_recv → net_poll` con eth header correcto | log eth dump | ✅ |
| `handle_ipv4` procesa (proto/dst/checksum OK) | rate-limit log | ✅ |
| `handle_icmp → icmp_send` con data correcta | log | ✅ |
| `i211_send` arma frame con dst=Mac MAC correcto | hex dump `tx_buf[0..15]` | ✅ |
| HW write-back `DD=1` | `olinfo_post` log | ✅ |
| Chip's `GPTC` counter incrementa | TX kick log | ✅ |
| Mac `tcpdump -i en5 -p` ve el frame | — | ❌ |

Las hipótesis kernel-side (doorbell readback en IRQ ctx, cache barrier,
phys-translation, IRQ ctx race) están **muertas**: cualquiera de ellas
sería visible como `GPTC` no incrementando o `DD=0`.

**Hipótesis vivas restantes (todas medio físico)**:
- A: USB-Ethernet bridge del Mac drop frames timing-related (TX
  inmediato post-RX dispara dentro de un burst window del adapter)
- B: Frame anomaly invisible al hex dump (padding/runt/FCS) que L1 del
  bridge rechaza
- C: I211 errata silently dropping reply-context frames — chip miente
  con `GPTC++` pero nunca emite al PHY

**Workaround actual (suficiente para el use case)**: OFTP (kdownload /
kupload) solo necesita Mac→OsitoK RX y OsitoK→Mac TX iniciado desde
shell context, ambos funcionan. El reply-context "broken" no impacta
la iteración kexec-over-network.

**Próximos experimentos requieren hardware extra** para B/C, pero
**hipótesis A es testeable en-tree**:
- Switch barato + sniffer L1 entre Mac y OsitoK con port mirroring → si
  el switch ve los frames y el Mac no, el bridge USB-Ethernet del Mac es
  el culpable; si el switch tampoco los ve, errata I211 confirmada
- Repro mínimo en Linux booteando OsitoK con USB live → mismo I211, RX→TX
  IRQ-ctx idéntico; si Linux entrega y OsitoK no, ahí sí hay algo en
  nuestro driver que falta ver

**Hipótesis-A test rig landed (commit `2e3716a`, 2026-05-17)**:
- `net.c` stamp `last_rx_complete_tsc` al final del RX loop en
  `net_poll()`.
- `net_pre_tx_wait()` busy-polls RDTSC en `eth_send` hasta que pasen
  `g_tx_post_rx_delay_us` µs desde el stamp.
- `txdelay <us>` shell builtin para tunear live.
- Test plan:
  1. Bootear, ping de Mac al guest continuo.
  2. `osito> txdelay 0` → confirmar 0% reply rate.
  3. Sweep `txdelay 100`, `500`, `1000`, `2000`, `5000`.
  4. Si alguno flippea a 100% → hipótesis-A confirmada, próximo paso:
     defer reply-path TX a workqueue que corra "1 tick post-RX".
  5. Si ni 5000 µs ayuda → hipótesis-A refutada, queda solo B/C con HW
     externo.

---

## Quick reference

| Comando shell | Función | File |
|---------------|---------|------|
| `dmesg \| grep X` | Log filtering | shell.c |
| `nic_stats` | I211 live state | i211.c, shell.c |
| `kdownload <ip> <port> <file> [save\|--kexec]` | OFTP fetch | shell.c |
| `kupdate` | (TODO) Web update | shell.c |
| `ping <ip>` | ICMP echo | net.c |
| `kexec [file]` | Boot loaded ELF | shell.c |

| Issue | Status | Workaround |
|-------|--------|------------|
| MSI delivery stuck post-boot | Fixed (GPIE.EIAME) | always-poll fallback (revertible) |
| RX RDT off-by-one | Fixed | — |
| PCI cap unaligned read | Fixed | — |
| Reply-path TX silent drop | **Open** | OFTP (OsitoK-initiated only) |
| `kupdate` web flow | Stub | needs Internet access |
