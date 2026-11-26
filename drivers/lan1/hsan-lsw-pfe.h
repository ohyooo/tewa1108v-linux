/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_LSW_PFE_H
#define HSAN_LSW_PFE_H
#include <linux/types.h>
#include <linux/bitops.h>

/* Snapshots are evidence of individual operations, not a forwarding-ready
 * decision. Every hardware bit may be false even when all shadow tables exist.
 */
struct hsan_pfe_snapshot {
    bool clock_on;
    bool global_hw_init_attempted;
    bool global_hw_init_completed;
    bool opp_hw_init_attempted;
    bool tfrt_hw_uncertain;
    bool global_attr_shadow;
    u32 tfrt_rows_hw_verified;
    u32 pub_shadows;
    /* The OEM PUB GRO table (id 0x128) is conditional on the observed
     * PIE GRO channel count. 8/9 may be complete when GRO is exactly 0.
     * Absence of a trusted observation is NOT equivalent to zero.
     */
    u32 pub_shadows_expected;
    u32 pub_gro_entries;
    bool pub_gro_count_known;
    /* OEM table 0x102 L3 interface encoder and port10 VLAN visibility.
     * Both are observational only, NEVER forwarding or HW ownership ACK.
     */
    bool l3intf_plan_valid;
    u32 l3intf_plan_row;
    u32 l3intf_plan_crc32;
    /* Paired read-only OEM table observations. A match is a snapshot of
     * two CPU-visible rows, NOT an L2/L3 forwarding installation ACK.
     */
    bool l3intf_hw_readback_valid;
    bool l3intf_hw_matches_plan;
    u32 l3intf_hw_readback_crc32;
    u64 l3intf_hw_read_begin_ns;
    u64 l3intf_hw_read_end_ns;
    bool lan1_vlan_sampled;
    bool lan1_vlan_membership;
    u32 lan1_vlan_id;
    u32 lan1_vlan_raw;
    u32 lan1_vlan_enable_plan;
    u32 lan1_vlan_disable_plan;
    u32 opp_shadows;
    u32 mcc_shadows;
    u32 mcc_rows_written;
    bool mcc_hw_attempted;
    bool mcc_write_stream_submitted;
    bool mcc_hw_uncertain;
    bool mcc_transaction_sealed; /* one-shot CSR owner, NOT forwarding ready */
    u32 ofc_shadows;
    u32 ofc_dynamic_entries;
    bool ofc_csr_attempted;
    bool ofc_csr_verified;
    bool ofc_hw_uncertain;
    bool ifc_shadow_registered;
    bool ifc_hw_attempted;
    bool ifc_hw_uncertain;
    u32 ifc_rows_committed;
    u32 ifc_rows_window_readback; /* not a verified IFC HW table owner */
    u32 ifc_last_status;
    bool ifc_observed_image_verified; /* CRC of 12 OEM DT rows only */
    bool ifc_late_window_sampled; /* all 12 CPU-visible windows re-read */
    u32 ifc_late_window_rows_matched; /* post-commit observational only */
    u32 ifc_late_window_read_errno; /* positive errno for diagnostics */
    bool ifc_late_window_drift_latched; /* irreversible mismatch */
    u32 ifc_observed_image_crc32; /* not a hardware lookup/activation ACK */
    u32 forwarding_blockers; /* software-known stages, never HW PASS */
    bool fam_cpu_stop_fenced; /* CPU-only, NEVER HW DMA stop ACK */
    bool fam_fence_abort_latched; /* a PFE MMIO sequence was aborted */
    u32 fam_fence_abort_step; /* first step stopped, see Stage52 evidence */
    bool oem_diag_requested; /* requires DT opt-in; read-only MMIO */
    u32 oem_diag_valid_groups; /* 1=IFC, 2=L2, 4=L3, 8=PA, 16=NAPT */
#define HSAN_PFE_OEM_DIAG_WORDS 11U
    u32 oem_diag_raw[HSAN_PFE_OEM_DIAG_WORDS]; /* non-latched, not LAN1-specific */
    u64 oem_diag_sample_begin_ns; /* local CLOCK_MONOTONIC interval */
    u64 oem_diag_sample_end_ns; /* never a HW multi-bank latch */
    u32 service_shadows; /* TNL/DSLite/UDE software-only registrations */
    u32 pa_shadows; /* PA 0x129 and 0x116 software-only registrations */
    bool ifc_hash_range_checked; /* observed bounds, NOT HW hash ownership */
    bool ifc_hash_reserved; /* Linux bitmap reservation, NOT HW allocator */
    bool ifc_engine_quarantined; /* shared CSR module pinned after IFC writes */
    u32 ifc_hash_observed_start;
    bool pa_csr_attempted;
    bool pa_csr_verified; /* one CSR only, NOT PA ready */
    bool pa_hw_uncertain;
    u32 napt_shadow_tables; /* 0x105/0x106 are software owners only */
    u32 napt_ipv4_rows; /* board-observed dynamic row count */
    u32 napt_ipv6_rows; /* board-observed dynamic row count */
    bool napt_oem_geometry_verified; /* OEM budget/row-count cross-check */
    bool napt_partition_layout_verified; /* full 4-word OEM base/stride/cost cross-check; NOT HW ACK */
    u32 napt_primary_level; /* OEM first cost loop [0..8] */
    u32 napt_secondary_level; /* OEM second cost loop [0..8] */
    u32 napt_observed_memory_bytes; /* runtime source, not Linux DRAM */
    u32 napt_ipv4_reserved_rows;
    u32 napt_ipv6_reserved_rows;
    u32 napt_ipv4_materialized_rows;
    u32 napt_ipv6_materialized_rows;
    bool napt_fixed_attempted; /* first five OEM NAPT CSR operations */
    bool napt_fixed_submitted; /* not NAPT initialized or routable */
    bool napt_partition_attempted;
    u32 napt_partition_commands; /* 0 or 2, never HW NAT table proof */
    u32 napt_partition_last_status;
    bool napt_hw_uncertain; /* HW layout/ACK not qualified */
};
#define HSAN_PFE_FWD_BLOCK_IFC      BIT(0)
#define HSAN_PFE_FWD_BLOCK_PA       BIT(1)
#define HSAN_PFE_FWD_BLOCK_NAPT     BIT(2)
#define HSAN_PFE_FWD_BLOCK_OFC_MCC  BIT(3)
#define HSAN_PFE_FWD_BLOCK_L2_L3    BIT(4) /* unported physical callbacks */
#define HSAN_PFE_FWD_BLOCK_FAM_STOP BIT(5) /* CPU publication fence only */
int hsan_lsw_pfe_snapshot(struct hsan_pfe_snapshot *out);
/* Stage78 diagnostics use a NEW exported symbol to preserve the exact
 * Stage77 hsan_pfe_snapshot layout for all existing module consumers.
 * The new fields are observational only, not forwarding-ready evidence.
 */
struct hsan_pfe_l3intf_commit_snapshot {
    bool requested;
    bool attempted;
    bool mmio_submitted;
    bool cpu_readback_ok;
    u32 last_errno;
    u32 row;
};
int hsan_lsw_pfe_l3intf_commit_snapshot(
    struct hsan_pfe_l3intf_commit_snapshot *out);
/* Kept separate from older snapshots so mixed module layouts cannot
 * silently change. A retained binding means only resources are preserved;
 * it never means that initialization or forwarding succeeded.
 */
struct hsan_pfe_lifecycle_snapshot {
    bool hardware_attempted;
    bool quarantined;
    bool bindings_retained;
    u32 failed_step; /* 1..10: write stage; 100..111: individual IFC row fence */
    u32 last_errno; /* positive errno of the first failed write stage */
};
int hsan_lsw_pfe_lifecycle_snapshot(struct hsan_pfe_lifecycle_snapshot *out);
#endif
