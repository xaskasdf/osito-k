# Bare-Metal AI OS — Research & Design Document

Proyecto experimental: sistema operativo bare-metal x86-64 orientado exclusivamente
a inferencia/entrenamiento de transformers, eliminando todo overhead del OS convencional.

Continuacion conceptual de OsitoK (bare-metal ESP8266) + NTransformer (CUDA puro sin
PyTorch/cuBLAS para inferencia de Llama 70B en consumer GPUs).

---

## 1. Motivacion

NTransformer ya elimino toda capa de software de ML — kernels CUDA puros, GEMV a mano,
streaming de capas PCIe con double-buffer, driver NVMe userspace. Pero sigue corriendo
sobre Linux, que introduce overhead innecesario para una maquina dedicada a AI:

| Bottleneck Linux          | Impacto                                                    | Solucion bare-metal                                      |
|---------------------------|------------------------------------------------------------|----------------------------------------------------------|
| Scheduler del kernel      | Context switches interrumpen transfers PCIe                | Un solo "proceso" — toda la CPU dedicada al orchestrator  |
| Page faults / TLB misses  | mmap del GGUF causa page faults aleatorios                 | Mapeo fisico directo, huge pages estaticas                |
| Syscall overhead          | cudaMemcpyAsync pasa por driver stack NVIDIA (ioctl)       | Driver GPU bare-metal: escribir directamente a BAR PCIe   |
| IOMMU/VFIO                | gpu-nvme-direct ya necesita VFIO para bypasear Linux       | Sin IOMMU — acceso directo a dispositivos PCIe            |
| IRQ routing               | Linux reparte interrupts entre cores                       | IRQs dedicados: GPU completion -> core especifico         |
| Memory allocator          | malloc/cudaMallocHost pasan por el kernel                  | Pool allocator sobre memoria fisica directa               |
| Filesystem VFS            | NVMe via bloque -> VFS -> ext4 -> page cache               | NVMe commands directos (ya existe en gpu-nvme-direct)     |

---

## 2. Arquitectura propuesta

```
+--------------------------------------------------------------+
|                     UEFI Firmware                              |
|  PCIe enumeration, memory map, GOP framebuffer, ACPI tables   |
+--------------------------+-----------------------------------+
                           | ExitBootServices()
                           v
+--------------------------------------------------------------+
|              NanoKernel (bare-metal, x86-64)                   |
|                                                                |
|  +----------+ +------------+ +-------------+ +-------------+  |
|  |   PCIe   | |  Physical  | |    APIC     | |   HPET /    |  |
|  |  Scanner  | |  Memory    | |    IRQ      | |   Timer     |  |
|  |  & Config | |  Manager   | |  Controller | |   Manager   |  |
|  +-----+----+ +------------+ +-------------+ +-------------+  |
|        |                                                       |
|  +-----v-------------------------------------------------+    |
|  |              GPU Driver (custom o GSP-shim)             |    |
|  |  - BAR0 MMIO mapping (16MB control registers)          |    |
|  |  - BAR1 VRAM aperture mapping                          |    |
|  |  - GPFIFO command submission (pushbuffer protocol)      |    |
|  |  - GPU page table setup (GMMU, 4-level)                |    |
|  |  - DMA engine control (H2D / D2H transfers)            |    |
|  |  - GSP firmware loading + RPC communication             |    |
|  +--------------------------------------------------------+    |
|  +----------+ +------------+                                   |
|  |  NVMe    | |  UART/Net  |                                   |
|  |  Driver  | |  Console   |                                   |
|  +----------+ +------------+                                   |
+--------------------------------------------------------------+
                           |
                           v
+--------------------------------------------------------------+
|           Transformer Engine (portado de NTransformer)          |
|                                                                |
|  - GGUF loader (directo de NVMe, sin filesystem)              |
|  - GEMV kernels (compilados a PTX/SASS)                       |
|  - Attention, RMSNorm, RoPE, SoftMax                          |
|  - Layer streamer (DMA directo GPU<->NVMe)                    |
|  - Tokenizer + Sampler                                         |
|  - Interfaz UART/Ethernet para recibir prompts                |
+--------------------------------------------------------------+
```

---

## 3. El problema central: Driver GPU NVIDIA

### 3.1 Opcion A: Blob Loading (GSP-shim approach)

Desde Turing (RTX 2000, 2018), NVIDIA incluye el **GSP** (GPU System Processor), un
core RISC-V dentro del die de la GPU que ejecuta el Resource Manager. El driver de
Linux es esencialmente un shim que:

1. Enumera PCIe, mapea BARs
2. Carga firmware GSP (~62MB binario) a memoria de la GPU
3. Arranca el core RISC-V
4. Se comunica con GSP-RM via message queues en shared memory + doorbell interrupts
5. GSP-RM maneja toda la complejidad del hardware internamente

**Que hay que shimear de Linux para cargar el blob:**

```
Tier 1 — Critico:
+-------------------+-------------------------------------------+----------------+
| Subsistema        | APIs a implementar                        | Complejidad    |
+-------------------+-------------------------------------------+----------------+
| PCI Enumeration   | pci_register_driver, pci_probe,           | Media          |
|                   | config space R/W, pci_enable_device       |                |
| MMIO Mapping      | ioremap, iounmap (mapear BARs fisicos)    | Baja           |
| DMA Allocation    | dma_alloc_coherent, dma_set_mask,         | Media          |
|                   | buffers fisicamente contiguos             |                |
| Interrupts        | request_irq, MSI/MSI-X setup, ISR        | Media-Alta     |
| Memory            | kmalloc, vmalloc, get_free_pages          | Media          |
| Firmware Load     | request_firmware (cargar GSP.bin)         | Media          |
+-------------------+-------------------------------------------+----------------+

Tier 2 — Operacional:
+-------------------+-------------------------------------------+
| Virtual Memory    | remap_pfn_range, vm_insert_page, vmap     |
| Timers            | jiffies, msleep, udelay                   |
| Locking           | mutex, spinlock, rwsem, completion        |
| Workqueues        | create_workqueue, queue_work              |
| Kernel Threads    | kthread_create, wake_up_process           |
+-------------------+-------------------------------------------+

Tier 3 — Feature completo:
+-------------------+-------------------------------------------+
| Char Devices      | /dev/nvidia* ioctl interface              |
| DRM               | Solo si queremos display output           |
| MMU Notifiers     | Para UVM (CPU/GPU memory coherency)       |
+-------------------+-------------------------------------------+
```

**Estimacion: ~10,000-30,000 lineas de shim code** para compute-only (sin display, sin UVM).

**Ventaja**: No necesitas entender el hardware de la GPU. GSP-RM maneja todo internamente.
El protocolo de comunicacion CPU-RM <-> GSP-RM esta documentado en nvidia-open.

**Desventaja**: Dependes del firmware binario propietario de NVIDIA. Cada version del
driver requiere su version de GSP firmware. Sin acceso al source del firmware, no puedes
optimizar ni debuggear problemas a nivel GPU.

### 3.2 Opcion B: Driver custom (reverse engineering)

Escribir tu propio driver que hable directamente con el hardware via MMIO/PFIFO,
sin depender del firmware GSP.

**Que necesitas implementar:**

```
1. Inicializacion GPU
   - Ejecutar DEVINIT scripts del VBIOS (documented en open-gpu-doc)
   - Configurar PLL/clocks del GPU
   - Inicializar controlador de memoria

2. PFIFO / Command Submission
   - Setup GPFIFO ring buffer en VRAM
   - Crear GP entries (8 bytes: address + size + flags)
   - Escribir pushbuffer commands (32-bit: method address + subchannel + data)
   - Manejar PBDMA engine (registers en BAR0)

3. GPU Memory Management (GMMU)
   - Page tables de 4 niveles (similar a x86-64)
   - Soporta 4KB, 64KB, 2MB, 512MB pages
   - Cada canal tiene su propio page directory

4. Compute Engine (PGRAPH)
   - Bind compute class al subchannel (e.g., TURING_COMPUTE_A = 0xc3c0)
   - Cargar shader programs (PTX compilado a SASS)
   - Configurar grid/block dimensions
   - Launch kernel via method writes
   - Manejar completion/fences

5. Copy Engine (CE)
   - DMA H2D y D2H para transfers de datos
   - Async copies via separate GPFIFO channel

6. Power/Clock Management
   - Reclocking: configurar PLLs para frecuencias altas
   - Sin esto, la GPU corre a boot clocks (rendimiento infimo)
   - ESTA ES LA PARTE MAS DIFICIL sin GSP
```

**Ventaja**: Control total. Puedes optimizar el driver para tu workload especifico
(inferencia de transformers), eliminar abstraction layers, hacer zero-overhead
command submission.

**Desventaja**: Esfuerzo monumental. El reclocking solo es un proyecto de anios
(Nouveau lleva 15+ anios y nunca lo completo para todas las generaciones).

### 3.3 Opcion C: Hibrida (GSP para init + custom para compute)

La opcion mas pragmatica:

1. Usar GSP firmware para inicializar la GPU (clocks, power, memory controller)
2. Una vez inicializada, hablar directamente con PFIFO/PGRAPH para compute
3. Saltarse el Resource Manager para command submission

**Ventaja**: Obtienes clocks altos (GSP maneja power management) y control directo
del compute path (minimo overhead en command submission).

**Desventaja**: Necesitas entender ambos: el protocolo GSP-RM Y el protocolo PFIFO.
Pero el segundo esta mucho mejor documentado (envytools + open-gpu-doc).

---

## 4. Diferencia fundamental: Blob vs Custom

```
                    BLOB (GSP-shim)              CUSTOM (reverse eng.)
                    ================             ====================

Control             Caja negra — GSP maneja      Control total del hw
                    todo internamente

Rendimiento         Overhead del RPC protocol    Zero-overhead commands
                    CPU-RM <-> GSP-RM via        directos al PFIFO
                    shared memory

Reclocking          GSP lo hace perfecto         Tienes que reversar PLLs,
                    (es su firmware real)        voltage regulators, etc.

Portabilidad        Firmware por generacion,     Si entiendes el protocolo
entre GPUs          NVIDIA publica para cada     PFIFO (estable desde Fermi),
                    una (Turing, Ampere, Ada,    el command submission es
                    Blackwell)                   identico entre generaciones

Debugging           Sin acceso al codigo GSP,    Debugging completo, puedes
                    errores son opacos           instrumentar cada register

Mantenimiento       Depende de NVIDIA            Depende de reverse eng.
                    publicando firmware           community (envytools,
                    compatible                   Nouveau, nvk)

Esfuerzo            ~10K-30K lineas de shim      ~50K-100K+ lineas,
estimado            (meses)                      years of work

Recomendacion       PRIMERA FASE                 FASE FUTURA (si se
                                                 justifica el esfuerzo)
```

---

## 5. RTX+ only: Estado del reverse engineering

Limitar el soporte a RTX (Turing) en adelante SIMPLIFICA enormemente las cosas,
porque todas estas generaciones comparten:

### 5.1 Que esta documentado para Turing+ (RTX 2000 -> 5000)

**Protocolo PFIFO / Command submission** — ESTABLE desde Fermi (2010):
- GPFIFO ring buffer: GP entries de 8 bytes apuntan a segmentos de pushbuffer
- Pushbuffer commands: 32 bits (method address + subchannel + count + data)
- PBDMA registers: `NV_PPBDMA_GP_BASE`, `GP_PUT`, `GP_GET`, `TOP_LEVEL_GET`
- Documentado en: envytools + NVIDIA open-gpu-doc (dev_pbdma.ref.txt para Volta/Turing)

**Lo que cambia entre generaciones (minor)**:
```
+--------------+--------------------+------------------------------------------+
| Generacion   | GPU class IDs      | Cambios vs anterior                      |
+--------------+--------------------+------------------------------------------+
| Turing       | TURING_A (0xc597)  | Baseline RTX. RT cores, tensor cores     |
| Ampere       | AMPERE_A (0xc797)  | Nuevos methods, misma ISA base           |
| Ada Lovelace | Extiende Ampere    | ISA 8.0 + extensiones, compatible atras  |
| Blackwell    | GB20x              | GSP firmware mas reciente, similar proto  |
+--------------+--------------------+------------------------------------------+
```

El protocol de submission es IDENTICO. Solo cambian:
- Class IDs (que "objeto" bindeas al subchannel)
- Nuevos methods dentro de cada class (RT, tensor ops)
- Encoding de instrucciones del shader compiler (ISA evoluciona)

### 5.2 NVK/Nouveau: Estado actual por generacion

```
+--------------+---------------+----------------+--------------------+
| Generacion   | NVK (Vulkan)  | Nouveau Kernel | GSP Firmware       |
+--------------+---------------+----------------+--------------------+
| Turing       | Vulkan 1.4    | Completo       | Disponible, opt-in |
| Ampere       | Vulkan 1.4    | Completo       | Disponible, opt-in |
| Ada Lovelace | Vulkan 1.4    | Completo       | Default            |
| Blackwell    | Vulkan 1.4    | Linux 6.16+    | Unico (no closed)  |
+--------------+---------------+----------------+--------------------+
```

**Dato clave para Blackwell**: NVIDIA elimino los kernel modules closed-source.
Los open kernel modules son la UNICA opcion. Esto valida el approach GSP-shim.

### 5.3 Que falta para compute bare-metal

Lo que NVK/Nouveau YA solucionaron y podemos reusar como referencia:
- [x] PCI BAR mapping y GPU identification
- [x] GSP firmware loading y comunicacion
- [x] GPFIFO channel creation y command submission
- [x] GPU page table setup (GMMU 4-level)
- [x] Compute shader dispatch (via Vulkan, pero el path de hw es el mismo)
- [x] Copy engine DMA transfers

Lo que NO aplica de NVK (graphics-only):
- [-] Display/KMS (no necesitamos)
- [-] 3D rendering pipeline
- [-] Ray tracing extensions
- [-] Video decode/encode

Lo que necesitamos pero no existe:
- [ ] Compilador PTX/SASS standalone (fuera de CUDA toolkit)
- [ ] Profiling sin NVIDIA tools

### 5.4 Limitar a RTX+ reduce problemas entre generaciones?

**Si, drasticamente**, por tres razones:

1. **GSP disponible en todas**: Todas tienen RISC-V GSP, el firmware se carga
   igual, el protocolo RPC es el mismo. La complejidad de hw init la absorbe GSP.

2. **PFIFO identico**: El command submission protocol no cambio desde Fermi.
   La misma infraestructura de channels, pushbuffers y GP entries funciona en
   todas. Solo cambian los class IDs al bindear engines.

3. **ISA forward-compatible**: Binarios compilados para Turing corren en Ampere,
   Ada y Blackwell. Las adiciones son backward-compatible. Esto significa que
   los kernels SASS de ntransformer compilados una vez corren en todas.

**Diferencias reales entre generaciones (que SI hay que manejar)**:
- Tensor core generations (no usamos — ntransformer usa GEMV puro)
- Memory bandwidth / cache sizes (tuning, no codigo diferente)
- Max warps/SM, register file size (parametros del kernel launch)
- GSP firmware version (necesitas el .bin correcto por generacion)

---

## 6. Path de implementacion propuesto

### Fase 0: UEFI bootloader (proof of concept)
- Aplicacion UEFI que arranca, muestra info del sistema por GOP framebuffer
- Enumera PCIe, encuentra GPU NVIDIA, lee BAR0 para identificar GPU
- Sale a un prompt UART/serial
- **Herramientas**: GNU-EFI o EDK2 (TianoCore)
- **Estimacion**: ~2,000 lineas

### Fase 1: NanoKernel base
- ExitBootServices() → flat mode, identity-mapped memory
- Physical memory manager (buddy allocator o bitmap)
- APIC init, HPET timer, basic IRQ handling
- UART driver (debug console)
- PCI enumeration completo
- **Estimacion**: ~5,000 lineas

### Fase 2: GPU init via GSP (blob approach)
- Mapear BARs de la GPU
- Cargar GSP firmware desde ramdisk (embedded en EFI binary o NVMe)
- Implementar shim layer para el protocolo nvidia-open
- Establecer comunicacion CPU-RM <-> GSP-RM
- Verificar: GPU inicializada, clocks correctos
- **Estimacion**: ~15,000-25,000 lineas (el grueso del trabajo)
- **Referencia**: Nova driver (Rust, minimal GSP-only, Linux 6.15+)

### Fase 3: Compute pipeline
- GPFIFO channel creation
- GPU page table setup
- Cargar kernels PTX pre-compilados
- Dispatch compute, wait completion
- DMA transfers H2D / D2H
- **Estimacion**: ~5,000 lineas
- **Referencia**: NVK compute path, envytools PFIFO docs

### Fase 4: NVMe driver
- Adaptar gpu-nvme-direct de ntransformer a bare-metal
- Ya es un driver userspace — eliminar la capa VFIO y hablar directo
- **Estimacion**: ~3,000 lineas

### Fase 5: Portar NTransformer
- Adaptar GGUF loader para leer de NVMe directo
- Portar los kernels CUDA (ya compilados a PTX, dispatch via Fase 3)
- Layer streamer con DMA directo
- Tokenizer + sampler
- Interfaz serial o Ethernet para prompts
- **Estimacion**: ~5,000 lineas de adaptacion

---

## 7. Referencia: Hardware de NVIDIA GPUs

### PCI BAR Layout
```
BAR0 (16MB) — MMIO Control Registers
  0x000000  PMC     Card master control, GPU ID, interrupt control
  0x001000  PBUS    Bus control
  0x002000  PFIFO   Command submission engine
  0x009000  PTIMER  Time measurement
  0x00a000  PDAEMON Card management (GT215+)
  0x100000  PFB     Memory interface, VM control
  0x400000  PGRAPH  2D/3D graphics + compute engine
  0x610000  PDISPLAY Display engine (G80+)
  0x700000  PMEM    Indirect VRAM/host memory access

BAR1 — VRAM Aperture (CPU-visible GPU memory, size varies)
BAR2 — RAMIN Aperture (memory control structures, G80+)
BAR5 — I/O ports for real-mode access (legacy)
```

### GPFIFO Command Submission (estable Fermi -> Blackwell)
```
User fills pushbuffer (system/VRAM memory)
  -> 32-bit words: [method_addr | subchannel | count | data...]
     |
GP entry (8 bytes): [pushbuf_address | size | flags]
  -> Escrito al GPFIFO ring buffer
     |
PBDMA engine (hardware): lee GP entries cuando GP_PUT != GP_GET
  -> Fetches pushbuf data, routes to target engine
     |
Target engine (PGRAPH for compute, CE for copy)
  -> Ejecuta methods
```

### Registers clave (PBDMA)
```
NV_PPBDMA_GP_BASE      Base address del GPFIFO ring
NV_PPBDMA_GP_PUT       Producer pointer (software escribe)
NV_PPBDMA_GP_GET       Consumer pointer (hardware lee)
NV_PPBDMA_TOP_LEVEL_GET Monitor progreso del pushbuffer
NV_RAMUSERD            Per-channel user-readable status
```

### GPU Page Tables (GMMU)
```
4 niveles (como x86-64 page tables)
Page sizes: 4KB, 64KB, 2MB, 512MB
49-bit virtual addresses (Pascal+)
Cada channel tiene su propio page directory
Traduce GPU VA -> GPU physical o Host physical
```

### GSP Communication Protocol
```
CPU-RM (host kernel driver)
  |
  | Lock-free shared memory message queues
  | Doorbell interrupts signal message availability
  v
GSP-RM (RISC-V core en el GPU die)
  |
  | Maneja: init, clocks, power, thermal, memory controller,
  |         fault handling, engine configuration
  v
GPU Hardware
```

---

## 8. Recursos y referencias

### Documentacion oficial NVIDIA
- open-gpu-doc: https://github.com/NVIDIA/open-gpu-doc
  (class headers, PBDMA refs, Pascal MMU format, DEVINIT scripts)
- open-gpu-kernel-modules: https://github.com/NVIDIA/open-gpu-kernel-modules
  (935K lineas, GPL/MIT, el driver real de produccion)
- GSP firmware docs: NVIDIA README chapters 42/43

### Reverse engineering community
- envytools: https://envytools.readthedocs.io/
  (MMIO registers, PFIFO, PGRAPH, memory management, multi-generacion)
- Nouveau: https://nouveau.freedesktop.org/
  (15+ anios de reverse engineering)
- NVK (Mesa Vulkan): https://docs.mesa3d.org/drivers/nvk.html
  (Vulkan 1.4, Turing -> Blackwell, compute shaders funcionales)

### Proyectos relacionados
- Nova driver: https://rust-for-linux.com/nova-gpu-driver
  (Rust, GSP-only, minimal, la referencia mas limpia para el approach GSP)
- Gdev: https://github.com/shinpei0208/gdev
  (GPU resource management sobre Nouveau, CUDA-compatible runtime)
- Cricket: https://github.com/RWTH-ACS/cricket
  (GPU desde unikernels via RPC — no bare-metal real)

### Build tools
- GNU-EFI o EDK2/TianoCore para UEFI applications
- NASM/GAS para x86-64 assembly (nanokernel bootstrap)
- GCC/Clang cross-compilation para freestanding x86-64
- NVCC para pre-compilar kernels a PTX/SASS (offline)
