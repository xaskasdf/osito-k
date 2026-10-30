# OsitoK x86-64 — Network Stack Notes (2026-05)

Sesión de bring-up de red en hardware real (ASUS B450-F + Ryzen 5800X +
Intel I211 8086:1539, conectado por cable USB-Ethernet directo a Mac).
Quedaron varios fixes y herramientas; este documento los junta para que
no haya que re-investigar.

## DHCP unicast investigation: 2026-09-24

The P1 is reachable over AX200/APIPA at `169.254.95.177`, including authenticated
SSH and binary log downloads. A further remote `dhcp` attempt failed after four
DISCOVER attempts while the link remained authorized. The old log says only
`No OFFER received`: it does not distinguish no reply from a rejected reply.
The router at `192.168.1.1` answers ICMP; that alone does not establish
that its DHCP service answered the P1. Host packet capture was not obtained.

A reproducible stack defect was found independently: `handle_ipv4` discarded
unicast OFFER/ACK packets addressed to `yiaddr` before that address was installed.
The actual AX200/NIC/DHCP integration passed broadcast and failed unicast in
both open and WPA2 configurations. RFC 2131 sections 2 and 4.1 describe this
bootstrap delivery: <https://www.rfc-editor.org/rfc/rfc2131.html#section-4.1>.

The correction admits UDP 67-to-68 replies to the explicit port-68 listener,
even before configuration or while APIPA is installed. It never routes these
foreign-address packets to the generic port-0 socket fallback. DHCP still
validates transaction/client identity, cookie, phase, server and options.
IPv4 validates minimum IHL, packet bounds, checksum and UDP bounds; fragments
are rejected because this stack has no reassembly. Broadcast behavior is kept.
Bounded DHCP RX verdicts and TX queued/failure traces distinguish the next
physical run's missing packets from parser rejection; no credentials are logged
by these added traces. This is not yet proof of the physical failure's cause.

Validation artifacts under `arch/x86/build/`:

- `dhcp-unicast-before-20260924`: original unicast regression reproduced.
- `dhcp-diagnostics-20260924`: DHCP/APIPA tests, GCC/Clang, normal and ASan/UBSan.
- `dhcp-unicast-negative-v2-20260924`: all 40 AX200 suite executions pass;
  integration covers open/WPA2, broadcast/unicast, APIPA replacement, explicit
  listener isolation, bad headers/lengths/checksum and fragment rejection.
- `dhcp-net-link-20260924`: all four link lifecycle executions pass.
- `dhcp-ssh-qemu-20260924/results.json`: all 16 production-kernel gates pass,
  including SSH, binary transfer, cold boot persistence and remote kexec.
- `p1-dhcp-sync-20260924`: physical boot log, WLAN status, failed DHCP retry,
  and pre-update log retrieved over SSH. A 32 KiB upload attempt reset its SSH
  session; the P1 remained reachable. That upload issue is separate and open.

Validated kernel: 7,116,880 bytes, SHA-256
`20f8d8003198927e2944eb2dcdc0e5efa0c3ec8aba265c8e9aafea04cef54fad`.
The complete file was committed on the P1 as `/kernel-dhcp-unicast.elf` over
`osito-sync-v1`, using 4 KiB writes after the large-write reset. Remote
`kexec /kernel-dhcp-unicast.elf` was sent; SSH disconnected and the previous
APIPA address stopped responding. The user reconnected WLAN and assigned APIPA
`169.254.51.25`. The new boot is confirmed by the added DHCP TX diagnostics in
`p1-dhcp-sync-20260924/dhcp-unicast-retest.log`; no USB/ESP boot file was changed.

The physical retest still fails: four DISCOVERs are queued, but no UDP 67-to-68
candidate reaches IPv4/DHCP. APIPA and authenticated SSH remain operational.
A second remote `dhcp` attempt reproduces this with xid `0623c2ad`; AX200 replay
drops increase from four to eight, exactly four more. This correlation is not
proof that OFFERs were dropped: reflected copies of the local DISCOVER also
appear in the earlier Ethernet log. Some RX source addresses equal the SNAP
prefix, another observation not yet attributed to the driver or the repeater.

`p1-dhcp-capture-20260924` contains the remote command/status and a 50-second
Windows PktMon NIC capture, filtered to UDP ports 67/68. It captured no packets
and lost no events. This wired vantage point therefore does not establish
whether the AP or DHCP server saw or answered the request. The capture stopped
and its sole temporary filter was removed; no host DHCP configuration changed.

The next AX200 diagnostic keeps the first eight post-authorization data rejects,
DHCP frames, or SNAP-source anomalies separately from the handshake ring. It
records public sequence/CCMP counters, address-relation bits, firmware status,
bounded UDP ports and DHCP xid, never payload or key bytes. Failed MIC/decryption
does not expose candidate IP fields. Replay drops still return before delivery
or counter changes. Ordinary traffic cannot evict the retained samples.
`wlan status` labels the lifetime counters as WLAN RX reasons, since they cover
both sides of authorization. This is observability, not a DHCP fix or permission
to accept replayed frames.

Diagnostic validation: `dhcp-rx-trace-20260924` passes all 40 AX200 suite
executions (GCC/Clang, normal/ASan/UBSan), including 162,150 CCMP/data checks
and 228,243 complete network checks per variant. `dhcp-rx-shell-20260924`
passes four variants with 118,901 checks each. Tests cover bounded/truncated
header inspection, unchanged replay/MIC rejection and counters, trace saturation,
and preservation of handshake evidence. The build uses
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
`dhcp-rx-qemu-20260924/results.json` passes all 16 production-kernel gates,
including remote kexec. This kernel is 7,121,024 bytes, SHA-256
`0cc6ee4a842c8ea3d4e7de715542f20a4c499032213b681a0c7e74d87e86cd45`.
It was fully committed as `/kernel-dhcp-rx.elf` over authenticated file sync
using 4 KiB writes; remote `kexec /kernel-dhcp-rx.elf` was sent. The physical boot
is confirmed by the new trace at APIPA `169.254.56.189`; the ESP remains unchanged.

### Physical RX and wire evidence

`p1-dhcp-rx-20260924` contains the boot/retry log (61,778 bytes) and WLAN status.
The user confirms the P1 connects to the repeater, while this Windows host is
cabled to that repeater. One association failed before M3; a subsequent one
authorized the data port and supports authenticated SSH. DHCP still fails.

The retained group replay sample is UDP 68-to-67 with local-source address bits,
key slot 2 and PN 2223 equal to the prior PN: a reflected local DISCOVER, not
an OFFER. Later reflected DISCOVERs with PN 2224/2225/2227 were accepted. Another
retained replay is mDNS. The trace retains only eight samples, so it does not
classify every later drop. Source-address/SNAP anomalies remain unexplained.
None of this justifies weakening CCMP authentication or replay checks.

PktMon was repeated at NIC and all-component scope with successful ICMP as a
control (`p1-dhcp-rx-capture-20260924`, `p1-dhcp-all-capture-20260924`). Both
captures are empty even though ping succeeds. The earlier empty PktMon capture
is therefore unusable as evidence that DHCP packets did not reach the PC.
All temporary capture sessions/filters were stopped and removed.

A bounded, elevated Winsock SIO_RCVALL capture on the Ethernet source alias
does see traffic (`p1-dhcp-raw-capture-20260924/packets.jsonl`). It retains only
ICMP to/from the P1 and DHCP matching its chaddr, and disables capture on exit.
It captured two echo requests/replies and all four DISCOVERs, ~3 seconds apart,
with wire xid `5e3684db` (kernel's raw little-endian print `db84365e`). IPv4
checksum, length, client MAC, cookie, broadcast flag and options are coherent.
UDP checksum zero is permitted for IPv4; `secs=0` is permitted by RFC 2131
table 5. No OFFER was observed, but unicast to the P1 need not be visible on
the wired PC. This proves TX reaches this side of the repeater, not whether
the upstream DHCP server received, rejected or answered it. Host DHCP remains
enabled; no host lease was released or renewed for these probes.

### Acquisition retransmission timing

An independent client gap was confirmed: fixed 3-second intervals and a
12-second DISCOVER deadline instead of the randomized exponential backoff
required by [RFC 2131 section 4.1](https://www.rfc-editor.org/rfc/rfc2131.html#section-4.1).
Acquisition now waits 4/8/16/32 seconds (+/-1 second) across the existing four
DISCOVER attempts. The three selecting REQUEST attempts restart at 4/8/16
seconds. The helper caps intervals at 64 seconds without overflowing for large
attempt counts. Its local MAC/tick-seeded PRNG is scheduling-only, following
the APIPA pattern; it is not a cryptographic entropy source. Packet identity,
CCMP, parsing and lease-renewal scheduling are unchanged.

Responses finish each wait immediately; polling continues, link/epoch changes
abort, and failed acquisition leaves the existing APIPA configuration intact.
With no OFFER the command now takes roughly 56-64 seconds, not 12. This repairs
a protocol gap but is not yet a demonstrated fix for the physical DHCP failure.
`dhcp-backoff-v2-20260924` passes all eight DHCP/APIPA GCC/Clang normal and
ASan/UBSan executions (217,494 DHCP checks per variant). Coverage includes jitter
range/variation/cap, both retry schedules, delayed OFFER/ACK, epoch cancellation,
unchanged old configuration and tick-counter wraparound.

`dhcp-backoff-iwx-20260924` passes all 40 AX200 suite executions, including
the real driver/NIC/DHCP integration. `dhcp-backoff-qemu-20260924/results.json`
passes all 16 production-kernel SSH/file-sync/cold-boot/kexec checks. The tested
ELF is 7,121,064 bytes, SHA-256
`014da961a20ec0b9a3b10314197f8da9d6494e268118d7593d9a36a2f590c4f9`.
`p1-dhcp-backoff-20260924/dhcp-before-backoff.log` preserves the physical log
before hot reload. Remote `dhcp` executes synchronously; allow at least
100 seconds without SSH keepalive failure when testing both acquisition phases.
The tested ELF was committed as `/kernel-dhcp-backoff.elf` using 4 KiB
authenticated sync writes (`p1-dhcp-backoff-20260924/upload.txt`). Remote
`kexec /kernel-dhcp-backoff.elf` was sent and SSH reset. The physical retest
below confirms the new acquisition timing; the ESP is unchanged.

### Backoff physical retest: 169.254.67.96

`p1-dhcp-backoff-retest-20260924/dmesg.log` preserves 66,833 bytes from the
next physical boot before additional probes. WPA2 authorized in session 1;
three complete DHCP transactions (xids `13342184`, `b00b9264`, `b1019395`)
each queued four DISCOVERs and ended with `No OFFER received`. There is no
DHCP RX verdict or logged panic/page fault in that saved log. APIPA assigned
`169.254.67.96`; authenticated SSH and file-sync downloads work.

The new acquisition timing is confirmed independently by the retained RX
trace: reflected DISCOVER arrivals at 122260, 127220 and 135620 ms are 4.96
and 8.40 seconds apart. The new helper's MAC/xid seed predicts 4.97, 8.39,
16.04 and 31.95 seconds for that first transaction. The first replay sample
is again a reflected 68-to-67 DISCOVER, not an OFFER. No retained sample is
a 67-to-68 reply, but 33 later samples were skipped by the bounded trace.
`wlan-status.log` records TX 162/162/0 sent/completed/failed and an open data port.
The timing correction did not solve the physical DHCP failure.

Two controlled UDP 67-to-68 probes were sent from the wired PC's link-local
source: 244 bytes unicast to the P1, then 245 bytes limited broadcast. Both
carried the P1 chaddr but no offered address, server identifier or lease, so
neither could install configuration. No DHCP acquisition was running. Both
reached the real DHCP handler with `result=inactive` in
`dmesg-rx-probe.log` (71,957 bytes); `ifconfig` stayed `169.254.67.96` with
link up. This verifies delivery from that wired peer through the repeater to
UDP/68 in both modes, not that the upstream DHCP service answered or that all
possible server packets work. No firmware/kernel update or reboot was done
during these probes. The next useful comparison is the main router's Wi-Fi
without the repeater, or a capture of the real server's response path.

## Primera prueba fisica: 2026-09-24

P1 por Wi-Fi al repetidor y PC de desarrollo por Ethernet: IPv4 local
verificado con APIPA `169.254.167.93`, 4/4 ping (3-11 ms, 0% perdidos) y eco
UDP 7777 con token exacto. DHCP fallo y el usuario ejecuto `apipa`; Internet
no esta validado. La ruta APIPA de Windows apuntaba a Tailscale, por lo que la
prueba uso direccion y ruta temporales en Ethernet, retiradas al terminar sin
modificar DHCP ni la salida normal del PC.

TCP 22 no respondio en ese kernel: `sshd.c` era un stub sin listener ni
sesiones. La implementacion nueva usa TinySSH bajo 0BSD, activacion explicita
y autenticacion Ed25519, con pruebas reales OpenSSH/QEMU; falta validarla
sobre AX200 en el P1. Ver [consola SSH](x86-ssh.md). UDP 7777 sigue siendo
solo eco/inferencia.
Evidencia y hashes: [prueba IPv4/SSH del P1](x86-ax200-wlan.md#physical-local-ipv4-and-ssh-probe-2026-09-24-0428-utc).
El kernel de esta prueba tiene eco temporal de clave pedido por el usuario;
no se debe confundir ese diagnostico con un requisito del protocolo WPA2.

## Estado historico revisado: 2026-09-23

Las secciones historicas de mayo no describen todo el estado actual. Para
el bring-up WLAN del P1 se reutilizara la pila existente, sin abrir otro
proyecto de acceso remoto autenticado:

```
AX200 / asociacion 802.11 / claves -> nic_ops -> net.c -> DHCP o APIPA
                                                   -> OFTP / HTTP / RPC
```

- `nic.c` selecciona un unico backend activo: I211, RTL8111, virtio-net o AX200.
  Entrega tramas Ethernet a `net.c`. No hay seleccion dinamica multiinterfaz.
- `main.c` prioriza Ethernet con enlace; si falta, falla o no tiene carrier y hay AX200,
  prepara el backend WLAN con enlace apagado y los mismos servicios de red.
  La seleccion ocurre antes de `net_init`, IRQs y DHCP, sin cambio automatico posterior.
  `wlan start`, `scan` y `connect` son acciones explicitas. `wlan connect`
  abre el asistente del shell. Los modos directos son `wlan connect wpa2 <BSSID>`
  o `wlan connect open <BSSID>`; son alternativas, nunca se escribe un pipe.
  La clave se pide sin eco/historial; solo se lanza DHCP tras autorizar datos.
  El P1 ya confirma arranque persistente, escaneo pasivo (7 redes, 13/13 comandos)
  y apagado con liberacion de DMA. El asistente tambien funciona en el P1;
  tres intentos en dos redes fallaron al asignar la cola TX, antes de autenticar.
  Se corrigen los IDs dinamicos y cursores de esa cola, con respuesta cruda
  visible en el diagnostico. El ultimo intento devolvio cola=1, flags=1 y
  cursor=0: lo detenia nuestro check del flag del cuerpo, que Intel/OpenBSD
  no usan como codigo de error. Se alinea ese contrato, manteniendo los errores
  de cabecera y las comprobaciones de TX. El retest fisico ya completa TX 2/2
  y asociacion con AID 1; descarta seis EAPOL sin instalar claves y el AP
  desconecta con reason 15. Falta conocer el motivo de esos descartes y validar
  WPA2/datos IP. El nuevo diagnostico conserva solo motivo, estado y campos
  publicos de cabecera, tambien despues de parar el transporte; no exporta
  claves ni payloads. No cambia las reglas de aceptacion de WPA2.
  La ventana y boton propios quedan como
  [plan de interfaz Wi-Fi](x86-wlan-ui-plan.md), no como GUI implementada.
- Los cambios de generacion/enlace/MAC invalidan direcciones, ARP, DNS pendiente
  y conexiones anteriores sin borrar listeners. No se repite `net_init`.
  DHCP/APIPA rechazan aplicar una configuracion sobre otro enlace. Una lectura
  de estado ocupada no equivale a desconexion; una renovacion GTK tampoco.
- DHCP y `apipa_assign` ya existen. APIPA configura IPv4 sobre un enlace
  ya disponible; no establece la asociacion Wi-Fi ni sustituye WPA.
  Su configuracion no lleva gateway ni DNS: no proporciona salida a Internet.
  La renovacion DHCP automatica y la defensa completa de conflictos siguen
  pendientes. Las pruebas con AP simulado no demuestran conectividad RF real;
  ver [estado AX200 y pruebas](x86-ax200-wlan.md).
- `net_start_poll_worker` crea `net-rx` en contexto de proceso. La descripcion
  historica de polling desde `sched_tick` ya no es el contrato actual.
- `kdownload` recibe archivos por OFTP, `kupload --dmesg` envia el klog desde
  RAM incluso sin filesystem, `kupdate` descarga mediante HTTPS y `kexec`
  carga un ELF de kernel. No hay que duplicar estos comandos para WLAN.

### Entrada de comandos: resultado de la inspeccion

`net.c::handle_ipv4` despacha UDP al callback registrado; `handle_tcp` alimenta
las conexiones y colas de accept. No llama al shell ni interpreta sus comandos.
Los consumidores encontrados en el arbol x86 son:

| Entrada | Uso | Activacion |
|---------|-----|------------|
| UDP 7777 | `main.c::prompt_handler`: inferencia o eco | Boot con NIC |
| UDP 7780 | Respuestas OFTP a `kdownload` | Comando local |
| UDP 19999 | Descubrimiento/heartbeats Inferconnect | Boot con NIC |
| TCP 50052 | Comandos RPC de Inferconnect, no shell arbitrario | Inicio explicito del servicio |
| HTTP `httpd` | GET/listado y lectura de archivos | Comando local |
| Serial QEMU | `scripts/drive.py` envia lineas al shell | Transporte del emulador |

El final de `kernel_entry` actualmente difiere el arranque de los workers
TCP RPC/cluster/reaper por un problema de scheduler en hardware real. Que
exista el servidor no significa que este escuchando tras el boot.
En la inspeccion del 23 de septiembre `sshd.c` tampoco ofrecia un shell
operativo. Esto queda reemplazado por la consola de mantenimiento documentada
en [x86-ssh.md](x86-ssh.md), habilitada mediante `sshd start`, no durante boot.
Las APIs de sockets permiten ademas servidores de aplicaciones.

### Correccion de recepcion OFTP

El receptor anterior usaba el mayor offset visto como si probara que todos
los bytes previos habian llegado. Perder un datagrama intermedio podia guardar
un archivo incompleto y encadenar `kexec`. Ahora `oftp_rx.h` conserva solamente
el prefijo contiguo: un EOF prematuro no completa, y OFTN pide desde el primer
byte ausente. Los bloques adelantados se descartan y retransmiten, sin bitmap
ni cambios al wire format. Se validan longitudes sin overflow y total estable.

`cmd_kdownload` filtra IP/puerto del servidor, protege el estado frente al
worker RX, comprueba registro del listener, reintenta el REQ perdido o con ARP
frio y no ejecuta `kexec` si falla guardar/flush. La comprobacion del peer no
es autenticacion; OFTP sigue siendo para una red de pruebas de confianza.
No tiene identificador de transferencia: datagramas antiguos del mismo peer
y tamano aun requieren una futura extension versionada del protocolo.

Pruebas reproducibles:

```
bash arch/x86/scripts/test-oftp-rx.sh <directorio-nuevo>
bash arch/x86/scripts/test-oftp-qemu.sh <directorio-nuevo>
```

La segunda usa discos pequenos aislados mediante `qemu-test-disks.sh`, prueba
el comando real con perdida, EOF adelantado y REQ perdido, y extrae los archivos
para compararlos byte por byte. No conecta USB fisicos ni discos persistentes.

Validacion de este cambio: `build/oftp-rx-v1/` pasa con GCC/Clang normales y
ASan/UBSan; `build/oftp-qemu-v1/result.json` registra los cinco casos y el
readback exacto. Kernel probado: SHA256
`82e11377aa75e90f1f4f7f88b105288f675c6c4ef239ba9b7b72aa8ac2982cc2`.
No se ejecuto un handoff `kexec`, no se probo AX200 y no se actualizo el USB.

### AX200 y gaps pendientes

Se extrajo, sin ejecutar el instalador, el paquete local `n2ow901w.exe` a
`arch/x86/build/ax200/n2ow901w/`. `Netwtw08.INF` incluye `8086:2723` (AX200),
DriverVer `02/27/2019,21.0.0.5`. `NETwtw08.sys` importa NDIS, NT, HAL y WDF;
no es una DLL Win32 ni contiene fuentes compilables para OsitoK. El archivo
`Netwfw08.dat` no tiene la cabecera TLV del firmware Linux y no se debe renombrar
a `.ucode` y tratarlo como compatible.

Ya existe el diagnostico nativo, descrito en [AX200 WLAN](x86-ax200-wlan.md):
deteccion PCI separada de Ethernet, `wlan probe` con MMIO/reloj/RF-kill y
apagado acotado, y `wlan firmware <path>` para inspeccionar el contenedor TLV
API-77 desde VFS. `wlan fwboot <path>` implementa context-info, DMA y espera de
ALIVE real, seguida siempre por apagado verificado; ambas etapas ya pasaron
en el P1, con y sin trazas persistentes.
Un fallo de apagado retiene los buffers, bloquea nuevos probes y exige reintento
con `wlan stop`. `kexec` prepara staging reservado por el allocator y usa la
misma exclusion para verificar el apagado antes de sobrescribir destinos
finales; ver [contrato y pruebas de kexec](x86-kexec.md).
Tambien estan implementados lectura NVM/MAC, transporte
persistente RX/comandos y escaneo pasivo con politica regulatoria del firmware
(`wlan start`, `scan`, `networks`, `abort`). `wlan associate wpa2 <BSSID>`
(o `wlan associate open <BSSID>`)
ahora implementa contextos MAC/PHY, TX de gestion y autenticacion/asociacion
802.11 con estado independiente de los ACK del firmware. El puerto de datos
permanece cerrado; no acepta credenciales ni anuncia conectividad. Estas etapas
pasan las pruebas host pero todavia requieren validacion fisica.
El core WPA2-PSK/CCMP ya esta compilado y probado contra OpenSSL en host
(intercambio inicial, renovacion GTK, replay, ACK/generaciones y aleatoriedad
con error explicito). El driver ya lo llama mediante `iwx_connect_psk()`:
EAPOL encolado, instalacion CCMP con ACKs/generaciones y validacion RX de MIC/PN.
El AP simulado completa esa ruta y la de `nic_ops`/DHCP/DNS, con colas Ethernet
y entrada segura de credenciales mediante `wlan connect`; no equivale a una
prueba RF. En el P1, el retest NVM completa 3/3 comandos, valida MAC/NVM y
libera DMA; INIT_COMPLETE trae secuencia 0xC002 y cuatro bytes. El arranque
tambien selecciona AX200 correctamente sin cable Ethernet. La evidencia esta
en el documento AX200; todavia no hay IP por WLAN verificada fisicamente.
El selector de `wlan connect` se corrigio para aceptar el conteo positivo de
redes copiadas por `iwx_scan_results()`. `wlan associate` sigue
siendo solo diagnostico.
Referencias primarias: [Linux iwlwifi](https://wireless.docs.kernel.org/en/latest/en/users/drivers/iwlwifi.html)
y [OpenBSD iwx](https://man.openbsd.org/iwx).

Otros gaps de la ruta existente que esta correccion no resuelve:

- APIPA usa la cache ARP para conflicto, no detecta todos los probes simultaneos
  y no limpia el gateway anterior. Requiere revisar probe/announce y defensa.
- `kupload` no espera un ACK de finalizacion ni recupera huecos; el servidor
  actual guarda al recibir EOF. No confundir envio completado con entrega
  comprobada del log.
- `kupdate` necesita revisar validacion HTTP/longitudes y errores de escritura
  y flush antes del handoff. No fue probado contra un servidor externo aqui.
- `kexec` existente no implica hot reload transparente: reinicia el kernel,
  y falta validar su ciclo de vida con AX200 sobre el P1 real.

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
en :7780. Re-tries vía NAK si stall > 500 ticks (5 s a 100 Hz, hasta 5 attempts).
El servidor usa 7781 si se omite el puerto; los ejemplos eligen 7779 explicitamente.

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

## 8. `kupdate` — web flow implementado

Commit: `cc3e578`.

La descripcion anterior como stub quedo obsoleta. `cmd_kupdate` usa
`http_open`, `http_request` y `http_read_body_full`, guarda `kernel.elf` y
encadena `cmd_kexec`, salvo con `--no-kexec`. Requiere Content-Length y un
filesystem montado. El host por defecto es `wasm.naranjositos.tech`; acepta
otro host/path o `--channel`. Esto describe implementacion, no disponibilidad
actual del servidor ni validacion completa del update (ver gaps arriba).

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

## 10. Passive TCP cold-ARP retry (2026-09-27)

An isolated Steam maintenance test exposed a failure before Steam launched:
the first SSH connection after the desktop timed out during banner exchange.
In `build/steam-ssh-independent-qemu-20260927-r1/` and `-r2/`, the serial log
records an incoming SYN, a SYN+ACK deferred for ARP, and the ARP reply. The
second run's GDB snapshot has the BSP idle, no ready tasks, an active SSH
service and no accepted connection. The listener waited for a duplicate SYN
instead of driving its own pending SYN+ACK. No retransmission timer covered
TCP_SYN_RCVD. This is a passive-open contract gap, not evidence of a shell
lock or a cause established for the P1's separate reconnection failure.

`net_poll` now retries passive opens on a one-second initial deadline, doubles
the interval up to 60 seconds, and expires the half-open after seven retries
(at least 183 seconds including the final wait). Expiry removes listener
queue references before releasing the connection slot and wakes accept
waiters. Duplicate SYNs do not restart the timer or consume sequence space.
SYN+ACK transmission uses the original ISN without moving SND.NXT backwards:
a failed retry cannot invalidate an ACK for an earlier successful send. The
handshake's timer/count are cleared on establishment; data starts with the
existing conservative three-second RTO. No change to SSH authentication,
outbound SYN policy, NIC selection or WLAN configuration is included.

The production-code host fixture `test-tcp-passive.sh` passes 181 checks
with GCC/Clang, normal and ASan/UBSan, in
`build/tcp-passive-positive-20260927-r2/`. Coverage includes a cold ARP cache
without any peer retransmission, a lost SYN+ACK, duplicate SYN timing,
sequence wrap, NIC send failure, a late ACK for an earlier successful send,
invalid ACK rejection, half-open expiry/slot reuse, unreachable ARP and
listener cancellation. The pre-fix net.c is the negative control in
`build/tcp-passive-negative-20260927-r1/`; it fails specifically at the absent
cold-ARP SYN+ACK retry. Link/DNS lifecycle still passes its 41 checks in all
four configurations in `build/net-link-passive-open-20260927-r1/`.

The post-fix isolated run `build/steam-ssh-independent-qemu-20260927-r3/`
establishes SSH and queues Steam in 2.080 seconds using the same cold setup
which failed twice before. At 60, 180 and 360 seconds, status, uptime, dmesg,
redirected write and readback all complete with the local Steam job running.
The authenticated 1280x800 snapshot completes in 0.556 seconds; an exclusive
reboot request is rejected, the matching console stop returns the local
console to idle, and SSH responds afterwards. The snapshot contains two
WebHelper page faults, not a functioning Steam library. Physical P1 validation
remains pending. Kernel SHA-256:
`45fa5bccf3c823cadd67e41fd5fcdb5c4f58facaf7a84d18fa25077736394cf9`.

The first general SSH run on this kernel passes 26 gates, then the connection
intended to send kexec times out before its SSH banner. No kexec is dispatched;
this is not a kexec crash. The focused session-reuse run passes eight binary
transfers with 64 KiB rekey limits, each followed immediately by a successful
exec session. Evidence: `build/ssh-passive-open-full-20260927-r1/` and
`build/ssh-session-reuse-20260927-r1/`. The intermittent teardown/admission
delay is a separate investigation, not covered by the passive-open fix.
Two subsequent traced full runs, `build/ssh-passive-open-full-20260927-r2/`
and `-r3/`, pass all 28 gates on the same kernel, including remote kexec,
automatic reconnect, stable host identity and the post-reload desktop capture.
They do not reproduce that timeout; no extra admission retries or timeout
increases were added to make these gates pass.

GitNexus was unavailable. Manual impact review covered `net_poll`,
`handle_tcp`, packet sequence publication and passive-listener ownership;
all inbound TCP services share the change. References:
[RFC 6298, retransmission timer](https://www.rfc-editor.org/rfc/rfc6298.html#section-5),
[RFC 9293, TCP](https://www.rfc-editor.org/rfc/rfc9293.html).

## 11. Virtio TX ownership under concurrent Steam traffic (2026-09-27)

The ten-minute run `build/steam-ssh-phase-qemu-20260927-r1/`, kernel SHA-256
`56da67b9cfa10066233fc8126ce9defea305277525087e31d4fa6c7b298f2ef7`,
passes 24 of 60 scheduled SSH maintenance requests. The last request at
180 seconds and all subsequent checkpoints fail. The system keeps scheduling
tasks, but the NIC stops responding before SSH can accept another session.
`qemu.log` reports `virtio: zero sized buffers are not allowed`. A live GDB
snapshot in `virtqueue-live.log` records device status `0x4f` (NEEDS_RESET),
RX available/used indexes `0x67d9/0x66d9`, and TX `0x6b3a/0x68e2` for a
256-entry queue. `runq-live-3.log` shows free TCP slots, an empty SSH listener,
`connection=-1` and `in_net_poll=0`; this is not a saturated connection table
or an SSH command waiting for the shell gate. Logs, PCAP and task snapshots
are retained. The final remote capture and cooperative stop could not run.

The code had two independent ownership violations: the final free descriptor
linked back to index zero instead of an exhausted-list sentinel, and every
TX descriptor referred to the same scratch buffer. Concurrent senders also
modified the descriptor free list without serialization. Even a single sender
could overwrite a delayed DMA packet after the bounded completion poll expired.
Deterministic tests fail on the old code for queue exhaustion, distinct
in-flight payloads, full-queue backpressure and error-status reporting.

The free list now terminates at queue size, and virtio-net allocates a persistent
2048-byte buffer for each TX descriptor (512 KiB at 256 descriptors). Submission
and completion reclamation share a short IRQ-saving lock. Submission does not
wait for hardware completion with interrupts disabled, and full queues return
failure without overwriting exposed descriptors or frames. Queue size is
written to the device after capping it, and the notification follows a store
barrier. NEEDS_RESET/FAILED or lost DRIVER_OK makes the link unavailable with
a diagnostic instead of continuing to advertise a working NIC. This does not
implement automatic device reset/reinitialization.

Manual impact review, with GitNexus unavailable: `virtqueue_init` is shared
by virtio-net and virtio-blk; the TX ownership/status changes are confined to
virtio-net. The AX200, TCP protocol, SSH authentication and shell gates are
unchanged. This virtual-device failure does not establish why the physical
P1, using AX200, is unreachable while its foreground shell remains occupied.
The buffer ownership requirement follows the
[Virtio specification, section 3.3](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
and its split-queue rules.

`build/virtio-net-negative-20260927-r1/` preserves the pre-fix sources and the
four failing cases. `build/virtio-net-host-20260927-r3/` passes all six groups
under GCC and Clang, normally and with ASan/UBSan. The concurrent group uses
four senders and validates 76,800 frames across descriptor reuse and the
16-bit ring-index wrap. Other groups cover full queues without publication,
out-of-order completions, immutable pending payloads, IRQ-state restoration,
device-error status and the device/driver queue-size agreement. Generic queue
tests cover the shared allocator; a real virtio-blk device was not exercised.

Kernel `be78fb60a6d273a6abb6decaddb205a2797b3c283a1b71d3017fad529b675431`
(7,262,624 bytes) passes the eight gates in
`build/virtio-tx-private-qemu-20260927-r1/`, including independent PE32 and
PE64 child-address-space echo tests (64 KiB each). All 28 general SSH gates
pass in `build/virtio-tx-ssh-qemu-20260927-r1/`, including diagnostics, files
and capture while the foreground shell is busy, persistent identity, cold
boot and remote kexec. Sustained real-Steam qualification is separate from
these bounded fixtures.

The subsequent ten-minute run `build/steam-virtio-tx-qemu-20260927-r1/`
passes all 60 scheduled maintenance requests, without extending the SSH
deadlines or retry counts. Steam's final authenticated 1280x800 GOP capture
shows the rendered Store, including product images. The capture completes
in 2.121 seconds (1,815,844 compressed wire bytes). Exclusive reboot remains
rejected while busy; cooperative stop returns the console on the third
status poll, and post-stop SSH succeeds. `maintenance-results.json` retains
each result. The late live queue snapshot reports device status `0x0f`, RX
outstanding 256 and TX outstanding zero; QEMU reports no invalid descriptor.
This validates sustained virtual-NIC maintenance for this run, not Library
interaction, game launch, automatic device-reset recovery or physical WLAN.

The physical follow-up `build/p1-steam-maint-20260927-r1/` is not a matching
pass: fifteen maintenance queries through 240 seconds complete (with later
latency), then three at 360 seconds time out. A subsequent authenticated
cooperative stop recovers the local prompt and diagnostics without a restart.
The new SSH phase logs place the recorded failures in key negotiation and
authentication, before shell dispatch. WLAN stays on channel 6; its RX
software-queue drops grow from zero before Steam to 706 after this test.
`iwx.c:link_notification` increments that counter when the 16-frame queue is
full. This is observed packet loss worth investigating, not proof that it
caused either SSH failure. No AX200 queue, scheduler priority or TCP timeout
was changed for this test. Remaining Steam child processes and its profile
window survive the main owner's stop; see `after-stop-ps.log` and capture.

## 12. TCP send ownership and cumulative ACKs (2026-09-27)

The physical P1 run `build/p1-gdi-screen-20260927-r1/` records
`WSOCK-SEND-FAIL` with established TCP connections, `kernel_error=0` and
`wsa_error=10050` (WSAENETDOWN). Its continuation WLAN snapshot reports
4105 Ethernet frames queued and 69 dropped, with the authorized link still
on channel 6. AX200's `iwx_send` can reject submissions when its software
queue is full or its probe lock is busy. Previously `net_tcp_send` retried
once immediately and propagated that rejection as a stream failure. A
successful submission was also published before its bytes entered the TCP
retransmit queue: an immediate ACK or an interleaved sender could leave the
queue inconsistent with the transmitted sequence space. The Steam log's
separate Encryption Failure is not yet proven to come from these defects.

External sends now reserve their sequence and append to the existing bounded
queue before NIC submission. A short IRQ-saving critical section protects
publication against BSP task preemption; driver calls and `net_poll` run
outside it. NIC rejection leaves the accepted bytes owned by TCP and covered
by its retransmit timer. Full queue/peer-window credit still returns zero,
and calls on closed connections still fail. Neither queue sizes nor timeout
limits are increased. Loopback keeps its synchronous delivery path.

Cumulative ACK processing uses modular sequence comparisons, rejects ACKs
beyond SND.NXT before accepting their payload/window, and trims from the
queue's actual sequence origin. Draining the queue clears its timer. CLOSE_WAIT
processes ACKs and retransmits outstanding data too; an ACK delivered during
retransmission is no longer overwritten by the caller's backoff bookkeeping.
This is scoped to current BSP task serialization, not a general SMP-safe TCP
or connection-lifetime redesign. RX buffer ownership, out-of-order reassembly,
zero-window persistence and congestion control remain separate work.

`test-tcp-tx.sh` builds the production-code fixture with GCC and Clang, both
normally and with ASan/UBSan. All nine groups pass in
`build/tcp-tx-positive-20260927-r2/`: bounded queues, immediate ACKs, NIC
backpressure, sequence wrap, invalid ACKs, half-close, nested sends, no credit,
and an independently checked 512 KiB stream with rejected submissions, lost
packets and delayed ACKs. Seven groups fail against the preserved pre-fix
source in `build/tcp-tx-negative-20260927-r2/`; normal/no-credit remain green.
The loss test proves byte integrity, not improved throughput. Passive-open
tests retain 181 checks in all four configurations in
`build/tcp-passive-tx-20260927-r1/`, and link/DNS lifecycle retains its 41 in
`build/net-link-tx-20260927-r1/`.

Kernel SHA-256
`f6d4b682f9a28c533b415035c7a76645fc15946220385bc6e17914151db55c17`
passes all 28 full SSH gates in `build/tcp-tx-ssh-qemu-20260927-r1/`, including
files/capture with a busy local console, cold boot and remote kexec. All eight
gates in `build/tcp-tx-private-qemu-20260927-r1/` pass, including 64 KiB exact
external TCP echoes from PE32 and PE64 child address spaces. Physical
installation and sustained Steam validation are still pending at this point;
the previous P1 kernel answers ping but times out accepting SSH while Steam
is open. That does not demonstrate a shell-command lock.

GitNexus was unavailable; manual impact review covered `net_tcp_send`,
`handle_tcp`, `net_poll` and their callers in SSH, TLS/HTTP, sockets, WinSock
and cluster RPC. References:
[RFC 9293, ACK processing](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.10.7.4),
[WinSock send contract](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-send).

The first real-Steam run, `build/steam-tcp-tx-qemu-20260927-r1/`, is not a
startup pass: the base USB image lacks the system font already installed on
the P1. It reports `no usable system font file found`, repeats the known
bottom-of-stack faults from the CEF font path, and passes only 7/15 scheduled
maintenance requests. Its final snapshot is stopped in `nt_section_page_fault`
with IF clear; the same instruction/tick remains in the late admission
snapshots. Capture and cooperative stop fail. All logs are retained; this
failure must not be counted as successful network or Steam qualification.

## 13. TCP receive-buffer preemption (2026-09-27)

A deterministic production-code probe found an independent corruption path.
Packet dispatch computes `rx_buf + rx_len`, then can be preempted by a consumer
that drains and compacts that buffer. The resumed producer writes to the old
offset but publishes a length based on the new one. A three-byte old prefix
followed by a four-byte new packet produces `OLDN` instead of `NEW!` in the
probe. Concurrent readers or a peek interrupted halfway by a consuming read
can similarly return a mixture which matches neither valid stream ordering.
These reproduce without WLAN, TLS or Steam; they do not by themselves prove
the cause of the physical Encryption Failure.

The incoming append and its length/sequence publication now share a short
IRQ-saving critical section. `net_tcp_recv` protects the copy, compaction and
length update together; `net_tcp_peek` protects its snapshot. Window-update
decisions are captured before unlocking, and ACKs/driver calls/wakeups stay
outside the critical sections. The two existing FIN_WAIT receive append
paths receive the same protection. Their separate partial-buffer and FIN
sequence semantics are not corrected by this patch. This follows the current
BSP task model; it is not a cross-CPU lock or a solution for concurrent socket
release/reuse. Buffer sizes and allocation policy are unchanged.

`test-tcp-rx.sh` builds seven groups against production `net.c` with a host-only
IRQ model which defers simulated task switches until interrupts are restored.
GCC/Clang, normal and ASan/UBSan, all pass in
`build/tcp-rx-positive-20260927-r1/`. Coverage includes append versus consume
(established and both FIN_WAIT states), overlapping readers, peek versus
consume, arrival during a read, buffered EOF, full receive windows, duplicate
segments, sequence wrap, zero-length reads and restoration of already-masked
IRQ state. The mock NIC rejects calls made inside a newly acquired IRQ guard.
Three groups fail on the preserved TX-only source in
`build/tcp-rx-negative-20260927-r1/`. No test-only hook is active in the kernel.

The nine TX groups remain green in `build/tcp-tx-rx-regression-20260927-r1/`;
the 181 passive-open and 41 link-lifecycle checks also pass all four compiler
configurations in `build/tcp-passive-rx-20260927-r1/` and
`build/net-link-rx-20260927-r1/`. The combined kernel builds successfully with
SHA-256 `cbb4d6b5a89b29c3489476614ee4e869735958aa465e0f2d47fc172dee6bf441`.
All 28 SSH gates pass on this combined TX/RX image in
`build/tcp-rx-ssh-qemu-20260927-r1/`. The eight private-address-space gates,
including PE32/PE64 external TCP echo, also pass in
`build/tcp-rx-private-qemu-20260927-r1/`. Physical validation remains pending.
GitNexus was unavailable; manual impact review covers receive/peek callers
in SSH, TLS, native sockets and WinSock, and all receive-buffer append paths.

## 14. Network waiter deadline retirement (2026-09-27)

`net_async_check` woke an expired waiter and then cleared its slot without
an outer IRQ guard. `sched_unblock` protects its own run-queue changes, but
restores the caller's interrupt state on return. A timer could therefore
run the awakened task, which could register another wait in the same slot
and block again. The expiry sweep would then delete that new wait, leaving
a blocked task with no network wakeup entry. Packet-triggered wakeups already
protected the whole operation; deadline-triggered wakeups did not.

The bounded 16-slot expiry scan now keeps wakeup and retirement under one
IRQ-saving guard. It preserves an already-masked caller and does not extend
the guard over DNS callbacks, NIC submission or the preceding async work.
This follows the existing BSP-owned scheduler model, not a cross-CPU lock.
Timeout values, queue sizes, priorities and TCP behavior are unchanged.

`test-net-waiter.sh` includes production `net.c` and models preemption at the
first enabled-IRQ boundary after wakeup. Same-owner rearm, different-owner
slot reuse and repeated churn fail on the preserved pre-fix source in
`build/net-waiter-negative-20260927-r2/`, with both GCC and Clang, normally
and with ASan/UBSan. The ordinary deadline, packet-wakeup and nested-IRQ
controls still pass. The fixed source passes all six groups in
`build/net-waiter-positive-20260927-r1/`, including all five waiter types,
64-bit deadlines, preserved IRQ state and 10,000 expire/rearm cycles.

The nine TX and seven RX groups, 181 passive-open checks and 41 link/DNS
lifecycle checks also pass all four configurations in the corresponding
`build/net-waiter-{tcp-tx,tcp-rx,tcp-passive,net-link}-20260927-r1/` runs.
The combined image builds as 7,262,664 bytes with SHA-256
`4bb6918040253ee1c915b03315313e30b9754179c47b10313d30d08b2de7923b`.
This is a demonstrated lost-wakeup defect, not yet a trace proving the cause
of a particular P1 SSH outage or SteamService fault. GitNexus was unavailable;
manual impact review covers `net_poll` -> `net_async_check` and the ARP,
TCP connect, receive, close and accept waiters (including SSH).

All 28 full-kernel SSH gates pass in
`build/net-waiter-ssh-qemu-20260927-r1/`, including busy-console diagnostics,
file I/O, capture, cold boot and remote kexec. All eight gates also pass in
`build/net-waiter-private-qemu-20260927-r1/`, including exact external TCP
echoes from PE32 and PE64 private address spaces. On the P1, authenticated
sync transferred the frozen tested image and `kupdate --file ... --no-kexec`
verified both OsitoFS and EFI copies in `build/p1-net-waiter-20260927-r1/`.
Runtime qualification after hotreload is separate from that persistence check.

## 15. Effective TCP send MSS (2026-09-27)

The sender capped data at 1460 bytes without accounting for TCP options.
With negotiated timestamps this produced 1526-byte Ethernet frames:
14 Ethernet + 20 IPv4 + 32 TCP + 1460 data. AX200 correctly rejects frames
above its 1514-byte Ethernet limit. RTO and fast retransmit repeated the
same oversized segment, so a larger queued write could make no progress
even though handshakes and small requests worked. The peer's MSS option
was also ignored on both active and passive opens.

`handle_tcp` now captures a nonzero MSS from SYN/SYN-ACK only. An absent
or zero MSS uses the IPv4 default of 536. Send capacity, retransmission
and serialization share the actual header-size calculation, including
timestamp padding and bounded SACK blocks. Data is limited to
`min(peer_mss, 1460) - TCP_options`, saturated at zero. A peer offering
less room than the negotiated options does not cause unsigned underflow
or queue bytes that cannot be sent. The serializer also rejects an
oversized payload before constructing the packet. Our advertised MSS
remains 1460, as required when subtracting only fixed headers from the
interface MTU; see [RFC 9293 section 3.7.1](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.7.1).

`test-tcp-mss.sh` includes production `net.c`, performs active/passive
handshakes and independently checks wire lengths, IP/TCP checksums,
sequence numbers and exact delivered data. Ten groups cover timestamps,
smaller/missing/zero/large peer MSS, all existing SACK/TS header layouts,
RTO, fast retransmit after NIC rejection, an MSS option on a non-SYN ACK,
and very small MSS values. Nine groups fail on the preserved source in
`build/tcp-mss-negative-20260927-r1/`; the no-options 1460-byte control
passes. All ten pass with GCC/Clang, normal and ASan/UBSan, in
`build/tcp-mss-positive-20260927-r1/` (4175 checks per configuration).
The TX fixture now explicitly sets its synthetic established peer MSS;
the new suite negotiates it through real handshakes instead.

The nine TX groups, seven RX groups, 181 passive-open checks, 41 link
lifecycle checks and six waiter groups remain green in the corresponding
`build/tcp-mss-{tx,rx,passive,link,waiter}-20260927-r1/` runs. The kernel
build is 7,262,704 bytes, SHA-256
`e538edab00157373fa2ef5d8e947294c65b0f4da5043386f26d77edb24ed1a39`.
GitNexus was unavailable. Manual impact review covers normal sends,
both retransmission callers, both handshake directions, send readiness,
loopback and the native/WinSock/SSH consumers of this shared TCP stack.

The ten AX200 host suites also pass all four configurations in
`build/tcp-mss-iwx-20260927-r1/`, including integrated DHCP/ARP/DNS/ICMP
and the unchanged Ethernet queue/CCMP/TKIP contracts. The production
SSH run `build/tcp-mss-ssh-qemu-20260927-r3/` passes all 28 gates on the
same image, including busy-console I/O/capture, binary sync, cold boot
and automatic reconnection after kexec. All eight private-address-space
gates pass in `build/tcp-mss-private-qemu-20260927-r1/`, including exact
external TCP echo from PE32 and PE64.

Do not suppress the preceding failures: `tcp-mss-ssh-qemu-20260927-r1`
passes 14 gates then times out during an SSH banner exchange; `-r2`
passes nine and fails similarly. The second run preserves PCAP and GDB
state. Its peer advertised MSS 1460 without timestamps, so that connection
does not exercise the new option deduction. At the sampled instant the
SSH service is running with no accepted connection, not executing a remote
shell command. These observations do not identify the cause of the delay.
The frozen pre-MSS image passes all 28 gates in
`build/tcp-mss-baseline-qemu-20260927-r1/`; successful retries do not establish
that intermittent admission delay is fixed. No timeout or gate was relaxed.

The initial physical deployment attempt failed: the P1 answered `console
status` with idle/request 2 returned before these tests, then stopped
answering ARP and TCP 22 on 192.168.1.20 at upload time. No transfer,
persistent update or kexec completed in that attempt. The tested image is in
`build/p1-tcp-mss-20260927-r1/kernel.elf`.

After the user rebooted the P1 on 2026-10-01, authenticated binary sync
transferred that exact image. `kupdate --file /kernel-tcp-mss-20260927.elf
--no-kexec` verified both OsitoFS and EFI copies; the subsequent kexec
reconnected automatically with DHCP 192.168.1.20 on channel 6. The kexec SSH
session and first reconnect timed out; the next authenticated uptime was
89 seconds. These failures are retained alongside the successful boot logs
in the same directory, with the `20261001` suffix.

Steam launched with the existing diagnostic arguments still displays
"Loading user data...". All nine uptime/console/dmesg probes at 30, 90 and
180 seconds succeed in `steam-check-20261001/`. A later authenticated
1920x1080 capture verifies 6,220,800 raw bytes from 443,546 compressed bytes
in 38.4 seconds overall (30.613 seconds transfer). This is not a controlled
throughput comparison. WLAN counters still show queue drops after the run:
Ethernet TX 7479 queued/572 dropped and RX 17361 queued/1176 dropped, despite
7482/7482 successful firmware TX completions. Those counters alone do not
identify the Steam connection failures.

After the user closed Steam, the console remained occupied. Diagnostics
and `console stop 2` succeeded, then `console status` reported idle/request
2 returned. During preparation for a separate no-arguments test, SSH again
timed out and ping received no replies from the P1 (one local-host
unreachable response is not a P1 reply). Thus this physical run demonstrates
working deployment and busy-console diagnostics, not sustained network or
application stability. No no-arguments launch had occurred at that point.

This does not add PMTU discovery, RX reassembly or congestion control,
enable SACK negotiation, grow AX200 queues, or prove the cause of a
specific Steam spinner.

## 16. Partially duplicate TCP receive segments (2026-10-02)

`handle_tcp` discarded a whole established-stream segment when its starting
sequence preceded `RCV.NXT`, even if its tail contained new bytes. With SACK
disabled, receiving `ABCDEF` at sequence 900 and then `DEFGHI` at 903 left
the ACK stuck at 906 and discarded `GHI`. The receiver now skips only the
acknowledged prefix and passes the new suffix through its existing atomic
receive-buffer append. Wholly old duplicates keep their duplicate-ACK path.
Trimming preserves the original FIN sequence, including across 32-bit wrap.
This follows the duplicate-data treatment in
[RFC 9293 section 3.10.7.4](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.10.7.4).

The existing full-window policy is unchanged: if the new suffix cannot fit,
no new bytes are acknowledged and the peer must retransmit. This does not
implement out-of-order reassembly, change FIN_WAIT receive handling, enable
SACK, enlarge buffers or change WinSock interfaces. The production handshake
already disables SACK; the dormant range-only SACK helpers therefore do not
establish the cause of the current physical Steam failure.

`test-tcp-rx.sh` now runs eight groups. The overlap group checks buffered and
already-consumed prefixes, reader preemption during append, wholly old data,
sequence wrap, window rejection/retry, and FIN with duplicate/new data.
`build/tcp-overlap-positive-20261002-r2/` passes all eight groups with GCC and
Clang, normal and ASan/UBSan (1534 checks in the new group per configuration).
The preserved pre-fix source fails the new cumulative-ACK assertion while
the seven original groups pass in `tcp-overlap-negative-20261002-r3/`.

The `tcp-overlap-{tx,mss,passive,link,waiter}-20261002-r1/` regression runs
also pass all four compiler/sanitizer configurations. The frozen kernel in
`tcp-overlap-ssh-qemu-20261002-r1/` passes all 28 SSH gates, including binary
sync, busy-console capture/I/O, cold boot and kexec reconnection. The private
PE32/PE64 network profile passes its eight gates in
`tcp-overlap-private-qemu-20261002-r1/`. The kernel is 7,333,904 bytes, SHA-256
`2056751b823e195e69654773a69cf47460c7d651df283320e151994e240dfa62`.

GitNexus was unavailable. Manual impact review covers established TCP RX,
duplicate ACKs, FIN placement and the SSH/native/WinSock consumers of the
shared stream. The fix is not yet deployed to the P1 and does not prove the
cause of Steam's repeated `Encryption Failure` or later SSH admission loss.

## 17. WinSock DisconnectEx ownership (2026-10-02)

Steam's DisconnectEx path dropped its kernel connection ID after shutdown
without releasing the TCP claim. The stack intentionally does not reclaim a
CLOSED record still claimed by a consumer: its buffered data/error may still
belong to that consumer. Therefore increasing the table or globally clearing
closed claims would hide the leak and risk reuse under a live owner.

The WinSock fix assigns the detached connection to a pending disconnect
request, keeps it claimed during graceful close, releases it once, and sends
the required event/IOCP completion. Cancellation and process teardown retire
the request too. See section 4.195 of `win32-layer-correctness.md` for the
causal Steam trace, pre-fix exhaustion control and native/QEMU x86/x64 tests.
This is a consumer-lifetime fix, not a TCP capacity change or evidence that
the P1's post-login encryption failures are resolved.

The final combined image is now installed in OsitoFS and the P1's USB EFI
partition. Hotreload restores saved Wi-Fi, DHCP and pinned SSH automatically,
and both private-child DisconnectEx fixtures pass on hardware. Natural Steam
in QEMU renders Store content at ten minutes with no unowned CLOSED claims
at the 180/360/600-second checkpoints. Physical Steam qualification remains
separate; these observations do not retroactively prove the cause of every
earlier transport failure.

## 18. Final WinSock process-exit ownership (2026-10-02)

Ordinary WinSock process teardown had another TCP claim leak: its last-owner
helper called `net_tcp_abort` but omitted `net_tcp_release`. The helper now
performs both operations, under the existing socket lifecycle lock and only
after checking all process handles and duplication tokens. The TCP table and
abort/release API contracts are unchanged. A surviving source descriptor
remains usable when an unconsumed duplicate's target exits.

Section 4.196 of `win32-layer-correctness.md` records the pre-fix exhaustion
control, native/QEMU x86/x64 results and an independent headless process-slot
limit encountered by the combined stress run. The new kernel is local only;
the P1 is still running section 17's image while Steam signs in. Its current
log again reports successful logon followed by `Encryption Failure`, and a
verified snapshot shows the user-data spinner. Sequential SSH queries remain
available. Neither the socket-exit fix nor the measured WLAN queue drops
establish the cause of that physical transport failure.

## 19. Validate TCP receive checksums (2026-10-02)

The shared TCP receive entry previously accepted segments without validating
their checksum. IPv4 header validation does not cover the TCP header or data,
and the NIC interface supplies no checksum-verified metadata. The virtio path
does not negotiate checksum offload either. A bad data byte could therefore
be acknowledged and delivered to an encrypted stream instead of discarded
for retransmission. This is a reproduced transport defect, not proof of the
physical Steam `Encryption Failure` cause.

`handle_tcp` now receives the actual IP destination and validates the complete
pseudo-header/header/options/data checksum before connection lookup, allocation,
ACK generation or state/window updates. The loopback serializer passes the same
destination used to compute its checksum. Control-only SYN/ACK/FIN/RST segments
have the same requirement; see [RFC 9293 section 3.1](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.1).
IPv4 length bounds exclude Ethernet padding. A checksum field of zero is still
accepted when the complete calculation is valid; unlike IPv4 UDP, it cannot
disable verification. The first 16 invalid segments produce bounded metadata
logs without payload. The checksum helper uses two-byte copies rather than
unaligned `uint16_t` dereferences and preserves the existing TX representation.

`tcp_rx_host.c` now constructs complete IPv4 packets with an independent
bytewise TCP checksum oracle. The preserved pre-fix source passes the eight
original groups but fails the new single-bit corruption check: it changes the
receive state and sends an ACK. All four configurations reproduce that failure
in `build/tcp-checksum-negative-20261002-r1/`. The final positive suite in
`tcp-checksum-positive-20261002-r2/` passes all nine groups with GCC/Clang,
normal and ASan/UBSan. Its 598 checksum checks cover damaged payloads/headers/
options/control segments, pseudo-header mismatches, short packets, odd lengths
and alignment, valid zero checksums, passive-open non-allocation and successful
retransmission with exact delivered bytes. The existing TX/MSS/passive fixtures
now supply valid checksums and the explicit destination to the receive entry.

All four compiler/sanitizer variants also pass in the corresponding
`tcp-checksum-{tx,mss,passive,link,waiter}-20261002-r1/` runs. The same frozen
image passes all 28 USB-backed SSH gates in
`tcp-checksum-ssh-qemu-20261002-r1/`, including binary sync, busy-console
diagnostics, cold boot and automatic kexec reconnection. External WinSock
traffic from private-address-space PE32/PE64 processes passes the eight gates
in `tcp-checksum-vnet-20261002-r1/`. Both ABIs also pass the eight private-child
loopback/DisconnectEx gates in `tcp-checksum-loopback-20261002-r1/`, including
data exchange, reconnect, event/IOCP completion and SSH after the fixtures.

The kernel is 7,351,448 bytes, SHA-256
`dca976418625b3043878081a5f6a9e9f7b1aebab24e6a7446c24dceb606d8587`.
It also contains the buffered-file-read optimization described in section
4.200 of `win32-layer-correctness.md`. GitNexus was unavailable; manual impact
review covers both receive callers, TX checksum serialization, four host
fixtures and the shared SSH/native/WinSock consumers. No connection-table,
crypto, timeout, NIC-offload or SACK policy is changed. General reassembly,
congestion control and previously unqualified FIN_WAIT behavior remain open.

Neither this kernel nor the buffered-read change has reached the P1: TCP 22
still times out. The last physical logs show a successful Steam logon before
the later encryption failure, not an initial credential rejection. The new
link-triggered breakpoint is separately awaiting an exact missing-import trace.

The subsequent user-initiated restart restored access. The exact combined
image above is now persisted to OsitoFS/EFI and hotreloaded on the P1, with
automatic channel-6 Wi-Fi, DHCP and pinned SSH reconnection. Natural Steam
reaches its complete login/QR form. No bad-checksum drop is present in the
captured launch dmesg, so this run does not establish that checksum rejection
explains the earlier encryption failures. See the physical buffered-read
deployment report in `x86-usb-storage.md` for measured startup improvement.
After the user signs in, the current log again records logon OK followed by
`Encryption Failure` roughly two minutes later (04:43:40/04:45:43 and
04:45:59/04:47:45). The screenshot remains at `Loading user data...`. The
post-login dmesg still has no bad-TCP-checksum drops. Checksum validation is
required independently but demonstrably does not resolve this failure in the
physical run. Short SSH queries and a revision-checked 16 KiB log tail remain
available while Steam owns the console; the complete changing-log pull and
shell's full-file tail scan are not valid negative reachability tests.

## Quick reference

| Comando shell | Función | File |
|---------------|---------|------|
| `dmesg \| grep X` | Log filtering | shell.c |
| `nic_stats` | I211 live state | i211.c, shell.c |
| `kdownload <ip> <port> <file> [save\|--kexec]` | OFTP fetch | shell.c |
| `kupdate` | HTTPS download, optional kexec | shell.c |
| `ping <ip>` | ICMP echo | net.c |
| `kexec [file]` | Boot loaded ELF | shell.c |

| Issue | Status | Workaround |
|-------|--------|------------|
| MSI delivery stuck post-boot | Fixed (GPIE.EIAME) | always-poll fallback (revertible) |
| RX RDT off-by-one | Fixed | — |
| PCI cap unaligned read | Fixed | — |
| Reply-path TX silent drop | **Open** | OFTP (OsitoK-initiated only) |
| `kupdate` web flow | Implemented; validation gaps remain | requires reachable HTTPS server |
