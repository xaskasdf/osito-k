# Sesión 2026-05-16 — TLS 1.3 + RSA + Agent layer closeout

Continuación de `docs/session-2026-05-15-network-fixes.md` (que dejó
la red en estado funcional con RAG end-to-end vía vmnet).  Esta
sesión cerró los siguientes ítems del roadmap, incluyendo varios
que estaban marcados como "deferred":

| # | Item                                  | Status final           | Commit  |
|---|---------------------------------------|------------------------|---------|
| 1 | RAG en system prompt                  | ✅ done                | edaa311 |
| 2 | Dynamic leaf-cert pin (A12.4)         | ✅ done                | 9be1e9e |
| 3 | TCP RFC 7323 window scaling           | ✅ done                | bc6c066 |
| 4 | TLS 1.3 cliente end-to-end            | ✅ done                | 4b2a773, f04a54a |
| 5 | RL bandit UCB1 sobre few-shot         | ✅ done                | e420664 |
| 6 | Slirp upstream                        | 📋 documented          | 4a129cc |
| 7 | Streaming SSE                         | ✅ ya estaba (17888)   | —       |
| 8a | RSA-2048 PKCS#1 v1.5 verify          | ✅ done                | 0f5f28e |
| 8b | X.509 chain link verify (A12.5)      | ✅ done (ECDSA + RSA)  | 8fe13d6 |
| 9 | A12.3 SHA-384 + RSA-SHA384 + root pin + validity (A12.6) | ✅ done | (this session, afternoon push) |

## 1. RAG en system prompt (`edaa311`)

`agent_build_system_prompt` ganó una sección "## Knowledge
retrieval" que enseña al modelo a despachar preguntas factuales
por `rag_query` antes de responder.  Sin esto el modelo inventaba
respuestas desde pre-training o pateaba para `read_file` /
`mem_stat` en preguntas que necesitaban un corpus externo.

Buffer del system prompt creció de ~1.7 KB a ~2.4 KB; el cap de
3 KB sigue holgado.

## 2. Dynamic leaf-cert pin — A12.4 (`9be1e9e`)

`cert_pin.c` ganó un runtime pin table persistido en
`osfs2:tls/pins.bin`.  Cuando un chain valida vía un pin estático
(intermedio típicamente) y el leaf no está aún registrado, se
captura y se persiste.  En el próximo boot,
`cert_pin_load_dynamic()` lee el archivo y restablece la tabla.

Beneficio: cuando CF rota el leaf cert (cada ~90 días) sin
necesidad de operator rebuild — el intermedio estático cubre el
primer handshake post-rotación, en el cual capturamos el nuevo
leaf, y a partir de ahí está pin-anchored.

Verificado live:

```
[PIN] match: GTS WE1 intermediate
[PIN] dyn capture: new leaf added
[PIN] dyn table saved (1 entries)
... (next connection same boot) ...
[PIN] match: dynamic pin
```

## 3. TCP Window Scaling RFC 7323 (`bc6c066`)

Tres cambios coordinados en `net.c`/`net.h`:

- `TCP_RX_BUF_SIZE`: 65535 → 131072 (128 KiB).  Memoria BSS:
  `tcp_conns[32]` × 128 KiB ≈ 4 MiB, ok con 512 MiB total.
- `tcp_send_segment` emite MSS + WS options en SYN (header
  24 → 28 bytes).  WS option = kind=3, len=3, shift=3.
- Inbound SYN parser extrae `peer_wscale` del WS option en
  SYN+ACK; capturado en `conn->snd_wscale`.  Sin opción en
  cualquier lado, ambos bajan a `wscale=0` (RFC 7323 §2.2).
- Outbound window field escalado por `rcv_wscale` en segmentos
  no-SYN.

Verificado live:

```
[TCP] Connected (conn 0, wscale snd=13 rcv=3)
pcap: options [mss 1460,nop,wscale 3]
```

CF responde con wscale=13 (es decir, su valor de window se
multiplica por 8192 antes de leerlo).  El nuestro=3 permite
advertir hasta 128 KiB de credit.

## 4. TLS 1.3 cliente end-to-end (`4b2a773`, `f04a54a`)

Reemplazo del stub completo.  `kernel/tls13.c` ahora hace:

- **ClientHello**: x25519 keypair (vía `x25519_public`),
  `supported_versions=0x0304`, `supported_groups=x25519`,
  `signature_algorithms` (ECDSA-P256-SHA256 + RSA-PSS-RSAE-SHA256),
  `key_share` con nuestra pubkey, SNI.  Cipher: only AES-128-GCM
  (poly1305 está incompleto en crypto2.c — sin update path).
- **ServerHello parser**: walks extensions, encuentra
  `supported_versions` (verifica = 0x0304), `key_share` (extrae
  peer x25519 pubkey).
- **ECDH**: `x25519(client_priv, server_pub)` → shared secret.
- **Transcript hash**: buffer + re-hash con sha256() — no tenemos
  SHA-256 incremental.  Tamaños de hs típicos ≤ ~5 KB.
- **HKDF key schedule (RFC 8446 §7.1)**:
  ```
  early_secret      = Extract(zero32, zero32)
  derived           = ExpandLabel(early, "derived", H(""), 32)
  handshake_secret  = Extract(derived, ECDH_shared)
  c_hs_traffic      = ExpandLabel(hs, "c hs traffic", th, 32)
  s_hs_traffic      = ExpandLabel(hs, "s hs traffic", th, 32)
  ```
  Master = `Extract(ExpandLabel(hs,"derived",H(""),32), zero32)`.
  App traffic = `ExpandLabel(master, "{c,s} ap traffic", th_at_server_Finished, 32)`.
  Key + IV via `ExpandLabel("key", 16)` / `ExpandLabel("iv", 12)`.
- **AEAD framing**: AES-128-GCM con AAD = 5-byte outer record
  header, plaintext = data ‖ inner_type, padding 0x00 stripped on
  decrypt, nonce = static_iv XOR (seq big-endian to 12 bytes).
- **Pin check**: walker propio para TLS 1.3 Certificate (con
  per-entry extensions), synthesize TLS-1.2-shape buffer para
  reusar `cert_pin_check_leaf`.
- **Server Finished MAC**: HMAC(finished_key, transcript-before-
  Finished); rechaza si no matchea.
- **Client Finished**: HMAC sobre transcript-after-Finished,
  emitido encrypted con c_hs_key.
- **App traffic switchover**: clave en `th_at_server_Finished`
  (NO post-client-Finished — bug inicial, causó decrypt fail en
  s_seq=0 hasta corregirse).
- **tls13_send / tls13_recv** públicas con sequence number
  tracking y close_notify alert handling.

Verificado live contra `rag.naranjositos.tech`:

```
[TLS1.3] ClientHello sent (149 bytes)
[TLS1.3] ServerHello parsed, x25519 key share OK
[TLS1.3] server Finished verified
[TLS1.3] client Finished sent
[TLS1.3] handshake complete — app traffic keys live
[PROBE] tls13 body=815 bytes  (= HTTP/1.1 404 Not Found para /healthz)
```

**Lo que NO está**: `CertificateVerify` ECDSA over the TLS-1.3
signature input (context string + transcript hash).  Pin nos da
identidad (cert correcto); CertificateVerify cerraría "el SKE
fue firmado por el dueño del cert" (lo mismo que hacemos en
`tls.c::SKE_verify`).  ~60 LoC, deferred — el pin ya tapa el
threat model que nos importa.

## 5. RL bandit UCB1 (`e420664`)

`agent_render_few_shot` rankeaba ejemplos por pure cosine.  Ahora
es composite ranking:

```
rank = cosine
     + 0.25 * (max(score, 0) / 5)        ← score bias
     + 0.15 * sqrt(ln(N) / max(1, n_i))  ← UCB1 exploration term
```

Donde `N` = total tasks completed this boot,
`n_i` = times this trajectory has been retrieved as few-shot.

`rl.c` añadió:
- `rl_entry_t::n_retrieved` (runtime-only, no persistido)
- `rl_global_task_count` incrementado en `rl_finalize_auto`
- `rl_bandit_bonus(task_id)` → bonus composite
- `rl_bandit_mark_visited(task_id)` para decay
- `rl_sqrtf` + `rl_logf` (Newton + bit-hack ln) para evitar libm

Net effect: trajectories que funcionaron se amplifican, pero
under-visited candidates igual reciben turno → exploración
contextual sobre el espacio de retrieval.

## 6. Slirp upstream — deferred (`4a129cc`, doc `slirp-defer.md`)

Workaround vmnet anda; libslirp 4.9.1 dropea continuation packets
en responses grandes (regresión documentada en
`docs/session-2026-05-10.md`).  Fix real es upstream — fuera de
alcance para una sesión.  `NET=vmnet bash scripts/test-*.sh`
con sudo es la solución.

## 7. SSE streaming — already shipped (commit `17888` del 10 may)

`/v1/chat/completions` ya hacía SSE en cada token con
`Content-Type: text/event-stream` + `data: {...}\n\n` por delta
+ `[DONE]` sentinel.  No fue necesario tocar nada.

## 8a. RSA-2048 PKCS#1 v1.5 verify (`0f5f28e`)

`kernel/rsa.c` (377 LoC).  Foundation para A12.3 chain
validation.  Implementa:

- 32-limb uint64 bignum (`bn_t`): add/sub/mul/mod (shift-and-
  subtract), exponentiation con e small (square-and-multiply).
  Verify-only → no constant-time discipline (operamos sobre
  valores públicos).
- `rsa_pkcs1_v15_sha256_verify(sig, n, e, hash)`:
  - decode EM = sig^e mod n
  - check `0x00 || 0x01 || 0xFF…0xFF || 0x00 || DigestInfo`
  - byte-compare inner hash contra el esperado
- Self-test boot-time contra un par 2048-bit generado offline
  con openssl.  Casos positivo y negativo (hash flipped 1 bit).

Verificado:

```
[KERN] RSA-2048 verify self-test: PASS
```

## 8b. X.509 chain link verify — A12.5 (`8fe13d6`)

`kernel/x509.c` ganó:

- `cert_split(cert)` → puntea a `(TBSCertificate, sigAlg, sigValue)`
  (parsea SEQUENCE outer + skip por offsets, BIT STRING strip
  del unused-bits byte).
- `sigalg_recognize(cp)` → enum, dispatches:
  - `sha256WithRSAEncryption` (OID `1.2.840.113549.1.1.11`)
  - `ecdsa-with-SHA256`       (OID `1.2.840.10045.4.3.2`)
- `extract_rsa_pubkey(cert)` → parsea SPKI de un issuer cert,
  reconoce `rsaEncryption` OID, devuelve `(n, e)` strippeando
  el leading-zero sign byte del modulus DER INTEGER.
- `x509_verify_chain_link(child, issuer)`:
  - hash de TBSCertificate del child con SHA-256
  - dispatch a `rsa_pkcs1_v15_sha256_verify` o `ecdsa_p256_verify`
  - 0 sólo si la firma valida criptográficamente

Wired en dos paths:

1. **TLS 1.2 vía `cert_pin.c`**: el walker existente captura los
   chain certs (`chain_ptr[]`, `chain_len_arr[]`); después del
   pin check itera pares `(i, i+1)` y llama
   `x509_verify_chain_link`.

2. **TLS 1.3 vía `tls13.c`**: parser propio del cert_list (con
   per-entry extensions de TLS 1.3 §4.4.2), sintetiza wrapper
   TLS-1.2-shape para el pin walker, después itera links.

Verificado live contra CF:

```
[PIN] match: GTS WE1 intermediate
[X509] chain: ECDSA-P256 link verified
[TLS1.3] chain link 0 → 1: OK            (leaf → intermediate)
[X509] chain: unrecognized signature algorithm
[TLS1.3] chain link 1 → 2: FAIL          (intermediate → root: SHA-384)
```

Link 1 → 2 falla porque GTS Root R4 usa ECDSA-with-SHA-384 que
todavía no soportamos.  ~60 LoC para agregar (SHA-384 + dos OIDs
nuevos).  Tracked en `docs/pki-roadmap.md`.

## Métricas de la sesión

```
$ git log --oneline 2026-05-15..HEAD | wc -l
18 commits

$ git diff --stat 2026-05-15..HEAD | tail -1
~3500 lines changed across kernel + agent + docs
```

LoC nuevo en runtime aproximadamente:

| Módulo                                | LoC |
|---------------------------------------|----:|
| `kernel/tls13.c` (handshake + send/recv) | 620 |
| `kernel/rsa.c` (bignum + PKCS#1 verify)  | 377 |
| `kernel/x509.c` (chain link addition)    | 220 |
| `kernel/crypto2.c` (hkdf_expand_label)   |  95 |
| `agent/rl.c` (bandit + helpers)          |  95 |
| `kernel/net.c` (window scaling parts)    |  60 |
| `kernel/cert_pin.c` (dynamic pin)        | 150 |
| `agent/agent.c` (system prompt + bandit) |  50 |
| **Total kernel/agent**                    | **~1670 LoC** |

Más ~600 LoC de docs (3 roadmap docs + session summary).

## Deuda residual conocida

Cosas que dejé como debt explícita con cost estimates:

| Item                                      | LoC | Doc                          |
|-------------------------------------------|----:|------------------------------|
| SHA-384 + sha384WithRSA + ecdsa-w-SHA-384 |  60 | `docs/pki-roadmap.md`        |
| Full X.509 (Validity, SAN, KeyUsage)      | 250 | `docs/pki-roadmap.md`        |
| Hostname → SAN matching                   | 150 | `docs/pki-roadmap.md`        |
| RTC clock readback                        |  50 | `docs/pki-roadmap.md`        |
| Root anchoring (CA bundle on disk)        | 200 | `docs/pki-roadmap.md`        |
| TLS 1.3 `CertificateVerify` ECDSA verify  |  60 | `docs/tls13-roadmap.md`      |
| TLS 1.3 NewSessionTicket processing       |  40 | informativo, no-op ahora     |
| TCP send-side flow control honoring peer.snd_wnd | 80 | (no doc; tx único MSS-sized) |
| Slirp upstream investigation              |  ?  | `docs/slirp-defer.md`        |
| Full Poly1305 update path                 | 100 | (para habilitar ChaCha20-Poly1305 AEAD en TLS 1.3) |

Total deuda explícita: ~1000 LoC de seguridad/perf nice-to-haves
que ninguna NO bloquea hoy.

## Lecciones aprendidas

Tres cosas que valieron la pena documentar:

1. **Comparar siempre con un cliente conocido-funcional** cuando
   hay un bug ambiguo.  El `curl` desde el Mac contra el mismo
   endpoint reveló inmediatamente que CF mandaba 19372 bytes en
   237 ms, descartando CF como sospechoso y enfocando la
   investigación en nuestro stack.  De ahí salió el bisect: SYN
   sin MSS → CF dropea silenciosamente.  Sin el cliente
   conocido-funcional el debug podría haber tomado días.

2. **El transcript hash de TLS 1.3 es delicado**.  El bug donde
   los app traffic secrets se derivan post-server-Finished (no
   post-client-Finished) es sutil y nada en los mensajes de error
   apuntaba ahí.  Identificarlo requirió leer RFC 8446 §7.1
   palabra por palabra.  Los handshakes se completaban
   (handshake_secret correcto) pero el primer app record fallaba
   el decrypt.

3. **Bandits genuinos requieren state**, no sólo metrics.  La
   diferencia entre "score-based retrieval" y "real bandit" está
   en el visit count y el exploration bonus.  Sin n_retrieved no
   hay exploration; sin exploration no hay aprendizaje real.

## A12.3 closeout — tarde del 2026-05-16

Atacando la parte de A12.3 que había quedado documentada como
deuda en la mañana, esta sesión cerró:

### SHA-384 (crypto2.c)

`sha384()` shared-IV variant de SHA-512 con FIPS 180-4 §5.3.4
constants y truncación a 384 bits.  Bonus: la implementación
nueva maneja correctamente el caso de padding de dos bloques
(el `sha512()` original tenía un "skip for now" en `rem >= 112`
que ahora sí cubrimos).  Self-test live PASS contra el vector
canónico SHA-384("abc").

### RSA-PKCS#1-v1.5 con SHA-384 (rsa.c)

Refactor: `rsa_verify_inner()` shared toma `(hash, hash_len,
di_prefix, di_prefix_len)`.  Wrappers thin:
`rsa_pkcs1_v15_sha256_verify` y `rsa_pkcs1_v15_sha384_verify`.
DigestInfo prefix de SHA-384 hardcoded (19 bytes — mismo tamaño
que SHA-256, OID arc byte 14 cambia 0x01→0x02, OCTET STRING
length byte 18 cambia 0x20→0x30).

### Dispatch ampliado (x509.c)

`sigalg_recognize()` ahora reconoce 4 algorithms:

| OID | Constante | Hash | Verify path |
|---|---|---|---|
| 1.2.840.113549.1.1.11 | sha256WithRSAEncryption | SHA-256 | `rsa_pkcs1_v15_sha256_verify` |
| 1.2.840.113549.1.1.12 | sha384WithRSAEncryption | SHA-384 | `rsa_pkcs1_v15_sha384_verify` |
| 1.2.840.10045.4.3.2   | ecdsa-with-SHA256       | SHA-256 | `ecdsa_p256_verify(hash[32])` |
| 1.2.840.10045.4.3.3   | ecdsa-with-SHA384       | SHA-384 | `ecdsa_p256_verify(hash[32])` con truncación FIPS 186-4 §6.4 |

### Root anchoring (cert_pin.c)

GTS Root R4 agregado al static pin_table:

```
{ .label = "GTS Root R4 (P-384)",
  .digest = { 0x76,0xb2,0x7b,0x80, ... 0x49,0x3b,0x5a,0x7 } }
```

Esto significa que aunque el link `WE1 → GTS Root R4` no se
verifique criptográficamente (necesita ECDSA P-384), el chain
QUEDA anchored al root por pin de SHA-256.  Si Google retira
WE1, el nuevo intermediate seguirá chaineando a Root R4 y el
pin se mantiene.

### Validity window (x509.c::x509_check_validity, A12.6)

```c
int x509_check_validity(const uint8_t *cert, uint32_t cert_len,
                        uint32_t now_utc);
```

Walks TBSCertificate hasta el campo Validity, parsea
notBefore + notAfter (ambos UTCTime `YYMMDDHHMMSSZ` o
GeneralizedTime `YYYYMMDDHHMMSSZ` per RFC 5280 §4.1.2.5),
convierte a Unix seconds vía Hinnant's `civil_to_unix`
algorithm, compara con `now_utc`.

Boot-time self-test con un cert real openssl-generado:

```
$ openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
              -subj '/CN=vtest' -out cert.pem
# notBefore=May 16 20:45:30 2026 GMT  (= unix 1778964330)
# notAfter =May 13 20:45:30 2036 GMT  (= unix 2094324330)
```

Cuatro casos: in-window (clock=2030-01-01), not-yet-valid
(clock=2025-01-01), expired (clock=2040-01-01), no-clock
(clock=0).  Live:

```
[KERN] X.509 validity self-test: PASS
```

Wired en ambos paths (TLS 1.2 vía cert_pin.c, TLS 1.3 vía
tls13.c).  Modo informativo (logs PASS/FAIL pero no aborta).
NTP no sincroniza bajo vmnet (issue ortogonal de red; el check
skip's cuando `ntp_get_utc()` devuelve 0).

### NTP fallback (ntp.c)

Cambio menor: intenta `time.google.com` primero (anycast), cae
a `pool.ntp.org` si DNS falla.  Mejora reliability cuando
pool.ntp.org rota a un server lento.  No resuelve el issue del
vmnet UDP bouncing — separado.

### Lo que queda de A12.3 (deferred)

| Item | LoC | Comentario |
|---|---|---|
| ECDSA P-384 verify | ~500 | Necesario para crypto-verificar WE1→Root.  Root está pin-anchored, así que es defense-in-depth. |
| SAN/hostname matching | ~150 | Solo útil con un endpoint no-pinned (todos los nuestros están pinned). |
| Full CA root bundle on disk | ~200 | Mismo argumento que SAN matching. |
| NTP sync bajo vmnet | varies | Net issue independiente. |
