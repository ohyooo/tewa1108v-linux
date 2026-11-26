/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_LSW_CSR_H
#define HSAN_LSW_CSR_H

#include <linux/device.h>
#include <linux/io.h>
#include <linux/types.h>

/* Exact OEM hi_lsw_pub bank model: [31:24] = bank number; [23:0]
 * = register offset.  The OEM stored {address, virtual, length} per bank,
 * using a fixed 12-byte A32 structure; this kernel implementation uses
 * native pointers and avoids relying on that ARM32 structure layout.
 */
int hsan_lsw_register_bank(struct device *owner, u8 bank,
			  void __iomem *regs, resource_size_t start,
			  resource_size_t bytes);
void hsan_lsw_unregister_bank(struct device *owner, u8 bank);
int hsan_lsw_read(u32 logical_address, u32 *value);
int hsan_lsw_write(u32 logical_address, u32 value);
/* One ownership-locked MMIO write + full readback. The return value
 * qualifies that CSR only, never a downstream subsystem state machine.
 */
int hsan_lsw_write_verify(u32 logical_address, u32 value);
int hsan_lsw_update_bits(u32 logical_address, u32 mask, u32 value);
/* Verify the masked fields within the same bank lock as the RMW itself. */
int hsan_lsw_update_bits_verify(u32 logical_address, u32 mask, u32 value);
int hsan_lsw_write_tab128(u32 base, u32 row, const u32 words[4]);
/* Stage78: OEM hi_lsw_pfe:l3intf_write publishes row A then row B.
 * Compare BOTH preimages, validate BOTH mapped spans and perform the 8
 * writes under ONE CPU CSR owner lock. This prevents concurrent CPU
 * interleaving, not partial visibility to hardware or DMA drain.
 * Only exact L3 interface table 0x102 addresses/rows AND the original
 * registered platform device owner are admitted. Single-shot per boot.
 * submitted is true as soon as the FIRST irreversible MMIO is issued;
 * a failed readback requires hardware quarantine, never rollback.
 */
int hsan_lsw_l3intf_pair_commit(struct device *owner, u32 row,
                               const u32 before_a[4], const u32 before_b[4],
                               const u32 after_a[4], const u32 after_b[4],
                               bool *submitted, bool *readback_ok);
/* Read-only preflight of the same owner, spans and preimages. It reserves
 * nothing; commit repeats every check under its own uninterrupted lock.
 */
int hsan_lsw_l3intf_pair_preflight(struct device *owner, u32 row,
                                  const u32 before_a[4],
                                  const u32 before_b[4]);
/* OEM read_tab128 word order, locked across all 4 reads; read-only. */
int hsan_lsw_read_tab128(u32 base, u32 row, u32 words[4]);
/* One-shot 255-row MCC stream and five register controls, dual-bank owner.
 * No MCC reset/ACK is proven; success means only all writes submitted.
 */
int hsan_lsw_mcc_health(void);
int hsan_lsw_mcc_commit(u32 *rows_written);
/* OEM IFC 0x120 command/status and 0x50000 two-row table transport.
 * The caller must supply observed post-encoded control/table words; this
 * interface neither invents default IFC rules nor enables forwarding.
 */
int hsan_lsw_ifc_health(void);
/* Read both CPU-visible IFC table halves under the CSR ownership lock.
 * This is NOT a hardware lookup-table activation or packet forwarding ACK.
 */
int hsan_lsw_ifc_read_window(u16 index, u32 words[8]);
/* Permanently quarantine and pin CSR owner after non-reversible IFC writes
 * whose physical table backing cannot be read back/ACKed. */
int hsan_lsw_ifc_quarantine(void);
int hsan_lsw_ifc_commit(u16 index, const u32 control[7],
                        const u32 data[8], u32 *last_status);
/* Stage44 OEM NAPT 2-command transaction: observed board words only.
 * Successful bit0-clear polls do NOT imply installed NAT hardware flows.
 * A first attempted write permanently quarantines/re-pins this engine.
 */
int hsan_lsw_napt_health(void);
int hsan_lsw_napt_partition_commit(const u32 words[5], u32 *last_status);
/* OEM hi_fam_init_buf bank40+42+43 register transaction. Neither ACK nor
 * a successful register commit qualifies a DMA stop or the LSW pipeline.
 * Once an irreversible write starts the owner is pinned until board reset.
 */
struct hsan_lsw_fam_config {
    u32 memory_base_reg;
    u32 unit14;
    u32 low16;
    u32 mode;
    u32 dma_bus_addr;
    u32 entries;
    u32 count16;
};
int hsan_lsw_fam_health(void);
/* IRQ-safe software-only FAM STOP fence; not an OEM HW STOP command. */
void hsan_lsw_fam_cpu_stop_fence(void);
/* IRQ-safe CPU publication domain, shared by PIE TX/RX/IRQ and the
 * CSR FAM STOP fence. Call begin/end while holding the PIE tx_lock.
 * No sleeping, allocators, PHY operations or hardware STOP assumptions
 * are permitted inside this critical section.
 */
struct hsan_lsw_fam_publish_guard {
    unsigned long irq_flags;
    bool held;
};
int hsan_lsw_fam_publish_begin(struct hsan_lsw_fam_publish_guard *guard);
int hsan_lsw_ips_queue_init(void);
int hsan_lsw_ips_boot_enable(void);
int hsan_lsw_ips_enable_guarded(struct hsan_lsw_fam_publish_guard *guard);
/* A completed TX mapping can be unmapped only after full-batch descriptor
 * validation AND admission under the shared FAM STOP lock. Unlike publish
 * begin, this counts releases separately. Use publish_end for unlocking.
 * This is NOT hardware DMA quiescence or a DMA STOP ACK.
 */
int hsan_lsw_fam_reclaim_begin(struct hsan_lsw_fam_publish_guard *guard);
/* IRQ-safe read-only completion/descriptor snapshot admission.
 * Serialized with CPU STOP without counting an MMIO publication or unmap.
 * Must pair a successful call with hsan_lsw_fam_publish_end().
 */
int hsan_lsw_fam_observe_begin(struct hsan_lsw_fam_publish_guard *guard);
/* IRQ mask/ACK service only: excludes CPU STOP for the existing service
 * writes, without counting a DMA publication or mapping reclaim.
 * Does not admit IRQ unmask, producer writes or hardware STOP/drain.
 * Must pair success with hsan_lsw_fam_publish_end().
 */
int hsan_lsw_fam_irq_service_begin(struct hsan_lsw_fam_publish_guard *guard);
void hsan_lsw_fam_publish_end(struct hsan_lsw_fam_publish_guard *guard);
/* A CPU-only, linearized write-publication fence. An admitted write may
 * have reached the device before this fence. It is NOT a DMA STOP ACK.
 */
struct hsan_lsw_fam_fence_snapshot {
    bool cpu_fenced;
    u64 rejected_write_operations;
    u64 rejected_dma_publications; /* PIE producer attempts after fence */
    u64 admitted_cpu_publications; /* producer begin() only */
    u64 rejected_tx_reclaims; /* external CPU STOP won before DMA unmap */
    u64 admitted_tx_reclaims; /* software-consumer-validated, not HW drain */
    u64 stop_fence_invocations; /* all CPU fence calls, NOT HW STOP attempts */
};
void hsan_lsw_fam_fence_snapshot(struct hsan_lsw_fam_fence_snapshot *out);
/* OEM hi_fam_dump_status @0x20d0: bank43+0x60d4/+0x61ac.
 * Non-latched, read-only observations; no DMA stop acknowledgement.
 */
struct hsan_lsw_fam_activity {
    u32 raw_60d4;
    u32 raw_61ac;
    u32 value_21bit;
    u32 value_14bit;
    u32 raw_init_ack_status; /* 0x43000000: INIT ack only, NOT stop */
    u64 sample_begin_ns; /* monotonic CPU timestamps, not HW latched */
    u64 sample_end_ns;
};
int hsan_lsw_fam_read_activity(struct hsan_lsw_fam_activity *out);
int hsan_lsw_fam_commit(const struct hsan_lsw_fam_config *cfg,
                        u32 *polls, u32 *last_status);
bool hsan_lsw_bank_mapped(u8 bank);
int hsan_lsw_check_span(u32 logical_address, u32 bytes, bool for_write);
/* Stage24: software-only table metadata owner. These are NOT hardware
 * hi_tab_mgr_create() and must never be used as a pipeline-ready signal.
 * OEM id is 13 bits; bits 6..7 are reserved, as in hi_lsw_pub A32.
 */
int hsan_lsw_shadow_table_create(struct device *owner, u16 id,
				 const char *name, u32 entries,
				 u32 entry_bytes, u8 oem_flags26);
/* NAPT only: logical table size is OEM-sized; cells allocate lazily.
 * These are still CPU shadow rows, not hardware table contents.
 */
int hsan_lsw_shadow_table_create_sparse(struct device *owner, u16 id,
                                       const char *name, u32 entries,
                                       u32 entry_bytes, u8 oem_flags26);
int hsan_lsw_shadow_table_delete(struct device *owner, u16 id);
int hsan_lsw_shadow_table_reserve_range(struct device *owner, u16 id,
                                        u32 first, u32 count);
int hsan_lsw_shadow_table_alloc_range(struct device *owner, u16 id,
                                      u32 count, u32 alignment, u32 *first);
int hsan_lsw_shadow_table_release_range(struct device *owner, u16 id,
                                        u32 first, u32 count);
int hsan_lsw_shadow_table_write(struct device *owner, u16 id, u32 row,
                                const void *data, size_t bytes);
int hsan_lsw_shadow_table_read(struct device *owner, u16 id, u32 row,
                               void *data, size_t bytes);
/* CPU-only row counts, not hardware forwarding readiness. */
int hsan_lsw_shadow_table_usage(struct device *owner, u16 id,
                                u32 *reserved, u32 *materialized);
/* OEM hi_eqm_write_tab / hi_eqm_read_tab copy at most five words,
 * even when the original caller supplies an eight-word stack buffer.
 * Callers MUST truncate explicitly and must not interpret success as
 * queue/FAM/PIE/PFE readiness.  The hardware register gate defaults off.
 */
#define HSAN_LSW_EQM_DATA_WORDS 5U
/* Reject writes/reads after an uncertain EQM transaction; no implicit reset. */
int hsan_lsw_eqm_health(void);
int hsan_lsw_eqm_write_table(u8 table, u16 row, const u32 *words,
                             u32 nr_words, u32 *last_status);
int hsan_lsw_eqm_read_table(u8 table, u16 row, u32 *words,
                            u32 nr_words, u32 *last_status);
int hsan_lsw_eqm_write_verify_table(u8 table, u16 row, const u32 *words,
                                    const u32 *expected_readback,
                                    u32 nr_words, u32 *last_status);
/* This predicate is deliberately false until a complete and separately
 * audited queue + PFE datapath initialisation implements a real handoff.
 * Resource mapping alone is NEVER sufficient to arm the PIE DMA engine.
 */
bool hsan_lsw_pipeline_operational(void);
/* A cold-reset PIE owner with no published descriptors, not DMA STOP. */
int hsan_lsw_pie_boot_reset_publish(u32 gro_channels);
void hsan_lsw_pie_boot_reset_invalidate(void);
int hsan_lsw_pie_boot_geometry(u32 *gro_start, u32 *gro_count);

#endif
