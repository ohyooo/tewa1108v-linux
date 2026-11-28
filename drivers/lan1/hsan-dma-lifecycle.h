/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_DMA_LIFECYCLE_H
#define HSAN_DMA_LIFECYCLE_H

#include <linux/types.h>

/* CPU protocol states. None denotes hardware STOP, drain, or ownership
 * return. The published allocations remain mapped in every retained state.
 * State and record updates are serialized by the driver's tx_lock.
 */
enum hsan_dma_phase {
	HSAN_DMA_UNPUBLISHED,
	HSAN_DMA_STARTING,
	HSAN_DMA_RUNNING,
	HSAN_DMA_STOPPING,
	HSAN_DMA_PAUSED_RETAINED,
	HSAN_DMA_RESUMING,
	HSAN_DMA_ERROR_RETAINED,
	HSAN_DMA_ERROR_UNPUBLISHED,
	HSAN_DMA_REMOVED_RETAINED,
};

struct hsan_dma_lifecycle {
	enum hsan_dma_phase phase;
	int last_errno;
	u64 sequence;
	u64 changed_ns;
	u64 operation_begin_ns;
	u64 operation_end_ns;
	u64 pause_attempts;
	u64 pause_completed;
	u64 resume_attempts;
	u64 resume_completed;
};

static inline const char *hsan_dma_phase_name(enum hsan_dma_phase phase)
{
	switch (phase) {
	case HSAN_DMA_UNPUBLISHED:
		return "UNPUBLISHED";
	case HSAN_DMA_STARTING:
		return "STARTING";
	case HSAN_DMA_RUNNING:
		return "RUNNING";
	case HSAN_DMA_STOPPING:
		return "STOPPING";
	case HSAN_DMA_PAUSED_RETAINED:
		return "PAUSED_RETAINED";
	case HSAN_DMA_RESUMING:
		return "RESUMING";
	case HSAN_DMA_ERROR_RETAINED:
		return "ERROR_RETAINED";
	case HSAN_DMA_ERROR_UNPUBLISHED:
		return "ERROR_UNPUBLISHED";
	case HSAN_DMA_REMOVED_RETAINED:
		return "REMOVED_RETAINED";
	default:
		return "INVALID";
	}
}

#endif /* HSAN_DMA_LIFECYCLE_H */
