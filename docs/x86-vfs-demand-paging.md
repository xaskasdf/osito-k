# x86-64 VFS, OsitoFS v3, Demand Paging, ETXTBSY

> Implemented in 2026-04-12 stabilization sweep. See commits `be59c00..207900d`
> for the full history. This doc is the architecture-level reference.

## Why these features

OsitoK was hitting three classes of failure when trying to run real-world
binaries:

1. **Filesystem ceiling**: OsitoFS v2 was a flat namespace with contiguous
   files. No directories, no extents — incompatible with anything that
   expects POSIX paths or sparse files.
2. **Memory ceiling**: The ELF loader did `kmalloc(file_size)` to slurp the
   whole binary into kernel heap before mapping segments. Anything above
   ~10 MB OOM'd or fragmented the heap. Goal: load Node.js / Bun (50–100 MB).
3. **Self-corruption**: Programs (notably zsh) would open paths that resolved
   to their own executable in `O_RDWR` and then write through that fd,
   silently overwriting their on-disk `.text` segment. Linux returns
   `ETXTBSY` for this; OsitoK happily executed it.

The fixes form a coherent stack: VFS gives us a uniform path layer that v2
and v3 share, demand paging eliminates the eager `kmalloc` for static binaries,
and ETXTBSY closes the self-corruption hole. They were all done in one sweep
because they touch the same files (`elf.c`, `syscall.c`, `vfs.c`).

## Architecture overview

```
                 sys_open / sys_read / sys_mmap / sys_write
                                  │
                                  ▼
                   ┌────────────────────────────────┐
                   │      vfs.c (path layer)        │
                   │  vfs_find / vfs_read / etc     │
                   └────────────┬───────────────────┘
                                │
                  ┌─────────────┴──────────────┐
                  ▼                            ▼
          ┌───────────────┐           ┌───────────────┐
          │  ositofs2.c   │           │  ositofs3.c   │
          │  flat namespc │           │  inode+extent │
          └───────┬───────┘           └───────┬───────┘
                  │                           │
                  └─────────────┬─────────────┘
                                ▼
                          nvme_read_bytes
```

ELF loading and page faults intersect with this stack:

```
proc_exec("zsh.elf")
  ├─ vfs_find("zsh.elf") → vfs_node_t (fs_version + data ptr)
  ├─ elf_exec
  │    ├─ vfs_read(node, 0, hdr_buf, 64)        ← 64 bytes
  │    ├─ vfs_read(node, e_phoff, phdr_buf, …)  ← ~500 bytes
  │    ├─ if has_dynamic → eager path (kmalloc + full file)
  │    └─ if static ET_EXEC → demand path:
  │         elf_setup_demand_segments()
  │           └─ vma_register_file(vaddr, pages, prot,
  │                                VMA_FILE_ELF, &node, file_off, …)
  └─ elf_jump → user-mode entry

   ↓ first instruction at vaddr triggers #PF

#PF → idt.c → demand_page_fault(cr2, err)
        ├─ scan vma_table[] for cr2
        ├─ allocate one phys page
        ├─ if VMA_FILE_*: vfs_read(node, file_offset+offset, page, 4096)
        └─ paging_map_page(cr2, phys, prot_to_pte_flags(vma->prot))
```

## Components

### VFS layer (`fs/vfs.c`, `fs/vfs.h`)

Single source of truth for path resolution and file ops. Used by syscall.c,
elf.c, shell.c, main.c. Replaces 6+ duplicate basename loops that used to
live in win32/ and kernel/.

Modes:
- `VFS_MODE_NATIVE` — flat OsitoFS names (`zsh.elf`)
- `VFS_MODE_POSIX` — `/usr/lib/libc.so` → strip leading `/` and lookup
- `VFS_MODE_WIN32` — `C:\System\Core.u` → basename + case-insensitive

`vfs_node_t` is a tagged union: `fs_version` selects between osfs2 and osfs3
backends. The struct is small (24 bytes) and copied by value into VMAs and
fd_table entries — no pointer aliasing, no heap allocation.

API:
```c
const char *vfs_resolve(const char *path, int mode);
bool        vfs_find(const char *path, int mode, vfs_node_t *out_node);
int         vfs_read(vfs_node_t *node, uint64_t offset, void *buf, uint64_t len);
void        vfs_list(const char *path);
```

### OsitoFS v3 (`fs/ositofs3.c`, `include/common/ositofs3_format.h`)

Inode + extent based, hierarchical directories, 1 MB blocks (NVMe DMA friendly).
On-disk layout:

```
Block 0          Superblock
Block 1          Inode bitmap
Block 2          Block bitmap
Block 3..K       Inode table  (256 B / inode, 4096 inodes / block)
Block K+1..N     Data blocks
```

Inode is 256 B with up to 12 direct extents (`{start_block, block_count}`).
Symlinks ≤ 64 bytes are inlined. Driver reads in 64 KB chunks (NVMe DMA
window for QEMU).

Mount fallback chain: `osfs3_mount()` → if bad magic → `osfs2_mount()`. The
existing nvme.img images stay readable.

### Demand-paged ELF loader (`kernel/elf.c`)

Two paths controlled by `use_demand = (e_type == ET_EXEC) && !has_dynamic`:

**Demand path** (static ET_EXEC):
1. Read 64 B header + N × 56 B program headers (~600 B total)
2. For each `PT_LOAD`, register a `VMA_FILE_ELF` with `file_offset`,
   `file_size`, `prot`, no physical pages
3. Allocate stack (eager), set up auxv, jump
4. The first instruction at `e_entry` page-faults → `demand_page_fault`
   reads exactly that 4 KB from disk and maps it

**Eager path** (`PT_DYNAMIC` or `ET_DYN`):
The original loader is preserved untouched. Dynamic linking, NT_GNU_ABI_TAG
patching, fork_saves, and TLS init all need the in-memory file buffer.

Tracking: demand-paged segments are *not* registered via `proc_add_region`.
They're tracked by the global `vma_table[]` and freed on exit by
`syscall_reset_process` walking PTEs through `vma_free_pages`.

### `vma_t` extension (`kernel/syscall.c`)

```c
typedef struct {
    uint64_t    base;
    uint64_t    pages;
    uint32_t    prot;
    bool        in_use;
    uint8_t     type;        // VMA_ANON / VMA_FILE_ELF / VMA_FILE_MMAP
    vfs_node_t  file_node;   // copy by value (24 B)
    uint64_t    file_offset;
    uint64_t    file_size;   // bytes backed by file (rest = zero-fill BSS)
} vma_t;
```

Backward compatible: existing code that creates VMAs leaves the new fields
zero, which means `type == VMA_ANON` and behaves like before.

### `demand_page_fault` (`kernel/syscall.c`)

```c
int demand_page_fault(uint64_t addr, uint64_t error_code) {
    if (error_code & 1) return -1;       // not a not-present fault
    vma_t *vma = find_vma_containing(addr);
    if (!vma) return -1;                 // → SIGSEGV

    void *page = mem_alloc_pages(1);
    memset(page, 0, 4096);

    if (vma->type == VMA_FILE_ELF || vma->type == VMA_FILE_MMAP) {
        uint64_t off = (addr & ~0xFFF) - vma->base;
        if (off < vma->file_size) {
            uint64_t to_read = min(4096, vma->file_size - off);
            vfs_read(&vma->file_node, vma->file_offset + off, page, to_read);
        }
    }
    paging_map_page(addr & ~0xFFF, (uint64_t)page, prot_to_pte_flags(vma->prot));
    return 0;
}
```

The dispatch is hooked from `idt.c` at the very top of the page fault handler
(before any diagnostic output) so kernel-mode `#PF` against demand-paged user
addresses doesn't recurse into the fault printer and #DF.

### High-memory allocator (`kernel/memory.c`, `kernel/paging.c`)

Root cause that took the longest to find: the kernel PML4 lived at
phys 0x01000000 (16 MB) because `mem_alloc_aligned` searched bottom-up
starting at page 4096. Zig binaries default to vaddr 0x01000000. When the
ELF loader did `paging_map_page(0x01000000, new_phys, …)` it remapped the
virtual address that the kernel itself used to access its PML4 (identity
mapping → `kernel_pml4 == 0x01000000`). Boom — all subsequent page-table
ops read from the new (zeroed) page and the kernel was suddenly without
paging structures.

Fix: `mem_alloc_aligned_high` searches top-down. `pt_alloc_page` calls it
preferentially with a fallback to the low allocator. Result: kernel paging
structures live above 256 MB, far from any ELF load range.

### ETXTBSY (`kernel/syscall.c`, `kernel/process.c`)

`proc_is_executing(name)` walks the proc table for live processes whose
basename matches. `sys_open` returns `-26` for any non-readonly open of an
executing binary. Matches Linux semantics.

This was the fix for zsh corrupting `zsh.elf` on disk. zsh opens its own
binary with `O_RDWR | O_NOCTTY` (probably tty resolution via
`readlink(/proc/self/exe)`) and then writes terminal output through that fd.
Without ETXTBSY the kernel happily wrote shell output (`osito#`,
`\x1b[?2004h`) to disk at offset 0 of zsh.elf.

Also: `syscall_reset_process` now force-resets `fd_table[0/1/2]` to console
on every exit, so a child that did `dup2(file_fd, 1)` doesn't leave the
parent shell with a stale file FD on stdout.

### Pipe blocking semantics (`kernel/syscall.c`)

Pipes used to return `-EAGAIN` whenever the buffer was empty (read) or full
(write), regardless of `O_NONBLOCK`. zsh's subshell coordination uses pipes,
so reads got immediate `EAGAIN`, the subshell init failed, and zsh reported
the no-op error `zsh: write failed: success` (because no errno was set on
the bogus failure path).

Fixed by checking `f->oflags & O_NONBLOCK`:
- not set → `sti; hlt; cli` loop until peer drains/fills/closes
- set    → return EAGAIN (old behavior)

The preemptive scheduler runs the peer on the next timer tick.

## Verification

| Binary             | Size   | Type            | Result |
|--------------------|--------|-----------------|--------|
| `hello_ositok.elf` | 9.6 KB | Zig static      | Prints `¡Hola desde Zig Nativo en OsitoK!`, exits 0 |
| `zsh.elf`          | 1.4 MB | musl static     | Boots, prompt, fork+pipe subshell, USB keyboard, exit clean |
| `GTA5.elf`         | 6.5 MB | RAGE static     | Demand-pages instantly (4476 KB R+X + 1188 KB R+W), runs to expected SIGSEGV in RAGE init (no assets), kernel cleans up |

For all three: MD5 of the binary in `nvme.img` is **identical** before and
after run (ETXTBSY confirmed).

## Known issues

### Auxv setup loop crash
`elf_setup_stack` builds an auxv array on the kernel stack and writes it
into the user stack with a backward-iterating loop. GCC sometimes emits a
loop bound that decrements past 0, faulting on a write to address `-8`.
The bug is sensitive to stack-frame layout, so adding/removing fields from
`elf_loaded_t` can mask or expose it. Affects zsh.elf in the current build
when launched from auto-exec; GTA5.elf was unaffected at the time of test.

Workaround candidates (not yet applied):
- Move auxv build to a heap buffer instead of stack
- Build auxv in forward order then reverse
- Compile elf.c with `-mincoming-stack-boundary=3` or equivalent

### fork+execve of the same binary on the demand path ✅ RESOLVED (2026-04-12 pm)
Originally a known hazard: the eager loader used `fork_saves[]` to memcpy
parent PF_W segments, but the demand loader had no equivalent. Fixed by
X-PGTBL (per-process CR3): `demand_page_fault` now installs PTEs via
`paging_map_page_in_cr3(proc_current_cr3(), …)`, so a child's fault-in
writes go into its own CR3 without touching the parent's. The
`!syscall_in_fork_exec()` gate in `use_demand` has been removed
(commit `18b21ce`). Parent's VMAs are protected by the `vma_t.owner`
filter during `syscall_reset_process` cleanup.

### Per-process address space ✅ RESOLVED (2026-04-12 pm, X-PGTBL commit `199ba97`)
Each process now has its own CR3: `paging_create_process_cr3()` clones
`kernel_pml4` and builds private PDs for PDPT[0] and PDPT[1] (covering
0..2GB where user binaries and mmap ranges live). Scheduler switches
CR3 on context change. `vma_t.owner` filters each process's mmap
regions so siblings don't see each other's VMAs. Concurrent zsh + any
static-PIE binary can run without interference.

### Per-process fd_table ✅ RESOLVED (2026-04-12 pm, commit `5876fe0`)
`fd_table[MAX_FDS]` moved from static global into
`process_t.fds[MAX_FDS]`. `pipe_buf_t` gained `int read_refs/write_refs`
replacing the old booleans; fork clones the parent's table and bumps
refs on every inherited pipe fd. Zsh fork+exec of external commands
now works — previously the parent closing one end of a subshell pipe
also killed the child's view because both processes shared the same
global table entry.

### User binaries now default to static-PIE (ET_DYN)
`zsh.elf` is linked as `ET_DYN` via `-Wl,-pie` with `rcrt1.o` self-
relocation from musl-1.2.6 rebuilt with `-fPIC`. The linker script no
longer hardcodes a load address; the kernel loader picks it via the
eager path (`elf_load_segments()` for ET_DYN). Hello world test
binaries follow the same pattern. See
`/Users/pc/ok-ported/zsh-5.9/Makefile.ositok` for the exact flags.

## File index

| Path | Role |
|------|------|
| `arch/x86/kernel/elf.c` | ELF loader (eager + demand paths) |
| `arch/x86/kernel/syscall.c` | VMA table, sys_mmap, demand_page_fault, sys_open ETXTBSY, pipe blocking |
| `arch/x86/kernel/process.c` | proc_is_executing, fork lifecycle |
| `arch/x86/kernel/paging.c` | paging_get_pte (exported), pt_alloc_page (high-mem) |
| `arch/x86/kernel/memory.c` | mem_alloc_aligned_high |
| `arch/x86/kernel/idt.c` | #PF handler dispatch to demand_page_fault |
| `arch/x86/fs/vfs.c` | Path resolution + dual-dispatch v2/v3 |
| `arch/x86/fs/ositofs3.c` | v3 driver (inodes, extents, dirs) |
| `include/common/ositofs3_format.h` | v3 on-disk layout |
| `tools/ositofs/mkfs3.c` | Host formatter for v3 |
| `tools/ositofs/write3.c` | Host file injector for v3 |
| `docs/ositofs3-spec.md` | v3 spec |

## Commit history (this sweep)

```
207900d  Phase 5: /proc/self/maps with file-backing + mprotect/fork docs
8240608  elf: re-add demand paging for static ET_EXEC binaries
105dc4c  process: remove dead per-process fd_table (proc_fd_t / p->fds)
21217fd  syscall: pipes now block by default instead of returning EAGAIN
f07b2d1  syscall: ETXTBSY on write-open of running binaries + reset stdio
c42531e  syscall: re-add getrusage (98), rt_sigsuspend (130), setitimer (38)
50df0e1  Paging: alloc kernel structures from high memory
24accd9  Stabilize: fix kthread wrapper + wire io_uring READ/WRITE
be59c00  Kernel: VFS unificado, OsitoFS v3, demand paging, compositor, crt.c
```

## Follow-up sweep (2026-04-12 pm)

X-PGTBL + static-PIE + per-process fd_table — removes the Bug #3
workaround and fixes zsh fork+exec:

```
5876fe0  x86: per-process fd_table — fix zsh fork+exec pipe sharing
18b21ce  elf+syscall: remove fork+exec gate — X-PGTBL handles isolation
4aa4c69  elf: remove demand_paged_active workaround
199ba97  x86: X-PGTBL per-process page tables — CR3 per proceso
a0e6413  elf: noinline elf_setup_stack to fix Bug #1 regression
```

Plus the user-space changes (musl rebuilt with `-fPIC`, `rcrt1.o`
installed in sysroot, zsh linker script cleaned up, `zsh.elf`
recompiled as `ET_DYN`) — these live in the `ok-ported/zsh-5.9`
submodule commit `2c9ca5e8e0`.
