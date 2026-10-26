/* SPDX-License-Identifier: GPL-2.0-only */
#include "xboot.h"

/* Board identity is checked with the apple,j713 FDT compatible by the adapter.
 * These owners describe the existing chain, not measured register contents. */
const struct xb_profile xb_j713 = {
    .chip = 0x8132, .cpus = 10,
    .image_align = 0x200000, .dt_align = 0x4000,
    .mtp_dapf_owner = XB_OWNER_LINUX,
    .dart_owner = XB_OWNER_INHERITED_UNVERIFIED,
};
