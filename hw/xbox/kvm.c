/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include "qapi/error.h"
#include "system/kvm.h"
#include "hw/xbox/kvm.h"

/*
 * CPUID describes instructions, not which guest execution modes KVM can run.
 * In particular, IA-32e support does not imply that a host without unrestricted
 * guest support can execute the Xbox's real and legacy protected modes. Probe
 * those modes in an isolated VM, including when nested VMX MSRs are
 * unavailable.
 */
int xbox_kvm_check_guest_modes(KVMState *s, int64_t tsc_khz, Error **errp)
{
    const size_t ram_size = 0x4000;
    const uint8_t code[] = { 0xb0, 0x78, 0xf4 }; /* mov al, 0x78; hlt */
    const uint8_t sse_code[] = {
        0x0f, 0x57, 0xc0, /* xorps xmm0, xmm0 */
        0x0f, 0x58, 0xc0, /* addps xmm0, xmm0 */
        0xb0, 0x78, 0xf4, /* mov al, 0x78; hlt */
    };
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = ram_size,
    };
    struct kvm_run *run = MAP_FAILED;
    void *ram = MAP_FAILED;
    int vmfd = -1, vcpufd = -1;
    int run_size = 0;
    int ret = -ENOTSUP;

    if (tsc_khz < 0 || tsc_khz > INT_MAX) {
        error_setg(errp, "sys.kvm_tsc_khz must be between 0 and %d", INT_MAX);
        return -EINVAL;
    }
    if (tsc_khz && !kvm_check_extension(s, KVM_CAP_TSC_CONTROL)) {
        error_setg(errp, "KVM TSC scaling is unavailable on this host; "
                         "set sys.kvm_tsc_khz to 0 or use TCG");
        return -ENOTSUP;
    }

    vmfd = kvm_ioctl(s, KVM_CREATE_VM, 0);
    if (vmfd < 0) {
        ret = vmfd;
        goto fail;
    }
    /* KVM uses three pages here when emulating real mode without EPT. */
    if (ioctl(vmfd, KVM_SET_TSS_ADDR, 0xfffbd000UL) < 0) {
        ret = -errno;
        goto fail;
    }
    ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        ret = -errno;
        goto fail;
    }
    memcpy((uint8_t *)ram + 0x1000, code, sizeof(code));
    /* Identity-map the probe and its page tables for legacy 32-bit paging. */
    uint32_t *page_directory = (uint32_t *)((uint8_t *)ram + 0x2000);
    uint32_t *page_table = (uint32_t *)((uint8_t *)ram + 0x3000);
    page_directory[0] = 0x3003;
    for (int i = 0; i < 4; i++) {
        page_table[i] = (i * 0x1000) | 3;
    }
    region.userspace_addr = (uintptr_t)ram;
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        ret = -errno;
        goto fail;
    }
    vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
    if (vcpufd < 0) {
        ret = -errno;
        goto fail;
    }
    if (tsc_khz && ioctl(vcpufd, KVM_SET_TSC_KHZ, (unsigned long)tsc_khz) < 0) {
        ret = -errno;
        error_setg_errno(errp, -ret,
                         "KVM cannot set the TSC frequency to "
                         "%" PRId64 " kHz",
                         tsc_khz);
        goto out;
    }
    run_size = kvm_ioctl(s, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (run_size < 0) {
        ret = run_size;
        goto fail;
    }
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    if (run == MAP_FAILED) {
        ret = -errno;
        goto fail;
    }

    for (int mode = 0; mode < 3; mode++) {
        bool protected_mode = mode != 0;
        bool paging = mode == 2;
        size_t code_size = paging ? sizeof(sse_code) : sizeof(code);
        struct kvm_sregs sregs;
        struct kvm_regs regs = { .rip = 0x1000, .rflags = 2 };
        struct kvm_segment data = {
            .base = 0,
            .limit = protected_mode ? UINT32_MAX : 0xffff,
            .selector = protected_mode ? 16 : 0,
            .type = 3,
            .present = 1,
            .s = 1,
            .db = protected_mode,
            .g = protected_mode,
        };

        if (ioctl(vcpufd, KVM_GET_SREGS, &sregs) < 0) {
            ret = -errno;
            goto fail;
        }
        sregs.cs = sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
        sregs.cs.type = 11;
        sregs.cs.selector = protected_mode ? 8 : 0;
        sregs.cr0 = protected_mode ? 0x11 : 0x10; /* ET, optionally PE; no PG */
        sregs.cr3 = sregs.cr4 = sregs.efer = 0;
        if (paging) {
            /*
             * A trivial probe may run entirely in KVM's instruction emulator.
             * SSE arithmetic with legacy paging also tests hardware execution,
             * rejecting IA-32e-only hosts even if simple instructions emulate.
             */
            memcpy((uint8_t *)ram + 0x1000, sse_code, sizeof(sse_code));
            sregs.cr0 |= 1U << 31;
            sregs.cr3 = 0x2000;
            sregs.cr4 = 1U << 9; /* OSFXSR */
        }
        if (ioctl(vcpufd, KVM_SET_SREGS, &sregs) < 0 ||
            ioctl(vcpufd, KVM_SET_REGS, &regs) < 0) {
            ret = -errno;
            goto fail;
        }
        int rc;
        do {
            rc = ioctl(vcpufd, KVM_RUN, 0);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0 || run->exit_reason != KVM_EXIT_HLT ||
            ioctl(vcpufd, KVM_GET_REGS, &regs) < 0 || regs.rax != 0x78 ||
            regs.rip != 0x1000 + code_size) {
            error_setg(errp,
                       "KVM cannot execute the Xbox's %s mode guest "
                       "(IA-32e alone is insufficient without legacy guest "
                       "or unrestricted guest support); use TCG",
                       protected_mode ? "32-bit protected" : "real");
            ret = -ENOTSUP;
            goto out;
        }
    }
    ret = 0;
    goto out;

fail:
    error_setg_errno(errp, -ret, "Could not check KVM Xbox guest modes");
out:
    if (run != MAP_FAILED) {
        munmap(run, run_size);
    }
    if (vcpufd >= 0) {
        close(vcpufd);
    }
    if (vmfd >= 0) {
        close(vmfd);
    }
    if (ram != MAP_FAILED) {
        munmap(ram, ram_size);
    }
    return ret;
}
