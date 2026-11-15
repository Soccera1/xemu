/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_XBOX_KVM_H
#define HW_XBOX_KVM_H

#include "qemu/typedefs.h"

int xbox_kvm_check_guest_modes(KVMState *s, int64_t tsc_khz, Error **errp);

#endif
