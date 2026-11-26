// SPDX-License-Identifier: GPL-2.0-only
/*
 * HSAN PIE v300 Linux 7.3.5 experimental DMA driver for LAN1.
 *
 * The hardware registers, descriptor layout and ring sizes are grounded in
 * hi_kpie.ko ARM32 ELF and the supplied FDT.  This is a REAL Linux dma_map_*
 * and NAPI implementation, unlike the synthetic proof/reference files.
 *
 * Warning: a completed Linux build is NOT evidence of hardware functionality.
 * A vendor-gated MAC/LSW/PFE route and the ACP/clock/reset state must be
 * qualified before enabling.  Without the FDT opt-in this module performs
 * no DMA or datapath register writes.  Never touch flash or optical lasers.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/unaligned.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include "hsan-lsw-csr.h"
#include "hsan-lsw-dp.h"
#include "hsan-lsw-pfe.h"
#include "hsan-gemac-lan1.h"
#include "hsan-pie-rx-desc.h"
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define HSAN_PIE_ADDR           0x12080000U
#define HSAN_PIE_LEN            0x10000U
#define HSAN_TX_CH              0x2000U
#define HSAN_RX_CH              0x3000U
/* OEM hi_pie_get_hw_ctrl_v300: RX channel IDs 0..7 each occupy 0x10
 * bytes at 0x3000; per-channel IRQ status/mask banks use 4-byte steps.
 * These addresses are from ARM32 code, not from a live board.
 */
#define HSAN_OEM_RX_CHANNELS    8U
#define HSAN_NET_RX_CHANNELS    4U
#define HSAN_OEM_IRQ_GROUPS     4U
#define HSAN_OEM_RX_CH_STRIDE   0x10U
#define HSAN_OEM_IRQ_STRIDE     0x04U
#define HSAN_CH_BASE            0x0000U
#define HSAN_CH_RING            0x0004U
#define HSAN_CH_WPTR            0x0008U
#define HSAN_CH_RPTR            0x000cU
#define HSAN_RX_IRQ_STATUS      0x0300U
#define HSAN_RX_IRQ_MASK        0x0400U
#define HSAN_RX_GLOBAL         0x3200U
#define HSAN_RX_GLOBAL_ENABLE  BIT(0)
/* hi_kpie.ko hi_pie_rx_init_chan @0x2430: buffer limit is in 128-byte
 * units; IRQ routing uses a two-bit group number per hardware channel.
 */
#define HSAN_RX_IRQ_ROUTE       0x3404U
#define HSAN_RX_LARGE_BUFFER    0x3408U
#define HSAN_RX_BUFFER_LIMIT    0x3504U
#define HSAN_DESC_BYTES        32U
#define HSAN_BUF_SIZE          2048U
#define HSAN_NET_TX_ORDER      12U
#define HSAN_NET_RX_ORDER      10U
#define HSAN_LAN1_PORT         10U
#define HSAN_MIN_TX            60U
#define HSAN_NAPI_WEIGHT       64
/* Bound the OEM-style deferred RX producer kick to a small NAPI batch.
 * Prepared-but-unpublished descriptors retain CPU publication ownership.
 */
#define HSAN_RX_KICK_BATCH_MAX  16U
#define HSAN_FAULT_DATAPATH    0
#define HSAN_STALL_WARN_TICKS  25U
#define HSAN_STALL_ABORT_TICKS 250U
#define HSAN_ROUTE_RECHECK_JIFFIES HZ
#define HSAN_FOREIGN_RX_RECHECK_JIFFIES (HZ / 5)

/* This hardware has no audited DMA stop/reset handshake. An ndo_stop or
 * sysfs unbind is NOT proof that DMA is quiescent. Persist the first ring
 * activation for the lifetime of this kernel module and pin that module:
 * otherwise a rebind could allocate new rings while HW still owns old DMA.
 * This guard does not by itself prove that platform unbind is safe.
 */
static atomic_t hsan_pie_dma_lifetime_touched = ATOMIC_INIT(0);
static atomic_t hsan_pie_boot_reset_attempted = ATOMIC_INIT(0);

struct hsan_v300_tx {
	void *buffer;
	dma_addr_t dma;
	u16 len;
	u16 payload_len;
};
struct hsan_v300_rx {
	void *buffer;
	dma_addr_t dma;
};
struct hsan_v300;
enum hsan_rx_wptr_cmp_state {
    HSAN_RX_WPTR_UNSAMPLED,
    HSAN_RX_WPTR_MATCHED,
    HSAN_RX_WPTR_MISMATCH,
};
struct hsan_v300_rx_queue {
    struct hsan_v300 *parent;
    struct napi_struct napi;
    struct hsan_v300_rx *rx;
    void *rx_desc;
    dma_addr_t rx_desc_dma;
    u16 rx_cons, rx_prod, rx_mask;
    /* Historical CPU readback, not current device or descriptor ownership. */
    u16 last_match_cons, last_match_prod;
    bool wptr_match_valid;
    enum hsan_rx_wptr_cmp_state wptr_cmp_state;
    u32 rx_count;
    u32 buffer_size;
    u8 ring_order;
    unsigned int channel;
    int irq;
    bool irq_ok; /* published/cleared under parent->tx_lock */
    bool napi_enabled;
    u64 packets, bytes, dropped, errors;
    u64 irq_events, polls;
    u16 stop_hw_rptr_before, stop_hw_rptr_after;
    u32 stop_hw_wptr_before, stop_hw_wptr_after;
};

enum hsan_net_open_stage {
    HSAN_NET_OPEN_IDLE,
    HSAN_NET_OPEN_LIFETIME,
    HSAN_NET_OPEN_DMA_OPTIN,
    HSAN_NET_OPEN_TOPOLOGY,
    HSAN_NET_OPEN_PIPELINE,
    HSAN_NET_OPEN_CLOCK,
    HSAN_NET_OPEN_GEMAC,
    HSAN_NET_OPEN_ACP,
    HSAN_NET_OPEN_PHY,
    HSAN_NET_OPEN_RX_IDLE,
    HSAN_NET_OPEN_ALLOC,
    HSAN_NET_OPEN_PHY_ATTACH,
    HSAN_NET_OPEN_IRQ,
    HSAN_NET_OPEN_LINK_PROGRAM,
    HSAN_NET_OPEN_REVALIDATE,
    HSAN_NET_OPEN_PUBLISH,
    HSAN_NET_OPEN_ACTIVE,
};

struct hsan_v300 {
	bool boot_reset_verified;
	bool boot_globals_verified;
	struct device *dev;
	struct net_device *ndev;
	void __iomem *regs;
	struct hsan_v300_rx_queue rxq[HSAN_OEM_RX_CHANNELS];
    unsigned int rx_channels;
	struct delayed_work watchdog;
	spinlock_t tx_lock;
	struct hsan_v300_tx *tx;
	void *tx_desc;
	dma_addr_t tx_desc_dma;
	u16 tx_prod, tx_cons;
	u16 tx_mask;
	u32 tx_count;
    u32 gro_channels;
    u32 irq_resources;
    u32 open_stage;
    u32 open_last_errno; /* positive errno, zero on success */
    u64 open_attempts;
    u32 rx_init_bad_channel;
    u32 rx_init_bad_fields;
    u32 stop_rx_irq_masked_channels;
    u32 stop_rx_irq_pending_channels;
	bool initialized;
    bool phy_connected;
	bool active;
	bool dma_ever_started;
	bool hardware_quiesced;
	unsigned long fault_flags;
	u32 stalled_ticks;
	bool reported_stall;
	u64 tx_packets, tx_bytes, tx_dropped, tx_errors;
	u64 rx_packets, rx_bytes, rx_dropped, rx_errors;
    u64 rx_unposted_pointer_faults, rx_oversized_desc_faults;
    u64 rx_descriptor_addr_faults;
    u64 tx_descriptor_addr_faults;
    u64 rx_rearm_suppressed; /* STOP/quarantine won the publication race */
    u64 rx_irq_rearms; /* IRQ mask released while TX/STOP lock held */
    u64 rx_irq_rearm_blocked; /* STOP or quarantine won the IRQ race */
    u64 rx_irq_lost_edge_repolls; /* newly queued after IRQ rearm */
    u64 link_mode_drift_faults; /* one-shot GEMAC mode changed while link up */
    bool stop_fam_cpu_fenced; /* software publication only, not DMA drain */
    unsigned long last_route_audit; /* 1 Hz; snapshot may sleep */
    u64 runtime_route_audits;
    u64 runtime_route_failures;
    u64 fam_external_fence_faults;
    u64 fam_fence_tx_blocks;
    u64 prearm_handoff_rejections; /* second DP/PFE check before first MMIO */
    u64 rx_irq_postack_reasserts; /* W1C status reassertion, not DMA fault */
    bool stop_irq_synchronized; /* synchronize_irq is CPU-only */
    bool stop_irq_masked_readback;
    bool stop_rx_global_disabled_readback;
    u32 stop_irq_status_after_ack; /* live W1C status, not TX/RX stop ACK */
    u32 dma_fault_first_code; /* first irreversible corruption, read-only */
    u32 dma_fault_events; /* first recorded transition is exactly once */
    /* Read-only last-stop pointer observation: never a DMA stop ACK. */
    bool stop_snapshot_taken;
    bool stop_hw_pointer_progress;
    u16 stop_sw_tx_pending;
    u16 stop_sw_rx_posted;
    u32 stop_tx_dma_retained; /* no HW STOP; mapped allocations kept */
    u32 stop_rx_dma_retained; /* not a count of drained descriptors */
    u16 stop_tx_hw_rptr;
    u16 stop_rx_hw_rptr;
    u16 stop_tx_hw_wptr;
    u16 stop_rx_hw_wptr;
    u16 stop_tx_hw_wptr_after;
    u16 stop_rx_hw_wptr_after;
    bool stop_hw_producer_progress; /* impossible to certify hardware STOP */
    /* Non-latched FAM cell observations at netdev stop. Two unchanged
     * samples do NOT establish a hardware DMA stop or a drain ACK.
     */
    bool stop_fam_sampled;
    bool stop_fam_counter_moved;
    u32 stop_fam_counter_a_before;
    u32 stop_fam_counter_a_after;
    u32 stop_fam_counter_b_before;
    u32 stop_fam_counter_b_after;
    u32 stop_fam_init_status_before; /* init ACK register, NOT STOP */
    u32 stop_fam_init_status_after;
    u32 stop_rx_hw_owned_seen; /* non-atomic coherent descriptor sample */
    u32 stop_rx_desc_addr_mismatch;
    u32 stop_tx_desc_addr_mismatch;
    /* Additional CPU-only admission audit of unsupported GRO RX4..7. A
     * clean-looking snapshot is necessary but NOT DMA-stop evidence.
     */
    u64 other_rx_prearm_checks;
    u64 other_rx_prearm_rejects;
    u32 other_rx_bad_channel;
    u32 other_rx_bad_flags;
    /* Stage64: RX7 shares IRQ[0] with RX0, but belongs to a different
     * OEM NAPI/ring. Record unexpected foreign activation while running.
     * These reads never authorize DMA ownership return.
     */
    unsigned long last_other_rx_audit;
    u64 other_rx_runtime_checks;
    u64 other_rx_runtime_faults;
    u32 other_rx_runtime_bad_channel;
    u32 other_rx_runtime_bad_flags;
    u64 other_rx_irq7_faults;
    /* RX4..7 checks at each ordinary RX producer/IRQ publication boundary,
     * distinct from the 200ms diagnostic watchdog and per-packet RX7 probe.
     */
    u64 other_rx_publish_checks;
    u64 other_rx_publish_faults;
    u64 rx_delivery_suppressed;
    /* Stage72: CPU RX DMA sync/copy must be admitted under the same
     * PIE STOP lock and cross-module FAM publication fence as the
     * following SF_RPTR and descriptor re-arm. These counters are CPU
     * serialisation diagnostics, never hardware DMA completion ACKs.
     */
    u64 rx_cpu_sync_admissions;
    u64 rx_cpu_sync_blocks;
    u64 rx_cpu_copied_bytes;
    /* Stage73: both coherent completion snapshots must be performed
     * under PIE tx_lock plus the FAM STOP publication guard.  The
     * second snapshot runs after skb allocation, before RX DMA sync.
     */
    u64 rx_completion_snapshots;
    u64 rx_completion_rechecks;
    u64 rx_completion_recheck_faults;
    /* No TSO/GSO HW descriptor format has been proven. Copy-only TX
     * supports non-linear non-GSO skbs through skb_copy_bits().
     */
    u64 tx_gso_unsupported_drops;
    u64 tx_nonlinear_flattened;
    /* Stage68: rx_recv may publish SF_RPTR for each consumed ring node;
     * rx_kick publishes the last replenished producer only once per batch.
     * No producer cursor advances until the entire prepared span is ready.
     */
    u64 rx_batch_kick_commits;
    u64 rx_batch_kick_descs;
    u64 rx_batch_kick_faults;
    u64 rx_batch_kick_max;
    /* Stage65: OEM v300 stores two software-controlled ring SLOT cursors
     * in one WPTR register. High16 is SF_RPTR; low16 is producer WPTR.
     * These are not 16-bit cumulative hardware pointer registers.
     */
    u64 rx_sf_cursor_publications;
    u64 rx_sf_cursor_readback_faults;
    u64 rx_hw_rptr_out_of_range;
    u64 rx_slot_cursor_wraps;
    u16 last_rx_sf_cursor_readback;
    u16 last_rx_hw_rptr;
    /* OEM TX set_wptr consumes (node_index + 1) modulo ring_count;
     * TX hardware RPTR is a slot, not a cumulative packet counter.
     */
    u64 tx_hw_rptr_out_of_range;
    u64 tx_slot_cursor_wraps;
    u16 last_tx_hw_rptr;
    u16 last_tx_published_wptr;
    u64 tx_nomem_drops; /* skb consumed; do not spin NETDEV_TX_BUSY */
    u64 irq_mask_readback_faults; /* IRQ mask bit0 failed CPU readback */
    u64 gemac_runtime_faults; /* running MAC clock/config/one-shot state lost */
    u64 tx_state_drops; /* link/STOP changed; not ring backpressure */
    u64 tx_late_ring_faults; /* ring capacity invariant broke after preflight */
    u64 tx_wptr_readback_faults;
    u64 rx_wptr_readback_faults;
    u64 irq_unmask_readback_faults;
    u64 initial_arm_mmio_faults;
    u64 tx_wptr_readback_ok;
    u64 rx_wptr_readback_ok;
    u16 last_tx_wptr_readback;
    u16 last_rx_wptr_readback;
};

static inline u32 hsan_r32(struct hsan_v300 *p, u32 offset)
{
	return readl(p->regs + offset);
}

static inline void hsan_w32(struct hsan_v300 *p, u32 offset, u32 val)
{
	writel(val, p->regs + offset);
}

/* v300 OEM RX MMIO address arithmetic. No unsupported channel is
 * programmed or acknowledged by these helpers.
 */
static inline u32 hsan_rx_chan_reg(unsigned int channel, u32 offset)
{
    return HSAN_RX_CH + HSAN_OEM_RX_CH_STRIDE * channel + offset;
}

static inline u32 hsan_rx_irq_reg(unsigned int channel, bool mask)
{
    return (mask ? HSAN_RX_IRQ_MASK : HSAN_RX_IRQ_STATUS) +
           HSAN_OEM_IRQ_STRIDE * channel;
}

static void hsan_mask_owned_rx_irqs(struct hsan_v300 *p)
{
    unsigned int ch;

    for (ch = 0; ch < p->rx_channels; ch++)
        hsan_w32(p, hsan_rx_irq_reg(ch, true), BIT(0));
}

/* The global RX enable applies to the entire PIE RX block. Ordinary
 * RX0..3 ownership must never bypass a read-only check for unimplemented
 * GRO RX4..7 ring/IRQ state.
 * We do not know whether clean register values prove a complete reset:
 * these are fail-closed *necessary conditions*, not a DMA STOP ACK.
 * Must run only while PIE MMIO clocks are qualified. Caller serializes
 * the final observation with the FAM CPU publication lock.
 */
static u32 hsan_other_rx_dirty(struct hsan_v300 *p, unsigned int ch)
{
    u32 mask = hsan_r32(p, hsan_rx_irq_reg(ch, true));
    u32 status = hsan_r32(p, hsan_rx_irq_reg(ch, false));
    u32 base = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE));
    u32 order = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RING));
    u32 wptr = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR));
    u32 rptr = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR));
    u32 dirty = 0;

    if (!(mask & BIT(0)))
        dirty |= BIT(0); /* IRQ not masked */
    if (status & BIT(0))
        dirty |= BIT(1); /* pending IRQ, no supported NAPI handler */
    if (base)
        dirty |= BIT(2); /* unowned descriptor DMA address */
    if (order)
        dirty |= BIT(3); /* foreign ring geometry */
    if (wptr || rptr)
        dirty |= BIT(4); /* producer/consumer not proven idle */
    return dirty;
}

static bool hsan_unused_rx_prearm_clean(struct hsan_v300 *p)
{
    unsigned int ch;

    p->other_rx_prearm_checks++;
    for (ch = p->rx_channels; ch < HSAN_OEM_RX_CHANNELS; ch++) {
        u32 dirty = hsan_other_rx_dirty(p, ch);

        if (dirty) {
            p->other_rx_prearm_rejects++;
            p->other_rx_bad_channel = ch;
            p->other_rx_bad_flags = dirty;
            return false;
        }
    }
    p->other_rx_bad_channel = 0;
    p->other_rx_bad_flags = 0;
    return true;
}

/* Runtime firmware or a second PIE owner may configure a foreign RX
 * channel AFTER the one-shot prearm checks. The global RX enable is
 * shared, so this is a terminal loss of exclusive DMA ownership.
 * Only call this under tx_lock while active and MMIO clocks qualified.
 */
static bool hsan_unused_rx_runtime_clean_locked(struct hsan_v300 *p)
{
    unsigned int ch;

    p->other_rx_runtime_checks++;
    for (ch = p->rx_channels; ch < HSAN_OEM_RX_CHANNELS; ch++) {
        u32 dirty = hsan_other_rx_dirty(p, ch);

        if (dirty) {
            p->other_rx_runtime_faults++;
            p->other_rx_runtime_bad_channel = ch;
            p->other_rx_runtime_bad_flags = dirty;
            return false;
        }
    }
    p->other_rx_runtime_bad_channel = 0;
    p->other_rx_runtime_bad_flags = 0;
    return true;
}

static inline u8 *hsan_tx_desc(struct hsan_v300 *p, unsigned int idx)
{
	return (u8 *)p->tx_desc + (idx & p->tx_mask) * HSAN_DESC_BYTES;
}

static inline u8 *hsan_rx_desc(struct hsan_v300_rx_queue *q, unsigned int idx)
{
	return (u8 *)q->rx_desc + (idx & q->rx_mask) * HSAN_DESC_BYTES;
}

static void hsan_desc_u32(u8 *desc, unsigned int off, u32 val)
{
	put_unaligned_le32(val, desc + off);
}

static void hsan_desc_u16(u8 *desc, unsigned int off, u16 val)
{
	put_unaligned_le16(val, desc + off);
}

static bool hsan_dma32(dma_addr_t addr, size_t len)
{
	return addr <= U32_MAX && len && len - 1 <= U32_MAX - addr;
}

/* OEM v300 hi_pie_rx_set_sf_rptr and hi_pie_rx_set_wptr are TWO
 * distinct read/modify/write callbacks on RX_WPTR. The former replaces
 * high16 with the next pending RX ring-node index IN rx_recv; the latter
 * replaces low16 with the posted node index in rx_kick, after addbuf.
 * The old combined write skipped that OEM-visible intermediate state.
 *
 * These functions must execute under BOTH tx_lock and the FAM publish
 * guard. The latter serializes the CPU publication with an external STOP,
 * but cannot prove that any previously launched DMA has completed.
 * Cumulative software counters are projected to ring-slot indices only
 * at the MMIO boundary. readl/writel carry the required CPU MMIO order;
 * dma_wmb is required before publishing the newly DMA-owned descriptor.
 */
static u32 hsan_rx_published_cursors(const struct hsan_v300_rx_queue *q)
{
    return ((u32)(q->rx_cons & q->rx_mask) << 16) |
           (u32)(q->rx_prod & q->rx_mask);
}

static void hsan_rx_publish_sf_cursor(struct hsan_v300_rx_queue *q)
{
    struct hsan_v300 *p = q->parent;
    u32 old = hsan_r32(p, hsan_rx_chan_reg(q->channel, HSAN_CH_WPTR));
    u32 next = (old & GENMASK(15, 0)) |
               ((u32)(q->rx_cons & q->rx_mask) << 16);

    hsan_w32(p, hsan_rx_chan_reg(q->channel, HSAN_CH_WPTR), next);
    p->rx_sf_cursor_publications++;
}

static void hsan_rx_publish_producer_cursor(struct hsan_v300_rx_queue *q)
{
    struct hsan_v300 *p = q->parent;
    u32 old = hsan_r32(p, hsan_rx_chan_reg(q->channel, HSAN_CH_WPTR));
    u32 next = (old & GENMASK(31, 16)) |
               (u32)(q->rx_prod & q->rx_mask);

    /* This is the ownership handoff: the new descriptor and data buffer
     * must be visible to the device before the OEM low16 WPTR advances.
     */
    dma_wmb();
    hsan_w32(p, hsan_rx_chan_reg(q->channel, HSAN_CH_WPTR), next);
}

/* OEM v300 TX set_wptr writes the complete 32-bit WPTR register with a
 * ring-slot index, so read back the full TX word. RX still checks its two
 * software-controlled cursor halfwords. CPU readback is NOT DMA STOP ACK.
 */
static bool hsan_check_wptr_readback(struct hsan_v300 *p,
                                     struct hsan_v300_rx_queue *q,
                                     u32 expected)
{
    u32 observed = hsan_r32(p, q ?
        hsan_rx_chan_reg(q->channel, HSAN_CH_WPTR) :
        HSAN_TX_CH + HSAN_CH_WPTR);

    if (!q) {
        p->last_tx_wptr_readback = (u16)observed;
        if (observed == expected) {
            p->tx_wptr_readback_ok++;
            return true;
        }
        p->tx_wptr_readback_faults++;
    } else {
        if (!q->channel) {
            p->last_rx_wptr_readback = (u16)observed;
            p->last_rx_sf_cursor_readback = (u16)(observed >> 16);
        }
        if (observed == expected) {
            p->rx_wptr_readback_ok++;
            /* All RX callers compare the projection of these cumulative
             * cursors while still under the STOP publication lock.
             */
            q->last_match_cons = q->rx_cons;
            q->last_match_prod = q->rx_prod;
            q->wptr_match_valid = true;
            q->wptr_cmp_state = HSAN_RX_WPTR_MATCHED;
            return true;
        }
        /* A failed compare does not invalidate a prior historical match,
         * confirm either new halfword, or establish HW DMA quiescence.
         */
        q->wptr_cmp_state = HSAN_RX_WPTR_MISMATCH;
        p->rx_wptr_readback_faults++;
        if ((observed ^ expected) & 0xffff0000U)
            p->rx_sf_cursor_readback_faults++;
    }
    return false;
}

static void hsan_tx_build_desc(struct hsan_v300 *p, unsigned int idx,
				 dma_addr_t dma, u16 len)
{
	u8 *d = hsan_tx_desc(p, idx);

	memset(d, 0, HSAN_DESC_BYTES);
	hsan_desc_u16(d, 0, len);
	hsan_desc_u32(d, 4, (u32)dma);
	/* hi_pie_tx_send @3714..372c sets metadata[7] low bits=3
	 * for a linear skb. v300 @1e14..1e50 maps them to bits6/5:
	 * owner(bit2), fixed framing(bit4), first/last(bits5/6).
	 */
	d[3] = 0x74;
	d[10] = 0x08;      /* descriptor mode=2 */
	d[15] = 0x40;      /* OEM fixed bitmap/control flag */
	d[16] = 0x02;
	hsan_desc_u32(d, 12, 0x40000000U | BIT(HSAN_LAN1_PORT + 10));
}

static void hsan_rx_build_desc(struct hsan_v300_rx_queue *q, unsigned int idx)
{
	u8 *d = hsan_rx_desc(q, idx);
	struct hsan_v300_rx *s = &q->rx[idx & q->rx_mask];

	memset(d, 0, HSAN_DESC_BYTES);
	hsan_desc_u32(d, 4, (u32)s->dma);
	d[3] = BIT(2); /* owner=hardware */
}

/* The quarantine decision MUST be serialized with ndo_start_xmit.
 * Previously RX/NAPI and the netdev TX watchdog could set active=false
 * between the TX lock's final test and descriptor publication; this could
 * hand hardware another DMA address after the driver declared quarantine.
 * TX, RX/NAPI and watchdog callers hold tx_lock for the decision.
 * These gates prevent new submissions, not a hardware DMA stop ACK.
 */
enum hsan_dma_fault_code {
    HSAN_DMA_FAULT_TX_CONSUMER = 1,
    HSAN_DMA_FAULT_TX_UNOWNED = 2,
    HSAN_DMA_FAULT_RX_UNPOSTED = 3,
    HSAN_DMA_FAULT_RX_OWNER = 4,
    HSAN_DMA_FAULT_RX_UNOWNED = 5,
    HSAN_DMA_FAULT_RX_OVERSIZE = 6,
    HSAN_DMA_FAULT_TX_STALLED = 7,
    HSAN_DMA_FAULT_TX_SLOT = 8,
    HSAN_DMA_FAULT_TX_TIMEOUT = 9,
    HSAN_DMA_FAULT_RX_ADDRESS = 10,
    HSAN_DMA_FAULT_LINK_MODE_DRIFT = 11,
    HSAN_DMA_FAULT_TX_ADDRESS = 12,
    HSAN_DMA_FAULT_RUNTIME_ROUTE = 13,
    HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE = 14,
    HSAN_DMA_FAULT_TX_WPTR_READBACK = 15,
    HSAN_DMA_FAULT_RX_WPTR_READBACK = 16,
    HSAN_DMA_FAULT_IRQ_UNMASK_READBACK = 17,
    HSAN_DMA_FAULT_INITIAL_ARM_READBACK = 18,
    HSAN_DMA_FAULT_IRQ_MASK_READBACK = 19,
    HSAN_DMA_FAULT_TX_LATE_RING = 20,
    HSAN_DMA_FAULT_GEMAC_RUNTIME = 21,
    HSAN_DMA_FAULT_FOREIGN_RX_ACTIVE = 22,
    HSAN_DMA_FAULT_RX_BATCH_LEDGER = 23,
    HSAN_DMA_FAULT_RX_STALE_SNAPSHOT = 24,
};

static void hsan_quarantine_datapath_locked(struct hsan_v300 *p,
            enum hsan_dma_fault_code code, const char *reason)
{
    if (test_and_set_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
        return;
    p->dma_fault_first_code = code;
    p->dma_fault_events++;
    WRITE_ONCE(p->active, false);
    if (p->dma_ever_started) {
        /* Atomic, IRQ-safe FAM producer fence shared with PFE. A fault
         * must close this gate immediately, not wait for ndo_stop.
         */
        hsan_lsw_dp_fam_stop_fence();
        p->stop_fam_cpu_fenced = true;
    }
    netif_stop_queue(p->ndev);
    netif_carrier_off(p->ndev);
    hsan_mask_owned_rx_irqs(p);
    hsan_w32(p, HSAN_RX_GLOBAL,
             hsan_r32(p, HSAN_RX_GLOBAL) & ~HSAN_RX_GLOBAL_ENABLE);
    (void)hsan_r32(p, HSAN_RX_GLOBAL);
    netdev_err(p->ndev,
          "PIE datapath quarantined (code=%u %s); retaining DMA mappings until qualified reset\n",
          code, reason);
}

/* OEM IRQ[0..3] services RX pairs 0/7, 1/6, 2/5, 3/4.
 * A watchdog-only scan leaves a window in which RX0 can publish another
 * descriptor (or unmask IRQ[0]) after a foreign GRO RX4..7 owner has appeared.
 * Recheck every unimplemented GRO channel at each publication boundary.
 * This is a necessary CPU-visible exclusion check, not a global hardware
 * ownership lock or a DMA STOP ACK. Caller holds tx_lock, never the FAM
 * publish guard: quarantine closes the FAM CPU fence and must not run while
 * its own serialization lock is already held.
 */
static bool hsan_rx_publication_foreign_clean_locked(struct hsan_v300 *p)
{
    p->other_rx_publish_checks++;
    if (hsan_unused_rx_runtime_clean_locked(p))
        return true;

    p->other_rx_publish_faults++;
    hsan_quarantine_datapath_locked(p,
        HSAN_DMA_FAULT_FOREIGN_RX_ACTIVE,
        "unsupported GRO RX4..7 owner detected at ordinary RX publication boundary");
    return false;
}

/* The four OEM IRQ groups pair ordinary RX0..3 with GRO RX7..4.
 * Before an IRQ or watchdog claims an ordinary NAPI, require the paired
 * GRO channel to have no visible ring as well as no active IRQ. A ring
 * can be programmed with its IRQ masked and no status pending.
 * Keep the existing mask/status-only check per packet; producer/unmask
 * publication already checks all four foreign channels. The caller holds
 * tx_lock and has checked p->active. These observations never authorize
 * writes/ACK to the peer and do not establish device ownership or STOP.
 */
static bool hsan_rx_foreign_peer_clean_locked(struct hsan_v300_rx_queue *q,
                                             bool check_ring)
{
    struct hsan_v300 *p = q->parent;
    unsigned int partner = HSAN_OEM_RX_CHANNELS - 1 - q->channel;
    u32 dirty;

    if (partner < p->rx_channels)
        return true; /* Both queues have independent owned rings/NAPI. */
    p->other_rx_runtime_checks++;
    if (check_ring) {
        dirty = hsan_other_rx_dirty(p, partner);
    } else {
        u32 mask = hsan_r32(p, hsan_rx_irq_reg(partner, true));
        u32 status = hsan_r32(p, hsan_rx_irq_reg(partner, false));

        dirty = 0;
        if (!(mask & BIT(0)))
            dirty |= BIT(0);
        if (status & BIT(0))
            dirty |= BIT(1);
    }
    if (!dirty)
        return true;

    p->other_rx_runtime_faults++;
    if (partner == 7)
        p->other_rx_irq7_faults++;
    p->other_rx_runtime_bad_channel = partner;
    p->other_rx_runtime_bad_flags = dirty;
    hsan_quarantine_datapath_locked(p,
        HSAN_DMA_FAULT_FOREIGN_RX_ACTIVE,
        "unowned GRO RX partner has visible ring state or IRQ activity");
    return false;
}

/* Stage73: never inspect a coherent RX completion without BOTH
 * CPU-side STOP serialisation gates.  Call only with PIE tx_lock held
 * AND an admitted FAM publish guard. This checks CPU-visible ring/desc
 * consistency; it does not establish that hardware DMA is quiescent.
 * The caller must release the FAM guard before quarantining a fault.
 */
enum hsan_rx_probe_result {
    HSAN_RX_PROBE_HW_RANGE = -1,
    HSAN_RX_PROBE_UNPOSTED = -2,
    HSAN_RX_PROBE_OWNER = -3,
    HSAN_RX_PROBE_UNOWNED = -4,
    HSAN_RX_PROBE_OVERSIZE = -5,
    HSAN_RX_PROBE_ADDRESS = -6,
    HSAN_RX_PROBE_CHANGED = -7,
    HSAN_RX_PROBE_EMPTY = 0,
    HSAN_RX_PROBE_READY = 1,
};

static enum hsan_rx_probe_result
hsan_rx_probe_completion_locked(struct hsan_v300_rx_queue *q, unsigned int slot,
                                struct hsan_pie_rx_metadata *metadata)
{
    struct hsan_v300 *p = q->parent;
    struct hsan_v300_rx *s = &q->rx[slot];
    u16 hw, posted, completed, len;
    u8 *desc;

    p->rx_completion_snapshots++;
    if (unlikely(slot != (q->rx_cons & q->rx_mask)))
        return HSAN_RX_PROBE_CHANGED;
    hw = (u16)hsan_r32(p, hsan_rx_chan_reg(q->channel, HSAN_CH_RPTR));
    if (!q->channel)
        p->last_rx_hw_rptr = hw;
    if (unlikely(hw >= q->rx_count))
        return HSAN_RX_PROBE_HW_RANGE;
    posted = (u16)(q->rx_prod - q->rx_cons);
    completed = (hw - slot) & q->rx_mask;
    if (unlikely(posted >= q->rx_count || completed > posted))
        return HSAN_RX_PROBE_UNPOSTED;
    if (!completed)
        return HSAN_RX_PROBE_EMPTY;
    dma_rmb();
    desc = hsan_rx_desc(q, slot);
    if (unlikely(READ_ONCE(desc[3]) & BIT(2)))
        return HSAN_RX_PROBE_OWNER;
    len = get_unaligned_le16(desc);
    if (unlikely(!s->buffer || !hsan_dma32(s->dma, q->buffer_size)))
        return HSAN_RX_PROBE_UNOWNED;
    if (unlikely(len > q->buffer_size))
        return HSAN_RX_PROBE_OVERSIZE;
    if (unlikely(get_unaligned_le32(desc + 4) != (u32)s->dma))
        return HSAN_RX_PROBE_ADDRESS;
    *metadata = hsan_pie_rx_decode(desc);
    return HSAN_RX_PROBE_READY;
}

/* Caller holds PIE tx_lock and has already released FAM publish guard.
 * An invalid second snapshot is terminal: never recycle or unmap an RX
 * buffer whose ownership is no longer described consistently.
 */
static void hsan_rx_probe_fault_locked(struct hsan_v300_rx_queue *q,
                                       enum hsan_rx_probe_result fault)
{
    struct hsan_v300 *p = q->parent;
    enum hsan_dma_fault_code code;
    const char *reason;

    p->rx_errors++;
    q->errors++;
    switch (fault) {
    case HSAN_RX_PROBE_HW_RANGE:
        p->rx_hw_rptr_out_of_range++;
        code = HSAN_DMA_FAULT_RX_UNPOSTED;
        reason = "RX HW pointer exceeds configured ring slots";
        break;
    case HSAN_RX_PROBE_UNPOSTED:
        p->rx_unposted_pointer_faults++;
        code = HSAN_DMA_FAULT_RX_UNPOSTED;
        reason = "RX consumer exceeds published ring span";
        break;
    case HSAN_RX_PROBE_OWNER:
        code = HSAN_DMA_FAULT_RX_OWNER;
        reason = "RX completed descriptor remains hardware-owned";
        break;
    case HSAN_RX_PROBE_UNOWNED:
        code = HSAN_DMA_FAULT_RX_UNOWNED;
        reason = "RX completion lacks owned software DMA buffer";
        break;
    case HSAN_RX_PROBE_OVERSIZE:
        p->rx_oversized_desc_faults++;
        code = HSAN_DMA_FAULT_RX_OVERSIZE;
        reason = "RX completion length exceeds mapped DMA buffer";
        break;
    case HSAN_RX_PROBE_ADDRESS:
        p->rx_descriptor_addr_faults++;
        code = HSAN_DMA_FAULT_RX_ADDRESS;
        reason = "RX completion DMA address differs from posted mapping";
        break;
    case HSAN_RX_PROBE_EMPTY:
    case HSAN_RX_PROBE_READY:
    case HSAN_RX_PROBE_CHANGED:
    default:
        code = HSAN_DMA_FAULT_RX_STALE_SNAPSHOT;
        reason = "RX descriptor/consumer changed across skb allocation";
        break;
    }
    hsan_quarantine_datapath_locked(p, code, reason);
}

/* OEM TX get_rptr returns UXTH of the hardware ring-SLOT index,
 * NOT a cumulative packet counter. Software tx_cons/tx_prod remain u16
 * cumulative accounting; completed descriptors are derived from the
 * modulo ring-slot distance and bounded by the submitted DMA ledger.
 */
static void hsan_tx_reclaim_locked(struct hsan_v300 *p)
{
    struct hsan_lsw_fam_publish_guard obs;
    u16 hw, pending, completed;
    unsigned int i;

    /* Even a moving consumer is NOT a qualified release when DMA has
     * been quarantined or stop() has begun. Keep mappings until an
     * audited hardware STOP/RESET protocol establishes ownership.
     */
    if (!READ_ONCE(p->active) ||
        test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
        return;
    /* Stage70: an independent FAM owner can close the CPU publication
     * fence without ndo_stop. Its closure is not a hardware DMA STOP
     * ACK, so retain ALL TX mappings and avoid sampling another live
     * completion cursor after the external ownership transition.
     */
    if (unlikely(hsan_lsw_dp_fam_cpu_fenced())) {
        p->fam_external_fence_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "external FAM CPU STOP fenced TX completion reclaim");
        return;
    }
    /* Take the first completion snapshot under the very same STOP
     * serialization lock as later per-slot DMA unmaps. Otherwise an
     * independent FAM CPU STOP could win between the soft-state check
     * above and this hardware RPTR read. The observation guard does NOT
     * count a DMA reclaim and cannot attest hardware bus quiescence.
     */
    if (unlikely(hsan_lsw_fam_observe_begin(&obs))) {
        p->fam_external_fence_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "external FAM CPU STOP blocked TX completion snapshot");
        return;
    }
    hw = (u16)hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR);
    dma_rmb();
    p->last_tx_hw_rptr = hw;
    pending = (u16)(p->tx_prod - p->tx_cons);
    /* OEM TX get_rptr returns the low 16 bits of the HW ring SLOT.
     * OEM tx_send masks (node->index + 1) by the ring order before
     * invoking v300 set_wptr. Project the cumulative SW consumer through
     * tx_mask, instead of treating hardware as a monotonically increasing
     * u16 that would diverge on the first ring wrap.
     * An out-of-range raw HW slot must not be silently normalized.
     */
    if (unlikely(hw >= p->tx_count)) {
        hsan_lsw_fam_publish_end(&obs);
        p->tx_hw_rptr_out_of_range++;
        p->tx_errors++;
        hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_CONSUMER,
                                       "TX hardware consumer outside ring slot range");
        return;
    }
    completed = (hw - (p->tx_cons & p->tx_mask)) & p->tx_mask;
    if (unlikely(pending >= p->tx_count || completed > pending)) {
        hsan_lsw_fam_publish_end(&obs);
        p->tx_errors++;
        hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_CONSUMER,
                                       "TX hardware consumer outside submitted slots");
        return;
    }
    if (!completed) {
        /* Reclaim can free slots while carrier is down without waking
         * the driver XOFF bit. Linkwatch does not clear XOFF. A valid
         * non-full ring has capacity even if some mappings are still
         * pending and no new descriptor completed on this poll.
         * Both the TX lock and FAM CPU STOP guard are still held.
         */
        if (pending < p->tx_count - 1 && netif_running(p->ndev) &&
            netif_carrier_ok(p->ndev) && netif_queue_stopped(p->ndev))
            netif_wake_queue(p->ndev);
        hsan_lsw_fam_publish_end(&obs);
        return;
    }
    /* Stage54: preflight the ENTIRE hardware-completed span before the
     * first DMA unmap. If a later descriptor is corrupt, the old single
     * pass could already have freed preceding mappings even though the
     * consumer evidence is internally inconsistent. An invalid batch
     * now pins every mapping and permanently quarantines the datapath.
     * Stage79 holds the observation guard across this ENTIRE scan. The
     * previous code dropped it immediately after RPTR, allowing an
     * external FAM CPU STOP to precede these descriptor observations.
     * This is a software integrity fence, not a hardware DMA drain ACK.
     */
    dma_rmb();
    for (i = 0; i < completed; i++) {
        u16 index = p->tx_cons + i;
        struct hsan_v300_tx *s = &p->tx[index & p->tx_mask];

        if (unlikely(!s->buffer || s->len < HSAN_MIN_TX ||
                     s->len > HSAN_BUF_SIZE ||
                     !s->payload_len || s->payload_len > s->len ||
                     !hsan_dma32(s->dma, s->len))) {
            hsan_lsw_fam_publish_end(&obs);
            p->tx_errors++;
            hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_UNOWNED,
                "TX completion batch contains an invalid software DMA mapping");
            return;
        }
        if (unlikely(get_unaligned_le32(hsan_tx_desc(p, index) + 4) !=
                     (u32)s->dma)) {
            hsan_lsw_fam_publish_end(&obs);
            p->tx_errors++;
            p->tx_descriptor_addr_faults++;
            hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_ADDRESS,
                "TX completion batch has a DMA address mismatch");
            return;
        }
    }
    hsan_lsw_fam_publish_end(&obs);
    /* External FAM STOP is serialized by the same CSR publish guard used
     * for TX/RX submission. A STOP can arrive between reclaimed entries;
     * every individual dma_unmap_single must first acquire that guard.
     * A STOP winner retains all remaining mappings, even when RPTR has
     * advanced; the STOP fence is CPU-only and not a DMA STOP ACK.
     * Never invoke quarantine (which takes the CSR STOP lock) while a
     * publish guard is held.
     */
    for (i = 0; i < completed; i++) {
        struct hsan_lsw_fam_publish_guard pub;
        struct hsan_v300_tx *s = &p->tx[p->tx_cons & p->tx_mask];
        u16 now, current_pending, remaining;
        u16 current_completed;

        if (unlikely(hsan_lsw_fam_reclaim_begin(&pub))) {
            p->tx_errors++;
            p->fam_external_fence_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                "external FAM CPU STOP blocked TX DMA mapping release");
            return;
        }
        /* The first RPTR/batch snapshot released FAM's CPU STOP lock
         * before this per-entry admission. Revalidate the remaining completion
         * window and this slot's DMA address INSIDE that same guard,
         * immediately before unmap. A STOP winner cannot free mapping
         * memory, while a changed/invalid consumer cannot use stale
         * batch evidence. Neither check is a physical DMA STOP ACK.
         */
        now = (u16)hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR);
        dma_rmb();
        current_pending = (u16)(p->tx_prod - p->tx_cons);
        remaining = completed - i;
        current_completed = (now - (p->tx_cons & p->tx_mask)) &
                            p->tx_mask;
        if (unlikely(now >= p->tx_count ||
                     current_pending >= p->tx_count ||
                     current_completed < remaining ||
                     current_completed > current_pending ||
                     !s->buffer || !hsan_dma32(s->dma, s->len) ||
                     get_unaligned_le32(hsan_tx_desc(p, p->tx_cons) + 4) !=
                     (u32)s->dma)) {
            hsan_lsw_fam_publish_end(&pub);
            p->tx_errors++;
            hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_CONSUMER,
                "TX consumer/descriptor changed before guarded DMA reclaim");
            return;
        }
        dma_unmap_single(p->dev, s->dma, s->len, DMA_TO_DEVICE);
        kfree(s->buffer);
        p->tx_packets++;
        p->tx_bytes += s->payload_len;
        s->buffer = NULL;
        s->dma = (dma_addr_t)0;
        s->len = 0;
        s->payload_len = 0;
        p->tx_cons++;
        /* The original OEM TX path reclaims completed queue entries
         * before allowing more sends.  Wake only after the final validated
         * mapping has been unmapped, WHILE the FAM STOP serialization
         * lock is still held. An external STOP that wins first prevents
         * this wake entirely; if STOP wins later, the final TX WPTR
         * publication guard still denies further device DMA handoffs.
         * netif_wake_queue schedules the qdisc, not a device MMIO write.
         * Linux clears netif_running before invoking ndo_stop.
         */
        if (i + 1 == completed && READ_ONCE(p->active) &&
            netif_running(p->ndev) && netif_carrier_ok(p->ndev) &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags) &&
            netif_queue_stopped(p->ndev) &&
            (u16)(p->tx_prod - p->tx_cons) < p->tx_count - 1)
            netif_wake_queue(p->ndev);
        hsan_lsw_fam_publish_end(&pub);
    }
}

static void hsan_tx_reclaim(struct hsan_v300 *p)
{
	unsigned long flags;

	spin_lock_irqsave(&p->tx_lock, flags);
	hsan_tx_reclaim_locked(p);
	spin_unlock_irqrestore(&p->tx_lock, flags);
}

/* Different RX NAPI instances may run on different CPUs. All aggregate
 * and per-queue 64-bit accounting uses the same lock as ethtool/STOP;
 * READ_ONCE alone cannot make concurrent increments lossless.
 */
static void hsan_rx_account(struct hsan_v300_rx_queue *q, bool delivered,
                            unsigned int bytes, bool error, bool suppressed)
{
    struct hsan_v300 *p = q->parent;
    unsigned long flags;

    spin_lock_irqsave(&p->tx_lock, flags);
    if (delivered) {
        p->rx_packets++;
        p->rx_bytes += bytes;
        q->packets++;
        q->bytes += bytes;
    } else {
        p->rx_dropped++;
        q->dropped++;
    }
    if (error) {
        p->rx_errors++;
        q->errors++;
    }
    if (suppressed)
        p->rx_delivery_suppressed++;
    spin_unlock_irqrestore(&p->tx_lock, flags);
}

/* OEM hi_pie_rx_poll calls rx_kick at a loop exit or an error branch,
 * NOT after each successful rx_addbuf. Each ordinary RX ring can safely
 * defer a bounded run of prepared descriptors because the previous HW
 * producer cursor still limits device reachability. Software rx_cons
 * advances on each SF_RPTR; rx_prod advances ONLY on the final kick.
 *
 * The private skbs are retained until the kick is admitted: on STOP or
 * uncertain readback the unpublished mappings remain pinned and all
 * private skbs are discarded. This is a partial OEM batching port, not
 * proof of FAM HW STOP ACK or equivalence to the 8-channel vendor queues.
 */
static bool hsan_rx_flush_kick_batch(struct hsan_v300_rx_queue *q,
                                     struct napi_struct *napi,
                                     struct sk_buff **pkts,
                                     unsigned int pending)
{
    struct hsan_v300 *p = q->parent;
    struct hsan_lsw_fam_publish_guard pub;
    unsigned long flags;
    bool published = false;
    unsigned int i;

    if (!pending)
        return true;
    spin_lock_irqsave(&p->tx_lock, flags);
    if (READ_ONCE(p->active) && hsan_lsw_dp_fam_cpu_fenced() &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        p->fam_external_fence_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "FAM CPU STOP blocked deferred RX producer kick");
    }
    if (READ_ONCE(p->active) &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        u16 posted = (u16)(q->rx_prod - q->rx_cons);

        /* The original ring has one unused slot. Before a deferred kick
         * it has exactly 'pending' fewer device-visible postings, while
         * every completed slot already has a fully rebuilt descriptor.
         * An unexpected ledger must NOT cause an MMIO producer advance.
         */
        if (unlikely(pending > HSAN_RX_KICK_BATCH_MAX ||
                     (unsigned int)posted + pending != q->rx_count - 1)) {
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_RX_BATCH_LEDGER,
                "RX deferred producer span/ownership ledger mismatch");
        } else if (!hsan_rx_publication_foreign_clean_locked(p)) {
            /* The paired GRO channel alone is insufficient here.
             * Global PIE RX enable also exposes the other GRO channels.
             * The helper quarantines before the FAM publication guard
             * is acquired and never ACKs another channel's IRQ.
             */
        } else if (hsan_lsw_fam_publish_begin(&pub)) {
            p->fam_external_fence_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                "FAM CPU STOP raced deferred RX producer publication");
        } else {
            /* SF_RPTR has moved but HW low16 WPTR is intentionally old.
             * Recheck the exact 32-bit image before releasing the batch.
             */
            if (hsan_check_wptr_readback(p, q,
                                         hsan_rx_published_cursors(q))) {
                q->rx_prod += pending;
                hsan_rx_publish_producer_cursor(q);
                published = hsan_check_wptr_readback(p, q,
                                                      hsan_rx_published_cursors(q));
            }
            hsan_lsw_fam_publish_end(&pub);
            if (!published)
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_RX_WPTR_READBACK,
                    "deferred RX producer WPTR readback differs");
        }
    }
    if (published) {
        p->rx_batch_kick_commits++;
        p->rx_batch_kick_descs += pending;
        if (pending > p->rx_batch_kick_max)
            p->rx_batch_kick_max = pending;
    } else {
        p->rx_batch_kick_faults++;
        p->rx_rearm_suppressed += pending;
    }
    spin_unlock_irqrestore(&p->tx_lock, flags);

    /* GRO must not observe a frame whose replacement ring slot was
     * merely prepared in CPU memory but never published to the device.
     */
    for (i = 0; i < pending; i++) {
        struct sk_buff *skb = pkts[i];

        if (!skb)
            continue;
        /* STOP may win after the successful kick lock is released but
         * before GRO. Recheck the visible software fence per skb; this
         * closes an observable late-STOP window, not the race with an
         * interrupt arriving *inside* the upper-stack receive call.
         */
        if (published && READ_ONCE(p->active) &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags) &&
            !hsan_lsw_dp_fam_cpu_fenced()) {
            unsigned int len = skb->len;

            skb_record_rx_queue(skb, q->channel);
            skb->protocol = eth_type_trans(skb, p->ndev);
            napi_gro_receive(napi, skb);
            hsan_rx_account(q, true, len, false, false);
        } else {
            dev_kfree_skb_any(skb);
            hsan_rx_account(q, false, 0, false, true);
        }
    }
    return published;
}

static int hsan_napi_poll(struct napi_struct *napi, int budget)
{
	struct hsan_v300_rx_queue *q =
        container_of(napi, struct hsan_v300_rx_queue, napi);
    struct hsan_v300 *p = q->parent;
    struct sk_buff *batch_skbs[HSAN_RX_KICK_BATCH_MAX];
    unsigned int batch_count = 0;
    int work_done = 0;
    unsigned long poll_flags;

    spin_lock_irqsave(&p->tx_lock, poll_flags);
    q->polls++;
    spin_unlock_irqrestore(&p->tx_lock, poll_flags);
    hsan_tx_reclaim(p);
	while (work_done < budget && READ_ONCE(p->active)) {
        /* A STOP from the DP/PFE owner can arrive independently of PIE's
         * ndo_stop. Do not consume another RX completion after the CPU
         * ownership fence is visibly closed. This guard does not claim
         * that an in-flight device DMA has already drained.
         */
        if (unlikely(hsan_lsw_dp_fam_cpu_fenced())) {
            unsigned long flags;

            spin_lock_irqsave(&p->tx_lock, flags);
            if (READ_ONCE(p->active) &&
                !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
                p->fam_external_fence_faults++;
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                    "external FAM CPU STOP blocked RX completion delivery");
            }
            spin_unlock_irqrestore(&p->tx_lock, flags);
            break;
        }
        unsigned int slot;
        struct hsan_v300_rx *s;
        struct sk_buff *skb = NULL;
        unsigned long mmio_flags;
        u16 len;
        struct hsan_pie_rx_metadata metadata;

        /* Stage73: read hardware RPTR, ledger and coherent descriptor
         * under the same FAM STOP gate as subsequent RX DMA access. The
         * old code released tx_lock before dma_rmb()/descriptor reads;
         * a STOP could close ownership in that unprotected window.
         * Allocation still happens outside spinlocks, so the completion
         * must be checked a second time before dma_sync_single_for_cpu.
         */
        spin_lock_irqsave(&p->tx_lock, mmio_flags);
        if (!READ_ONCE(p->active) ||
            test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            spin_unlock_irqrestore(&p->tx_lock, mmio_flags);
            break;
        }
        if (!hsan_rx_foreign_peer_clean_locked(q, false)) {
            spin_unlock_irqrestore(&p->tx_lock, mmio_flags);
            break;
        }
        {
            struct hsan_lsw_fam_publish_guard pub;
            enum hsan_rx_probe_result probe;

            if (hsan_lsw_fam_publish_begin(&pub)) {
                p->fam_external_fence_faults++;
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                    "FAM STOP denied RX coherent completion snapshot");
                spin_unlock_irqrestore(&p->tx_lock, mmio_flags);
                break;
            }
            slot = q->rx_cons & q->rx_mask;
            probe = hsan_rx_probe_completion_locked(q, slot, &metadata);
            hsan_lsw_fam_publish_end(&pub);
            if (probe < 0)
                hsan_rx_probe_fault_locked(q, probe);
            spin_unlock_irqrestore(&p->tx_lock, mmio_flags);
            if (probe != HSAN_RX_PROBE_READY)
                break;
        }
        s = &q->rx[slot];
        len = metadata.length;
        /* Allocate the destination skb before taking either spinlock.
         * No CPU access to the DMA-backed buffer or cache-sync occurs
         * until the STOP-serialized admission below succeeds.
         */
        /* OEM hi_net_rx_proc routes by decoded port/subport, not by the
         * PIE queue ID. This netdev owns port10 only. The original API
         * falls back to a port's default slot for unknown subports, so
         * subport alone is not a reason to reject a normal LAN frame.
         * Hardware aggregates require their own GRO delivery path; do
         * not feed them to eth_type_trans as ordinary Ethernet frames.
         */
        if (metadata.port != HSAN_LAN1_PORT || metadata.gro_segments) {
            hsan_rx_account(q, false, 0, false, false);
        } else if (len < ETH_HLEN) {
            hsan_rx_account(q, false, 0, true, false);
		} else {
			skb = napi_alloc_skb(napi, len + NET_IP_ALIGN);
			if (unlikely(!skb)) {
                hsan_rx_account(q, false, 0, false, false);
			} else {
				skb_reserve(skb, NET_IP_ALIGN);
				/* OEM hi_pie_rx_poll calls rx_addbuf BEFORE
				 * hi_net_rx_proc. Defer the upper-stack handoff until
				 * the replacement descriptor has been published. If
				 * FAM STOP wins, the skb is still ours to drop safely.
				 */
			}
		}
        /* Stage72: the old path performed dma_sync_single_for_cpu and
         * skb_put_data outside both STOP fences, then only protected the
         * later CPU->device re-arm. A concurrent external FAM STOP could
         * complete between those operations, leaving a late CPU read of
         * an unknown HW-owned RX buffer. The bounded (<= 2048 byte)
         * CPU cache-sync, skb copy, return sync, SF_RPTR update and
         * descriptor re-arm now form one FAM/PIE critical section.
         * skb allocation was done above, outside the spinlocks.
         */
        {
            unsigned long flags;
            bool may_rearm;
            bool cpu_sync_admitted = false;

            spin_lock_irqsave(&p->tx_lock, flags);
            /* A different owner may have permanently closed FAM CPU
             * publication after open(). Never post an RX buffer then.
             */
            if (READ_ONCE(p->active) && hsan_lsw_dp_fam_cpu_fenced() &&
                !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
                p->fam_external_fence_faults++;
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                    "FAM CPU producer fence closed before RX repost");
            }
            may_rearm = READ_ONCE(p->active) &&
                !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags);
            if (may_rearm) {
                struct hsan_lsw_fam_publish_guard pub;

                if (hsan_lsw_fam_publish_begin(&pub)) {
                    p->fam_external_fence_faults++;
                    hsan_quarantine_datapath_locked(p,
                        HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                        "FAM CPU stop raced RX cursor publication");
                    may_rearm = false;
                } else {
                    /* First snapshot was STOP-admitted but skb allocation
                     * released the locks. Recheck the same slot, complete
                     * bit, mapped bus address and length NOW, while the
                     * identical STOP guards protect the payload copy.
                     */
                    enum hsan_rx_probe_result probe;
                    struct hsan_pie_rx_metadata verified;

                    p->rx_completion_rechecks++;
                    probe = hsan_rx_probe_completion_locked(q, slot,
                                                            &verified);
                    if (probe != HSAN_RX_PROBE_READY ||
                        !hsan_pie_rx_metadata_equal(&metadata, &verified)) {
                        p->rx_completion_recheck_faults++;
                        hsan_lsw_fam_publish_end(&pub);
                        if (probe == HSAN_RX_PROBE_READY ||
                            probe == HSAN_RX_PROBE_EMPTY)
                            probe = HSAN_RX_PROBE_CHANGED;
                        hsan_rx_probe_fault_locked(q, probe);
                        may_rearm = false;
                    } else {
                        /* STOP cannot close the CPU publication gate
                         * between descriptor validation, DMA sync,
                         * bounded copy and completed-slot re-arm.
                         */
                        /* No skb means the payload was discarded. Do
                         * not transition its streaming mapping to CPU
                         * ownership when only the coherent descriptor
                         * and its software cursor will be updated.
                         */
                        if (skb) {
                            dma_sync_single_for_cpu(p->dev, s->dma,
                                                    q->buffer_size,
                                                    DMA_FROM_DEVICE);
                            p->rx_cpu_sync_admissions++;
                            cpu_sync_admitted = true;
                            skb_put_data(skb, s->buffer, len);
                            p->rx_cpu_copied_bytes += len;
                            dma_sync_single_for_device(p->dev, s->dma,
                                                       q->buffer_size,
                                                       DMA_FROM_DEVICE);
                        }
                        q->rx_cons++;
                        if (!(q->rx_cons & q->rx_mask))
                            p->rx_slot_cursor_wraps++;
                        dma_wmb();
                        hsan_rx_publish_sf_cursor(q);
                        if (!hsan_check_wptr_readback(p, q,
                                                      hsan_rx_published_cursors(q))) {
                            may_rearm = false;
                        } else {
                            hsan_rx_build_desc(q, slot);
                            dma_wmb();
                        }
                        hsan_lsw_fam_publish_end(&pub);
                        if (!may_rearm)
                            hsan_quarantine_datapath_locked(p,
                                HSAN_DMA_FAULT_RX_WPTR_READBACK,
                                "OEM RX cursor RMW readback failed (SF/producer)");
                    }
                }
            }
            if (!may_rearm) {
                /* Count only pre-sync denials of planned copies.
                 * No-skb slots never needed a payload sync. An SF_RPTR
                 * failure after a successful DMA sync is a different
                 * fault and must not masquerade as a STOP interception.
                 * Either way, retain the mapping without HW STOP ACK.
                 */
                p->rx_rearm_suppressed++;
                if (skb && !cpu_sync_admitted)
                    p->rx_cpu_sync_blocks++;
            }
            spin_unlock_irqrestore(&p->tx_lock, flags);
            /* No frame reaches GRO until its descriptor's low16
             * producer kick succeeds. Even dropped/short/ENOMEM frames
             * consume a ring slot and count against the NAPI budget.
             */
            work_done++;
            if (!may_rearm) {
                if (skb) {
                    dev_kfree_skb_any(skb);
                    hsan_rx_account(q, false, 0, false, true);
                }
                break;
            }
            batch_skbs[batch_count++] = skb;
            if (batch_count == HSAN_RX_KICK_BATCH_MAX) {
                bool kicked = hsan_rx_flush_kick_batch(q, napi,
                                                        batch_skbs, batch_count);

                batch_count = 0;
                if (!kicked)
                    break;
            }
        }
    }
    /* Flush a partial batch on budget exhaustion, no-more-completions,
     * or an error path. A STOP/quarantine winner prevents the low16
     * kick, and the helper drops all still-private skbs.
     */
    if (batch_count)
        (void)hsan_rx_flush_kick_batch(q, napi, batch_skbs, batch_count);
    /* This exported API can synchronously deliver GRO/normal packets
     * and reenter ndo_start_xmit. Neither tx_lock nor the FAM guard may
     * be held. It releases NAPI ownership before the IRQ-rearm tail.
     */
    if (work_done < budget && napi_complete_done(napi, work_done)) {
        unsigned long flags;

        /* IRQ unmask is a CPU->device publication, just like RX rearm.
         * It MUST share the STOP/quarantine lock: the old check then
         * writel could reopen interrupts after ndo_stop had masked them.
         */
        spin_lock_irqsave(&p->tx_lock, flags);
        if (READ_ONCE(p->active) && hsan_lsw_dp_fam_cpu_fenced() &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            p->fam_external_fence_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                "FAM CPU producer fence closed before IRQ unmask");
        }
        if (q->irq_ok && READ_ONCE(p->active) &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags) &&
            /* Watchdog/ISR may have rescheduled after complete_done.
             * They take this same lock: leave their NAPI IRQ masked.
             * Never unmask on behalf of an already scheduled poll.
             */
            !napi_is_scheduled(napi) &&
            hsan_rx_publication_foreign_clean_locked(p)) {
            struct hsan_lsw_fam_publish_guard pub;

            if (hsan_lsw_fam_publish_begin(&pub)) {
                p->fam_external_fence_faults++;
                p->rx_irq_rearm_blocked++;
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                    "FAM CPU stop raced IRQ unmask publication");
            } else {
                bool unmask_bad;

                hsan_w32(p, hsan_rx_irq_reg(q->channel, true), 0);
                unmask_bad = !!(hsan_r32(p, hsan_rx_irq_reg(q->channel, true)) & BIT(0));
                if (unmask_bad)
                    p->irq_unmask_readback_faults++;
                else
                    p->rx_irq_rearms++;
                /* Close the classic lost-edge window: a descriptor can
                 * complete between NAPI's last RPTR read and IRQ unmask.
                 * Mask and re-schedule before releasing the shared STOP
                 * admission lock. This is NOT a hardware DMA STOP ACK.
                 */
                {
                    u16 hw_slot = (u16)hsan_r32(p, hsan_rx_chan_reg(q->channel, HSAN_CH_RPTR));
                    bool invalid_slot = hw_slot >= q->rx_count;
                    bool remask_bad = false;

                    if (!q->channel)
                        p->last_rx_hw_rptr = hw_slot;
                    if (!unmask_bad && !invalid_slot &&
                        hw_slot != (q->rx_cons & q->rx_mask)) {
                        hsan_w32(p, hsan_rx_irq_reg(q->channel, true), 1);
                        remask_bad = !(hsan_r32(p, hsan_rx_irq_reg(q->channel, true)) & BIT(0));
                        if (remask_bad)
                            p->irq_mask_readback_faults++;
                        /* Busy poll may claim SCHED after our check.
                         * The wrapper still reports MISSED to that
                         * owner; count only a new NAPI enqueue.
                         */
                        else if (napi_schedule(napi))
                            p->rx_irq_lost_edge_repolls++;
                    }
                    /* Never quarantine with the FAM publish lock held. */
                    hsan_lsw_fam_publish_end(&pub);
                    if (remask_bad) {
                        hsan_quarantine_datapath_locked(p,
                            HSAN_DMA_FAULT_IRQ_MASK_READBACK,
                            "NAPI lost-edge IRQ remask bit did not latch");
                    } else if (invalid_slot) {
                        p->rx_errors++;
                        q->errors++;
                        p->rx_hw_rptr_out_of_range++;
                        hsan_quarantine_datapath_locked(p,
                            HSAN_DMA_FAULT_RX_UNPOSTED,
                            "RX hardware ring-slot invalid on IRQ rearm");
                    } else if (unmask_bad) {
                        hsan_quarantine_datapath_locked(p,
                            HSAN_DMA_FAULT_IRQ_UNMASK_READBACK,
                            "PIE IRQ mask did not accept CPU unmask");
                    }
                }
            }
        } else if (q->irq_ok) {
            p->rx_irq_rearm_blocked++;
        }
        spin_unlock_irqrestore(&p->tx_lock, flags);
    }
	return work_done;
}

static irqreturn_t hsan_rx_irq_one(int irq, void *data)
{
    struct hsan_v300_rx_queue *q = data;
    struct hsan_v300 *p = q->parent;
    unsigned long flags;
    u32 mask_reg = hsan_rx_irq_reg(q->channel, true);
    u32 status_reg = hsan_rx_irq_reg(q->channel, false);
    struct hsan_lsw_fam_publish_guard service;
    bool schedule, mask_bad;

    /* Each IRQ group owns its ordinary RX channel and observes its GRO
     * partner (7-group) without acknowledging that unowned channel.
     * IRQ[0..3] therefore handles RX0..3 independently with four NAPIs.
     */
    spin_lock_irqsave(&p->tx_lock, flags);
    if (!READ_ONCE(p->active) ||
        test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        spin_unlock_irqrestore(&p->tx_lock, flags);
        return IRQ_NONE;
    }
    if (!hsan_rx_foreign_peer_clean_locked(q, true)) {
        spin_unlock_irqrestore(&p->tx_lock, flags);
        return IRQ_HANDLED;
    }
    /* OEM hi_pie_rx_interrupt tests BOTH enabled and pending before
     * NAPI ownership. A masked pending bit belongs to the current poll;
     * do not steal/ACK it merely because the paired IRQ was delivered.
     */
    if ((hsan_r32(p, mask_reg) & BIT(0)) ||
        !(hsan_r32(p, status_reg) & BIT(0))) {
        spin_unlock_irqrestore(&p->tx_lock, flags);
        return IRQ_NONE;
    }
    q->irq_events++;
    if (hsan_lsw_dp_fam_cpu_fenced()) {
        p->fam_external_fence_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "FAM CPU producer fence closed while PIE IRQ pending");
        spin_unlock_irqrestore(&p->tx_lock, flags);
        return IRQ_HANDLED;
    }
    schedule = napi_schedule_prep(&q->napi);
    /* FAM STOP can win after the Boolean check without taking tx_lock. */
    if (hsan_lsw_fam_irq_service_begin(&service)) {
        p->fam_external_fence_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "FAM CPU STOP denied ordinary RX IRQ service");
        goto complete_prep;
    }
    hsan_w32(p, mask_reg, BIT(0));
    mask_bad = !(hsan_r32(p, mask_reg) & BIT(0));
    if (!mask_bad && schedule) {
        hsan_w32(p, status_reg, BIT(0));
        if (hsan_r32(p, status_reg) & BIT(0))
            p->rx_irq_postack_reasserts++;
    }
    hsan_lsw_fam_publish_end(&service);
    if (mask_bad) {
        p->irq_mask_readback_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_IRQ_MASK_READBACK,
            "PIE RX IRQ mask bit0 did not latch after ISR mask write");
    }
complete_prep:
    /* Balance a successful napi_schedule_prep even after quarantine:
     * the inactive poll completes without touching descriptors.
     */
    if (schedule)
        __napi_schedule(&q->napi);
    spin_unlock_irqrestore(&p->tx_lock, flags);
    return IRQ_HANDLED;
}

/* One physical IRQ serves an ordinary queue and its GRO peer. Each
 * queue independently masks/ACKs its own status and schedules its NAPI.
 */
static irqreturn_t hsan_rx_irq(int irq, void *data)
{
    struct hsan_v300_rx_queue *q = data;
    unsigned int partner = HSAN_OEM_RX_CHANNELS - 1 - q->channel;
    irqreturn_t first = hsan_rx_irq_one(irq, q);
    irqreturn_t second = IRQ_NONE;

    if (partner < q->parent->rx_channels)
        second = hsan_rx_irq_one(irq, &q->parent->rxq[partner]);
    return first == IRQ_HANDLED || second == IRQ_HANDLED ?
           IRQ_HANDLED : IRQ_NONE;
}

/* Caller holds the STOP/IRQ lock. The watchdog fallback must mask BEFORE
 * scheduling each NAPI too, including when an IRQ is present; otherwise
 * a parallel ISR could repeatedly assert the same unmasked pending bit.
 */
static void hsan_poll_owned_rx_locked(struct hsan_v300 *p)
{
    unsigned int ch;

    if (!READ_ONCE(p->active) ||
        test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
        return;
    if (hsan_lsw_dp_fam_cpu_fenced()) {
        p->fam_external_fence_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "FAM CPU producer fence closed before watchdog RX scheduling");
        return;
    }
    /* Global RX enable is shared. Screen all four unowned channels
     * before the first ordinary NAPI claim, even for IRQ-less polling.
     * This is a CPU-visible snapshot, not a hardware STOP or ownership
     * transfer; ordinary publication guards still apply at their sites.
     */
    if (!hsan_unused_rx_runtime_clean_locked(p)) {
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FOREIGN_RX_ACTIVE,
            "unsupported GRO RX4..7 visible before watchdog RX scheduling");
        return;
    }

    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];
        struct hsan_lsw_fam_publish_guard service;
        u32 mask_reg = hsan_rx_irq_reg(ch, true);
        bool mask_bad;

        if (!READ_ONCE(p->active))
            break;
        if (!napi_schedule_prep(&q->napi))
            continue;
        if (hsan_lsw_fam_irq_service_begin(&service)) {
            p->fam_external_fence_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                "FAM CPU STOP denied watchdog RX IRQ service");
            __napi_schedule(&q->napi); /* balance the inactive poll */
            break;
        }
        hsan_w32(p, mask_reg, BIT(0));
        mask_bad = !(hsan_r32(p, mask_reg) & BIT(0));
        hsan_lsw_fam_publish_end(&service);
        if (mask_bad) {
            p->irq_mask_readback_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_IRQ_MASK_READBACK,
                "watchdog RX IRQ mask bit did not latch");
        }
        __napi_schedule(&q->napi);
    }
}

static bool hsan_phy3_link(struct hsan_v300 *p);
static bool hsan_network_pipeline_ready(struct hsan_v300 *p);
static void hsan_adjust_link(struct net_device *ndev);

/* Stage58: a transient PHY link-down is recoverable via PHYLIB, but a
 * running GEMAC clock loss, MTU field change, or one-shot mode drift is
 * not. Stage57 dropped carrier for these failures while PIE RX DMA could
 * remain enabled indefinitely: its route audit checked FAM/PFE only.
 *
 * A snapshot is read-only and may sleep, so fetch it OUTSIDE tx_lock.
 * Serialize the terminal fault with TX/NAPI/IRQ publication, and retain
 * all potentially device-owned DMA maps; this is no hardware STOP ACK.
 * The same predicate is used from the PHY callback and 20ms watchdog.
 */
static bool hsan_gemac_runtime_qualified(struct hsan_v300 *p,
              const struct hsan_gemac_snapshot *mac, int snap_ret)
{
    unsigned long flags;

    if (!READ_ONCE(p->active))
        return false;
    if (!snap_ret && mac->gate_on && mac->mac_link_programmed &&
        !mac->link_hw_uncertain && !mac->link_register_drift_latched &&
        mac->link_register_readback_valid &&
        (mac->config_csr & HSAN_GEMAC_MTU_MASK) ==
            HSAN_GEMAC_LAN1_FRAME_LIMIT)
        return true;

    spin_lock_irqsave(&p->tx_lock, flags);
    if (READ_ONCE(p->active) &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        p->gemac_runtime_faults++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_GEMAC_RUNTIME,
            "GEMAC runtime clock/MTU/one-shot mode no longer qualified");
    }
    spin_unlock_irqrestore(&p->tx_lock, flags);
    return false;
}

static void hsan_watchdog(struct work_struct *w)
{
	struct hsan_v300 *p = container_of(to_delayed_work(w),
					   struct hsan_v300, watchdog);
	unsigned long flags;
	u16 tx_prod, tx_cons, pending;

	if (!READ_ONCE(p->active) ||
	    test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
		return;
    /* The once-only startup preflight is not a lifetime authorization:
     * PFE/IFC can become uncertain after ndo_open. Snapshot slowly (the
     * owner mutex and OEM readbacks may sleep) outside PIE's spinlock.
     * Never claim hardware forwarding when these checks pass.
     */
    if (time_after_eq(jiffies, READ_ONCE(p->last_route_audit) +
                      HSAN_ROUTE_RECHECK_JIFFIES)) {
        WRITE_ONCE(p->last_route_audit, jiffies);
        spin_lock_irqsave(&p->tx_lock, flags);
        p->runtime_route_audits++;
        spin_unlock_irqrestore(&p->tx_lock, flags);
        if (!hsan_network_pipeline_ready(p)) {
            spin_lock_irqsave(&p->tx_lock, flags);
            p->runtime_route_failures++;
            /* The read-only audit may sleep. A STOP or earlier fault
             * which won while it was running owns the final state.
             */
            if (READ_ONCE(p->active) &&
                !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_RUNTIME_ROUTE,
                    "PFE/IFC/FAM runtime handoff no longer qualified");
            spin_unlock_irqrestore(&p->tx_lock, flags);
            return;
        }
    }
    /* PHY link loss is not fatal; GEMAC gate/config drift IS fatal. The
     * 1Hz route audit above cannot detect MAC-only clock/MTU changes.
     * Check the independent owner each watchdog pass before NAPI/TX work.
     */
    {
        struct hsan_gemac_snapshot mac;
        int mac_ret = hsan_gemac_lan1_snapshot(&mac);

        if (!hsan_gemac_runtime_qualified(p, &mac, mac_ret))
            return;
    }
    /* Stop-start ordering only protected the initial RX4..7 snapshot.
     * Recheck while running so another owner cannot silently configure
     * an unsupported ring and use the shared RX global-enable bit.
     * Read-only polling under tx_lock is safe against ndo_stop's CPU
     * shutdown transition, but does not establish physical DMA drain.
     */
    if (time_after_eq(jiffies, READ_ONCE(p->last_other_rx_audit) +
                      HSAN_FOREIGN_RX_RECHECK_JIFFIES)) {
        spin_lock_irqsave(&p->tx_lock, flags);
        if (READ_ONCE(p->active) &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            p->last_other_rx_audit = jiffies;
            if (!hsan_unused_rx_runtime_clean_locked(p)) {
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_FOREIGN_RX_ACTIVE,
                    "unsupported GRO RX4..7 ring/IRQ became active at runtime");
                spin_unlock_irqrestore(&p->tx_lock, flags);
                return;
            }
        }
        spin_unlock_irqrestore(&p->tx_lock, flags);
    }
    /* TX completion IRQ is not reverse-qualified: timed reclaim only. */
	hsan_tx_reclaim(p);
	if (!READ_ONCE(p->active))
		return;
    spin_lock_irqsave(&p->tx_lock, flags);
    if (READ_ONCE(p->active))
        hsan_poll_owned_rx_locked(p);
    spin_unlock_irqrestore(&p->tx_lock, flags);
    /* A raw PHY link is not proof that GEMAC port10 has the negotiated
     * speed/duplex/pause programmed. Keep the PHYLIB link adjustment as
     * the sole carrier-on path; otherwise the periodic watchdog could
     * incorrectly override its failed MAC validation and submit DMA.
     */
    if (!hsan_phy3_link(p)) {
        netif_carrier_off(p->ndev);
    } else {
        hsan_adjust_link(p->ndev);
    }

    /* Stage60: the watchdog must not release tx_lock before reading
     * PIE pointers. ndo_stop can set active=false and disable RX
     * between the old software-counter snapshot and the unguarded
     * MMIO calls. Hold the STOP lock for the complete diagnostic
     * snapshot and for the terminal stall decision, then log the
     * copied values outside the IRQ-disabled critical section.
     * An independent DP/PFE owner can close the CPU FAM fence without
     * taking tx_lock while link sampling sleeps. Use the same separate
     * observation guard as TX reclaim before observing live cursors.
     */
    {
        u32 tx_hw_wptr = 0, tx_hw_rptr = 0;
        u32 rx_hw_wptr = 0, rx_hw_rptr = 0;
        bool stall_warn = false;
        enum hsan_dma_fault_code abort_code = 0;
        const char *abort_reason = NULL;

        spin_lock_irqsave(&p->tx_lock, flags);
        if (!READ_ONCE(p->active) ||
            test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            spin_unlock_irqrestore(&p->tx_lock, flags);
            return;
        }
        tx_prod = p->tx_prod;
        tx_cons = p->tx_cons;
        pending = (u16)(tx_prod - tx_cons);
        if (pending) {
            struct hsan_lsw_fam_publish_guard obs;

            if (hsan_lsw_fam_observe_begin(&obs)) {
                p->fam_external_fence_faults++;
                hsan_quarantine_datapath_locked(p,
                    HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                    "external FAM CPU STOP blocked TX watchdog snapshot");
                spin_unlock_irqrestore(&p->tx_lock, flags);
                return;
            }
            tx_hw_rptr = hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR);
            if (unlikely((u16)tx_hw_rptr >= p->tx_count)) {
                p->last_tx_hw_rptr = (u16)tx_hw_rptr;
                p->tx_hw_rptr_out_of_range++;
                p->tx_errors++;
                abort_code = HSAN_DMA_FAULT_TX_CONSUMER;
                abort_reason = "TX watchdog observed an out-of-range hardware ring slot";
            } else if ((u16)tx_hw_rptr == (tx_cons & p->tx_mask)) {
                p->stalled_ticks++;
                if (p->stalled_ticks == HSAN_STALL_WARN_TICKS &&
                    !p->reported_stall) {
                    p->reported_stall = true;
                    tx_hw_wptr = hsan_r32(p, HSAN_TX_CH + HSAN_CH_WPTR);
                    rx_hw_wptr = hsan_r32(p, HSAN_RX_CH + HSAN_CH_WPTR);
                    rx_hw_rptr = hsan_r32(p, HSAN_RX_CH + HSAN_CH_RPTR);
                    stall_warn = true;
                }
                if (p->stalled_ticks >= HSAN_STALL_ABORT_TICKS) {
                    abort_code = HSAN_DMA_FAULT_TX_STALLED;
                    abort_reason = "TX consumer failed to advance for five seconds";
                }
            } else {
                p->stalled_ticks = 0;
            }
            /* Quarantine itself closes the FAM fence; never call it
             * while the observation guard owns that same CSR spinlock.
             */
            hsan_lsw_fam_publish_end(&obs);
        } else {
            p->stalled_ticks = 0;
        }
        if (abort_reason)
            hsan_quarantine_datapath_locked(p, abort_code, abort_reason);
        spin_unlock_irqrestore(&p->tx_lock, flags);
        if (stall_warn && of_property_read_bool(p->dev->of_node,
                                  "hsan,experimental-cpu-lan1-diagnostic")) {
            struct hsan_lsw_fam_publish_guard obs;
            u8 desc[HSAN_DESC_BYTES];
            u32 controls[4];
            u64 counters[14] = { 0 };
            unsigned int i, j, sampled = 0;
            int counter_err = 0;

            spin_lock_irqsave(&p->tx_lock, flags);
            if (!hsan_lsw_fam_observe_begin(&obs)) {
                dma_rmb();
                memcpy(desc, hsan_tx_desc(p, tx_cons & p->tx_mask),
                       sizeof(desc));
                controls[0] = hsan_r32(p, 0x1000);
                controls[1] = hsan_r32(p, 0x2200);
                controls[2] = hsan_r32(p, HSAN_TX_CH + HSAN_CH_BASE);
                controls[3] = hsan_r32(p, HSAN_TX_CH + HSAN_CH_RING);
                /* OEM v300 DFX read command @0x27e4: type2 TX,
                 * index0..13 channel0; distinct from counter-clear writes.
                 * Bound busy polling, never replay a failed command.
                 */
                for (i = 0; i < ARRAY_SIZE(counters); i++) {
                    u32 status = 0;
                    unsigned int poll;

                    for (poll = 0; poll < 1001; poll++) {
                        status = hsan_r32(p, 0x1004);
                        if (!(status & BIT(30)))
                            break;
                    }
                    if (status & (BIT(30) | BIT(28))) {
                        counter_err = -EBUSY;
                        break;
                    }
                    hsan_w32(p, 0x1004, 0x40020000U | i);
                    for (poll = 0; poll < 1001; poll++) {
                        status = hsan_r32(p, 0x1004);
                        if (!(status & BIT(30)))
                            break;
                    }
                    if (status & (BIT(30) | BIT(28))) {
                        counter_err = -EIO;
                        break;
                    }
                    counters[i] = hsan_r32(p, 0x1010) |
                                  ((u64)hsan_r32(p, 0x1014) << 32);
                    sampled++;
                }
                hsan_lsw_fam_publish_end(&obs);
                netdev_warn(p->ndev,
                    "TX readonly snapshot: control=%08x/%08x base=%08x ring=%08x descriptor=%*phN\n",
                    controls[0], controls[1], controls[2], controls[3],
                    (int)sizeof(desc), desc);
            }
            spin_unlock_irqrestore(&p->tx_lock, flags);
            netdev_warn(p->ndev, "PIE TX DFX read sampled=%u rc=%d\n",
                        sampled, counter_err);
            for (i = 0; i < sampled; i++)
                netdev_warn(p->ndev, "PIE TX DFX index%u=%llu\n",
                            i, (unsigned long long)counters[i]);
            /* Same read-only counters as OEM ips_diag_get_cnt @0x63b0.
             * No diagnostic command writes and no ownership changes.
             */
            for (i = 0; i < 11; i++) {
                u32 counts[7] = { 0 };
                int err = 0;

                for (j = 0; j < ARRAY_SIZE(counts); j++) {
                    err = hsan_lsw_read(0x4400101c + i * 28 + j * 4,
                                        &counts[j]);
                    if (err)
                        break;
                }
                netdev_warn(p->ndev,
                    "IPS readonly row%u rc=%d counts=%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
                    i, err, counts[0], counts[1], counts[2], counts[3],
                    counts[4], counts[5], counts[6]);
                if (err)
                    break;
            }
        }
        if (stall_warn)
            netdev_warn(p->ndev,
                "TX stall 500ms: sw=%04x/%04x hw=%08x/%08x RX=%08x/%08x\n",
                tx_prod, tx_cons, tx_hw_wptr, tx_hw_rptr,
                rx_hw_wptr, rx_hw_rptr);
        if (abort_reason)
            return;
    }
	if (READ_ONCE(p->active))
		mod_delayed_work(system_wq, &p->watchdog, msecs_to_jiffies(20));
}

static netdev_tx_t hsan_start_xmit(struct sk_buff *skb,
				  struct net_device *ndev)
{
	struct hsan_v300 *p = netdev_priv(ndev);
	struct hsan_v300_tx *s;
	unsigned long flags;
	void *cpu;
	dma_addr_t dma;
	unsigned int len, idx;
    bool nonlinear;

	if (unlikely(test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)))
		goto drop;
    if (unlikely(!READ_ONCE(p->active) || !netif_carrier_ok(ndev))) {
        /* Neither DOWN nor link loss is a full TX ring. Returning BUSY
         * here without stopping the queue can retry indefinitely. The
         * network stack still owns skb, so consume/drop it explicitly.
         */
        spin_lock_irqsave(&p->tx_lock, flags);
        p->tx_state_drops++;
        spin_unlock_irqrestore(&p->tx_lock, flags);
        goto drop;
    }
    /* Drop a stale-owner packet before allocating/mapping a new DMA
     * buffer. The definitive race closure remains publish_begin() under
     * the TX lock immediately before handing the descriptor to hardware.
     */
    if (unlikely(hsan_lsw_dp_fam_cpu_fenced())) {
        unsigned long stop_flags;

        spin_lock_irqsave(&p->tx_lock, stop_flags);
        if (READ_ONCE(p->active) &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            p->fam_fence_tx_blocks++;
            p->fam_external_fence_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                "external FAM CPU STOP detected before TX DMA map");
        }
        spin_unlock_irqrestore(&p->tx_lock, stop_flags);
        goto drop;
    }
    /* OEM hi_pie_tx_send reclaims completed frames before it takes
     * another TX slot.  Do the same before checksum/payload allocation:
     * returning NETDEV_TX_BUSY on a full ring must not follow a change
     * to the caller-owned skb, nor spend DMA mapping work on a packet
     * that cannot currently be submitted.  This is only a preflight:
     * the final publish lock below still decides actual ownership.
     */
    spin_lock_irqsave(&p->tx_lock, flags);
    hsan_tx_reclaim_locked(p);
    if (unlikely(!READ_ONCE(p->active) ||
                 test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))) {
        bool fault = test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags);

        if (!fault)
            p->tx_state_drops++;
        spin_unlock_irqrestore(&p->tx_lock, flags);
        /* Inactive state is not ring congestion; avoid unbounded BUSY.
         * No skb checksum or DMA mapping has been attempted yet.
         */
        goto drop;
    }
    if (unlikely((u16)(p->tx_prod - p->tx_cons) >= p->tx_count - 1)) {
        netif_stop_queue(ndev);
        spin_unlock_irqrestore(&p->tx_lock, flags);
        return NETDEV_TX_BUSY;
    }
    spin_unlock_irqrestore(&p->tx_lock, flags);

    /* The v300 descriptor below represents ONE wire frame. Software
     * checksum fallback and skb_copy_bits support fragments, but do not
     * segment a GSO skb. Without audited OEM TSO/GSO descriptor fields,
     * never transmit an unsegmented super-packet on the physical link.
     * The driver advertises no HW GSO/TSO support to the networking core.
     */
    if (unlikely(skb_is_gso(skb))) {
        spin_lock_irqsave(&p->tx_lock, flags);
        p->tx_gso_unsupported_drops++;
        spin_unlock_irqrestore(&p->tx_lock, flags);
        goto drop;
    }

	len = max_t(unsigned int, skb->len, HSAN_MIN_TX);
	if (unlikely(len > HSAN_BUF_SIZE))
		goto drop;
    /* Allocate first: ENOMEM consumes/drops the skb, not BUSY retry.
     * skb_checksum_help() may turn a partial checksum into a full one;
     * that is performed only once the normal ring-full BUSY case has
     * already been excluded. After checksum failure we consume/drop.
     */
	cpu = kmalloc(len, GFP_ATOMIC);
    if (unlikely(!cpu)) {
        /* No legitimate ring capacity event: BUSY without stopping the
         * queue can spin on the same skb indefinitely. The vendor driver
         * also consumes failed TX skbs, whereas Linux ndo_start_xmit
         * uses NETDEV_TX_OK even for an explicitly counted drop.
         * No DMA ownership has changed at this point.
         */
        spin_lock_irqsave(&p->tx_lock, flags);
        p->tx_nomem_drops++;
        spin_unlock_irqrestore(&p->tx_lock, flags);
        goto drop;
    }
    if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb)) {
        kfree(cpu);
        goto drop;
    }
    nonlinear = skb_is_nonlinear(skb);
	if (skb_copy_bits(skb, 0, cpu, skb->len)) {
		kfree(cpu);
		goto drop;
	}
	if (skb->len < len)
		memset(cpu + skb->len, 0, len - skb->len);
	dma = dma_map_single(p->dev, cpu, len, DMA_TO_DEVICE);
	if (dma_mapping_error(p->dev, dma)) {
		kfree(cpu);
		goto drop;
	}
	if (unlikely(!hsan_dma32(dma, len))) {
		dma_unmap_single(p->dev, dma, len, DMA_TO_DEVICE);
		kfree(cpu);
		goto drop;
	}
	spin_lock_irqsave(&p->tx_lock, flags);
    /* Already reclaimed and checked capacity before touching skb.
     * skb_checksum_help may have modified the skb by now, so a late
     * failure MUST consume the skb, never return NETDEV_TX_BUSY. The
     * usual single-queue TX serialization protects capacity; the
     * final lock still checks STOP/quarantine before publishing DMA.
     */
    if (READ_ONCE(p->active) && hsan_lsw_dp_fam_cpu_fenced() &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        p->fam_external_fence_faults++;
        p->fam_fence_tx_blocks++;
        hsan_quarantine_datapath_locked(p,
            HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
            "FAM CPU producer fence closed before TX submit");
    }
	/* Reclaim can quarantine DMA after a corrupt HW consumer. A race with
	 * ndo_stop can also deactivate the controller after the first test.
	 * Neither condition may submit an additional device-owned mapping.
	 */
    if (unlikely(!READ_ONCE(p->active) || !netif_carrier_ok(ndev) ||
                 test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))) {
        p->tx_state_drops++;
        spin_unlock_irqrestore(&p->tx_lock, flags);
        /* Mapping has NOT been exposed in an HW descriptor yet. It can
         * be unmapped even if an unrelated FAM CPU STOP just closed.
         */
        dma_unmap_single(p->dev, dma, len, DMA_TO_DEVICE);
        kfree(cpu);
        goto drop;
    }
    if (unlikely((u16)(p->tx_prod - p->tx_cons) >= p->tx_count - 1)) {
        /* This cannot be ordinary congestion after the preflight with
         * a single TX queue. Treat unexpected producer reentrancy or
         * corrupted capacity bookkeeping as a terminal invariant fault.
         * The skb may already be checksummed: never hand it back BUSY.
         */
        p->tx_late_ring_faults++;
        hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_LATE_RING,
                   "TX ring capacity changed after skb staging");
        spin_unlock_irqrestore(&p->tx_lock, flags);
        dma_unmap_single(p->dev, dma, len, DMA_TO_DEVICE);
        kfree(cpu);
        goto drop;
    }
	idx = p->tx_prod & p->tx_mask;
	s = &p->tx[idx];
    if (unlikely(s->buffer)) {
        /* A supposedly free slot with an in-flight DMA mapping is
         * corruption, not a recoverable packet drop or ring-full event.
         */
        hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_SLOT, "TX free slot still DMA-owned");
		spin_unlock_irqrestore(&p->tx_lock, flags);
		dma_unmap_single(p->dev, dma, len, DMA_TO_DEVICE);
		kfree(cpu);
		goto drop;
	}
    {
        struct hsan_lsw_fam_publish_guard pub;

        if (hsan_lsw_fam_publish_begin(&pub)) {
            p->fam_external_fence_faults++;
            p->fam_fence_tx_blocks++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_FAM_EXTERNAL_FENCE,
                "FAM CPU stop raced TX WPTR publication");
            spin_unlock_irqrestore(&p->tx_lock, flags);
            dma_unmap_single(p->dev, dma, len, DMA_TO_DEVICE);
            kfree(cpu);
            goto drop;
        }
        s->buffer = cpu;
        s->dma = dma;
        s->len = len;
        s->payload_len = skb->len;
        if (nonlinear)
            p->tx_nonlinear_flattened++;
        hsan_tx_build_desc(p, idx, dma, len);
        dma_wmb();
        p->tx_prod++;
        /* hi_pie_tx_send: (node_index + 1) & ((1<<tx_order)-1).
         * hi_pie_tx_set_wptr stores that slot as an entire 32-bit word.
         * Software accounting stays cumulative, but HW only sees the
         * ring-modulo cursor. Never publish a cumulative count to HW.
         */
        p->last_tx_published_wptr = p->tx_prod & p->tx_mask;
        if (!p->last_tx_published_wptr)
            p->tx_slot_cursor_wraps++;
        hsan_w32(p, HSAN_TX_CH + HSAN_CH_WPTR,
                 p->last_tx_published_wptr);
        /* Do not free this mapping on a mismatch: ownership is uncertain.
         * Publish guard MUST be released before quarantine calls STOP.
         */
        if (!hsan_check_wptr_readback(p, NULL,
                                      p->last_tx_published_wptr)) {
            hsan_lsw_fam_publish_end(&pub);
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_TX_WPTR_READBACK,
                "TX WPTR CPU-visible readback differs after publication");
        } else {
            hsan_lsw_fam_publish_end(&pub);
        }
    }
	if ((u16)(p->tx_prod - p->tx_cons) >= p->tx_count - 1)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&p->tx_lock, flags);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

drop:
	spin_lock_irqsave(&p->tx_lock, flags);
	p->tx_errors++;
	p->tx_dropped++;
	spin_unlock_irqrestore(&p->tx_lock, flags);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void hsan_free_unstarted(struct hsan_v300 *p)
{
    unsigned int ch, i;

    if (WARN_ON_ONCE(p->dma_ever_started))
        return;
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        if (q->rx) {
            for (i = 0; i < q->rx_count; i++) {
                if (!q->rx[i].buffer)
                    continue;
                dma_unmap_single(p->dev, q->rx[i].dma, q->buffer_size,
                                 DMA_FROM_DEVICE);
                kfree(q->rx[i].buffer);
            }
        }
        kvfree(q->rx);
        q->rx = NULL;
        if (q->rx_desc)
            dma_free_coherent(p->dev, q->rx_count * HSAN_DESC_BYTES,
                              q->rx_desc, q->rx_desc_dma);
        q->rx_desc = NULL;
    }
    kvfree(p->tx);
    p->tx = NULL;
    if (p->tx_desc)
        dma_free_coherent(p->dev, p->tx_count * HSAN_DESC_BYTES,
                          p->tx_desc, p->tx_desc_dma);
    p->tx_desc = NULL;
    p->initialized = false;
}

static int hsan_alloc_queues(struct hsan_v300 *p)
{
    unsigned int ch, i;
    int ret = -ENOMEM;

    if (p->initialized)
        return 0;
    p->tx_desc = dma_alloc_coherent(p->dev,
        p->tx_count * HSAN_DESC_BYTES, &p->tx_desc_dma, GFP_KERNEL);
    if (!p->tx_desc)
        return -ENOMEM;
    if (!hsan_dma32(p->tx_desc_dma, p->tx_count * HSAN_DESC_BYTES)) {
        ret = -ERANGE;
        goto err;
    }
    p->tx = kvcalloc(p->tx_count, sizeof(*p->tx), GFP_KERNEL);
    if (!p->tx)
        goto err;
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        q->rx_desc = dma_alloc_coherent(p->dev,
            q->rx_count * HSAN_DESC_BYTES, &q->rx_desc_dma, GFP_KERNEL);
        if (!q->rx_desc)
            goto err;
        if (!hsan_dma32(q->rx_desc_dma, q->rx_count * HSAN_DESC_BYTES)) {
            ret = -ERANGE;
            goto err;
        }
        q->rx = kvcalloc(q->rx_count, sizeof(*q->rx), GFP_KERNEL);
        if (!q->rx)
            goto err;
        for (i = 0; i < q->rx_count; i++) {
            struct hsan_v300_rx *slot = &q->rx[i];

            slot->buffer = kmalloc(q->buffer_size, GFP_KERNEL);
            if (!slot->buffer)
                goto err;
            slot->dma = dma_map_single(p->dev, slot->buffer,
                                      q->buffer_size, DMA_FROM_DEVICE);
            if (dma_mapping_error(p->dev, slot->dma)) {
                kfree(slot->buffer);
                slot->buffer = NULL;
                goto err;
            }
            if (!hsan_dma32(slot->dma, q->buffer_size)) {
                ret = -ERANGE;
                goto err;
            }
        }
    }
    p->initialized = true;
    return 0;
err:
    hsan_free_unstarted(p);
    return ret;
}

static void hsan_disable_owned_napi(struct hsan_v300 *p)
{
    unsigned int ch;

    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        if (q->napi_enabled) {
            napi_disable(&q->napi);
            q->napi_enabled = false;
        }
    }
}

static void hsan_free_owned_irqs(struct hsan_v300 *p)
{
    unsigned int ch;

    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        if (q->irq_ok) {
            unsigned long flags;

            /* free_irq may wait for the ISR, which also takes tx_lock.
             * Only the flag update is serialized with the poll tail:
             * napi_disable need not have waited for that tail to exit.
             */
            if (ch < HSAN_NET_RX_CHANNELS)
                free_irq(q->irq, q);
            spin_lock_irqsave(&p->tx_lock, flags);
            q->irq_ok = false;
            spin_unlock_irqrestore(&p->tx_lock, flags);
        }
    }
}

/* No MMIO write: check all four ordinary channels and the shared global
 * enable immediately before publication. RX4..7 still need the separate
 * foreign-owner inspection even when gro_ring is absent from this DT.
 */
static bool hsan_owned_rx_idle(struct hsan_v300 *p)
{
    u32 route = hsan_r32(p, HSAN_RX_IRQ_ROUTE);
    u32 large = hsan_r32(p, HSAN_RX_LARGE_BUFFER);
    u32 bad = 0;
    unsigned int ch;

    p->rx_init_bad_fields = 0;
    if (hsan_r32(p, HSAN_RX_GLOBAL) & HSAN_RX_GLOBAL_ENABLE)
        bad |= BIT(7);
    if (hsan_r32(p, HSAN_TX_CH + HSAN_CH_BASE))
        bad |= BIT(8);
    if (hsan_r32(p, HSAN_TX_CH + HSAN_CH_RING))
        bad |= BIT(9);
    if (hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR) ||
        hsan_r32(p, HSAN_TX_CH + HSAN_CH_WPTR))
        bad |= BIT(10);
    if (bad) {
        p->rx_init_bad_channel = HSAN_OEM_RX_CHANNELS; /* TX/global */
        p->rx_init_bad_fields = bad;
        return false;
    }
    for (ch = 0; ch < p->rx_channels; ch++) {
        u32 old_group = (route >> (2 * ch)) & 3;

        bad = 0;
        /* rx_init_chan uses OR, so a stale conflicting route cannot be
         * repaired by replaying OEM initialization. Refuse it unchanged.
         * Zero cursors do not prove that a pre-existing BASE/RING is
         * unowned: no audited hardware STOP/handoff exists in this port.
         */
        if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE)))
            bad |= BIT(0);
        if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RING)))
            bad |= BIT(1);
        if (old_group && old_group !=
            (ch < HSAN_NET_RX_CHANNELS ? ch : HSAN_OEM_RX_CHANNELS - 1 - ch))
            bad |= BIT(3);
        if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR)) ||
            hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR)))
            bad |= BIT(4);
        if (large & BIT(ch))
            bad |= BIT(6);
        if (bad) {
            p->rx_init_bad_channel = ch;
            p->rx_init_bad_fields = bad;
            return false;
        }
    }
    return true;
}

/* Exact v300 hi_pie_rx_init_chan ordinary-RX branch, including the buffer
 * limit and IRQ routing which the previous RX0 port omitted. Both locks
 * are held, all DMA allocations are pinned before the first BASE write.
 * Readback confirms CPU-visible programming only, never hardware readiness.
 */
static bool hsan_program_rx_channel(struct hsan_v300_rx_queue *q)
{
    struct hsan_v300 *p = q->parent;
    unsigned int ch = q->channel;
    u32 limit_reg = HSAN_RX_BUFFER_LIMIT + ch * HSAN_OEM_RX_CH_STRIDE;
    u32 route = hsan_r32(p, HSAN_RX_IRQ_ROUTE);
    u32 bad = 0;
    u32 group = ch < HSAN_NET_RX_CHANNELS ? ch : HSAN_OEM_RX_CHANNELS - 1 - ch;
    u32 large = hsan_r32(p, HSAN_RX_LARGE_BUFFER);

    hsan_w32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE), (u32)q->rx_desc_dma);
    hsan_w32(p, hsan_rx_chan_reg(ch, HSAN_CH_RING), q->ring_order);
    hsan_w32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR), 0);
    hsan_w32(p, limit_reg, q->buffer_size >> 7);
    hsan_w32(p, HSAN_RX_IRQ_ROUTE, route | (group << (2 * ch)));
    large = q->buffer_size > 9600 ? large | BIT(ch) : large & ~BIT(ch);
    hsan_w32(p, HSAN_RX_LARGE_BUFFER, large);
    hsan_rx_publish_producer_cursor(q);
    hsan_w32(p, hsan_rx_irq_reg(ch, false), BIT(0));
    hsan_w32(p, hsan_rx_irq_reg(ch, true), BIT(0));
    if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE)) !=
        (u32)q->rx_desc_dma)
        bad |= BIT(0);
    if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RING)) != q->ring_order)
        bad |= BIT(1);
    if (hsan_r32(p, limit_reg) != q->buffer_size >> 7)
        bad |= BIT(2);
    if (((hsan_r32(p, HSAN_RX_IRQ_ROUTE) >> (2 * ch)) & 3) != group)
        bad |= BIT(3);
    if (!hsan_check_wptr_readback(p, q, hsan_rx_published_cursors(q)))
        bad |= BIT(4);
    if (!(hsan_r32(p, hsan_rx_irq_reg(ch, true)) & BIT(0)))
        bad |= BIT(5);
    if (!!(hsan_r32(p, HSAN_RX_LARGE_BUFFER) & BIT(ch)) !=
        (q->buffer_size > 9600))
        bad |= BIT(6);
    if (bad) {
        p->rx_init_bad_channel = ch;
        p->rx_init_bad_fields = bad;
    }
    return !bad;
}

/* Four OEM clock gates, read-only.  The DT clock-provider nodes being
 * disabled says nothing definitive about physical gate state; use MMIO
 * readback and NEVER infer enable state solely from an FDT status string.
 */
static bool hsan_clock_gates_ready(struct hsan_v300 *p)
{
	static const struct {
		phys_addr_t addr;
		u32 required;
		const char *name;
	} gates[] = {
		{ 0x10108254U, BIT(0), "PIE" },
		{ 0x14001020U, BIT(7), "LAN1 GEMAC" },
		{ 0x14980024U, BIT(1) | BIT(2), "LSW DP+PFE" },
		{ 0x10113180U, BIT(5), "PIE ACP" },
	};
	bool ok = true;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		void __iomem *reg = ioremap(gates[i].addr, sizeof(u32));
		u32 v;

		if (!reg) {
			dev_err(p->dev, "cannot read clock/ACP gate %s\n", gates[i].name);
			ok = false;
			continue;
		}
		v = readl(reg);
		iounmap(reg);
		dev_info(p->dev, "gate-readonly %s @%pa: value=%08x required=%08x\n",
			 gates[i].name, &gates[i].addr, v, gates[i].required);
		if ((v & gates[i].required) != gates[i].required)
			ok = false;
	}
	return ok;
}

/* OEM hi_kpie .init.text hi_pie_probe @0xc4..0xdc: assert/deassert
 * before publishing new descriptors. This is a cold-boot operation only,
 * never a runtime DMA drain, ownership return or safe-unmap assertion.
 */
static int hsan_pie_boot_reset(struct platform_device *pdev, struct hsan_v300 *p)
{
	struct reset_control *reset;
	struct of_phandle_args spec;
	void __iomem *clock_gate;
	u32 clock_value;
	unsigned int ch;
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				   "hsan,experimental-reset-pie-at-boot"))
		return 0;
	/* OEM probe enables the PIE bus clock before reset. ACP is a later
	 * datapath prerequisite, still checked independently before DMA.
	 */
	clock_gate = ioremap(0x10108254U, sizeof(u32));
	if (!clock_gate)
		return -ENOMEM;
	clock_value = readl(clock_gate);
	iounmap(clock_gate);
	dev_info(&pdev->dev, "PIE reset prerequisite bus clock=%#x\n", clock_value);
	if (!(clock_value & BIT(0)))
		return -EHOSTDOWN;
	if (of_count_phandle_with_args(pdev->dev.of_node, "resets", "#reset-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(pdev->dev.of_node, "resets",
					 "#reset-cells", 0, &spec);
	if (ret)
		return ret;
	ret = spec.args_count == 2 && spec.args[0] == 0x454 && spec.args[1] == 0 ?
		0 : -EINVAL;
	of_node_put(spec.np);
	if (ret)
		return ret;
	reset = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	if (atomic_cmpxchg(&hsan_pie_boot_reset_attempted, 0, 1))
		return -EALREADY;
	__module_get(THIS_MODULE);
	ret = reset_control_assert(reset);
	if (ret)
		return ret;
	ret = reset_control_status(reset);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;
	ret = reset_control_status(reset);
	if (ret)
		return ret < 0 ? ret : -EIO;
	dev_info(&pdev->dev, "PIE post-reset RX global=%#x TX base=%#x\n",
		 hsan_r32(p, HSAN_RX_GLOBAL), hsan_r32(p, HSAN_TX_CH + HSAN_CH_BASE));
	if (hsan_r32(p, HSAN_TX_CH + HSAN_CH_BASE))
		return -EBUSY;
	/* v300 reset defaults RX global to 0x2001. Explicitly disable the
	 * documented bit0 before configuring new rings; preserve all others.
	 * This is initial admission control, not a DMA drain acknowledgment.
	 */
	hsan_w32(p, HSAN_RX_GLOBAL,
		 hsan_r32(p, HSAN_RX_GLOBAL) & ~HSAN_RX_GLOBAL_ENABLE);
	if (hsan_r32(p, HSAN_RX_GLOBAL) & HSAN_RX_GLOBAL_ENABLE)
		return -EBUSY;
	for (ch = 0; ch < HSAN_OEM_RX_CHANNELS; ch++)
		if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE)))
			return -EBUSY;
	p->boot_reset_verified = true;
	dev_info(&pdev->dev,
		 "OEM PIE boot reset verified: RX disabled, eight RX bases and TX base zero; not runtime STOP ACK\n");
	return 0;
}

/* Original hi_acp.ko hi_acp_hw_set_cache_attr @.text+0x1b4 computes
 * (old & ~mask) | (value & mask).  OEM hi_pie_reg_init explicitly calls
 * hi_acp_notifier_call_chain(7), the "hsan,acp-pie" notifier.  Stage11
 * had partial ACP writes, so a clock bit ALONE cannot qualify DMA.
 * Program only on explicit cold-boot opt-in; open only verifies readback.
 */
/* OEM v300 hi_pie_reg_init @25c8..2750 precedes ring publication.
 * Cold reset alone does not install these controller fields. Keep RX
 * disabled and every ring unpublished throughout this one-shot prelude.
 */
static int hsan_pie_boot_globals(struct hsan_v300 *p)
{
	static const struct { u32 offset, mask, value; } fields[] = {
		{ 0x1000, 0x1c1, 0x1c0 },
		{ 0x3208, U32_MAX, 125 },
		{ 0x3204, U32_MAX, 0 },
		{ 0x3200, 3, 0 }, /* OEM ordinary RX count=4, not single-RX mode */
		{ 0x2200, BIT(22), 0 },
		{ 0x3408, U32_MAX, 0 },
		{ 0x3404, U32_MAX, 0 },
	};
	unsigned int i, ch;
	u32 value;

	if (!of_property_read_bool(p->dev->of_node,
				  "hsan,experimental-program-pie-global-registers"))
		return 0;
	if (!p->boot_reset_verified ||
	    (hsan_r32(p, HSAN_RX_GLOBAL) & HSAN_RX_GLOBAL_ENABLE) ||
	    hsan_r32(p, HSAN_TX_CH + HSAN_CH_BASE))
		return -EBUSY;
	for (ch = 0; ch < HSAN_OEM_RX_CHANNELS; ch++)
		if (hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE)))
			return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		value = hsan_r32(p, fields[i].offset);
		hsan_w32(p, fields[i].offset,
			 (value & ~fields[i].mask) | fields[i].value);
		value = hsan_r32(p, fields[i].offset);
		if ((value & fields[i].mask) != fields[i].value) {
			dev_err(p->dev, "PIE global %#x readback=%#x mask=%#x expected=%#x\n",
				fields[i].offset, value, fields[i].mask, fields[i].value);
			return -EUCLEAN;
		}
	}
	for (ch = 0; ch < HSAN_OEM_RX_CHANNELS; ch++) {
		hsan_w32(p, 0x3210 + ch * 4, 3);
		hsan_w32(p, 0x3310 + ch * 4, 8);
		hsan_w32(p, 0x3290 + ch * 4, 2);
		if (hsan_r32(p, 0x3210 + ch * 4) != 3 ||
		    hsan_r32(p, 0x3310 + ch * 4) != 8 ||
		    hsan_r32(p, 0x3290 + ch * 4) != 2) {
			dev_err(p->dev, "PIE global threshold readback mismatch channel=%u\n", ch);
			return -EUCLEAN;
		}
		/* OEM interrupt initialization includes W1C status writes;
		 * validate the persistent masks, never invent a status ACK.
		 */
		hsan_w32(p, 0x380 + ch * 4, 1);
		hsan_w32(p, 0x400 + ch * 4, 1);
		hsan_w32(p, 0x180 + ch * 4, 1);
		hsan_w32(p, 0x200 + ch * 4, 1);
		if (!(hsan_r32(p, 0x400 + ch * 4) & 1) ||
		    !(hsan_r32(p, 0x200 + ch * 4) & 1))
			return -EUCLEAN;
	}
	/* OEM @2724..272c writes 0x3bff without a readback contract.
	 * Actual cold-boot hardware reads zero after this write; it must
	 * not be verified as a persistent configuration field.
	 */
	hsan_w32(p, 0x3600, 0x3bff);
	p->boot_globals_verified = true;
	dev_info(p->dev,
		 "OEM PIE v300 globals verified: 7 fields, 24 thresholds, RX disabled, IRQs masked; 0x3600 submitted only\n");
	return 0;
}

static bool hsan_acp_pie_attrs_ready(struct hsan_v300 *p, bool program)
{
	struct device_node *np = p->dev->of_node;
	u32 a[27];
	u32 gate[3];
	unsigned int i;
	int ret;

    if (of_property_read_bool(np, "hsan,experimental-noncoherent-dma")) {
        /* Conservative Linux DMA cache maintenance, no ACP bypass.
         * The FDT must omit dma-coherent before platform DMA setup.
         */
        if (of_property_read_bool(np, "dma-coherent") ||
            dev_is_dma_coherent(p->dev))
            return false;
        dev_info(p->dev, "PIE diagnostic uses noncoherent Linux DMA cache maintenance\n");
    } else if (!of_property_read_bool(np, "dma-coherent")) {
        dev_warn(p->dev, "ACP PIE coherent declaration absent; DMA blocked\n");
        return false;
    }
	/* The fixed-array OF helper accepts a longer property. Require the
	 * exact byte length, not merely a valid prefix of nine tuples.
	 */
	ret = of_property_read_variable_u32_array(np, "acp-later-cfg-attr",
						 a, ARRAY_SIZE(a), ARRAY_SIZE(a));
	if (ret < 0) {
		dev_err(p->dev, "ACP PIE expected exactly nine FDT attribute tuples: %d\n",
			ret);
		return false;
	}
	/* Validate every tuple before reading any ACP attribute CSR. */
	for (i = 0; i < ARRAY_SIZE(a) / 3; i++) {
		u32 addr = a[3 * i], mask = a[3 * i + 1];
		u32 desired = a[3 * i + 2];
		u32 expected_mask = i == 8 ? 0xffU : 0x0fff0fffU;
		u32 expected_value = i == 8 ? 0xbbU : 0x0ffb0bfbU;

		if (addr != HSAN_PIE_ADDR + 0x1040U + 4 * i ||
		    mask != expected_mask ||
		    (desired & mask) != (expected_value & mask)) {
			dev_err(p->dev, "ACP PIE FDT tuple %u unexpected\n", i);
			return false;
		}
	}
	if (program) {
		void __iomem *reg;
		u32 value;

		if (!p->boot_reset_verified ||
		    of_property_count_u32_elems(np, "acp-gate") != 3 ||
		    of_property_read_u32_array(np, "acp-gate", gate, 3) ||
		    gate[0] != 0x10113180U || gate[1] != BIT(5) ||
		    gate[2] != BIT(5))
			return false;
		reg = ioremap(gate[0], sizeof(u32));
		if (!reg)
			return false;
		value = readl(reg);
		writel((value & ~gate[1]) | gate[2], reg);
		value = readl(reg);
		iounmap(reg);
		if ((value & gate[1]) != gate[2])
			return false;
		for (i = 0; i < ARRAY_SIZE(a) / 3; i++) {
			u32 offset = a[3 * i] - HSAN_PIE_ADDR;
			u32 mask = a[3 * i + 1];
			u32 desired = a[3 * i + 2];

			value = hsan_r32(p, offset);
			hsan_w32(p, offset, (value & ~mask) | (desired & mask));
		}
	}

	for (i = 0; i < ARRAY_SIZE(a) / 3; i++) {
		u32 addr = a[3 * i], mask = a[3 * i + 1];
		u32 desired = a[3 * i + 2], found;

		found = hsan_r32(p, addr - HSAN_PIE_ADDR);
		if ((found & mask) != (desired & mask)) {
			dev_warn(p->dev,
				 "ACP PIE attr%u @%08x=%08x expected=%08x mask=%08x; DMA blocked\n",
				 i, addr, found, desired, mask);
			return false;
		}
	}
	dev_info(p->dev, "ACP PIE nine OEM attribute fields verified (%s)\n",
		 program ? "cold boot programmed" : "read only");
	return true;
}

static bool hsan_phy3_link(struct hsan_v300 *p)
{
	struct hsan_gemac_link_snapshot link;
    struct hsan_gemac_snapshot mac;
	int ret = hsan_gemac_lan1_link_snapshot(&link);

	if (ret) {
		dev_dbg(p->dev, "GEMAC-owned PHY3 C22 snapshot unavailable: %d\n", ret);
		return false;
	}
    if (link.link_up && !link.resolved)
        dev_dbg(p->dev, "PHY3 linked but speed/duplex unresolved, MAC programming blocked\n");
    if (!link.link_up || !link.resolved ||
        (link.aneg_enabled && !link.aneg_complete))
        return false;
    /* A PHY speed/duplex change after the one-shot OEM MAC register
     * sequence is NOT safe to treat as carrier up. Full phylink hot
     * reconfiguration remains unqualified.
     */
    if (READ_ONCE(p->active)) {
        /* Once attached, PHYLIB is the link-state owner. Do not allow
         * watchdog reads to override a PHYLIB link-down notification.
         */
        struct phy_device *phy = p->ndev->phydev;

        if (!p->phy_connected || !phy || !phy->link ||
            READ_ONCE(phy->speed) != link.speed_mbps ||
            (READ_ONCE(phy->duplex) == DUPLEX_FULL) != link.full_duplex)
            return false;
        ret = hsan_gemac_lan1_snapshot(&mac);
        if (ret || !mac.mac_link_programmed ||
            mac.programmed_speed_mbps != link.speed_mbps ||
            mac.programmed_full_duplex != link.full_duplex ||
            mac.programmed_rx_pause != link.rx_pause ||
            mac.programmed_tx_pause != link.tx_pause)
            return false;
    }
    return true;
}

static bool hsan_network_pipeline_ready(struct hsan_v300 *p)
{
	struct device_node *dp, *pfe;
    struct hsan_dp_queue_snapshot q;
    struct hsan_dp_init_snapshot dp_init;
    struct hsan_pfe_snapshot state;
    struct hsan_pfe_lifecycle_snapshot pfe_init;
    int dp_ret, pfe_ret;
	bool ok;

    dp = of_find_compatible_node(NULL, NULL, "hsan,dp");
    pfe = of_find_compatible_node(NULL, NULL, "hsan,pfe");
	ok = dp && pfe && of_device_is_available(dp) &&
	     of_device_is_available(pfe);
	of_node_put(dp);
	of_node_put(pfe);
	if (!ok) {
		dev_warn(p->dev,
			 "LSW DP/PFE disabled: refusing DMA until both nodes are enabled\n");
		return false;
	}
    /* A retained binding after a failed hardware write deliberately
     * remains Live to preserve resources. It is not an initialized owner.
     * Read these CPU-only snapshots before the optional MMIO diagnostics.
     */
    dp_ret = hsan_lsw_dp_init_snapshot(&dp_init);
    pfe_ret = hsan_lsw_pfe_lifecycle_snapshot(&pfe_init);
    if (dp_ret || pfe_ret || dp_init.quarantined || pfe_init.quarantined) {
        dev_warn(p->dev,
                 "datapath init unavailable/quarantined: DP=%d PFE=%d\n",
                 dp_ret ? dp_ret : dp_init.last_errno,
                 pfe_ret ? pfe_ret : -(int)pfe_init.last_errno);
        return false;
    }
    if (!p->boot_reset_verified) {
        dev_warn(p->dev, "PIE initial reset and empty RX bases not verified\n");
        return false;
    }
    if (!dp_init.queue_routing_verified) {
        dev_warn(p->dev, "DP queue routes have not completed hardware readback\n");
        return false;
    }
    if (!dp_init.pie_map_geometry_valid ||
        dp_init.pie_gro_start != HSAN_NET_RX_CHANNELS ||
        dp_init.pie_gro_count != p->gro_channels) {
        dev_warn(p->dev,
                 "DP/PIE queue geometry mismatch: map_known=%d GRO start=%u count=%u; driver ordinary=%u GRO=%u\n",
                 dp_init.pie_map_geometry_valid, dp_init.pie_gro_start,
                 dp_init.pie_gro_count, HSAN_NET_RX_CHANNELS,
                 p->gro_channels);
        return false;
    }
    /* Detailed read-only evidence identifies which owner is absent.
     * A successful map, software table or MDIO link alone is insufficient.
     * The final CSR predicate remains deliberately fail-closed.
     */
    dp_ret = hsan_lsw_dp_get_queue(HSAN_LAN1_PORT, &q);
    pfe_ret = hsan_lsw_pfe_snapshot(&state);
    if (dp_ret || pfe_ret) {
        dev_warn(p->dev, "datapath owner not published: DP=%d PFE=%d\n",
                 dp_ret, pfe_ret);
        return false;
    }
    if (!state.clock_on || !state.global_attr_shadow ||
        !state.pub_gro_count_known ||
        state.pub_gro_entries != p->gro_channels ||
        (state.pub_shadows_expected != 8 &&
         state.pub_shadows_expected != 9) ||
        state.pub_shadows != state.pub_shadows_expected ||
        state.opp_shadows != 2) {
        dev_warn(p->dev,
                 "PFE owners incomplete: clock=%d global=%d pub=%u/%u GRO=%u known=%d opp=%u\n",
                 state.clock_on, state.global_attr_shadow,
                 state.pub_shadows, state.pub_shadows_expected,
                 state.pub_gro_entries, state.pub_gro_count_known,
                 state.opp_shadows);
        return false;
    }
    if (!q.reserved_memory_verified || !q.fam_addresses_prepared ||
        !q.fam_cpu_table_verified || !q.fam_dma_attrs_zero ||
        !q.fam_hw_owned || !q.fam_hw_ack_seen ||
        q.fam_cpu_stop_fenced ||
        !q.fam_oem_runtime_geometry_verified ||
        !q.eqm_bootstrap_committed || !q.eqm_queue_rows_committed ||
        !q.pie_map_hw_committed || q.pie_map_hw_uncertain ||
        !state.global_hw_init_completed || state.tfrt_hw_uncertain) {
        dev_warn(p->dev,
                 "datapath not qualified: mem=%d fam_staging=%d fam_ack=%d eqm=%d/%d pie=%d/%d pfe_globals=%d tfrt_uncertain=%d\n",
                 q.reserved_memory_verified, q.fam_addresses_prepared,
                 q.fam_hw_ack_seen, q.eqm_bootstrap_committed, q.eqm_queue_rows_committed,
                 q.pie_map_hw_committed, q.pie_map_hw_uncertain,
                 state.global_hw_init_completed, state.tfrt_hw_uncertain);
        return false;
    }
    /* This admission predicate covers the existing full OEM profile,
     * including its IFC/L2/L3/PA/NAPT/OFC/MCC owners. It does not establish
     * that NAT or every offload engine is required for basic LAN ARP.
     * A separately qualified minimal PIE-to-GEMAC path is still missing;
     * MMIO initialization or table shadows alone cannot substitute for it.
     */
    /* A bounded CPU diagnostic uses the exact OPP/PUB/IFC bootstrap,
     * without claiming that NAT/L2/L3 offload or hardware STOP works.
     * DP ownership, map, clock, PHY and all ring checks still apply.
     */
    if (of_property_read_bool(p->dev->of_node,
                             "hsan,experimental-cpu-lan1-diagnostic"))
        return p->boot_globals_verified && hsan_lsw_pfe_cpu_bootstrap_ready();
    if (state.forwarding_blockers) {
        dev_warn(p->dev,
                 "PFE full OEM profile incomplete: blockers=%#x (IFC/PA/NAPT/OFC-MCC/L2-L3); minimal LAN path not yet qualified\n",
                 state.forwarding_blockers);
        return false;
    }
	/* A changed DT status is only a *declaration*; the 17 OEM CSR banks,
	 * FAM/queue/PIE mapping and PFE tables must actually be installed.
	 * The LSW registrar returns false until that handoff is verified.
	 */
	if (!hsan_lsw_pipeline_operational()) {
		dev_warn(p->dev,
			 "LSW mapped but FAM/queue/PFE initialization not qualified: refusing DMA\n");
		return false;
	}
	return true;
}

/* The PIE netdev and OEM GEMAC are separate platform devices. The
 * cross-module MAC snapshot is read-only, but prevents a running PIE
 * controller from assuming GEMAC/PHY3 configuration from DT status alone.
 * The phylink speed/duplex/pause write sequence remains unqualified.
 */
static int hsan_gemac_preflight(struct hsan_v300 *p)
{
    struct hsan_gemac_snapshot mac;
    int ret;

    ret = hsan_gemac_lan1_snapshot(&mac);
    if (ret)
        return ret;
    if (!mac.gate_on || mac.port != HSAN_LAN1_PORT || mac.phy_addr != 3)
        return -ENODEV;
    if (mac.link_hw_uncertain)
        return -EUCLEAN;
    if ((mac.config_csr & HSAN_GEMAC_MTU_MASK) !=
        HSAN_GEMAC_LAN1_FRAME_LIMIT) {
        dev_warn(p->dev,
                 "GEMAC port10 CSR0x100 frame limit mismatch: %#x; DMA refused\n",
                 mac.config_csr);
        return -EMSGSIZE;
    }
    dev_dbg(p->dev, "GEMAC port10 snapshot %#x, PHY3, gate ON (read only)\n",
            mac.config_csr);
    return 0;
}

static void hsan_adjust_link(struct net_device *ndev)
{
    struct hsan_v300 *p = netdev_priv(ndev);
    struct phy_device *phy = ndev->phydev;
    struct hsan_gemac_snapshot mac;
    struct hsan_gemac_link_snapshot link;
    unsigned long flags;

    int mac_ret, link_ret;

    /* Read independently established PHY and MAC state outside the
     * spinlock; the MDIO accessor sleeps. Link loss is transient, but a
     * resolved, LINK-UP speed/duplex/pause change cannot be handled by
     * this one-shot OEM GEMAC programming sequence. Quarantine DMA.
     */
    mac_ret = hsan_gemac_lan1_snapshot(&mac);
    link_ret = hsan_gemac_lan1_link_snapshot(&link);
    /* A lost MAC gate or one-shot register image is a DMA ownership
     * hazard even if the PHY has concurrently dropped link or MDIO read
     * fails. Do not reduce it to carrier_off and leave RX active.
     */
    if (READ_ONCE(p->active) &&
        !hsan_gemac_runtime_qualified(p, &mac, mac_ret)) {
        netif_carrier_off(ndev);
        return;
    }
    if (READ_ONCE(p->active) && phy && phy->link && !mac_ret &&
        !link_ret && mac.mac_link_programmed && link.link_up &&
        link.resolved &&
        (mac.link_register_drift_latched ||
         link.speed_mbps != mac.programmed_speed_mbps ||
         link.full_duplex != mac.programmed_full_duplex ||
         link.rx_pause != mac.programmed_rx_pause ||
         link.tx_pause != mac.programmed_tx_pause)) {
        spin_lock_irqsave(&p->tx_lock, flags);
        if (READ_ONCE(p->active) &&
            !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            p->link_mode_drift_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_LINK_MODE_DRIFT,
                "PHY3 renegotiated beyond immutable GEMAC register mode");
        }
        spin_unlock_irqrestore(&p->tx_lock, flags);
        netif_carrier_off(ndev);
        return;
    }
    if (!READ_ONCE(p->active) || !phy || !phy->link ||
        mac_ret || !mac.mac_link_programmed || mac.link_hw_uncertain ||
        phy->speed != mac.programmed_speed_mbps ||
        (phy->duplex == DUPLEX_FULL) != mac.programmed_full_duplex ||
        link_ret || !link.link_up || !link.resolved ||
        link.speed_mbps != mac.programmed_speed_mbps ||
        link.full_duplex != mac.programmed_full_duplex ||
        link.rx_pause != mac.programmed_rx_pause ||
        link.tx_pause != mac.programmed_tx_pause) {
        netif_carrier_off(ndev);
        return;
    }
    /* GEMAC/PHY snapshots may sleep. Recheck the final carrier-on
     * decision only after taking the TX submission/STOP lock; otherwise
     * ndo_stop could drop active while this callback was reading MDIO,
     * and the stale callback would publish carrier after stop.
     */
    spin_lock_irqsave(&p->tx_lock, flags);
    if (READ_ONCE(p->active) && p->phy_connected && phy->link &&
        !hsan_lsw_dp_fam_cpu_fenced() &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
        netif_carrier_on(ndev);
    else
        netif_carrier_off(ndev);
    spin_unlock_irqrestore(&p->tx_lock, flags);
}

static int hsan_open_datapath(struct net_device *ndev)
{
    struct hsan_v300 *p = netdev_priv(ndev);
    struct phy_device *phy;
    bool arm_mmio_fault = false;
    unsigned int ch, i;
    int ret;

    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_LIFETIME);
    if (p->dma_ever_started ||
        atomic_read(&hsan_pie_dma_lifetime_touched) ||
        test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags))
        return -EOPNOTSUPP;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_DMA_OPTIN);
    if (!of_property_read_bool(p->dev->of_node,
                              "hsan,experimental-dma-enable"))
        return -EACCES;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_TOPOLOGY);
    /* Four ordinary RX rings now have real DMA ownership, independent
     * NAPI contexts and the four OEM IRQ groups. The saved board's four
     * additional GRO rings have 15360-byte buffers and vendor receive
     * metadata. They cannot be enabled by pretending they are RX0.
     * No old single-RX0 opt-in bypasses this missing implementation.
     */
    if (p->gro_channels &&
        !of_property_read_bool(p->dev->of_node,
                              "hsan,experimental-cpu-lan1-diagnostic")) {
        dev_err(p->dev,
            "RX0..3 ordinary queues implemented; %u GRO queues configured but OEM GRO metadata/delivery remains unimplemented\n",
            p->gro_channels);
        return -EOPNOTSUPP;
    }
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_PIPELINE);
    if (!hsan_network_pipeline_ready(p))
        return -ENOLINK;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_CLOCK);
    if (!hsan_clock_gates_ready(p))
        return -EHOSTDOWN;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_GEMAC);
    ret = hsan_gemac_preflight(p);
    if (ret)
        return ret;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_ACP);
    if (!hsan_acp_pie_attrs_ready(p, false))
        return -EHOSTDOWN;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_PHY);
    if (!hsan_phy3_link(p))
        return -ENOLINK;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_RX_IDLE);
    if (!hsan_owned_rx_idle(p) || !hsan_unused_rx_prearm_clean(p)) {
        dev_err(p->dev,
            "PIE RX ownership/geometry is not idle; foreign RX%u flags=%#x owned RX%u fields=%#x\n",
            p->other_rx_bad_channel, p->other_rx_bad_flags,
            p->rx_init_bad_channel, p->rx_init_bad_fields);
        return -EBUSY;
    }
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_ALLOC);
    ret = hsan_alloc_queues(p);
    if (ret)
        return ret;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_PHY_ATTACH);
    ret = hsan_gemac_lan1_get_phy(&phy);
    if (ret)
        goto err_queues;
    ret = phy_connect_direct(ndev, phy, hsan_adjust_link,
                             PHY_INTERFACE_MODE_GMII);
    put_device(&phy->mdio.dev);
    if (ret)
        goto err_queues;
    p->phy_connected = true;
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_IRQ);
    /* Reserve every configured IRQ before GEMAC or ring publication.
     * A partially acquired set is unwound; a failed request is never
     * silently converted to watchdog polling.
     */
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];
        u32 mask_reg = hsan_rx_irq_reg(ch, true);
        unsigned long flags;

        hsan_w32(p, mask_reg, BIT(0));
        if (!(hsan_r32(p, mask_reg) & BIT(0))) {
            ret = -EIO;
            goto err_irq;
        }
        if (q->irq <= 0)
            continue;
        ret = ch < HSAN_NET_RX_CHANNELS ?
            request_irq(q->irq, hsan_rx_irq, 0, "hsan-pie-v300", q) : 0;
        if (ret) {
            dev_err(p->dev, "PIE IRQ[%u] request failed: %d\n", ch, ret);
            goto err_irq;
        }
        spin_lock_irqsave(&p->tx_lock, flags);
        q->irq_ok = true;
        spin_unlock_irqrestore(&p->tx_lock, flags);
    }
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_LINK_PROGRAM);
    ret = hsan_gemac_lan1_program_link();
    if (ret) {
        dev_err(p->dev, "OEM GEMAC link-mode hardware program blocked: %d\n", ret);
        goto err_irq;
    }
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_REVALIDATE);
    if (!hsan_network_pipeline_ready(p)) {
        ret = -ENOLINK;
        goto err_prearm;
    }
    ret = hsan_gemac_preflight(p);
    if (ret)
        goto err_prearm;
    if (!hsan_phy3_link(p)) {
        ret = -ENOLINK;
        goto err_prearm;
    }
    /* All replacement buffers and software descriptors are prepared
     * before taking the FAM guard. No ring BASE points at them yet.
     */
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        /* Prepare the empty tail slot too. The first producer advance
         * exposes it while the just-consumed slot becomes the new gap.
         * OEM rx_init allocates/adds every node, not only count - 1.
         */
        for (i = 0; i < q->rx_count; i++)
            hsan_rx_build_desc(q, i);
        q->rx_cons = 0;
        q->rx_prod = q->rx_count - 1;
    }
    p->tx_prod = p->tx_cons = 0;
    /* Linux 7.3.5 napi_enable takes netdev_lock(), so it must not run
     * inside tx_lock or the FAM spinlock. IRQs remain masked and active
     * remains false until every queue is programmed below.
     */
    for (ch = 0; ch < p->rx_channels; ch++) {
        napi_enable(&p->rxq[ch].napi);
        p->rxq[ch].napi_enabled = true;
    }
    WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_PUBLISH);
    {
        struct hsan_lsw_fam_publish_guard pub;
        unsigned long flags;

        spin_lock_irqsave(&p->tx_lock, flags);
        ret = hsan_lsw_fam_publish_begin(&pub);
        if (ret || test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            p->prearm_handoff_rejections++;
            if (!ret) {
                hsan_lsw_fam_publish_end(&pub);
                ret = -ESHUTDOWN;
            }
            spin_unlock_irqrestore(&p->tx_lock, flags);
            goto err_irq;
        }
        /* Repeat both owned and unowned channel checks at the actual
         * publication boundary. The shared global RX bit is still off.
         */
        if (!hsan_owned_rx_idle(p) || !hsan_unused_rx_prearm_clean(p)) {
            p->prearm_handoff_rejections++;
            hsan_lsw_fam_publish_end(&pub);
            spin_unlock_irqrestore(&p->tx_lock, flags);
            ret = -EBUSY;
            goto err_irq;
        }
        if (atomic_cmpxchg(&hsan_pie_dma_lifetime_touched, 0, 1)) {
            hsan_lsw_fam_publish_end(&pub);
            spin_unlock_irqrestore(&p->tx_lock, flags);
            ret = -EUCLEAN;
            goto err_irq;
        }
        __module_get(THIS_MODULE);
        p->dma_ever_started = true;
        dma_wmb();
        hsan_w32(p, HSAN_TX_CH + HSAN_CH_BASE, (u32)p->tx_desc_dma);
        hsan_w32(p, HSAN_TX_CH + HSAN_CH_RING, HSAN_NET_TX_ORDER);
        hsan_w32(p, HSAN_TX_CH + HSAN_CH_WPTR, 0);
        if (hsan_r32(p, HSAN_TX_CH + HSAN_CH_BASE) != (u32)p->tx_desc_dma ||
            hsan_r32(p, HSAN_TX_CH + HSAN_CH_RING) != HSAN_NET_TX_ORDER ||
            !hsan_check_wptr_readback(p, NULL, 0))
            arm_mmio_fault = true;
        for (ch = 0; ch < p->rx_channels && !arm_mmio_fault; ch++)
            if (!hsan_program_rx_channel(&p->rxq[ch]))
                arm_mmio_fault = true;
        if (!arm_mmio_fault &&
            of_property_read_bool(p->dev->of_node,
                                  "hsan,experimental-cpu-lan1-diagnostic")) {
            if (hsan_lsw_ips_enable_guarded(&pub))
                arm_mmio_fault = true;
            else
                netdev_info(ndev, "OEM IPS enabled after all PIE rings published\n");
        }
        if (!arm_mmio_fault) {
            WRITE_ONCE(p->active, true);
            p->last_route_audit = jiffies;
            p->last_other_rx_audit = jiffies;
            hsan_w32(p, HSAN_RX_GLOBAL,
                     hsan_r32(p, HSAN_RX_GLOBAL) | HSAN_RX_GLOBAL_ENABLE);
            if (!(hsan_r32(p, HSAN_RX_GLOBAL) & HSAN_RX_GLOBAL_ENABLE))
                arm_mmio_fault = true;
            for (ch = 0; ch < p->rx_channels && !arm_mmio_fault; ch++) {
                struct hsan_v300_rx_queue *q = &p->rxq[ch];
                u32 mask_reg = hsan_rx_irq_reg(ch, true);

                if (q->irq_ok)
                    hsan_w32(p, mask_reg, 0);
                if (!!(hsan_r32(p, mask_reg) & BIT(0)) == q->irq_ok)
                    arm_mmio_fault = true;
            }
        }
        hsan_lsw_fam_publish_end(&pub);
        if (arm_mmio_fault) {
            p->initial_arm_mmio_faults++;
            hsan_quarantine_datapath_locked(p,
                HSAN_DMA_FAULT_INITIAL_ARM_READBACK,
                "PIE first irreversible ring/IRQ MMIO readback failed");
        }
        spin_unlock_irqrestore(&p->tx_lock, flags);
    }
    if (arm_mmio_fault) {
        ret = -EIO;
        goto err_published;
    }
    /* IRQ/NAPI may quarantine immediately after unmask. Avoid starting
     * the PHY if the already-published datapath has been stopped.
     */
    if (!READ_ONCE(p->active)) {
        ret = -ESHUTDOWN;
        goto err_published;
    }
    netif_carrier_off(ndev);
    phy_start(ndev->phydev);
    {
        unsigned long flags;

        /* PHY start can sleep. Serialize the final queue/open status
         * commit, not the PHY start, with IRQ/NAPI quarantine. A fault
         * after this commit is ordinary runtime quarantine.
         */
        spin_lock_irqsave(&p->tx_lock, flags);
        if (!READ_ONCE(p->active) ||
            test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
            spin_unlock_irqrestore(&p->tx_lock, flags);
            ret = -ESHUTDOWN;
            goto err_published;
        }
        netif_start_queue(ndev);
        WRITE_ONCE(p->open_stage, HSAN_NET_OPEN_ACTIVE);
        spin_unlock_irqrestore(&p->tx_lock, flags);
    }
    mod_delayed_work(system_wq, &p->watchdog, msecs_to_jiffies(20));
    dev_info(p->dev,
        "PIE v300 DMA submitted: TX %u, RX queues=%u ordinary=%u GRO=%u; aggregate delivery and hardware traffic not qualified\n",
        p->tx_count, p->rx_channels, HSAN_NET_RX_CHANNELS, p->gro_channels);
    return 0;
err_published:
    /* All four allocations are retained after ANY ring publication.
     * IRQ/NAPI cleanup returns CPU ownership only. PHYLIB also stops
     * a started PHY when disconnecting; no ndo_stop or DMA free here.
     */
    hsan_free_owned_irqs(p);
    hsan_disable_owned_napi(p);
    phy_disconnect(ndev->phydev);
    p->phy_connected = false;
    return ret;

err_prearm:
    p->prearm_handoff_rejections++;
err_irq:
    hsan_free_owned_irqs(p);
    hsan_disable_owned_napi(p);
    phy_disconnect(ndev->phydev);
    p->phy_connected = false;
err_queues:
    hsan_free_unstarted(p);
    return ret;
}

static int hsan_open(struct net_device *ndev)
{
    struct hsan_v300 *p = netdev_priv(ndev);
    int ret;

    WRITE_ONCE(p->open_attempts, READ_ONCE(p->open_attempts) + 1);
    WRITE_ONCE(p->open_last_errno, 0);
    ret = hsan_open_datapath(ndev);
    WRITE_ONCE(p->open_last_errno, ret < 0 ? -ret : 0);
    if (ret)
        netdev_err(ndev, "Stage79 open failed: stage=%u errno=%d\n",
                   p->open_stage, ret);
    return ret;
}

/* Stopping IRQ/NAPI and clearing RX_GLOBAL are not an audited DMA stop.
 * Sample both ring pointers twice AFTER IRQ/NAPI drains to preserve a
 * read-only diagnostic of hardware progress. Do not infer quiescence from
 * two matching snapshots (a stuck DMA may also have stationary pointers).
 * All values are retained in net_device, never converted into a permit to
 * unmap DMA buffers or rebind the controller.
 */
static void hsan_record_stop_pointers(struct hsan_v300 *p)
{
    unsigned long flags;
    u16 before_tx, before_rx;
    u16 after_tx, after_rx;
    struct hsan_dp_queue_snapshot fam_before, fam_after;
    bool fam_before_ok, fam_after_ok;
    unsigned int ch, i;

    spin_lock_irqsave(&p->tx_lock, flags);
    /* A duplicate stop observation must not accumulate prior snapshots
     * as though more DMA mappings had been allocated.
     */
    p->stop_tx_dma_retained = 0;
    p->stop_rx_dma_retained = 0;
    p->stop_rx_hw_owned_seen = 0;
    p->stop_rx_desc_addr_mismatch = 0;
    p->stop_tx_desc_addr_mismatch = 0;
    p->stop_fam_sampled = false;
    p->stop_fam_counter_moved = false;
    p->stop_sw_tx_pending = p->tx_prod - p->tx_cons;
    p->stop_sw_rx_posted = 0;
    /* HW may still have prefetched a mapping even if the consumer
     * pointer has moved. The total retained memory includes RX buffers
     * that were CPU-owned when STOP started; no mapping is released.
     */
    for (i = 0; i < p->tx_count; i++)
        p->stop_tx_dma_retained += !!p->tx[i].buffer;
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        p->stop_sw_rx_posted += (u16)(q->rx_prod - q->rx_cons);
        for (i = 0; i < q->rx_count; i++)
            p->stop_rx_dma_retained += !!q->rx[i].buffer;
    }
    spin_unlock_irqrestore(&p->tx_lock, flags);
    /* The software has stopped publishing descriptors; hardware may
     * still DMA into these coherent rings. These are NON-ATOMIC observed
     * fields, NOT a CPU/HW ownership transfer or safe-unmap test.
     */
    dma_rmb();
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        for (i = 0; i < q->rx_count; i++) {
            u32 addr;

            if (!q->rx[i].buffer)
                continue;
            if (READ_ONCE(hsan_rx_desc(q, i)[3]) & BIT(2))
                p->stop_rx_hw_owned_seen++;
            addr = get_unaligned_le32(hsan_rx_desc(q, i) + 4);
            if (addr != (u32)q->rx[i].dma && addr != 0)
                p->stop_rx_desc_addr_mismatch++;
        }
    }
    for (i = 0; i < p->tx_count; i++) {
        if (!p->tx[i].buffer)
            continue;
        if (get_unaligned_le32(hsan_tx_desc(p, i) + 4) !=
            (u32)p->tx[i].dma)
            p->stop_tx_desc_addr_mismatch++;
    }
    /* The OEM FAM dump uses bank43 counters 0x60d4/0x61ac. Sample
     * before/after the PIE pointers without claiming a synchronized HW
     * timestamp or a release of any still-mapped DMA allocation.
     */
    fam_before_ok = !hsan_lsw_dp_get_queue(HSAN_LAN1_PORT, &fam_before) &&
                    fam_before.fam_activity_sampled;
    before_tx = (u16)hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR);
    before_rx = (u16)hsan_r32(p, HSAN_RX_CH + HSAN_CH_RPTR);
    p->stop_tx_hw_wptr = (u16)hsan_r32(p, HSAN_TX_CH + HSAN_CH_WPTR);
    p->stop_rx_hw_wptr = (u16)hsan_r32(p, HSAN_RX_CH + HSAN_CH_WPTR);
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        q->stop_hw_rptr_before =
            (u16)hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR));
        q->stop_hw_wptr_before =
            hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR));
    }
    usleep_range(1000, 1500);
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        q->stop_hw_rptr_after =
            (u16)hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR));
        q->stop_hw_wptr_after =
            hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR));
    }
    after_tx = (u16)hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR);
    after_rx = (u16)hsan_r32(p, HSAN_RX_CH + HSAN_CH_RPTR);
    p->stop_tx_hw_wptr_after = (u16)hsan_r32(p, HSAN_TX_CH + HSAN_CH_WPTR);
    p->stop_rx_hw_wptr_after = (u16)hsan_r32(p, HSAN_RX_CH + HSAN_CH_WPTR);
    p->stop_hw_producer_progress =
        p->stop_tx_hw_wptr != p->stop_tx_hw_wptr_after ||
        p->stop_rx_hw_wptr != p->stop_rx_hw_wptr_after;
    fam_after_ok = !hsan_lsw_dp_get_queue(HSAN_LAN1_PORT, &fam_after) &&
                   fam_after.fam_activity_sampled;
    p->stop_fam_sampled = fam_before_ok && fam_after_ok;
    if (p->stop_fam_sampled) {
        p->stop_fam_counter_a_before = fam_before.fam_activity_21bit;
        p->stop_fam_counter_b_before = fam_before.fam_activity_14bit;
        p->stop_fam_counter_a_after = fam_after.fam_activity_21bit;
        p->stop_fam_counter_b_after = fam_after.fam_activity_14bit;
        p->stop_fam_init_status_before = fam_before.fam_init_status_raw;
        p->stop_fam_init_status_after = fam_after.fam_init_status_raw;
        p->stop_fam_counter_moved =
            (p->stop_fam_counter_a_before != p->stop_fam_counter_a_after ||
             p->stop_fam_counter_b_before != p->stop_fam_counter_b_after);
    }
    p->stop_tx_hw_rptr = after_tx;
    p->stop_rx_hw_rptr = after_rx;
    p->stop_hw_pointer_progress = (before_tx != after_tx ||
                                   before_rx != after_rx);
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        p->stop_hw_pointer_progress |=
            q->stop_hw_rptr_before != q->stop_hw_rptr_after;
        p->stop_hw_producer_progress |=
            q->stop_hw_wptr_before != q->stop_hw_wptr_after;
    }
    p->stop_snapshot_taken = true;
    dev_warn(p->dev,
       "PIE stop read-only snapshot: SW TX pending=%u RX posted=%u HW TX r/w=%04x/%04x RX r/w=%04x/%04x consumer moved=%d producer moved=%d; retained TX maps=%u RX maps=%u; NOT a DMA stop ACK\n",
       p->stop_sw_tx_pending, p->stop_sw_rx_posted,
       after_tx, p->stop_tx_hw_wptr_after, after_rx, p->stop_rx_hw_wptr_after,
       p->stop_hw_pointer_progress, p->stop_hw_producer_progress,
       p->stop_tx_dma_retained,
       p->stop_rx_dma_retained);
    if (p->stop_fam_sampled)
        dev_warn(p->dev,
                 "FAM read-only counters before/after: 21bit=%u/%u 14bit=%u/%u moved=%d; NOT a HW stop ACK\n",
                 p->stop_fam_counter_a_before, p->stop_fam_counter_a_after,
                 p->stop_fam_counter_b_before, p->stop_fam_counter_b_after,
                 p->stop_fam_counter_moved);
    dev_warn(p->dev,
             "PIE stop observed DMA descriptors: RX hw-owner-bit=%u TX/RX addr mismatch=%u/%u; FAM init-status before/after=%#x/%#x; these are NOT a STOP ACK or DMA release grant\n",
             p->stop_rx_hw_owned_seen, p->stop_tx_desc_addr_mismatch,
             p->stop_rx_desc_addr_mismatch,
             p->stop_fam_init_status_before, p->stop_fam_init_status_after);
}

static int hsan_stop(struct net_device *ndev)
{
    struct hsan_v300 *p = netdev_priv(ndev);
    unsigned long flags;
    unsigned int ch;

    netif_tx_disable(ndev);
    spin_lock_irqsave(&p->tx_lock, flags);
    WRITE_ONCE(p->active, false);
    if (p->dma_ever_started) {
        hsan_lsw_dp_fam_stop_fence();
        p->stop_fam_cpu_fenced = true;
    }
    hsan_mask_owned_rx_irqs(p);
    hsan_w32(p, HSAN_RX_GLOBAL,
             hsan_r32(p, HSAN_RX_GLOBAL) & ~HSAN_RX_GLOBAL_ENABLE);
    (void)hsan_r32(p, HSAN_RX_GLOBAL);
    spin_unlock_irqrestore(&p->tx_lock, flags);
    cancel_delayed_work_sync(&p->watchdog);
    if (p->phy_connected) {
        phy_stop(ndev->phydev);
        phy_disconnect(ndev->phydev);
        p->phy_connected = false;
    }
    /* Synchronize every installed physical IRQ, then all four NAPIs.
     * These CPU drains do not establish a hardware DMA STOP ACK.
     */
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        if (q->irq_ok) {
            synchronize_irq(q->irq);
            p->stop_irq_synchronized = true;
        }
    }
    hsan_disable_owned_napi(p);
    spin_lock_irqsave(&p->tx_lock, flags);
    p->stop_rx_irq_masked_channels = 0;
    p->stop_rx_irq_pending_channels = 0;
    for (ch = 0; ch < p->rx_channels; ch++) {
        u32 mask_reg = hsan_rx_irq_reg(ch, true);
        u32 status_reg = hsan_rx_irq_reg(ch, false);
        u32 status;

        hsan_w32(p, mask_reg, BIT(0));
        (void)hsan_r32(p, mask_reg);
        hsan_w32(p, status_reg, BIT(0));
        status = hsan_r32(p, status_reg);
        if (!ch)
            p->stop_irq_status_after_ack = status; /* legacy RX0 stat */
        if (status & BIT(0))
            p->stop_rx_irq_pending_channels |= BIT(ch);
        if (hsan_r32(p, mask_reg) & BIT(0))
            p->stop_rx_irq_masked_channels |= BIT(ch);
    }
    p->stop_irq_masked_readback = p->stop_rx_irq_masked_channels ==
                                 GENMASK(p->rx_channels - 1, 0);
    p->stop_rx_global_disabled_readback =
        !(hsan_r32(p, HSAN_RX_GLOBAL) & HSAN_RX_GLOBAL_ENABLE);
    spin_unlock_irqrestore(&p->tx_lock, flags);
    hsan_free_owned_irqs(p);
    netif_carrier_off(ndev);
    if (p->dma_ever_started)
        hsan_record_stop_pointers(p);
    WRITE_ONCE(p->hardware_quiesced, false);
    dev_warn(p->dev,
        "PIE STOP CPU: FAM fence=%d IRQ handlers synchronized=%d RX masks=%#x RX-global-off=%d RX pending=%#x; hardware DMA drain unverified, all mappings retained\n",
        p->stop_fam_cpu_fenced, p->stop_irq_synchronized,
        p->stop_rx_irq_masked_channels, p->stop_rx_global_disabled_readback,
        p->stop_rx_irq_pending_channels);
    return 0;
}

static void hsan_get_stats64(struct net_device *ndev,
				     struct rtnl_link_stats64 *s)
{
	struct hsan_v300 *p = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&p->tx_lock, flags);
	s->tx_packets = p->tx_packets;
	s->tx_bytes = p->tx_bytes;
	s->tx_dropped = p->tx_dropped;
	s->tx_errors = p->tx_errors;
    s->rx_packets = p->rx_packets;
    s->rx_bytes = p->rx_bytes;
    s->rx_dropped = p->rx_dropped;
    s->rx_errors = p->rx_errors;
    spin_unlock_irqrestore(&p->tx_lock, flags);
}

static int hsan_change_mtu(struct net_device *ndev, int mtu)
{
	/* The GEMAC frame ceiling includes Ethernet header and FCS.
	 * Keep the Linux payload MTU fixed until separately qualified.
	 */
	if (mtu != ETH_DATA_LEN)
		return -EOPNOTSUPP;
	ndev->mtu = mtu;
	return 0;
}

static void hsan_tx_timeout(struct net_device *ndev, unsigned int queue)
{
	struct hsan_v300 *p = netdev_priv(ndev);

	unsigned long flags;

    /* A delayed netdev timeout can race ndo_stop. Without this lock,
     * timeout could issue new PIE IRQ/GLOBAL writes after stop has
     * deactivated the controller or its clock owner. Count/poison only
     * while the same TX/STOP lock still sees an active datapath.
     */
    spin_lock_irqsave(&p->tx_lock, flags);
    if (READ_ONCE(p->active) &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        p->tx_errors++;
        hsan_quarantine_datapath_locked(p, HSAN_DMA_FAULT_TX_TIMEOUT,
                                      "netdev TX watchdog timeout");
    }
    spin_unlock_irqrestore(&p->tx_lock, flags);
}

/* Stage31: diagnostic counters for actual LAN1 DMA bring-up. Hardware
 * pointers are sampled only while the controller is live and not poisoned;
 * observing the interface while down must not read an unclocked PIE bank.
 * These are raw register values, NOT evidence of successful delivery.
 */
static const char hsan_ethtool_stat_names[][ETH_GSTRING_LEN] = {
    "tx_submitted", "tx_reclaimed", "rx_reclaimed", "rx_posted",
    "dma_active", "dma_started", "dma_quarantined", "tx_stall_ticks",
    "tx_hw_wptr", "tx_hw_rptr", "rx_hw_wptr", "rx_hw_rptr",
    "tx_packets", "tx_errors", "rx_packets", "rx_errors",
    "dp_port10_queue_known", "dp_reserved_memory", "dp_fam_staging",
    "dp_eqm_bootstrap", "dp_eqm_queues_verified", "dp_pie_map_committed",
    "dp_pie_map_uncertain", "pfe_clock", "pfe_globals",
    "pfe_pub_shadow_tables", "pfe_opp_shadow_tables",
    "gemac_clock_on", "gemac_link_configured",
    "pfe_mcc_shadow_tables", "pfe_mcc_hw_rows", "pfe_mcc_hw_uncertain",
    "pfe_mcc_write_stream_sent",
    "pfe_ofc_shadow_tables", "pfe_ofc_dynamic_entries",
    "pfe_ofc_csr_verified", "pfe_ofc_hw_uncertain",
    "dp_fam_staging_addresses",
    "pfe_ifc_shadow_table", "pfe_ifc_hw_rows", "pfe_ifc_hw_uncertain",
    "pfe_service_shadow_tables", "pfe_pa_shadow_tables",
    "pfe_ifc_hash_range_checked", "pfe_ifc_hash_start",
    "pfe_pa_csr_verified", "pfe_pa_hw_uncertain",
    "dp_fam_dma_range_32bit", "pfe_ifc_hash_reserved",
    "pfe_ifc_engine_quarantined",
    "pie_dma_lifetime_pinned", "eqm_engine_quarantined",
    "dma_hw_quiesced_verified", "bind_unbind_sysfs_suppressed",
    "dp_fam_cpu_table_verified", "dp_fam_dma_attrs_zero",
    "dp_fam_hw_owned", "dp_fam_row_limit_131072",
    "gemac_link_hw_attempted", "gemac_link_hw_uncertain",
    "gemac_link_module_pinned",
    "dp_fam_oem_geometry_verified", "dp_fam_oem_derived_rows",
    "rx_posted_descriptors", "rx_unposted_pointer_faults",
    "rx_oversized_descriptor_faults",
    "dp_fam_arm_attempted", "dp_fam_oem_ack_seen",
    "pfe_napt_shadow_tables", "pfe_napt_ipv4_rows",
    "pfe_napt_ipv6_rows", "pfe_napt_prelude_attempted",
    "pfe_napt_prelude_submitted", "pfe_napt_hw_uncertain",
    "pfe_napt_geometry_verified", "pfe_napt_oem_primary_level",
    "pfe_napt_oem_secondary_level", "pfe_napt_observed_memory_bytes",
    "pfe_napt_ipv4_reserved", "pfe_napt_ipv6_reserved",
    "pfe_napt_ipv4_materialized", "pfe_napt_ipv6_materialized",
    "pfe_napt_partition_attempted", "pfe_napt_partition_commands",
    "pfe_napt_partition_last_status", "pfe_napt_cmd_quarantined",
    "dma_first_fault_code", "dma_fault_events",
    "tx_pending_sw", "rx_posted_sw",
    "fam_csr_transaction_pinned", "napt_partition_layout_verified",
    "pie_stop_snapshot_taken", "pie_stop_tx_pending", "pie_stop_rx_posted",
    "pie_stop_tx_hw_rptr", "pie_stop_rx_hw_rptr",
    "pie_stop_tx_hw_wptr", "pie_stop_rx_hw_wptr",
    "pie_stop_hw_pointer_moved",
    "dp_fam_activity_sampled", "dp_fam_counter_21bit",
    "dp_fam_counter_14bit", "pfe_ifc_window_readback_rows",
    "pie_stop_fam_sampled", "pie_stop_fam_counter_moved",
    "pie_stop_fam_21_before", "pie_stop_fam_21_after",
    "pie_stop_fam_14_before", "pie_stop_fam_14_after",
    "rx_rearm_suppressed", "rx_dma_address_faults",
    "eps_l3_counter_row_known", "eps_l3_counter_sampled",
    "eps_l3_counter_row", "eps_l3_first_counter_lo",
    "eps_l3_second_counter_lo", "eps_l3_second_counter_hi8",
    "mcc_dual_bank_tx_sealed",
    "rx_irq_rearms", "rx_irq_rearm_blocked",
    "rx_irq_lost_edge_repolls",
    "fam_dma_cpu_stop_fenced", "pie_stop_irq_cpu_synced",
    "pie_stop_irq_mask_readback", "pie_stop_rx_disabled_readback",
    "pie_stop_irq_status_after_ack", "gemac_link_mode_drift_faults",
    "pfe_forwarding_blockers", "pfe_ifc_oem_image_crc32",
    "pfe_ifc_oem_image_verified",
    "gemac_live_register_readback_ok",
    "gemac_reg_drift_latched",
    "fam_csr_cpu_fenced", "fam_csr_write_rejected",
    "tx_dma_addr_faults", "stop_tx_mappings_retained",
    "stop_rx_mappings_retained",
    "ifc_late_window_all_read", "ifc_late_rows_matched",
    "ifc_late_window_errno", "ifc_late_drift_latched",
    "pfe_oem_diag_requested", "pfe_oem_diag_valid_groups",
    "pfe_diag_ifc_1000", "pfe_diag_ifc_1018",
    "pfe_diag_l2_40000", "pfe_diag_l2_40004",
    "pfe_diag_l3_1000", "pfe_diag_l3_1018",
    "pfe_diag_pa_1000", "pfe_diag_pa_1004",
    "pfe_diag_napt_ifc_1024", "pfe_diag_napt_l3_1014",
    "pfe_diag_napt_1000",
    "runtime_route_audits", "runtime_route_failures",
    "fam_external_fence_faults", "fam_fence_tx_blocks",
    "gemac_oem_stats_requested", "gemac_oem_stats_valid",
    "gemac_oem_counter_0400", "gemac_oem_counter_04d0",
    "gemac_oem_counter_04c0", "gemac_oem_counter_0588",
    "pie_prearm_handoff_rejects", "pie_irq_postack_reasserts",
    "stop_tx_wptr_after", "stop_rx_wptr_after",
    "stop_hw_producer_moved", "pfe_fam_cpu_fenced",
    "pfe_fam_fence_abort_latched", "pfe_fam_fence_abort_step",
    "stop_fam_init_status_before", "stop_fam_init_status_after",
    "stop_rx_hw_owned_seen", "stop_tx_desc_addr_mismatch",
    "stop_rx_desc_addr_mismatch", "dp_fam_init_ack_status",
    "fam_cpu_publish_rejections",
    "pie_tx_wptr_readback_faults", "pie_rx_wptr_readback_faults",
    "pie_irq_unmask_readback_faults", "pie_initial_arm_mmio_faults",
    "pie_tx_wptr_readback_ok", "pie_rx_wptr_readback_ok",
    "pie_last_tx_wptr_readback", "pie_last_rx_wptr_readback",
    "fam_cpu_publish_admissions", "fam_cpu_fence_invocations",
    "pfe_diag_sample_begin_ns", "pfe_diag_sample_end_ns",
    "gemac_diag_sample_begin_ns", "gemac_diag_sample_end_ns",
    "fam_diag_sample_begin_ns", "fam_diag_sample_end_ns",
    "pie_ethtool_sample_begin_ns", "pie_ethtool_sample_end_ns",
    "fam_tx_reclaim_admitted", "fam_tx_reclaim_rejected",
    "pie_tx_nomem_drops", "pie_irq_mask_readback_faults",
    "pie_tx_state_drops", "pie_tx_late_ring_faults",
    "pie_gemac_runtime_faults",
    "pie_other_rx_prearm_checks", "pie_other_rx_prearm_rejects",
    "pie_other_rx_bad_channel", "pie_other_rx_bad_flags",
    "pie_other_rx_runtime_checks", "pie_other_rx_runtime_faults",
    "pie_other_rx_runtime_channel", "pie_other_rx_runtime_flags",
    "pie_irq07_foreign_faults", "pie_rx_delivery_suppressed",
    "pie_rx_sf_cursor_publications", "pie_rx_sf_readback_faults",
    "pie_rx_hw_slot_out_of_range", "pie_rx_slot_wraps",
    "pie_rx_last_sf_cursor", "pie_rx_last_hw_cursor",
    "pie_tx_hw_slot_out_of_range", "pie_tx_slot_wraps",
    "pie_tx_last_hw_cursor", "pie_tx_last_published_cursor",
    "pie_rx_batch_kick_commits", "pie_rx_batch_kick_descs",
    "pie_rx_batch_kick_faults", "pie_rx_batch_kick_max",
    "pie_rx_pub_foreign_checks", "pie_rx_pub_foreign_faults",
    "pie_rx_cpu_sync_admissions", "pie_rx_cpu_sync_blocks",
    "pie_rx_cpu_copied_bytes",
    "pie_rx_completion_snapshots",
    "pie_rx_completion_rechecks",
    "pie_rx_completion_recheck_faults",
    "pie_tx_gso_unsupported_drops",
    "pie_tx_nonlinear_flattened",
    "net_open_attempts",
    "net_open_stage",
    "net_open_errno",
    "rx_ordinary_queues",
    "rx_gro_configured",
    "rx_gro_supported",
    "rx_irq_resources",
    "rx_init_bad_channel",
    "rx_init_bad_fields",
    "stop_rx_irq_mask_bitmap",
    "stop_rx_irq_pending_bitmap",
    "dp_init_owner_known",
    "dp_init_stage",
    "dp_init_errno",
    "dp_init_mmio_attempted",
    "dp_init_quarantined",
    "pfe_lifecycle_owner_known",
    "pfe_hw_attempted",
    "pfe_init_quarantined",
    "pfe_bindings_retained",
    "pfe_init_failed_step",
    "pfe_init_errno",
    "pfe_l3_commit_owner_known",
    "pfe_l3_commit_requested",
    "pfe_l3_commit_attempted",
    "pfe_l3_commit_submitted",
    "pfe_l3_cpu_readback_ok",
    "pfe_l3_commit_errno",
    "pfe_l3_commit_row",
    "pfe_pub_gro_count_known",
    "pfe_pub_gro_entries",
    "pfe_pub_expected_tables",
    "net_pipeline_operational",
    "rx_cpu_queue_bitmap",
    "rx_gro_blocked",
    "rx0_sw_cons",
    "rx0_sw_prod",
    "rx0_sw_posted",
    "rx0_descriptors",
    "rx0_packets",
    "rx0_bytes",
    "rx0_dropped",
    "rx0_errors",
    "rx0_irq_number",
    "rx0_irq_bound",
    "rx0_irq_events",
    "rx0_polls",
    "rx0_hw_rptr",
    "rx0_hw_wptr",
    "rx0_stop_hw_rptr",
    "rx0_stop_hw_wptr",
    "rx1_sw_cons",
    "rx1_sw_prod",
    "rx1_sw_posted",
    "rx1_descriptors",
    "rx1_packets",
    "rx1_bytes",
    "rx1_dropped",
    "rx1_errors",
    "rx1_irq_number",
    "rx1_irq_bound",
    "rx1_irq_events",
    "rx1_polls",
    "rx1_hw_rptr",
    "rx1_hw_wptr",
    "rx1_stop_hw_rptr",
    "rx1_stop_hw_wptr",
    "rx2_sw_cons",
    "rx2_sw_prod",
    "rx2_sw_posted",
    "rx2_descriptors",
    "rx2_packets",
    "rx2_bytes",
    "rx2_dropped",
    "rx2_errors",
    "rx2_irq_number",
    "rx2_irq_bound",
    "rx2_irq_events",
    "rx2_polls",
    "rx2_hw_rptr",
    "rx2_hw_wptr",
    "rx2_stop_hw_rptr",
    "rx2_stop_hw_wptr",
    "rx3_sw_cons",
    "rx3_sw_prod",
    "rx3_sw_posted",
    "rx3_descriptors",
    "rx3_packets",
    "rx3_bytes",
    "rx3_dropped",
    "rx3_errors",
    "rx3_irq_number",
    "rx3_irq_bound",
    "rx3_irq_events",
    "rx3_polls",
    "rx3_hw_rptr",
    "rx3_hw_wptr",
    "rx3_stop_hw_rptr",
    "rx3_stop_hw_wptr",
    "rx0_wptr_cmp_state", "rx0_wptr_match_valid",
    "rx0_wptr_match_cons", "rx0_wptr_match_prod",
    "rx1_wptr_cmp_state", "rx1_wptr_match_valid",
    "rx1_wptr_match_cons", "rx1_wptr_match_prod",
    "rx2_wptr_cmp_state", "rx2_wptr_match_valid",
    "rx2_wptr_match_cons", "rx2_wptr_match_prod",
    "rx3_wptr_cmp_state", "rx3_wptr_match_valid",
    "rx3_wptr_match_cons", "rx3_wptr_match_prod",
    "pfe_tocpu_000",
    "pfe_tocpu_001",
    "pfe_tocpu_002",
    "pfe_tocpu_003",
    "pfe_tocpu_004",
    "pfe_tocpu_005",
    "pfe_tocpu_006",
    "pfe_tocpu_007",
    "pfe_tocpu_008",
    "pfe_tocpu_009",
    "pfe_tocpu_010",
    "pfe_tocpu_011",
    "pfe_tocpu_012",
    "pfe_tocpu_013",
    "pfe_tocpu_014",
    "pfe_tocpu_015",
    "pfe_tocpu_016",
    "pfe_tocpu_017",
    "pfe_tocpu_018",
    "pfe_tocpu_019",
    "pfe_tocpu_020",
    "pfe_tocpu_021",
    "pfe_tocpu_022",
    "pfe_tocpu_023",
    "pfe_tocpu_024",
    "pfe_tocpu_025",
    "pfe_tocpu_026",
    "pfe_tocpu_027",
    "pfe_tocpu_028",
    "pfe_tocpu_029",
    "pfe_tocpu_030",
    "pfe_tocpu_031",
    "pfe_tocpu_032",
    "pfe_tocpu_033",
    "pfe_tocpu_034",
    "pfe_tocpu_035",
    "pfe_tocpu_036",
    "pfe_tocpu_037",
    "pfe_tocpu_038",
    "pfe_tocpu_039",
    "pfe_tocpu_040",
    "pfe_tocpu_041",
    "pfe_tocpu_042",
    "pfe_tocpu_043",
    "pfe_tocpu_044",
    "pfe_tocpu_045",
    "pfe_tocpu_046",
    "pfe_tocpu_047",
    "pfe_tocpu_048",
    "pfe_tocpu_049",
    "pfe_tocpu_050",
    "pfe_tocpu_051",
    "pfe_tocpu_052",
    "pfe_tocpu_053",
    "pfe_tocpu_054",
    "pfe_tocpu_055",
    "pfe_tocpu_056",
    "pfe_tocpu_057",
    "pfe_tocpu_058",
    "pfe_tocpu_059",
    "pfe_tocpu_060",
    "pfe_tocpu_061",
    "pfe_tocpu_062",
    "pfe_tocpu_063",
    "pfe_tocpu_064",
    "pfe_tocpu_065",
    "pfe_tocpu_066",
    "pfe_tocpu_067",
    "pfe_tocpu_068",
    "pfe_tocpu_069",
    "pfe_tocpu_070",
    "pfe_tocpu_071",
    "pfe_tocpu_072",
    "pfe_tocpu_073",
    "pfe_tocpu_074",
    "pfe_tocpu_075",
    "pfe_tocpu_076",
    "pfe_tocpu_077",
    "pfe_tocpu_078",
    "pfe_tocpu_079",
    "pfe_tocpu_080",
    "pfe_tocpu_081",
    "pfe_tocpu_082",
    "pfe_tocpu_083",
    "pfe_tocpu_084",
    "pfe_tocpu_085",
    "pfe_tocpu_086",
    "pfe_tocpu_087",
    "pfe_tocpu_088",
    "pfe_tocpu_089",
    "pfe_tocpu_090",
    "pfe_tocpu_091",
    "pfe_tocpu_092",
    "pfe_tocpu_093",
    "pfe_tocpu_094",
    "pfe_tocpu_095",
    "pfe_tocpu_096",
    "pfe_tocpu_097",
    "pfe_tocpu_098",
    "pfe_tocpu_099",
    "pfe_tocpu_100",
    "pfe_tocpu_101",
    "pfe_tocpu_102",
    "pfe_tocpu_103",
    "pfe_tocpu_104",
    "pfe_tocpu_105",
    "pfe_tocpu_106",
    "pfe_tocpu_107",
    "pfe_tocpu_108",
    "pfe_tocpu_109",
    "pfe_tocpu_110",
    "pfe_tocpu_111",
    "pfe_tocpu_112",
    "pfe_tocpu_113",
    "pfe_tocpu_114",
    "pfe_tocpu_115",
    "pfe_tocpu_116",
    "pfe_tocpu_117",
    "pfe_tocpu_118",
    "pfe_tocpu_119",
    "pfe_tocpu_120",
    "pfe_tocpu_121",
    "pfe_tocpu_122",
    "pfe_tocpu_123",
    "pfe_tocpu_124",
    "pfe_tocpu_125",
    "pfe_tocpu_126",
    "pfe_tocpu_127",
    "pfe_tocpu_128",
    "pfe_tocpu_129",
    "pfe_tocpu_130",
    "pfe_tocpu_131",
    "pfe_tocpu_132",
    "pfe_tocpu_133",
    "pfe_tocpu_134",
    "pfe_tocpu_135",
    "pfe_tocpu_136",
    "pfe_tocpu_137",
    "pfe_pa_1008",
    "pfe_pa_100c",
    "pfe_opp_1000",
    "pfe_opp_1004",
    "pfe_ofc_1000",
    "pfe_ofc_1004",
    "pfe_cpu_counters_valid",
    "pfe_drop_000",
    "pfe_drop_001",
    "pfe_drop_002",
    "pfe_drop_003",
    "pfe_drop_004",
    "pfe_drop_005",
    "pfe_drop_006",
    "pfe_drop_007",
    "pfe_drop_008",
    "pfe_drop_009",
    "pfe_drop_010",
    "pfe_drop_011",
    "pfe_drop_012",
    "pfe_drop_013",
    "pfe_drop_014",
    "pfe_drop_015",
    "pfe_drop_016",
    "pfe_drop_017",
    "pfe_drop_018",
    "pfe_drop_019",
    "pfe_drop_020",
    "pfe_drop_021",
    "pfe_drop_022",
    "pfe_drop_023",
    "pfe_drop_024",
    "pfe_drop_025",
    "pfe_drop_026",
    "pfe_drop_027",
    "pfe_drop_028",
    "pfe_drop_029",
    "pfe_drop_030",
    "pfe_drop_031",
    "pfe_drop_032",
    "pfe_drop_033",
    "pfe_drop_034",
    "pfe_drop_035",
    "pfe_drop_036",
    "pfe_drop_037",
    "pfe_drop_038",
    "pfe_drop_039",
    "pfe_drop_040",
    "pfe_drop_041",
    "pfe_drop_042",
    "pfe_drop_043",
    "pfe_drop_044",
    "pfe_drop_045",
    "pfe_drop_046",
    "pfe_drop_047",
    "pfe_drop_048",
    "pfe_drop_049",
    "pfe_drop_050",
    "pfe_drop_051",
    "pfe_drop_052",
    "pfe_drop_053",
    "pfe_drop_054",
    "pfe_drop_055",
    "pfe_drop_056",
    "pfe_drop_057",
    "pfe_drop_058",
    "pfe_drop_059",
    "pfe_drop_060",
    "pfe_drop_061",
    "pfe_drop_062",
    "pfe_drop_063",
    "pfe_drop_064",
    "pfe_drop_065",
    "pfe_drop_066",
    "pfe_drop_067",
    "pfe_drop_068",
    "pfe_drop_069",
    "pfe_drop_070",
    "pfe_drop_071",
    "pfe_drop_072",
    "pfe_drop_073",
    "pfe_drop_074",
    "pfe_drop_075",
    "pfe_drop_076",
    "pfe_drop_077",
    "pfe_drop_078",
    "pfe_drop_079",
    "pfe_drop_080",
    "pfe_drop_081",
    "pfe_drop_082",
    "pfe_drop_083",
    "pfe_drop_084",
    "pfe_drop_085",
    "pfe_drop_086",
    "pfe_drop_087",
    "pfe_drop_088",
    "pfe_drop_089",
    "pfe_drop_090",
    "pfe_drop_091",
    "pfe_drop_092",
    "pfe_drop_093",
    "pfe_drop_094",
    "pfe_drop_095",
    "pfe_drop_096",
    "pfe_drop_097",
    "pfe_drop_098",
    "pfe_drop_099",
    "pfe_drop_100",
    "pfe_drop_101",
    "pfe_drop_102",
    "pfe_drop_103",
    "pfe_drop_104",
    "pfe_drop_105",
    "pfe_drop_106",
    "pfe_drop_107",
    "pfe_drop_108",
    "pfe_drop_109",
    "pfe_drop_110",
    "pfe_drop_111",
    "pfe_drop_112",
    "pfe_drop_113",
    "pfe_drop_114",
    "pfe_drop_115",
    "pfe_drop_116",
    "pfe_drop_117",
    "pfe_drop_118",
    "pfe_drop_119",
    "pfe_drop_120",
    "pfe_drop_121",
    "pfe_drop_122",
    "pfe_drop_123",
    "pfe_drop_124",
    "pfe_drop_125",
    "pfe_drop_126",
    "pfe_drop_127",
    "pfe_drop_128",
    "pfe_drop_129",
    "pfe_drop_130",
    "pfe_drop_131",
    "pfe_drop_132",
    "pfe_drop_133",
    "pfe_drop_134",
    "pfe_drop_135",
    "pie_rx0_rptr_raw",
    "pie_rx0_wptr_raw",
    "pie_rx1_rptr_raw",
    "pie_rx1_wptr_raw",
    "pie_rx2_rptr_raw",
    "pie_rx2_wptr_raw",
    "pie_rx3_rptr_raw",
    "pie_rx3_wptr_raw",
    "pie_rx4_rptr_raw",
    "pie_rx4_wptr_raw",
    "pie_rx5_rptr_raw",
    "pie_rx5_wptr_raw",
    "pie_rx6_rptr_raw",
    "pie_rx6_wptr_raw",
    "pie_rx7_rptr_raw",
    "pie_rx7_wptr_raw",
    "pie_rx_global_raw",
    "pie_rx_large_raw",
    "pie_rx_route_raw",
    "eqm_q81_t81_w0",
    "eqm_q81_t81_w1",
    "eqm_q81_t81_w2",
    "eqm_q81_t81_w3",
    "eqm_q81_t80_w2",
    "eqm_q81_valid",
    "eqm_q82_t81_w0",
    "eqm_q82_t81_w1",
    "eqm_q82_t81_w2",
    "eqm_q82_t81_w3",
    "eqm_q82_t80_w2",
    "eqm_q82_valid",
    "eqm_q83_t81_w0",
    "eqm_q83_t81_w1",
    "eqm_q83_t81_w2",
    "eqm_q83_t81_w3",
    "eqm_q83_t80_w2",
    "eqm_q83_valid",
    "eqm_q84_t81_w0",
    "eqm_q84_t81_w1",
    "eqm_q84_t81_w2",
    "eqm_q84_t81_w3",
    "eqm_q84_t80_w2",
    "eqm_q84_valid",
    "eqm_q85_t81_w0",
    "eqm_q85_t81_w1",
    "eqm_q85_t81_w2",
    "eqm_q85_t81_w3",
    "eqm_q85_t80_w2",
    "eqm_q85_valid",
    "eqm_q86_t81_w0",
    "eqm_q86_t81_w1",
    "eqm_q86_t81_w2",
    "eqm_q86_t81_w3",
    "eqm_q86_t80_w2",
    "eqm_q86_valid",
    "eqm_q87_t81_w0",
    "eqm_q87_t81_w1",
    "eqm_q87_t81_w2",
    "eqm_q87_t81_w3",
    "eqm_q87_t80_w2",
    "eqm_q87_valid",
    "eqm_q88_t81_w0",
    "eqm_q88_t81_w1",
    "eqm_q88_t81_w2",
    "eqm_q88_t81_w3",
    "eqm_q88_t80_w2",
    "eqm_q88_valid",
    "eqm_q129_t81_w0",
    "eqm_q129_t81_w1",
    "eqm_q129_t81_w2",
    "eqm_q129_t81_w3",
    "eqm_q129_t80_w2",
    "eqm_q129_valid",
    "eqm_q130_t81_w0",
    "eqm_q130_t81_w1",
    "eqm_q130_t81_w2",
    "eqm_q130_t81_w3",
    "eqm_q130_t80_w2",
    "eqm_q130_valid",
    "eqm_q131_t81_w0",
    "eqm_q131_t81_w1",
    "eqm_q131_t81_w2",
    "eqm_q131_t81_w3",
    "eqm_q131_t80_w2",
    "eqm_q131_valid",
    "eqm_q132_t81_w0",
    "eqm_q132_t81_w1",
    "eqm_q132_t81_w2",
    "eqm_q132_t81_w3",
    "eqm_q132_t80_w2",
    "eqm_q132_valid",
    "eqm_q133_t81_w0",
    "eqm_q133_t81_w1",
    "eqm_q133_t81_w2",
    "eqm_q133_t81_w3",
    "eqm_q133_t80_w2",
    "eqm_q133_valid",
    "eqm_q134_t81_w0",
    "eqm_q134_t81_w1",
    "eqm_q134_t81_w2",
    "eqm_q134_t81_w3",
    "eqm_q134_t80_w2",
    "eqm_q134_valid",
    "eqm_q135_t81_w0",
    "eqm_q135_t81_w1",
    "eqm_q135_t81_w2",
    "eqm_q135_t81_w3",
    "eqm_q135_t80_w2",
    "eqm_q135_valid",
    "eqm_q136_t81_w0",
    "eqm_q136_t81_w1",
    "eqm_q136_t81_w2",
    "eqm_q136_t81_w3",
    "eqm_q136_t80_w2",
    "eqm_q136_valid",
    "eqm_q137_t81_w0",
    "eqm_q137_t81_w1",
    "eqm_q137_t81_w2",
    "eqm_q137_t81_w3",
    "eqm_q137_t80_w2",
    "eqm_q137_valid",
    "eqm_q138_t81_w0",
    "eqm_q138_t81_w1",
    "eqm_q138_t81_w2",
    "eqm_q138_t81_w3",
    "eqm_q138_t80_w2",
    "eqm_q138_valid",
    "eqm_q139_t81_w0",
    "eqm_q139_t81_w1",
    "eqm_q139_t81_w2",
    "eqm_q139_t81_w3",
    "eqm_q139_t80_w2",
    "eqm_q139_valid",
    "eqm_q140_t81_w0",
    "eqm_q140_t81_w1",
    "eqm_q140_t81_w2",
    "eqm_q140_t81_w3",
    "eqm_q140_t80_w2",
    "eqm_q140_valid",
    "eqm_q141_t81_w0",
    "eqm_q141_t81_w1",
    "eqm_q141_t81_w2",
    "eqm_q141_t81_w3",
    "eqm_q141_t80_w2",
    "eqm_q141_valid",
    "eqm_q142_t81_w0",
    "eqm_q142_t81_w1",
    "eqm_q142_t81_w2",
    "eqm_q142_t81_w3",
    "eqm_q142_t80_w2",
    "eqm_q142_valid",
    "eqm_q143_t81_w0",
    "eqm_q143_t81_w1",
    "eqm_q143_t81_w2",
    "eqm_q143_t81_w3",
    "eqm_q143_t80_w2",
    "eqm_q143_valid",
    "eqm_q144_t81_w0",
    "eqm_q144_t81_w1",
    "eqm_q144_t81_w2",
    "eqm_q144_t81_w3",
    "eqm_q144_t80_w2",
    "eqm_q144_valid",
    "eqm_q145_t81_w0",
    "eqm_q145_t81_w1",
    "eqm_q145_t81_w2",
    "eqm_q145_t81_w3",
    "eqm_q145_t80_w2",
    "eqm_q145_valid",
    "eqm_q146_t81_w0",
    "eqm_q146_t81_w1",
    "eqm_q146_t81_w2",
    "eqm_q146_t81_w3",
    "eqm_q146_t80_w2",
    "eqm_q146_valid",
    "eqm_q147_t81_w0",
    "eqm_q147_t81_w1",
    "eqm_q147_t81_w2",
    "eqm_q147_t81_w3",
    "eqm_q147_t80_w2",
    "eqm_q147_valid",
    "eqm_q148_t81_w0",
    "eqm_q148_t81_w1",
    "eqm_q148_t81_w2",
    "eqm_q148_t81_w3",
    "eqm_q148_t80_w2",
    "eqm_q148_valid",
    "eqm_q149_t81_w0",
    "eqm_q149_t81_w1",
    "eqm_q149_t81_w2",
    "eqm_q149_t81_w3",
    "eqm_q149_t80_w2",
    "eqm_q149_valid",
    "eqm_q150_t81_w0",
    "eqm_q150_t81_w1",
    "eqm_q150_t81_w2",
    "eqm_q150_t81_w3",
    "eqm_q150_t80_w2",
    "eqm_q150_valid",
    "eqm_q151_t81_w0",
    "eqm_q151_t81_w1",
    "eqm_q151_t81_w2",
    "eqm_q151_t81_w3",
    "eqm_q151_t80_w2",
    "eqm_q151_valid",
    "eqm_q152_t81_w0",
    "eqm_q152_t81_w1",
    "eqm_q152_t81_w2",
    "eqm_q152_t81_w3",
    "eqm_q152_t80_w2",
    "eqm_q152_valid",
    "eqm_q153_t81_w0",
    "eqm_q153_t81_w1",
    "eqm_q153_t81_w2",
    "eqm_q153_t81_w3",
    "eqm_q153_t80_w2",
    "eqm_q153_valid",
    "eqm_q154_t81_w0",
    "eqm_q154_t81_w1",
    "eqm_q154_t81_w2",
    "eqm_q154_t81_w3",
    "eqm_q154_t80_w2",
    "eqm_q154_valid",
    "eqm_q155_t81_w0",
    "eqm_q155_t81_w1",
    "eqm_q155_t81_w2",
    "eqm_q155_t81_w3",
    "eqm_q155_t80_w2",
    "eqm_q155_valid",
    "eqm_q156_t81_w0",
    "eqm_q156_t81_w1",
    "eqm_q156_t81_w2",
    "eqm_q156_t81_w3",
    "eqm_q156_t80_w2",
    "eqm_q156_valid",
    "eqm_q157_t81_w0",
    "eqm_q157_t81_w1",
    "eqm_q157_t81_w2",
    "eqm_q157_t81_w3",
    "eqm_q157_t80_w2",
    "eqm_q157_valid",
    "eqm_q158_t81_w0",
    "eqm_q158_t81_w1",
    "eqm_q158_t81_w2",
    "eqm_q158_t81_w3",
    "eqm_q158_t80_w2",
    "eqm_q158_valid",
    "eqm_q159_t81_w0",
    "eqm_q159_t81_w1",
    "eqm_q159_t81_w2",
    "eqm_q159_t81_w3",
    "eqm_q159_t80_w2",
    "eqm_q159_valid",
    "eqm_q160_t81_w0",
    "eqm_q160_t81_w1",
    "eqm_q160_t81_w2",
    "eqm_q160_t81_w3",
    "eqm_q160_t80_w2",
    "eqm_q160_valid",
    "ips_row0_raw0",
    "ips_row0_sop_drop",
    "ips_row0_eop_drop",
    "ips_row0_exlong_drop",
    "ips_row0_exshort_drop",
    "ips_row0_mac_err_drop",
    "ips_row0_burr_drop",
    "ips_row0_valid",
    "ips_row1_raw0",
    "ips_row1_sop_drop",
    "ips_row1_eop_drop",
    "ips_row1_exlong_drop",
    "ips_row1_exshort_drop",
    "ips_row1_mac_err_drop",
    "ips_row1_burr_drop",
    "ips_row1_valid",
    "ips_row2_raw0",
    "ips_row2_sop_drop",
    "ips_row2_eop_drop",
    "ips_row2_exlong_drop",
    "ips_row2_exshort_drop",
    "ips_row2_mac_err_drop",
    "ips_row2_burr_drop",
    "ips_row2_valid",
    "ips_row3_raw0",
    "ips_row3_sop_drop",
    "ips_row3_eop_drop",
    "ips_row3_exlong_drop",
    "ips_row3_exshort_drop",
    "ips_row3_mac_err_drop",
    "ips_row3_burr_drop",
    "ips_row3_valid",
    "ips_row4_raw0",
    "ips_row4_sop_drop",
    "ips_row4_eop_drop",
    "ips_row4_exlong_drop",
    "ips_row4_exshort_drop",
    "ips_row4_mac_err_drop",
    "ips_row4_burr_drop",
    "ips_row4_valid",
    "ips_row5_raw0",
    "ips_row5_sop_drop",
    "ips_row5_eop_drop",
    "ips_row5_exlong_drop",
    "ips_row5_exshort_drop",
    "ips_row5_mac_err_drop",
    "ips_row5_burr_drop",
    "ips_row5_valid",
    "ips_row6_raw0",
    "ips_row6_sop_drop",
    "ips_row6_eop_drop",
    "ips_row6_exlong_drop",
    "ips_row6_exshort_drop",
    "ips_row6_mac_err_drop",
    "ips_row6_burr_drop",
    "ips_row6_valid",
    "ips_row7_raw0",
    "ips_row7_sop_drop",
    "ips_row7_eop_drop",
    "ips_row7_exlong_drop",
    "ips_row7_exshort_drop",
    "ips_row7_mac_err_drop",
    "ips_row7_burr_drop",
    "ips_row7_valid",
    "ips_row8_raw0",
    "ips_row8_sop_drop",
    "ips_row8_eop_drop",
    "ips_row8_exlong_drop",
    "ips_row8_exshort_drop",
    "ips_row8_mac_err_drop",
    "ips_row8_burr_drop",
    "ips_row8_valid",
    "ips_row9_raw0",
    "ips_row9_sop_drop",
    "ips_row9_eop_drop",
    "ips_row9_exlong_drop",
    "ips_row9_exshort_drop",
    "ips_row9_mac_err_drop",
    "ips_row9_burr_drop",
    "ips_row9_valid",
    "ips_row10_raw0",
    "ips_row10_sop_drop",
    "ips_row10_eop_drop",
    "ips_row10_exlong_drop",
    "ips_row10_exshort_drop",
    "ips_row10_mac_err_drop",
    "ips_row10_burr_drop",
    "ips_row10_valid",
};
#define HSAN_RX_WPTR_STATS_BASE (274 + 16 * HSAN_NET_RX_CHANNELS)
static_assert(ARRAY_SIZE(hsan_ethtool_stat_names) == HSAN_RX_WPTR_STATS_BASE +
              4 * HSAN_NET_RX_CHANNELS + 300 +
              6 * HSAN_DP_DIAG_QUEUE_COUNT + 88);

static void hsan_get_drvinfo(struct net_device *ndev,
                             struct ethtool_drvinfo *info)
{
    strscpy(info->driver, "hsan-net-v300", sizeof(info->driver));
    strscpy(info->version, "stage79", sizeof(info->version));
    strscpy(info->bus_info, dev_name(ndev->dev.parent),
            sizeof(info->bus_info));
}

static int hsan_get_sset_count(struct net_device *ndev, int sset)
{
    if (sset == ETH_SS_STATS)
        return ARRAY_SIZE(hsan_ethtool_stat_names);
    return -EOPNOTSUPP;
}

static void hsan_get_strings(struct net_device *ndev, u32 stringset,
                             u8 *data)
{
    if (stringset == ETH_SS_STATS)
        memcpy(data, hsan_ethtool_stat_names,
               sizeof(hsan_ethtool_stat_names));
}

static void hsan_get_ethtool_stats(struct net_device *ndev,
                                  struct ethtool_stats *stats, u64 *data)
{
    struct hsan_v300 *p = netdev_priv(ndev);
    unsigned long flags;
    unsigned int ch;

    memset(data, 0, sizeof(u64) * ARRAY_SIZE(hsan_ethtool_stat_names));
    data[196] = ktime_get_ns();
    spin_lock_irqsave(&p->tx_lock, flags);
    data[0] = p->tx_prod;
    data[1] = p->tx_cons;
    data[4] = READ_ONCE(p->active);
    data[5] = READ_ONCE(p->dma_ever_started);
    data[6] = test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags);
    data[7] = READ_ONCE(p->stalled_ticks);
    data[12] = p->tx_packets;
    data[13] = p->tx_errors;
    data[86] = p->dma_fault_first_code;
    data[87] = p->dma_fault_events;
    data[88] = (u16)(p->tx_prod - p->tx_cons);
    data[119] = p->rx_irq_rearms;
    data[120] = p->rx_irq_rearm_blocked;
    data[121] = p->rx_irq_lost_edge_repolls;
    data[127] = p->link_mode_drift_faults;
    data[135] = p->tx_descriptor_addr_faults;
    data[136] = p->stop_tx_dma_retained;
    data[137] = p->stop_rx_dma_retained;
    data[155] = p->runtime_route_audits;
    data[156] = p->runtime_route_failures;
    data[157] = p->fam_external_fence_faults;
    data[158] = p->fam_fence_tx_blocks;
    data[165] = p->prearm_handoff_rejections;
    data[166] = p->rx_irq_postack_reasserts;
    data[180] = p->tx_wptr_readback_faults;
    data[200] = p->tx_nomem_drops;
    data[201] = p->irq_mask_readback_faults;
    data[202] = p->tx_state_drops;
    data[203] = p->tx_late_ring_faults;
    data[204] = p->gemac_runtime_faults;
    data[205] = p->other_rx_prearm_checks;
    data[206] = p->other_rx_prearm_rejects;
    data[207] = p->other_rx_bad_channel;
    data[208] = p->other_rx_bad_flags;
    data[209] = p->other_rx_runtime_checks;
    data[210] = p->other_rx_runtime_faults;
    data[211] = p->other_rx_runtime_bad_channel;
    data[212] = p->other_rx_runtime_bad_flags;
    data[213] = p->other_rx_irq7_faults;
    data[214] = p->rx_delivery_suppressed;
    data[215] = p->rx_sf_cursor_publications;
    data[216] = p->rx_sf_cursor_readback_faults;
    data[217] = p->rx_hw_rptr_out_of_range;
    data[218] = p->rx_slot_cursor_wraps;
    data[219] = p->last_rx_sf_cursor_readback;
    data[220] = p->last_rx_hw_rptr;
    data[221] = p->tx_hw_rptr_out_of_range;
    data[222] = p->tx_slot_cursor_wraps;
    data[223] = p->last_tx_hw_rptr;
    data[224] = p->last_tx_published_wptr;
    data[225] = p->rx_batch_kick_commits;
    data[226] = p->rx_batch_kick_descs;
    data[227] = p->rx_batch_kick_faults;
    data[228] = p->rx_batch_kick_max;
    data[229] = p->other_rx_publish_checks;
    data[230] = p->other_rx_publish_faults;
    data[231] = p->rx_cpu_sync_admissions;
    data[232] = p->rx_cpu_sync_blocks;
    data[233] = p->rx_cpu_copied_bytes;
    data[234] = p->rx_completion_snapshots;
    data[235] = p->rx_completion_rechecks;
    data[236] = p->rx_completion_recheck_faults;
    data[237] = p->tx_gso_unsupported_drops;
    data[238] = p->tx_nonlinear_flattened;
    /* Existing RX cursor/pointer stats retain RX0 meaning. New per-queue
     * fields below identify all four NAPI/ring states explicitly. */
    data[2] = p->rxq[0].rx_cons;
    data[3] = p->rxq[0].rx_prod;
    data[89] = (u16)(p->rxq[0].rx_prod - p->rxq[0].rx_cons);
    data[63] = data[89];
    data[14] = p->rx_packets;
    data[15] = p->rx_errors;
    data[239] = READ_ONCE(p->open_attempts);
    data[240] = READ_ONCE(p->open_stage);
    data[241] = READ_ONCE(p->open_last_errno);
    data[242] = HSAN_NET_RX_CHANNELS;
    data[243] = p->gro_channels;
    data[244] = 0; /* GRO payload/metadata reconstruction unfinished */
    data[245] = p->irq_resources;
    data[246] = p->rx_init_bad_channel;
    data[247] = p->rx_init_bad_fields;
    data[248] = p->stop_rx_irq_masked_channels;
    data[249] = p->stop_rx_irq_pending_channels;
    data[272] = GENMASK(p->rx_channels - 1, 0);
    data[273] = !!p->gro_channels;
    for (ch = 0; ch < HSAN_NET_RX_CHANNELS; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];
        unsigned int base = 274 + 16 * ch;

        data[base] = q->rx_cons;
        data[base + 1] = q->rx_prod;
        data[base + 2] = (u16)(q->rx_prod - q->rx_cons);
        data[base + 3] = q->rx_count;
        data[base + 4] = q->packets;
        data[base + 5] = q->bytes;
        data[base + 6] = q->dropped;
        data[base + 7] = q->errors;
        data[base + 8] = q->irq > 0 ? q->irq : 0;
        data[base + 9] = q->irq_ok;
        data[base + 10] = q->irq_events;
        data[base + 11] = q->polls;
        data[base + 14] = q->stop_hw_rptr_after;
        data[base + 15] = q->stop_hw_wptr_after;
        /* Appended fields retain prior indices and share this SW snapshot. */
        base = HSAN_RX_WPTR_STATS_BASE + 4 * ch;
        data[base] = q->wptr_cmp_state;
        data[base + 1] = q->wptr_match_valid;
        data[base + 2] = q->last_match_cons;
        data[base + 3] = q->last_match_prod;
    }
    data[181] = p->rx_wptr_readback_faults;
    data[182] = p->irq_unmask_readback_faults;
    data[183] = p->initial_arm_mmio_faults;
    data[184] = p->tx_wptr_readback_ok;
    data[185] = p->rx_wptr_readback_ok;
    data[186] = p->last_tx_wptr_readback;
    data[187] = p->last_rx_wptr_readback;
    spin_unlock_irqrestore(&p->tx_lock, flags);
    data[167] = READ_ONCE(p->stop_tx_hw_wptr_after);
    data[168] = READ_ONCE(p->stop_rx_hw_wptr_after);
    data[169] = READ_ONCE(p->stop_hw_producer_progress);
    data[173] = READ_ONCE(p->stop_fam_init_status_before);
    data[174] = READ_ONCE(p->stop_fam_init_status_after);
    data[175] = READ_ONCE(p->stop_rx_hw_owned_seen);
    data[176] = READ_ONCE(p->stop_tx_desc_addr_mismatch);
    data[177] = READ_ONCE(p->stop_rx_desc_addr_mismatch);
    data[85] = hsan_lsw_napt_health() == -EUCLEAN;
    data[90] = hsan_lsw_fam_health() == -EUCLEAN;
    data[122] = READ_ONCE(p->stop_fam_cpu_fenced);
    data[123] = READ_ONCE(p->stop_irq_synchronized);
    data[124] = READ_ONCE(p->stop_irq_masked_readback);
    data[125] = READ_ONCE(p->stop_rx_global_disabled_readback);
    data[126] = READ_ONCE(p->stop_irq_status_after_ack);
    data[92] = READ_ONCE(p->stop_snapshot_taken);
    data[93] = READ_ONCE(p->stop_sw_tx_pending);
    data[94] = READ_ONCE(p->stop_sw_rx_posted);
    data[95] = READ_ONCE(p->stop_tx_hw_rptr);
    data[96] = READ_ONCE(p->stop_rx_hw_rptr);
    data[97] = READ_ONCE(p->stop_tx_hw_wptr);
    data[98] = READ_ONCE(p->stop_rx_hw_wptr);
    data[99] = READ_ONCE(p->stop_hw_pointer_progress);
    data[104] = READ_ONCE(p->stop_fam_sampled);
    data[105] = READ_ONCE(p->stop_fam_counter_moved);
    data[106] = READ_ONCE(p->stop_fam_counter_a_before);
    data[107] = READ_ONCE(p->stop_fam_counter_a_after);
    data[108] = READ_ONCE(p->stop_fam_counter_b_before);
    data[109] = READ_ONCE(p->stop_fam_counter_b_after);
    data[61] = 0; /* overwritten by owner snapshot when published */
    data[62] = 0;
    data[64] = READ_ONCE(p->rx_unposted_pointer_faults);
    data[65] = READ_ONCE(p->rx_oversized_desc_faults);
    data[110] = READ_ONCE(p->rx_rearm_suppressed);
    data[111] = READ_ONCE(p->rx_descriptor_addr_faults);
    data[66] = 0;
    data[67] = 0;
    data[50] = atomic_read(&hsan_pie_dma_lifetime_touched);
    data[51] = hsan_lsw_eqm_health() == -EUCLEAN;
    data[52] = READ_ONCE(p->hardware_quiesced);
    data[53] = 1; /* driver-core bind/unbind sysfs entries disabled */
    {
        struct hsan_dp_init_snapshot dp_init;
        struct hsan_pfe_lifecycle_snapshot pfe_init;
        struct hsan_pfe_l3intf_commit_snapshot l3;

        if (!hsan_lsw_dp_init_snapshot(&dp_init)) {
            data[250] = 1;
            data[251] = dp_init.stage;
            data[252] = dp_init.last_errno < 0 ?
                        -(s64)dp_init.last_errno : dp_init.last_errno;
            data[253] = dp_init.mmio_attempted;
            data[254] = dp_init.quarantined;
        }
        if (!hsan_lsw_pfe_lifecycle_snapshot(&pfe_init)) {
            data[255] = 1;
            data[256] = pfe_init.hardware_attempted;
            data[257] = pfe_init.quarantined;
            data[258] = pfe_init.bindings_retained;
            data[259] = pfe_init.failed_step;
            data[260] = pfe_init.last_errno;
        }
        if (!hsan_lsw_pfe_l3intf_commit_snapshot(&l3)) {
            data[261] = 1;
            data[262] = l3.requested;
            data[263] = l3.attempted;
            data[264] = l3.mmio_submitted;
            data[265] = l3.cpu_readback_ok;
            data[266] = l3.last_errno;
            data[267] = l3.row;
        }
        data[271] = hsan_lsw_pipeline_operational();
    }
    /* Copy-only cross-module software snapshots are safe while lan1 is
     * DOWN. They show WHY DMA remains blocked, without accessing a
     * potentially unclocked PIE register or enabling the datapath.
     */
    {
        struct hsan_dp_queue_snapshot queue;
        struct hsan_pfe_snapshot pfe;
        struct hsan_gemac_snapshot mac;

        if (!hsan_lsw_dp_get_queue(HSAN_LAN1_PORT, &queue)) {
            data[16] = 1;
            data[17] = queue.reserved_memory_verified;
            data[18] = queue.fam_addresses_prepared;
            data[19] = queue.eqm_bootstrap_committed;
            data[20] = queue.eqm_queue_rows_committed;
            data[21] = queue.pie_map_hw_committed;
            data[22] = queue.pie_map_hw_uncertain;
            data[37] = queue.fam_entry_count;
            data[47] = queue.fam_dma_range_32bit;
            data[54] = queue.fam_cpu_table_verified;
            data[55] = queue.fam_dma_attrs_zero;
            data[56] = queue.fam_hw_owned;
            data[57] = queue.fam_entry_count <= 131072 &&
                       !(queue.fam_entry_count % 32);
            data[61] = queue.fam_oem_runtime_geometry_verified;
            data[62] = queue.fam_oem_derived_entries;
            data[66] = queue.fam_hw_attempted;
            data[67] = queue.fam_hw_ack_seen;
            data[100] = queue.fam_activity_sampled;
            data[101] = queue.fam_activity_21bit;
            data[102] = queue.fam_activity_14bit;
            data[178] = queue.fam_init_status_raw;
            data[179] = queue.fam_rejected_pie_publications;
            data[188] = queue.fam_admitted_cpu_publications;
            data[198] = queue.fam_admitted_tx_reclaims;
            data[199] = queue.fam_rejected_tx_reclaims;
            data[189] = queue.fam_stop_fence_invocations;
            data[194] = queue.fam_sample_begin_ns;
            data[195] = queue.fam_sample_end_ns;
            data[112] = queue.eps_l3_counter_row_known;
            data[113] = queue.eps_l3_counter_sampled;
            data[114] = queue.eps_l3_counter_row;
            data[115] = queue.eps_l3_first_low32;
            data[116] = queue.eps_l3_second_low32;
            data[117] = queue.eps_l3_second_high8;
            data[133] = queue.fam_cpu_stop_fenced;
            data[134] = queue.fam_rejected_csr_writes;
        }
        if (!hsan_lsw_pfe_snapshot(&pfe)) {
            data[268] = pfe.pub_gro_count_known;
            data[269] = pfe.pub_gro_entries;
            data[270] = pfe.pub_shadows_expected;
            data[23] = pfe.clock_on;
            data[24] = pfe.global_hw_init_completed;
            data[25] = pfe.pub_shadows;
            data[26] = pfe.opp_shadows;
            data[29] = pfe.mcc_shadows;
            data[30] = pfe.mcc_rows_written;
            data[31] = pfe.mcc_hw_uncertain;
            data[32] = pfe.mcc_write_stream_submitted;
            data[118] = pfe.mcc_transaction_sealed;
            data[33] = pfe.ofc_shadows;
            data[34] = pfe.ofc_dynamic_entries;
            data[35] = pfe.ofc_csr_verified;
            data[36] = pfe.ofc_hw_uncertain;
            data[38] = pfe.ifc_shadow_registered;
            data[39] = pfe.ifc_rows_committed;
            data[103] = pfe.ifc_rows_window_readback;
            data[40] = pfe.ifc_hw_uncertain;
            data[41] = pfe.service_shadows;
            data[42] = pfe.pa_shadows;
            data[43] = pfe.ifc_hash_range_checked;
            data[44] = pfe.ifc_hash_observed_start;
            data[45] = pfe.pa_csr_verified;
            data[46] = pfe.pa_hw_uncertain;
            data[48] = pfe.ifc_hash_reserved;
            data[49] = pfe.ifc_engine_quarantined;
            data[68] = pfe.napt_shadow_tables;
            data[69] = pfe.napt_ipv4_rows;
            data[70] = pfe.napt_ipv6_rows;
            data[71] = pfe.napt_fixed_attempted;
            data[72] = pfe.napt_fixed_submitted;
            data[73] = pfe.napt_hw_uncertain;
            data[74] = pfe.napt_oem_geometry_verified;
            data[75] = pfe.napt_primary_level;
            data[76] = pfe.napt_secondary_level;
            data[77] = pfe.napt_observed_memory_bytes;
            data[78] = pfe.napt_ipv4_reserved_rows;
            data[79] = pfe.napt_ipv6_reserved_rows;
            data[80] = pfe.napt_ipv4_materialized_rows;
            data[81] = pfe.napt_ipv6_materialized_rows;
            data[82] = pfe.napt_partition_attempted;
            data[83] = pfe.napt_partition_commands;
            data[84] = pfe.napt_partition_last_status;
            data[91] = pfe.napt_partition_layout_verified;
            data[128] = pfe.forwarding_blockers;
            data[129] = pfe.ifc_observed_image_crc32;
            data[130] = pfe.ifc_observed_image_verified;
            data[138] = pfe.ifc_late_window_sampled;
            data[170] = pfe.fam_cpu_stop_fenced;
            data[171] = pfe.fam_fence_abort_latched;
            data[172] = pfe.fam_fence_abort_step;
            data[139] = pfe.ifc_late_window_rows_matched;
            data[140] = pfe.ifc_late_window_read_errno;
            data[141] = pfe.ifc_late_window_drift_latched;
            data[142] = pfe.oem_diag_requested;
            data[143] = pfe.oem_diag_valid_groups;
            data[190] = pfe.oem_diag_sample_begin_ns;
            data[191] = pfe.oem_diag_sample_end_ns;
            {
                unsigned int j;

                for (j = 0; j < HSAN_PFE_OEM_DIAG_WORDS; j++)
                    data[144 + j] = pfe.oem_diag_raw[j];
            }
        }
        {
            struct hsan_gemac_oem_counters raw;
            unsigned int i;

            (void)hsan_gemac_lan1_oem_counters(&raw);
            data[159] = raw.requested;
            data[160] = raw.valid;
            data[192] = raw.sample_begin_ns;
            data[193] = raw.sample_end_ns;
            for (i = 0; i < HSAN_GEMAC_OEM_COUNTER_PAIRS; i++)
                data[161 + i] = raw.raw[i];
        }
        if (!hsan_gemac_lan1_snapshot(&mac)) {
            data[27] = mac.gate_on;
            data[28] = mac.mac_link_programmed;
            data[58] = mac.link_hw_attempted;
            data[59] = mac.link_hw_uncertain;
            data[60] = mac.link_module_pinned;
            data[131] = mac.link_register_readback_valid;
            data[132] = mac.link_register_drift_latched;
        }
    }
    /* Do not check active outside the lock and then read MMIO: ndo_stop
     * may deactivate the controller between those operations. Its state
     * transition and fault quarantine hold this same lock. The four
     * pointer reads remain diagnostic-only and never grant DMA release.
     */
    spin_lock_irqsave(&p->tx_lock, flags);
    if (READ_ONCE(p->active) &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        data[8] = hsan_r32(p, HSAN_TX_CH + HSAN_CH_WPTR);
        data[9] = hsan_r32(p, HSAN_TX_CH + HSAN_CH_RPTR);
        data[10] = hsan_r32(p, HSAN_RX_CH + HSAN_CH_WPTR);
        data[11] = hsan_r32(p, HSAN_RX_CH + HSAN_CH_RPTR);
        {
            struct hsan_lsw_fam_publish_guard obs;
            const unsigned int base = HSAN_RX_WPTR_STATS_BASE +
                                      4 * HSAN_NET_RX_CHANNELS + 281;

            if (!hsan_lsw_fam_observe_begin(&obs)) {
                for (ch = 0; ch < p->rx_channels; ch++) {
                    data[base + ch * 2] = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR));
                    data[base + ch * 2 + 1] = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR));
                }
                data[base + 16] = hsan_r32(p, HSAN_RX_GLOBAL);
                data[base + 17] = hsan_r32(p, HSAN_RX_LARGE_BUFFER);
                data[base + 18] = hsan_r32(p, 0x3404);
                hsan_lsw_fam_publish_end(&obs);
            }
        }
        for (ch = 0; ch < HSAN_NET_RX_CHANNELS; ch++) {
            unsigned int base = 274 + 16 * ch;

            data[base + 12] = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR));
            data[base + 13] = hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR));
        }
    }
    spin_unlock_irqrestore(&p->tx_lock, flags);
    {
        struct hsan_pfe_cpu_counters counters;
        unsigned int i;
        const unsigned int base = HSAN_RX_WPTR_STATS_BASE +
                                  4 * HSAN_NET_RX_CHANNELS;

        if (!hsan_lsw_pfe_cpu_counters(&counters)) {
            for (i = 0; i < HSAN_PFE_TOCPU_REASON_COUNT; i++)
                data[base + i] = counters.reason[i];
            for (i = 0; i < HSAN_PFE_PATH_COUNTER_COUNT; i++)
                data[base + HSAN_PFE_TOCPU_REASON_COUNT + i] = counters.path[i];
            data[base + 144] = 1;
            for (i = 0; i < HSAN_PFE_DROP_REASON_COUNT; i++)
                data[base + 145 + i] = counters.drop[i];
        }
    }
    if (READ_ONCE(p->active) &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags)) {
        struct hsan_dp_queue_counters counters;
        const unsigned int base = HSAN_RX_WPTR_STATS_BASE +
                                  4 * HSAN_NET_RX_CHANNELS + 300;
        unsigned int i, word;

        /* No PIE spinlock is held across the sleeping EQM transaction.
         * The CSR command path independently enforces the STOP fence.
         */
        hsan_lsw_dp_queue_counters(&counters);
        for (i = 0; i < HSAN_DP_DIAG_QUEUE_COUNT; i++) {
            if (!counters.valid[i])
                continue;
            for (word = 0; word < 5; word++)
                data[base + i * 6 + word] = counters.words[i][word];
            data[base + i * 6 + 5] = 1;
        }
    }
    if (READ_ONCE(p->active) &&
        !test_bit(HSAN_FAULT_DATAPATH, &p->fault_flags) &&
        !hsan_lsw_dp_fam_cpu_fenced() &&
        of_property_read_bool(p->dev->of_node,
                             "hsan,experimental-cpu-lan1-diagnostic")) {
        unsigned int row, field;
        const unsigned int base = ARRAY_SIZE(hsan_ethtool_stat_names) - 88;

        /* OEM ips_diag_get_cnt @0x63b0: eleven rows, seven adjacent
         * registers. Raw observations only, not atomic snapshots or
         * DMA ownership evidence. No command register writes.
         */
        for (row = 0; row < 11; row++) {
            u32 counts[7];
            int err = 0;

            /* OEM row8 selector is 64 and never selects the six error
             * registers through its u32 port mask. Leave it invalid.
             */
            if (row == 8)
                continue;
            for (field = 0; field < ARRAY_SIZE(counts); field++) {
                err = hsan_lsw_read(0x4400101c + row * 28 + field * 4,
                                    &counts[field]);
                if (err)
                    break;
            }
            if (err)
                break;
            for (field = 0; field < ARRAY_SIZE(counts); field++)
                data[base + row * 8 + field] = counts[field];
            data[base + row * 8 + 7] = 1;
        }
    }
    data[197] = ktime_get_ns();
}

static int hsan_get_link_ksettings(struct net_device *ndev,
                                   struct ethtool_link_ksettings *cmd)
{
    if (!ndev->phydev)
        return -ENODEV;
    return phy_ethtool_get_link_ksettings(ndev, cmd);
}

static const struct ethtool_ops hsan_ethtool_ops = {
    .get_drvinfo = hsan_get_drvinfo,
    .get_link = ethtool_op_get_link,
    .get_link_ksettings = hsan_get_link_ksettings,
    .get_sset_count = hsan_get_sset_count,
    .get_strings = hsan_get_strings,
    .get_ethtool_stats = hsan_get_ethtool_stats,
};

/* The OEM DT lacks a MAC on PIE and GEMAC. A random local address is
 * legitimate for discovery but must not become an immutable identity:
 * userspace may set a known board address BEFORE DMA ever starts. This
 * updates only Linux netdev state; programming the physical GEMAC/LSW
 * filter is a separate, still unqualified hardware task.
 */
static int hsan_set_mac_address(struct net_device *ndev, void *addr)
{
    struct hsan_v300 *p = netdev_priv(ndev);

    if (READ_ONCE(p->active) || READ_ONCE(p->dma_ever_started) ||
        atomic_read(&hsan_pie_dma_lifetime_touched))
        return -EBUSY;
    return eth_mac_addr(ndev, addr); /* checks validity and IFF_UP */
}

static const struct net_device_ops hsan_ops = {
	.ndo_open = hsan_open,
	.ndo_stop = hsan_stop,
	.ndo_start_xmit = hsan_start_xmit,
    .ndo_set_mac_address = hsan_set_mac_address,
	.ndo_tx_timeout = hsan_tx_timeout,
	.ndo_get_stats64 = hsan_get_stats64,
	.ndo_change_mtu = hsan_change_mtu,
};

/* Consolidate topology, lifecycle and forwarding blockers at probe.
 * The first ndo_open error cannot describe all independent dependencies.
 * Read-only observations never grant DMA publication permission.
 */
static void hsan_report_startup_blockers(struct hsan_v300 *p)
{
    struct device_node *dp, *pfe;
    struct hsan_dp_queue_snapshot q;
    struct hsan_dp_init_snapshot dp_init = { 0 };
    struct hsan_pfe_snapshot forwarding;
    struct hsan_pfe_lifecycle_snapshot pfe_init = { 0 };
    bool dp_on, pfe_on, dma_optin;
    int dp_ret = -ENODEV, pfe_ret = -ENODEV;
    int dp_init_ret, pfe_init_ret;
    u32 blockers = 0;
    const char *fam_cpu_publication_fence = "unobserved";

    dp = of_find_compatible_node(NULL, NULL, "hsan,dp");
    pfe = of_find_compatible_node(NULL, NULL, "hsan,pfe");
    dp_on = dp && of_device_is_available(dp);
    pfe_on = pfe && of_device_is_available(pfe);
    of_node_put(dp);
    of_node_put(pfe);
    dma_optin = of_property_read_bool(p->dev->of_node,
                                     "hsan,experimental-dma-enable");
    dp_init_ret = hsan_lsw_dp_init_snapshot(&dp_init);
    pfe_init_ret = hsan_lsw_pfe_lifecycle_snapshot(&pfe_init);
    if (!dma_optin)
        blockers |= BIT(0);
    if (p->gro_channels)
        blockers |= BIT(1);
    if (!dp_on)
        blockers |= BIT(2);
    if (!pfe_on)
        blockers |= BIT(3);
    if (dp_init.quarantined || pfe_init.quarantined)
        blockers |= BIT(6);
    if (!hsan_lsw_pipeline_operational())
        blockers |= BIT(7);
    /* A failed probe can retain its binding to keep DMA allocations
     * alive. Its lifecycle snapshot is CPU-only; do not request optional
     * MMIO observations from a quarantined or absent hardware owner.
     */
    if (dp_on && !dp_init_ret && !dp_init.quarantined) {
        dp_ret = hsan_lsw_dp_get_queue(HSAN_LAN1_PORT, &q);
        if (!dp_ret)
            fam_cpu_publication_fence = q.fam_cpu_stop_fenced ? "set" : "clear";
        if (dp_ret || !q.fam_hw_owned || !q.fam_hw_ack_seen ||
            !q.pie_map_hw_committed || !q.eqm_queue_rows_committed ||
            !dp_init.pie_map_geometry_valid ||
            dp_init.pie_gro_start != HSAN_NET_RX_CHANNELS ||
            dp_init.pie_gro_count != p->gro_channels)
            blockers |= BIT(4);
    } else {
        blockers |= BIT(4);
    }
    if (pfe_on && !pfe_init_ret && !pfe_init.quarantined) {
        pfe_ret = hsan_lsw_pfe_snapshot(&forwarding);
        if (pfe_ret || !forwarding.global_hw_init_completed ||
            forwarding.forwarding_blockers ||
            !forwarding.pub_gro_count_known ||
            forwarding.pub_gro_entries != p->gro_channels)
            blockers |= BIT(5);
    } else {
        blockers |= BIT(5);
    }
    dev_warn(p->dev,
        "STAGE79_LAN1_STARTUP_BLOCKERS mask=%#x (bit0=dma_optin bit1=GRO_unimplemented bit2=DP_DT bit3=PFE_DT bit4=FAM_EQM_PIE_hw bit5=PFE_forwarding bit6=init_quarantined bit7=pipeline_unqualified) DP=%d PFE=%d; no DMA started\n",
        blockers, dp_ret, pfe_ret);
    dev_info(p->dev,
        "init observations: DP owner=%d stage=%u errno=%d attempted=%d quarantined=%d; PFE owner=%d step=%u errno=%u attempted=%d quarantined=%d;"
        " fam_cpu_publication_fence=%s (snapshot only; not DMA stop/drain or route readiness)\n",
        !dp_init_ret, dp_init.stage, dp_init.last_errno,
        dp_init.mmio_attempted, dp_init.quarantined,
        !pfe_init_ret, pfe_init.failed_step, pfe_init.last_errno,
        pfe_init.hardware_attempted, pfe_init.quarantined,
        fam_cpu_publication_fence);
}

static int hsan_collect_rx_irqs(struct platform_device *pdev, bool use_irqs,
                                 int irqs[HSAN_NET_RX_CHANNELS])
{
    unsigned int ch, prior;

    for (ch = 0; ch < HSAN_NET_RX_CHANNELS; ch++) {
        int irq = use_irqs ? platform_get_irq(pdev, ch) : -ENXIO;

        if (use_irqs && irq < 0)
            return irq; /* Preserve IRQ-domain EPROBE_DEFER. */
        irqs[ch] = irq;
    }
    if (!use_irqs)
        return 0;

    /* ndo_open requests all four as exclusive Linux IRQs. Compare only
     * after the complete set resolves so a real lookup error still wins.
     */
    for (ch = 1; ch < HSAN_NET_RX_CHANNELS; ch++)
        for (prior = 0; prior < ch; prior++)
            if (irqs[ch] == irqs[prior])
                return dev_err_probe(&pdev->dev, -EINVAL,
                    "PIE RX groups %u and %u share exclusive IRQ %d\n",
                    prior, ch, irqs[ch]);
    return 0;
}

static int hsan_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct net_device *ndev;
    struct hsan_v300 *p;
    struct resource *res;
    u32 tx[2], rx[8], gro[8], hwver;
    unsigned int ch, added_napi = 0;
    unsigned int gro_channels = 0;
    int ret, cells, irq_count;
    int irqs[HSAN_NET_RX_CHANNELS];
    bool polling;
    bool rx0_large;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res || res->start != HSAN_PIE_ADDR ||
        resource_size(res) != HSAN_PIE_LEN)
        return -EINVAL;
    if (of_property_read_u32(dev->of_node, "hwver", &hwver) || hwver != 0x300)
        return -EINVAL;
    if (atomic_read(&hsan_pie_dma_lifetime_touched))
        return dev_err_probe(dev, -EUCLEAN,
            "PIE DMA already published in this module lifetime; qualified reset required\n");
    if (of_property_count_u32_elems(dev->of_node, "tx_ring") != ARRAY_SIZE(tx) ||
        of_property_count_u32_elems(dev->of_node, "rx_ring") != ARRAY_SIZE(rx) ||
        of_property_read_u32_array(dev->of_node, "tx_ring", tx, ARRAY_SIZE(tx)) ||
        of_property_read_u32_array(dev->of_node, "rx_ring", rx, ARRAY_SIZE(rx)))
        return -EINVAL;
    if (tx[0] != HSAN_NET_TX_ORDER || tx[1] != 8)
        return dev_err_probe(dev, -EINVAL, "OEM TX ring definitions changed\n");
    rx0_large = of_property_read_bool(dev->of_node,
                                     "hsan,experimental-rx0-large-buffer");
    if (rx0_large && !of_property_read_bool(dev->of_node,
                                          "hsan,experimental-cpu-lan1-diagnostic"))
        return dev_err_probe(dev, -EINVAL, "RX0 large buffer requires CPU diagnostic mode\n");
    for (ch = 0; ch < HSAN_NET_RX_CHANNELS; ch++)
        if (rx[2 * ch] != HSAN_NET_RX_ORDER ||
            rx[2 * ch + 1] != (rx0_large && !ch ? 15360 : HSAN_BUF_SIZE))
            return dev_err_probe(dev, -EINVAL,
                "ordinary RX%u has unexpected diagnostic ring/buffer geometry\n", ch);
    /* OEM probe explicitly treats an absent gro_ring as zero GRO rings.
     * Parse the optional geometry rather than silently ignoring it. The
     * recovered 4x(64,15360) configuration registers for diagnostics, but
     * ndo_open refuses it until vendor GRO receive semantics are ported.
     */
    if (of_find_property(dev->of_node, "gro_ring", NULL)) {
        cells = of_property_count_u32_elems(dev->of_node, "gro_ring");
        if (cells <= 0 || cells > ARRAY_SIZE(gro) || (cells & 1) ||
            of_property_read_u32_array(dev->of_node, "gro_ring", gro, cells))
            return dev_err_probe(dev, -EINVAL, "invalid optional GRO ring geometry\n");
        gro_channels = cells / 2;
        for (ch = 0; ch < gro_channels; ch++)
            if (gro[2 * ch] != 6 || gro[2 * ch + 1] != 15360)
                return dev_err_probe(dev, -EINVAL,
                    "unrecognized OEM GRO%u descriptor/buffer geometry\n", ch);
    }
    irq_count = of_irq_count(dev->of_node);
    polling = of_property_read_bool(dev->of_node,
                                   "hsan,experimental-polling-without-irq");
    if (irq_count != HSAN_OEM_IRQ_GROUPS && !(irq_count == 0 && polling))
        return dev_err_probe(dev, -EINVAL,
            "PIE requires all four OEM RX IRQ groups (found %d)\n", irq_count);
    /* A zero of_irq_count() may be a parse failure at index zero,
     * not the absence of an interrupt declaration.
     */
    if (!irq_count &&
        (of_property_present(dev->of_node, "interrupts") ||
         of_property_present(dev->of_node, "interrupts-extended")))
        return dev_err_probe(dev, -EINVAL,
            "PIE will not poll with unresolved DT IRQ declarations\n");
    ndev = alloc_etherdev_mqs(sizeof(*p), 1, HSAN_NET_RX_CHANNELS + gro_channels);
    if (!ndev)
        return -ENOMEM;
    p = netdev_priv(ndev);
    p->dev = dev;
    p->ndev = ndev;
    p->tx_count = BIT(HSAN_NET_TX_ORDER);
    p->tx_mask = p->tx_count - 1;
    p->gro_channels = gro_channels;
    p->rx_channels = HSAN_NET_RX_CHANNELS + gro_channels;
    p->irq_resources = irq_count;
    p->regs = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(p->regs)) {
        ret = PTR_ERR(p->regs);
        goto err_free;
    }
    ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
    if (ret)
        goto err_free;
    ret = hsan_pie_boot_reset(pdev, p);
    if (ret)
        goto err_free;
    if (of_property_read_bool(dev->of_node,
                             "hsan,experimental-program-pie-acp-at-boot") &&
        !hsan_acp_pie_attrs_ready(p, true)) {
        ret = -EIO;
        goto err_free;
    }
    ret = hsan_pie_boot_globals(p);
    if (ret)
        goto err_free;
    spin_lock_init(&p->tx_lock);
    INIT_DELAYED_WORK(&p->watchdog, hsan_watchdog);
    ret = hsan_collect_rx_irqs(pdev, irq_count != 0, irqs);
    if (ret)
        goto err_free;
    for (ch = 0; ch < p->rx_channels; ch++) {
        struct hsan_v300_rx_queue *q = &p->rxq[ch];

        q->parent = p;
        q->channel = ch;
        q->ring_order = ch < HSAN_NET_RX_CHANNELS ? HSAN_NET_RX_ORDER : 6;
        q->buffer_size = ch < HSAN_NET_RX_CHANNELS ? rx[2 * ch + 1] : 15360;
        q->rx_count = BIT(q->ring_order);
        q->rx_mask = q->rx_count - 1;
        q->irq = irqs[ch < HSAN_NET_RX_CHANNELS ? ch :
                          HSAN_OEM_RX_CHANNELS - 1 - ch];
        netif_napi_add(ndev, &q->napi, hsan_napi_poll);
        added_napi++;
    }
    strscpy(ndev->name, "lan1", IFNAMSIZ);
    ndev->netdev_ops = &hsan_ops;
    ndev->ethtool_ops = &hsan_ethtool_ops;
    ndev->watchdog_timeo = msecs_to_jiffies(5000);
    ndev->min_mtu = ETH_DATA_LEN;
    ndev->max_mtu = ETH_DATA_LEN;
    ret = of_get_ethdev_address(dev->of_node, ndev);
    if (ret == -EPROBE_DEFER)
        goto err_napi;
    if (ret) {
        struct device_node *mac_np;

        for_each_compatible_node(mac_np, NULL, "hsan,mac") {
            struct resource mac_res;
            const char *mode;
            u32 port;

            /* The fallback supplies address metadata only. Match the
             * enabled LAN1 node, not a disabled same-base declaration.
             */
            if (!of_device_is_available(mac_np) ||
                of_address_to_resource(mac_np, 0, &mac_res) ||
                resource_type(&mac_res) != IORESOURCE_MEM ||
                mac_res.start != 0x14400000U ||
                resource_size(&mac_res) != 0x10000U ||
                of_property_read_u32(mac_np, "port", &port) ||
                port != HSAN_LAN1_PORT ||
                of_property_read_string(mac_np, "phy-mode", &mode) ||
                strcmp(mode, "gmii"))
                continue;
            ret = of_get_ethdev_address(mac_np, ndev);
            break;
        }
        of_node_put(mac_np);
        if (ret == -EPROBE_DEFER)
            goto err_napi;
        if (ret) {
            eth_hw_addr_random(ndev);
            dev_warn(dev,
                "no device MAC in PIE/port10 DT/nvmem; using a random local address\n");
        }
    }
    SET_NETDEV_DEV(ndev, dev);
    netif_carrier_off(ndev);
    ret = register_netdev(ndev);
    if (ret)
        goto err_napi;
    platform_set_drvdata(pdev, ndev);
    if (p->boot_reset_verified) {
        ret = hsan_lsw_pie_boot_reset_publish(p->gro_channels);
        if (ret) {
            unregister_netdev(ndev);
            goto err_napi;
        }
        dev_info(dev, "PIE empty boot owner published: ordinary=4 GRO=%u\n",
                 p->gro_channels);
    }
    if (hsan_clock_gates_ready(p)) {
        unsigned int ch;

        dev_info(dev, "PIE boot RX global=%#x (read-only, not a STOP ACK)\n",
                 hsan_r32(p, HSAN_RX_GLOBAL));
        for (ch = 0; ch < HSAN_OEM_RX_CHANNELS; ch++)
            dev_info(dev, "PIE boot RX%u base=%#x ring=%#x wptr=%#x rptr=%#x\n",
                     ch, hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_BASE)),
                     hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RING)),
                     hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_WPTR)),
                     hsan_r32(p, hsan_rx_chan_reg(ch, HSAN_CH_RPTR)));
    }
    hsan_report_startup_blockers(p);
    dev_info(dev,
        "Stage79 PIE: ordinary RX0..3 each 1024x2048; %u configured GRO rings remain unsupported; IRQ groups 0/7,1/6,2/5,3/4; %s\n",
        p->gro_channels, irq_count ? "four physical IRQs" : "explicit watchdog-only mode");
    return 0;
err_napi:
    while (added_napi)
        netif_napi_del(&p->rxq[--added_napi].napi);
err_free:
    free_netdev(ndev);
    return ret;
}

static void hsan_remove(struct platform_device *pdev)
{
    struct net_device *ndev = platform_get_drvdata(pdev);
    struct hsan_v300 *p = netdev_priv(ndev);
    unsigned int ch;

    if (p->boot_reset_verified)
        hsan_lsw_pie_boot_reset_invalidate();
    unregister_netdev(ndev);
    cancel_delayed_work_sync(&p->watchdog);
    /* napi_disable releases scheduling ownership, not necessarily the
     * entire callback tail after complete_done. Keep p/q/MMIO alive
     * until every netif_napi_del has synchronized network readers and
     * stopped its optional NAPI thread. A stopped tail checks active
     * under tx_lock and only touches retained software state, not rings.
     */
    for (ch = 0; ch < p->rx_channels; ch++)
        netif_napi_del(&p->rxq[ch].napi);
    if (!p->dma_ever_started)
        hsan_free_unstarted(p);
    else
        dev_warn(p->dev, "retaining all hardware-visible DMA memory until qualified reset\n");
    free_netdev(ndev);
}

/* No audited stop/reset contract exists once the PIE DMA rings were
 * submitted. Never let system suspend/hibernate assert a false quiescence
 * transition and later resume using still-live physical DMA pointers.
 */
static int hsan_v300_pm_suspend(struct device *dev)
{
    struct net_device *ndev = dev_get_drvdata(dev);
    struct hsan_v300 *p;

    if (atomic_read(&hsan_pie_dma_lifetime_touched))
        return -EBUSY;
    if (!ndev)
        return 0;
    p = netdev_priv(ndev);
    return READ_ONCE(p->active) || READ_ONCE(p->dma_ever_started) ?
        -EBUSY : 0;
}

static const struct dev_pm_ops hsan_v300_pm_ops = {
    .suspend = hsan_v300_pm_suspend,
    .freeze = hsan_v300_pm_suspend,
    .poweroff = hsan_v300_pm_suspend,
};

static const struct of_device_id hsan_of_match[] = {
	{ .compatible = "hsan,pie" }, { }
};
MODULE_DEVICE_TABLE(of, hsan_of_match);
static struct platform_driver hsan_driver = {
	.probe = hsan_probe,
	.remove = hsan_remove,
    /* Prevent manual sysfs unbind while DMA may own leaked/retained pages.
     * Does not veto physical hot-remove, DT overlay removal or system reset.
     */
    .driver = {.name = "hsan-net-v300", .of_match_table = hsan_of_match,
               .suppress_bind_attrs = true, .pm = &hsan_v300_pm_ops},
};
module_platform_driver(hsan_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V Stage79 PIE v300 four ordinary RX queues and STOP-serialized DMA");
