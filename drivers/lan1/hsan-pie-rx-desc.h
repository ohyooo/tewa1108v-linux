/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PIE_RX_DESC_H
#define HSAN_PIE_RX_DESC_H

#include <linux/types.h>
#include <linux/unaligned.h>

struct hsan_pie_rx_metadata {
	u16 length;
	u8 port;
	u8 subport;
	u8 gro_segments;
	bool bypass;
};

/* hi_kpie.ko v300 hi_pie_rx_get_desc .text+0x1c80..0x1dc8.
 * Call only after the coherent descriptor's DMA completion barrier.
 * Port/subport have two encodings, selected by descriptor byte19 bit3.
 * The byte8 low7 field is not the source port. No checksum or GRO
 * capability is inferred from decoding these fields.
 */
static inline struct hsan_pie_rx_metadata
hsan_pie_rx_decode(const u8 *desc)
{
	bool bypass = !!(desc[19] & 0x08);

	return (struct hsan_pie_rx_metadata) {
		.length = get_unaligned_le16(desc),
		.port = bypass ? desc[18] : (desc[9] >> 3),
		.subport = bypass ? desc[17] : desc[18],
		.gro_segments = get_unaligned_le32(desc + 20) >> 27,
		.bypass = bypass,
	};
}

static inline bool
hsan_pie_rx_metadata_equal(const struct hsan_pie_rx_metadata *a,
			   const struct hsan_pie_rx_metadata *b)
{
	return a->length == b->length && a->port == b->port &&
	       a->subport == b->subport && a->gro_segments == b->gro_segments &&
	       a->bypass == b->bypass;
}

#endif
