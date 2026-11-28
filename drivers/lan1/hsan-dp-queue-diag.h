/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_DP_QUEUE_DIAG_H
#define HSAN_DP_QUEUE_DIAG_H

#include <linux/types.h>
#include "hsan-eqm-diag-decode.h"

/* Matches the exact existing port10/CPU diagnostic queue selection.
 * It does not enumerate every producer in the SoC.
 */
#define HSAN_DP_QUEUE_DIAG_COUNT 64U
#define HSAN_DP_QUEUE_DIAG_T81_VALID 1U
#define HSAN_DP_QUEUE_DIAG_T80_VALID 2U
#define HSAN_DP_QUEUE_DIAG_BOTH_VALID 3U

struct hsan_dp_queue_diag {
	u32 table81[HSAN_EQM_DIAG_TABLE81_WORDS];
	u32 table80[HSAN_EQM_DIAG_TABLE80_WORDS];
	u64 begin_ns;
	u64 end_ns;
	u16 qid;
	u8 valid_tables;
};

/* Sleeping context. Each table read issues an EQM command under the existing
 * FAM/EQM locks and poison/fence checks. The pair is NOT hardware-atomic.
 * Existing owner, route, diagnostic opt-in and CPU-stop guards still apply.
 * On an attempted transaction, preserve timing and partial-table validity;
 * the return value and BOTH_VALID are required before decoding the pair.
 * No entry here grants permission to reclaim DMA or replay initialization.
 */
int hsan_lsw_dp_queue_diag(unsigned int index, struct hsan_dp_queue_diag *out);

#endif /* HSAN_DP_QUEUE_DIAG_H */
