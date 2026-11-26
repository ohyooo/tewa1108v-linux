/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_CAPA_LOOKUP_PLAN_H
#define HSAN_CAPA_LOOKUP_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 hsan_capa_u8;
typedef u16 hsan_capa_u16;
typedef u32 hsan_capa_u32;
typedef s32 hsan_capa_s32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t hsan_capa_u8;
typedef uint16_t hsan_capa_u16;
typedef uint32_t hsan_capa_u32;
typedef int32_t hsan_capa_s32;
#endif

#define HSAN_CAPA_SELECTOR7_TABLE_ID 0x11bU
#define HSAN_CAPA_SELECTOR7_UNTAGGED 0x1000U
#define HSAN_CAPA_SELECTOR7_PREFIX_LEN 16U
#define HSAN_CAPA_SELECTOR7_HANDLE_TAG 0x40000000U
#define HSAN_CAPA_SELECTOR7_FAILURE 0x90000000U

/*
 * Only the independently established prefix of the OEM's 32-byte CPU
 * candidate. The missing half depends on earlier operations and metadata.
 * No complete hardware row or buffer for the table manager is returned.
 */
struct hsan_capa_selector7_input {
	hsan_capa_u8 address_octets[6];
	hsan_capa_u16 raw_vid;
	hsan_capa_u32 selector;
};

struct hsan_capa_selector7_prefix {
	hsan_capa_u8 octets[HSAN_CAPA_SELECTOR7_PREFIX_LEN];
};

static inline bool
hsan_capa_selector7_prefix_plan(const struct hsan_capa_selector7_input *in,
			       struct hsan_capa_selector7_prefix *out)
{
	struct hsan_capa_selector7_prefix result;
	hsan_capa_u32 selector_mask;
	hsan_capa_u16 field;
	unsigned int i;

	/* A U16 mask can express selectors 0..15. Refuse to infer what
	 * zero/truncated masks for larger selectors mean to later stages.
	 */
	if (!in || !out || in->selector >= 16U)
		return false;
	for (i = 0; i < 6; ++i)
		result.octets[i] = in->address_octets[5U - i];
	field = in->raw_vid == HSAN_CAPA_SELECTOR7_UNTAGGED ? 0 : in->raw_vid;
	result.octets[6] = (hsan_capa_u8)field;
	result.octets[7] = (hsan_capa_u8)(field >> 8);
	selector_mask = 1U << in->selector;
	result.octets[8] = (hsan_capa_u8)selector_mask;
	result.octets[9] = (hsan_capa_u8)(selector_mask >> 8);
	result.octets[10] = 1;
	result.octets[11] = 1;
	result.octets[12] = in->raw_vid == HSAN_CAPA_SELECTOR7_UNTAGGED ? 0x49 : 5;
	result.octets[13] = 0x45;
	result.octets[14] = 0;
	result.octets[15] = 0x40;
	*out = result;
	return true;
}

enum hsan_capa_selector7_stage {
	HSAN_CAPA_NEED_PREPARATION,
	HSAN_CAPA_PREPARATION_FAILED,
	HSAN_CAPA_NEED_LOOKUP_OBSERVATION,
	HSAN_CAPA_LOOKUP_RETURNED_HANDLE,
	HSAN_CAPA_NEED_ALLOCATOR_OBSERVATION,
	HSAN_CAPA_ALLOCATOR_RETURNED_HANDLE,
	HSAN_CAPA_ALLOCATOR_FAILED,
};

struct hsan_capa_selector7_observations {
	bool preparation_observed, lookup_observed, allocator_observed;
	hsan_capa_s32 preparation, lookup, allocator;
};

struct hsan_capa_selector7_outcome {
	enum hsan_capa_selector7_stage stage;
	bool has_oem_return, has_handle;
	hsan_capa_u32 oem_return_bits, handle;
};

/*
 * Classify supplied, same-candidate results; neither invokes nor authorizes
 * the operations. Reject out-of-order observations, leaving out untouched.
 * A result and a tagged handle describe the software ABI only. They do not
 * show that the table, an IFC rule, or any DMA user accepted the candidate.
 */
static inline bool
hsan_capa_selector7_observe(const struct hsan_capa_selector7_observations *in,
			  struct hsan_capa_selector7_outcome *out)
{
	struct hsan_capa_selector7_outcome r =
		{HSAN_CAPA_NEED_PREPARATION, false, false, 0, 0};

	if (!in || !out)
		return false;
	if (in->lookup_observed && (!in->preparation_observed || in->preparation))
		return false;
	if (in->allocator_observed && (!in->lookup_observed || in->lookup > 0))
		return false;

	if (in->preparation_observed) {
		if (in->preparation) {
			r.stage = HSAN_CAPA_PREPARATION_FAILED;
			r.has_oem_return = true;
			r.oem_return_bits = HSAN_CAPA_SELECTOR7_FAILURE;
		} else if (!in->lookup_observed) {
			r.stage = HSAN_CAPA_NEED_LOOKUP_OBSERVATION;
		} else if (in->lookup > 0) {
			r.stage = HSAN_CAPA_LOOKUP_RETURNED_HANDLE;
			r.has_oem_return = r.has_handle = true;
			r.handle = (hsan_capa_u32)in->lookup | HSAN_CAPA_SELECTOR7_HANDLE_TAG;
		} else if (!in->allocator_observed) {
			r.stage = HSAN_CAPA_NEED_ALLOCATOR_OBSERVATION;
		} else if (in->allocator >= 0) {
			r.stage = HSAN_CAPA_ALLOCATOR_RETURNED_HANDLE;
			r.has_oem_return = r.has_handle = true;
			r.handle = (hsan_capa_u32)in->allocator | HSAN_CAPA_SELECTOR7_HANDLE_TAG;
		} else {
			r.stage = HSAN_CAPA_ALLOCATOR_FAILED;
			r.has_oem_return = true;
			r.oem_return_bits = HSAN_CAPA_SELECTOR7_FAILURE;
		}
	}
	*out = r;
	return true;
}

#endif
