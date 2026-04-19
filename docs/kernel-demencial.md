# Kernel Demencial — 10 Features Avanzadas

**Status**: implementadas (build exitoso con `x86_64-elf-gcc 15.2`, `make build/kernel.elf`). Fecha: 2026-04-19.

Convierten OsitoK de "un OS que corre inference" en "un inference runtime que también es un OS": el kernel, scheduler, FS y network se optimizan para la workload primaria (transformer forward pass) y exponen esa capacidad como primitiva del sistema.

Plan maestro: [`~/.claude/plans/ya-estamos-en-territorio-soft-sparrow.md`](../../../.claude/plans/ya-estamos-en-territorio-soft-sparrow.md) (local).

---

## Resumen por fase

| Fase | Feature | Archivos nuevos | LOC | Shell cmd |
|------|---------|-----------------|-----|-----------|
| 0 | CPU feature detection | `cpu_features.h/c` | 230 | `cpu` |
| 1 | Hardware PMU counters | `perf.h/c` | 280 | `perf`, `perf reset`, `perf snapshot` |
| 2 | Superpage tensor arena | `tensor_arena.h/c` | 140 | (via `mem`) |
| 3 | CPUID multipath dispatch | `dispatch.h/c` | 120 | (boot log) |
| 4 | sys_inference syscall | `sys_inference.c` | 230 | (via userspace) |
| 5 | ASLR lite (stack jitter) | (en `elf.c`) | +15 | (boot log) |
| 6 | Predictive scheduling | `pred_sched.h/c` | 210 | `pred`, `pred reset` |
| 7 | Speculative I/O prefetch | `io_predict.h/c` | 220 | `io_predict`, `io_predict reset` |
| 8 | Zero-copy scatter-gather TX | (en `i211.c`) | +60 | (usado por net stack) |
| 9 | Hardware breakpoints | `hwbp.h/c` | 210 | `watch`, `unwatch`, `hwbp` |
| 10 | Self-optimizing (dry-run) | `self_optimize.h/c` | 160 | `self_opt`, `self_opt apply` |

**Total**: 18 archivos nuevos (9 .c, 9 .h), ~1,900 LOC nuevas + ~500 modificadas. Impacto en `kernel.elf`: +~30 KB text.

---

## Fase 0 — `cpu_features`

Centraliza CPUID + cache hierarchy + PMU capability en un struct global. Reemplaza las 3 copias dispersas de `rdmsr`/`wrmsr` y el bloque AVX2-detect que estaba en `tensor.c`. Enables CR4.OSXSAVE y XCR0 una vez por boot.

**API clave** (`arch/x86/include/cpu_features.h`):
```c
extern cpu_features_t cpu_features;
void cpu_features_detect(void);
void cpu_features_dump(void);
uint64_t hw_random64(void);          // RDRAND o fallback TSC-mix
```

**Integración**: llamado en `main.c:kernel_entry()` inmediatamente después de `serial_init()`, antes de cualquier uso de AVX/PMU.

**Struct**: vendor, brand, todos los flags SIMD (SSE*, AVX*, FMA, F16C, BMI2, ERMS), paging (PGE/PSE/PAE/1GB pages/la57), PMU (version, num counters, width), cache (L1d/L2/L3 sizes + line size), topology (cores, HT).

---

## Fase 1 — Hardware PMU

Programa 3 contadores fijos (instructions/cycles/ref_cycles) + 4 PMCs (L1d/L2/TLB/branch misses) via MSRs Intel. Habilita `CR4.PCE` para permitir `RDPMC` desde userspace sin syscall.

**API** (`arch/x86/include/perf.h`):
```c
void perf_init(void);
void perf_init_ap(void);                        // cada AP vía smp_ap_entry
void perf_snapshot(perf_snapshot_t *out);       // ~40 ciclos
void perf_phase_enter(int slot, const char *name);
void perf_phase_exit(int slot);
```

**Phase slots** reservados: `PERF_PHASE_FORWARD=0` (llama_forward está instrumentado), QKV, ATTN, FFN, SAMPLE, NET_RX/TX, FS_READ, SYS_INFERENCE.

**Shell**: `perf`, `perf reset`, `perf snapshot`. Output ejemplo:
```
[PERF] Phase profiling:
  [llama_forward] calls=32 cycles=5.8G insns=7.4G IPC=1.28
    L1d_miss=72M L2_miss=3.1M TLB_miss=230K BR_miss=14M
```

**Uso**: valida impacto de Fase 2 (TLB miss rate debe caer ~2% → <0.01%) y Fase 3 (IPC debe subir).

---

## Fase 2 — Superpage Tensor Arena

Reserva 512 MB contiguos, alineados a 2 MB (256 superpages), desde `mem_alloc_aligned_high`. El upper-half mirror ya los mapea con `PTE_LARGE` (2 MB pages) — `paging_map_2m()` ya existía y lo hace automáticamente cuando alineación lo permite.

`inference.c` ahora alloca scratch (~110 MB) + KV cache (~33 MB) desde la arena. Fallback a `mem_alloc_pages` si la arena no está disponible (sistemas con poca RAM contigua).

**API** (`arch/x86/include/tensor_arena.h`):
```c
int  tensor_arena_init(tensor_arena_t *a, uint64_t size_mb);
void *tensor_arena_alloc(tensor_arena_t *a, uint64_t size, uint64_t align);
extern tensor_arena_t g_tensor_arena;
```

**Beneficio teórico**: matvec de 2048×2048 Q4_0 toca ~1.2 MB de weights = 300 páginas de 4KB (>> 64 TLB entries) → 1 superpage. Proyección: -0.6 a -2.4 ms/token por eliminación de TLB walks.

---

## Fase 3 — CPUID Multipath Dispatch

Tabla de punteros a función seleccionada al boot según `cpu_features`. `tensor.c:matvec_q4_0` ya hacía dispatch runtime AVX2/scalar (cached); esta fase agrega el patrón para futuros paths (AVX-512, BMI2) y **ERMS memcpy** (`rep movsb` — mucho más rápido que loop software para ≥64B).

**API** (`arch/x86/include/dispatch.h`):
```c
extern cpu_dispatch_t disp;           // slots: matvec_q4_0/q8_0, rmsnorm,
                                       // vec_add/mul, memcpy_fast, memset_fast
void dispatch_init(void);              // llamar después de cpu_features_detect
```

**Boot log**:
```
[DISPATCH] compute path: AVX2+FMA+F16C +ERMS-memcpy
```

---

## Fase 4 — sys_inference Syscall

Inference del kernel expuesta como syscalls **530-534** (evita colisión con `SYS_SHM_*` 500-506). Cualquier ELF compilado con `ositok.h` puede generar tokens:

```c
#include "ositok.h"

uint32_t prompt[] = { 128000, 1234, 5678 };
uint32_t out[128];
long n = oi_inference(prompt, 3, out, 128, 0.7f);  // temperature 0.7

char text[1024];
oi_detokenize(out, n, text, sizeof text);
puts(text);
```

**Syscalls**:
| nr | nombre | función |
|----|--------|---------|
| 530 | `SYS_INFERENCE` | forward + sample loop, devuelve # tokens |
| 531 | `SYS_INFERENCE_RESET` | rewind KV cache (nueva conversación) |
| 532 | `SYS_INFERENCE_STATE` | query vocab_size, ctx_len, pos |
| 533 | `SYS_INFERENCE_TOKENIZE` | BPE encode (usa `g_tokenizer`) |
| 534 | `SYS_INFERENCE_DETOKENIZE` | token[] → UTF-8 |

**Serialización**: spinlock `cmpxchg` — solo un proceso a la vez. Yield cada 8 tokens para que el resto del sistema siga respirando.

**Validación**: chequeo básico de user pointers (no-NULL, no wrap, < kernel-half). Full page-walk validation queda como follow-up.

---

## Fase 5 — ASLR Lite (Stack Jitter)

En `elf.c`, antes de armar argv/envp, decrementa el stack top por `hw_random64() & 0xFF) * 16` bytes (0-4080, siempre 16-aligned para System V ABI). No es para seguridad — es para **cache-set diversification** entre procesos concurrentes y surface bugs layout-dependientes en código de usuario.

```c
uint64_t sp_jitter = (hw_random64() & 0xFF) * 16;
uint64_t sp = (uint64_t)stack_base + USER_STACK_SIZE - sp_jitter;
```

Requiere que `cpu_features_detect()` haya corrido antes (usa `cpu_features.rdrand`).

---

## Fase 6 — Predictive Scheduling (Markov)

Modelo bigram 64-bucket × 4-way hasheado por `(prev_pid, trigger)`. En cada context switch:
1. `pred_record(cur, next, trigger)` — entrena
2. `pred_next(next, QUANTUM)` — predice el siguiente-siguiente
3. `pred_prewarm(predicted)` — prefetch kernel_rsp (3 líneas) + FPU state (8 líneas) vía `prefetcht0`/`prefetcht1`

Integrado en `process.c:sched_tick` justo antes de `sched_switch_rsp = next->kernel_rsp`.

**Accesors opacos** añadidos a `process.c`: `proc_find_ptr`, `proc_kernel_rsp`, `proc_fpu_state_ptr` — para evitar exponer `process_t` fuera.

**Shell**: `pred` muestra top 10 transiciones + accuracy; `pred reset` limpia.

Solo devuelve predicción si >60% de las observaciones apuntan al mismo destino y total ≥ 4 muestras. Sin esto, las primeras transiciones ruidosas desperdiciarían cache.

---

## Fase 7 — Speculative I/O Prefetch

Tabla 64-pattern × 4-predicción que captura secuencias `"A → B"` en opens observados por `vfs_find()`. Cuando un proceso abre A, los B con >50% confidence se prefetchan en APs vía `smp_submit_ff`.

Ejemplo: TCC abre `foo.c` y después siempre `foo.h`, `stdio.h`, etc. Con pattern cacheado, para cuando TCC parse `#include <stdio.h>` el header ya está en RAM.

**Hook**: `vfs_find()` llama `io_predict_observe(path)` justo antes de devolver éxito (v2 o v3 branch).

**Shell**: `io_predict` lista top 10 patrones; `io_predict reset` limpia.

LRU replacement: el bucket menos recientemente usado se sobrescribe cuando se llena. Solo prefetch files ≤ 64 KB.

---

## Fase 8 — Zero-Copy Scatter-Gather TX (I211)

Nuevo `i211_send_sg(const uint64_t phys[], const uint32_t lens[], int n)` usa **descriptor chaining legacy**: EOP=0 en todos los fragmentos salvo el último. El NIC concatena por DMA sin `memcpy` intermedio.

Ejemplo: prompt server UDP responde 2 KB de tokens decodificados — antes hacía `memcpy(tx_buf, response, len)`. Ahora:
```c
uint64_t frags[2] = { VIRT_TO_PHYS(hdr), VIRT_TO_PHYS(response) };
uint32_t lens[2]  = { 42, resp_len };
i211_send_sg(frags, lens, 2);
```

Bound: n_frags ≤ 4, total ≤ MTU (2048). Solo kernel virtual addresses por ahora — user pages requieren pinning (follow-up).

`i211_send` (legacy, con memcpy) se mantiene como fallback.

---

## Fase 9 — Hardware Breakpoints (DR0-DR3)

4 slots de watchpoints. Cada slot puede trigger en: execute fetch, write, read+write, o I/O port. Dispara `#DB` (vector 1) que lee `DR6` para saber cuál slot.

**Integración en `idt.c:896`**: `hwbp_dispatch(frame)` es el primer handler dentro de `if (vec == 1)`. Si matchea, retorna; sino, fall through al código PE32 compat32 existente.

**Shell**:
```
> watch 0xFFFF800001234500 write 8    # slot auto-asignado
Set hwbp slot 0 on 0xFFFF800001234500
> hwbp                                  # lista slots activos + hits
  [0] write 8B @ 0xFFFF800001234500 "0xFFFF800001234500" hits=127
> unwatch 0
Cleared hwbp slot 0
```

**Uso típico**: debuggear data corruption. Ejemplo real: `watch sched_switch_rsp write 8` para ver quién escribe ese global si el scheduler se corrompe.

`RFLAGS.RF` se setea antes de IRETQ para no re-trigger en la misma instrucción.

---

## Fase 10 — Self-Optimizing Kernel (Dry-Run v1)

Registro de "static branches" — runtime checks que se vuelven permanentes después del boot (ej. `if (cpu_features.avx2)` nunca cambia). Después de tick 1000 con `self_opt_enabled=true`, `self_opt_apply()` rewrite esos branches a JMP/NOP.

**V1 es DRY-RUN**: acepta registraciones, lista lo que rewritearía. El rewriting real está gated detrás de `SELF_OPT_PATCH=1` + requiere `paging_text_make_writable/readonly` helpers que no existen aún. Esto permite que la API se estabilize y crezca la lista de sites antes de tocar `.text`.

**API** (`arch/x86/include/self_optimize.h`):
```c
void self_opt_register_branch(void *site, const char *desc);
void self_opt_apply(void);
void self_opt_undo_all(void);
extern bool self_opt_enabled;           // OFF por defecto
```

**Shell**:
```
> self_opt                   # lista sites registrados
> self_opt enable
> self_opt apply             # dry-run en v1
> self_opt undo
```

Strategies #2-#4 (function cloning, proctab reorder, ML-guided) — follow-up.

---

## Boot flow tras todas las fases

```
[OsitoK] Serial initialized (COM1 115200)
[CPU] Intel Core i7-9xxx (or whatever)
[CPU] Flags: SSE2 SSE4.2 AVX AVX2 FMA F16C BMI2 ERMS RDRAND TSC_INV
[CPU] Cache: L1d=32KB L2=256KB L3=16MB line=64
[CPU] PMU v4 (4 programmable + 3 fixed, 48-bit)
[PERF] PMU enabled (3 fixed + 4 PMC) ver=4
[DISPATCH] compute path: AVX2+FMA+F16C +ERMS-memcpy
...
[TENSOR-ARENA] 512 MB on 256 superpages: phys=0x... virt=0xFFFF800001000000
[LLAMA] KV cache: 33 MB (16 layers x 256 seq x 512 kv_dim)
[TENSOR-ARENA] post-llama_init: 151040 KB used / 524288 KB total (peak 151040 KB) 256 superpages
```

## End-to-end verification

Desde shell OsitoK:

```
> cpu                          # Phase 0
> perf                         # Phase 1 baseline
> chat "hola"                  # genera tokens via UDP server interno
> perf inference               # ver L1d miss rate, IPC
> perf reset                   # contadores a cero
> pred                         # Phase 6 transiciones
> io_predict                   # Phase 7 patterns después de abrir archivos
> hwbp                         # Phase 9 slots libres
> watch sched_switch_rsp write 8
> [haz trabajo, observa hits]
> self_opt                     # Phase 10 sites registrados (vacío por ahora)
```

Desde userspace (ELF compilado con `ositok.h`):

```c
#include "ositok.h"
int main(void) {
    uint32_t toks[128], out[64];
    int n_in = oi_tokenize("Explica QEMU en 2 lineas:", toks, 128);
    int n_out = oi_inference(toks, n_in, out, 64, 0.7f);
    char text[512];
    oi_detokenize(out, n_out, text, sizeof text);
    puts(text);
    return 0;
}
```

Compilar: `cc test_inf.c -o test_inf.elf` dentro del OsitoK shell (TCC in-OS) o con host TCC. Correr: `./test_inf.elf`.

## Métricas esperadas vs baseline

| Métrica | Baseline | Objetivo | Validación |
|---------|----------|----------|------------|
| ms/token (Llama 3.2 1B) | 1860 | <1400 | `perf inference` + rdtsc |
| TLB miss rate (forward) | ~2% | <0.01% | `perf` L1d/TLB counters |
| Cache miss rate (scheduler) | ??? | -5 a -10% | `perf` alrededor de ctx_switch |
| Pred accuracy | n/a | >60% | `pred` accuracy line |

## Follow-up / no-scope

- **Fase 10 real .text patching** — necesita `paging_text_make_writable` helpers
- **Strategy 2-4 del self-opt** (function cloning, proctab reorder, ML-guided)
- **User pages pinning para sys_inference + SG TX** — actualmente sólo kernel buffers
- **PMU virtualization para procesos** — cada proceso lee sus propios counters via VDSO
- **NUMA-aware tensor arena** — una arena por socket en multi-socket
- **GPU PMU counters** — NVIDIA PM counters, plan separado
- **Sub-phase instrumentation** de `llama_forward` (QKV / ATTN / FFN / SAMPLE como slots separados)
