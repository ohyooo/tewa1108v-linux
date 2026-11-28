/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_EQM_DIAG_DECODE_H
#define HSAN_EQM_DIAG_DECODE_H

/*
 * Decode CPU-endian words returned by the audited EQM table interface.
 * No MMIO, allocation, barriers or DMA ownership changes occur here.
 *
 * Evidence: hi_lsw_dp.ko, SHA-256
 * 250e5da2dd32f65887069a4b0c54773e9258bf48b988b7aff122620f1768bded.
 * Anonymous .text+0x2bfc..0x2d6b (NOT chba_queue_cnt):
 *   0x2c50/0x2c6c read table 81 then table 80;
 *   0x2c78..0x2cb0 reconstruct 36-bit out, in and drop sum;
 *   0x2cfc..0x2d14 extract cell/priv;
 *   0x2d18..0x2d48 print in/out/drop/tail/low.
 * The caller requests eight words, but hi_eqm_read_tab 0x1e8c..0x1e94
 * clamps the actual transfer to five words. Word 4 of table 81 is needed.
 *
 * Tables 81 and 80 are separate command transactions. These decoded fields
 * are not an atomic cross-table snapshot and do not prove DMA quiescence.
 * in_derived is the OEM diagnostic expression, not a separately read counter.
 * in_out_delta is the raw 15-bit term added to out; its relation to all
 * outstanding DMA requests is not established by this diagnostic.
 */

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 hsan_eqm_diag_u32;
typedef u64 hsan_eqm_diag_u64;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t hsan_eqm_diag_u32;
typedef uint64_t hsan_eqm_diag_u64;
#endif

#define HSAN_EQM_DIAG_TABLE80_WORDS 3U
#define HSAN_EQM_DIAG_TABLE81_WORDS 5U

struct hsan_eqm_diag_decoded {
	/* Original format strings name these fields cell and priv. */
	hsan_eqm_diag_u32 cell;
	hsan_eqm_diag_u32 priv;
	hsan_eqm_diag_u32 in_out_delta;
	hsan_eqm_diag_u32 tail;
	hsan_eqm_diag_u32 low;
	/* Table 81 word 2 contributes to drop; no further name is inferred. */
	hsan_eqm_diag_u32 drop_component2;
	hsan_eqm_diag_u64 out;
	hsan_eqm_diag_u64 in_derived;
	hsan_eqm_diag_u64 drop_sum;
};

/*
 * Both word counts describe accessible u32 elements, not byte counts.
 * Inputs must be aligned CPU-endian arrays, not byte-cast DMA buffers.
 * Returns false for invalid/short input; leaves *out untouched on failure.
 * A successful decode establishes only that the input fields were decoded.
 */
static inline bool
hsan_eqm_diag_decode(const hsan_eqm_diag_u32 *table80, size_t nr80,
		     const hsan_eqm_diag_u32 *table81, size_t nr81,
		     struct hsan_eqm_diag_decoded *out)
{
	struct hsan_eqm_diag_decoded decoded;

	if (!out || !table80 || !table81 ||
	    nr80 < HSAN_EQM_DIAG_TABLE80_WORDS ||
	    nr81 < HSAN_EQM_DIAG_TABLE81_WORDS)
		return false;

	decoded.cell = table80[1] & 0x7fffU;
	decoded.priv = table80[0] & 0x3ffU;
	decoded.in_out_delta = table80[2] & 0x7fffU;
	decoded.tail = table81[0];
	decoded.low = table81[1];
	decoded.drop_component2 = table81[2];
	decoded.out = ((hsan_eqm_diag_u64)(table81[4] & 0xfU) << 32) |
		      (hsan_eqm_diag_u64)table81[3];
	decoded.in_derived = decoded.out + decoded.in_out_delta;
	decoded.drop_sum = (hsan_eqm_diag_u64)table81[0] + table81[1] +
			   table81[2];
	*out = decoded;
	return true;
}

#endif /* HSAN_EQM_DIAG_DECODE_H */
