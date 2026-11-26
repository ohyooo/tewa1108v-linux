/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PFE_GRO_V4_PLAN_H
#define HSAN_PFE_GRO_V4_PLAN_H
#include <linux/types.h>
#include <linux/errno.h>

#define HSAN_PFE_GRO_WORK_WORDS 19U
struct hsan_pfe_gro_v4_tuple {
    u32 saddr_host;
    u32 daddr_host;
    u16 sport_host;
    u16 dport_host;
};

/* Restricted LAN1 TCP/type5 encoding from OEM pfe_napt_v4_write_flow
 * and helper @0x567c, with no NAT, tunnel or packet-header rewrite
 * Addresses/ports are CPU order, matching hi_cfe_ntoh_ip and L4 parsing
 */
static inline int hsan_pfe_gro_v4_plan(const struct hsan_pfe_gro_v4_tuple *key,
                                     u32 l3_id, u32 nhp_id, u32 gro_id,
                                     u32 words[HSAN_PFE_GRO_WORK_WORDS])
{
    unsigned int i;

    if (!key || !words || !l3_id || l3_id > 31U ||
        !nhp_id || nhp_id > 399U || !gro_id || gro_id > 4U)
        return -EINVAL;
    for (i = 0; i < HSAN_PFE_GRO_WORK_WORDS; i++)
        words[i] = 0;
    words[0] = 0x00010100U;
    words[1] = (l3_id << 26) | (nhp_id << 17);
    words[2] = (10U << 27) | 4U;
    words[3] = ((u32)key->dport_host << 16) | key->sport_host;
    words[4] = key->saddr_host;
    words[8] = key->daddr_host;
    words[12] = 0x00000603U;
    words[15] = 0x80000000U | (gro_id << 27);
    return 0;
}
#endif
