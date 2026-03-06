/*
 * OsitoK x86-64 — GPU Tensor Operations (X39)
 *
 * Dispatch layer for GPU-accelerated tensor operations.
 * Manages VRAM buffers via bump allocator, data transfer via PRAMIN,
 * and kernel dispatch via QMD + GPFIFO pushbuffer.
 *
 * ── Kernel compilation workflow ──────────────────────────────
 *
 * For complex tensor kernels (matvec_q4_0, rmsnorm, etc.), compile
 * PTX on a CUDA-capable machine and embed the SASS bytes:
 *
 *   1. Write PTX:  kernels/vec_add.ptx
 *   2. Compile:    ptxas --gpu-name sm_75 -o vec_add.cubin vec_add.ptx
 *   3. Disasm:     nvdisasm -b SM75 -hex vec_add.cubin > vec_add.sass
 *   4. Extract:    python3 scripts/cubin2array.py vec_add.cubin > vec_add_code.h
 *   5. Embed:      #include the byte array in sass.c, register in sass_init()
 *
 * ── PTX source for future kernels ───────────────────────────
 *
 * vec_add_f32.ptx:
 *   .version 7.5
 *   .target sm_75
 *   .entry vec_add(.param .u64 dst, .param .u64 a, .param .u64 b, .param .u32 n) {
 *     .reg .u32 %tid, %n;  .reg .u64 %dst, %a, %b, %off;
 *     .reg .f32 %va, %vb, %vc;  .reg .pred %p;
 *     mov.u32 %tid, %tid.x;
 *     ld.param.u32 %n, [n];
 *     setp.ge.u32 %p, %tid, %n;  @%p bra done;
 *     mul.wide.u32 %off, %tid, 4;
 *     ld.param.u64 %a, [a];  add.u64 %a, %a, %off;  ld.global.f32 %va, [%a];
 *     ld.param.u64 %b, [b];  add.u64 %b, %b, %off;  ld.global.f32 %vb, [%b];
 *     add.f32 %vc, %va, %vb;
 *     ld.param.u64 %dst, [dst];  add.u64 %dst, %dst, %off;  st.global.f32 [%dst], %vc;
 *   done: ret;
 *   }
 *
 * matvec_q4_0.ptx:
 *   .entry matvec_q4_0(.param .u64 out, .param .u64 w, .param .u64 x,
 *                       .param .u32 rows, .param .u32 cols) {
 *     // Each thread computes one output row
 *     // Q4_0 block: 2-byte scale (f16) + 16 bytes (32 nibbles)
 *     // Inner loop: for each Q4_0 block in the row,
 *     //   dequant 32 values, dot product with x[offset..offset+31]
 *     // Accumulate in f32, store to out[row]
 *   }
 *
 * rmsnorm.ptx:
 *   .entry rmsnorm(.param .u64 out, .param .u64 x, .param .u64 w, .param .u32 n) {
 *     // Phase 1: parallel sum-of-squares (shared memory reduction)
 *     // Phase 2: rsqrt(mean_sq + eps)
 *     // Phase 3: out[i] = x[i] * scale * w[i]
 *   }
 *
 * softmax.ptx:
 *   .entry softmax(.param .u64 x, .param .u32 n) {
 *     // Phase 1: parallel max reduction
 *     // Phase 2: exp(x[i] - max), parallel sum
 *     // Phase 3: x[i] = exp_val / sum
 *   }
 *
 * rope.ptx:
 *   .entry rope(.param .u64 vec, .param .u32 n_heads, .param .u32 head_dim,
 *               .param .u32 pos, .param .f32 theta) {
 *     // Element-wise: for each (head, dim_pair):
 *     //   freq = 1.0 / pow(theta, 2*i/head_dim)
 *     //   cos_val = cos(pos * freq), sin_val = sin(pos * freq)
 *     //   (x, y) = (x*cos - y*sin, x*sin + y*cos)
 *   }
 *
 * silu.ptx:
 *   .entry silu(.param .u64 x, .param .u32 n) {
 *     // x[i] = x[i] * (1.0 / (1.0 + exp(-x[i])))
 *   }
 */

#include "../include/types.h"
#include "gpu.h"
#include "sass.h"
#include "gpu_tensor.h"

/* ── External Functions ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* ── State ───────────────────────────────────────────────── */

static gpu_tensor_state_t tensor_state;

gpu_tensor_state_t *gpu_tensor_get_state(void)
{
    return &tensor_state;
}

/* ── VRAM Buffer Management ──────────────────────────────── */

uint64_t gpu_tensor_alloc(uint32_t size)
{
    if (!tensor_state.initialized || size == 0) return 0;

    /* 256-byte align */
    uint32_t aligned = (size + 255) & ~255u;
    if (tensor_state.vram_next + aligned > tensor_state.vram_end)
        return 0;

    uint64_t addr = tensor_state.vram_next;
    tensor_state.vram_next += aligned;
    return addr;
}

void gpu_tensor_reset(void)
{
    tensor_state.vram_next = tensor_state.vram_base;
}

/* ── Data Transfer via PRAMIN ────────────────────────────── */

int gpu_tensor_upload(uint64_t vram_addr, const void *data, uint32_t size)
{
    if (!data || size == 0) return -1;

    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) return -1;

    uint32_t saved_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);
    const uint32_t *src = (const uint32_t *)data;
    uint32_t dwords = (size + 3) / 4;
    uint32_t written = 0;

    while (written < dwords) {
        uint64_t cur_addr = vram_addr + written * 4;
        uint32_t window_val = (uint32_t)(cur_addr >> 16);
        gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
        wmb();

        /* Write within current 64KB window */
        uint32_t off = (uint32_t)(cur_addr & 0xFFFF);
        while (written < dwords && off < NV_PRAMIN_SIZE) {
            gpu_reg_write(NV_PRAMIN_BASE + off, src[written]);
            written++;
            off += 4;
        }
    }
    wmb();

    gpu_reg_write(NV_PBUS_BAR0_WINDOW, saved_window);
    wmb();
    return 0;
}

int gpu_tensor_download(uint64_t vram_addr, void *data, uint32_t size)
{
    if (!data || size == 0) return -1;

    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) return -1;

    uint32_t saved_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);
    uint32_t *dst = (uint32_t *)data;
    uint32_t dwords = (size + 3) / 4;
    uint32_t read_count = 0;

    while (read_count < dwords) {
        uint64_t cur_addr = vram_addr + read_count * 4;
        uint32_t window_val = (uint32_t)(cur_addr >> 16);
        gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
        wmb();
        rmb();

        uint32_t off = (uint32_t)(cur_addr & 0xFFFF);
        while (read_count < dwords && off < NV_PRAMIN_SIZE) {
            dst[read_count] = gpu_reg_read(NV_PRAMIN_BASE + off);
            read_count++;
            off += 4;
        }
    }

    gpu_reg_write(NV_PBUS_BAR0_WINDOW, saved_window);
    wmb();
    return 0;
}

/* ── Tensor Operations ───────────────────────────────────── */

int gpu_vec_add(uint64_t dst_vram, uint64_t a_vram, uint64_t b_vram, uint32_t n)
{
    if (n == 0 || n > 256) {
        serial_puts("[GPU_TENSOR] vec_add: n must be 1-256\n");
        return -1;
    }

    /* Patch kernel with buffer addresses */
    int ret = sass_patch_vec_add((uint32_t)a_vram, (uint32_t)b_vram, (uint32_t)dst_vram);
    if (ret < 0) {
        serial_puts("[GPU_TENSOR] vec_add: kernel patch/upload failed\n");
        return -1;
    }

    sass_kernel_t *k = sass_get_kernel("vec_add_f32");
    if (!k || !k->uploaded) return -1;

    /* Allocate semaphore */
    uint32_t *sem = (uint32_t *)mem_alloc_aligned(4096, 4096);
    if (!sem) return -1;
    *sem = 0;
    wmb();
    uint64_t sem_phys = (uint64_t)(uintptr_t)sem;

    /* Dispatch */
    compute_dispatch_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.program_addr   = k->vram_addr;
    desc.grid_x         = 1;
    desc.grid_y         = 1;
    desc.grid_z         = 1;
    desc.block_x        = n;
    desc.block_y        = 1;
    desc.block_z        = 1;
    desc.register_count = k->register_count;
    desc.sem_addr       = sem_phys;
    desc.sem_payload    = 0xADD0ADD0;

    serial_puts("[GPU_TENSOR] vec_add: n=");
    serial_putdec(n);
    serial_puts(" a=0x");
    serial_puthex(a_vram, 8);
    serial_puts(" b=0x");
    serial_puthex(b_vram, 8);
    serial_puts(" dst=0x");
    serial_puthex(dst_vram, 8);
    serial_puts("\n");

    ret = gsp_compute_dispatch(&desc);
    if (ret < 0) return -1;

    return gsp_compute_wait(sem_phys, 0xADD0ADD0, 1000);
}

/* ── X42: Compiled PTX Kernel Dispatch (CB0 parameter passing) ── */

/*
 * CB0 (Constant Buffer 0) layout for compiled PTX kernels:
 *   c[0x0][0x00] = blockDim.x  (uint32_t)
 *   c[0x0][0x04] = blockDim.y  (uint32_t)
 *   c[0x0][0x08] = blockDim.z  (uint32_t)
 *   ...
 *   c[0x0][0x160..] = user .param parameters (kernel-specific)
 *
 * ptxas SM75 places .param arguments at EIATTR_PARAM_CBANK offset 0x160.
 * The compiled SASS reads params via LDC instructions from c[0x0][0x160+off].
 */

/* Build CB0 in a local buffer, upload to VRAM, dispatch kernel */
int gpu_dispatch_kernel(const char *name, uint32_t grid_x, uint32_t grid_y,
                        uint32_t block_x, uint32_t block_y,
                        const void *params, uint32_t params_size,
                        uint32_t shared_mem)
{
    sass_kernel_t *k = sass_get_kernel(name);
    if (!k || !k->uploaded) {
        serial_puts("[GPU_TENSOR] dispatch: kernel '");
        serial_puts(name);
        serial_puts("' not found/uploaded\n");
        return -1;
    }

    if (params_size > CB0_TOTAL_SIZE - CB0_PARAM_OFFSET) {
        serial_puts("[GPU_TENSOR] dispatch: params too large\n");
        return -1;
    }

    /* Allocate CB0 in VRAM (within GMMU identity-mapped region) */
    uint64_t cb0_vram = gpu_tensor_alloc(CB0_TOTAL_SIZE);
    if (!cb0_vram) {
        serial_puts("[GPU_TENSOR] dispatch: CB0 VRAM alloc failed\n");
        return -1;
    }

    /* Build CB0 buffer in host RAM */
    uint8_t cb0[CB0_TOTAL_SIZE];
    memset(cb0, 0, CB0_TOTAL_SIZE);

    /* blockDim at offset 0x00 */
    uint32_t *cb0_u32 = (uint32_t *)cb0;
    cb0_u32[0] = block_x;    /* c[0x0][0x00] = blockDim.x */
    cb0_u32[1] = block_y;    /* c[0x0][0x04] = blockDim.y */
    cb0_u32[2] = 1;          /* c[0x0][0x08] = blockDim.z */

    /* User params at offset 0x160 */
    if (params && params_size > 0)
        memcpy(cb0 + CB0_PARAM_OFFSET, params, params_size);

    /* Upload CB0 to VRAM */
    if (gpu_tensor_upload(cb0_vram, cb0, CB0_TOTAL_SIZE) < 0) {
        serial_puts("[GPU_TENSOR] dispatch: CB0 upload failed\n");
        return -1;
    }

    /* Allocate semaphore for completion */
    uint32_t *sem = (uint32_t *)mem_alloc_aligned(4096, 4096);
    if (!sem) return -1;
    *sem = 0;
    wmb();
    uint64_t sem_phys = (uint64_t)(uintptr_t)sem;

    /* Build dispatch descriptor */
    compute_dispatch_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.program_addr   = k->vram_addr;
    desc.grid_x         = grid_x;
    desc.grid_y         = grid_y;
    desc.grid_z         = 1;
    desc.block_x        = block_x;
    desc.block_y        = block_y;
    desc.block_z        = 1;
    desc.register_count = k->register_count;
    desc.shared_mem_size = shared_mem;
    desc.barrier_count  = k->barrier_count;
    desc.sem_addr       = sem_phys;
    desc.sem_payload    = 0xD15A7C40 + (uint32_t)(uintptr_t)name;
    desc.cbuf_addr      = cb0_vram;  /* GPU VA = VRAM phys (identity mapped) */
    desc.cbuf_size      = CB0_TOTAL_SIZE;

    serial_puts("[GPU_TENSOR] dispatch '");
    serial_puts(name);
    serial_puts("' grid=");
    serial_putdec(grid_x); serial_puts("x"); serial_putdec(grid_y);
    serial_puts(" block=");
    serial_putdec(block_x); serial_puts("x"); serial_putdec(block_y);
    serial_puts(" CB0=0x");
    serial_puthex(cb0_vram, 8);
    serial_puts("\n");

    int ret = gsp_compute_dispatch(&desc);
    if (ret < 0) return -1;

    return gsp_compute_wait(sem_phys, desc.sem_payload, 1000);
}

/* ── Per-kernel dispatch wrappers ─────────────────────────── */

int gpu_vec_add_ptx(uint64_t out_vram, uint64_t a_vram, uint64_t b_vram, uint32_t n)
{
    if (n == 0) return -1;
    /* CB0 params layout (matches elementwise.ptx .param order):
     *   0x160: .u64 out
     *   0x168: .u64 a
     *   0x170: .u64 b
     *   0x178: .u32 n
     */
    struct { uint64_t out; uint64_t a; uint64_t b; uint32_t n; } params = {
        out_vram, a_vram, b_vram, n
    };
    uint32_t threads = 256;
    uint32_t blocks = (n + threads - 1) / threads;
    return gpu_dispatch_kernel("vec_add", blocks, 1, threads, 1,
                               &params, sizeof(params), 0);
}

int gpu_vec_mul_ptx(uint64_t out_vram, uint64_t a_vram, uint64_t b_vram, uint32_t n)
{
    if (n == 0) return -1;
    struct { uint64_t out; uint64_t a; uint64_t b; uint32_t n; } params = {
        out_vram, a_vram, b_vram, n
    };
    uint32_t threads = 256;
    uint32_t blocks = (n + threads - 1) / threads;
    return gpu_dispatch_kernel("vec_mul", blocks, 1, threads, 1,
                               &params, sizeof(params), 0);
}

int gpu_add_inplace_ptx(uint64_t a_vram, uint64_t b_vram, uint32_t n)
{
    if (n == 0) return -1;
    /* add_inplace params: .u64 a, .u64 b, .u32 n */
    struct { uint64_t a; uint64_t b; uint32_t n; } params = {
        a_vram, b_vram, n
    };
    uint32_t threads = 256;
    uint32_t blocks = (n + threads - 1) / threads;
    return gpu_dispatch_kernel("add_inplace", blocks, 1, threads, 1,
                               &params, sizeof(params), 0);
}

int gpu_silu_mul_ptx(uint64_t out_vram, uint64_t gate_vram, uint64_t up_vram, uint32_t n)
{
    if (n == 0) return -1;
    /* silu_mul params: .u64 out, .u64 gate, .u64 up, .u32 n */
    struct { uint64_t out; uint64_t gate; uint64_t up; uint32_t n; } params = {
        out_vram, gate_vram, up_vram, n
    };
    uint32_t threads = 256;
    uint32_t blocks = (n + threads - 1) / threads;
    return gpu_dispatch_kernel("silu_mul", blocks, 1, threads, 1,
                               &params, sizeof(params), 0);
}

int gpu_rmsnorm_ptx(uint64_t out_vram, uint64_t x_vram, uint64_t w_vram,
                    uint32_t hidden_size, float eps)
{
    if (hidden_size == 0) return -1;
    /* rmsnorm params: .u64 out, .u64 x, .u64 w, .u32 n, .f32 eps */
    struct { uint64_t out; uint64_t x; uint64_t w; uint32_t n; float eps; } params = {
        out_vram, x_vram, w_vram, hidden_size, eps
    };
    /* 1 block, threads = min(hidden_size, 256) */
    uint32_t threads = hidden_size < 256 ? hidden_size : 256;
    return gpu_dispatch_kernel("rmsnorm", 1, 1, threads, 1,
                               &params, sizeof(params),
                               threads * 4);  /* shared mem for reduction */
}

int gpu_softmax_ptx(uint64_t out_vram, uint64_t in_vram, uint32_t cols, uint32_t rows)
{
    if (cols == 0 || rows == 0) return -1;
    /* softmax params: .u64 out, .u64 in, .u32 cols, .u32 rows */
    struct { uint64_t out; uint64_t in; uint32_t cols; uint32_t rows; } params = {
        out_vram, in_vram, cols, rows
    };
    /* 1 block per row, threads = min(cols, 256) */
    uint32_t threads = cols < 256 ? cols : 256;
    return gpu_dispatch_kernel("softmax", rows, 1, threads, 1,
                               &params, sizeof(params),
                               threads * 4);  /* shared mem for reductions */
}

int gpu_rope_ptx(uint64_t q_vram, uint64_t k_vram, uint32_t pos,
                 uint32_t n_heads, uint32_t n_kv_heads, uint32_t head_dim,
                 float theta_base)
{
    if (head_dim == 0) return -1;
    /* rope params: .u64 q, .u64 k, .u32 pos, .u32 n_heads, .u32 n_kv_heads,
     *              .u32 head_dim, .f32 theta_base */
    struct {
        uint64_t q; uint64_t k;
        uint32_t pos; uint32_t n_heads; uint32_t n_kv_heads;
        uint32_t head_dim; float theta_base;
    } params = {
        q_vram, k_vram, pos, n_heads, n_kv_heads, head_dim, theta_base
    };
    /* Each thread handles one dim pair. Total pairs = n_heads * head_dim/2 */
    uint32_t total_pairs = n_heads * (head_dim / 2);
    uint32_t threads = 256;
    uint32_t blocks = (total_pairs + threads - 1) / threads;
    return gpu_dispatch_kernel("rope", blocks, 1, threads, 1,
                               &params, sizeof(params), 0);
}

int gpu_gemv_q4_0_ptx(uint64_t y_vram, uint64_t w_vram, uint64_t x_vram,
                      uint32_t out_features, uint32_t in_features)
{
    if (out_features == 0 || in_features == 0) return -1;
    /* gemv_q4_0 params: .u64 y, .u64 w, .u64 x, .u32 out_features, .u32 in_features */
    struct {
        uint64_t y; uint64_t w; uint64_t x;
        uint32_t out_features; uint32_t in_features;
    } params = {
        y_vram, w_vram, x_vram, out_features, in_features
    };
    /* 1 block per output row, 256 threads per block for warp reduction */
    return gpu_dispatch_kernel("gemv_q4_0", out_features, 1, 256, 1,
                               &params, sizeof(params),
                               256 * 4);  /* shared mem for reduction */
}

/* ── Self-Test ───────────────────────────────────────────── */

int gpu_tensor_test(void)
{
    serial_puts("\n[GPU_TENSOR] == X39: GPU Tensor Ops Self-Test ==\n");

    /* Test 1: store_pattern (proves GMMU + STG) */
    serial_puts("[GPU_TENSOR] Test 1: store_pattern\n");
    sass_store_test();

    /* Test 2: vec_add (proves LDG + FADD + STG pipeline) */
    serial_puts("[GPU_TENSOR] Test 2: vec_add_f32\n");
    if (!tensor_state.initialized) {
        serial_puts("[GPU_TENSOR] Tensor subsystem not initialized, skipping vec_add\n");
    } else {
        /* Allocate VRAM buffers for 4 floats */
        uint32_t n = 4;
        uint32_t buf_size = n * 4;
        uint64_t a_vram = gpu_tensor_alloc(buf_size);
        uint64_t b_vram = gpu_tensor_alloc(buf_size);
        uint64_t c_vram = gpu_tensor_alloc(buf_size);

        if (a_vram && b_vram && c_vram) {
            /* Upload test data: a = {1.0, 2.0, 3.0, 4.0}, b = {10.0, 20.0, 30.0, 40.0} */
            float a_data[] = {1.0f, 2.0f, 3.0f, 4.0f};
            float b_data[] = {10.0f, 20.0f, 30.0f, 40.0f};
            gpu_tensor_upload(a_vram, a_data, buf_size);
            gpu_tensor_upload(b_vram, b_data, buf_size);

            /* Clear output */
            float c_data[] = {0.0f, 0.0f, 0.0f, 0.0f};
            gpu_tensor_upload(c_vram, c_data, buf_size);

            serial_puts("[GPU_TENSOR] Buffers: a=0x");
            serial_puthex(a_vram, 8);
            serial_puts(" b=0x");
            serial_puthex(b_vram, 8);
            serial_puts(" c=0x");
            serial_puthex(c_vram, 8);
            serial_puts("\n");

            /* Dispatch vec_add */
            int ret = gpu_vec_add(c_vram, a_vram, b_vram, n);
            if (ret == 0) {
                /* Download and check results */
                gpu_tensor_download(c_vram, c_data, buf_size);
                serial_puts("[GPU_TENSOR] Results: ");
                uint32_t raw[4];
                memcpy(raw, c_data, buf_size);
                for (uint32_t i = 0; i < n; i++) {
                    serial_puthex(raw[i], 8);
                    if (i < n - 1) serial_puts(", ");
                }
                serial_puts("\n");
                /* Expected: {11.0, 22.0, 33.0, 44.0} */
            } else {
                serial_puts("[GPU_TENSOR] vec_add pending — needs GSP boot chain\n");
            }
        }

        gpu_tensor_reset();
    }

    serial_puts("[GPU_TENSOR] == X39 self-test complete ==\n\n");
    return 0;
}

/* ── Initialization ──────────────────────────────────────── */

int gpu_tensor_init(void)
{
    serial_puts("\n[GPU_TENSOR] == X39: GPU Tensor Ops ==\n");

    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) {
        serial_puts("[GPU_TENSOR] No GPU detected, skipping\n");
        return -1;
    }

    memset(&tensor_state, 0, sizeof(tensor_state));

    /* Setup VRAM buffer region */
    tensor_state.vram_base = (uint64_t)GPU_TENSOR_VRAM_OFFSET_MB * 1024 * 1024;
    tensor_state.vram_next = tensor_state.vram_base;
    tensor_state.vram_end  = tensor_state.vram_base +
                             (uint64_t)GPU_TENSOR_VRAM_SIZE_MB * 1024 * 1024;
    tensor_state.initialized = true;

    serial_puts("[GPU_TENSOR] VRAM buffer region: 0x");
    serial_puthex(tensor_state.vram_base, 8);
    serial_puts(" - 0x");
    serial_puthex(tensor_state.vram_end, 8);
    serial_puts(" (");
    serial_putdec(GPU_TENSOR_VRAM_SIZE_MB);
    serial_puts("MB)\n");

    /* Kernel status */
    sass_state_t *ss = sass_get_state();
    if (ss) {
        serial_puts("[GPU_TENSOR] SASS kernels available: ");
        serial_putdec(ss->count);
        serial_puts(" (");
        for (uint32_t i = 0; i < ss->count; i++) {
            if (i > 0) serial_puts(", ");
            serial_puts(ss->kernels[i].name);
        }
        serial_puts(")\n");
    }

    fb_puts(" GPU Tensor: ");
    fb_putdec(GPU_TENSOR_VRAM_SIZE_MB);
    fb_puts("MB buf\n");

    /* Run self-tests */
    gpu_tensor_test();

    serial_puts("[GPU_TENSOR] == X39 complete ==\n\n");
    return 0;
}
