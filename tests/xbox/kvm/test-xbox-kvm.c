/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/kvm.h>
#include "qapi/error.h"
#include "system/kvm.h"
#include "hw/xbox/kvm.h"

static int kvmfd;
static int rejected_mode;
static int guest_mode;
static bool reject_create;
static bool fail_entry;
static bool reject_tsc_control;
static bool reject_tsc_set;
static unsigned tsc_set_count;
static int64_t requested_tsc_khz;
static int actual_tsc_khz;

/* Keep the actual VM setup, but simulate hosts that cannot enter legacy mode.
 */
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == KVM_SET_TSC_KHZ) {
        tsc_set_count++;
        requested_tsc_khz = (uintptr_t)arg;
        if (reject_tsc_set) {
            errno = EINVAL;
            return -1;
        }
    }
    if (request == KVM_SET_SREGS) {
        uint64_t cr0 = ((struct kvm_sregs *)arg)->cr0;
        guest_mode = (cr0 & (1U << 31)) ? 2 : (cr0 & 1);
    }
    if (request == KVM_RUN && rejected_mode == guest_mode) {
        if (fail_entry) {
            int size = syscall(SYS_ioctl, kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);
            struct kvm_run *run =
                mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            g_assert_true(run != MAP_FAILED);
            run->exit_reason = KVM_EXIT_FAIL_ENTRY;
            munmap(run, size);
            return 0;
        }
        errno = EINVAL;
        return -1;
    }
    int ret = syscall(SYS_ioctl, fd, request, arg);
    if (ret == 0 && request == KVM_SET_TSC_KHZ) {
        actual_tsc_khz = syscall(SYS_ioctl, fd, KVM_GET_TSC_KHZ, 0);
    }
    return ret;
}

int kvm_check_extension(KVMState *s, unsigned int extension)
{
    if (reject_tsc_control && extension == KVM_CAP_TSC_CONTROL) {
        return 0;
    }
    return syscall(SYS_ioctl, kvmfd, KVM_CHECK_EXTENSION, extension);
}

int kvm_ioctl(KVMState *s, unsigned long request, ...)
{
    if (reject_create && request == KVM_CREATE_VM) {
        return -EACCES;
    }
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int ret = ioctl(kvmfd, request, arg);
    return ret < 0 ? -errno : ret;
}

static void test_modes(void)
{
    Error *err = NULL;
    rejected_mode = -1;
    reject_create = false;
    tsc_set_count = 0;
    kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmfd < 0) {
        g_test_skip("KVM is unavailable");
        return;
    }
    int ret = xbox_kvm_check_guest_modes(NULL, 0, &err);
    if (ret < 0) {
        g_test_skip(error_get_pretty(err));
        error_free(err);
        close(kvmfd);
        return;
    }
    g_assert_null(err);
    g_assert_cmpuint(tsc_set_count, ==, 0);

    for (int exit_kind = 0; exit_kind < 2; exit_kind++) {
        fail_entry = exit_kind;
        for (int mode = 0; mode < 3; mode++) {
            rejected_mode = mode;
            g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, 0, &err), ==,
                            -ENOTSUP);
            g_assert_nonnull(strstr(error_get_pretty(err),
                                    mode ? "32-bit protected" : "real"));
            g_assert_nonnull(strstr(error_get_pretty(err), "IA-32e"));
            error_free(err);
            err = NULL;
        }
    }
    /* A failed probe must not leave the next VM unusable. */
    rejected_mode = -1;
    g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, 0, &err), ==, 0);
    g_assert_null(err);
    close(kvmfd);
}

static void test_create_failure(void)
{
    Error *err = NULL;
    reject_create = true;
    g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, 0, &err), ==, -EACCES);
    g_assert_nonnull(strstr(error_get_pretty(err), "Could not check KVM"));
    error_free(err);
    reject_create = false;
}

static void test_tsc_validation(void)
{
    const int64_t invalid[] = { -1, (int64_t)INT_MAX + 1, INT64_MAX };
    Error *err = NULL;

    for (size_t i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, invalid[i], &err), ==,
                        -EINVAL);
        g_assert_nonnull(strstr(error_get_pretty(err), "sys.kvm_tsc_khz"));
        error_free(err);
        err = NULL;
    }
    reject_tsc_control = true;
    g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, 733333, &err), ==,
                    -ENOTSUP);
    g_assert_nonnull(
        strstr(error_get_pretty(err), "TSC scaling is unavailable"));
    error_free(err);
    reject_tsc_control = false;
}

static void test_tsc_scaling(void)
{
    const int64_t frequencies[] = { 733333, 1000000 };
    Error *err = NULL;

    kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmfd < 0) {
        g_test_skip("KVM is unavailable");
        return;
    }
    if (kvm_check_extension(NULL, KVM_CAP_TSC_CONTROL) <= 0 ||
        kvm_check_extension(NULL, KVM_CAP_GET_TSC_KHZ) <= 0) {
        g_test_skip("KVM TSC scaling or frequency readback is unavailable");
        close(kvmfd);
        return;
    }
    rejected_mode = -1;
    for (size_t i = 0; i < G_N_ELEMENTS(frequencies); i++) {
        tsc_set_count = 0;
        g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, frequencies[i], &err),
                        ==, 0);
        g_assert_null(err);
        g_assert_cmpuint(tsc_set_count, ==, 1);
        g_assert_cmpint(requested_tsc_khz, ==, frequencies[i]);
        g_assert_cmpint(actual_tsc_khz, ==, frequencies[i]);
    }
    reject_tsc_set = true;
    g_assert_cmpint(xbox_kvm_check_guest_modes(NULL, 733333, &err), ==,
                    -EINVAL);
    g_assert_nonnull(strstr(error_get_pretty(err), "733333 kHz"));
    error_free(err);
    reject_tsc_set = false;
    close(kvmfd);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/kvm/guest-modes", test_modes);
    g_test_add_func("/xbox/kvm/create-failure", test_create_failure);
    g_test_add_func("/xbox/kvm/tsc-validation", test_tsc_validation);
    g_test_add_func("/xbox/kvm/tsc-scaling", test_tsc_scaling);
    return g_test_run();
}
