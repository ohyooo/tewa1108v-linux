// SPDX-License-Identifier: GPL-2.0-only
/* hi_lsw_dp.ko hi_dp_probe @.text+0xcc8: five original .rodata banks.
 * The original writes CSR registers, resets and initialises FAM+queue+PIE.
 * This Linux7 port maps/validates resources and optionally programs the
 * separately audited register prelude. It NEVER claims datapath readiness
 * before FAM, queue, PIE map and PFE owners complete the entire init graph.
 */
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/reset.h>
#include <linux/mutex.h>
#include "hsan-lsw-dp.h"
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "hsan-lsw-platform.h"
#include "hsan-lsw-csr.h"
#include "hsan-pie-map-plan.h"

static const struct hsan_lsw_window hsan_dp_windows[] = {
	{0x40, "dp",  0x15000000U, 0x80000U},
	{0x41, "eps", 0x15280000U, 0x80000U},
	{0x42, "eqm", 0x15200000U, 0x80000U},
	{0x43, "fam", 0x15100000U, 0x80000U},
	{0x44, "ips", 0x15080000U, 0x80000U},
};
static const struct hsan_lsw_mapping hsan_dp_cfg = {
	.windows = hsan_dp_windows,
	.nr_windows = ARRAY_SIZE(hsan_dp_windows),
};
/* Original hi_dp_probe @0xe78..0xf94 global register writes.
 * The @0x928 register helper is ported separately below; this function
 * stops short of the enable CSR and never declares datapath ready.
 * This function is not called unless FDT explicitly opts in, and the
 * bank dispatcher separately permits CSR writes at module load time.
 */
static const struct { u32 reg, val; } hsan_dp_global_writes[] = {
		{0x44000008U, 42},
		{0x44000580U, 6},
		{0x44000360U, 0x00015555U},
		{0x44000600U, 300}, {0x4400060cU, 300},
		{0x44000618U, 300}, {0x44000624U, 300},
		{0x44000604U, 576}, {0x44000610U, 576},
		{0x4400061cU, 576}, {0x44000628U, 576},
		{0x44000630U, 255}, {0x44000638U, 15},
		{0x40000144U, 16384},
};

static int hsan_dp_verified_global_csrs(void)
{
	unsigned int i;
	u32 v;
	int ret;

	/* Validate the entire register set BEFORE the first irreversible write. */
	ret = hsan_lsw_check_span(0x44000010U, sizeof(u32), true);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(hsan_dp_global_writes); i++) {
		ret = hsan_lsw_check_span(hsan_dp_global_writes[i].reg, sizeof(u32), true);
		if (ret)
			return ret;
	}
	ret = hsan_lsw_check_span(0x42000600U, 16 * sizeof(u32), true);
	if (ret)
		return ret;
	ret = hsan_lsw_read(0x44000010U, &v);
	if (ret)
		return ret;
	ret = hsan_lsw_write(0x44000010U, v | BIT(16));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(hsan_dp_global_writes); i++) {
		ret = hsan_lsw_write(hsan_dp_global_writes[i].reg, hsan_dp_global_writes[i].val);
		if (ret)
			return ret;
	}
	for (i = 0; i < 16; i++) {
		ret = hsan_lsw_write(0x42000600U + 4 * i, 1024);
		if (ret)
			return ret;
	}
	/* OEM calls an unported helper @0x928 before writing 0x4400007c;
	 * do NOT speculatively activate that last enable bit here.
	 */
	return 0;
}

/* hi_lsw_dp anonymous .text+0x928: the register-init chain that follows
 * hi_dp_probe's global setup. This port is independent from the unfinished
 * FAM, queue and PIE map owners. Only an explicit *second* experimental DT
 * property can request it; merely mapping banks never executes this code.
 *
 * The four command payloads come from .text+0x7ec's 10-byte packer, using
 * the original local arrays (.text+0x928) and endian reversal:
 *   a[0..1]={f1,13}, a[2..19]=0;
 *   b[0..1]={0,0}, b[2..19]=ff.
 * Commands are ordered (channel 0,2,1,3) with offsets (10,0,10,0).
 * They match reconstruction/{dp_pack,dp_regs,dp_program}_reference.c.
 */
struct hsan_dp_cmd {
	u32 channel;
	u32 words[3];
};
static const struct hsan_dp_cmd hsan_dp_init_cmds[] = {
	{ 0, { 0, 0, 0 } },
	{ 2, { 0x0000f113U, 0, 0 } },
	{ 1, { 0x0000ffffU, 0xffffffffU, 0xffffffffU } },
	{ 3, { 0, 0xffffffffU, 0xffffffffU } },
};

/* hi_dp_register_init .text+0x928 first performs seven read-modify-write
 * operations at the DP bank 0x44. Avoid any speculative reset or enable.
 */
static int hsan_dp_init_oem_global_fields(void)
{
	static const u32 addrs[] = {
		0x44000468U, 0x4400046cU, 0x44000470U,
		0x44000474U, 0x44000478U, 0x44000400U,
		0x44000428U,
	};
	unsigned int i;
	int ret;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(addrs); ++i) {
		ret = hsan_lsw_read(addrs[i], &v);
		if (ret)
			return ret;
		if (i == 0)
			v = (v & 0x80808080U) | 0x0c0d0000U;
		else if (i < 5)
			v &= 0x80808080U;
		else if (i == 5)
			v |= BIT(0);
		else
			v = (v & 0xfec00000U) | 0x01030000U | 0x0000ffefU;
		ret = hsan_lsw_write(addrs[i], v);
		if (ret)
			return ret;
	}
	return 0;
}

/* hi_dp_program_regs .text+0x878: 1001 polls of CSR 0x44000404.
 * OEM would still issue a command after a timeout; this Linux port
 * deliberately fails closed, never writing a command to a busy engine.
 */
static int hsan_dp_wait_command_idle(void)
{
	u32 status = 0;
	unsigned int attempts;
	int ret;

	for (attempts = 0; attempts < 1001; ++attempts) {
		ret = hsan_lsw_read(0x44000404U, &status);
		if (ret)
			return ret;
		if (!(status & BIT(31)))
			break;
		cpu_relax();
	}
	return attempts == 1001 ? -ETIMEDOUT : 0;
}

static int hsan_dp_issue_command(const struct hsan_dp_cmd *cmd)
{
	int ret;

	ret = hsan_dp_wait_command_idle();
	if (ret)
		return ret;

	ret = hsan_lsw_write(0x44000408U, cmd->words[0]);
	if (ret)
		return ret;
	ret = hsan_lsw_write(0x4400040cU, cmd->words[1]);
	if (ret)
		return ret;
	ret = hsan_lsw_write(0x44000410U, cmd->words[2]);
	if (ret)
		return ret;
	ret = hsan_lsw_write(0x44000404U,
		0xc0000000U | ((cmd->channel & 3U) << 16));
	if (ret)
		return ret;
	/* The next command used to drain the previous one, leaving the fourth
	 * command unobserved at handoff. Wait on the same busy bit after every
	 * submission; this is command completion, not a FAM/DMA STOP ACK.
	 */
	return hsan_dp_wait_command_idle();
}

static int hsan_dp_verified_register_init(void)
{
    static const u32 prelude[] = {
        0x44000468U, 0x4400046cU, 0x44000470U,
        0x44000474U, 0x44000478U, 0x44000400U,
        0x44000428U, 0x44000404U, 0x44000408U,
        0x4400040cU, 0x44000410U,
    };
	unsigned int i;
	int ret;

    for (i = 0; i < ARRAY_SIZE(prelude); i++) {
        ret = hsan_lsw_check_span(prelude[i], sizeof(u32), true);
        if (ret)
            return ret;
    }
	ret = hsan_dp_init_oem_global_fields();
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(hsan_dp_init_cmds); ++i) {
		ret = hsan_dp_issue_command(&hsan_dp_init_cmds[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* An *optional* observed channel contract (not present in OEM DTB) can
 * generate a reviewable plan. It does NOT program PIE or claim it is ready.
 * The numbers must come from the future audited hi_kpie channel owner,
 * never from TX/RX ring *sizes* or an assumed port number.
 */
static int hsan_dp_read_pie_channels(struct device_node *node, u32 gro[2])
{
	static const char property[] = "hsan,observed-pie-gro-channels";
	int count, ret;

	count = of_property_count_u32_elems(node, property);
	if (count < 0)
		return count;
	if (count != 2)
		return -EINVAL;
	ret = of_property_read_u32_array(node, property, gro, 2);
	if (ret)
		return ret;
	/* OEM @0x5934 skips the GRO map when its explicit count is zero.
	 * A missing observation is distinct from that valid no-GRO mode.
	 */
	if (!gro[0] || gro[0] > 7 || gro[1] > 8U - gro[0])
		return -ERANGE;
	return 0;
}

static int hsan_dp_make_pie_map_plan(struct platform_device *pdev,
                                      struct hsan_pie_map_plan *plan,
                                      u32 start, u32 count, unsigned int cpus)
{
    static const char property[] = "hsan,experimental-gro-rx-rotate";
    u32 rotate;
    int ret;

    ret = hsan_pie_make_v300_map_plan(plan, start, count, cpus);
    if (ret || !of_find_property(pdev->dev.of_node, property, NULL))
        return ret;
    if (of_property_count_u32_elems(pdev->dev.of_node, property) != 1)
        return -EINVAL;
    ret = of_property_read_u32(pdev->dev.of_node, property, &rotate);
    if (ret)
        return ret;
    return hsan_pie_rotate_gro_map(plan, start, count, rotate);
}

static int hsan_dp_readonly_pie_plan(struct platform_device *pdev)
{
	struct hsan_pie_map_plan *plan;
	u32 gro[2];
	unsigned int i;
	int ret;

	if (!of_find_property(pdev->dev.of_node,
			      "hsan,observed-pie-gro-channels", NULL))
		return 0; /* no trusted PIE channel owner supplied */
	ret = hsan_dp_read_pie_channels(pdev->dev.of_node, gro);
	if (ret)
		return ret;
	plan = devm_kzalloc(&pdev->dev, sizeof(*plan), GFP_KERNEL);
	if (!plan)
		return -ENOMEM;
	ret = hsan_dp_make_pie_map_plan(pdev, plan, gro[0], gro[1],
					 num_online_cpus());
	if (ret) {
		dev_err(&pdev->dev,
			"PIE v300 shadow map invalid: gro start=%u count=%u cpus=%u (%d)\n",
			gro[0], gro[1], num_online_cpus(), ret);
		return ret;
	}
	for (i = 0; i < plan->nr_writes; i++)
		dev_dbg(&pdev->dev, "PIE plan CSR %#08x = %#08x (NO MMIO)\n",
			plan->writes[i].logical_csr, plan->writes[i].value);
	dev_info(&pdev->dev,
		 "PIE v300 shadow map: %u slots %u writes, NO MMIO and NO DMA readiness\n",
		 plan->nr_slots, plan->nr_writes);
	return 0;
}


/* Original hi_mem_info_init @.text+0x5a80 discovers two no-map pools.
 * This is a geometry/ownership probe ONLY.  It never ioremaps reserved RAM.
 * The 32-bit OEM mem-info array cannot be reused as a Linux arm64 pointer.
 */
#define HSAN_DP_QUEUE_SLOTS 30U
#define HSAN_DP_QUEUE_STRIDE 44U
#define HSAN_DP_FAM_RING_BYTES 655360U  /* OEM hi_fam_init_buf DMA allocation */
/* hi_fam_init_buf @0x227c-0x2288: rows are min(calculated, 131072),
 * rounded DOWN to 32. The allocation holds 163840 u32 slots, but the
 * remaining tail is NOT an additional 32768 permissible hardware rows.
 */
#define HSAN_DP_FAM_MAX_ENTRIES 131072U
#define HSAN_DP_FAM_ROW_ALIGNMENT 32U
#define HSAN_DP_FAM_DMA_ATTRS 0UL

/* OEM hi_queue_init @0x52c4..0x5324: hi_tab_mgr_create(ID=0),
 * .cfg[8]=63, .cfg[16]=4, and hi_queue_exit deletes ID=0.
 * This is only a software owner placeholder, not the EQM 0x42000048
 * command interface or the OEM 32-bit callback ABI.
 */
#define HSAN_DP_EQM_SHT_ID 0U
#define HSAN_DP_EQM_SHT_ENTRIES 63U
#define HSAN_DP_EQM_SHT_BYTES 4U

struct hsan_dp_port_queue {
	u32 qid;
	u32 count;
	bool observed;
};

struct hsan_dp_queue_slot {
	u32 state, port, qid, count;
	u32 oem_private[7];
};
static_assert(sizeof(struct hsan_dp_queue_slot) == HSAN_DP_QUEUE_STRIDE);

struct hsan_dp_state {
    bool eqm_counter_diagnostic;
	enum hsan_dp_init_stage init_stage;
	int init_errno;
	bool mmio_attempted;
	bool quarantined;
	struct clk_bulk_data *clks;
	int num_clks;
	bool clocks_on;
	bool boot_reset_completed;
	bool queue_routing_verified;
	struct reset_control_bulk_data resets[4];
	bool reserved_verified;
	struct resource queue_ram, flow_ram;
	void *fam_ring;
	dma_addr_t fam_dma;
    bool fam_dma_range_32bit;
    bool fam_cpu_table_verified;
    bool fam_dma_attrs_zero;
    bool fam_hw_owned; /* arm attempted: DMA may remain owned even without ACK */
    bool fam_oem_runtime_geometry_verified;
    u32 fam_oem_derived_entries;
    u32 fam_oem_memory_base;
    u32 fam_oem_unit_bytes;
    u32 fam_oem_stride;
    /* Optional explicitly observed EPS L3 count row, independent of the
     * OEM port10 queue ID. This is a read-only diagnostics selector.
     */
    bool eps_l3_counter_row_known;
    u32 eps_l3_counter_row;
    bool fam_hw_attempted;
    bool fam_hw_ack_seen;
	struct hsan_dp_queue_slot *observed_queues;
	struct hsan_dp_port_queue port_queues[64];
	u32 active_port_bitmap[2];
	u32 expected_port_bitmap[2];
	u32 committed_queue_rows;
    u32 expected_queue_rows;
    u32 verified_queue_rows;
	bool eqm_hardware_tainted;
	bool eqm_sht_shadow_registered;
	bool eqm_bootstrap_hw_written;
	u32 fam_entries_prepared;
	bool pie_map_hw_committed;
	u32 pie_gro_start;
	u32 pie_gro_count;
    u32 pie_map_expected_cpus;
	bool pie_map_hw_uncertain;
	u32 pie_map_hw_words;
};

/* A partially written PIE map has no audited reset/replay sequence.
 * This latch survives probe failure and device rebind until module unload.
 */
static atomic_t hsan_dp_pie_map_attempted = ATOMIC_INIT(0);
/* A PIE stop closes the CPU publication side of FAM for the remainder
 * of the boot. No OEM FAM DMA STOP/IRQ-drain ACK has been recovered.
 */
static atomic_t hsan_dp_fam_stop_fenced = ATOMIC_INIT(0);
/* Global CSR and command prelude are not reversible. Never replay them in
 * the same module lifetime after a partially committed probe failure.
 */
static atomic_t hsan_dp_global_attempted = ATOMIC_INIT(0);
static atomic_t hsan_dp_registers_attempted = ATOMIC_INIT(0);
static atomic_t hsan_dp_boot_reset_attempted = ATOMIC_INIT(0);
/* DP global/queue/PIE writes have no qualified reset/replay handshake.
 * Keep these static once-only guards resident even if sysfs unbinds DP.
 */
static atomic_t hsan_dp_lifetime_pinned = ATOMIC_INIT(0);
static void hsan_dp_pin_hw_lifetime(void)
{
    if (atomic_cmpxchg(&hsan_dp_lifetime_pinned, 0, 1) == 0) {
        __module_get(THIS_MODULE);
        pr_warn("hsan-lsw-dp: non-reversible DP MMIO attempt; module pinned until board reset\n");
    }
}
static DEFINE_MUTEX(hsan_dp_owner_lock);
/* Resource preflight and non-replayable init must have one probe owner. */
static DEFINE_MUTEX(hsan_dp_probe_lock);
static struct hsan_dp_state *hsan_dp_owner;
/* Lifetime history protected by owner_lock. Forced removal destroys devres,
 * but does not prove the engine stopped or make an old arm safely reusable.
 */
static struct hsan_fam_init_record hsan_dp_fam_arm_history;

/* Exact queue ranges recovered from the active OEM port10 and CPU rows.
 * Sleeping context only: the existing EQM API serializes shared staging,
 * checks each command, and poisons the engine after an uncertain failure.
 * Partial rows are explicitly invalid; no retries and no DMA release.
 */
int hsan_lsw_dp_queue_counters(struct hsan_dp_queue_counters *out)
{
    struct hsan_dp_state *state;
    unsigned int i;
    int ret = -ENODEV;

    memset(out, 0, sizeof(*out));
    mutex_lock(&hsan_dp_owner_lock);
    state = hsan_dp_owner;
    if (!state || !state->clocks_on || state->quarantined)
        goto unlock;
    ret = -EACCES;
    if (!state->eqm_counter_diagnostic || !state->queue_routing_verified ||
        !state->pie_map_hw_committed)
        goto unlock;
    for (i = 0; i < HSAN_DP_DIAG_QUEUE_COUNT; i++) {
        u32 table80[3], table81[4];
        u16 qid = i < 8 ? 81 + i : (i < 56 ? 129 + i - 8 : 33 + i - 56);

        if (hsan_lsw_dp_fam_cpu_fenced()) {
            ret = -ESHUTDOWN;
            goto unlock;
        }
        ret = hsan_lsw_eqm_read_table(81, qid, table81, 4, NULL);
        if (ret)
            goto unlock;
        ret = hsan_lsw_eqm_read_table(80, qid, table80, 3, NULL);
        if (ret)
            goto unlock;
        memcpy(out->words[i], table81, sizeof(table81));
        out->words[i][4] = table80[2];
        out->valid[i] = true;
    }
unlock:
    mutex_unlock(&hsan_dp_owner_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_dp_queue_counters);

int hsan_lsw_dp_init_snapshot(struct hsan_dp_init_snapshot *out)
{
	struct hsan_dp_state *state;
	int ret = 0;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	mutex_lock(&hsan_dp_owner_lock);
	state = hsan_dp_owner;
	if (!state) {
		ret = -ENODEV;
	} else {
		out->stage = state->init_stage;
		out->last_errno = state->init_errno;
		out->mmio_attempted = state->mmio_attempted;
		out->quarantined = state->quarantined;
		out->queue_routing_verified = state->queue_routing_verified;
		out->boot_reset_completed = state->boot_reset_completed;
		out->pie_map_geometry_valid = state->pie_map_hw_committed &&
			!state->pie_map_hw_uncertain && !state->quarantined;
		out->pie_gro_start = state->pie_gro_start;
		out->pie_gro_count = state->pie_gro_count;
	}
	mutex_unlock(&hsan_dp_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_dp_init_snapshot);

int hsan_lsw_dp_get_queue(u32 port, struct hsan_dp_queue_snapshot *out)
{
	struct hsan_dp_state *state;
	int ret = 0;

	if (!out || port >= 64)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	mutex_lock(&hsan_dp_owner_lock);
	state = hsan_dp_owner;
	if (!state)
		ret = -ENODEV;
	else if (!state->port_queues[port].observed)
		ret = -ENOENT;
	else {
		out->port = port;
		out->qid = state->port_queues[port].qid;
		out->count = state->port_queues[port].count;
		out->reserved_memory_verified = state->reserved_verified;
		out->fam_addresses_prepared = state->fam_cpu_table_verified &&
					  !!state->fam_entries_prepared;
		out->eqm_bootstrap_committed = state->eqm_bootstrap_hw_written;
        /* Partial writes are not an installed queue table. Report success
         * only after all observed rows were committed AND read back.
         */
        out->eqm_queue_rows_committed =
            !state->eqm_hardware_tainted &&
            state->expected_queue_rows &&
            state->committed_queue_rows == state->expected_queue_rows &&
            state->verified_queue_rows == state->expected_queue_rows;
		out->pie_map_hw_committed = state->pie_map_hw_committed;
		out->pie_map_hw_uncertain = state->pie_map_hw_uncertain;
        out->pie_map_hw_words = state->pie_map_hw_words;
        out->fam_entry_count = state->fam_entries_prepared;
        out->fam_dma_range_32bit = state->fam_dma_range_32bit;
        out->fam_cpu_table_verified = state->fam_cpu_table_verified;
        out->fam_dma_attrs_zero = state->fam_dma_attrs_zero;
        out->fam_hw_owned = state->fam_hw_owned;
        out->fam_oem_runtime_geometry_verified =
            state->fam_oem_runtime_geometry_verified;
        out->fam_oem_derived_entries = state->fam_oem_derived_entries;
        out->fam_hw_attempted = state->fam_hw_attempted;
        out->fam_hw_ack_seen = state->fam_hw_ack_seen;
        {
            struct hsan_lsw_fam_fence_snapshot fence;

            hsan_lsw_fam_fence_snapshot(&fence);
            out->fam_cpu_stop_fenced = fence.cpu_fenced ||
                atomic_read(&hsan_dp_fam_stop_fenced);
            out->fam_rejected_csr_writes = fence.rejected_write_operations;
            out->fam_rejected_pie_publications =
                fence.rejected_dma_publications;
            out->fam_admitted_cpu_publications =
                fence.admitted_cpu_publications;
            out->fam_admitted_tx_reclaims =
                fence.admitted_tx_reclaims;
            out->fam_rejected_tx_reclaims =
                fence.rejected_tx_reclaims;
            out->fam_stop_fence_invocations =
                fence.stop_fence_invocations;
        }
        out->fam_hw_stop_ack_seen = false;
        /* hi_tab_read_l3_cnt @0x50 reads four words via bank41 table128.
         * OEM returns two counters: words[0], then words[1] with the
         * low byte of words[2] as its upper 8 bits. It clears the first
         * count's upper dword. Do not infer any row-to-port mapping.
         */
        out->eps_l3_counter_row_known = state->eps_l3_counter_row_known;
        out->eps_l3_counter_row = state->eps_l3_counter_row;
        if (!state->quarantined &&
            state->eps_l3_counter_row_known && state->clocks_on) {
            u32 words[4];

            if (!hsan_lsw_read_tab128(0x4106a000U,
                    state->eps_l3_counter_row, words)) {
                out->eps_l3_counter_sampled = true;
                out->eps_l3_first_low32 = words[0];
                out->eps_l3_second_low32 = words[1];
                out->eps_l3_second_high8 = words[2] & 0xffU;
            }
        }
        if (!state->quarantined &&
            state->fam_hw_attempted && state->clocks_on) {
            struct hsan_lsw_fam_activity activity;

            if (!hsan_lsw_fam_read_activity(&activity)) {
                out->fam_activity_sampled = true;
                out->fam_activity_21bit = activity.value_21bit;
                out->fam_activity_14bit = activity.value_14bit;
                out->fam_init_status_raw = activity.raw_init_ack_status;
                out->fam_sample_begin_ns = activity.sample_begin_ns;
                out->fam_sample_end_ns = activity.sample_end_ns;
            }
        }
	}
	mutex_unlock(&hsan_dp_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_dp_get_queue);

/* Monotonic, cross-module FAM STOP publication barrier. This does NOT
 * write an undocumented hardware STOP bit and must never authorize a
 * dma_free, unmap, clock disable, or hardware re-arm. The release store
 * makes the state visible to subsequent PFE and PIE readiness checks.
 */
void hsan_lsw_dp_fam_stop_fence(void)
{
    /* Called under the PIE TX/IRQ spinlock, including quarantines.
     * No sleeping mutex or MMIO is permitted here. Atomic release
     * prevents any new PFE/PIE CPU-side handoff; DMA remains HW-owned.
     */
    hsan_lsw_fam_cpu_stop_fence();
    atomic_set(&hsan_dp_fam_stop_fenced, 1);
    smp_mb(); /* CPU publication barrier; HW DMA may still be running. */
}
EXPORT_SYMBOL_GPL(hsan_lsw_dp_fam_stop_fence);

bool hsan_lsw_dp_fam_cpu_fenced(void)
{
    struct hsan_lsw_fam_fence_snapshot fence;

    /* No mutex, no MMIO, no allocator: safe in PIE's TX/IRQ spinlock.
     * Include the CSR fence so an independent owner cannot bypass PIE.
     */
    hsan_lsw_fam_fence_snapshot(&fence);
    return fence.cpu_fenced || atomic_read(&hsan_dp_fam_stop_fenced);
}
EXPORT_SYMBOL_GPL(hsan_lsw_dp_fam_cpu_fenced);

int hsan_lsw_dp_fam_stop_snapshot(struct hsan_dp_fam_stop_snapshot *out)
{
	if (!out)
		return -EINVAL;
	mutex_lock(&hsan_dp_owner_lock);
	hsan_fam_project_stop(&hsan_dp_fam_arm_history, !!hsan_dp_owner,
			      hsan_dp_owner && hsan_dp_owner->quarantined,
			      hsan_lsw_dp_fam_cpu_fenced(), out);
	mutex_unlock(&hsan_dp_owner_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_dp_fam_stop_snapshot);

static int hsan_dp_get_reserved(struct device *dev, const char *compatible,
				struct resource *out)
{
	struct device_node *node, *parent, *reserved;
	bool proper_parent;
	int ret;

	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node)
		return -ENOENT;
	/* Linux scans only enabled direct children of the root reserve node.
	 * This checks DT geometry provenance, not runtime memory ownership.
	 */
	if (!of_device_is_available(node)) {
		dev_err(dev, "%s is disabled; no reserved-memory geometry\n", compatible);
		ret = -EINVAL;
		goto put_node;
	}
	parent = of_get_parent(node);
	reserved = of_find_node_by_path("/reserved-memory");
	proper_parent = reserved && parent == reserved;
	of_node_put(reserved);
	of_node_put(parent);
	if (!proper_parent) {
		dev_err(dev, "%s is not a direct child of /reserved-memory\n",
			compatible);
		ret = -EINVAL;
		goto put_node;
	}
	if (!of_property_read_bool(node, "no-map")) {
		dev_err(dev, "%s lacks required no-map ownership marker\n", compatible);
		ret = -EINVAL;
	} else {
		ret = of_address_to_resource(node, 0, out);
	}
put_node:
	of_node_put(node);
	return ret;
}

static int hsan_dp_probe_reserved(struct platform_device *pdev,
				  struct hsan_dp_state *state)
{
	int qr, fr;

	qr = hsan_dp_get_reserved(&pdev->dev, "hsan,queue_reserved",
				  &state->queue_ram);
	fr = hsan_dp_get_reserved(&pdev->dev, "hsan,flow_reserved",
				  &state->flow_ram);
	if (qr == -ENOENT && fr == -ENOENT) {
		dev_warn(&pdev->dev, "queue/flow reserved-memory absent: FAM memory not qualified\n");
		return 0;
	}
	if (qr || fr) {
		dev_err(&pdev->dev, "incomplete/invalid OEM reserved-memory: queue=%d flow=%d\n",
			qr, fr);
		return qr ? qr : fr;
	}
	/* Exact OEM board region values observed in board-source-observed.dts.
	 * Non-overlap, 4MiB alignment and 32-bit bus addressability are required.
	 */
	if (state->queue_ram.start != 0x90000000ULL ||
	    resource_size(&state->queue_ram) != 0x02400000ULL ||
	    state->flow_ram.start != 0x99000000ULL ||
	    resource_size(&state->flow_ram) != 0x00410000ULL ||
	    !IS_ALIGNED(state->queue_ram.start, SZ_4M) ||
	    !IS_ALIGNED(state->flow_ram.start, SZ_4M) ||
	    state->queue_ram.end > U32_MAX ||
	    state->flow_ram.end > U32_MAX ||
	    !(state->queue_ram.end < state->flow_ram.start ||
	      state->flow_ram.end < state->queue_ram.start)) {
		dev_err(&pdev->dev, "OEM queue/flow memory geometry mismatch; no FAM handoff\n");
		return -EINVAL;
	}
	state->reserved_verified = true;
	dev_info(&pdev->dev, "OEM reserved-memory geometry verified (queue 36MiB, flow 4MiB+64KiB); no memory remap\n");
	return 0;
}

/* hi_queue_get_qid @0x3750 and hi_queue_get_q_num @0x37bc search
 * 30 entries, stride 44: +0 state, +4 port, +8 qid, +12 count.
 * This Linux port uses errno rather than OEM's 0x8fffffee sentinel.
 * The table is an OPTIONAL observed snapshot; hi_queue_init's EQM table
 * per-port queue allocator remains unported; the one bootstrap write
 * is optional and never proves forwarding readiness.
 */
static int hsan_dp_shadow_queue_lookup(const struct hsan_dp_state *state,
				       u32 port, u32 *qid, u32 *count)
{
	if (port >= ARRAY_SIZE(state->port_queues))
		return -EINVAL;
	if (!state->port_queues[port].observed)
		return -ENOENT;
	*qid = state->port_queues[port].qid;
	*count = state->port_queues[port].count;
	return 0;
}

static int hsan_dp_observe_oem_queues(struct platform_device *pdev,
				     struct hsan_dp_state *state)
{
	struct device_node *node = pdev->dev.of_node;
	unsigned int i, j;
	int nr, ret;
	bool hidden_active_port = false;
	u32 bitmap[2];
	u64 port_bitmap;
	u32 qid, count;

	if (!of_find_property(node, "hsan,observed-oem-queue-table", NULL))
		return 0;
	nr = of_property_count_u32_elems(node, "hsan,observed-oem-queue-table");
	if (nr != HSAN_DP_QUEUE_SLOTS * (HSAN_DP_QUEUE_STRIDE / 4))
		return -EINVAL;
	/* hi_queue_init @0x50bc..0x50d4 selects ports from two
	 * 32-bit words of the OEM port_bitmap, before EQM table writes.
	 */
	/* OEM hi_dp_probe @0xcf8 reads one big-endian u64. DT cell0 is
	 * the HIGH word, whereas bitmap[port / 32] needs the low word first.
	 */
	if (of_property_count_u32_elems(node, "port_bitmap") != 2)
		return -EINVAL;
	ret = of_property_read_u64(node, "port_bitmap", &port_bitmap);
	if (ret)
		return ret;
	bitmap[0] = lower_32_bits(port_bitmap);
	bitmap[1] = upper_32_bits(port_bitmap);
	memcpy(state->expected_port_bitmap, bitmap, sizeof(bitmap));
	state->observed_queues = devm_kcalloc(&pdev->dev,
		HSAN_DP_QUEUE_SLOTS, sizeof(*state->observed_queues), GFP_KERNEL);
	if (!state->observed_queues)
		return -ENOMEM;
	ret = of_property_read_u32_array(node, "hsan,observed-oem-queue-table",
		(u32 *)state->observed_queues, nr);
	if (ret)
		return ret;
	for (i = 0; i < HSAN_DP_QUEUE_SLOTS; i++) {
		const struct hsan_dp_queue_slot *entry = &state->observed_queues[i];
		u32 end;

		if (entry->state != 1)
			continue;
		if (!entry->qid || !entry->count || entry->port >= 64 ||
		    !(bitmap[entry->port / 32] & BIT(entry->port % 32)) ||
		    check_add_overflow(entry->qid, entry->count, &end))
			return -EINVAL;
		for (j = 0; j < i; j++) {
			const struct hsan_dp_queue_slot *prev = &state->observed_queues[j];
			u32 prev_end;

			if (prev->state != 1) {
				if (prev->port == entry->port)
					hidden_active_port = true;
				continue;
			}
			if (prev->port == entry->port)
				return -EEXIST;
			if (check_add_overflow(prev->qid, prev->count, &prev_end))
				return -EINVAL;
			/* Shared/overlapping queue IDs are not proven safe on
			 * this board; reject ambiguous owners before handoff.
			 */
			if (entry->qid < prev_end && prev->qid < end)
				return -EADDRINUSE;
		}
		state->port_queues[entry->port].qid = entry->qid;
		state->port_queues[entry->port].count = entry->count;
		state->port_queues[entry->port].observed = true;
		state->active_port_bitmap[entry->port / 32] |= BIT(entry->port % 32);
	}
	/* The OEM getters stop at the first matching port, even if that
	 * row is inactive. Preserve the existing parser's error precedence
	 * above, but do not report an otherwise valid hidden port as live.
	 */
	if (hidden_active_port)
		return -EINVAL;
	ret = hsan_dp_shadow_queue_lookup(state, 10,
					  &qid, &count);
	if (!ret)
		dev_info(&pdev->dev, "observed OEM port10 queue: qid=%u count=%u (shadow ONLY)\n",
			 qid, count);
	else
		dev_warn(&pdev->dev, "observed OEM queue table lacks active port10 (%d); no forwarding ready\n",
			 ret);
	return 0;
}

/* hi_eqm_write_tab @hi_lsw_dp:.text+0x1d04: only the command word
 * encoding and the 5-word data limit are ported at this stage. The real
 * engine requires serialized FIFO writes, command acknowledgment and
 * audited hi_queue_init dependencies. Planning itself never writes MMIO.
 */
#define HSAN_DP_EQM_COMMAND_CSR 0x42000048U
#define HSAN_DP_EQM_MAX_DATA_WORDS HSAN_LSW_EQM_DATA_WORDS
struct hsan_dp_eqm_write_plan {
	u32 command_csr;
	u32 command_word;
	u32 data[HSAN_DP_EQM_MAX_DATA_WORDS];
	u32 data_words;
};

static int hsan_dp_plan_eqm_write(struct hsan_dp_eqm_write_plan *plan,
				  u32 table, u32 entry, const u32 *data,
				  unsigned int nr_words)
{
	unsigned int i;

	if (!plan || !data || !nr_words || table > 127 || entry > U16_MAX)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	plan->command_csr = HSAN_DP_EQM_COMMAND_CSR;
	/* OEM packs 0xc000 | ((table & 0x7f) << 4) as upper 16 bits. */
	plan->command_word = 0xc0000000U | (table << 20) | entry;
	plan->data_words = min(nr_words, HSAN_DP_EQM_MAX_DATA_WORDS);
	for (i = 0; i < plan->data_words; i++)
		plan->data[i] = data[i];
	return 0;
}

/* OEM hi_queue_init @.text+0x5038..0x5064 does:
 * memset_s(32 bytes, 0xff), halfword at offset 4 = 1000, then
 * hi_eqm_write_tab(19, 0, buffer, 8). This constructs the FIRST
 * unexecuted command only. All port queue writes and polling remain gated.
 */
static int hsan_dp_plan_eqm_bootstrap(struct hsan_dp_eqm_write_plan *plan)
{
	u32 data[8];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(data); i++)
		data[i] = U32_MAX;
	data[1] = (data[1] & 0xffff0000U) | 1000U;
	return hsan_dp_plan_eqm_write(plan, 19, 0, data, ARRAY_SIZE(data));
}

/* OEM hi_queue_init @0x523c..0x5270: for each active queue entry
 * and each offset [0,count), initialize 32 bytes to zero, set byte 2 to
 * 7, then hi_eqm_write_tab(table=35,row=qid+offset,data,8). The EQM
 * transfer helper truncates the eight-word input to five words.
 * These are reviewable A32-derived commands, not inferred from DTB size.
 */
#define HSAN_DP_EQM_PER_PORT_TABLE 35U
#define HSAN_DP_EQM_QUEUE_FIRST_WORD 0x00070000U
#define HSAN_DP_EQM_MAX_PROGRAM_ROWS 4096U

static int hsan_dp_plan_per_port_queue(struct hsan_dp_eqm_write_plan *plan,
                                        u32 qid, u32 offset)
{
    u32 image[8] = { HSAN_DP_EQM_QUEUE_FIRST_WORD };
    u32 row;

    if (check_add_overflow(qid, offset, &row) || row > U16_MAX)
        return -ERANGE;
    return hsan_dp_plan_eqm_write(plan, HSAN_DP_EQM_PER_PORT_TABLE,
                                  row, image, ARRAY_SIZE(image));
}

/* Perform *complete* preflight before issuing any irreversible EQM MMIO.
 * Absent an observed 30x44-byte table, never invent a queue allocation.
 */
static int hsan_dp_validate_queue_program(struct hsan_dp_state *state,
                                          u32 *total_rows)
{
    u32 total = 0;
    unsigned int i;

    if (!state->observed_queues || !state->port_queues[10U].observed)
        return -ENODATA;
    /* All bitmap-selected ports must have observed, non-overlapping queue
     * owners before the first irreversible table35 hardware command.
     */
    if (memcmp(state->active_port_bitmap, state->expected_port_bitmap,
               sizeof(state->active_port_bitmap)))
        return -ENODATA;
    for (i = 0; i < HSAN_DP_QUEUE_SLOTS; ++i) {
        const struct hsan_dp_queue_slot *e = &state->observed_queues[i];
        struct hsan_dp_eqm_write_plan last;
        int ret;

        if (e->state != 1)
            continue;
        if (!e->count || e->qid > U16_MAX ||
            check_add_overflow(total, e->count, &total))
            return -ERANGE;
        ret = hsan_dp_plan_per_port_queue(&last, e->qid, e->count - 1);
        if (ret)
            return ret;
    }
    if (!total)
        return -ENODATA;
    /* Bound an explicit hardware opt-in to avoid multi-second busy loops
     * if an untrusted observed snapshot contains implausible queue sizes.
     */
    if (total > HSAN_DP_EQM_MAX_PROGRAM_ROWS)
        return -E2BIG;
    *total_rows = total;
    return 0;
}

/* Only explicit opt-in executes the OEM per-port table35 command stream.
 * CPU+DMA ownership remains incomplete even after every EQM row returns
 * success. No hardware rollback exists for a partially written stream.
 */
static int hsan_dp_program_eqm_per_port(struct platform_device *pdev,
                                        struct hsan_dp_state *state,
                                        bool verify)
{
    struct hsan_dp_eqm_write_plan plan;
    u32 total = 0, status = 0, committed = 0;
    unsigned int i;
    int ret;

    if (!state->eqm_bootstrap_hw_written || !state->clocks_on ||
        !state->fam_ring || !state->reserved_verified ||
        !state->fam_entries_prepared || !state->fam_hw_ack_seen)
        return -EPERM;
    ret = hsan_dp_validate_queue_program(state, &total);
    if (ret)
        return ret;
    state->expected_queue_rows = total;
    state->verified_queue_rows = 0;
    ret = hsan_lsw_eqm_health();
    if (ret)
        return ret;
    for (i = 0; i < HSAN_DP_QUEUE_SLOTS; ++i) {
        const struct hsan_dp_queue_slot *e = &state->observed_queues[i];
        u32 q;

        if (e->state != 1)
            continue;
        for (q = 0; q < e->count; ++q) {
            ret = hsan_dp_plan_per_port_queue(&plan, e->qid, q);
            if (ret)
                goto failed;
            ret = verify ?
                hsan_lsw_eqm_write_verify_table(HSAN_DP_EQM_PER_PORT_TABLE,
                    (u16)(e->qid + q), plan.data, plan.data,
                    plan.data_words, &status) :
                hsan_lsw_eqm_write_table(HSAN_DP_EQM_PER_PORT_TABLE,
                    (u16)(e->qid + q), plan.data,
                    plan.data_words, &status);
            if (ret)
                goto failed;
            ++committed;
            state->committed_queue_rows = committed;
            if (verify)
                state->verified_queue_rows = committed;
            if (!(committed & 63))
                cond_resched();
        }
    }
    dev_warn(&pdev->dev,
             "EQM table35 per-port writes=%u/%u%s; FAM/PFE/PIE NOT ready\n",
             committed, total, verify ? " with readback" : "");
    return 0;
failed:
    /* EQM commands have no generic rollback: retain the known dirty-state
     * result in the device lifetime, and the CSR engine also quarantines
     * failed transactions after a partially sent command.
     */
    state->eqm_hardware_tainted = true;
    dev_err(&pdev->dev,
            "EQM table35 partial stream %u/%u rows, status=%#x, err=%d (HW rollback unavailable; reboot/requalify before retry)\n",
            committed, total, status, ret);
    return ret;
}

/* OEM .data+0x5ac, thirteen records of five u32 words. These auxiliary
 * queues are distinct from the 192 ordinary queues and their priorities.
 */
struct hsan_dp_aux_queue {
	u32 qid, port, hwport, priority, buffer_offset;
};

static const struct hsan_dp_aux_queue hsan_dp_aux_queues[] = {
	{ 0, 8, 48, 1, 1 }, { 1, 9, 49, 1, 1 },
	{ 2, 10, 50, 1, 1 }, { 3, 11, 51, 1, 1 },
	{ 4, 12, 52, 1, 1 }, { 5, 13, 53, 1, 1 },
	{ 8, 33, 56, 1, 1 }, { 9, 33, 56, 2, 1 },
	{ 10, 33, 56, 3, 1 }, { 11, 34, 57, 1, 1 },
	{ 12, 34, 57, 2, 1 }, { 13, 35, 58, 1, 1 },
	{ 15, 0, 63, 1, 1 },
};

static const struct hsan_dp_queue_slot *
hsan_dp_queue_record(struct hsan_dp_state *state, u32 port)
{
	unsigned int i;

	for (i = 0; i < HSAN_DP_QUEUE_SLOTS; i++)
		if (state->observed_queues[i].port == port)
			return &state->observed_queues[i];
	return NULL;
}

/* Called only by the singleton boot probe, before publishing its owner.
 * No runtime EQM mutation API is exposed by this initialization path.
 */
static int hsan_dp_eqm_update(u8 table, u16 row, unsigned int word,
			     u32 mask, u32 value)
{
	u32 data[HSAN_LSW_EQM_DATA_WORDS], status;
	int ret;

	ret = hsan_lsw_eqm_read_table(table, row, data, ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	data[word] = (data[word] & ~mask) | (value & mask);
	return hsan_lsw_eqm_write_verify_table(table, row, data, data,
					      ARRAY_SIZE(data), &status);
}

/* hi_lsw_dp @0x267c: sixteen queue-to-buffer bytes per table16 row. */
static int hsan_dp_eqm_buffer_route(u32 qid, u32 group)
{
	u32 shift = (qid & 3U) * 8U;

	return hsan_dp_eqm_update(16, qid >> 4, (qid & 15U) >> 2,
				  0xbfU << shift, (group | 0x80U) << shift);
}

/* hi_lsw_dp @0x272c: eight three-bit priorities in table32 word1. */
static int hsan_dp_eqm_priority(u32 port, u32 priority, u32 target)
{
	return hsan_dp_eqm_update(32, port, 1, 7U << (priority * 3U),
				  target << (priority * 3U));
}

/* hi_lsw_dp @0x27d8: queue destination, hierarchy flag and priority. */
static int hsan_dp_eqm_destination(u32 qid, u32 port, u32 priority, bool flag)
{
	u32 data[HSAN_LSW_EQM_DATA_WORDS], status;
	u32 mask = 0x0005e07fU;
	u32 value = BIT(18) | (flag ? BIT(13) : 0) | (priority << 14) | port;
	int ret;

	ret = hsan_lsw_eqm_read_table(34, qid, data, ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	if ((data[0] & BIT(18)) && (data[0] & mask) != value)
		return -ESTALE;
	data[0] = (data[0] & ~mask) | value;
	return hsan_lsw_eqm_write_verify_table(34, qid, data, data,
					      ARRAY_SIZE(data), &status);
}

/* hi_lsw_dp @0x2a28: coefficient=2 from .data..read_mostly+0. */
static int hsan_dp_eqm_buffer_group(u32 group, u32 size, u32 maximum)
{
	u32 capacity, status;
	u32 data[HSAN_LSW_EQM_DATA_WORDS] = {
		BIT(31), (size << 20) | maximum, 0, 0, 0,
	};
	int ret;

	ret = hsan_lsw_read(0x42000038U, &capacity);
	if (ret)
		return ret;
	data[2] = (capacity & 0xffffU) < maximum ? maximum : maximum / 2U;
	return hsan_lsw_eqm_write_verify_table(17, group, data, data,
					      ARRAY_SIZE(data), &status);
}

/* hi_lsw_dp @0x3580, rate=0 branch: disabled hash index, byte1=24
 * from .data..read_mostly+4, byte2=1 only for logical ports4..7.
 * A foreign hash owner is not freed or overwritten by this boot owner.
 * The OEM getter @0x31d0..0x3258 determines rate=0 from word0's hash
 * index. Verify its complete configuration word, not unrelated returned
 * state words (word1 can be 0x800 even after the OEM zero-padded write).
 */
static int hsan_dp_eqm_disable_shaping(u32 qid, u32 port)
{
	u32 data[HSAN_LSW_EQM_DATA_WORDS], status;
	u32 expected;
	int ret;

	ret = hsan_lsw_eqm_read_table(38, qid, data, ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	if (data[0] & 0x3fU)
		return -ESTALE;
	memset(data, 0, sizeof(data));
	data[0] = 24U << 8;
	if (port >= 4 && port <= 7)
		data[0] |= BIT(16);
	expected = data[0];
	ret = hsan_lsw_eqm_write_table(38, qid, data, ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	ret = hsan_lsw_eqm_read_table(38, qid, data, ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	return data[0] == expected ? 0 : -EUCLEAN;
}

static int hsan_dp_eqm_auxiliary(const struct hsan_dp_queue_slot *entry,
				const struct hsan_dp_aux_queue *aux)
{
	u32 data[HSAN_LSW_EQM_DATA_WORDS] = { 7U << 16, 0, 0, 0, 0 };
	u32 status, bridge = 256U + entry->oem_private[4];
	int ret;

	ret = hsan_dp_eqm_update(32, aux->hwport, 0, 0xffU, entry->oem_private[5]);
	if (ret)
		return ret;
	ret = hsan_dp_eqm_buffer_route(aux->qid,
				       entry->oem_private[0] + aux->buffer_offset);
	if (ret)
		return ret;
	ret = hsan_lsw_eqm_write_verify_table(35, aux->qid, data, data,
					      ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	ret = hsan_lsw_eqm_write_verify_table(35, bridge, data, data,
					      ARRAY_SIZE(data), &status);
	if (ret)
		return ret;
	ret = hsan_dp_eqm_priority(aux->hwport, 0, aux->buffer_offset);
	if (ret)
		return ret;
	ret = hsan_dp_eqm_destination(bridge, aux->hwport, 0, true);
	if (ret)
		return ret;
	ret = hsan_dp_eqm_priority(aux->hwport, aux->priority, aux->buffer_offset);
	if (ret)
		return ret;
	return hsan_dp_eqm_destination(aux->qid, aux->hwport, aux->priority, true);
}

static int hsan_dp_preflight_queue_routing(struct hsan_dp_state *state)
{
	const struct hsan_dp_queue_slot *parent;
	unsigned int i, q;

	if (!state->observed_queues)
		return -ENODATA;
	for (i = 0; i < HSAN_DP_QUEUE_SLOTS; i++) {
		const struct hsan_dp_queue_slot *entry = &state->observed_queues[i];
		const u32 *p = entry->oem_private;

		if (entry->state != 1 && entry->port != 32)
			continue;
		if (entry->count > 8 || p[1] > 8 || p[4] > 63 || p[5] > 255)
			return -EINVAL;
		if (entry->port == 32 && !entry->count &&
		    (entry->qid || p[4] != 1 || p[5] != 16 || p[6] != 64))
			return -EINVAL;
		if (entry->count && (!p[2] || p[2] > 511 || !p[3] || p[3] > 32767))
			return -EINVAL;
		if (p[6] != 64) {
			parent = hsan_dp_queue_record(state, p[6]);
			if (!parent || parent == entry || p[5] > 7 ||
			    parent->oem_private[4] > 63 || parent->oem_private[5] > 255)
				return -EINVAL;
		}
		for (q = 0; q < entry->count; q++)
			if ((u64)p[0] + (p[1] > 7 ? q : p[1]) > 63)
				return -ERANGE;
		for (q = 0; q < ARRAY_SIZE(hsan_dp_aux_queues); q++)
			if (hsan_dp_aux_queues[q].port == entry->port &&
			    (u64)p[0] + hsan_dp_aux_queues[q].buffer_offset > 63)
				return -ERANGE;
	}
	return 0;
}

static int hsan_dp_program_queue_routing(struct platform_device *pdev,
					struct hsan_dp_state *state)
{
	unsigned int i, j, q, ports = 0, ordinary = 0, auxiliary = 0;
	unsigned int shaping = 0;
	int ret;

	if (!state->fam_hw_ack_seen || state->eqm_hardware_tainted ||
	    state->verified_queue_rows != state->expected_queue_rows)
		return -ENODATA;
	for (i = 0; i < HSAN_DP_QUEUE_SLOTS; i++) {
		const struct hsan_dp_queue_slot *entry = &state->observed_queues[i];
		const struct hsan_dp_queue_slot *parent = NULL;
		const u32 *p = entry->oem_private;
		bool has_aux = false;

		if (entry->state != 1 && entry->port != 32)
			continue;
		if (p[6] != 64)
			parent = hsan_dp_queue_record(state, p[6]);
		if (!parent) {
			for (j = 0; j < ARRAY_SIZE(hsan_dp_aux_queues); j++) {
				if (hsan_dp_aux_queues[j].port != entry->port)
					continue;
				has_aux = true;
				ret = hsan_dp_eqm_auxiliary(entry, &hsan_dp_aux_queues[j]);
				if (ret)
					goto failed;
				auxiliary++;
			}
		}
		ret = hsan_dp_eqm_update(32, p[4], 0, 0xffU,
					parent ? parent->oem_private[5] : p[5]);
		if (ret)
			goto failed;
		if (parent) {
			ret = hsan_dp_eqm_destination(256U + p[4],
						      parent->oem_private[4], p[5], true);
			if (ret)
				goto failed;
		}
		for (q = 0; q < entry->count; q++) {
			u32 qid = entry->qid + q;
			u32 group = p[0] + (p[1] > 7 ? q : p[1]);

			if (!parent) {
				ret = hsan_dp_eqm_priority(p[4], q, 7U - q);
				if (ret)
					goto failed;
			}
			ret = hsan_dp_eqm_buffer_group(group, p[2], p[3]);
			if (ret)
				goto failed;
			ret = hsan_dp_eqm_buffer_route(qid, group);
			if (ret)
				goto failed;
			ret = hsan_dp_eqm_destination(qid, p[4], q, !parent && !has_aux);
			if (ret)
				goto failed;
			if (!parent) {
				ret = hsan_dp_eqm_disable_shaping(qid, entry->port);
				if (ret)
					goto failed;
				shaping++;
			}
			ordinary++;
		}
		ports++;
	}
	state->queue_routing_verified = true;
	dev_info(&pdev->dev,
		 "OEM queue routes verified: ports=%u ordinary=%u auxiliary=%u; PIE/PFE not qualified\n",
		 ports, ordinary, auxiliary);
	dev_info(&pdev->dev, "OEM default disabled shaping verified: queues=%u\n", shaping);
	return 0;
failed:
	state->eqm_hardware_tainted = true;
	dev_err(&pdev->dev, "queue routing partial at ordinary=%u auxiliary=%u: %d\n",
		ordinary, auxiliary, ret);
	return ret;
}

/* Reserve the original queue SHT logical table ID and an independent
 * software backing. No EQM CSR/MMIO is touched here. This is reversible
 * even when the subsequent DP probe fails before PIE/PFE ownership.
 */
static int hsan_dp_prepare_eqm_sht_shadow(struct platform_device *pdev,
					 struct hsan_dp_state *state)
{
	struct hsan_dp_eqm_write_plan bootstrap;
	int ret;

	ret = hsan_dp_plan_eqm_bootstrap(&bootstrap);
	if (ret)
		return ret;
	/* Pure software result, NEVER issue hsan_lsw_write(). */
	dev_dbg(&pdev->dev,
		"EQM first command plan CSR %#08x value %#08x data_words %u (NOT SENT)\n",
		bootstrap.command_csr, bootstrap.command_word,
		bootstrap.data_words);
	ret = hsan_lsw_shadow_table_create(&pdev->dev, HSAN_DP_EQM_SHT_ID,
			"eqm_sht", HSAN_DP_EQM_SHT_ENTRIES,
			HSAN_DP_EQM_SHT_BYTES, 0);
	if (ret)
		return ret;
	state->eqm_sht_shadow_registered = true;
	dev_info(&pdev->dev,
		 "EQM SHT id=0 shadow allocated (63 x 4B); observed queue owners=%u; NO HW writes\n",
		 hweight32(state->active_port_bitmap[0]) +
		 hweight32(state->active_port_bitmap[1]));
	return 0;
}

/* Exact OEM hi_queue_init first write (table 19, row 0, input 8 words)
 * is only allowed after the explicit DP clock/register opt-ins and known
 * LAN1 queue ownership. hi_eqm_write_tab itself transfers FIVE data
 * words, not eight; the stage24 planner already clips the OEM buffer.
 * The EQM engine is shared and serialized in hsan-lsw-csr. Passing here
 * DOES NOT establish full EQM queues or FAM/PIE/PFE readiness.
 * Hardware command bytes have no generic rollback; do not repeat boot
 * blindly or try to undo the command by resetting the board.
 */
/* hi_queue_init @0x4f90..0x5034 precedes the table19 command. The FAM
 * count is read after this boot's FAM ACK, never guessed from pool bytes.
 */
static int hsan_dp_program_eqm_registers(struct platform_device *pdev)
{
	u32 count;
	int ret;

	ret = hsan_lsw_read(0x420000a4U, &count);
	if (ret)
		return ret;
	if ((count & 0xffffU) < 768U)
		return -ERANGE;
	ret = hsan_lsw_update_bits_verify(0x42000018U, BIT(31) | 0xffffU,
					BIT(31) | ((count - 768U) & 0xffffU));
	if (ret)
		return ret;
	ret = hsan_lsw_update_bits_verify(0x42000038U, 0xffffU,
					(count >> 2) & 0x3fffU);
	if (ret)
		return ret;
	ret = hsan_lsw_update_bits_verify(0x42000040U, 0x8a800000U,
					0x8a800000U);
	if (ret)
		return ret;
	dev_info(&pdev->dev, "OEM EQM register prelude verified, FAM count=%#x\n",
		 count);
	return 0;
}

static int hsan_dp_program_eqm_bootstrap(struct platform_device *pdev,
                                         struct hsan_dp_state *state, bool verify)
{
    /* Exact complete five-word READ image, separately from OEM's write
     * padding. Board reference: before our reset ffff,ffff,ffff,0,0;
     * after the unchanged OEM bootstrap ffff,03e8,ffff,0,0.
     * Compare every bit of all five words; never ignore a mismatch.
     */
    static const u32 readback[] = { 0xffffU, 1000U, 0xffffU, 0, 0 };
    struct hsan_dp_eqm_write_plan plan;
    u32 status = 0;
    int ret;

    if (hsan_lsw_eqm_health())
        return -EUCLEAN;
    if (!state->clocks_on || !state->reserved_verified || !state->fam_ring ||
        !state->fam_hw_owned || !state->fam_hw_ack_seen ||
        !state->eqm_sht_shadow_registered ||
        !state->port_queues[10].observed)
        return -ENODATA;
    ret = hsan_dp_plan_eqm_bootstrap(&plan);
    if (ret)
        return ret;
    if (plan.command_csr != HSAN_DP_EQM_COMMAND_CSR ||
        plan.command_word != (0xc0000000U | (19U << 20)) ||
        plan.data_words != HSAN_LSW_EQM_DATA_WORDS)
        return -EINVAL;
    ret = hsan_dp_program_eqm_registers(pdev);
    if (ret) {
        state->eqm_hardware_tainted = true;
        return ret;
    }
    ret = verify ?
        hsan_lsw_eqm_write_verify_table(19, 0, plan.data, readback,
                                       plan.data_words, &status) :
        hsan_lsw_eqm_write_table(19, 0, plan.data,
                                  plan.data_words, &status);
    if (ret) {
        dev_err(&pdev->dev,
                "EQM bootstrap write failed: %d status=%#08x; no DMA\n",
                ret, status);
        return ret;
    }
    state->eqm_bootstrap_hw_written = true;
    dev_warn(&pdev->dev,
             "EQM first table19 row0 HW write acknowledged; remaining queues and DMA NOT ready\n");
    return 0;
}

/* Stage29: exact OEM v300 PIE->EPS channel-map register stream. This
 * performs actual bank-0x41 CSR writes only after an independently observed
 * channel map and a verified DP/EQM staging chain. No U-Boot/DTB default
 * can opt in by itself. This is not the missing FAM hardware owner.
 */
static int hsan_dp_program_pie_v300_map(struct platform_device *pdev,
                                      struct hsan_dp_state *state)
{
    struct hsan_pie_map_plan *plan;
    struct device *dev = &pdev->dev;
    u32 gro[2];
    unsigned int i;
    int ret;

    if (!of_property_read_bool(dev->of_node,
                        "hsan,experimental-program-pie-v300-map"))
        return 0;
    if (!state->clocks_on || !state->reserved_verified ||
        !state->fam_entries_prepared || !state->eqm_sht_shadow_registered ||
        !state->eqm_bootstrap_hw_written || !state->committed_queue_rows ||
        state->eqm_hardware_tainted || hsan_lsw_eqm_health())
        return -ENODATA;
    ret = hsan_dp_read_pie_channels(dev->of_node, gro);
    if (ret)
        return ret;
    plan = kzalloc(sizeof(*plan), GFP_KERNEL);
    if (!plan)
        return -ENOMEM;
    /* Preserve the preflight CPU topology across the full CSR stream:
     * a hotplug event cannot silently change the OEM channel assignment.
     */
    cpus_read_lock();
    if (!state->pie_map_expected_cpus ||
        state->pie_map_expected_cpus != num_online_cpus()) {
        ret = -EAGAIN;
        goto out;
    }
    ret = hsan_dp_make_pie_map_plan(pdev, plan, gro[0], gro[1],
                                       state->pie_map_expected_cpus);
    if (ret)
        goto out;
    /* All offsets and the complete map size are checked BEFORE writing
     * any irreversible register. Data/sideband pairs stay in OEM order.
     */
    for (i = 0; i < plan->nr_writes; ++i) {
        ret = hsan_lsw_check_span(plan->writes[i].logical_csr,
                                  sizeof(u32), true);
        if (ret)
            goto out;
    }
    if (atomic_cmpxchg(&hsan_dp_pie_map_attempted, 0, 1)) {
        ret = -EALREADY;
        goto out;
    }
    state->pie_map_hw_uncertain = true;
    for (i = 0; i < plan->nr_writes; ++i) {
        ret = hsan_lsw_write(plan->writes[i].logical_csr,
                             plan->writes[i].value);
        if (ret)
            goto failed;
        state->pie_map_hw_words++;
        /* OEM sideband at offset +0x80 may be W1C/write-only. Reading
         * it back as a normal CSR would invent completion semantics.
         * The data word is checked only when explicitly requested.
         */
        if (!(i & 1) && of_property_read_bool(dev->of_node,
                    "hsan,experimental-verify-pie-map-data")) {
            u32 readback;

            ret = hsan_lsw_read(plan->writes[i].logical_csr, &readback);
            if (ret)
                goto failed;
            if (readback != plan->writes[i].value) {
                ret = -EUCLEAN;
                goto failed;
            }
        }
    }
    state->pie_gro_start = gro[0];
    state->pie_gro_count = gro[1];
    state->pie_map_hw_committed = true;
    state->pie_map_hw_uncertain = false;
    if (of_property_read_bool(dev->of_node,
                              "hsan,experimental-verify-pie-map-data"))
        dev_info(dev, "PIE map data readback verified: slots=%u GRO_start=%u GRO_count=%u\n",
                 plan->nr_slots, gro[0], gro[1]);
    dev_warn(dev, "PIE v300 map %u CSR writes submitted; sideband ACK, FAM HW owner and PFE forwarding are NOT qualified\n",
             state->pie_map_hw_words);
    ret = 0;
    goto out;
failed:
    dev_err(dev, "PIE map partial transaction after %u/%u words, error=%d; no replay until audited hardware reset\n",
            state->pie_map_hw_words, plan->nr_writes, ret);
out:
    cpus_read_unlock();
    kfree(plan);
    return ret;
}

static void hsan_dp_release_eqm_sht_shadow(struct platform_device *pdev,
					    struct hsan_dp_state *state)
{
	int ret;

	if (!state->eqm_sht_shadow_registered)
		return;
	ret = hsan_lsw_shadow_table_delete(&pdev->dev, HSAN_DP_EQM_SHT_ID);
	if (ret)
		dev_warn(&pdev->dev, "EQM SHT shadow delete failed: %d\n", ret);
	else
		state->eqm_sht_shadow_registered = false;
}

static int hsan_dp_prepare_clocks(struct platform_device *pdev,
				  struct hsan_dp_state *state)
{
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				   "hsan,experimental-enable-dp-clocks"))
		return 0;
	ret = devm_clk_bulk_get_all(&pdev->dev, &state->clks);
	if (ret <= 0)
		return ret ? ret : -ENODEV;
	state->num_clks = ret;
	ret = clk_bulk_prepare_enable(state->num_clks, state->clks);
	if (ret)
		return ret;
	state->clocks_on = true;
	dev_info(&pdev->dev, "DP clock prepare/enable success: %d clock(s), resets untouched\n",
		 state->num_clks);
	return 0;
}

static void hsan_dp_release_clocks(struct hsan_dp_state *state)
{
    /* A partly programmed FAM can still issue DMA: never gate its clock
     * without an audited FAM stop and outstanding-descriptor drain ACK.
     */
    if (hsan_fam_retain_device_resources(state->fam_hw_attempted,
                                         state->fam_hw_owned) ||
        atomic_read(&hsan_dp_lifetime_pinned) ||
        atomic_read(&hsan_dp_global_attempted) ||
        atomic_read(&hsan_dp_registers_attempted) ||
        atomic_read(&hsan_dp_pie_map_attempted) ||
        state->eqm_bootstrap_hw_written ||
        state->eqm_hardware_tainted ||
        state->pie_map_hw_uncertain || state->pie_map_hw_committed) {
        /* DP global/queue/PIE may still depend on this clock even when
         * the FAM DMA start was never reached. No claimed quiescence.
         */
        return;
    }
    if (state->clocks_on) {
		clk_bulk_disable_unprepare(state->num_clks, state->clks);
		state->clocks_on = false;
	}
}

/* Verify the complete DMA-visible software image, including the unused
 * 128KiB tail. This does not confirm DMA cache-coherence or device reads.
 * It ensures no partially populated/foreign physical address is exported
 * to later EQM/PIE stages after an interrupted or invalid DT contract.
 */
static int hsan_dp_verify_fam_cpu_table(struct hsan_dp_state *state,
                                        u32 base, u32 stride, u32 count)
{
    const u32 *rows = state->fam_ring;
    u32 i;

    if (!rows || count > HSAN_DP_FAM_MAX_ENTRIES ||
        !IS_ALIGNED(count, HSAN_DP_FAM_ROW_ALIGNMENT))
        return -EINVAL;
    for (i = 0; i < count; i++) {
        u32 expected = base + (u32)((u64)i * stride);

        if (READ_ONCE(rows[i]) != expected)
            return -EUCLEAN;
    }
    for (; i < HSAN_DP_FAM_RING_BYTES / sizeof(*rows); i++)
        if (READ_ONCE(rows[i]))
            return -EUCLEAN;
    return 0;
}

/* Original hi_fam_init_buf @0x21a0..0x2298 determines the FAM row
 * geometry from hi_system_mem_info_get outputs and four OEM runtime words.
 * ARM32 semantics (before the optional OEM property-dependent adjustment):
 *     consumed = reserved_units * unit_bytes;
 *     rows = min((available_bytes - consumed) / row_stride, 131072) & ~31;
 *     first_row = memory_base + consumed.
 *
 * These runtime values CANNOT be inferred from the no-map pool's size or
 * the DMA allocation. An optional six-cell observed contract supplies:
 *   <memory_base available_bytes minimum_available_bytes
 *    reserved_units unit_bytes row_stride>
 * under hsan,observed-oem-fam-runtime-geometry. All six values must be
 * independently observed from the OEM runtime AFTER any 0x21fc..0x2238
 * property-conditioned adjustment. This function cross-checks the already
 * observed <first_row stride rows> address table before it is filled.
 * It does not program a CSR, arm FAM, or transfer ownership to the device.
 */
static int hsan_dp_verify_observed_fam_runtime(struct platform_device *pdev,
                                                struct hsan_dp_state *state,
                                                const u32 address[3])
{
    u32 runtime[6];
    u64 consumed, available, first, last, computed_rows;
    int ret;

    if (!of_find_property(pdev->dev.of_node,
            "hsan,observed-oem-fam-runtime-geometry", NULL))
        return 0; /* Not verified; never manufacture OEM mem-info */
    if (of_property_count_u32_elems(pdev->dev.of_node,
            "hsan,observed-oem-fam-runtime-geometry") != 6)
        return -EINVAL;
    ret = of_property_read_u32_array(pdev->dev.of_node,
            "hsan,observed-oem-fam-runtime-geometry", runtime, 6);
    if (ret)
        return ret;
    /* OEM division @0x2278; zero stride, or an impossible memory
     * geometry, must fail BEFORE any row is published to coherent DMA.
     */
    if (!runtime[3] || !runtime[4] || !runtime[5] ||
        !IS_ALIGNED(runtime[0], 4) || !IS_ALIGNED(runtime[5], 4) ||
        runtime[1] < runtime[2])
        return -ERANGE;
    if (check_mul_overflow((u64)runtime[3], (u64)runtime[4],
                           &consumed) || consumed >= runtime[1])
        return -ERANGE;
    available = (u64)runtime[1] - consumed;
    computed_rows = min_t(u64, available / runtime[5],
                          HSAN_DP_FAM_MAX_ENTRIES);
    computed_rows &= ~(u64)(HSAN_DP_FAM_ROW_ALIGNMENT - 1);
    first = (u64)runtime[0] + consumed;
    last = first + computed_rows * runtime[5];
    if (!computed_rows || first > U32_MAX ||
        last > (u64)U32_MAX + 1 ||
        (u64)runtime[0] + runtime[1] > (u64)U32_MAX + 1 ||
        first != address[0] || runtime[5] != address[1] ||
        computed_rows != address[2]) {
        dev_err(&pdev->dev,
                "OEM FAM runtime mismatch: derived first=%#llx stride=%u rows=%llu, observed first=%#x stride=%u rows=%u\n",
                (unsigned long long)first, runtime[5],
                (unsigned long long)computed_rows,
                address[0], address[1], address[2]);
        return -EINVAL;
    }
    state->fam_oem_memory_base = runtime[0];
    state->fam_oem_unit_bytes = runtime[4];
    state->fam_oem_stride = runtime[5];
    state->fam_oem_derived_entries = (u32)computed_rows;
    state->fam_oem_runtime_geometry_verified = true;
    return 0;
}

/* OEM hi_fam_init_buf @0x23d4..0x23f8 fills a DMA-owned u32 address
 * table: entry[i] = base + stride*i. The original base/count/stride come
 * from hi_system_mem_info_get and must not be guessed from an address range.
 * Build this software/coherent image only when a *separately observed*
 * 3-cell (base,stride,count) FDT contract is provided. No FAM MMIO here.
 */
static int hsan_dp_prepare_fam_addresses(struct platform_device *pdev,
                                          struct hsan_dp_state *state)
{
    u32 observed[3];
    u64 buffer_end;
    u32 *entries;
    struct resource *pool;
    unsigned int i;
    int ret;

    if (!of_find_property(pdev->dev.of_node,
                          "hsan,observed-fam-address-table", NULL))
        return 0;
    if (!state->fam_ring || !state->reserved_verified)
        return -ENODATA;
    /* A partial read of an overlong DT array would silently ignore
     * extra hardware addresses; the original observed contract is 3 cells.
     */
    if (of_property_count_u32_elems(pdev->dev.of_node,
                     "hsan,observed-fam-address-table") != 3)
        return -EINVAL;
    ret = of_property_read_u32_array(pdev->dev.of_node,
                         "hsan,observed-fam-address-table", observed, 3);
    if (ret)
        return ret;
    if (!observed[1] || !observed[2] ||
        !IS_ALIGNED(observed[0], 4) || !IS_ALIGNED(observed[1], 4) ||
        observed[2] > HSAN_DP_FAM_MAX_ENTRIES ||
        !IS_ALIGNED(observed[2], HSAN_DP_FAM_ROW_ALIGNMENT))
        return -ERANGE;
    /* Each entry names an entire stride-sized packet buffer. Checking
     * only its first four bytes can accept a last buffer past no-map RAM.
     */
    buffer_end = (u64)observed[0] +
                 (u64)observed[2] * observed[1];
    if (buffer_end > (u64)U32_MAX + 1)
        return -ERANGE;
    /* Original no-map RAM must be the *entire* backing of this image;
     * never advertise random normal kernel pages to the OEM datapath.
     */
    pool = NULL;
    if (observed[0] >= state->queue_ram.start &&
        buffer_end - 1 <= state->queue_ram.end)
        pool = &state->queue_ram;
    if (observed[0] >= state->flow_ram.start &&
        buffer_end - 1 <= state->flow_ram.end)
        pool = &state->flow_ram;
    if (!pool)
        return -ERANGE;
    ret = hsan_dp_verify_observed_fam_runtime(pdev, state, observed);
    if (ret)
        return ret;
    entries = state->fam_ring;
    /* The DMA allocation need not be zero-filled on every architecture.
     * Never leave unobserved slots as arbitrary device-visible addresses.
     */
    memset(state->fam_ring, 0, HSAN_DP_FAM_RING_BYTES);
    for (i = 0; i < observed[2]; ++i)
        entries[i] = (u32)((u64)observed[0] + (u64)i * observed[1]);
    ret = hsan_dp_verify_fam_cpu_table(state, observed[0], observed[1],
                                       observed[2]);
    if (ret)
        return ret;
    dma_wmb();
    state->fam_cpu_table_verified = true;
    state->fam_entries_prepared = observed[2];
    dev_info(&pdev->dev,
             "FAM DMA staging filled %u observed addresses in no-map pool %pa; HW NOT armed\n",
             observed[2], &pool->start);
    return 0;
}

/* The original hi_fam_init_buf @0x2364..0x2378 uses five arguments:
 * r0=device, r1=655360, r2=&dma_handle, r3=0xCC0 GFP flags,
 * and the fifth argument stored at [sp]=0 (DMA attributes). Thus the
 * original DMA attributes are EXACTLY zero; 0xCC0 is NOT an attrs mask.
 * GFP_KERNEL is the correct symbolic Linux7 equivalent of allocation
 * semantics; do not transplant the old kernel's numeric __GFP bitmask.
 * This is still a CPU-owned coherent STAGING image, never FAM hardware
 * ownership or proof that ACP/translation has been qualified.
 */
static int hsan_dp_prepare_fam_shadow(struct platform_device *pdev,
				     struct hsan_dp_state *state)
{
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				   "hsan,experimental-prepare-fam-dma-shadow"))
		return 0;
	if (!state->reserved_verified)
		return -ENODATA;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	state->fam_ring = dma_alloc_attrs(&pdev->dev, HSAN_DP_FAM_RING_BYTES,
					 &state->fam_dma, GFP_KERNEL,
                                         HSAN_DP_FAM_DMA_ATTRS);
	if (!state->fam_ring)
		return -ENOMEM;
    /* A base that fits in u32 is insufficient: the LAST byte of the
     * 640KiB coherent allocation must also fit the 32-bit FAM bus.
     * dma_set_mask_and_coherent() should already enforce this, but the
     * explicit bound protects future DMA backends or layout changes.
     */
	if ((u64)state->fam_dma > U32_MAX - (HSAN_DP_FAM_RING_BYTES - 1U)) {
		dma_free_attrs(&pdev->dev, HSAN_DP_FAM_RING_BYTES,
			       state->fam_ring, state->fam_dma,
                               HSAN_DP_FAM_DMA_ATTRS);
		state->fam_ring = NULL;
		return -ERANGE;
	}
    state->fam_dma_range_32bit = true;
    state->fam_dma_attrs_zero = true;
	ret = hsan_dp_prepare_fam_addresses(pdev, state);
	if (ret) {
		dev_err(&pdev->dev, "observed FAM DMA address table invalid: %d\n", ret);
		return ret;
	}
	dev_info(&pdev->dev,
		 "FAM staging allocation 640KiB, prepared=%u addresses (NOT armed, no MMIO)\n",
		 state->fam_entries_prepared);
	return 0;
}

static void hsan_dp_release_fam_shadow(struct platform_device *pdev,
				       struct hsan_dp_state *state)
{
	if (state->fam_ring) {
        if (WARN_ON_ONCE(hsan_fam_retain_device_resources(
                             state->fam_hw_attempted, state->fam_hw_owned))) {
            dev_err(&pdev->dev, "FAM may still own DMA; retain allocation until proven HW reset\n");
            return;
        }
		dma_free_attrs(&pdev->dev, HSAN_DP_FAM_RING_BYTES,
			       state->fam_ring, state->fam_dma,
                               HSAN_DP_FAM_DMA_ATTRS);
		state->fam_ring = NULL;
		state->fam_entries_prepared = 0;
        state->fam_dma_range_32bit = false;
        state->fam_cpu_table_verified = false;
        state->fam_dma_attrs_zero = false;
        state->fam_oem_runtime_geometry_verified = false;
        state->fam_oem_derived_entries = 0;
        state->fam_oem_memory_base = 0;
        state->fam_oem_unit_bytes = 0;
        state->fam_oem_stride = 0;
	}
}

/* Stage38: conservative subset of the OEM hi_fam_init_buf instruction
 * sequence (.text+0x2164, disasm/hi_lsw_dp.objdump). The OEM accesses
 * FAM 0x43000504/514/50c/530/534/510 and EQM 0x42000388/000a4,
 * then waits via 0x43000000. Stage40 implements a separate explicitly
 * gated OEM FAM arm sequence below, and requires independently observed
 * runtime memory geometry; there is still no audited STOP/IRQ drain ACK.
 * This helper remains just an upfront MMIO bounds and ownership check.
 */
static int hsan_dp_preflight_known_fam_registers(struct hsan_dp_state *state)
{
    static const u32 write_csrs[] = {
        0x43000504U, 0x43000514U, 0x4300050cU,
        0x43000530U, 0x43000534U, 0x43000510U,
        0x42000388U, 0x420000a4U,
        0x40000028U, 0x4000001cU,
        0x43000098U, 0x430000a4U,
    };
    unsigned int i;
    int ret;

    if (!state->reserved_verified || !state->fam_ring ||
        !state->fam_dma_range_32bit || !state->fam_dma_attrs_zero ||
        !state->fam_cpu_table_verified || !state->fam_entries_prepared ||
        !state->fam_oem_runtime_geometry_verified ||
        state->fam_hw_owned)
        return -ENODATA;
    for (i = 0; i < ARRAY_SIZE(write_csrs); ++i) {
        ret = hsan_lsw_check_span(write_csrs[i], sizeof(u32), true);
        if (ret)
            return ret;
    }
    return hsan_lsw_check_span(0x43000000U, sizeof(u32), false);
}

/* OEM hi_fam_init_buf @0x21a0..0x2570: guarded hardware submission.
 * Runtime memory geometry is required (never guessed from DT bank sizes).
 * The 0x43000514 value is the dma_alloc_attrs output: the OEM reuses
 * stack slot [sp+0x1c] first for the optional FDT word and then for
 * the DMA bus address. This was resolved from the 0x2378 relocation and
 * 0x2398/0x2408 load/use chain. At @0x2490, hi_lsw_wait_set waits
 * until 0x43000000 bit0 is SET (confirmed against hi_lsw_pub @0x184).
 *
 * The only supported path is an explicit experimental FDT property and
 * the CSR module's default-off write gate. Completion only verifies a
 * narrow FAM init ACK, NOT a DMA stop handshake or complete forwarding.
 */
static const u32 hsan_dp_fam_mode_strides[] = { 0x800, 0xa00, 0x1000, 0x2800 };
static const u32 hsan_dp_fam_write_csrs[] = {
    0x40000028U, 0x4000001cU, 0x43000504U,
    0x42000388U, 0x420000a4U, 0x40000200U,
    0x43000514U, 0x4300050cU, 0x43000530U,
    0x43000534U, 0x43000510U, 0x40000044U,
    0x43000098U, 0x430000a4U,
};

static int hsan_dp_fam_hw_mode(const struct hsan_dp_state *state,
                               u32 *mode)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(hsan_dp_fam_mode_strides); ++i)
        if (state->fam_oem_stride == hsan_dp_fam_mode_strides[i]) {
            *mode = i;
            return 0;
        }
    return -EINVAL; /* original only writes 0x40000200 on a match */
}

/* OEM hi_dp_probe @0xdd0..0xdf0 initial-boot reset sequence only.
 * This is never a runtime DMA STOP/owner-return implementation. All new
 * allocations remain CPU-owned, and the board must reboot before replay.
 */
static int hsan_dp_boot_reset(struct platform_device *pdev,
			      struct hsan_dp_state *state)
{
	static const char * const names[] = {
		"pfe_logic", "pfe_srst", "dp_logic", "dp_srst",
	};
	struct reset_control_bulk_data *resets = state->resets;
	u32 fam_before, fam_after;
	u32 eqm_oem[HSAN_LSW_EQM_DATA_WORDS], eqm_status;
	unsigned int i;
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				   "hsan,experimental-reset-dp-at-boot"))
		return 0;
	if (!state->clocks_on || state->fam_hw_owned ||
	    state->fam_hw_attempted || hsan_lsw_pipeline_operational())
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(names); i++)
		resets[i].id = names[i];
	ret = devm_reset_control_bulk_get_exclusive(&pdev->dev,
					  ARRAY_SIZE(names), resets);
	if (ret)
		return ret;
	ret = hsan_lsw_read(0x43000000U, &fam_before);
	if (ret)
		return ret;
	if (of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-program-eqm-bootstrap")) {
		/* Preserve the firmware's table image before our cold reset.
		 * This read command does not write a queue/table entry and is
		 * evidence only, never an automatic readback-mask exemption.
		 */
		ret = hsan_lsw_eqm_read_table(19, 0, eqm_oem,
					    ARRAY_SIZE(eqm_oem), &eqm_status);
		if (ret)
			return ret;
		dev_info(&pdev->dev,
			 "OEM pre-reset EQM table19 words=%08x,%08x,%08x,%08x,%08x\n",
			 eqm_oem[0], eqm_oem[1], eqm_oem[2], eqm_oem[3], eqm_oem[4]);
	}
	if (of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-program-queue-routing")) {
		static const u16 reference[][2] = {
			{ 16, 5 }, { 17, 1 }, { 32, 10 }, { 34, 81 }, { 38, 81 },
		};

		for (i = 0; i < ARRAY_SIZE(reference); i++) {
			ret = hsan_lsw_eqm_read_table(reference[i][0], reference[i][1],
						    eqm_oem, ARRAY_SIZE(eqm_oem), &eqm_status);
			if (ret)
				return ret;
			dev_info(&pdev->dev,
				 "OEM pre-reset EQM table%u row%u=%08x,%08x,%08x,%08x,%08x\n",
				 reference[i][0], reference[i][1], eqm_oem[0], eqm_oem[1],
				 eqm_oem[2], eqm_oem[3], eqm_oem[4]);
		}
	}
	if (atomic_cmpxchg(&hsan_dp_boot_reset_attempted, 0, 1))
		return -EALREADY;
	state->mmio_attempted = true;
	hsan_dp_pin_hw_lifetime();
	/* OEM reset_control_array_deassert walks DT order. The bulk API
	 * releases in reverse order, which is different for these four lines.
	 */
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = reset_control_deassert(resets[i].rstc);
		if (ret)
			return ret;
	}
	ret = reset_control_bulk_assert(ARRAY_SIZE(names), resets);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = reset_control_status(resets[i].rstc);
		if (ret != 1)
			return ret < 0 ? ret : -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = reset_control_deassert(resets[i].rstc);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = reset_control_status(resets[i].rstc);
		if (ret)
			return ret < 0 ? ret : -EIO;
	}
	ret = hsan_lsw_read(0x43000000U, &fam_after);
	if (ret)
		return ret;
	dev_info(&pdev->dev, "FAM init status across boot reset: %#x -> %#x\n",
		 fam_before, fam_after);
	if (fam_after & BIT(0))
		return -EBUSY;
	state->boot_reset_completed = true;
	dev_info(&pdev->dev,
		 "OEM boot reset: four lines asserted/released with status readback; not DMA STOP ACK\n");
	return 0;
}

static int hsan_dp_preflight_fam_plan(struct hsan_dp_state *state)
{
    unsigned int i;
    u32 mode;
    int ret;

    if (!state->clocks_on || !state->reserved_verified ||
        !state->fam_ring || !state->fam_dma_range_32bit ||
        !state->fam_cpu_table_verified || !state->fam_dma_attrs_zero ||
        !state->fam_oem_runtime_geometry_verified ||
        state->fam_entries_prepared < 64 ||
        state->fam_entries_prepared != state->fam_oem_derived_entries ||
        state->fam_hw_owned || state->fam_hw_attempted)
        return -ENODATA;
    ret = hsan_dp_fam_hw_mode(state, &mode);
    if (ret)
        return ret;
    /* Preflight all FAM, EQM and DP bank windows before the first write.
     * check_span(true) also checks the independent CSR module write gate.
     */
    for (i = 0; i < ARRAY_SIZE(hsan_dp_fam_write_csrs); ++i) {
        ret = hsan_lsw_check_span(hsan_dp_fam_write_csrs[i], 4, true);
        if (ret)
            return ret;
    }
    ret = hsan_lsw_check_span(0x43000000U, 4, false);
    if (ret)
        return ret;
    return 0;
}

static int hsan_dp_preflight_fam_hw(struct hsan_dp_state *state)
{
    int ret = hsan_dp_preflight_fam_plan(state);

    if (ret)
        return ret;
    if (!state->boot_reset_completed)
        return -ENODATA;
    /* OEM wait_set checks bit0 AFTER start. A bit already set BEFORE
     * arm can be stale from firmware or an earlier owner, so it cannot
     * qualify this allocation's ownership. Reject before any DP writes.
     */
    {
        u32 status;

        ret = hsan_lsw_read(0x43000000U, &status);
        if (ret)
            return ret;
        if (status & BIT(0))
            return -EBUSY;
    }
    return 0;
}

static int hsan_dp_program_oem_fam(struct platform_device *pdev,
                                    struct hsan_dp_state *state)
{
    struct hsan_lsw_fam_config cfg;
    u32 mode, base_reg, unit14, polls = 0, status = 0;
    int ret;

    if (atomic_read(&hsan_dp_fam_stop_fenced))
        return -ESHUTDOWN;
    if (!of_property_read_bool(pdev->dev.of_node,
            "hsan,experimental-arm-fam-oem-registers"))
        return 0;
    ret = hsan_dp_preflight_fam_hw(state);
    if (ret)
        return ret;
    ret = hsan_dp_fam_hw_mode(state, &mode);
    if (ret)
        return ret;
    base_reg = state->fam_oem_memory_base + BIT(31);
    unit14 = state->fam_oem_unit_bytes & GENMASK(13, 0);
    cfg = (struct hsan_lsw_fam_config) {
        .memory_base_reg = base_reg,
        .unit14 = unit14,
        .low16 = min_t(u32, state->fam_entries_prepared - 64 + unit14,
                       16320U),
        .mode = mode,
        .dma_bus_addr = (u32)state->fam_dma,
        .entries = state->fam_entries_prepared,
        .count16 = min_t(u32, (u16)state->fam_entries_prepared, 16384U),
    };
    /* A failed or partial CSR transaction cannot prove hardware stopped.
     * Pin THIS module and retain coherent memory and clocks before the
     * call. The CSR owner independently pins itself before its first write.
     */
    mutex_lock(&hsan_dp_owner_lock);
    ret = hsan_fam_init_begin(&hsan_dp_fam_arm_history) ? 0 : -EALREADY;
    mutex_unlock(&hsan_dp_owner_lock);
    if (ret)
        return ret;
    hsan_dp_pin_hw_lifetime();
    state->fam_hw_attempted = true;
    state->fam_hw_owned = true;
    dma_wmb();
    ret = hsan_lsw_fam_commit(&cfg, &polls, &status);
    mutex_lock(&hsan_dp_owner_lock);
    ret = hsan_fam_init_finish(&hsan_dp_fam_arm_history, ret, polls, status);
    mutex_unlock(&hsan_dp_owner_lock);
    if (ret) {
        dev_err(&pdev->dev,
                "exclusive FAM bank40/42/43 transaction failed: %d status=%#x polls=%u, memory retained until audited reset\n",
                ret, status, polls);
        return ret;
    }
    state->fam_hw_ack_seen = true;
    dev_warn(&pdev->dev,
             "exclusive OEM FAM transaction ACK observed after %u polls; DMA STOP/PFE forwarding still unqualified\n",
             polls);
    return 0;
}

/* Stage31: reject all observable request/configuration failures BEFORE any
 * irreversible DP/EQM/PIE CSR write. The later individual checks remain in
 * place because other module owners can disappear or hardware may fail.
 * This does NOT imply the FAM device owns its DMA buffers.
 */
static int hsan_dp_preflight_hw_plan(struct platform_device *pdev,
                                     struct hsan_dp_state *state,
                                     bool globals, bool regs,
                                     bool bootstrap, bool per_port,
                                     bool verify, bool pie_map)
{
    static const u32 register_csrs[] = {
        0x44000468U, 0x4400046cU, 0x44000470U,
        0x44000474U, 0x44000478U, 0x44000400U,
        0x44000428U, 0x44000404U, 0x44000408U,
        0x4400040cU, 0x44000410U,
    };
    static const u32 eqm_register_csrs[] = {
        0x42000018U, 0x42000038U, 0x42000040U,
    };
    struct hsan_pie_map_plan *plan;
    u32 gro[2], total;
    unsigned int i;
    int ret;

    if (!globals && !regs && !bootstrap && !per_port && !pie_map)
        return 0;
    if (!state->clocks_on)
        return -EHOSTDOWN;
    /* Validate every OEM global CSR before the first write. Previously
     * this plan only checked register_init/EQM, so global-stage failures
     * could still leave the device half configured.
     */
    if (globals) {
        ret = hsan_lsw_check_span(0x44000010U, 4, true);
        if (ret)
            return ret;
        for (i = 0; i < ARRAY_SIZE(hsan_dp_global_writes); i++) {
            ret = hsan_lsw_check_span(hsan_dp_global_writes[i].reg, 4, true);
            if (ret)
                return ret;
        }
        ret = hsan_lsw_check_span(0x42000600U, 16U * 4U, true);
        if (ret)
            return ret;
    }
    if (regs) {
        for (i = 0; i < ARRAY_SIZE(register_csrs); i++) {
            ret = hsan_lsw_check_span(register_csrs[i], 4, true);
            if (ret)
                return ret;
        }
    }
    if (bootstrap || per_port || pie_map) {
        /* Fail before ANY irreversible global CSR programming when even
         * the audited FAM prerequisites are missing. This does not set
         * FAM hardware ownership or mark pipeline operational.
         */
        ret = hsan_dp_preflight_known_fam_registers(state);
        if (ret)
            return ret;
        if (!state->reserved_verified || !state->fam_ring ||
            !state->fam_entries_prepared ||
            !state->fam_cpu_table_verified ||
            !state->eqm_sht_shadow_registered ||
            !state->port_queues[10].observed)
            return -ENODATA;
        ret = hsan_lsw_eqm_health();
        if (ret)
            return ret;
        ret = hsan_lsw_check_span(0x42000048U, 4, true);
        if (ret)
            return ret;
        ret = hsan_lsw_check_span(0x4200005cU, 0x18, true);
        if (ret)
            return ret;
        if (verify) {
            ret = hsan_lsw_check_span(0x4200004cU, 0x24, false);
            if (ret)
                return ret;
        }
    }
    if (bootstrap) {
        for (i = 0; i < ARRAY_SIZE(eqm_register_csrs); i++) {
            ret = hsan_lsw_check_span(eqm_register_csrs[i], 4, true);
            if (ret)
                return ret;
        }
    }
    if (per_port) {
        ret = hsan_dp_validate_queue_program(state, &total);
        if (ret)
            return ret;
        state->expected_queue_rows = total;
    }
    if (!pie_map)
        return 0;
    ret = hsan_dp_read_pie_channels(pdev->dev.of_node, gro);
    if (ret)
        return ret;
    plan = kzalloc(sizeof(*plan), GFP_KERNEL);
    if (!plan)
        return -ENOMEM;
    state->pie_map_expected_cpus = num_online_cpus();
    ret = hsan_dp_make_pie_map_plan(pdev, plan, gro[0], gro[1],
                                      state->pie_map_expected_cpus);
    if (!ret) {
        for (i = 0; i < plan->nr_writes; i++) {
            ret = hsan_lsw_check_span(plan->writes[i].logical_csr, 4,
                                      true);
            if (ret)
                break;
        }
    }
    kfree(plan);
    return ret;
}

/* hi_tab_read_l3_cnt @0x50: OEM EPS counter table0x4106a000.
 * No row-to-port relationship has been recovered. A board-provided row
 * selector opts into read-only measurements while the DP clock is owned.
 * The 5632 bound comes from the 0x80000 EPS MMIO window, NOT a proven
 * hardware table capacity. No counter reset/write is performed here.
 */
#define HSAN_DP_EPS_L3_CNT_BASE 0x4106a000U
#define HSAN_DP_EPS_L3_CNT_WINDOW 0x80000U
static int hsan_dp_observe_eps_l3_counter_row(struct platform_device *pdev,
                                               struct hsan_dp_state *state)
{
    u32 row;
    int ret;

    if (!of_find_property(pdev->dev.of_node,
                          "hsan,observed-eps-l3-counter-row", NULL))
        return 0;
    ret = of_property_read_u32(pdev->dev.of_node,
                                "hsan,observed-eps-l3-counter-row", &row);
    if (ret)
        return ret;
    if (row >= (HSAN_DP_EPS_L3_CNT_WINDOW - 0x6a000U) / 16U)
        return -ERANGE;
    ret = hsan_lsw_check_span(HSAN_DP_EPS_L3_CNT_BASE + row * 16U,
                              16U, false);
    if (ret)
        return ret;
    state->eps_l3_counter_row = row;
    state->eps_l3_counter_row_known = true;
    dev_info(&pdev->dev, "OEM EPS L3 counter row %u selected for read-only observation; not a LAN1 port mapping\n",
             row);
    return 0;
}

static int hsan_dp_probe_locked(struct platform_device *pdev)
{
	struct hsan_dp_state *state;
	u32 hwver;
	u32 expected_pie[2], owned_pie[2];
	bool request_globals, request_registers, request_eqm;
    bool request_per_port, verify_eqm, request_pie_map, request_fam_hw;
	bool request_routing;
	int ret;

	if (of_property_read_u32(pdev->dev.of_node, "hwver", &hwver) ||
	    hwver != 0x300)
		return -EINVAL;
	/* Resolve this dependency before enabling clocks or allocating FAM.
	 * A defer must never discard resources already visible to hardware.
	 */
	if (of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-program-pie-v300-map")) {
		if (!of_property_read_bool(pdev->dev.of_node,
					   "hsan,experimental-verify-pie-map-data"))
			return -EINVAL;
		ret = hsan_dp_read_pie_channels(pdev->dev.of_node, expected_pie);
		if (ret)
			return ret;
		ret = hsan_lsw_pie_boot_geometry(&owned_pie[0], &owned_pie[1]);
		if (ret)
			return ret == -ENODEV ? -EPROBE_DEFER : ret;
		if (expected_pie[0] != owned_pie[0] || expected_pie[1] != owned_pie[1])
			return -EINVAL;
	}
	state = devm_kzalloc(&pdev->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	platform_set_drvdata(pdev, state);
	ret = hsan_dp_probe_reserved(pdev, state);
	if (ret)
		return ret;
	ret = hsan_dp_observe_oem_queues(pdev, state);
	if (ret)
		return ret;
	ret = hsan_dp_prepare_clocks(pdev, state);
	if (ret)
		return ret;
	ret = hsan_lsw_map_windows(pdev, &hsan_dp_cfg);
	if (ret)
		goto clocks;
	ret = hsan_dp_observe_eps_l3_counter_row(pdev, state);
	if (ret)
		goto rollback;
	ret = hsan_dp_readonly_pie_plan(pdev);
	if (ret)
		goto rollback;
	ret = hsan_dp_prepare_fam_shadow(pdev, state);
	if (ret)
		goto rollback;
	ret = hsan_dp_prepare_eqm_sht_shadow(pdev, state);
	if (ret)
		goto rollback;
	request_globals = of_property_read_bool(pdev->dev.of_node,
					 "hsan,experimental-program-global-registers");
	request_registers = of_property_read_bool(pdev->dev.of_node,
					 "hsan,experimental-program-dp-register-init");
	request_eqm = of_property_read_bool(pdev->dev.of_node,
					 "hsan,experimental-program-eqm-bootstrap");
	request_per_port = of_property_read_bool(pdev->dev.of_node,
					 "hsan,experimental-program-eqm-per-port");
	verify_eqm = of_property_read_bool(pdev->dev.of_node,
                                         "hsan,experimental-verify-eqm-readback");
    request_pie_map = of_property_read_bool(pdev->dev.of_node,
                                         "hsan,experimental-program-pie-v300-map");
    request_fam_hw = of_property_read_bool(pdev->dev.of_node,
                                         "hsan,experimental-arm-fam-oem-registers");
	request_routing = of_property_read_bool(pdev->dev.of_node,
				"hsan,experimental-program-queue-routing");
	if (request_routing) {
		if (!request_fam_hw || !request_eqm || !request_per_port || !verify_eqm) {
			ret = -EINVAL;
			goto rollback;
		}
		ret = hsan_dp_preflight_queue_routing(state);
		if (ret)
			goto rollback;
	}
    if (request_fam_hw && (!request_globals || !request_registers)) {
        dev_err(&pdev->dev, "FAM hardware arm requires global/register prelude opt-ins\n");
        ret = -EPERM;
        goto rollback;
    }
    if (request_eqm && !request_fam_hw) {
        dev_err(&pdev->dev, "EQM hardware commands require FAM hardware arm/ACK opt-in\n");
        ret = -EPERM;
        goto rollback;
    }
    if (request_pie_map && (!request_per_port || !verify_eqm)) {
        dev_err(&pdev->dev, "PIE map writes require EQM per-port and verified readback opt-ins\n");
        ret = -EPERM;
        goto rollback;
    }
	if ((request_per_port || verify_eqm) && !request_eqm) {
		ret = -EINVAL;
		dev_err(&pdev->dev, "EQM per-port/readback requires bootstrap opt-in\n");
		goto rollback;
	}
	/* Without an enabled/owned clock, even a CSR read can fault or hang. */
	if ((request_globals || request_registers) && !state->clocks_on) {
		ret = -EPERM;
		dev_err(&pdev->dev, "DP writes require explicit prepared clocks\n");
		goto rollback;
	}
	if (request_registers && !request_globals) {
		ret = -EINVAL;
		goto rollback;
	}
	if (request_eqm && (!request_globals || !request_registers ||
			    !state->clocks_on)) {
		dev_err(&pdev->dev,
			"EQM bootstrap requires separately enabled DP clocks/global/register init\n");
		ret = -EPERM;
		goto rollback;
	}
    /* One preflight across ALL requested downstream stages. Previously a
     * malformed table35 or map could fail after global CSR modifications.
     */
    /* FAM geometry, physical bank windows and CSR write gate are fully
     * checked BEFORE global DP CSR writes, not halfway through the arm.
     */
    if (request_fam_hw) {
        ret = hsan_dp_preflight_fam_plan(state);
        if (ret) {
            dev_err(&pdev->dev, "OEM FAM arm preflight failed before global writes: %d\n", ret);
            goto rollback;
        }
    }
    ret = hsan_dp_preflight_hw_plan(pdev, state, request_globals,
            request_registers, request_eqm, request_per_port,
            verify_eqm, request_pie_map);
    if (ret) {
        dev_err(&pdev->dev,
                "DP full hardware plan rejected before first MMIO write: %d\n",
                ret);
        goto rollback;
    }
	ret = hsan_dp_boot_reset(pdev, state);
	if (ret)
		goto rollback;
	if (request_fam_hw) {
		ret = hsan_dp_preflight_fam_hw(state);
		if (ret) {
			dev_err(&pdev->dev,
				"FAM status not clear after boot preparation: %d\n", ret);
			goto rollback;
		}
	}
	if (request_globals) {
        state->init_stage = HSAN_DP_INIT_GLOBALS;
        if (atomic_cmpxchg(&hsan_dp_global_attempted, 0, 1)) {
            ret = -EALREADY;
            dev_err(&pdev->dev, "DP global CSR write already attempted; no unsafe replay\n");
            goto rollback;
        }
        state->mmio_attempted = true;
        hsan_dp_pin_hw_lifetime();
		ret = hsan_dp_verified_global_csrs();
		if (ret) {
			dev_err(&pdev->dev,
				"DP global CSR opt-in failed %d; no datapath handoff\n", ret);
			goto rollback;
		}
		dev_warn(&pdev->dev, "OEM DP global registers written; FAM/queue/PFE still unqualified\n");
	}
	if (request_registers) {
        state->init_stage = HSAN_DP_INIT_REGISTERS;
        if (atomic_cmpxchg(&hsan_dp_registers_attempted, 0, 1)) {
            ret = -EALREADY;
            dev_err(&pdev->dev, "DP register prelude already attempted; no unsafe replay\n");
            goto rollback;
        }
		ret = hsan_dp_verified_register_init();
		if (ret) {
			dev_err(&pdev->dev, "DP 4-command init failed %d; no DMA\n", ret);
			goto rollback;
		}
		dev_warn(&pdev->dev, "DP 4-command register init issued; no FAM/queue/PFE readiness\n");
	}
    if (of_property_read_bool(pdev->dev.of_node,
                              "hsan,experimental-enable-ips-before-fam")) {
        if (!state->boot_reset_completed || !request_fam_hw ||
            !request_routing || !request_pie_map) {
            ret = -EPERM;
            goto rollback;
        }
        ret = hsan_lsw_ips_boot_enable();
        if (ret)
            goto rollback;
        dev_info(&pdev->dev, "OEM IPS boot enable verified before FAM init; PIE RX remains disabled\n");
    }
    if (request_fam_hw) {
        state->init_stage = HSAN_DP_INIT_FAM;
        ret = hsan_dp_program_oem_fam(pdev, state);
        if (ret)
            goto rollback;
    }
	if (request_eqm) {
        state->init_stage = HSAN_DP_INIT_EQM_BOOTSTRAP;
		ret = hsan_dp_program_eqm_bootstrap(pdev, state, verify_eqm);
		if (ret)
			goto rollback;
	}
	if (request_per_port) {
        state->init_stage = HSAN_DP_INIT_EQM_PORTS;
		ret = hsan_dp_program_eqm_per_port(pdev, state, verify_eqm);
		if (ret)
			goto rollback;
	}
    if (request_routing) {
        state->init_stage = HSAN_DP_INIT_QUEUE_ROUTING;
        ret = hsan_dp_program_queue_routing(pdev, state);
        if (ret)
            goto rollback;
    }
    if (request_pie_map) {
        state->init_stage = HSAN_DP_INIT_PIE_MAP;
        ret = hsan_dp_program_pie_v300_map(pdev, state);
        if (ret)
            goto rollback;
    }
    if (of_property_read_bool(pdev->dev.of_node,
                              "hsan,experimental-program-ips-queues")) {
        if (!state->boot_reset_completed || !state->fam_hw_ack_seen ||
            !state->queue_routing_verified || !state->pie_map_hw_committed) {
            ret = -EPERM;
            goto rollback;
        }
        ret = hsan_lsw_ips_queue_init();
        if (ret)
            goto rollback;
        dev_info(&pdev->dev, "OEM IPS queues verified: 8 registers; final enable deferred until PIE rings published\n");
    }
	state->init_stage = HSAN_DP_INIT_PLAN_COMPLETE;
	mutex_lock(&hsan_dp_owner_lock);
    state->eqm_counter_diagnostic = of_property_read_bool(pdev->dev.of_node,
        "hsan,experimental-eqm-queue-counters");
	hsan_dp_owner = state;
	mutex_unlock(&hsan_dp_owner_lock);
    dev_warn(&pdev->dev,
             "DP queue snapshot published: FAM arm attempted=%d ACK=%d; complete PIE/PFE forwarding NOT ready; DMA blocked\n",
             state->fam_hw_attempted, state->fam_hw_ack_seen);
	return 0;
rollback:
    if (state->mmio_attempted ||
        hsan_fam_retain_device_resources(state->fam_hw_attempted,
                                         state->fam_hw_owned)) {
        /* Returning an errno releases all devm state, mapped banks and
         * clock handles. A module reference does not preserve devres.
         * Bind a quarantined owner instead; retain the exact failure and
         * fence new CPU publications without claiming hardware stopped.
         */
        state->init_errno = ret;
        state->quarantined = true;
        hsan_lsw_dp_fam_stop_fence();
        mutex_lock(&hsan_dp_owner_lock);
        hsan_dp_owner = state;
        mutex_unlock(&hsan_dp_owner_lock);
        dev_err(&pdev->dev,
                "DP init stage=%u failed (%d) after MMIO attempt; quarantined binding retains banks, DMA, clocks and diagnostics\n",
                state->init_stage, ret);
        return 0;
    }
	if (state->eqm_bootstrap_hw_written || state->committed_queue_rows ||
	    state->eqm_hardware_tainted || state->pie_map_hw_uncertain ||
        state->pie_map_hw_committed)
		dev_err(&pdev->dev,
			"DP probe failed after an irreversible EQM write; software rollback does not restore hardware queue ownership\n");
	hsan_dp_release_eqm_sht_shadow(pdev, state);
	hsan_dp_release_fam_shadow(pdev, state);
	hsan_lsw_unmap_windows(pdev, &hsan_dp_cfg);
clocks:
	hsan_dp_release_clocks(state);
	return ret;
}

static int hsan_dp_probe(struct platform_device *pdev)
{
	struct hsan_dp_init_snapshot snapshot;
	int ret;

	mutex_lock(&hsan_dp_probe_lock);
	mutex_lock(&hsan_dp_owner_lock);
	ret = hsan_dp_owner ? -EBUSY : 0;
	mutex_unlock(&hsan_dp_owner_lock);
	if (!ret)
		ret = hsan_dp_probe_locked(pdev);
	if (!ret && !hsan_lsw_dp_init_snapshot(&snapshot))
		dev_info(&pdev->dev,
			 "DP boot snapshot: owner=1 stage=%u errno=%d attempted=%u quarantined=%u\n",
			 snapshot.stage, snapshot.last_errno, snapshot.mmio_attempted,
			 snapshot.quarantined);
	mutex_unlock(&hsan_dp_probe_lock);
	return ret;
}

static void hsan_dp_remove_locked(struct platform_device *pdev)
{
	struct hsan_dp_state *state = platform_get_drvdata(pdev);

	/* Normal hardware-owner unload is prevented by the module pin. A
	 * forced platform-device removal still must stop CPU publications and
	 * invalidate the visible owner before driver-core devres cleanup.
	 * No DMA drain or safe hot removal is implied by this fence.
	 */
	if (state->mmio_attempted ||
	    hsan_fam_retain_device_resources(state->fam_hw_attempted,
					     state->fam_hw_owned))
		hsan_lsw_dp_fam_stop_fence();
	mutex_lock(&hsan_dp_owner_lock);
	if (hsan_dp_owner == state)
		hsan_dp_owner = NULL;
	mutex_unlock(&hsan_dp_owner_lock);
    if (state->pie_map_hw_committed || state->pie_map_hw_uncertain) {
        dev_warn(&pdev->dev, "PIE map hardware programming history remains on device; no software undo\n");
    }
	hsan_dp_release_eqm_sht_shadow(pdev, state);
	hsan_lsw_unmap_windows(pdev, &hsan_dp_cfg);
	hsan_dp_release_fam_shadow(pdev, state);
	hsan_dp_release_clocks(state);
}

static void hsan_dp_remove(struct platform_device *pdev)
{
	mutex_lock(&hsan_dp_probe_lock);
	hsan_dp_remove_locked(pdev);
	mutex_unlock(&hsan_dp_probe_lock);
}
/* DP global/EQM/PIE writes are one-shot with no qualified resume.
 * A rejected system suspend is safer than clock-off with unknown FAM state.
 */
static int hsan_dp_pm_suspend(struct device *dev)
{
    (void)dev;
    if (atomic_read(&hsan_dp_fam_stop_fenced) ||
        atomic_read(&hsan_dp_lifetime_pinned) ||
        atomic_read(&hsan_dp_global_attempted) ||
        atomic_read(&hsan_dp_registers_attempted) ||
        atomic_read(&hsan_dp_pie_map_attempted))
        return -EBUSY;
    return 0;
}

static const struct dev_pm_ops hsan_dp_pm_ops = {
    .suspend = hsan_dp_pm_suspend,
    .freeze = hsan_dp_pm_suspend,
    .poweroff = hsan_dp_pm_suspend,
};

static const struct of_device_id hsan_dp_match[] = {
	{ .compatible = "hsan,dp" }, {}
};
MODULE_DEVICE_TABLE(of, hsan_dp_match);
static struct platform_driver hsan_dp_driver = {
	.probe = hsan_dp_probe, .remove = hsan_dp_remove,
    /* Global/EQM/PIE MMIO may be non-reversible. Do not offer a manual
     * sysfs unbind/rebind path that clears per-device state.
     */
    .driver = { .name = "hsan-lsw-dp", .of_match_table = hsan_dp_match,
                .suppress_bind_attrs = true, .pm = &hsan_dp_pm_ops },
};
module_platform_driver(hsan_dp_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V Stage79 DP init ownership and retained failure state");
