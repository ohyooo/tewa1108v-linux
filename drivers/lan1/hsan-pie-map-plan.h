/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PIE_MAP_PLAN_H
#define HSAN_PIE_MAP_PLAN_H

/* Linux 7.3.5 port of hi_lsw_dp.ko:
 *   hi_pie_map_init @.text+0x58a8 (v300 branch)
 *   hi_set_mqoe_chn_map @.text+0x56f8/+0x57f0
 *
 * This is a PURE, bounded CSR command planner. It never calls
 * hsan_lsw_write(), so no MMIO can be performed by planning. The actual
 * GRO/WOC channel ownership is still the unported hi_kpie.ko runtime state.
 * Do not program these entries until the PIE owner + FAM + queue + PFE
 * initialization, hardware clocks and reset order have been qualified.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#define HSAN_PIE_PLAN_SLOTS 16U
#define HSAN_PIE_PLAN_MAX_WRITES (HSAN_PIE_PLAN_SLOTS * 2U)
#define HSAN_PIE_MAP_BASE 0x41000b00U

struct hsan_pie_plan_write {
	u32 logical_csr;
	u32 value;
};
struct hsan_pie_map_plan {
	struct hsan_pie_plan_write writes[HSAN_PIE_PLAN_MAX_WRITES];
	unsigned int nr_writes;
	unsigned int nr_slots;
};

static int hsan_pie_plan_add(struct hsan_pie_map_plan *p,
			     unsigned int slot, u32 value)
{
	unsigned int w = p->nr_writes;

	if (slot >= HSAN_PIE_PLAN_SLOTS ||
	    w > HSAN_PIE_PLAN_MAX_WRITES - 2)
		return -ERANGE;
	p->writes[w].logical_csr = HSAN_PIE_MAP_BASE + 4 * slot;
	p->writes[w].value = value;
	p->writes[w + 1].logical_csr = HSAN_PIE_MAP_BASE + 4 * slot + 0x80U;
	p->writes[w + 1].value = U32_MAX;
	p->nr_writes += 2;
	p->nr_slots = slot + 1;
	return 0;
}

/* OEM anonymous +0x57f0: fixed four slots, mapping count must be nonzero. */
static int hsan_pie_plan_four(struct hsan_pie_map_plan *p,
			      unsigned int slot, u32 ch, u32 start, u32 count)
{
	u32 value;
	unsigned int i;
	int ret;

	if (!count || slot > HSAN_PIE_PLAN_SLOTS - 4 ||
	    ch < 4 || ch > 7)
		return -EINVAL;
	value = (0x80000880U | ((ch - 4) << 5)) & 0x800008e0U;
	for (i = 0; i < 4; i++) {
		value = (value & 0x8ffffcffU) | ((i << 8) & 0x70000300U) |
			(((start + i % count) & 7U) << 28);
		ret = hsan_pie_plan_add(p, slot + i, value);
		if (ret)
			return ret;
	}
	return 0;
}

/* OEM anonymous +0x56f8: variable channel count, return next slot.
 * Preserve the original running value's bit12..15 accumulation.
 */
static int hsan_pie_plan_many(struct hsan_pie_map_plan *p,
			      unsigned int slot, u32 ch, u32 start, u32 count)
{
	u32 value;
	unsigned int i;
	int ret;

	if (!count || count > HSAN_PIE_PLAN_SLOTS - slot ||
	    ch < 4 || ch > 7)
		return -ERANGE;
	value = (0x80000080U | ((ch - 4) << 5) |
			((start << 28) & 0x70000000U)) & 0xf00000e0U;
	for (i = 0; i < count; i++) {
		if (i)
			value = (value & 0x8fffffffU) | ((i << 12) & 0xffffU) |
				(((start + i % count) & 7U) << 28);
		ret = hsan_pie_plan_add(p, slot + i, value);
		if (ret)
			return ret;
	}
	return 0;
}

/* Reconstructed v300 (hwver >= 0x300) flow. hi_pie_map_init first
 * calls the fixed-four helper for ch4 with gro_start capped to online
 * CPUs, then the variable helper for ch5 when gro_num != 0.
 * The old OEM code can divide by zero or write past slot16; reject these
 * BEFORE any real MMIO becomes possible. This cannot assert DMA ready.
 */
static int hsan_pie_make_v300_map_plan(struct hsan_pie_map_plan *p,
				       u32 gro_start, u32 gro_num,
				       unsigned int online_cpus)
{
	unsigned int initial_count = min_t(u32, gro_start, online_cpus);
	int ret;

	/* OEM v300 encodes GRO start in three channel bits. An oversized
	 * DT observation must never be silently wrapped to a different owner.
	 */
	if (!p || !online_cpus || gro_start > 7 ||
	    !initial_count || gro_num > 8U - gro_start)
		return -EINVAL;
	memset(p, 0, sizeof(*p));
	ret = hsan_pie_plan_four(p, 0, 4, 0, initial_count);
	if (ret)
		return ret;
	if (gro_num) {
		ret = hsan_pie_plan_many(p, 4, 5, gro_start, gro_num);
		if (ret)
			return ret;
	}
	return 0;
}
/* Cold-boot diagnostic permutation within the already owned RX4..7 set
 * No selector, ordinary RX route, sideband, or DMA geometry is changed
 */
static int hsan_pie_rotate_gro_map(struct hsan_pie_map_plan *p,
                                  u32 gro_start, u32 gro_num, u32 rotate)
{
    unsigned int i;

    if (!p || gro_start != 4 || gro_num != 4 || rotate > 3 ||
        p->nr_slots != 8 || p->nr_writes != 16)
        return -EINVAL;
    /* Complete validation before changing even the CPU-only plan */
    for (i = 0; i < 4; i++) {
        unsigned int w = 8 + 2 * i;

        if (p->writes[w].logical_csr != HSAN_PIE_MAP_BASE + 4 * (4 + i) ||
            p->writes[w + 1].logical_csr !=
                HSAN_PIE_MAP_BASE + 4 * (4 + i) + 0x80 ||
            p->writes[w + 1].value != U32_MAX ||
            ((p->writes[w].value >> 28) & 7) != 4 + i ||
            !(p->writes[w].value & 0x80000000U))
            return -EINVAL;
    }
    for (i = 0; i < 4; i++) {
        unsigned int w = 8 + 2 * i;

        p->writes[w].value = (p->writes[w].value & ~0x70000000U) |
                            ((4 + ((i + rotate) % 4)) << 28);
    }
    return 0;
}
#endif /* HSAN_PIE_MAP_PLAN_H */
