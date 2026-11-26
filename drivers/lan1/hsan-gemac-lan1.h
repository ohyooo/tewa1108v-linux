/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_GEMAC_LAN1_H
#define HSAN_GEMAC_LAN1_H
#include <linux/types.h>
#include <linux/if_ether.h>
struct phy_device;

/* No MAC link configuration writes are performed here. This contract
 * describes one read-only CSR snapshot for PIE's pre-DMA gate.
 */
#define HSAN_GEMAC_MTU_MASK 0x3fffU
/* OEM MAC callback uses the configured MTU ceiling plus Ethernet
 * header, then inserts four FCS bytes. This is not the netdev MTU.
 * The port remains fixed at the default untagged Ethernet ceiling.
 */
#define HSAN_GEMAC_LAN1_FRAME_LIMIT (ETH_FRAME_LEN + ETH_FCS_LEN)
struct hsan_gemac_snapshot {
    u32 config_csr;
    u32 port;
    u32 phy_addr;
    bool gate_on;
    bool mac_link_programmed;
    bool link_hw_attempted;
    bool link_hw_uncertain;
    bool link_module_pinned;
    bool link_register_readback_valid; /* three live GEMAC MMIO fields */
    bool link_register_drift_latched; /* irrecoverable one-shot link mode */
    u32 programmed_speed_mbps;
    bool programmed_full_duplex;
    bool programmed_rx_pause;
    bool programmed_tx_pause;
};
/* Original hi_kmac.ko hi_mac_hw_get_stats64, ARM32 .text+0x914:
 * Four raw totals plus five error pairs from hi_mac_dump_cnt @0x784.
 * Nine raw 64-bit register pairs. These are CPU-visible MMIO snapshots,
 * not a verified LAN1 packet/byte accounting or DMA-drain certificate.
 */
#define HSAN_GEMAC_OEM_COUNTER_PAIRS 9U
struct hsan_gemac_oem_counters {
    bool requested;
    bool valid;
    u64 raw[HSAN_GEMAC_OEM_COUNTER_PAIRS];
    u64 sample_begin_ns; /* CPU monotonic, not chip counter latch */
    u64 sample_end_ns;
};
int hsan_gemac_lan1_oem_counters(struct hsan_gemac_oem_counters *out);
int hsan_gemac_lan1_snapshot(struct hsan_gemac_snapshot *snapshot);
struct hsan_gemac_link_snapshot {
	u32 phy_id;
	u16 bmsr;
	u16 bmcr;
	bool link_up;
	bool aneg_enabled;
	bool aneg_complete;
    bool resolved;
    bool full_duplex;
    bool rx_pause;
    bool tx_pause;
    u32 speed_mbps;
};
int hsan_gemac_lan1_link_snapshot(struct hsan_gemac_link_snapshot *snapshot);
int hsan_gemac_lan1_program_link(void);
/* Caller receives a device reference; must put_device() after attaching. */
int hsan_gemac_lan1_get_phy(struct phy_device **phy);
#endif
