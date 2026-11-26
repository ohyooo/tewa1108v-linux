// SPDX-License-Identifier: GPL-2.0-only
/* hi_lsw_pfe.ko hi_pfe_probe @.text+0x2cdc: 12 original .rodata banks.
 * The experimental DTB contains an additional 'ptc' resource, but the
 * original 12-entry registered-bank table does not include it.
 */
#include <linux/clk.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/crc32.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "hsan-lsw-platform.h"
#include "hsan-lsw-csr.h"
#include "hsan-lsw-dp.h"
#include "hsan-lsw-pfe.h"
#include "hsan-pfe-l3intf-plan.h"
#include "hsan-pfe-pa-port-plan.h"
#include "hsan-capa-lookup-plan.h"
#include "hsan-pfe-vlan-plan.h"

/* hi_pfe_probe @.text+0x2cdc initialisation sequence:
 *   hi_pfe_init -> pub -> opp -> ofc -> mcc -> napt -> tnl ->
 *   dslite -> ude -> ifc -> pa -> dfx.
 * Only the hi_pfe_init's register prelude is ported. The remaining
 * table-manager and submodule owners stay UNINITIALISED, never "ready".
 */
/* OEM hi_pfe_init @0x2a94 creates table ID 0x10f with cfg[8]=3,
 * cfg[16]=4, cfg[26]=1; hi_pfe_exit deletes the same ID.
 * We reserve only a Linux software shadow. The original callback pointers
 * and physical table-manager/HW entry programming are not ported.
 */
#define HSAN_PFE_GLOBAL_ATTR_TAB 0x10fU
#define HSAN_PFE_PUB_TABLE_COUNT 9U
#define HSAN_PFE_PUB_GRO_ID 0x128U
#define HSAN_PFE_OPP_TABLE_COUNT 2U
/* hi_pfe_opp_init @.text+0x30ac/0x3140 registers table 0x10d and
 * 0x112; the matching hi_pfe_opp_exit deletes both. The ARM32
 * callbacks are NOT portable and these tables remain software only.
 */
#define HSAN_PFE_MAX_OBSERVED_GRO 16U

/* hi_lsw_pfe.ko .data+0xb0: hi_pfe_pub_init @0x4318 iterates nine
 * records of 56 bytes; ID precedes the 52-byte ARM32 config. Each
 * config stores entries at +8 and entry width at +16. For ID 0x128
 * entries are overwritten by hi_pie_get_gro_ch_num() at runtime.
 * The original callback pointers (A32) are not usable on arm64.
 */
struct hsan_pfe_pub_table_desc {
    u16 id;
    u32 entries;
    u32 bytes_per_entry;
    const char *shadow_name;
};
static const struct hsan_pfe_pub_table_desc hsan_pfe_pub_tables[] = {
    {0x101,  399, 12, "pub_0101"},
    {0x102,   31, 20, "pub_0102"},
    {0x103,  255, 12, "pub_0103"},
    {0x110,   32, 12, "pub_0110"},
    {0x113,  255,  8, "pub_0113"},
    {0x11d,  127,  4, "pub_011d"},
    {0x006,   63, 12, "pub_0006"},
    {0x117, 4095,  4, "pub_0117"},
    {0x128,    0,  4, "pub_0128"},
};
static_assert(ARRAY_SIZE(hsan_pfe_pub_tables) == HSAN_PFE_PUB_TABLE_COUNT);
static const struct hsan_pfe_pub_table_desc hsan_pfe_opp_tables[] = {
    {0x10d, 16, 4, "opp_tfrt"},
    {0x112, 255, 8, "opp_wifi_user_ll"},
};
static_assert(ARRAY_SIZE(hsan_pfe_opp_tables) == HSAN_PFE_OPP_TABLE_COUNT);
/* hi_pfe_mcc_init @0x6c1c calls hi_tab_mgr_create(0x10c,0x10a,0x800).
 * Configuration records are at .rodata+0xab4/+0xaec/+0xb24 in the OEM ELF;
 * +8 gives entry count, +16 gives element bytes. No ARM32 callbacks are
 * reused on ARM64. These are independent software lifecycle owners until
 * the audited MCC register sequence has separately been qualified.
 */
#define HSAN_PFE_MCC_TABLE_COUNT 3U
static const struct hsan_pfe_pub_table_desc hsan_pfe_mcc_tables[] = {
    {0x10c, 1023, 8, "mcc_010c"},
    {0x10a,   32, 44, "mcc_010a"},
    {0x800,   63, 12, "mcc_0800"},
};
static_assert(ARRAY_SIZE(hsan_pfe_mcc_tables) == HSAN_PFE_MCC_TABLE_COUNT);
/* OEM hi_pfe_ofc_init @0x5184 registers table-manager IDs 0x108 and
 * 0x104 (in this order); hi_pfe_ofc_exit deletes both. The A32 stack
 * config gives ID 0x108 width=36 with a runtime-adjusted initial limit
 * 256, and ID 0x104 count=191 width=12. The dynamic count is NOT
 * recoverable from static DT alone; require an explicit observed value.
 * These are software-owned descriptors, NOT original HW callbacks.
 */
/* OEM hi_pfe_ifc_init @0x830 allocates a 12-slot hash area from table
 * 0x103 and creates table 0x11b (500 rows, 20 bytes). Its 12 default
 * IFC commands use a separate bank0 MMIO transport, not this shadow.
 */
#define HSAN_PFE_IFC_TABLE_ID 0x11bU
#define HSAN_PFE_IFC_TABLE_ROWS 500U
#define HSAN_PFE_IFC_TABLE_WIDTH 20U
#define HSAN_PFE_IFC_DEFAULT_ROWS 11U
#define HSAN_PFE_IFC_HASH_RESERVED_ROWS 1U
#define HSAN_PFE_IFC_CONTROL_WORDS 7U
#define HSAN_PFE_IFC_DATA_WORDS 8U
#define HSAN_PFE_IFC_OBSERVED_WORDS (HSAN_PFE_IFC_CONTROL_WORDS + HSAN_PFE_IFC_DATA_WORDS)
#define HSAN_PFE_OFC_TABLE_COUNT 2U
#define HSAN_PFE_OFC_CONTROL_CSR 0x04000030U
#define HSAN_PFE_OFC_CONTROL_MASK 0x000007ffU
#define HSAN_PFE_OFC_CONTROL_VALUE 0x00000124U
static const struct hsan_pfe_pub_table_desc hsan_pfe_ofc_tables[] = {
    {0x108, 0,   36, "ofc_direct"},
    {0x104, 191, 12, "ofc_qos"},
};
static_assert(ARRAY_SIZE(hsan_pfe_ofc_tables) == HSAN_PFE_OFC_TABLE_COUNT);
/* Further OEM PFE lifetime owners recovered from hi_lsw_pfe.ko:
 * hi_pfe_tnl_init @0x81ac: id=0x114, .rodata+0xc48 cfg[8]=14,
 * cfg[16]=128; hi_pfe_dslite_init @0x8770: id=0x119 64x64B;
 * hi_pfe_ude_init @0x81d0: id=0x11a 15x4B.
 * hi_pfe_pa_init @0x75bc jumps into code at 0x6d40, registering
 * id=0x129 (3x44B), writing CSR 0x07000704=1, then registering
 * id=0x116 (16x4B); hi_pfe_pa_exit deletes 0x116 then 0x129.
 * These are software table descriptors, not the OEM hardware callbacks.
 */
#define HSAN_PFE_SERVICE_TABLE_COUNT 3U
static const struct hsan_pfe_pub_table_desc hsan_pfe_service_tables[] = {
    {0x114, 14, 128, "tnl_0114"},
    {0x119, 64, 64, "dslite_0119"},
    {0x11a, 15, 4, "ude_011a"},
};
#define HSAN_PFE_PA_TABLE_COUNT 2U
static const struct hsan_pfe_pub_table_desc hsan_pfe_pa_tables[] = {
    {0x129, 3, 44, "pa_0129"},
    {HSAN_PFE_PA_PORT_TABLE_ID, HSAN_PFE_PA_PORT_ROWS,
     HSAN_PFE_PA_PORT_PAYLOAD_BYTES, "pa_0116"},
};
/* hi_pfe_napt_init @0x62f8 calls the shared setup at 0x5354 and
 * hi_pfe_napt_exit @0x66e0 deletes 0x105/0x106. The OEM table-manager
 * configs use 40-byte IPv4 entries and 64-byte IPv6 entries; table counts
 * depend on hi_system_mem_info_get(2), NOT a fixed board-independent
 * constant. Only exact observed per-board counts may create the shadows.
 */
#define HSAN_PFE_NAPT_TABLE_COUNT 2U
static const struct hsan_pfe_pub_table_desc hsan_pfe_napt_tables[] = {
    {0x105, 0, 40, "napt_ipv4"},
    {0x106, 0, 64, "napt_ipv6"},
};
static_assert(ARRAY_SIZE(hsan_pfe_napt_tables) == HSAN_PFE_NAPT_TABLE_COUNT);
#define HSAN_PFE_NAPT_CONFIG_CSR 0x03000070U
#define HSAN_PFE_NAPT_CONFIG_MASK 0x43331f3fU
#define HSAN_PFE_NAPT_CONFIG_VALUE 0x40111d38U
#define HSAN_PFE_PA_ENABLE_CSR 0x07000704U
static_assert(ARRAY_SIZE(hsan_pfe_service_tables) == HSAN_PFE_SERVICE_TABLE_COUNT);
static_assert(ARRAY_SIZE(hsan_pfe_pa_tables) == HSAN_PFE_PA_TABLE_COUNT);
/* One irreversible pair of OEM L3 interface table rows per boot.
 * No hardware table manager rollback or forwarding activation ACK.
 */
static atomic_t hsan_pfe_l3intf_write_attempted = ATOMIC_INIT(0);
static atomic_t hsan_pfe_napt_write_attempted = ATOMIC_INIT(0);
static atomic_t hsan_pfe_napt_partition_attempted = ATOMIC_INIT(0);
static atomic_t hsan_pfe_pa_write_attempted = ATOMIC_INIT(0);
static atomic_t hsan_pfe_ifc_write_attempted = ATOMIC_INIT(0);
static atomic_t hsan_pfe_ofc_write_attempted = ATOMIC_INIT(0);
static atomic_t hsan_pfe_mcc_write_attempted = ATOMIC_INIT(0);
/* PFE 62+ global writes have no known hardware rollback; a failed or
 * successful attempt cannot be safely replayed by a later reprobe.
 */
static atomic_t hsan_pfe_global_write_attempted = ATOMIC_INIT(0);
/* Software table release or platform unbind does not reverse OEM global
 * PFE register writes. Preserve the once-only guard across module reloads
 * by retaining a reference after the first attempted hardware write.
 */
static atomic_t hsan_pfe_lifetime_pinned = ATOMIC_INIT(0);
static void hsan_pfe_pin_hw_lifetime(void)
{
    if (atomic_cmpxchg(&hsan_pfe_lifetime_pinned, 0, 1) == 0) {
        __module_get(THIS_MODULE);
        pr_warn("hsan-lsw-pfe: non-reversible PFE MMIO attempt; module pinned until board reset\n");
    }
}

/* A dynamic GRO table cannot be sized independently from the DP PIE map.
 * Both values must represent the same *observed* OEM channel ownership;
 * neither creates a hardware owner or enables forwarding.
 */
static int hsan_pfe_observed_gro_count(struct device *dev, u32 *count)
{
    struct device_node *dp;
    u32 gro[2];
    u32 pfe_count = 0;
    bool pfe_seen, dp_seen;
    int ret;

    *count = 0;
    pfe_seen = of_find_property(dev->of_node,
                       "hsan,observed-pie-gro-count", NULL) != NULL;
    if (!pfe_seen)
        return 0;
    ret = of_property_read_variable_u32_array(dev->of_node,
                         "hsan,observed-pie-gro-count", &pfe_count, 1, 1);
    if (ret < 0)
        return ret;
    if (pfe_count > HSAN_PFE_MAX_OBSERVED_GRO)
        return -ERANGE;
    dp = of_find_compatible_node(NULL, NULL, "hsan,dp");
    if (!dp)
        return -ENODATA;
    if (!of_device_is_available(dp)) {
        of_node_put(dp);
        return -ENODEV;
    }
    dp_seen = of_find_property(dp, "hsan,observed-pie-gro-channels",
                                NULL) != NULL;
    ret = dp_seen ? of_property_read_variable_u32_array(dp,
             "hsan,observed-pie-gro-channels", gro, 2, 2) : -ENODATA;
    of_node_put(dp);
    if (ret < 0)
        return ret;
    /* Match the existing DP v300 observed-channel parser; a valid
     * prefix or a wider PUB limit does not make an observation valid.
     */
    if (!gro[0] || gro[0] > 7 ||
        gro[1] != pfe_count || gro[1] > 8U - gro[0]) {
        dev_err(dev, "DP/PFE PIE GRO ownership mismatch: dp=%u+%u, pfe=%u\n",
                gro[0], gro[1], pfe_count);
        return -EINVAL;
    }
    *count = pfe_count;
    return 0;
}

static int hsan_pfe_validate_pub_descriptors(void)
{
    size_t total = 0;
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(hsan_pfe_pub_tables); ++i) {
        const struct hsan_pfe_pub_table_desc *a = &hsan_pfe_pub_tables[i];
        size_t bytes;

        if (!a->shadow_name || !a->bytes_per_entry ||
            !IS_ALIGNED(a->bytes_per_entry, 4) ||
            check_mul_overflow((size_t)a->entries,
                               (size_t)a->bytes_per_entry, &bytes) ||
            check_add_overflow(total, bytes, &total) || total > SZ_256K)
            return -EINVAL;
        for (j = 0; j < i; ++j)
            if (a->id == hsan_pfe_pub_tables[j].id)
                return -EEXIST;
    }
    return 0;
}


struct hsan_pfe_state {
    struct device *owner;
	struct clk *clk;
	bool clock_on;
	bool global_attr_shadow_registered;
	bool pub_shadow_registered[HSAN_PFE_PUB_TABLE_COUNT];
	bool opp_shadow_registered[HSAN_PFE_OPP_TABLE_COUNT];
    bool mcc_shadow_registered[HSAN_PFE_MCC_TABLE_COUNT];
    bool ofc_shadow_registered[HSAN_PFE_OFC_TABLE_COUNT];
    bool service_shadow_registered[HSAN_PFE_SERVICE_TABLE_COUNT];
    bool pa_shadow_registered[HSAN_PFE_PA_TABLE_COUNT];
    bool napt_shadow_registered[HSAN_PFE_NAPT_TABLE_COUNT];
    u32 napt_observed_rows[HSAN_PFE_NAPT_TABLE_COUNT];
    bool napt_oem_geometry_verified;
    bool napt_partition_layout_verified; /* OEM 0x130/138/134/13c decoded */
    u32 napt_primary_level; /* OEM first 512<<n capacity search */
    u32 napt_secondary_level; /* OEM second capacity search */
    u32 napt_observed_memory_bytes;
    bool napt_fixed_attempted;
    bool napt_fixed_submitted;
    bool napt_partition_attempted;
    u32 napt_partition_commands; /* successful bit0-clear strobes, not NAT ready */
    u32 napt_partition_last_status;
    bool napt_hw_uncertain;
    bool ifc_hash_range_checked;
    bool ifc_hash_reserved;
    bool ifc_engine_quarantined;
    u32 ifc_hash_observed_start;
    bool pa_csr_attempted;
    bool pa_csr_verified;
    bool pa_hw_uncertain;
    bool ifc_shadow_registered;
    bool ifc_hw_attempted;
    bool ifc_hw_uncertain;
    u32 ifc_rows_committed;
    u32 ifc_rows_window_readback; /* CPU-visible 32-byte window, not IFC forwarding */
    u32 ifc_last_status;
    bool ifc_observed_image_verified;
    u32 ifc_observed_image_crc32;
    bool ifc_late_window_drift_latched;
    bool fam_fence_abort_latched;
    u32 fam_fence_abort_step;
    u32 ofc_dynamic_entries;
    bool ofc_csr_attempted;
    bool ofc_csr_verified;
    bool ofc_hw_uncertain;
    bool mcc_hw_attempted;
    bool mcc_write_stream_submitted;
    bool mcc_hw_uncertain;
    u32 mcc_hw_rows_written;
	bool global_writes_attempted;
    bool global_writes_completed;
    bool lan1_opp_verified;
    bool lan1_pub_verified;
    bool ifc_tag_verified;
    u32 pub_gro_entries;
    bool pub_gro_count_known;
    bool l3intf_plan_valid;
    u32 l3intf_plan_row;
    u32 l3intf_plan_crc32;
    u32 l3intf_plan_a[4];
    u32 l3intf_plan_b[4];
    u32 l3intf_preimage[8];
    bool l3intf_preflight_complete;
    bool l3intf_hw_readback_valid;
    bool l3intf_hw_matches_plan;
    bool l3intf_commit_requested;
    bool l3intf_write_attempted;
    bool l3intf_write_submitted;
    bool l3intf_write_readback_ok;
    u32 l3intf_write_errno;
    u32 l3intf_hw_readback_crc32;
    u64 l3intf_hw_read_begin_ns;
    u64 l3intf_hw_read_end_ns;
    bool lan1_vlan_sampled;
    bool lan1_vlan_membership;
    u32 lan1_vlan_id;
    u32 lan1_vlan_raw;
    u32 lan1_vlan_enable_plan;
    u32 lan1_vlan_disable_plan;
	bool opp_regs_hw_attempted;
	u32 opp_tfrt_hw_committed;
	bool opp_tfrt_hw_uncertain;
    bool probe_quarantined;
    bool bindings_retained;
    u32 probe_step;
    u32 probe_errno;
};

static bool hsan_pfe_hardware_attempted(const struct hsan_pfe_state *p)
{
    return p->global_writes_attempted || p->opp_regs_hw_attempted ||
        p->opp_tfrt_hw_committed || p->opp_tfrt_hw_uncertain ||
        p->ofc_csr_attempted || p->ofc_hw_uncertain ||
        p->mcc_hw_attempted || p->mcc_hw_uncertain ||
        p->napt_fixed_attempted || p->napt_partition_attempted ||
        p->napt_hw_uncertain || p->ifc_hw_attempted ||
        p->ifc_hw_uncertain || p->pa_csr_attempted || p->pa_hw_uncertain ||
        p->l3intf_write_submitted;
}

/* Stage52: a FAM/PIE STOP fence is monotonic and irrevocable for this
 * boot. It is a CPU-publication barrier, NOT a hardware DMA drain ACK.
 * Take a fresh observation before EACH PFE write-stage boundary and each
 * IFC row, rather than trusting an earlier sleepable preflight. Generic
 * CSR writes also reject an external STOP under their IRQ-safe spinlock.
 * Step 1=global, 2=OPP, 3=TFRT, 4=OFC, 5=MCC, 6=NAPT fixed,
 * 7=NAPT partition, 8=IFC, 9=PA; 100..111=individual IFC rows.
 */
static int hsan_pfe_fam_fence_guard(struct hsan_pfe_state *state, u32 step)
{
    if (!hsan_lsw_dp_fam_cpu_fenced())
        return 0;
    if (!state->fam_fence_abort_latched) {
        state->fam_fence_abort_latched = true;
        state->fam_fence_abort_step = step;
    }
    dev_err(state->owner,
            "PFE hardware transaction step %u refused: FAM CPU fence closed (not a HW STOP ACK)\n",
            step);
    return -ESHUTDOWN;
}

/* A copied report lets the PIE driver identify missing physical stages
 * without a potentially stale pointer into a removable platform device.
 */
static DEFINE_MUTEX(hsan_pfe_owner_lock);
/* Serialize probe/teardown, including the interval before owner publication.
 * A second platform device must never enter hardware initialization while
 * the first is probing or retained after a partial write failure.
 */
static DEFINE_MUTEX(hsan_pfe_probe_lock);
static struct hsan_pfe_state *hsan_pfe_owner;

/* hi_lsw_pfe.ifc_dump @0x4c0 uses bank0 0x50000 (2x128-bit rows).
 * Independently re-read the installed CPU MMIO window after the 11-rule
 * batch was completed. The hardware hash/table activation path has no
 * observed ACK: a match remains observational only, while mismatch is
 * a sticky hardware uncertainty that cannot be repaired by replay.
 */
static void hsan_pfe_observe_ifc_late_window(struct hsan_pfe_state *p,
                                             struct hsan_pfe_snapshot *out)
{
    u32 observed[HSAN_PFE_IFC_DEFAULT_ROWS * HSAN_PFE_IFC_OBSERVED_WORDS];
    u32 actual[HSAN_PFE_IFC_DATA_WORDS];
    unsigned int i;
    int ret;

    out->ifc_late_window_drift_latched =
        p->ifc_late_window_drift_latched;
    if (p->probe_quarantined || !p->clock_on ||
        !p->ifc_observed_image_verified ||
        p->ifc_rows_committed != HSAN_PFE_IFC_DEFAULT_ROWS)
        return;
    if (of_property_count_u32_elems(p->owner->of_node,
            "hsan,observed-ifc-default-words") != ARRAY_SIZE(observed)) {
        ret = -ESTALE;
        goto invalid;
    }
    ret = of_property_read_u32_array(p->owner->of_node,
            "hsan,observed-ifc-default-words", observed,
            ARRAY_SIZE(observed));
    if (ret)
        goto invalid;
    for (i = 0; i < HSAN_PFE_IFC_DEFAULT_ROWS; i++) {
        const u32 *expected = observed +
            i * HSAN_PFE_IFC_OBSERVED_WORDS + HSAN_PFE_IFC_CONTROL_WORDS;

        ret = hsan_lsw_ifc_read_window(i + 1, actual);
        if (ret)
            goto invalid;
        if (memcmp(actual, expected, sizeof(actual))) {
            ret = -EUCLEAN;
            goto invalid;
        }
        out->ifc_late_window_rows_matched++;
    }
    out->ifc_late_window_sampled = true;
    return;
invalid:
    p->ifc_late_window_drift_latched = true;
    p->ifc_hw_uncertain = true;
    out->ifc_late_window_drift_latched = true;
    out->ifc_late_window_read_errno = -ret;
}

/* Original hi_lsw_pfe.ko diagnostic callbacks:
 * ifc_diag_get_cnt @0x7bb8: bank0 0x1000/0x1018;
 * l2_diag_get_cnt  @0x79e4: bank1 0x40000/0x40004;
 * l3_diag_get_cnt  @0x77c8: bank2 0x1000/0x1018;
 * pa_diag_get_cnt  @0x7930: bank7 0x1000/0x1004;
 * napt_diag_get_cnt @0x7ac0: bank0 0x1024 + bank2 0x1014,
 *                            and bank3 0x1000.
 * These are UNLATCHED chip-wide counters; even a changing value cannot
 * be attributed to port10, forwarding or DMA drain. Default off because
 * read-side effects beyond the observed OEM call chain aren't qualified.
 */
static void hsan_pfe_observe_oem_diag(struct hsan_pfe_state *p,
                                      struct hsan_pfe_snapshot *out)
{
    static const u32 addrs[HSAN_PFE_OEM_DIAG_WORDS] = {
        0x00001000U, 0x00001018U,
        0x01040000U, 0x01040004U,
        0x02001000U, 0x02001018U,
        0x07001000U, 0x07001004U,
        0x00001024U, 0x02001014U, 0x03001000U,
    };
    static const u8 starts[5] = { 0, 2, 4, 6, 8 };
    static const u8 counts[5] = { 2, 2, 2, 2, 3 };
    unsigned int group, i;
    int ret;

    /* Preserve the owner's request even when sampling is blocked. */
    out->oem_diag_requested = of_property_read_bool(p->owner->of_node,
             "hsan,experimental-read-pfe-oem-diagnostics");
    if (p->probe_quarantined || !p->clock_on || !out->oem_diag_requested)
        return;
    out->oem_diag_sample_begin_ns = ktime_get_ns();
    for (group = 0; group < ARRAY_SIZE(starts); group++) {
        ret = 0;
        for (i = starts[group]; i < starts[group] + counts[group]; i++) {
            ret = hsan_lsw_read(addrs[i], &out->oem_diag_raw[i]);
            if (ret)
                break;
        }
        if (ret) {
            memset(&out->oem_diag_raw[starts[group]], 0,
                   counts[group] * sizeof(u32));
            continue;
        }
        out->oem_diag_valid_groups |= BIT(group);
    }
    out->oem_diag_sample_end_ns = ktime_get_ns();
}

int hsan_lsw_pfe_snapshot(struct hsan_pfe_snapshot *out)
{
    struct hsan_pfe_state *p;
    unsigned int i;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    mutex_lock(&hsan_pfe_owner_lock);
    p = hsan_pfe_owner;
    if (!p) {
        mutex_unlock(&hsan_pfe_owner_lock);
        return -ENODEV;
    }
    out->clock_on = p->clock_on;
    out->global_hw_init_attempted = p->global_writes_attempted;
    out->global_hw_init_completed = p->global_writes_completed;
    out->opp_hw_init_attempted = p->opp_regs_hw_attempted;
    out->tfrt_rows_hw_verified = p->opp_tfrt_hw_committed;
    out->tfrt_hw_uncertain = p->opp_tfrt_hw_uncertain;
    out->global_attr_shadow = p->global_attr_shadow_registered;
    out->pub_gro_count_known = p->pub_gro_count_known;
    out->pub_gro_entries = p->pub_gro_entries;
    out->l3intf_plan_valid = p->l3intf_plan_valid;
    out->l3intf_plan_row = p->l3intf_plan_row;
    out->l3intf_plan_crc32 = p->l3intf_plan_crc32;
    out->l3intf_hw_readback_valid = p->l3intf_hw_readback_valid;
    out->l3intf_hw_matches_plan = p->l3intf_hw_matches_plan;
    out->l3intf_hw_readback_crc32 = p->l3intf_hw_readback_crc32;
    out->l3intf_hw_read_begin_ns = p->l3intf_hw_read_begin_ns;
    out->l3intf_hw_read_end_ns = p->l3intf_hw_read_end_ns;
    out->lan1_vlan_sampled = p->lan1_vlan_sampled;
    out->lan1_vlan_membership = p->lan1_vlan_membership;
    out->lan1_vlan_id = p->lan1_vlan_id;
    out->lan1_vlan_raw = p->lan1_vlan_raw;
    out->lan1_vlan_enable_plan = p->lan1_vlan_enable_plan;
    out->lan1_vlan_disable_plan = p->lan1_vlan_disable_plan;
    out->pub_shadows_expected = HSAN_PFE_PUB_TABLE_COUNT -
        (p->pub_gro_entries ? 0U : 1U);
    out->mcc_hw_attempted = p->mcc_hw_attempted;
    out->mcc_write_stream_submitted = p->mcc_write_stream_submitted;
    out->mcc_hw_uncertain = p->mcc_hw_uncertain;
    out->mcc_transaction_sealed = hsan_lsw_mcc_health() == -EUCLEAN;
    out->mcc_rows_written = p->mcc_hw_rows_written;
    out->ofc_dynamic_entries = p->ofc_dynamic_entries;
    out->ofc_csr_attempted = p->ofc_csr_attempted;
    out->ofc_csr_verified = p->ofc_csr_verified;
    out->ofc_hw_uncertain = p->ofc_hw_uncertain;
    out->ifc_shadow_registered = p->ifc_shadow_registered;
    out->ifc_hw_attempted = p->ifc_hw_attempted;
    out->ifc_hw_uncertain = p->ifc_hw_uncertain;
    out->ifc_rows_committed = p->ifc_rows_committed;
    out->ifc_rows_window_readback = p->ifc_rows_window_readback;
    out->ifc_last_status = p->ifc_last_status;
    out->ifc_observed_image_verified = p->ifc_observed_image_verified;
    out->ifc_observed_image_crc32 = p->ifc_observed_image_crc32;
    hsan_pfe_observe_ifc_late_window(p, out);
    /* The late observation can latch uncertainty in this very snapshot. */
    out->ifc_hw_uncertain = p->ifc_hw_uncertain;
    hsan_pfe_observe_oem_diag(p, out);
    /* A verified MMIO command/window alone never establishes a PFE
     * physical forwarding path. Report each missing stage independently.
     * L2/L3 physical table callbacks and lookup ownership remain unported.
     */
    out->forwarding_blockers = HSAN_PFE_FWD_BLOCK_L2_L3;
    if (p->l3intf_write_submitted && !p->l3intf_write_readback_ok)
        out->forwarding_blockers |= HSAN_PFE_FWD_BLOCK_FAM_STOP;
    out->fam_cpu_stop_fenced = hsan_lsw_dp_fam_cpu_fenced();
    out->fam_fence_abort_latched = p->fam_fence_abort_latched;
    out->fam_fence_abort_step = p->fam_fence_abort_step;
    if (out->fam_cpu_stop_fenced || out->fam_fence_abort_latched)
        out->forwarding_blockers |= HSAN_PFE_FWD_BLOCK_FAM_STOP;
    if (!p->ifc_observed_image_verified || p->ifc_hw_uncertain ||
        !p->ifc_hw_attempted || p->ifc_engine_quarantined ||
        p->ifc_rows_committed != HSAN_PFE_IFC_DEFAULT_ROWS)
        out->forwarding_blockers |= HSAN_PFE_FWD_BLOCK_IFC;
    if (!p->pa_csr_verified || p->pa_hw_uncertain)
        out->forwarding_blockers |= HSAN_PFE_FWD_BLOCK_PA;
    if (!p->napt_fixed_submitted || p->napt_hw_uncertain)
        out->forwarding_blockers |= HSAN_PFE_FWD_BLOCK_NAPT;
    if (!p->ofc_csr_verified || p->ofc_hw_uncertain ||
        !p->mcc_write_stream_submitted || p->mcc_hw_uncertain)
        out->forwarding_blockers |= HSAN_PFE_FWD_BLOCK_OFC_MCC;
    out->ifc_hash_range_checked = p->ifc_hash_range_checked;
    out->ifc_hash_reserved = p->ifc_hash_reserved;
    out->ifc_engine_quarantined = p->ifc_engine_quarantined;
    out->ifc_hash_observed_start = p->ifc_hash_observed_start;
    out->pa_csr_attempted = p->pa_csr_attempted;
    out->pa_csr_verified = p->pa_csr_verified;
    out->pa_hw_uncertain = p->pa_hw_uncertain;
    out->napt_ipv4_rows = p->napt_observed_rows[0];
    out->napt_ipv6_rows = p->napt_observed_rows[1];
    out->napt_oem_geometry_verified = p->napt_oem_geometry_verified;
    out->napt_partition_layout_verified = p->napt_partition_layout_verified;
    out->napt_primary_level = p->napt_primary_level;
    out->napt_secondary_level = p->napt_secondary_level;
    out->napt_observed_memory_bytes = p->napt_observed_memory_bytes;
    /* Software-only NAPT occupancy, never HW flow installation. */
    if (p->napt_shadow_registered[0])
        hsan_lsw_shadow_table_usage(p->owner, 0x105,
                &out->napt_ipv4_reserved_rows,
                &out->napt_ipv4_materialized_rows);
    if (p->napt_shadow_registered[1])
        hsan_lsw_shadow_table_usage(p->owner, 0x106,
                &out->napt_ipv6_reserved_rows,
                &out->napt_ipv6_materialized_rows);
    out->napt_fixed_attempted = p->napt_fixed_attempted;
    out->napt_fixed_submitted = p->napt_fixed_submitted;
    out->napt_partition_attempted = p->napt_partition_attempted;
    out->napt_partition_commands = p->napt_partition_commands;
    out->napt_partition_last_status = p->napt_partition_last_status;
    out->napt_hw_uncertain = p->napt_hw_uncertain;
    for (i = 0; i < HSAN_PFE_NAPT_TABLE_COUNT; i++)
        out->napt_shadow_tables += p->napt_shadow_registered[i];
    for (i = 0; i < HSAN_PFE_SERVICE_TABLE_COUNT; i++)
        out->service_shadows += p->service_shadow_registered[i];
    for (i = 0; i < HSAN_PFE_PA_TABLE_COUNT; i++)
        out->pa_shadows += p->pa_shadow_registered[i];
    for (i = 0; i < HSAN_PFE_OFC_TABLE_COUNT; ++i)
        out->ofc_shadows += p->ofc_shadow_registered[i];
    for (i = 0; i < HSAN_PFE_PUB_TABLE_COUNT; i++)
        out->pub_shadows += p->pub_shadow_registered[i];
    for (i = 0; i < HSAN_PFE_OPP_TABLE_COUNT; i++)
        out->opp_shadows += p->opp_shadow_registered[i];
    for (i = 0; i < HSAN_PFE_MCC_TABLE_COUNT; i++)
        out->mcc_shadows += p->mcc_shadow_registered[i];
    mutex_unlock(&hsan_pfe_owner_lock);
    return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pfe_snapshot);

int hsan_lsw_pfe_l3intf_commit_snapshot(
    struct hsan_pfe_l3intf_commit_snapshot *out)
{
    struct hsan_pfe_state *p;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    mutex_lock(&hsan_pfe_owner_lock);
    p = hsan_pfe_owner;
    if (!p) {
        mutex_unlock(&hsan_pfe_owner_lock);
        return -ENODEV;
    }
    out->requested = p->l3intf_commit_requested;
    out->attempted = p->l3intf_write_attempted;
    out->mmio_submitted = p->l3intf_write_submitted;
    out->cpu_readback_ok = p->l3intf_write_readback_ok;
    out->last_errno = p->l3intf_write_errno;
    out->row = p->l3intf_plan_row;
    mutex_unlock(&hsan_pfe_owner_lock);
    return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pfe_l3intf_commit_snapshot);

int hsan_lsw_pfe_lifecycle_snapshot(struct hsan_pfe_lifecycle_snapshot *out)
{
    struct hsan_pfe_state *p;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    mutex_lock(&hsan_pfe_owner_lock);
    p = hsan_pfe_owner;
    if (!p) {
        mutex_unlock(&hsan_pfe_owner_lock);
        return -ENODEV;
    }
    out->hardware_attempted = hsan_pfe_hardware_attempted(p);
    out->quarantined = p->probe_quarantined;
    out->bindings_retained = p->bindings_retained;
    out->failed_step = p->probe_quarantined ? p->probe_step : 0;
    out->last_errno = p->probe_errno;
    mutex_unlock(&hsan_pfe_owner_lock);
    return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pfe_lifecycle_snapshot);

bool hsan_lsw_pfe_cpu_bootstrap_ready(void)
{
    struct hsan_pfe_state *p;
    bool ready;

    mutex_lock(&hsan_pfe_owner_lock);
    p = hsan_pfe_owner;
    ready = p && p->clock_on && !p->probe_quarantined &&
        p->global_writes_completed && p->lan1_opp_verified &&
        p->lan1_pub_verified && p->ifc_tag_verified &&
        p->ifc_rows_committed == HSAN_PFE_IFC_DEFAULT_ROWS &&
        p->ifc_rows_window_readback == HSAN_PFE_IFC_DEFAULT_ROWS &&
        !p->ifc_late_window_drift_latched &&
        !hsan_lsw_dp_fam_cpu_fenced();
    mutex_unlock(&hsan_pfe_owner_lock);
    return ready;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pfe_cpu_bootstrap_ready);

static const struct hsan_lsw_window hsan_pfe_windows[] = {
	{0x00, "ifc",     0x16100000U, 0x80000U},
	{0x01, "l2",      0x16180000U, 0x80000U},
	{0x02, "l3",      0x16200000U, 0x80000U},
	{0x03, "napt",    0x16280000U, 0x80000U},
	{0x04, "ofc",     0x16300000U, 0x80000U},
	{0x05, "mcc",     0x16380000U, 0x80000U},
	{0x06, "opp",     0x16400000U, 0x80000U},
	{0x07, "pa",      0x16080000U, 0x80000U},
	{0x08, "pe",      0x16600000U, 0x80000U},
	{0x09, "pfe_glb", 0x16000000U, 0x80000U},
	{0x0a, "prbs",    0x15380000U, 0x80000U},
	{0x0b, "pub",     0x16480000U, 0x80000U},
};
static const struct hsan_lsw_mapping hsan_pfe_cfg = {
	.windows = hsan_pfe_windows,
	.nr_windows = ARRAY_SIZE(hsan_pfe_windows),
};
/* OEM hi_pfe_init @.text+0x2a94: 14+48 tab128 entries, then
 * six global CSR ops.  Requires explicit experimental DT opt-in AND
 * hsan-lsw-csr's boot-time write permission.  hi_tab_mgr_create(0x10f)
 * and all PFE subsystem initialization are separate, still unported.
 */
static int hsan_pfe_global_csrs(bool submit)
{
	static const struct { u32 reg, val; } writes[] = {
		{0x08000368U, 11}, {0x04000070U, 0x02000000U},
		{0x00000020U, 0x0707ffffU}, {0x06000140U, 1},
		{0x06000144U, 0},
	};
	const u32 first[4] = {0x00080000U, 0, 0, 0};
	const u32 second[4] = {0x08080808U, 0x08080808U,
			       0x08080808U, 0x00000808U};
	unsigned int i;
	int ret;

	/* Preflight all required bank ranges to avoid a partial PFE table
	 * write caused by a missing mapping or insufficient bank length.
	 * Ownership remains with this platform device until remove().
	 */
	ret = hsan_lsw_check_span(HSAN_PFE_PA_PORT_TAB_BASE,
				  HSAN_PFE_PA_PORT_BOOT_ROWS * HSAN_PFE_PA_PORT_TAB_STRIDE,
				  true);
	if (ret)
		return ret;
	ret = hsan_lsw_check_span(0x07041600U, 48 * 16, true);
	if (ret)
		return ret;
	ret = hsan_lsw_check_span(0x0100010cU, 4, true);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		ret = hsan_lsw_check_span(writes[i].reg, 4, true);
		if (ret)
			return ret;
	}
	if (!submit)
		return 0;

	for (i = 0; i < HSAN_PFE_PA_PORT_BOOT_ROWS; i++) {
		ret = hsan_lsw_write_tab128(HSAN_PFE_PA_PORT_TAB_BASE, i, first);
		if (ret)
			return ret;
	}
	for (i = 0; i < 48; i++) {
		ret = hsan_lsw_write_tab128(0x07041600U, i, second);
		if (ret)
			return ret;
	}
	ret = hsan_lsw_update_bits(0x0100010cU, BIT(6), 0);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		ret = hsan_lsw_write(writes[i].reg, writes[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int hsan_pfe_prepare_global_attr_shadow(struct platform_device *pdev,
						struct hsan_pfe_state *state)
{
	int ret;

	ret = hsan_lsw_shadow_table_create(&pdev->dev,
			HSAN_PFE_GLOBAL_ATTR_TAB, "pfe_glb_attr", 3, 4, 1);
	if (ret)
		return ret;
	state->global_attr_shadow_registered = true;
	dev_info(&pdev->dev,
		 "PFE global attribute tab_mgr id=0x10f shadow registered (3 x 4B), NO HW backing\n");
	return 0;
}

static void hsan_pfe_release_global_attr_shadow(struct platform_device *pdev,
						 struct hsan_pfe_state *state)
{
	int ret;

	if (!state->global_attr_shadow_registered)
		return;
	ret = hsan_lsw_shadow_table_delete(&pdev->dev,
			HSAN_PFE_GLOBAL_ATTR_TAB);
	if (ret)
		dev_warn(&pdev->dev, "PFE global attribute shadow delete failed: %d\n", ret);
	else
		state->global_attr_shadow_registered = false;
}

/* Preserve the original order, and release successful earlier owners
 * in reverse order on any error (the original init loop returns early).
 * Missing GRO count skips only the dynamic ID=0x128, as OEM does when
 * hi_pie_get_gro_ch_num() returns zero. An observed DT property may be
 * supplied for *software planning* but never asserts PIE readiness.
 */
static void hsan_pfe_release_pub_shadows(struct platform_device *pdev,
                                         struct hsan_pfe_state *state)
{
    int i;

    for (i = HSAN_PFE_PUB_TABLE_COUNT - 1; i >= 0; i--) {
        int ret;

        if (!state->pub_shadow_registered[i])
            continue;
        ret = hsan_lsw_shadow_table_delete(&pdev->dev,
                                            hsan_pfe_pub_tables[i].id);
        if (ret)
            dev_warn(&pdev->dev, "PUB shadow table %#x delete failed: %d\n",
                     hsan_pfe_pub_tables[i].id, ret);
        else
            state->pub_shadow_registered[i] = false;
    }
}

static int hsan_pfe_prepare_pub_shadows(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    u32 gro_count = 0;
    unsigned int i, registered = 0;
    int ret;

    ret = hsan_pfe_validate_pub_descriptors();
    if (ret)
        return ret;
    ret = hsan_pfe_observed_gro_count(&pdev->dev, &gro_count);
    if (ret)
        return ret;
    /* A missing observed count was historically treated as zero, so
     * eight software tables appeared while PIE always required nine.
     * Preserve the distinction between a proven zero and unknown count.
     */
    state->pub_gro_count_known =
        of_find_property(pdev->dev.of_node,
                         "hsan,observed-pie-gro-count", NULL) != NULL;
    state->pub_gro_entries = gro_count;
    for (i = 0; i < ARRAY_SIZE(hsan_pfe_pub_tables); ++i) {
        const struct hsan_pfe_pub_table_desc *d = &hsan_pfe_pub_tables[i];
        u32 entries = (d->id == HSAN_PFE_PUB_GRO_ID) ?
                      gro_count : d->entries;

        if (!entries)
            continue;
        ret = hsan_lsw_shadow_table_create(&pdev->dev, d->id,
                                d->shadow_name, entries,
                                d->bytes_per_entry, 0);
        if (ret) {
            dev_err(&pdev->dev,
                    "PUB shadow create id=%#x entries=%u width=%u failed: %d\n",
                    d->id, entries, d->bytes_per_entry, ret);
            goto rollback;
        }
        state->pub_shadow_registered[i] = true;
        registered++;
    }
    dev_info(&pdev->dev,
             "PUB software-only tables: %u/%u registered (GRO=%u known=%d), no hardware callbacks\n",
             registered,
             HSAN_PFE_PUB_TABLE_COUNT - (gro_count ? 0U : 1U),
             gro_count, state->pub_gro_count_known);
    return 0;
rollback:
    hsan_pfe_release_pub_shadows(pdev, state);
    return ret;
}

/* Stage76: OEM PUB table #1 (id=0x102) l3intf_write @0x3b38
 * accepts a 20-byte ARM32 record, then calls write_tab128 twice at
 * 0x0b05b000/0x0b05c000. A DT observed row is strictly a software
 * preflight: WITHOUT an independent table-manager/forwarding ACK,
 * NEVER send those writes to hardware. No guessed LAN1 L3 address.
 * The six DT cells are (row, five 32-bit little-endian OEM words).
 */
static int hsan_pfe_plan_observed_l3intf(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    static const char prop[] = "hsan,observed-l3intf-words";
    struct hsan_pfe_l3intf_plan plan;
    u8 src[HSAN_PFE_L3INTF_INPUT_BYTES];
    u32 cells[6];
    unsigned int i;
    int ret;

    if (!of_find_property(pdev->dev.of_node, prop, NULL))
        return 0;
    if (of_property_count_u32_elems(pdev->dev.of_node, prop) != 6)
        return -EINVAL;
    ret = of_property_read_u32_array(pdev->dev.of_node, prop,
                                     cells, ARRAY_SIZE(cells));
    if (ret)
        return ret;
    for (i = 0; i < 5; i++)
        put_unaligned_le32(cells[i + 1], src + 4 * i);
    ret = hsan_pfe_l3intf_pack(cells[0], src, &plan);
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(HSAN_PFE_L3INTF_BANK_A +
                              plan.row * 16, 16, false);
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(HSAN_PFE_L3INTF_BANK_B +
                              plan.row * 16, 16, false);
    if (ret)
        return ret;
    /* CRC covers a canonical LE wire representation, independent of
     * host alignment; it is a plan identifier, not installed state.
     */
    {
        u8 encoded[32];

        for (i = 0; i < 4; i++) {
            put_unaligned_le32(plan.a[i], encoded + i * 4);
            put_unaligned_le32(plan.b[i], encoded + 16 + i * 4);
        }
        state->l3intf_plan_crc32 = crc32_le(0, encoded, sizeof(encoded));
    }
    state->l3intf_plan_row = plan.row;
    memcpy(state->l3intf_plan_a, plan.a, sizeof(plan.a));
    memcpy(state->l3intf_plan_b, plan.b, sizeof(plan.b));
    state->l3intf_plan_valid = true;
    dev_info(&pdev->dev,
             "OEM L3intf id=0x102 row=%u shadow encode crc32=%08x (NO MMIO / NO HW owner)\n",
             plan.row, state->l3intf_plan_crc32);
    return 0;
}

/* Stage77: optional read-only comparison of the actual paired OEM
 * L3 interface rows with the stage76 CPU encoding. The A32 write callback
 * uses two distinct table windows in one logical bank; one 128-bit
 * snapshot is NOT enough.
 * OEM hi_lsw_pub.ko read_tab128 provides the corresponding increasing-
 * address reader. No MMIO writes and no claim that either read was an
 * atomic multi-bank hardware snapshot or a forwarding ownership ACK.
 */
static int hsan_pfe_observe_l3intf_hw(struct platform_device *pdev,
                                      struct hsan_pfe_state *state)
{
    static const char prop[] = "hsan,experimental-read-l3intf-rows";
    u32 words[8];
    unsigned int i;
    u8 encoded[sizeof(words)];
    int ret;

    if (!of_property_read_bool(pdev->dev.of_node, prop))
        return 0;
    if (!state->l3intf_plan_valid)
        return -ENODATA;
    if (!state->clock_on)
        return -EHOSTDOWN;
    state->l3intf_hw_read_begin_ns = ktime_get_ns();
    ret = hsan_lsw_read_tab128(HSAN_PFE_L3INTF_BANK_A,
                               state->l3intf_plan_row, words);
    if (ret)
        goto read_failed;
    ret = hsan_lsw_read_tab128(HSAN_PFE_L3INTF_BANK_B,
                               state->l3intf_plan_row, words + 4);
    if (ret)
        goto read_failed;
    state->l3intf_hw_read_end_ns = ktime_get_ns();
    for (i = 0; i < ARRAY_SIZE(words); ++i)
        put_unaligned_le32(words[i], encoded + i * 4);
    state->l3intf_hw_readback_crc32 = crc32_le(0, encoded,
                                               sizeof(encoded));
    state->l3intf_hw_matches_plan =
        !memcmp(words, state->l3intf_plan_a, 16) &&
        !memcmp(words + 4, state->l3intf_plan_b, 16);
    state->l3intf_hw_readback_valid = true;
    dev_info(&pdev->dev,
        "OEM L3intf bank A/B row %u observed crc32=%08x planned=%08x match=%d (read only; HW forwarding still unproven)\n",
        state->l3intf_plan_row, state->l3intf_hw_readback_crc32,
        state->l3intf_plan_crc32, state->l3intf_hw_matches_plan);
    return 0;
read_failed:
    state->l3intf_hw_read_end_ns = ktime_get_ns();
    dev_err(&pdev->dev,
            "OEM L3intf paired read-only row %u failed %d; no writes attempted\n",
            state->l3intf_plan_row, ret);
    return ret;
}

static bool hsan_pfe_dp_lan1_handoff(const struct hsan_dp_queue_snapshot *q);

/* Resolve the entire optional transaction before any PFE global write.
 * Cache the observed input once; an overlay change between preflight and
 * commit must not substitute a different eight-word transaction.
 */
static int hsan_pfe_preflight_l3intf(struct platform_device *pdev,
                                    struct hsan_pfe_state *state,
                                    bool globals)
{
    static const char preimage[] = "hsan,observed-l3intf-preimage-words";
    struct hsan_dp_queue_snapshot q;
    int ret;

    state->l3intf_commit_requested = of_property_read_bool(pdev->dev.of_node,
                                     "hsan,experimental-commit-l3intf-row");
    if (!state->l3intf_commit_requested)
        return 0;
    if (!globals || !state->l3intf_plan_valid || !state->clock_on ||
        !state->pub_shadow_registered[1])
        return -EINVAL;
    if (of_property_count_u32_elems(pdev->dev.of_node, preimage) != 8)
        return -ENODATA;
    ret = of_property_read_u32_array(pdev->dev.of_node, preimage,
                     state->l3intf_preimage, ARRAY_SIZE(state->l3intf_preimage));
    if (ret)
        return ret;
    ret = hsan_lsw_dp_get_queue(10, &q);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&q))
        return -ENOLINK;
    ret = hsan_lsw_l3intf_pair_preflight(&pdev->dev, state->l3intf_plan_row,
                           state->l3intf_preimage, state->l3intf_preimage + 4);
    if (ret)
        return ret;
    state->l3intf_preflight_complete = true;
    return 0;
}

/* Stage78: an explicit board-lab-only OEM L3 interface physical
 * callback. A trustworthy 8-word preimage is mandatory; the CSR owner
 * compares both physical rows while holding the same lock used to submit
 * them. Do not allow an optional write from an uninitialized PFE/FAM/DP
 * stack, or treat post-write CPU readback as forwarding readiness.
 *
 * This executes at the END of the OEM PFE initialization attempt, after
 * global/IFC/PA stage ordering; it never executes during normal probe.
 * It is intentionally independent of the Stage77 read-only sampling flag.
 */
static int hsan_pfe_commit_l3intf_row(struct platform_device *pdev,
                                      struct hsan_pfe_state *state)
{
    struct hsan_dp_queue_snapshot q;
    bool submitted = false, matched = false;
    u64 begin_ns;
    int ret;

    if (!state->l3intf_commit_requested)
        return 0;
    if (!state->l3intf_preflight_complete ||
        !state->l3intf_plan_valid || !state->clock_on ||
        !state->pub_shadow_registered[1] ||
        !state->global_writes_completed)
        return -EHOSTDOWN;
    ret = hsan_lsw_dp_get_queue(10, &q);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&q))
        return -ENOLINK;
    ret = hsan_pfe_fam_fence_guard(state, 10);
    if (ret)
        return ret;
    /* An irreversible attempt must be kept for lifetime/rollback
     * diagnostics even if the CPU-visible readback later disagrees.
     */
    if (atomic_cmpxchg(&hsan_pfe_l3intf_write_attempted, 0, 1))
        return -EALREADY;
    state->l3intf_write_attempted = true;
    hsan_pfe_pin_hw_lifetime();
    begin_ns = ktime_get_ns();
    ret = hsan_lsw_l3intf_pair_commit(&pdev->dev,
                                     state->l3intf_plan_row,
                                     state->l3intf_preimage,
                                     state->l3intf_preimage + 4,
                                     state->l3intf_plan_a,
                                     state->l3intf_plan_b,
                                     &submitted, &matched);
    state->l3intf_write_submitted = submitted;
    state->l3intf_write_readback_ok = matched;
    state->l3intf_write_errno = ret < 0 ? (u32)-ret : 0;
    if (submitted) {
        /* Do not keep an obsolete pre-write observation as the latest
         * row sample. A matched commit readback qualifies CPU words only.
         */
        state->l3intf_hw_read_begin_ns = begin_ns;
        state->l3intf_hw_read_end_ns = ktime_get_ns();
        state->l3intf_hw_readback_valid = matched;
        state->l3intf_hw_matches_plan = matched;
        state->l3intf_hw_readback_crc32 = matched ?
                                         state->l3intf_plan_crc32 : 0;
    }
    if (ret) {
        dev_err(&pdev->dev,
                "OEM L3intf physical pair row=%u ret=%d MMIO-submitted=%d; no rollback/forwarding ACK\n",
                state->l3intf_plan_row, ret, submitted);
        return ret;
    }
    dev_warn(&pdev->dev,
             "OEM L3intf row=%u A+B MMIO submitted/readback matched; L2/L3 forwarding and DMA STOP remain UNVERIFIED\n",
             state->l3intf_plan_row);
    return 0;
}

/* hi_lsw_pfe.ko port_vlan_write @.text+0x3998..0x39f0,
 * PUB table #7 (id=0x117): address is 0x0b040000 + VID*4.
 * OEM sets bit (port & 31) in the low16 and also bit16 (valid).
 * Only inspect explicitly observed VID, with a physically enabled
 * PFE clock. Never infer VLAN from the U-Boot IP or program a VLAN.
 */
static int hsan_pfe_observe_lan1_vlan(struct platform_device *pdev,
                                      struct hsan_pfe_state *state)
{
    static const char prop[] = "hsan,observed-lan1-vlan-id";
    u32 vid, raw;
    int ret;

    if (!of_find_property(pdev->dev.of_node, prop, NULL))
        return 0;
    ret = of_property_read_u32(pdev->dev.of_node, prop, &vid);
    if (ret)
        return ret;
    /* 4095 entries describe indices 0..4094, never index 4095. */
    if (vid >= 4095U)
        return -ERANGE;
    if (!state->clock_on)
        return -EHOSTDOWN;
    ret = hsan_lsw_read(0x0b040000U + vid * 4, &raw);
    if (ret)
        return ret;
    state->lan1_vlan_sampled = true;
    state->lan1_vlan_id = vid;
    state->lan1_vlan_raw = raw;
    ret = hsan_pfe_vlan_plan(vid, HSAN_PFE_LAN1_PORT, raw, true,
                             &state->lan1_vlan_enable_plan);
    if (ret)
        return ret;
    ret = hsan_pfe_vlan_plan(vid, HSAN_PFE_LAN1_PORT, raw, false,
                             &state->lan1_vlan_disable_plan);
    if (ret)
        return ret;
    state->lan1_vlan_membership = (raw & (BIT(10) | BIT(16))) ==
                                 (BIT(10) | BIT(16));
    dev_info(&pdev->dev,
             "OEM PUB port10 VLAN%u raw=%08x add_plan=%08x del_plan=%08x member_and_valid=%d (CPU-only plan, no writes)\n",
             vid, raw, state->lan1_vlan_enable_plan,
             state->lan1_vlan_disable_plan, state->lan1_vlan_membership);
    return 0;
}

/* Additional OEM OPP table-manager owners, no guessed register writes.
 * The original follows the two hi_tab_mgr_create calls with per-port
 * queue-derived MMIO; that hardware phase is explicitly NOT executed.
 */
static void hsan_pfe_release_opp_shadows(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    int i;

    for (i = HSAN_PFE_OPP_TABLE_COUNT - 1; i >= 0; --i) {
        int ret;

        if (!state->opp_shadow_registered[i])
            continue;
        ret = hsan_lsw_shadow_table_delete(&pdev->dev,
                                            hsan_pfe_opp_tables[i].id);
        if (ret)
            dev_warn(&pdev->dev, "OPP shadow table %#x release: %d\n",
                     hsan_pfe_opp_tables[i].id, ret);
        else
            state->opp_shadow_registered[i] = false;
    }
}

static int hsan_pfe_prepare_opp_shadows(struct platform_device *pdev,
                                       struct hsan_pfe_state *state)
{
    unsigned int i;
    int ret;

    for (i = 0; i < ARRAY_SIZE(hsan_pfe_opp_tables); i++) {
        const struct hsan_pfe_pub_table_desc *t = &hsan_pfe_opp_tables[i];

        ret = hsan_lsw_shadow_table_create(&pdev->dev, t->id,
                        t->shadow_name, t->entries, t->bytes_per_entry, 0);
        if (ret) {
            hsan_pfe_release_opp_shadows(pdev, state);
            return ret;
        }
        state->opp_shadow_registered[i] = true;
    }
    return 0;
}

/* OFC table ownership follows the OEM 0x108 -> 0x104 create order.
 * On any partial registration error, release 0x104 before 0x108.
 */
static void hsan_pfe_release_ofc_shadows(struct platform_device *pdev,
                                          struct hsan_pfe_state *state)
{
    int i;

    for (i = HSAN_PFE_OFC_TABLE_COUNT - 1; i >= 0; --i) {
        int ret;

        if (!state->ofc_shadow_registered[i])
            continue;
        ret = hsan_lsw_shadow_table_delete(&pdev->dev,
                                            hsan_pfe_ofc_tables[i].id);
        if (ret)
            dev_warn(&pdev->dev, "OFC shadow id=%#x delete failed: %d\n",
                     hsan_pfe_ofc_tables[i].id, ret);
        else
            state->ofc_shadow_registered[i] = false;
    }
}

static int hsan_pfe_prepare_ofc_shadows(struct platform_device *pdev,
                                         struct hsan_pfe_state *state)
{
    struct device *dev = &pdev->dev;
    u32 observed = 0;
    unsigned int i;
    int ret;

    if (of_find_property(dev->of_node,
                         "hsan,observed-ofc-available-entries", NULL)) {
        ret = of_property_read_u32(dev->of_node,
                             "hsan,observed-ofc-available-entries", &observed);
        if (ret)
            return ret;
        /* hi_pfe_ofc_init starts from 256 and subtracts the OEM
         * runtime helper result; never invent that result or allow
         * zero/overflow to masquerade as a qualified hardware owner.
         */
        if (!observed || observed > 256)
            return -ERANGE;
    }
    state->ofc_dynamic_entries = observed;
    for (i = 0; i < HSAN_PFE_OFC_TABLE_COUNT; ++i) {
        const struct hsan_pfe_pub_table_desc *tab = &hsan_pfe_ofc_tables[i];
        u32 entries = i ? tab->entries : observed;

        if (!entries)
            continue;
        ret = hsan_lsw_shadow_table_create(dev, tab->id,
                tab->shadow_name, entries, tab->bytes_per_entry, 0);
        if (ret) {
            hsan_pfe_release_ofc_shadows(pdev, state);
            return ret;
        }
        state->ofc_shadow_registered[i] = true;
    }
    if (!observed)
        dev_info(dev, "OFC id=0x104 shadow registered; id=0x108 runtime capacity unknown (no HW writes)\n");
    return 0;
}

/* One physical DP->PIE handoff contract for all PFE sub-engines.
 * A registered shadow table, a completed EQM FIFO command or a PIE map
 * CSR write is NOT enough to prove the 640 KiB FAM DMA image was handed
 * to the device. Stage47 used weaker conditions for OFC/IFC than MCC and
 * NAPT, allowing an unqualified early irreversible PFE write.
 * These checks are state gates, not a replacement for a hardware STOP ACK.
 */
static bool hsan_pfe_dp_lan1_handoff(const struct hsan_dp_queue_snapshot *q)
{
    return q->reserved_memory_verified && q->fam_addresses_prepared &&
        q->fam_cpu_table_verified && q->fam_dma_attrs_zero &&
        q->fam_oem_runtime_geometry_verified && q->fam_hw_owned &&
        q->fam_hw_ack_seen && !q->fam_cpu_stop_fenced &&
        !q->fam_hw_stop_ack_seen && q->eqm_bootstrap_committed &&
        q->eqm_queue_rows_committed && q->pie_map_hw_committed &&
        !q->pie_map_hw_uncertain;
}

/* OEM hi_pfe_ofc_init @0x5280..0x52a4 performs:
 *   CSR[0x04000030] = (old & ~0x7ff) | 0x124
 * This is only ONE verified register prelude; ofc_write/QoS callbacks,
 * port tables and IRQ/ACK are still missing. An explicit separate opt-in
 * must be present, and the register mask is checked after the write.
 */
static int hsan_pfe_preflight_ofc_control(struct device *dev,
                                           struct hsan_pfe_state *state)
{
    struct hsan_dp_queue_snapshot q;
    int ret;

    /* This masked control prelude addresses no OFC table entries.
     * Runtime table capacity remains unknown and is not fabricated here.
     * Keep clock, FAM/queue handoff and explicit control opt-in checks.
     */
    if (!state->clock_on)
        return -ENODATA;
    ret = hsan_lsw_dp_get_queue(10, &q);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&q))
        return -ENODATA;
    ret = hsan_lsw_check_span(HSAN_PFE_OFC_CONTROL_CSR, sizeof(u32), true);
    if (ret)
        dev_err(dev, "OFC control CSR not available: %d\n", ret);
    return ret;
}

static int hsan_pfe_program_ofc_control(struct platform_device *pdev,
                                         struct hsan_pfe_state *state)
{
    struct device *dev = &pdev->dev;
    u32 observed;
    int ret;

    if (!of_property_read_bool(dev->of_node,
                          "hsan,experimental-program-ofc-control"))
        return 0;
    if (!state->global_writes_completed || !state->opp_regs_hw_attempted)
        return -EHOSTDOWN;
    ret = hsan_pfe_preflight_ofc_control(dev, state);
    if (ret)
        return ret;
    if (atomic_cmpxchg(&hsan_pfe_ofc_write_attempted, 0, 1))
        return -EALREADY;
    state->ofc_csr_attempted = true;
    state->ofc_hw_uncertain = true;
    ret = hsan_lsw_update_bits_verify(HSAN_PFE_OFC_CONTROL_CSR,
                  HSAN_PFE_OFC_CONTROL_MASK, HSAN_PFE_OFC_CONTROL_VALUE);
    if (ret)
        goto failed;
    ret = hsan_lsw_read(HSAN_PFE_OFC_CONTROL_CSR, &observed);
    if (ret)
        goto failed;
    if ((observed & HSAN_PFE_OFC_CONTROL_MASK) !=
        HSAN_PFE_OFC_CONTROL_VALUE) {
        ret = -EIO;
        goto failed;
    }
    state->ofc_csr_verified = true;
    state->ofc_hw_uncertain = false; /* just this masked CSR, NOT OFC */
    dev_warn(dev, "OFC CSR prelude verified; OFC tables/routing NOT operational\n");
    return 0;
failed:
    dev_err(dev, "OFC CSR prelude status uncertain (%d); no replay without reset\n", ret);
    return ret;
}

/* Reversible software table lifetimes for the three OEM service
 * subsystems, followed by PA. A table-manager reservation does not
 * install a hardware callback, IRQ owner, route or forwarding rule.
 */
static void hsan_pfe_release_aux_tables(struct platform_device *pdev,
                    const struct hsan_pfe_pub_table_desc *desc,
                    bool *owned, unsigned int count)
{
    int i;

    for (i = (int)count - 1; i >= 0; i--) {
        int ret;

        if (!owned[i])
            continue;
        ret = hsan_lsw_shadow_table_delete(&pdev->dev, desc[i].id);
        if (ret)
            dev_warn(&pdev->dev, "PFE auxiliary table %#x release failed: %d\n",
                     desc[i].id, ret);
        else
            owned[i] = false;
    }
}

static int hsan_pfe_prepare_aux_tables(struct platform_device *pdev,
                    const struct hsan_pfe_pub_table_desc *desc,
                    bool *owned, unsigned int count)
{
    unsigned int i, j;
    int ret;

    for (i = 0; i < count; i++) {
        size_t bytes;

        if (!desc[i].entries || !desc[i].bytes_per_entry ||
            !IS_ALIGNED(desc[i].bytes_per_entry, 4) ||
            check_mul_overflow((size_t)desc[i].entries,
                               (size_t)desc[i].bytes_per_entry, &bytes) ||
            bytes > SZ_64K)
            return -EINVAL;
        for (j = 0; j < i; j++)
            if (desc[i].id == desc[j].id)
                return -EEXIST;
    }
    for (i = 0; i < count; i++) {
        ret = hsan_lsw_shadow_table_create(&pdev->dev, desc[i].id,
                    desc[i].shadow_name, desc[i].entries,
                    desc[i].bytes_per_entry, 0);
        if (ret) {
            hsan_pfe_release_aux_tables(pdev, desc, owned, count);
            return ret;
        }
        owned[i] = true;
    }
    return 0;
}

/* hi_pfe_napt_init @0x6390..0x6444 searches two bounded power-of-two
 * cost budgets. It then computes the 0x105 (IPv4) and 0x106 (IPv6)
 * hi_tab_mgr_create counts at @0x6540/0x65ec -> @0x5354:
 *   selected > 0: ipv6 = 8 * (512 << (selected - 1) + 64),
 *                 ipv4 = ipv6 - 256
 *   selected == 0: ipv6 = 512, ipv4 = 256.
 * Here parentheses follow the OEM actual (512<<n)+64 operation.
 * The second budget removes the selected first-stage capacity cost
 * (0x01a9a000 only for level 8, @0x63f0; lower levels use @0x6604).
 * No deduction from Linux DRAM size is allowed. Both memory words come
 * ONLY from hi_system_mem_info_get(2) observations, never guesses.
 * This calculates expected table rows, NOT the entire hardware memory
 * partition or the NAPT command/ACK protocol.
 */
#define HSAN_PFE_NAPT_OEM_FIXED_RESERVE 0x01a9a000U
#define HSAN_PFE_NAPT_MAX_LEVEL 8U
static int hsan_pfe_oem_napt_geometry(const u32 memory[2],
                                      u32 observed_v4, u32 observed_v6,
                                      u32 *primary, u32 *secondary)
{
    u32 level, first = 0, second = 0;
    u32 expected_v4, expected_v6;
    u64 budget, remaining, end;

    end = (u64)memory[0] + memory[1];
    if (!IS_ALIGNED(memory[0], SZ_4K) ||
        !IS_ALIGNED(memory[1], SZ_4K) ||
        !memory[1] || end > (u64)U32_MAX + 1)
        return -ERANGE;
    /* @0x6390 branches directly to the fallback table sizes when the
     * observed region is smaller than 128 KiB: neither budget loop runs.
     */
    if (memory[1] < SZ_128K) {
        if (observed_v4 != 256 || observed_v6 != 512)
            return -EINVAL;
        *primary = 0;
        *secondary = 0;
        return 0;
    }
    budget = memory[1] / 2U;
    remaining = memory[1];
    for (level = 0; level < HSAN_PFE_NAPT_MAX_LEVEL; level++) {
        u64 slots = 512ULL << level;
        u64 cost = 16ULL * slots +
                   2048ULL * DIV_ROUND_UP_ULL(slots, 5);

        if (cost > budget)
            break;
        first = level + 1;
    }
    /* @0x63f8 subtracts the cost of the selected first-stage level.
     * The assembler loads 0x01a9a000 only for the max-level path;
     * @0x6604 recomputes the cost for partial-capacity selection.
     */
    if (first) {
        u64 slots = 512ULL << (first - 1);
        u64 selected_cost = 16ULL * slots +
                           2048ULL * DIV_ROUND_UP_ULL(slots, 5);

        if (first == HSAN_PFE_NAPT_MAX_LEVEL &&
            selected_cost != HSAN_PFE_NAPT_OEM_FIXED_RESERVE)
            return -EINVAL;
        if (selected_cost > remaining)
            return -ERANGE;
        remaining -= selected_cost;
    }
    for (level = 0; level < HSAN_PFE_NAPT_MAX_LEVEL; level++) {
        u64 slots = 512ULL << level;
        u64 cost = 16ULL * slots +
                   2048ULL * DIV_ROUND_UP_ULL(2 * slots, 7);

        if (cost > remaining)
            break;
        second = level + 1;
    }
    if (!first) {
        expected_v6 = 512;
        expected_v4 = 256;
    } else {
        expected_v6 = ((512U << (first - 1)) + 64U) * 8U;
        expected_v4 = expected_v6 - 256U;
    }
    if (observed_v4 != expected_v4 || observed_v6 != expected_v6)
        return -EINVAL;
    *primary = first;
    *secondary = second;
    return 0;
}

/* The real 0x105/0x106 row counts depend on the OEM runtime memory
 * partitioner, which is not reconstructable from static FDT alone. No
 * property => NO table registration; incomplete/invalid property => fail.
 * Dense tables retain a 256 KiB limit; NAPT uses sparse CPU-only shadows.
 */
static int hsan_pfe_prepare_napt_shadows(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    struct device *dev = &pdev->dev;
    u32 rows[HSAN_PFE_NAPT_TABLE_COUNT];
    u32 memory[2], primary = 0, secondary = 0;
    bool observed_memory;
    unsigned int i;
    int ret, elems;

    elems = of_property_count_u32_elems(dev->of_node,
                   "hsan,observed-napt-table-rows");
    if (!of_find_property(dev->of_node,
                          "hsan,observed-napt-table-rows", NULL))
        return 0; /* no board trace supplied */
    if (elems != HSAN_PFE_NAPT_TABLE_COUNT)
        return -EINVAL;
    ret = of_property_read_u32_array(dev->of_node,
                    "hsan,observed-napt-table-rows", rows,
                    HSAN_PFE_NAPT_TABLE_COUNT);
    if (ret)
        return ret;
    for (i = 0; i < HSAN_PFE_NAPT_TABLE_COUNT; ++i) {
        if (!rows[i] || rows[i] > SZ_1M)
            return -ERANGE;
    }
    observed_memory = !!of_find_property(dev->of_node,
                "hsan,observed-napt-runtime-memory", NULL);
    if (observed_memory) {
        if (of_property_count_u32_elems(dev->of_node,
                "hsan,observed-napt-runtime-memory") != 2)
            return -EINVAL;
        ret = of_property_read_u32_array(dev->of_node,
                "hsan,observed-napt-runtime-memory", memory, 2);
        if (ret)
            return ret;
        ret = hsan_pfe_oem_napt_geometry(memory, rows[0], rows[1],
                                         &primary, &secondary);
        if (ret) {
            dev_err(dev, "OEM NAPT expected row geometry mismatch: rows=%u/%u memory=%u ret=%d\n",
                    rows[0], rows[1], memory[1], ret);
            return ret;
        }
        state->napt_oem_geometry_verified = true;
        state->napt_primary_level = primary;
        state->napt_secondary_level = secondary;
        state->napt_observed_memory_bytes = memory[1];
    }
    for (i = 0; i < HSAN_PFE_NAPT_TABLE_COUNT; ++i) {
        ret = hsan_lsw_shadow_table_create_sparse(dev,
                   hsan_pfe_napt_tables[i].id,
                   hsan_pfe_napt_tables[i].shadow_name, rows[i],
                   hsan_pfe_napt_tables[i].bytes_per_entry, 0);
        if (ret)
            return ret; /* rollback will delete any earlier table */
        state->napt_shadow_registered[i] = true;
        state->napt_observed_rows[i] = rows[i];
    }
    dev_info(dev, "OEM NAPT IPv4=%u IPv6=%u logical rows (lazy software shadow), geometry=%d levels=%u/%u; no HW forwarding\n",
             rows[0], rows[1], state->napt_oem_geometry_verified,
             state->napt_primary_level, state->napt_secondary_level);
    return 0;
}

static void hsan_pfe_release_napt_shadows(struct platform_device *pdev,
                                         struct hsan_pfe_state *state)
{
    int i;

    for (i = HSAN_PFE_NAPT_TABLE_COUNT - 1; i >= 0; --i) {
        int ret;

        if (!state->napt_shadow_registered[i])
            continue;
        ret = hsan_lsw_shadow_table_delete(&pdev->dev,
                                           hsan_pfe_napt_tables[i].id);
        if (ret)
            dev_warn(&pdev->dev, "NAPT shadow table %#x delete failed: %d\n",
                     hsan_pfe_napt_tables[i].id, ret);
        else
            state->napt_shadow_registered[i] = false;
    }
}

/* The first five NAPT init writes are provable from hi_pfe_napt_init
 * @0x6300..0x637c. Runtime memory partitioning (0x03000130/134/138/13c),
 * the 0x03000060 command/ACK protocol and actual table callbacks are NOT
 * complete. Opt-in submits only this fixed, non-replayable subset, preserving
 * NAPT uncertainty; it is explicitly NOT an activation or a stop sequence.
 */
static int hsan_pfe_preflight_napt_fixed(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    static const u32 addrs[] = {
        0x03000070U, 0x03000064U, 0x03000054U,
        0x03000168U, 0x03000170U,
    };
    struct hsan_dp_queue_snapshot q;
    u32 memory[2];
    unsigned int i;
    u64 end, total;
    int ret;

    if (!state->clock_on || !state->napt_shadow_registered[0] ||
        !state->napt_shadow_registered[1] ||
        !state->napt_observed_rows[0] || !state->napt_observed_rows[1] ||
        !state->napt_oem_geometry_verified)
        return -ENODATA;
    /* OEM hi_system_mem_info_get(2) supplies base/bytes dynamically.
     * This is a board observation contract, never guessed from DRAM size.
     * The software table row widths are a conservative minimum only.
     */
    if (of_property_count_u32_elems(pdev->dev.of_node,
            "hsan,observed-napt-runtime-memory") != 2)
        return -ENODATA;
    ret = of_property_read_u32_array(pdev->dev.of_node,
            "hsan,observed-napt-runtime-memory", memory, 2);
    if (ret)
        return ret;
    end = (u64)memory[0] + memory[1];
    total = (u64)state->napt_observed_rows[0] * 40U +
            (u64)state->napt_observed_rows[1] * 64U;
    if (!IS_ALIGNED(memory[0], SZ_4K) || !IS_ALIGNED(memory[1], SZ_4K) ||
        !memory[1] || end > (u64)U32_MAX + 1 || total > memory[1])
        return -ERANGE;
    ret = hsan_lsw_dp_get_queue(10, &q);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&q))
        return -ENODATA;
    for (i = 0; i < ARRAY_SIZE(addrs); ++i) {
        ret = hsan_lsw_check_span(addrs[i], 4, true);
        if (ret)
            return ret;
    }
    return 0;
}

static int hsan_pfe_program_napt_fixed(struct platform_device *pdev,
                                      struct hsan_pfe_state *state)
{
    int ret;

    if (!of_property_read_bool(pdev->dev.of_node,
                       "hsan,experimental-program-napt-fixed-prelude"))
        return 0;
    if (!state->global_writes_completed || !state->clock_on ||
        !state->mcc_write_stream_submitted)
        return -EHOSTDOWN;
    ret = hsan_pfe_preflight_napt_fixed(pdev, state);
    if (ret)
        return ret;
    if (atomic_cmpxchg(&hsan_pfe_napt_write_attempted, 0, 1))
        return -EALREADY;
    hsan_pfe_pin_hw_lifetime();
    state->napt_fixed_attempted = true;
    state->napt_hw_uncertain = true;
    ret = hsan_lsw_update_bits_verify(HSAN_PFE_NAPT_CONFIG_CSR,
                   HSAN_PFE_NAPT_CONFIG_MASK, HSAN_PFE_NAPT_CONFIG_VALUE);
    if (ret)
        goto err;
    /* 0x03000064 may be a transient command: do not demand a stable
     * readback from a potentially self-clearing register.
     */
    ret = hsan_lsw_write(0x03000064U, 1);
    if (ret)
        goto err;
    ret = hsan_lsw_write_verify(0x03000054U, 256);
    if (ret)
        goto err;
    ret = hsan_lsw_write_verify(0x03000168U, 65536);
    if (ret)
        goto err;
    ret = hsan_lsw_write_verify(0x03000170U, 38);
    if (ret)
        goto err;
    state->napt_fixed_submitted = true;
    dev_warn(&pdev->dev,
       "OEM NAPT 5-register fixed prelude submitted; runtime memory split, ACK, HW tables and NAT forwarding NOT ready\n");
    return 0;
err:
    dev_err(&pdev->dev,
       "OEM NAPT fixed prelude failed (%d); NAPT hardware uncertain, no replay/clock gating before reset\n", ret);
    return ret;
}

/* OEM hi_pfe_napt_init @0x6448..0x6524 uses runtime-calculated
 * address partitions and the two 0x03000060 commands in the CSR module.
 * These are not derivable from Linux DRAM or from the table-manager row
 * count. Require all FIVE already-encoded register values from the same
 * OEM board run, matching the independently observed memory geometry.
 *
 * The last word is 0x03000068: BIT16 | (secondary_level<<8) |
 * primary_level, as decoded at .text+0x64ac..0x64bc. It is separately
 * checked so that a swapped/partial device-tree array cannot arm NAPT.
 */
#define HSAN_PFE_NAPT_LAYOUT_WORDS 5U
static int hsan_pfe_preflight_napt_partition(struct platform_device *pdev,
                                             struct hsan_pfe_state *state)
{
    static const u32 csrs[] = {
        0x03000130U, 0x03000138U, 0x03000134U,
        0x0300013cU, 0x03000068U, 0x03000060U,
    };
    struct device *dev = &pdev->dev;
    u32 observed[HSAN_PFE_NAPT_LAYOUT_WORDS];
    u32 expected_control, memory[2];
    u64 slots, consumed, first_offset, second_offset;
    u64 expected[4], pool_end;
    unsigned int i;
    int ret;

    if (!state->clock_on || !state->napt_oem_geometry_verified ||
        !state->napt_shadow_registered[0] ||
        !state->napt_shadow_registered[1] ||
        !state->napt_primary_level || !state->napt_secondary_level)
        return -ENODATA;
    if (of_property_count_u32_elems(dev->of_node,
         "hsan,observed-napt-partition-register-words") !=
         HSAN_PFE_NAPT_LAYOUT_WORDS)
        return -ENODATA;
    ret = of_property_read_u32_array(dev->of_node,
         "hsan,observed-napt-partition-register-words",
         observed, HSAN_PFE_NAPT_LAYOUT_WORDS);
    if (ret)
        return ret;
    /* hi_pfe_napt_init @0x63fc..0x6654: the four partition words
     * are fully derivable from the separately observed memory base,
     * OEM-selected primary/secondary levels, and the first-stage cost.
     * Prior versions checked only 4-byte alignment and could accept an
     * out-of-range or overlapping NAPT RAM partition as an experiment.
     * The first word is the system memory base; the second points 8KiB
     * << (primary-1) into that pool; the third is the first-stage end;
     * the fourth points 8KiB << (secondary-1) past that end.
     * The primary==0 fallback never passes this experiment's gate.
     */
    ret = of_property_read_u32_array(dev->of_node,
                "hsan,observed-napt-runtime-memory", memory, 2);
    if (ret)
        return ret;
    if (memory[1] != state->napt_observed_memory_bytes ||
        !IS_ALIGNED(memory[0], SZ_4K) ||
        state->napt_primary_level > HSAN_PFE_NAPT_MAX_LEVEL ||
        state->napt_secondary_level > HSAN_PFE_NAPT_MAX_LEVEL)
        return -EINVAL;
    slots = 512ULL << (state->napt_primary_level - 1);
    consumed = 16ULL * slots +
               2048ULL * DIV_ROUND_UP_ULL(slots, 5);
    if (state->napt_primary_level == HSAN_PFE_NAPT_MAX_LEVEL &&
        consumed != HSAN_PFE_NAPT_OEM_FIXED_RESERVE)
        return -EUCLEAN;
    first_offset = 8192ULL << (state->napt_primary_level - 1);
    second_offset = 8192ULL << (state->napt_secondary_level - 1);
    pool_end = (u64)memory[0] + memory[1];
    expected[0] = memory[0];
    expected[1] = (u64)memory[0] + first_offset;
    expected[2] = (u64)memory[0] + consumed;
    expected[3] = expected[2] + second_offset;
    if (pool_end > (u64)U32_MAX + 1 ||
        expected[1] > expected[2] ||
        expected[3] > pool_end ||
        expected[3] > U32_MAX)
        return -ERANGE;
    for (i = 0; i < 4; ++i) {
        if (observed[i] != expected[i]) {
            dev_err(dev,
                "NAPT OEM partition word%u mismatch: observed=%#x derived=%#llx (no hardware writes)\n",
                i, observed[i], (unsigned long long)expected[i]);
            return -EINVAL;
        }
    }
    expected_control = BIT(16) |
             ((state->napt_secondary_level & 0xfU) << 8) |
             (state->napt_primary_level & 0xfU);
    if (observed[4] != expected_control)
        return -EINVAL;
    ret = hsan_lsw_napt_health();
    if (ret)
        return ret;
    for (i = 0; i < ARRAY_SIZE(csrs); ++i) {
        ret = hsan_lsw_check_span(csrs[i], sizeof(u32), true);
        if (ret)
            return ret;
    }
    state->napt_partition_layout_verified = true;
    return 0;
}

static int hsan_pfe_program_napt_partition(struct platform_device *pdev,
                                            struct hsan_pfe_state *state)
{
    u32 observed[HSAN_PFE_NAPT_LAYOUT_WORDS];
    u32 status = 0;
    int ret;

    if (!of_property_read_bool(pdev->dev.of_node,
            "hsan,experimental-program-napt-partition"))
        return 0;
    if (!state->napt_fixed_submitted ||
        !state->global_writes_completed ||
        !state->mcc_write_stream_submitted || state->mcc_hw_uncertain)
        return -EHOSTDOWN;
    ret = hsan_pfe_preflight_napt_partition(pdev, state);
    if (ret)
        return ret;
    ret = of_property_read_u32_array(pdev->dev.of_node,
         "hsan,observed-napt-partition-register-words",
         observed, HSAN_PFE_NAPT_LAYOUT_WORDS);
    if (ret)
        return ret;
    if (atomic_cmpxchg(&hsan_pfe_napt_partition_attempted, 0, 1))
        return -EALREADY;
    hsan_pfe_pin_hw_lifetime();
    state->napt_partition_attempted = true;
    state->napt_hw_uncertain = true;
    ret = hsan_lsw_napt_partition_commit(observed, &status);
    state->napt_partition_last_status = status;
    if (ret) {
        dev_err(&pdev->dev,
          "NAPT partition/dual command failed: %d status=%#x; no replay/reset assumed\n",
          ret, status);
        return ret;
    }
    state->napt_partition_commands = 2;
    dev_warn(&pdev->dev,
       "NAPT two OEM command bit0-clear waits completed; no HW table readback, NAT/forwarding NOT qualified\n");
    return 0;
}

/* The PA register prelude is one genuine OEM MMIO write, not PA ready.
 * The OEM enables it only after IFC. Our IFC 128-bit payload has no
 * audited readback/ACK yet, so a PA request MUST fail without touching
 * hardware until the IFC uncertain state can truly be cleared.
 */
static int hsan_pfe_preflight_pa(struct platform_device *pdev,
                                struct hsan_pfe_state *state)
{
    if (!state->clock_on || !state->pa_shadow_registered[0] ||
        !state->pa_shadow_registered[1])
        return -ENODATA;
    return hsan_lsw_check_span(HSAN_PFE_PA_ENABLE_CSR, 4, true);
}

static int hsan_pfe_program_pa_prelude(struct platform_device *pdev,
                                      struct hsan_pfe_state *state)
{
    int ret;

    if (!of_property_read_bool(pdev->dev.of_node,
                        "hsan,experimental-program-pa-prelude"))
        return 0;
    if (!state->global_writes_completed ||
        !state->ifc_hw_attempted ||
        state->ifc_rows_committed != HSAN_PFE_IFC_DEFAULT_ROWS ||
        state->ifc_hw_uncertain)
        return -EHOSTDOWN;
    ret = hsan_pfe_preflight_pa(pdev, state);
    if (ret)
        return ret;
    if (atomic_cmpxchg(&hsan_pfe_pa_write_attempted, 0, 1))
        return -EALREADY;
    state->pa_csr_attempted = true;
    state->pa_hw_uncertain = true;
    ret = hsan_lsw_write_verify(HSAN_PFE_PA_ENABLE_CSR, 1);
    if (ret) {
        dev_err(&pdev->dev, "PA CSR 0x07000704 write/readback failed: %d; no retry\n", ret);
        return ret;
    }
    state->pa_csr_verified = true;
    state->pa_hw_uncertain = false; /* This ONE register only. */
    dev_warn(&pdev->dev, "PA CSR prelude verified; PA table callbacks/forwarding NOT ready\n");
    return 0;
}

/* IFC table manager software owner is separate from the OEM bank0
 * command engine. The module only publishes this shadow when registration
 * succeeds; deletion never pretends to undo hardware IFC writes.
 */
static int hsan_pfe_prepare_ifc_shadow(struct platform_device *pdev,
                                       struct hsan_pfe_state *state)
{
    int ret = hsan_lsw_shadow_table_create(&pdev->dev,
            HSAN_PFE_IFC_TABLE_ID, "ifc_011b", HSAN_PFE_IFC_TABLE_ROWS,
            HSAN_PFE_IFC_TABLE_WIDTH, 0);

    if (!ret)
        state->ifc_shadow_registered = true;
    return ret;
}

static void hsan_pfe_release_ifc_shadow(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    int ret;

    if (!state->ifc_shadow_registered)
        return;
    ret = hsan_lsw_shadow_table_delete(&pdev->dev,
                                       HSAN_PFE_IFC_TABLE_ID);
    if (ret)
        dev_warn(&pdev->dev, "IFC table shadow delete failed: %d\n", ret);
    else
        state->ifc_shadow_registered = false;
}

/* These are POST-ENCODING OEM control/table words from a separately
 * observed board trace, not generated defaults. Every 11-rule request
 * must be complete, which avoids writing an incomplete default IFC map.
 * Write order and command protocol follow original ifc_dump +0x30c/+0x3d8.
 * No AUTO execution on a merely registered table-manager shadow.
 */
static int hsan_pfe_preflight_ifc(struct platform_device *pdev,
                                  struct hsan_pfe_state *state)
{
    struct device *dev = &pdev->dev;
    struct hsan_dp_queue_snapshot q;
    unsigned int i;
    u32 start;
    int ret;

    if (!state->clock_on || !state->ifc_shadow_registered ||
        !state->pub_shadow_registered[2])
        return -ENODATA;
    ret = hsan_lsw_ifc_health();
    if (ret)
        return ret;
    /* OEM hi_tab_mgr_alloc_hash(0x103, data, 12) allocates ONE tag
     * entry with 12 bytes of data, not twelve entries. Reserve that
     * index in Linux before the separately verified tag_write callback.
     */
    if (of_property_count_u32_elems(dev->of_node,
          "hsan,observed-ifc-hash-allocation") != 1)
        return -ENODATA;
    ret = of_property_read_u32(dev->of_node,
            "hsan,observed-ifc-hash-allocation", &start);
    if (ret)
        return ret;
    if (start > hsan_pfe_pub_tables[2].entries - HSAN_PFE_IFC_HASH_RESERVED_ROWS)
        return -ERANGE;
    if (state->ifc_hash_reserved) {
        if (state->ifc_hash_observed_start != start)
            return -ESTALE;
    } else {
        ret = hsan_lsw_shadow_table_reserve_range(dev,
                hsan_pfe_pub_tables[2].id, start,
                HSAN_PFE_IFC_HASH_RESERVED_ROWS);
        if (ret)
            return ret;
        state->ifc_hash_reserved = true;
        state->ifc_hash_observed_start = start;
        dev_info(dev,
                 "IFC hash rows %u..%u reserved in Linux table manager (software only)\n",
                 start, start + HSAN_PFE_IFC_HASH_RESERVED_ROWS - 1);
    }
    state->ifc_hash_range_checked = true;
    ret = hsan_lsw_check_span(0x0b054000U + start * 16U, 16, true);
    if (ret)
        return ret;
    if (of_property_count_u32_elems(dev->of_node,
          "hsan,observed-ifc-default-words") !=
          HSAN_PFE_IFC_DEFAULT_ROWS * HSAN_PFE_IFC_OBSERVED_WORDS)
        return -EINVAL;
    /* The OEM 11 x 15-word post-encoding image is immutable evidence.
     * Validate its explicit little-endian CRC *before* any PFE global
     * or IFC irreversible MMIO work, not midway through a command batch.
     * This requires a separate board-observed digest, not a generated
     * value that silently endorses arbitrary DT contents.
     */
    {
        __le32 image[HSAN_PFE_IFC_DEFAULT_ROWS * HSAN_PFE_IFC_OBSERVED_WORDS];
        u32 words[HSAN_PFE_IFC_DEFAULT_ROWS * HSAN_PFE_IFC_OBSERVED_WORDS];
        u32 expected, digest;

        ret = of_property_read_u32(dev->of_node,
                    "hsan,observed-ifc-default-crc32", &expected);
        if (ret)
            return -ENODATA;
        ret = of_property_read_u32_array(dev->of_node,
                    "hsan,observed-ifc-default-words", words,
                    ARRAY_SIZE(words));
        if (ret)
            return ret;
        for (i = 0; i < ARRAY_SIZE(words); i++)
            image[i] = cpu_to_le32(words[i]);
        digest = crc32_le(0, (u8 *)image, sizeof(image));
        if (expected != digest) {
            dev_err(dev, "IFC OEM image CRC mismatch: observed=%#x expected=%#x, no hardware commands\n",
                    digest, expected);
            return -EBADMSG;
        }
        state->ifc_observed_image_crc32 = digest;
        state->ifc_observed_image_verified = true;
    }
    ret = hsan_lsw_dp_get_queue(10, &q);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&q))
        return -ENODATA;
    ret = hsan_lsw_check_span(0x00000118U, 4, true);
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(0x00000120U, 4, true);
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(0x00000138U, 0x20U, true);
    if (ret)
        return ret;
    for (i = 0; i < HSAN_PFE_IFC_DEFAULT_ROWS; i++) {
        ret = hsan_lsw_check_span(0x00050000U + (i + 1) * 32U, 32U, true);
        if (ret)
            return ret;
    }
    return 0;
}

static int hsan_pfe_program_ifc_defaults(struct platform_device *pdev,
                                         struct hsan_pfe_state *state)
{
    struct device *dev = &pdev->dev;
    u32 *observed;
    unsigned int i;
    int ret;

    if (!of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-ifc-defaults"))
        return 0;
    /* A successful PFE prelude does not qualify FAM/forwarding, but the
     * IFC command may only follow these OEM predecessor steps.
     */
    if (!state->global_writes_completed || !state->opp_regs_hw_attempted)
        return -EHOSTDOWN;
    /* Basic CPU Ethernet uses the IFC default classification image without
     * activating the independent multicast/NAPT offload engines. The full
     * OEM profile retains its stronger predecessor checks.
     */
    if (!of_property_read_bool(dev->of_node,
                              "hsan,experimental-ifc-cpu-path") &&
        (!state->ofc_csr_verified || !state->mcc_write_stream_submitted ||
         state->mcc_hw_uncertain))
        return -EHOSTDOWN;
    ret = hsan_pfe_preflight_ifc(pdev, state);
    if (ret)
        return ret;
    observed = kcalloc(HSAN_PFE_IFC_DEFAULT_ROWS *
                       HSAN_PFE_IFC_OBSERVED_WORDS, sizeof(u32), GFP_KERNEL);
    if (!observed)
        return -ENOMEM;
    ret = of_property_read_u32_array(dev->of_node,
          "hsan,observed-ifc-default-words", observed,
          HSAN_PFE_IFC_DEFAULT_ROWS * HSAN_PFE_IFC_OBSERVED_WORDS);
    if (ret)
        goto out;
    /* Compare again after the allocation and command preflight so a
     * revised DT-backed image cannot be submitted under an old digest.
     */
    {
        __le32 image[HSAN_PFE_IFC_DEFAULT_ROWS * HSAN_PFE_IFC_OBSERVED_WORDS];
        unsigned int j;

        for (j = 0; j < ARRAY_SIZE(image); j++)
            image[j] = cpu_to_le32(observed[j]);
        if (!state->ifc_observed_image_verified ||
            crc32_le(0, (u8 *)image, sizeof(image)) !=
                state->ifc_observed_image_crc32) {
            ret = -ESTALE;
            goto out;
        }
    }
    if (atomic_cmpxchg(&hsan_pfe_ifc_write_attempted, 0, 1)) {
        ret = -EALREADY;
        goto out;
    }
    state->ifc_hw_attempted = true;
    state->ifc_hw_uncertain = true;
    /* OEM allocation data {0, 0, 8} invokes tag_write @3e1c.
     * Its actual A32 callback emits this complete 128-bit image.
     */
    {
        const u32 tag[4] = { 0, 0x0000c000U, 0, 0 };
        u32 actual[4];

        ret = hsan_lsw_write_tab128(0x0b054000U,
                                   state->ifc_hash_observed_start, tag);
        if (ret)
            goto out;
        ret = hsan_lsw_read_tab128(0x0b054000U,
                                  state->ifc_hash_observed_start, actual);
        if (ret)
            goto out;
        if (memcmp(actual, tag, sizeof(tag))) {
            ret = -EUCLEAN;
            goto out;
        }
        state->ifc_tag_verified = true;
        dev_info(dev, "IFC OEM tag callback image verified: index=%u bytes=12 entries=1\n",
                 state->ifc_hash_observed_start);
    }
    for (i = 0; i < HSAN_PFE_IFC_DEFAULT_ROWS; i++) {
        u32 *row = observed + i * HSAN_PFE_IFC_OBSERVED_WORDS;

        ret = hsan_pfe_fam_fence_guard(state, 100U + i);
        if (ret)
            goto out;
        ret = hsan_lsw_ifc_commit(i + 1, row,
                       row + HSAN_PFE_IFC_CONTROL_WORDS,
                       &state->ifc_last_status);
        if (ret)
            goto out;
        state->ifc_rows_committed++;
        /* The CSR call compares both 128-bit halves in the same MMIO
         * window before it succeeds. This is only a readback sample.
         */
        state->ifc_rows_window_readback++;
    }
    /* OEM hi_pfe_ifc_init @d4c..d54 finishes with CSR[0x118] = 0. */
    ret = hsan_lsw_write_verify(0x00000118U, 0);
    if (ret)
        goto out;
    /* The CPU-visible two-half MMIO window was read back for every
     * row, but the IFC hardware table *activation* and forwarding-state
     * read/ACK protocol remain unqualified. Permanently quarantine the
     * IFC engine; do not infer correct lookup from the MMIO comparison.
     */
    ret = hsan_lsw_ifc_quarantine();
    if (ret)
        goto out;
    state->ifc_engine_quarantined = true;
    dev_warn(dev, "IFC 11 commands accepted; engine quarantined until reboot because table backing/forwarding is unverified\n");
out:
    if (ret && state->ifc_hw_attempted) {
        state->ifc_engine_quarantined =
            hsan_lsw_ifc_health() == -EUCLEAN;
        dev_err(dev, "IFC HW uncertain after %u/%u rows, status=%#x error=%d; do not retry\n",
                state->ifc_rows_committed, HSAN_PFE_IFC_DEFAULT_ROWS,
                state->ifc_last_status, ret);
    }
    kfree(observed);
    return ret;
}

/* hi_pfe_mcc_exit @0x6d20 deletes IDs 0x10c, 0x10a and 0x800.
 * Reverse-order cleanup is for host software tables only; hardware writes
 * in hi_pfe_mcc_init are NOT reversible through this API.
 */
static void hsan_pfe_release_mcc_shadows(struct platform_device *pdev,
                                         struct hsan_pfe_state *state)
{
    int i;

    for (i = HSAN_PFE_MCC_TABLE_COUNT - 1; i >= 0; --i) {
        int ret;

        if (!state->mcc_shadow_registered[i])
            continue;
        ret = hsan_lsw_shadow_table_delete(&pdev->dev,
                                           hsan_pfe_mcc_tables[i].id);
        if (ret)
            dev_warn(&pdev->dev, "MCC shadow %#x delete failed: %d\n",
                     hsan_pfe_mcc_tables[i].id, ret);
        else
            state->mcc_shadow_registered[i] = false;
    }
}

static int hsan_pfe_prepare_mcc_shadows(struct platform_device *pdev,
                                        struct hsan_pfe_state *state)
{
    unsigned int i;
    int ret;

    for (i = 0; i < HSAN_PFE_MCC_TABLE_COUNT; ++i) {
        const struct hsan_pfe_pub_table_desc *t = &hsan_pfe_mcc_tables[i];

        ret = hsan_lsw_shadow_table_create(&pdev->dev, t->id,
                          t->shadow_name, t->entries, t->bytes_per_entry, 0);
        if (ret) {
            hsan_pfe_release_mcc_shadows(pdev, state);
            return ret;
        }
        state->mcc_shadow_registered[i] = true;
    }
    return 0;
}

/* Verified OEM MCC instructions @0x6c5c..0x6d08:
 *  0x05055004 + 4*(i-1) = 0x200|i, i=1..255
 *  0x0500000c=0; 0x0200009c=17;
 *  bits[18:16] of 0x02000094 and 0x02000098 = 2;
 *  0x02000090=1. Preflight *all* resources before first write.
 * The command chain includes no proven HW reset/rollback and must not run
 * merely because MCC software table owners were registered.
 */
static int hsan_pfe_preflight_mcc_hw(struct device *dev)
{
    static const u32 one_word[] = {
        0x0500000cU, 0x0200009cU, 0x02000094U,
        0x02000098U, 0x02000090U,
    };
    struct hsan_dp_queue_snapshot queue;
    unsigned int i;
    int ret;

    ret = hsan_lsw_dp_get_queue(10, &queue);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&queue))
        return -ENODATA;
    ret = hsan_lsw_eqm_health();
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(0x05055004U, 255U * sizeof(u32), true);
    if (ret)
        return ret;
    for (i = 0; i < ARRAY_SIZE(one_word); ++i) {
        ret = hsan_lsw_check_span(one_word[i], sizeof(u32), true);
        if (ret)
            return ret;
    }
    dev_dbg(dev, "OEM MCC init register program preflight succeeded (no writes)\n");
    return 0;
}

static int hsan_pfe_program_mcc_hw(struct platform_device *pdev,
                                   struct hsan_pfe_state *state)
{
    struct device *dev = &pdev->dev;
    u32 i;
    int ret;

    if (!of_property_read_bool(dev->of_node,
                              "hsan,experimental-program-mcc-registers"))
        return 0;
    if (!state->clock_on || !state->global_writes_completed)
        return -EHOSTDOWN;
    for (i = 0; i < HSAN_PFE_MCC_TABLE_COUNT; i++)
        if (!state->mcc_shadow_registered[i])
            return -ENODATA;
    ret = hsan_pfe_preflight_mcc_hw(dev);
    if (ret)
        return ret;
    if (atomic_cmpxchg(&hsan_pfe_mcc_write_attempted, 0, 1))
        return -EALREADY;
    state->mcc_hw_attempted = true;
    state->mcc_hw_uncertain = true;
    /* OEM MCC stream is one bank2+bank5 transaction. Generic writes
     * cannot interleave between 255 rows and the control-field updates.
     * The CSR owner retains the one-shot latch after the first write;
     * CPU write completion does not establish a working MCC datapath.
     */
    ret = hsan_lsw_mcc_commit(&state->mcc_hw_rows_written);
    if (ret)
        goto fail;
    /* A posted MMIO write is not proof that the complete MCC forwarding
     * subsystem has a functional table manager/IRQ. No PFE ready claim.
     */
    state->mcc_write_stream_submitted = true;
    /* No OEM readback/acknowledgement has been qualified for the MCC
     * write-only sequence. Preserve uncertainty even after all writes
     * have been submitted to the CPU bus.
     */
    dev_warn(dev, "MCC OEM 255-row register prelude submitted; hardware ACK and other PFE subsystems not ready\n");
    return 0;
fail:
    dev_err(dev, "MCC hardware programming interrupted after %u/255 rows (err=%d); restart forbidden\n",
            state->mcc_hw_rows_written, ret);
    return ret;
}

/* OEM hi_pfe_opp_init @.text+0x3150..0x31b8 performs these three
 * CSR read/modify/write operations after creating OPP IDs 0x10d/0x112,
 * before iterating queue-derived per-port control. The third operation
 * ORs bits 7,5,3. Apply only the exact observed fields, atomically with
 * other bank owners, and independently verify the readback of each mask.
 * A successful prelude is NOT the missing per-port OPP state machine.
 */
static int hsan_pfe_opp_register_prelude(struct platform_device *pdev,
					struct hsan_pfe_state *state)
{
	static const struct { u32 csr, mask, value; } fields[] = {
		{ 0x060000b0U, BIT(1), BIT(1) },
		{ 0x060001b4U, GENMASK(7, 0), 0x21U },
		{ 0x060000c0U, 0x000000a8U, 0x000000a8U },
	};
	struct hsan_dp_queue_snapshot queue;
	unsigned int i;
	u32 actual;
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				"hsan,experimental-program-opp-prelude"))
		return 0;
	if (!state->clock_on || !state->global_writes_attempted ||
	    !state->opp_shadow_registered[0] ||
	    !state->opp_shadow_registered[1])
		return -EPERM;
	ret = hsan_lsw_dp_get_queue(10, &queue);
	if (ret)
		return ret;
	if (!queue.reserved_memory_verified ||
	    !queue.fam_addresses_prepared ||
        !queue.fam_cpu_table_verified || !queue.fam_dma_attrs_zero ||
        !queue.fam_oem_runtime_geometry_verified ||
        (!queue.fam_hw_owned || !queue.fam_hw_ack_seen) ||
	    !queue.eqm_bootstrap_committed ||
	    !queue.eqm_queue_rows_committed ||
        !queue.pie_map_hw_committed || queue.pie_map_hw_uncertain)
		return -ENODATA;
	for (i = 0; i < ARRAY_SIZE(fields); ++i) {
		ret = hsan_lsw_check_span(fields[i].csr, sizeof(u32), true);
		if (ret)
			return ret;
	}
	/* Partial global writes are not software-reversible, and retry is
	 * deliberately forbidden after the first attempted hardware field.
	 */
	state->opp_regs_hw_attempted = true;
	for (i = 0; i < ARRAY_SIZE(fields); ++i) {
		ret = hsan_lsw_update_bits_verify(fields[i].csr, fields[i].mask,
					   fields[i].value);
		if (ret)
			goto failed;
		ret = hsan_lsw_read(fields[i].csr, &actual);
		if (ret)
			goto failed;
		if ((actual & fields[i].mask) != fields[i].value) {
			ret = -EUCLEAN;
			goto failed;
		}
	}
	dev_warn(&pdev->dev, "OPP three OEM global CSR fields verified; per-port OPP still missing\n");
	return 0;
failed:
	dev_err(&pdev->dev,
		"OPP register prelude failed after an irreversible attempt (field=%u error=%d); no replay\n",
		i, ret);
	return ret;
}

/* OEM hi_lsw_pfe:tab_write_tfrt @.text+0x2ff0 is a genuine
 * hi_tab_mgr callback (table 0x10d), rather than a shadow-only owner:
 *   logical CSR = (0x01811200 + row) << 2;
 *   encoded value = (input & GENMASK(19,0)) | BIT(20).
 * Only rows [0,15] belong to the 16-entry OEM table. The other OPP table
 * (0x112) has a no-op write callback and must NOT be given invented MMIO.
 * Hardware opt-in additionally requires complete observed DP queue handoff;
 * even a verified TFRT row does NOT establish a working PFE pipeline.
 */
#define HSAN_PFE_TFRT_CSR_BASE 0x06044800U
#define HSAN_PFE_TFRT_MAX_ROWS 16U
#define HSAN_PFE_TFRT_VALUE_MASK GENMASK(19, 0)

struct hsan_pfe_tfrt_row {
	u32 row;
	u32 value;
};

static int hsan_pfe_program_tfrt(struct platform_device *pdev,
				struct hsan_pfe_state *state)
{
	struct hsan_dp_queue_snapshot queue;
	struct hsan_pfe_tfrt_row rows[HSAN_PFE_TFRT_MAX_ROWS] = { };
	struct device *dev = &pdev->dev;
	unsigned int i, j, n;
	int cells, ret;

	if (!of_property_read_bool(dev->of_node,
				"hsan,experimental-program-opp-tfrt"))
		return 0;
	if (!state->clock_on || !state->global_writes_attempted ||
	    !state->opp_regs_hw_attempted || !state->opp_shadow_registered[0])
		return -EPERM;
	/* Link to the real DP platform owner, not a guess about port10/PHY3. */
	ret = hsan_lsw_dp_get_queue(10, &queue);
	if (ret)
		return ret;
	if (!queue.reserved_memory_verified ||
	    !queue.fam_addresses_prepared ||
        !queue.fam_cpu_table_verified || !queue.fam_dma_attrs_zero ||
        !queue.fam_oem_runtime_geometry_verified ||
        (!queue.fam_hw_owned || !queue.fam_hw_ack_seen) ||
	    !queue.eqm_bootstrap_committed ||
	    !queue.eqm_queue_rows_committed ||
        !queue.pie_map_hw_committed || queue.pie_map_hw_uncertain)
		return -ENODATA;
	cells = of_property_count_u32_elems(dev->of_node,
				"hsan,observed-opp-tfrt-entries");
	if (cells < 2 || cells > 2 * HSAN_PFE_TFRT_MAX_ROWS || (cells & 1))
		return -EINVAL;
	n = cells / 2;
	/* The full bounded scratch fits locally; do not allocate after
	 * the irreversible global/prelude stages.
	 */
	ret = of_property_read_u32_array(dev->of_node,
			"hsan,observed-opp-tfrt-entries", (u32 *)rows, cells);
	if (ret)
		return ret;
	/* Preflight the ENTIRE row stream before any irreversible MMIO. */
	for (i = 0; i < n; i++) {
		if (rows[i].row >= HSAN_PFE_TFRT_MAX_ROWS ||
		    (rows[i].value & ~HSAN_PFE_TFRT_VALUE_MASK))
			return -ERANGE;
		for (j = 0; j < i; j++)
			if (rows[i].row == rows[j].row)
				return -EEXIST;
		ret = hsan_lsw_check_span(HSAN_PFE_TFRT_CSR_BASE +
					 4 * rows[i].row, 4, true);
		if (ret)
			return ret;
	}
	for (i = 0; i < n; i++) {
		u32 csr = HSAN_PFE_TFRT_CSR_BASE + 4 * rows[i].row;
		u32 encoded = rows[i].value | BIT(20);
		u32 readback;

		/* The original callback is a FULL one-word write, not RMW. */
		state->opp_tfrt_hw_uncertain = true;
		ret = hsan_lsw_write(csr, encoded);
		if (ret)
			goto failed;
		ret = hsan_lsw_read(csr, &readback);
		if (ret)
			goto failed;
		if ((readback & (HSAN_PFE_TFRT_VALUE_MASK | BIT(20))) != encoded) {
			ret = -EUCLEAN;
			goto failed;
		}
		ret = hsan_lsw_shadow_table_write(dev, 0x10dU,
					 rows[i].row, &rows[i].value, sizeof(u32));
		if (ret)
			goto failed;
		state->opp_tfrt_hw_committed++;
	}
	state->opp_tfrt_hw_uncertain = false;
	dev_warn(dev, "OPP TFRT %u OEM callback rows written/read back; remaining PFE route incomplete\n", n);
	return 0;
failed:
	dev_err(dev, "OPP TFRT hardware uncertain after %u/%u rows (error=%d), no automatic retry or SW rollback\n",
		state->opp_tfrt_hw_committed, n, ret);
	return ret;
}

/* Stage31: full PFE opt-in preflight before the first global CSR
 * write. The previous probe wrote global tables before discovering that
 * PUB GRO/DP ownership or the OPP TFRT request was inconsistent.
 */
/* OEM hi_pfe_opp_init @3314 and its helper @301c map port <=15 to
 * 0x06042800 + (port * 8 + priority) * 4, value BIT(8) | qid.
 * This is the LAN1 queue route, independent of dynamic TFRT offload.
 */
static int hsan_pfe_lan1_opp(struct platform_device *pdev, bool submit)
{
	struct hsan_dp_queue_snapshot queue;
	u32 priority;
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-program-lan1-opp"))
		return 0;
	if (!of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-program-opp-prelude"))
		return -EINVAL;
	ret = hsan_lsw_dp_get_queue(HSAN_PFE_LAN1_PORT, &queue);
	if (ret)
		return ret;
	if (!hsan_pfe_dp_lan1_handoff(&queue) || queue.count != 8 ||
	    queue.qid > U8_MAX - 7)
		return -ENODATA;
	ret = hsan_lsw_check_span(0x06042940U, 8 * sizeof(u32), true);
	if (ret || !submit)
		return ret;
	for (priority = 0; priority < 8; priority++) {
		ret = hsan_lsw_write_verify(0x06042940U + priority * 4,
					    BIT(8) | (queue.qid + priority));
		if (ret)
			return ret;
	}
	dev_info(&pdev->dev,
		 "LAN1 OPP queue map verified: port=10 priorities=8 qid=%u..%u\n",
		 queue.qid, queue.qid + 7);
	return 0;
}

/* OEM port_config_write @72e8 updates word0.bit16 (VLAN check)
 * and word1.bits13:12 (SMAC check). port_vlan_write @3998 sets
 * valid and the port membership bit at 0x0b040000 + VID * 4.
 * The original runtime port_vlan dump explicitly records LAN1 VLAN0.
 */
static int hsan_pfe_lan1_pub(struct platform_device *pdev, bool submit)
{
	static const u32 ports[] = { 0, HSAN_PFE_LAN1_PORT };
	u32 words[4], observed[4], vid;
	unsigned int i;
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-program-lan1-pub"))
		return 0;
	ret = of_property_read_u32(pdev->dev.of_node,
				  "hsan,observed-lan1-vlan-id", &vid);
	if (ret || vid != 0)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		ret = hsan_lsw_check_span(0x07040000U + ports[i] * 16, 16, true);
		if (ret)
			return ret;
	}
	ret = hsan_lsw_check_span(0x0b040000U, 4, true);
	if (ret || !submit)
		return ret;
	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		ret = hsan_lsw_read_tab128(0x07040000U, ports[i], words);
		if (ret)
			return ret;
		words[0] |= BIT(16);
		words[1] &= ~GENMASK(13, 12);
		ret = hsan_lsw_write_tab128(0x07040000U, ports[i], words);
		if (ret)
			return ret;
		ret = hsan_lsw_read_tab128(0x07040000U, ports[i], observed);
		if (ret)
			return ret;
		if (memcmp(words, observed, sizeof(words)))
			return -EUCLEAN;
	}
	ret = hsan_lsw_update_bits_verify(0x0b040000U,
					 BIT(16) | BIT(10) | BIT(0),
					 BIT(16) | BIT(10) | BIT(0));
	if (ret)
		return ret;
	dev_info(&pdev->dev,
		 "LAN1 PUB config verified: ports=0,10 VLAN=0 membership=0x401\n");
	return 0;
}

static int hsan_pfe_preflight_hw_plan(struct platform_device *pdev,
                                     bool globals)
{
    struct device *dev = &pdev->dev;
    struct hsan_dp_queue_snapshot queue;
    struct hsan_pfe_tfrt_row rows[HSAN_PFE_TFRT_MAX_ROWS] = { };
    bool opp = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-opp-prelude");
    bool tfrt = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-opp-tfrt");
    bool mcc = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-mcc-registers");
    bool ofc = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-ofc-control");
    bool ifc = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-ifc-defaults");
    bool cpu_ifc = of_property_read_bool(dev->of_node,
                         "hsan,experimental-ifc-cpu-path");
    bool pa = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-pa-prelude");
    bool napt = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-napt-fixed-prelude");
    bool napt_partition = of_property_read_bool(dev->of_node,
                         "hsan,experimental-program-napt-partition");
    unsigned int i, j, count;
    int cells, ret;

    if (globals) {
        ret = hsan_pfe_global_csrs(false);
        if (ret)
            return ret;
    }
    if (of_property_read_bool(dev->of_node,
                             "hsan,experimental-program-lan1-pub")) {
        if (!globals)
            return -EINVAL;
        ret = hsan_pfe_lan1_pub(pdev, false);
        if (ret)
            return ret;
    }
    if (of_property_read_bool(dev->of_node,
                             "hsan,experimental-program-lan1-opp")) {
        if (!globals || !opp)
            return -EINVAL;
        ret = hsan_pfe_lan1_opp(pdev, false);
        if (ret)
            return ret;
    }
    ret = hsan_pfe_preflight_l3intf(pdev, platform_get_drvdata(pdev),
                                    globals);
    if (ret)
        return ret;
    if (!opp && !tfrt && !mcc && !ofc && !ifc && !pa &&
        !napt && !napt_partition)
        return 0;
    if (!globals || (tfrt && !opp) || (ofc && !opp) ||
        (napt && !mcc) || (napt_partition && !napt) ||
        /* OEM probe calls NAPT before IFC: never let IFC bypass even
         * the verified memory geometry and separately gated NAPT stage. */
        (ifc && !cpu_ifc && (!opp || !tfrt || !mcc || !ofc || !napt)) ||
        (pa && !ifc))
        return -EINVAL;
    if (cpu_ifc && (!ifc || !opp || !globals || pa ||
        !of_property_read_bool(dev->of_node,
                              "hsan,experimental-program-lan1-opp") ||
        !of_property_read_bool(dev->of_node,
                              "hsan,experimental-program-lan1-pub")))
        return -EINVAL;
    if (pa) {
        struct hsan_pfe_state *state = platform_get_drvdata(pdev);

        ret = hsan_pfe_preflight_pa(pdev, state);
        if (ret)
            return ret;
        /* The IFC command path has no audited 128-bit table readback.
         * Its uncertainty remains set even on a successful command.
         * Reject PA opt-in BEFORE PFE global MMIO or IFC side effects;
         * otherwise this necessarily fails after irreversible writes.
         */
        dev_err(dev, "PA prelude refused before hardware writes: IFC table ACK/ownership not qualified\n");
        return -EOPNOTSUPP;
    }
    if (napt) {
        struct hsan_pfe_state *state = platform_get_drvdata(pdev);

        ret = hsan_pfe_preflight_napt_fixed(pdev, state);
        if (ret)
            return ret;
        if (napt_partition) {
            ret = hsan_pfe_preflight_napt_partition(pdev, state);
            if (ret)
                return ret;
        }
    }
    if (ifc) {
        struct hsan_pfe_state *state = platform_get_drvdata(pdev);

        ret = hsan_pfe_preflight_ifc(pdev, state);
        if (ret)
            return ret;
    }
    if (ofc) {
        struct hsan_pfe_state *state = platform_get_drvdata(pdev);

        ret = hsan_pfe_preflight_ofc_control(dev, state);
        if (ret)
            return ret;
    }
    if (mcc) {
        ret = hsan_pfe_preflight_mcc_hw(dev);
        if (ret)
            return ret;
    }
    if (!opp && !tfrt)
        return 0;
    ret = hsan_lsw_dp_get_queue(10, &queue);
    if (ret)
        return ret;
    if (!hsan_pfe_dp_lan1_handoff(&queue))
        return -ENODATA;
    ret = hsan_lsw_check_span(0x060000b0U, 4, true);
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(0x060001b4U, 4, true);
    if (ret)
        return ret;
    ret = hsan_lsw_check_span(0x060000c0U, 4, true);
    if (ret || !tfrt)
        return ret;
    cells = of_property_count_u32_elems(dev->of_node,
                         "hsan,observed-opp-tfrt-entries");
    if (cells < 2 || cells > 2 * HSAN_PFE_TFRT_MAX_ROWS || (cells & 1))
        return -EINVAL;
    count = cells / 2;
    ret = of_property_read_u32_array(dev->of_node,
                    "hsan,observed-opp-tfrt-entries", (u32 *)rows, cells);
    if (ret)
        return ret;
    for (i = 0; i < count; i++) {
        if (rows[i].row >= HSAN_PFE_TFRT_MAX_ROWS ||
            (rows[i].value & ~HSAN_PFE_TFRT_VALUE_MASK))
            return -ERANGE;
        for (j = 0; j < i; j++)
            if (rows[i].row == rows[j].row)
                return -EEXIST;
        ret = hsan_lsw_check_span(HSAN_PFE_TFRT_CSR_BASE +
                                  rows[i].row * 4, 4, true);
        if (ret)
            return ret;
    }
    return 0;
}

static int hsan_pfe_probe_locked(struct platform_device *pdev)
{
	struct hsan_pfe_state *state;
	struct hsan_dp_init_snapshot dp;
	bool request_globals;
	u32 hwver;
	int ret;

	/* hsan_pfe_probe_lock reserves the singleton before any mapping or
	 * writes, including a previously quarantined but still bound owner.
	 */
	mutex_lock(&hsan_pfe_owner_lock);
	ret = hsan_pfe_owner ? -EBUSY : 0;
	mutex_unlock(&hsan_pfe_owner_lock);
	if (ret)
		return ret;
	if (of_property_read_u32(pdev->dev.of_node, "hwver", &hwver) ||
	    hwver != 0x300)
		return -EINVAL;
	request_globals = of_property_read_bool(pdev->dev.of_node,
				 "hsan,experimental-program-global-registers");
	/* DP boot reset also resets PFE. Wait before allocating or touching
	 * hardware so a later DP probe cannot silently erase our writes.
	 */
	if (request_globals) {
		ret = hsan_lsw_dp_init_snapshot(&dp);
		if (ret == -ENODEV)
			return -EPROBE_DEFER;
		if (ret)
			return ret;
		if (dp.quarantined || dp.last_errno)
			return -EUCLEAN;
		if (!dp.boot_reset_completed ||
		    dp.stage != HSAN_DP_INIT_PLAN_COMPLETE ||
		    !dp.queue_routing_verified || !dp.pie_map_geometry_valid)
			return -ENODATA;
	}
	state = devm_kzalloc(&pdev->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
    state->owner = &pdev->dev;
	platform_set_drvdata(pdev, state);
	/* OEM hi_pfe_probe uses clk_prepare + clk_enable before CSR init.
	 * Enable only on explicit opt-in, and undo on each error/exit.
	 */
	if (of_property_read_bool(pdev->dev.of_node,
				  "hsan,experimental-enable-pfe-clock")) {
		state->clk = devm_clk_get(&pdev->dev, NULL);
		if (IS_ERR(state->clk))
			return PTR_ERR(state->clk);
		ret = clk_prepare_enable(state->clk);
		if (ret)
			return ret;
		state->clock_on = true;
	}
	ret = hsan_lsw_map_windows(pdev, &hsan_pfe_cfg);
	if (ret)
		goto clocks;
    /* Reversible shadow setup belongs ahead of all global MMIO. */
    ret = hsan_pfe_prepare_global_attr_shadow(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_pub_shadows(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_plan_observed_l3intf(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_observe_l3intf_hw(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_observe_lan1_vlan(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_opp_shadows(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_ofc_shadows(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_mcc_shadows(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_napt_shadows(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_aux_tables(pdev, hsan_pfe_service_tables,
            state->service_shadow_registered, HSAN_PFE_SERVICE_TABLE_COUNT);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_ifc_shadow(pdev, state);
    if (ret)
        goto rollback;
    ret = hsan_pfe_prepare_aux_tables(pdev, hsan_pfe_pa_tables,
            state->pa_shadow_registered, HSAN_PFE_PA_TABLE_COUNT);
    if (ret)
        goto rollback;
    if (request_globals && !state->clock_on) {
        dev_err(&pdev->dev, "PFE register writes need an explicitly enabled clock\n");
        ret = -EPERM;
        goto rollback;
    }
    ret = hsan_pfe_preflight_hw_plan(pdev, request_globals);
    if (ret) {
        dev_err(&pdev->dev,
                "PFE hardware plan rejected before first global write: %d\n",
                ret);
        goto rollback;
    }
	if (request_globals) {
        state->probe_step = 1;
        ret = hsan_pfe_fam_fence_guard(state, 1);
        if (ret)
            goto rollback;
		if (atomic_cmpxchg(&hsan_pfe_global_write_attempted, 0, 1)) {
			ret = -EALREADY;
			dev_err(&pdev->dev, "PFE HW global write already attempted this module lifetime; refusing repeat\n");
			goto rollback;
		}
		state->global_writes_attempted = true;
        hsan_pfe_pin_hw_lifetime();
		ret = hsan_pfe_global_csrs(true);
		if (ret) {
			dev_err(&pdev->dev, "PFE CSR preflight/init failed %d; no readiness\n", ret);
			goto rollback;
		}
        state->global_writes_completed = true;
		dev_warn(&pdev->dev,
			 "PFE 14+48 tables & globals written; table manager and routing NOT ready\n");
	}
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 2);
        if (ret)
            goto rollback;
    }
    state->probe_step = 2;
	ret = hsan_pfe_opp_register_prelude(pdev, state);
	if (ret)
		goto rollback;
	ret = hsan_pfe_lan1_opp(pdev, true);
	if (ret)
		goto rollback;
    state->lan1_opp_verified = of_property_read_bool(pdev->dev.of_node,
                                  "hsan,experimental-program-lan1-opp");
	ret = hsan_pfe_lan1_pub(pdev, true);
	if (ret)
		goto rollback;
    state->lan1_pub_verified = of_property_read_bool(pdev->dev.of_node,
                                  "hsan,experimental-program-lan1-pub");
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 3);
        if (ret)
            goto rollback;
    }
    state->probe_step = 3;
	ret = hsan_pfe_program_tfrt(pdev, state);
	if (ret)
		goto rollback;
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 4);
        if (ret)
            goto rollback;
    }
    state->probe_step = 4;
    ret = hsan_pfe_program_ofc_control(pdev, state);
    if (ret)
        goto rollback;
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 5);
        if (ret)
            goto rollback;
    }
    state->probe_step = 5;
    ret = hsan_pfe_program_mcc_hw(pdev, state);
    if (ret)
        goto rollback;
    /* Same subsystem order as OEM hi_pfe_probe @0x2e80..0x2ecc. */
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 6);
        if (ret)
            goto rollback;
    }
    state->probe_step = 6;
    ret = hsan_pfe_program_napt_fixed(pdev, state);
    if (ret)
        goto rollback;
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 7);
        if (ret)
            goto rollback;
    }
    state->probe_step = 7;
    ret = hsan_pfe_program_napt_partition(pdev, state);
    if (ret)
        goto rollback;
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 8);
        if (ret)
            goto rollback;
    }
    state->probe_step = 8;
    ret = hsan_pfe_program_ifc_defaults(pdev, state);
    if (ret)
        goto rollback;
    if (request_globals) {
        ret = hsan_pfe_fam_fence_guard(state, 9);
        if (ret)
            goto rollback;
    }
    state->probe_step = 9;
    ret = hsan_pfe_program_pa_prelude(pdev, state);
    if (ret)
        goto rollback;
    state->probe_step = 10;
    ret = hsan_pfe_commit_l3intf_row(pdev, state);
    if (ret) {
        state->l3intf_write_errno = -ret;
        goto rollback;
    }
    mutex_lock(&hsan_pfe_owner_lock);
    hsan_pfe_owner = state;
    mutex_unlock(&hsan_pfe_owner_lock);
	dev_warn(&pdev->dev,
		 "PFE banks mapped; PUB %u + OPP two software table owners + global 0x10f; forwarding not qualified\n",
         HSAN_PFE_PUB_TABLE_COUNT - (state->pub_gro_entries ? 0U : 1U));
	return 0;
rollback:
    /* Returning a probe error releases ALL devm mappings and the state.
     * A retained module/clock alone cannot prevent that. After an MMIO
     * attempt bind in quarantine instead: keep the complete owner and
     * expose the original failure, while fencing subsequent CPU writes.
     */
    if (hsan_pfe_hardware_attempted(state)) {
        state->probe_quarantined = true;
        state->bindings_retained = true;
        state->probe_errno = ret < 0 ? (u32)-ret : EIO;
        if (state->fam_fence_abort_latched)
            state->probe_step = state->fam_fence_abort_step;
        hsan_pfe_pin_hw_lifetime();
        hsan_lsw_dp_fam_stop_fence();
        mutex_lock(&hsan_pfe_owner_lock);
        hsan_pfe_owner = state;
        mutex_unlock(&hsan_pfe_owner_lock);
        dev_err(&pdev->dev,
                "PFE init failed at step %u errno=%u: bound in quarantine, MMIO/clock/tables retained; forwarding disabled\n",
                state->probe_step, state->probe_errno);
        return 0;
    }
    if (state->ifc_hash_reserved) {
        int hash_ret = hsan_lsw_shadow_table_release_range(&pdev->dev,
                hsan_pfe_pub_tables[2].id, state->ifc_hash_observed_start,
                HSAN_PFE_IFC_HASH_RESERVED_ROWS);

        if (hash_ret)
            dev_warn(&pdev->dev, "IFC hash software reservation release failed: %d\n", hash_ret);
        else
            state->ifc_hash_reserved = false;
    }
    hsan_pfe_release_aux_tables(pdev, hsan_pfe_pa_tables,
            state->pa_shadow_registered, HSAN_PFE_PA_TABLE_COUNT);
    hsan_pfe_release_ifc_shadow(pdev, state);
    hsan_pfe_release_aux_tables(pdev, hsan_pfe_service_tables,
            state->service_shadow_registered, HSAN_PFE_SERVICE_TABLE_COUNT);
    hsan_pfe_release_napt_shadows(pdev, state);
    hsan_pfe_release_mcc_shadows(pdev, state);
    hsan_pfe_release_ofc_shadows(pdev, state);
	hsan_pfe_release_opp_shadows(pdev, state);
	hsan_pfe_release_pub_shadows(pdev, state);
	hsan_pfe_release_global_attr_shadow(pdev, state);
	hsan_lsw_unmap_windows(pdev, &hsan_pfe_cfg);
clocks:
    if (state->clock_on) {
        if (atomic_read(&hsan_pfe_lifetime_pinned) ||
            atomic_read(&hsan_pfe_global_write_attempted) ||
            atomic_read(&hsan_pfe_l3intf_write_attempted) ||
            atomic_read(&hsan_pfe_napt_write_attempted) ||
            state->napt_fixed_attempted || state->global_writes_attempted ||
            state->opp_regs_hw_attempted || state->ofc_csr_attempted ||
            state->mcc_hw_attempted || state->ifc_hw_attempted ||
            state->pa_csr_attempted) {
            dev_warn(&pdev->dev,
                     "PFE MMIO history unresolved: retaining hardware clock until board reset\n");
        } else {
            clk_disable_unprepare(state->clk);
            state->clock_on = false;
        }
    }
	return ret;
}
static int hsan_pfe_probe(struct platform_device *pdev)
{
    int ret;

    mutex_lock(&hsan_pfe_probe_lock);
    ret = hsan_pfe_probe_locked(pdev);
    mutex_unlock(&hsan_pfe_probe_lock);
    return ret;
}

static void hsan_pfe_remove_locked(struct platform_device *pdev)
{
	struct hsan_pfe_state *state = platform_get_drvdata(pdev);

    /* Normal unbind/module unload is suppressed after a hardware attempt.
     * A forced platform-device removal still releases devres after this
     * callback; never leave a published software pointer into that state.
     */
    if (hsan_pfe_hardware_attempted(state)) {
        hsan_lsw_dp_fam_stop_fence();
        dev_crit(&pdev->dev,
                 "forced removal after PFE MMIO: CPU fenced; no hardware drain ACK or safe rebind\n");
    }
    mutex_lock(&hsan_pfe_owner_lock);
    if (hsan_pfe_owner == state)
        hsan_pfe_owner = NULL;
    mutex_unlock(&hsan_pfe_owner_lock);

    if (state->ifc_hash_reserved) {
        int hash_ret = hsan_lsw_shadow_table_release_range(&pdev->dev,
                hsan_pfe_pub_tables[2].id, state->ifc_hash_observed_start,
                HSAN_PFE_IFC_HASH_RESERVED_ROWS);

        if (hash_ret)
            dev_warn(&pdev->dev, "IFC hash software reservation release failed: %d\n", hash_ret);
        else
            state->ifc_hash_reserved = false;
    }
    if (state->l3intf_write_submitted ||
        state->pa_csr_attempted || state->pa_hw_uncertain ||
        state->ifc_hw_attempted || state->ofc_csr_attempted ||
        state->opp_regs_hw_attempted ||
        state->opp_tfrt_hw_committed ||
        state->mcc_hw_attempted || state->mcc_hw_uncertain ||
        state->napt_fixed_attempted || state->napt_partition_attempted ||
        state->napt_hw_uncertain ||
	    state->opp_tfrt_hw_uncertain)
		dev_warn(&pdev->dev, "OPP hardware write history retained until board reset; software table release cannot undo hardware\n");
    hsan_pfe_release_aux_tables(pdev, hsan_pfe_pa_tables,
            state->pa_shadow_registered, HSAN_PFE_PA_TABLE_COUNT);
    hsan_pfe_release_ifc_shadow(pdev, state);
    hsan_pfe_release_aux_tables(pdev, hsan_pfe_service_tables,
            state->service_shadow_registered, HSAN_PFE_SERVICE_TABLE_COUNT);
    hsan_pfe_release_napt_shadows(pdev, state);
    hsan_pfe_release_mcc_shadows(pdev, state);
    hsan_pfe_release_ofc_shadows(pdev, state);
	hsan_pfe_release_opp_shadows(pdev, state);
	hsan_pfe_release_pub_shadows(pdev, state);
	hsan_pfe_release_global_attr_shadow(pdev, state);
	hsan_lsw_unmap_windows(pdev, &hsan_pfe_cfg);
    if (state->clock_on) {
        if (atomic_read(&hsan_pfe_lifetime_pinned) ||
            atomic_read(&hsan_pfe_global_write_attempted) ||
            atomic_read(&hsan_pfe_l3intf_write_attempted) ||
            atomic_read(&hsan_pfe_napt_write_attempted) ||
            state->napt_fixed_attempted || state->global_writes_attempted ||
            state->opp_regs_hw_attempted || state->ofc_csr_attempted ||
            state->mcc_hw_attempted || state->ifc_hw_attempted ||
            state->pa_csr_attempted) {
            dev_warn(&pdev->dev,
                     "PFE MMIO history unresolved: retaining hardware clock until board reset\n");
        } else {
            clk_disable_unprepare(state->clk);
            state->clock_on = false;
        }
    }
}

static void hsan_pfe_remove(struct platform_device *pdev)
{
    mutex_lock(&hsan_pfe_probe_lock);
    hsan_pfe_remove_locked(pdev);
    mutex_unlock(&hsan_pfe_probe_lock);
}
/* PFE MMIO and IFC operations cannot be replayed after a suspend.
 * Reject system power-state transitions once any irreversible path ran.
 */
static int hsan_pfe_pm_suspend(struct device *dev)
{
    (void)dev;
    if (atomic_read(&hsan_pfe_lifetime_pinned) ||
        atomic_read(&hsan_pfe_global_write_attempted) ||
        atomic_read(&hsan_pfe_l3intf_write_attempted) ||
        atomic_read(&hsan_pfe_ifc_write_attempted) ||
        atomic_read(&hsan_pfe_pa_write_attempted) ||
        atomic_read(&hsan_pfe_ofc_write_attempted) ||
        atomic_read(&hsan_pfe_mcc_write_attempted) ||
        atomic_read(&hsan_pfe_napt_write_attempted) ||
        atomic_read(&hsan_pfe_napt_partition_attempted))
        return -EBUSY;
    return 0;
}

static const struct dev_pm_ops hsan_pfe_pm_ops = {
    .suspend = hsan_pfe_pm_suspend,
    .freeze = hsan_pfe_pm_suspend,
    .poweroff = hsan_pfe_pm_suspend,
};

static const struct of_device_id hsan_pfe_match[] = {
	{ .compatible = "hsan,pfe" }, {}
};
MODULE_DEVICE_TABLE(of, hsan_pfe_match);
static struct platform_driver hsan_pfe_driver = {
	.probe = hsan_pfe_probe, .remove = hsan_pfe_remove,
    .driver = { .name = "hsan-lsw-pfe", .of_match_table = hsan_pfe_match,
                .suppress_bind_attrs = true, .pm = &hsan_pfe_pm_ops },
};
module_platform_driver(hsan_pfe_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V Stage79 PFE preflight and retained failure lifecycle; forwarding not armed");
