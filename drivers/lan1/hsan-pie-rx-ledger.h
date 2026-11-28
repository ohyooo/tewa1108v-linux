/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PIE_RX_LEDGER_H
#define HSAN_PIE_RX_LEDGER_H

#include <linux/errno.h>
#include <linux/types.h>

/* These classes describe the software ring ledger relative to one sampled
 * hardware cursor. They do not certify device ownership, DMA completion on
 * an interconnect, or permission to unmap/rebuild a ring.
 */
enum hsan_pie_rx_slot_class {
	HSAN_PIE_RX_SLOT_UNCLASSIFIED,
	HSAN_PIE_RX_SLOT_CURSOR_COMPLETED,
	HSAN_PIE_RX_SLOT_PUBLISHED_AVAILABLE,
	HSAN_PIE_RX_SLOT_DEFERRED,
	HSAN_PIE_RX_SLOT_GAP,
	HSAN_PIE_RX_SLOT_CLASS_COUNT,
};

struct hsan_pie_rx_ledger {
	u32 ring_count;
	u32 cursor_completed;
	u32 published_available;
	u32 deferred_to_publish;
	u16 consumer_slot;
	u16 producer_slot;
	u16 hardware_slot;
};

/* OEM v300 RX uses ring-slot cursors and one unpublished gap. The current
 * driver retains cumulative u16 software counters, so their difference must
 * first be truncated to u16 and then bounded by the actual ring geometry.
 * Keep the caller's previous output untouched when validation fails.
 */
static inline int
hsan_pie_rx_ledger_decode(u32 ring_count, u16 sw_cons, u16 sw_prod,
			 u16 hw_slot, struct hsan_pie_rx_ledger *out)
{
	struct hsan_pie_rx_ledger value;
	u32 mask, published, completed;

	if (!out || ring_count < 2U || ring_count > 0x10000U ||
	    (ring_count & (ring_count - 1U)))
		return -EINVAL;
	if ((u32)hw_slot >= ring_count)
		return -ERANGE;

	mask = ring_count - 1U;
	published = (u16)(sw_prod - sw_cons);
	completed = ((u32)hw_slot - ((u32)sw_cons & mask)) & mask;
	if (published >= ring_count || completed > published)
		return -EUCLEAN;

	value.ring_count = ring_count;
	value.cursor_completed = completed;
	value.published_available = published - completed;
	value.deferred_to_publish = ring_count - 1U - published;
	value.consumer_slot = (u16)((u32)sw_cons & mask);
	value.producer_slot = (u16)((u32)sw_prod & mask);
	value.hardware_slot = hw_slot;
	*out = value;
	return 0;
}

/* Call with a ledger produced successfully by hsan_pie_rx_ledger_decode().
 * DEFERRED slots are the next slots a valid retained resume would publish;
 * descriptor owner/address integrity remains a separate observation.
 */
static inline enum hsan_pie_rx_slot_class
hsan_pie_rx_ledger_classify(const struct hsan_pie_rx_ledger *ledger, u32 slot)
{
	u32 distance, published;

	if (!ledger || slot >= ledger->ring_count)
		return HSAN_PIE_RX_SLOT_UNCLASSIFIED;
	distance = (slot - (u32)ledger->consumer_slot) &
		   (ledger->ring_count - 1U);
	published = ledger->cursor_completed + ledger->published_available;
	if (distance < ledger->cursor_completed)
		return HSAN_PIE_RX_SLOT_CURSOR_COMPLETED;
	if (distance < published)
		return HSAN_PIE_RX_SLOT_PUBLISHED_AVAILABLE;
	if (distance < published + ledger->deferred_to_publish)
		return HSAN_PIE_RX_SLOT_DEFERRED;
	return HSAN_PIE_RX_SLOT_GAP;
}

#endif
