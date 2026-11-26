/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PFE_VLAN_PLAN_H
#define HSAN_PFE_VLAN_PLAN_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>

/* OEM hi_lsw_pfe.ko port_vlan_write @.text+0x3998..0x39f0 and
 * port_vlan_del @.text+0x3930..0x3994. Both use (VID & 0xfff)
 * for the PUB bank 0x0b040000 entry. Bit16 is valid and the lower
 * halfword carries the physical port membership; upper 15 bits are
 * preserved by the OEM write/delete callbacks. This is a CPU-only
 * row transformer, NOT permission to program a live forwarding table.
 */
#define HSAN_PFE_VLAN_ENTRIES 4095U
#define HSAN_PFE_VLAN_CSR_BASE 0x0b040000U
#define HSAN_PFE_LAN1_PORT 10U

static inline int hsan_pfe_vlan_plan(u32 vid, u8 port, u32 old,
                                     bool add, u32 *next)
{
    u32 members, result;

    if (!next || vid >= HSAN_PFE_VLAN_ENTRIES || port >= 16)
        return -EINVAL;
    members = old & GENMASK(15, 0);
    if (add) {
        members |= BIT(port);
        result = (old & ~GENMASK(15, 0)) | members | BIT(16);
    } else {
        members &= ~BIT(port);
        result = (old & ~GENMASK(15, 0)) | members;
        if (!members)
            result &= ~BIT(16);
    }
    *next = result;
    return 0;
}
#endif
