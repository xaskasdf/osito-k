#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/kvm.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* No KVM_SET_GUEST_DEBUG, debugger, QEMU or Osito-K code participates.
 * Each case uses a fresh VM. A mismatched frame is an actual test failure. */
enum { MEMORY_SIZE = 2 * 1024 * 1024, CODE = 0x8000, HANDLER = 0x9000,
       GDT = 0x4000, IDT = 0x5000, TSS = 0x6000, RESULT = 0x7000 };
static volatile sig_atomic_t timed_out;
static sigjmp_buf user_return;
static volatile uintptr_t user_rip;
static volatile sig_atomic_t user_signal, user_signal_code;

extern void dos_debug_user_icebp(void);
extern const char dos_debug_user_resume[];
__asm__(".text\n.global dos_debug_user_icebp\n"
        "dos_debug_user_icebp: .byte 0xf1\n"
        ".global dos_debug_user_resume\n"
        "dos_debug_user_resume: ret\n");

static void user_trap(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    user_signal = signal;
    user_signal_code = info->si_code;
    user_rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    /* Leave via libc, without repairing a possibly incorrect trap frame. */
    siglongjmp(user_return, 1);
}

static void timeout_handler(int signal)
{
    (void)signal;
    timed_out = 1;
}

static void require(bool success, const char *operation)
{
    if (!success) {
        perror(operation);
        exit(2);
    }
}

static bool userspace_case(void)
{
    struct sigaction old, action = { .sa_sigaction = user_trap, .sa_flags = SA_SIGINFO };
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGTRAP, &action, &old) == 0, "sigaction SIGTRAP");
    if (sigsetjmp(user_return, 1) == 0) dos_debug_user_icebp();
    require(sigaction(SIGTRAP, &old, NULL) == 0, "restore SIGTRAP");
    bool success = user_signal == SIGTRAP && user_rip == (uintptr_t)dos_debug_user_resume;
    printf("%s LINUX USER ICEBP rip=%" PRIxPTR " expected=%" PRIxPTR " signal=%d code=%d\n",
           success ? "PASS" : "FAIL", user_rip, (uintptr_t)dos_debug_user_resume,
           user_signal, user_signal_code);
    return success;
}

static void put64(uint8_t *memory, unsigned address, uint64_t value)
{
    memcpy(memory + address, &value, sizeof(value));
}

static struct kvm_segment segment(uint16_t selector, uint32_t base,
                                  unsigned type, bool long_mode)
{
    return (struct kvm_segment){ .base = base, .limit = 0xffffffff,
        .selector = selector, .type = type, .present = 1, .dpl = selector & 3,
        .db = !long_mode, .s = 1, .l = long_mode, .g = 1 };
}

static uint64_t descriptor(struct kvm_segment seg)
{
    uint64_t limit = seg.limit >> (seg.g ? 12 : 0);
    uint64_t access = seg.type | (seg.s << 4) | (seg.dpl << 5) | (seg.present << 7);
    uint64_t flags = seg.avl | (seg.l << 1) | (seg.db << 2) | (seg.g << 3);
    return (limit & 0xffff) | ((seg.base & 0xffffff) << 16) | (access << 40) |
           ((limit & 0xf0000) << 32) | (flags << 52) | ((seg.base >> 24) << 56);
}

struct result {
    uint64_t rip, cs, flags, sp, ss, dr6, dr7;
};

struct profile {
    const char *name;
    bool long_mode;
    unsigned cpl;
    uint32_t base;
};

struct test_case {
    const char *name;
    uint8_t bytes[12];
    unsigned length;
    unsigned return_offset;
    uint64_t flags, dr0, dr7, causes;
};

static bool run_case(int kvm, const uint8_t *handler, size_t handler_size,
                     const struct profile *profile, const struct test_case *test,
                     bool ist, bool interrupt_gate)
{
    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    require(vm >= 0, "KVM_CREATE_VM");
    uint8_t *memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(memory != MAP_FAILED, "mmap memory");
    /* Fault in host backing; nested translations may still be populated lazily. */
    memset(memory, 0, MEMORY_SIZE);
    struct kvm_userspace_memory_region region = { .slot = 0,
        .memory_size = MEMORY_SIZE, .userspace_addr = (uintptr_t)memory };
    require(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) == 0, "KVM_SET_USER_MEMORY_REGION");
    int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    require(vcpu >= 0, "KVM_CREATE_VCPU");
    int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    require(run_size >= (int)sizeof(struct kvm_run), "KVM_GET_VCPU_MMAP_SIZE");
    struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    require(run != MAP_FAILED, "mmap vcpu");

    put64(memory, 0x1000, 0x2007);
    put64(memory, 0x2000, 0x3007);
    put64(memory, 0x3000, 0x0087);
    struct kvm_segment kernel_cs = segment(8, 0, 11, true);
    struct kvm_segment kernel_ds = segment(16, 0, 3, false);
    struct kvm_segment cs = segment(24 | profile->cpl, profile->base, 11, profile->long_mode);
    struct kvm_segment ds = segment(32 | profile->cpl, 0, 3, false);
    struct kvm_segment tr = { .base = TSS, .limit = 103, .selector = 40,
        .type = 11, .present = 1 };
    put64(memory, GDT + 8, descriptor(kernel_cs));
    put64(memory, GDT + 16, descriptor(kernel_ds));
    put64(memory, GDT + 24, descriptor(cs));
    put64(memory, GDT + 32, descriptor(ds));
    put64(memory, GDT + 40, descriptor(tr));
    put64(memory, TSS + 4, 0x18000);
    put64(memory, TSS + 36, 0x19000);
    memory[TSS + 102] = 104;
    uint64_t gate = (HANDLER & 0xffff) | (8ull << 16) | ((uint64_t)ist << 32) |
        ((uint64_t)(interrupt_gate ? 0x8e : 0x8f) << 40) |
        ((uint64_t)(HANDLER >> 16) << 48);
    put64(memory, IDT + 16, gate);
    memcpy(memory + CODE + profile->base, test->bytes, test->length);
    memcpy(memory + HANDLER, handler, handler_size);
    struct kvm_sregs sregs;
    require(ioctl(vcpu, KVM_GET_SREGS, &sregs) == 0, "KVM_GET_SREGS");
    sregs.cs = cs;
    sregs.ds = sregs.es = sregs.ss = sregs.fs = sregs.gs = ds;
    sregs.tr = tr;
    sregs.ldt = (struct kvm_segment){ .unusable = 1 };
    sregs.gdt = (struct kvm_dtable){ .base = GDT, .limit = 55 };
    sregs.idt = (struct kvm_dtable){ .base = IDT, .limit = 4095 };
    sregs.cr0 = 0x80000011;
    sregs.cr3 = 0x1000;
    sregs.cr4 = 0x20;
    sregs.efer = 0x500;
    require(ioctl(vcpu, KVM_SET_SREGS, &sregs) == 0, "KVM_SET_SREGS");
    struct kvm_regs regs = { .rip = CODE, .rsp = 0x20000, .rsi = 0x7700,
        .rflags = 2 | test->flags };
    require(ioctl(vcpu, KVM_SET_REGS, &regs) == 0, "KVM_SET_REGS");
    struct kvm_debugregs debug = { .dr6 = 0xffff0ff0, .dr7 = test->dr7 };
    debug.db[0] = test->dr0 == CODE ? CODE + profile->base : test->dr0;
    require(ioctl(vcpu, KVM_SET_DEBUGREGS, &debug) == 0, "KVM_SET_DEBUGREGS");
    timed_out = 0;
    alarm(3);
    int rc = ioctl(vcpu, KVM_RUN, 0);
    alarm(0);
    struct result result;
    memcpy(&result, memory + RESULT, sizeof(result));
    bool delivered = rc == 0 && run->exit_reason == KVM_EXIT_IO &&
        run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0xe9 &&
        run->io.size == 1 && run->io.count == 1 &&
        *((uint8_t *)run + run->io.data_offset) == 0xdb;
    const uint64_t cause_mask = 0xe00f;
    /* Instruction-breakpoint #DB, unlike other faults, does not set RF.
     * The debugger must set it when it wants to retry past that breakpoint. */
    bool success = delivered && result.rip == CODE + test->return_offset &&
        result.cs == cs.selector && result.ss == ds.selector && result.sp == regs.rsp &&
        (result.dr6 & cause_mask) == test->causes &&
        result.flags == regs.rflags;
    printf("%s %s ist=%u gate=%s %s rip=%08" PRIx64 " expected=%08x"
           " cs=%04" PRIx64 " ss=%04" PRIx64 " sp=%08" PRIx64
           " flags=%08" PRIx64 " dr6=%08" PRIx64 " dr7=%08" PRIx64
           " delivered=%u exit=%u timeout=%u\n",
           success ? "PASS" : "FAIL", profile->name, ist, interrupt_gate ? "interrupt" : "trap",
           test->name, result.rip, CODE + test->return_offset, result.cs, result.ss,
           result.sp, result.flags, result.dr6, result.dr7, delivered,
           rc == 0 ? run->exit_reason : UINT32_MAX, timed_out != 0);
    require(munmap(run, run_size) == 0, "munmap vcpu");
    close(vcpu);
    close(vm);
    require(munmap(memory, MEMORY_SIZE) == 0, "munmap memory");
    return success;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s handler.bin\n", argv[0]);
        return 2;
    }
    uint8_t handler[4096];
    FILE *file = fopen(argv[1], "rb");
    require(file != NULL, "open handler");
    size_t size = fread(handler, 1, sizeof(handler), file);
    require(size > 0 && size < sizeof(handler) && !ferror(file), "read handler");
    fclose(file);
    struct sigaction action = { .sa_handler = timeout_handler };
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGALRM, &action, NULL) == 0, "sigaction");
    bool user_passed = userspace_case();
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    require(kvm >= 0, "open /dev/kvm");
    require(ioctl(kvm, KVM_GET_API_VERSION, 0) == KVM_API_VERSION, "KVM_GET_API_VERSION");
    const struct profile profiles[] = {
        { "long64-ring0", true, 0, 0 }, { "long64-ring3", true, 3, 0 },
        { "compat32-ring0", false, 0, 0 }, { "compat32-ring3", false, 3, 0 },
        { "compat32-base-ring3", false, 3, 0x10000 }
    };
    const struct test_case tests[] = {
        { "icebp", { 0xf1, 0xf4 }, 2, 1, 0, 0, 0x400, 0 },
        { "icebp66", { 0x66, 0xf1, 0xf4 }, 3, 2, 0, 0, 0x400, 0 },
        { "icebp-pair", { 0xf1, 0xf1, 0xf4 }, 3, 1, 0, 0, 0x400, 0 },
        { "tf-before-icebp", { 0x90, 0xf1, 0xf4 }, 3, 1, 0x100, 0, 0x400, 0x4000 },
        { "execute-breakpoint", { 0xf1, 0xf4 }, 2, 0, 0, CODE, 0x401, 1 },
        /* MOV byte [ESI/RSI],1 has the same encoding in both code modes. */
        { "data-breakpoint", { 0xc6, 0x06, 1, 0xf1 }, 4, 3, 0, 0x7700, 0x10401, 1 }
    };
    unsigned passed = 0, failed = 0;
    for (unsigned p = 0; p < sizeof(profiles) / sizeof(profiles[0]); ++p)
        for (unsigned ist = 0; ist < 2; ++ist)
            for (unsigned gate = 0; gate < 2; ++gate)
                for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); ++t) {
                    if (run_case(kvm, handler, size, &profiles[p], &tests[t], ist, gate)) ++passed;
                    else ++failed;
                }
    close(kvm);
    printf("KVM DEBUG CONTRACT: %u passed, %u failed\n", passed, failed);
    return failed || !user_passed ? 1 : 0;
}
