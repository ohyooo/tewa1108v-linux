/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_LSW_DP_H
#define HSAN_LSW_DP_H
#include <linux/types.h>
#include "hsan-lsw-fam-stop-state.h"

struct hsan_dp_queue_snapshot {
	u32 port;
	u32 qid;
	u32 count;
	bool reserved_memory_verified;
	bool fam_addresses_prepared;
	bool eqm_bootstrap_committed;
	bool eqm_queue_rows_committed;
	bool pie_map_hw_committed;
	bool pie_map_hw_uncertain;
    u32 pie_map_hw_words;
    u32 fam_entry_count; /* coherent software staging, not HW ownership */
    bool fam_dma_range_32bit; /* whole 640KiB allocation fits the bus mask */
    bool fam_cpu_table_verified; /* all rows and unused tail checked in CPU memory */
    bool fam_dma_attrs_zero; /* exact OEM dma_alloc_attrs fifth argument */
    bool fam_hw_owned; /* DMA may be owned after an arm attempt, even without ACK */
    bool fam_oem_runtime_geometry_verified; /* OEM runtime memory arithmetic cross-check */
    u32 fam_oem_derived_entries; /* observed OEM-derived rows, NOT hardware ownership */
    bool fam_hw_attempted; /* irreversible FAM CSR/DMA submission history */
    bool fam_hw_ack_seen; /* OEM 0x43000000.bit0 after programming */
    bool fam_cpu_stop_fenced; /* no further software FAM/PIE submissions */
    u64 fam_rejected_csr_writes; /* CPU fence blocked, not HW stop ACK */
    u64 fam_rejected_pie_publications; /* CPU-only linearized publish */
    u64 fam_admitted_cpu_publications; /* producer publications only */
    u64 fam_admitted_tx_reclaims; /* consumer-validated unmaps before CPU STOP */
    u64 fam_rejected_tx_reclaims; /* STOP blocked a TX mapping unmap */
    u64 fam_stop_fence_invocations; /* not a hardware STOP register write */
    bool fam_hw_stop_ack_seen; /* ALWAYS false until a separately audited STOP ACK */
    bool fam_activity_sampled; /* OEM hi_fam_dump_status, not stop ACK */
    u32 fam_activity_21bit;
    u32 fam_activity_14bit;
    u32 fam_init_status_raw; /* OEM FAM arm ACK (not DMA STOP) */
    u64 fam_sample_begin_ns;
    u64 fam_sample_end_ns; /* not HW synchronized with PIE/PFE */
    bool eps_l3_counter_row_known;
    bool eps_l3_counter_sampled; /* read-only, no known port10 mapping */
    u32 eps_l3_counter_row;
    u32 eps_l3_first_low32;
    u32 eps_l3_second_low32;
    u32 eps_l3_second_high8;
};
/* Copy-only observation contract for PFE. No DMA/forwarding-ready signal. */
int hsan_lsw_dp_get_queue(u32 port, struct hsan_dp_queue_snapshot *out);

/* Separate from queue_snapshot so existing consumers keep its layout.
 * PLAN_COMPLETE describes only the requested init plan, not forwarding.
 */
enum hsan_dp_init_stage {
	HSAN_DP_INIT_CONFIG = 0,
	HSAN_DP_INIT_GLOBALS,
	HSAN_DP_INIT_REGISTERS,
	HSAN_DP_INIT_FAM,
	HSAN_DP_INIT_EQM_BOOTSTRAP,
	HSAN_DP_INIT_EQM_PORTS,
	HSAN_DP_INIT_PIE_MAP,
	HSAN_DP_INIT_PLAN_COMPLETE,
	HSAN_DP_INIT_QUEUE_ROUTING,
};

struct hsan_dp_init_snapshot {
	u32 stage;
	s32 last_errno;
	bool mmio_attempted;
	bool quarantined;
	bool queue_routing_verified;
	bool boot_reset_completed;
	/* Geometry of the complete submitted PIE map stream, not a HW ACK. */
	bool pie_map_geometry_valid;
	u32 pie_gro_start;
	u32 pie_gro_count;
};

/* Software-only: valid even when no observed port queue is available. */
int hsan_lsw_dp_init_snapshot(struct hsan_dp_init_snapshot *out);
/* Independent software-only observation; owner_bound may be false while the
 * monotonic arm history still requires retention. No queue lookup or MMIO.
 */
int hsan_lsw_dp_fam_stop_snapshot(struct hsan_dp_fam_stop_snapshot *out);
/* Monotonic software-only stop fence; NEVER transfers DMA ownership. */
void hsan_lsw_dp_fam_stop_fence(void);
/* IRQ-safe, read-only and monotonic: unlike HW STOP this is ONLY the
 * CPU-side handoff fence. Callers must stop publishing DMA descriptors.
 */
bool hsan_lsw_dp_fam_cpu_fenced(void);
/* OEM hi_queue_port_cnt_get tables 81/80; separate non-atomic samples.
 * A read command is still an MMIO write and remains CPU-fence protected.
 * Counter clear-on-read behavior is unqualified; never an ownership ACK.
 */
#define HSAN_DP_DIAG_QUEUE_COUNT 56
struct hsan_dp_queue_counters {
    u32 words[HSAN_DP_DIAG_QUEUE_COUNT][5]; /* table81[0..3], table80[2] */
    bool valid[HSAN_DP_DIAG_QUEUE_COUNT];
};
int hsan_lsw_dp_queue_counters(struct hsan_dp_queue_counters *out);
#endif
