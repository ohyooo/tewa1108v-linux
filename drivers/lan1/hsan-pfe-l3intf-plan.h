/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PFE_L3INTF_PLAN_H
#define HSAN_PFE_L3INTF_PLAN_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/unaligned.h>

/* hi_lsw_pfe.ko l3intf_write @.text+0x3b38..0x3c74;
 * PUB descriptor #1 (table 0x102, 31 entries, 20-byte source).
 * The ARM32 callback builds TWO distinct 16-byte CSR rows:
 * 0x0b05b000 and 0x0b05c000, both indexed by the same table row.
 * This helper only translates the input. It cannot claim that the
 * hardware table manager accepted or activated these rows.
 */
#define HSAN_PFE_L3INTF_ROWS       31U
#define HSAN_PFE_L3INTF_INPUT_BYTES 20U
#define HSAN_PFE_L3INTF_BANK_A    0x0b05b000U
#define HSAN_PFE_L3INTF_BANK_B    0x0b05c000U

struct hsan_pfe_l3intf_plan {
    u32 row;
    u32 a[4];
    u32 b[4];
};

static inline int hsan_pfe_l3intf_pack(u32 row,
                                     const u8 src[HSAN_PFE_L3INTF_INPUT_BYTES],
                                     struct hsan_pfe_l3intf_plan *out)
{
    u8 packed[32] = { 0 };
    u16 flags16;
    u32 flags32, control, upper10;
    unsigned int i;

    if (!src || !out || row >= HSAN_PFE_L3INTF_ROWS)
        return -EINVAL;
    /* First 128-bit row: raw 32-bit field, two byte-reversed fields,
     * 2-bit qualifier. All destinations start at zero, matching the
     * OEM 32-byte scratch row initialized at .text+0x3b58..0x3b70.
     */
    put_unaligned_le32(get_unaligned_le32(src), packed);
    put_unaligned_be32(get_unaligned_le32(src + 6), packed + 4);
    put_unaligned_be16(get_unaligned_le16(src + 4), packed + 8);
    packed[10] = src[15] & 3U;

    /* Second 128-bit row: OEM halfword at source +10 remains LE. */
    packed[16] = ((src[16] >> 4) & 1U) << 7;
    put_unaligned_le16(get_unaligned_le16(src + 10), packed + 17);
    flags16 = get_unaligned_le16(src + 12);
    flags32 = get_unaligned_le32(src + 12);
    control = (flags16 & 0x7ffU) | BIT(19) |
              ((u32)(src[16] & 0x0fU) << 12) |
              ((u32)((src[15] >> 2) & 3U) << 16) |
              ((u32)((src[15] >> 4) & 1U) << 18) |
              ((u32)((flags16 >> 11) & 7U) << 21);
    /* MOVW #0xf7ff; MOVT #0x00ef at .text+0x3bec/0x3bf0. */
    control &= 0x00eff7ffU;
    if (flags32 & 0x00ffc000U) {
        upper10 = (flags32 >> 14) & 0x3ffU;
        packed[19] = (u8)upper10;
        control |= ((upper10 << 12) & BIT(20)) | BIT(11);
    }
    put_unaligned_le32(control, packed + 20);
    if (get_unaligned_le16(src + 16) & 0x07e0U) {
        u8 selector = (get_unaligned_le16(src + 16) >> 5) & 0x1fU;

        packed[23] = (packed[23] & ~0x3fU) | (selector | BIT(5));
    }
    out->row = row;
    for (i = 0; i < 4; ++i) {
        out->a[i] = get_unaligned_le32(packed + 4 * i);
        out->b[i] = get_unaligned_le32(packed + 16 + 4 * i);
    }
    return 0;
}

#endif
