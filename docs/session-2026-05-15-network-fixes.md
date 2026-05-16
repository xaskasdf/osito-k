# Sesión 2026-05-15 — Cadena de fixes de red para RAG end-to-end

Notas de la sesión que llevó `rag_query` de "ERR: rag_query failed"
a "Wikipedia says: …" usando el corpus de Cloudflare R2 / Workers AI.

## Cadena de commits (orden cronológico de fix)

| Commit  | Capa             | Cambio |
|---------|------------------|--------|
| `c01e4a7` (sesión previa) | Init       | UDP listener dedupe + DNS `sti;hlt` wait |
| `f43cbc5` | RAG/TCP          | `TCP_RX_BUF_SIZE` 8K → 65535 + Path 2 corpus baking |
| `4007d3e` | TCP              | Ventana dinámica + window-update ACK al drenar |
| `0fcacbc` | HTTP             | Retry de `tls_recv` distinguiendo timeout vs close |
| `15fe11a` | TCP + scripts    | MSS option en SYN + soporte `NET=vmnet` |
| `5999926` | virtio-net       | Rewrite modern + RX ring 32 → 256 |

## 1. `net_udp_listen` idempotente (`net.c`)

**Bug:** `net_dns_resolve` se llamaba dos veces para el mismo host →
re-registraba el handler en puerto 10053. Después de 4 registros,
`MAX_UDP_LISTENERS=4` rebotaba todo en silencio con "Too many UDP
listeners". El segundo DNS lookup colgaba.

**Fix:** walker de listeners short-circuita cuando `(port, handler)`
ya está presente.

## 2. `net_dns_resolve` resistente a `cli` heredado (`net.c`)

**Bug:** `net_tcp_close` usa `sti; hlt; cli` y deja IF=0 al volver.
El siguiente caller que use `hlt` "pelado" se queda esperando
forever.

**Fix:** el wait loop de DNS pasó a `sti; hlt` (sin `cli` al final),
funciona independientemente del estado de interrupts que dejó el
caller.

## 3. TCP receive window grande + dinámica

### Parte A — capacidad (`net.h`, commit `f43cbc5`)

`TCP_RX_BUF_SIZE: 8192 → 65535` (máximo para 16-bit window sin
RFC 7323 window scaling). Antes de esto, responses bulk se
truncaban a ~5KB.

### Parte B — advertise correcto (`net.c` línea 1005, commit `4007d3e`)

**Bug:** `tcp_send_segment` ponía `tcp->window = htons(TCP_RX_BUF_SIZE)`
siempre, mintiendo al peer sobre nuestra capacidad real. CF mandaba
bytes pasado nuestro rx_buf real → `handle_tcp` los clampeaba a
`space` (línea 1267) → datos descartados silenciosamente.

**Fix:** `tcp->window = htons(TCP_RX_BUF_SIZE - rx_len)` en cada
paquete saliente, reflejando el espacio real disponible.

### Parte C — window-update ACK (`net.c`, `net_tcp_recv`)

Cuando la app drena el buffer cruzando de > ½ lleno a ≤ ½ libre,
mandamos un ACK puro para que el peer se entere que la ventana se
abrió de nuevo.

## 4. MSS option en SYN saliente (`net.c`, commit `15fe11a`)

**Bug:** Nuestros SYN salían sin ningún TCP option:

```
length 40 = 20 IP + 20 TCP, opciones: NONE
```

Comparado con macOS:

```
length 64 = 20 IP + 44 TCP, opciones: mss 1460, wscale 6, TS, sackOK
```

CF (y otros load-balancers/scrubbers comerciales) **dropean
silenciosamente** los SYN sin MSS — los confunden con port scans.
SYN+ACK nunca llegaba. Bajo slirp el bug estaba enmascarado porque
slirp hacía loopback local.

**Fix:** `tcp_hdr_len = (flags & TCP_SYN) ? 24 : 20`. En SYN
agregamos 4 bytes:

```
+--------+--------+---------------+
|  0x02  |  0x04  |    1460 (16)  |
+--------+--------+---------------+
   kind    length     MSS value
```

## 5. `http_read_body` distingue timeout vs close (`http.c`, commit `0fcacbc`)

**Bug:** `tls_recv` retornaba -1 para dos cosas distintas:

- `tls_read_exact` deadline elapsed (transient — peer está midway)
- TCP cerró de verdad

El loop `if (n <= 0) break;` trataba ambos como end-of-stream →
bodies grandes con pausas entre TLS records se truncaban.

**Fix:** después de -1, peek a `net_tcp_state(tcp_conn)`. Si está
ESTABLISHED y `idle < MAX_IDLE_RETRIES` (12 × 500 ticks ≈ 60s),
`continue`. Solo break en close real. Aplicado a ambas ramas
(chunked y content-length).

## 6. virtio-net modernización + RX ring (`virtio_net.c`, commit `5999926`)

### Parte A — virtio 1.0+ PCI transport

Antes: BAR0 layout legacy. Solo funcionaba contra
`disable-modern=off,disable-legacy=on` con la I/O BAR legacy (que
se está deprecando).

Ahora: walk del PCI capability list para descubrir
COMMON/NOTIFY/ISR/DEVICE cfg regions. Negocia
`VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC`. Usa el primitive
`virtqueue_t` de `virtio.c`.

### Parte B — RX ring buffer pool (`VNET_RX_BUFS 32 → 256`)

**Bug:** 32 buffers × 2048 B ≈ 64 KB total. Bajo vmnet line rate
(~80 KB/s), un burst >32 paquetes entre drains llenaba el used
ring → virtio dropeaba los siguientes.

Pcap evidencia: CF retransmitió `seq 15736:17196` seis veces,
nuestro kernel siempre ACK 15736 → los paquetes llegaban a vmnet
pero virtio los descartaba antes de que `nic_recv` los viera.

**Fix:** 256 buffers ≈ 512 KB, ~370 ms de drain budget a line rate.

## 7. Soporte `NET=vmnet` en scripts (`qemu-agent-hvf.sh`, `test-rag-probe.sh`)

`-netdev user` (QEMU slirp) tiene un bug conocido: dropea
silenciosamente paquetes de continuación en respuestas grandes
(regresión documentada en sesión 2026-05-10). El switch:

```bash
sudo NET=vmnet bash scripts/test-rag-probe.sh
```

Pasa a `-netdev vmnet-shared,id=net0`. Apple's vmnet framework
hace bridging real con NAT vía pf, sin los dropeos de slirp.
Requiere root (vmnet entitlement); el script chequea y errorea
claro si no hay sudo.

## Verificación end-to-end

```
[PROBE] rag_query rc=651 q="What is the capital of Japan?"
  obs="Wikipedia says: Tokyo — Tokyo () is the capital and largest
  city of Japan. It is on the island of Honshu in the region of
  Kanto..."
[PROBE] rag_query rc=251 q="Who wrote Hamlet?"
  obs="Wikipedia says: Hamlet — The Tragedy of Hamlet, Prince of
  Denmark is a tragedy play by William Shakespeare..."
=== summary: 2 passed, 0 failed ===
```

## Metodología de diagnóstico (lo que funcionó)

1. **Curl desde el host como ground truth.** `curl
   https://rag.naranjositos.tech/embed` desde el Mac entregó los
   19372 bytes en 237 ms. Esto descartó el endpoint y CF como
   causa, dejando el problema en nuestro lado.

2. **Pcap diff host vs kernel.** Comparar el SYN del curl del host
   contra el SYN del kernel destapó el MSS faltante:

   - host: `Flags [SEW], options [mss 1460,nop,wscale 6,…sackOK,eol]`
   - kernel: `Flags [S], length 40` (sin opciones)

3. **Trazar handle_tcp temporariamente.** Después de cambiar a
   vmnet y arreglar MSS, seguíamos truncando. Trace de
   `[RX-DATA] seq=… len=… copy=…` mostró que solo recibíamos un
   handful de packets. La pcap simultánea mostró CF retransmitiendo
   `seq 15736:17196` seis veces — diferencia entre "lo que CF
   manda" y "lo que kernel ve" identificó la pérdida en el RX ring
   de virtio.

4. **Bisect por volumen recibido.** A medida que arreglábamos cada
   capa, el tope de truncación subía: 4758 → 7494 → 11597 →
   completo. Eso confirmaba que cada fix era load-bearing.

## Lo que NO está cubierto / queda como deuda

- **Window scaling (RFC 7323):** advertizamos `rcv_wscale=3` pero
  el código de send no lo usa — el max window real sigue siendo
  64 KiB.
- **SACK, timestamps:** no negociamos ni en SYN ni en ACK.
- **Slirp (`-netdev user`):** queda truncando responses >5KB. Fix
  sería upstream de QEMU; el workaround es vmnet.
- **PKI chain validation (A12.3):** pinned a leaf+intermediate; sin
  RSA-2048 + name matching para endpoints terceros. Deferred
  deliberadamente.
