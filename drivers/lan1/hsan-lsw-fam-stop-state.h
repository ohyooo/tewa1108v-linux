/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_LSW_FAM_STOP_STATE_H
#define HSAN_LSW_FAM_STOP_STATE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <errno.h>
#endif

/* No state here represents a hardware STOP or a DMA drain. */
enum hsan_fam_init_phase {
	HSAN_FAM_INIT_NOT_ATTEMPTED,
	HSAN_FAM_INIT_IN_FLIGHT,
	HSAN_FAM_INIT_ACK_COMPLETE,
	HSAN_FAM_INIT_POLL_TIMEOUT,
	HSAN_FAM_INIT_FAILED,
};

struct hsan_fam_init_record {
	enum hsan_fam_init_phase phase;
	int init_errno;                /* Valid after the call returns. */
	unsigned int init_polls;       /* Diagnostic for the INIT transaction. */
	unsigned int init_raw_status;  /* May be stale, not a STOP register. */
};

enum hsan_fam_stop_evidence {
	HSAN_FAM_STOP_UNAUDITED = 0,
};

struct hsan_dp_fam_stop_snapshot {
	struct hsan_fam_init_record arm;
	enum hsan_fam_stop_evidence stop_evidence;
	bool dp_owner_bound;
	bool dp_owner_quarantined;
	bool cpu_publications_fenced;
	bool dma_may_have_device_owner;
	bool hw_stop_ack_seen;
	bool outstanding_dma_drained;
};

/* Serialize history updates and observations at the caller. */
static inline bool hsan_fam_init_begin(struct hsan_fam_init_record *record)
{
	if (record->phase != HSAN_FAM_INIT_NOT_ATTEMPTED)
		return false;
	record->phase = HSAN_FAM_INIT_IN_FLIGHT;
	return true;
}

static inline int hsan_fam_init_finish(struct hsan_fam_init_record *record,
				      int rc, unsigned int polls,
				      unsigned int raw_status)
{
	if (record->phase != HSAN_FAM_INIT_IN_FLIGHT)
		return -EALREADY;
	/* An INIT success cannot lack the post-start observation. On all
	 * errors, even if INIT bit0 was seen, retain the possibility of DMA.
	 */
	if (!rc && (!polls || !(raw_status & 1U)))
		rc = -EUCLEAN;

	record->init_errno = rc;
	record->init_polls = polls;
	record->init_raw_status = raw_status;
	if (!rc)
		record->phase = HSAN_FAM_INIT_ACK_COMPLETE;
	else if (rc == -ETIMEDOUT)
		record->phase = HSAN_FAM_INIT_POLL_TIMEOUT;
	else
		record->phase = HSAN_FAM_INIT_FAILED;
	return rc;
}

static inline bool hsan_fam_retain_device_resources(bool arm_attempted,
						      bool may_be_owned)
{
	/* Resource or transaction state may be only partially published. */
	return arm_attempted || may_be_owned;
}

static inline void hsan_fam_project_stop(
	const struct hsan_fam_init_record *record, bool owner_bound,
	bool owner_quarantined, bool cpu_fenced,
	struct hsan_dp_fam_stop_snapshot *out)
{
	*out = (struct hsan_dp_fam_stop_snapshot) {
		.arm = *record,
		.stop_evidence = HSAN_FAM_STOP_UNAUDITED,
		.dp_owner_bound = owner_bound,
		.dp_owner_quarantined = owner_bound && owner_quarantined,
		.cpu_publications_fenced = cpu_fenced,
		.dma_may_have_device_owner =
			record->phase != HSAN_FAM_INIT_NOT_ATTEMPTED,
		.hw_stop_ack_seen = false,
		.outstanding_dma_drained = false,
	};
}

#endif
