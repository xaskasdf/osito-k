# OsitoK Kernel — Diagramas

5 diagramas Mermaid cubriendo arquitectura, boot, dispatch de syscalls, forward pass de inference y el mapa de integración de las 10 features demenciales. Renderizables directamente en GitHub, VS Code, Obsidian y cualquier viewer Markdown con soporte Mermaid.

---

## 1. Arquitectura — 111 archivos agrupados en 12 subsistemas

```mermaid
graph TB
    subgraph BOOT["UEFI Boot"]
        A1[boot.efi<br/>gnu-efi loader]
        A2[kernel_entry<br/>main.c]
    end

    subgraph CORE["Core — memoria + excepciones"]
        M[memory.c · paging.c<br/>heap.c · slab.c]
        TA[tensor_arena.c<br/>512MB / 2MB pages]
        I[idt.c · isr_stubs.S]
        EX[panic · crash_report<br/>coredump · hwbp]
    end

    subgraph SCH["Scheduler + SMP"]
        P[process.c<br/>sched_rt.c]
        PR[pred_sched.c<br/>Markov]
        SM[smp.c · smp_work.c<br/>kthread · workqueue · rcu]
    end

    subgraph SYS["Syscalls + VDSO"]
        SC[syscall.c<br/>syscall_entry.S]
        SI[sys_inference.c<br/>530-534]
        VD[vdso_thunks.c]
        IPC[shm · sysv_ipc · pty<br/>socket · io_uring<br/>pipe · futex]
    end

    subgraph FSX["VFS + 12 Filesystems"]
        V[vfs.c + io_predict.c]
        FSS[osfs2/3 · fat32 · tmpfs<br/>ext2/3/4 · iso9660 · exfat<br/>ntfs · udf · sqfs · hfs+<br/>btrfs · apfs]
    end

    subgraph DRV["Drivers"]
        ST[nvme · ahci · virtio_blk<br/>usb_storage]
        NC[i211 + SG · virtio_net]
        IN[xhci · keyboard · evdev<br/>input_events]
        AU[hda audio]
        GP[gpu · gsp · sass · gmmu<br/>gpu_tensor · inference<br/>display]
    end

    subgraph NET["Network stack"]
        NS[net.c ARP/TCP/UDP]
        NP[dhcp · ntp · mdns · ipv6]
        NT[tls · tls13 · http<br/>sshd · netfilter]
    end

    subgraph CMP["Compute + Inference"]
        CF[cpu_features.c]
        DP[dispatch.c]
        TE[tensor.c · tensor_avx2.c]
        INF[inference.c<br/>llama_forward]
        TK[tokenizer.c]
        PF[perf.c · PMU counters]
    end

    subgraph UI["Display + UI"]
        FB[framebuffer · display]
        CP[compositor · wayland]
        SH[terminal · shell · vt]
    end

    subgraph CT["Binary compat"]
        EL[elf.c · dynlink.c]
        W3[win32/pe + 15 DLL shims]
        DO[dos/cpu8086 · DPMI]
    end

    subgraph OB["Observabilidad + seguridad"]
        KP[kprof · trace · strace<br/>klog · psi · lockdep · oom]
        SO[self_optimize.c]
        CA[caps · ns · seccomp<br/>crypto · crypto2 · random]
    end

    A1 --> A2
    A2 --> CORE
    A2 --> SCH
    A2 --> DRV
    A2 --> NET
    A2 --> CMP

    DRV --> FSX
    DRV --> NET
    DRV --> UI
    DRV --> CMP

    FSX --> SYS
    IPC --> SYS
    UI --> SYS
    CT --> SYS
    CMP --> SYS

    CF -.->|AVX2| DP
    CF -.->|CR4.PCE| PF
    DP -.->|matvec| INF
    TA -.->|scratch + KV| INF
    PF -.->|phase tag| INF
    PR -.->|hook| P
    V -.->|open hook| FSS
    NC -.->|SG when len >= 256| NS
    EX -.->|DR0-3| I
    SO -.->|register| DP
```

Las líneas punteadas son integraciones cross-subsistema de las 10 features recientes (ver §5).

---

## 2. Secuencia de boot — 22 pasos desde UEFI hasta el prompt server

```mermaid
graph TB
    S0[boot.efi carga kernel.elf<br/>pasa boot_info_t]
    S1[zero BSS · save boot_info<br/>copy UEFI memmap]
    S2[serial_init · enable_sse<br/>fb_init]
    S3[cpu_features_detect<br/>CPUID · CR4.OSXSAVE · XCR0]
    S4[perf_init<br/>PMU MSRs · CR4.PCE]
    S5[dispatch_init<br/>cpu_dispatch_t + 3 self_opt sites]
    S6[mem_init · sys_caps_init]
    S7[idt_init<br/>APIC timer · exception vectors]
    S8[paging_init<br/>PML4 · upper-half mirror]
    S9[paging_setup_pat<br/>WC framebuffer · shadow FB]
    S10[heap_init<br/>fpu_percpu_init]
    S11[smp_init<br/>INIT-SIPI-SIPI APs<br/>each AP: perf_init_ap]
    S12[smp_work_init<br/>work-stealing deques]
    S13[syscall_init<br/>vdso_init · vdso_thunks_init]
    S14[proc_init · dl_init<br/>win32_init]
    S15[crypto_selftest parallel<br/>pci_scan on BSP]
    S16[tensor_benchmark on AP<br/>BAR mapping on BSP]
    S17[GPU probe · gpu_init<br/>NVMe init · osfs2/3 mount]
    S18[gguf_load<br/>tok_init from embedded tokenizer]
    S19[tensor_arena_init 512MB<br/>on 256 x 2MB superpages]
    S20[llama_init<br/>scratch+KV from arena]
    S21[network stack<br/>DHCP · NTP · mDNS · IPv6]
    S22[init shell · main loop<br/>UDP 7777 prompt server]

    S0 --> S1 --> S2 --> S3 --> S4 --> S5 --> S6 --> S7 --> S8 --> S9 --> S10
    S10 --> S11 --> S12 --> S13 --> S14 --> S15 --> S16 --> S17 --> S18 --> S19
    S19 --> S20 --> S21 --> S22

    style S3 fill:#4a9eff,color:#fff
    style S4 fill:#4a9eff,color:#fff
    style S5 fill:#4a9eff,color:#fff
    style S19 fill:#4a9eff,color:#fff
    style S20 fill:#4a9eff,color:#fff
```

Los pasos azules son inserciones del commit de las 10 features. S11+S16 corren en paralelo (BSP + AP) gracias a `smp_submit_any`.

---

## 3. Dispatch de syscalls — camino completo ring3 → ring0 → ring3

```mermaid
sequenceDiagram
    autonumber
    participant U as User ELF
    participant SE as syscall_entry.S
    participant SD as syscall_dispatch
    participant MEMO as memo cache
    participant H as Handler
    participant VDSO as VDSO page

    U->>U: SYSCALL (Linux ABI:<br/>rax=nr · rdi rsi rdx r10 r8 r9)
    U->>SE: trap to ring 0 via LSTAR MSR
    SE->>SE: swap gs · save user RSP<br/>push frame
    SE->>SD: syscall_dispatch(nr, a1..a5)
    alt nr in {uname, getcwd, fstat, stat}
        SD->>MEMO: lookup(nr, hash)
        MEMO-->>SD: hit -> return cached
    end
    SD->>H: switch(nr)
    Note over H: 130+ handlers incl<br/>read/write/mmap (1-12)<br/>fork/exec/wait (39/59/61)<br/>socket (41)<br/>shm* (500-506)<br/>batch (520) cmdring (521)<br/>inference (530-534)
    H-->>SD: int64_t ret
    SD-->>SE: return value in rax
    SE->>SE: restore user frame<br/>swap gs back
    SE-->>U: SYSRET
    U->>VDSO: (skip) read perf / time<br/>direct load from 0x7FFFE000
    VDSO-->>U: zero-syscall time/perf
```

Caveats: syscalls memoizables (`uname`, `getcwd`, `fstat`, `stat`) cortocircuitan antes del handler cuando el buffer destino y los argumentos coinciden con un hit cacheado.

---

## 4. Forward pass de Llama 3.2 1B — 16 capas + sample

```mermaid
graph TB
    TOK[input token id<br/>uint32_t]
    EMB[embed_token<br/>x = tok_embd rows token]
    PH[perf_phase_enter<br/>PERF_PHASE_FORWARD]

    subgraph LAYER["Transformer layer · 16 iters"]
        RN1[rmsnorm xb · x · attn_norm]
        QKV{disp.matvec_q4_0<br/>via table}
        Q[Q = attn_q · xb · dim]
        K[K = attn_k · xb · kv_dim on AP]
        V[V = attn_v · xb · kv_dim on AP]
        RO[RoPE Q,K · pos · 500000]
        KV[write KV cache<br/>kv_cache l from arena]
        GQA[Grouped Query Attention<br/>8 kv heads · 4:1 ratio<br/>scaled dot-product · softmax]
        AO[attn_output matvec]
        RES1[residual: x += xb]
        RN2[rmsnorm FFN input]
        SW[SwiGLU<br/>gate BSP · up AP<br/>silu gate * up]
        DN[ffn_down matvec]
        RES2[residual: x += xb]
        PRE[__builtin_prefetch<br/>next layer weights]
    end

    FN[final rmsnorm]
    LG[logits = output · x<br/>vocab_size = 128256]
    PE[perf_phase_exit]
    SAMP[sample_topp<br/>temperature · top_p 0.9]
    NT[next token id]

    TOK --> PH --> EMB --> RN1
    RN1 --> QKV
    QKV --> Q
    QKV --> K
    QKV --> V
    Q --> RO
    K --> RO
    V --> KV
    RO --> KV --> GQA --> AO --> RES1 --> RN2 --> SW --> DN --> RES2 --> PRE
    PRE -.->|l+1| RN1
    RES2 -->|loop done| FN --> LG --> PE --> SAMP --> NT

    style QKV fill:#4a9eff,color:#fff
    style PH fill:#7aff7a,color:#000
    style PE fill:#7aff7a,color:#000
    style KV fill:#ffaa4a,color:#000
```

Nodos coloreados: azul = dispatch table (Fase 3), verde = tags de PMU (Fase 1), naranja = memoria de la arena superpage (Fase 2). QKV/FFN large matvecs se paralelizan K/V en APs mientras el BSP procesa Q, y similar gate/up en FFN.

---

## 5. Mapa de integración de las 10 features

```mermaid
graph LR
    subgraph BOOT["Boot path"]
        KE[kernel_entry<br/>main.c]
    end

    subgraph P0["Phase 0 cpu_features"]
        F0[cpu_features.c<br/>CPUID cache PMU]
    end
    subgraph P1["Phase 1 perf"]
        F1[perf.c<br/>3 fixed + 4 PMC<br/>CR4.PCE]
    end
    subgraph P2["Phase 2 tensor_arena"]
        F2[tensor_arena.c<br/>512MB · 256 x 2MB]
    end
    subgraph P3["Phase 3 dispatch"]
        F3[dispatch.c<br/>disp table + ERMS memcpy]
    end
    subgraph P4["Phase 4 sys_inference"]
        F4[sys_inference.c<br/>syscalls 530-534]
    end
    subgraph P5["Phase 5 ASLR-lite"]
        F5[elf.c stack jitter<br/>hw_random64 · 0-4080B]
    end
    subgraph P6["Phase 6 pred_sched"]
        F6[pred_sched.c<br/>Markov 64x4]
    end
    subgraph P7["Phase 7 io_predict"]
        F7[io_predict.c<br/>pattern table]
    end
    subgraph P8["Phase 8 SG TX"]
        F8[i211.c i211_send_sg<br/>descriptor chaining]
    end
    subgraph P9["Phase 9 hwbp"]
        F9[hwbp.c DR0-DR3<br/>shell watch/unwatch]
    end
    subgraph P10["Phase 10 self_opt"]
        F10[self_optimize.c<br/>dry-run branch registry]
    end

    KE --> F0
    F0 -->|avx2/fma flags| F3
    F0 -->|rdrand/pmu| F1
    F0 -->|rdrand| F5

    F1 -->|phase_enter/exit| INFH[inference.c<br/>llama_forward]
    F2 -->|tensor_arena_alloc| INFH
    F3 -->|matvec_q4_0 q8_0| INFH

    F4 -->|syscall 530| SYSH[syscall.c<br/>dispatch 530-534]
    F4 -->|uses| INFH

    F5 -->|load_bias| ELFH[elf.c<br/>proc_exec]

    F6 -->|pred_record<br/>pred_prewarm| SCHH[process.c<br/>sched_tick]
    F7 -->|io_predict_observe| VFSH[fs/vfs.c<br/>vfs_find]

    F8 -->|frags 2| NETH[net.c<br/>net_udp_send]

    F9 -->|hwbp_dispatch| IDTH[idt.c<br/>DB vector 1]

    F3 -->|register 3 sites| F10

    style P0 fill:#4a9eff,color:#fff
    style P1 fill:#4a9eff,color:#fff
    style P2 fill:#4a9eff,color:#fff
    style P3 fill:#4a9eff,color:#fff
    style P4 fill:#ff7a4a,color:#fff
    style P5 fill:#ff7a4a,color:#fff
    style P6 fill:#7aff7a,color:#000
    style P7 fill:#7aff7a,color:#000
    style P8 fill:#aa7aff,color:#fff
    style P9 fill:#ffff7a,color:#000
    style P10 fill:#ff7a7a,color:#000
```

Código de colores por capa de stack:

| Color | Capa | Fases |
|-------|------|-------|
| 🔵 azul | Compute / boot-time | 0 · 1 · 2 · 3 |
| 🟠 naranja | Process + syscall | 4 · 5 |
| 🟢 verde | Scheduler / FS | 6 · 7 |
| 🟣 violeta | Networking | 8 |
| 🟡 amarillo | Debug | 9 |
| 🔴 rojo | Self-modify | 10 |

---

## Ver también

- [`docs/kernel-demencial.md`](kernel-demencial.md) — descripción detallada de las 10 features
- [`docs/kernel-architecture.md`](kernel-architecture.md) — arquitectura previa (pre-features)
- [`docs/kernel-separation.md`](kernel-separation.md) — split boot.efi + kernel.elf
- [`docs/x86-features-detail.md`](x86-features-detail.md) — catálogo de X9-X42 + X-OS* + X-NET*
