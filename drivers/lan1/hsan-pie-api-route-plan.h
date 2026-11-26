/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PIE_API_ROUTE_PLAN_H
#define HSAN_PIE_API_ROUTE_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 hsan_pie_api_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t hsan_pie_api_u32;
#endif

/* OEM hi_kpie g_api_priv lookup keys; symbols identify CPU routing, not
 * physical GEMAC lanes, queue ownership or hardware handoff.
 */
#define HSAN_PIE_API_PORT_COUNT 32U
#define HSAN_PIE_API_SUBPORT_COUNT 256U

enum hsan_pie_api_slot {
	HSAN_PIE_API_ABSENT,      /* pointer proved null */
	HSAN_PIE_API_INACTIVE,    /* nonnull pointer with zero +4 field */
	HSAN_PIE_API_ATTACHED,    /* nonnull pointer with nonzero +4 field */
	HSAN_PIE_API_UNOBSERVED,
};

enum hsan_pie_api_route {
	HSAN_PIE_API_NO_TARGET,
	HSAN_PIE_API_EXACT_TARGET,
	HSAN_PIE_API_DEFAULT_TARGET,
	HSAN_PIE_API_UNDETERMINED,
};

static inline bool hsan_pie_api_index(hsan_pie_api_u32 port, hsan_pie_api_u32 subport,
				      hsan_pie_api_u32 *index)
{
	if (!index || port >= HSAN_PIE_API_PORT_COUNT ||
	    subport >= HSAN_PIE_API_SUBPORT_COUNT)
		return false;
	*index = port * HSAN_PIE_API_SUBPORT_COUNT + subport;
	return true;
}

/* get_net_priv_by_port @0x557c fetches default ONLY if exact pointer
 * is NULL, never when the exact pointer is present but unattached.
 * For subport zero, both reads would name the same slot. Classifying
 * supplied values is not a pointer lookup, callback, or DMA admission.
 */
static inline enum hsan_pie_api_route
hsan_pie_api_route_observe(hsan_pie_api_u32 port, hsan_pie_api_u32 subport,
			  enum hsan_pie_api_slot exact,
			  enum hsan_pie_api_slot fallback)
{
	if (port >= HSAN_PIE_API_PORT_COUNT ||
	    subport >= HSAN_PIE_API_SUBPORT_COUNT)
		return HSAN_PIE_API_NO_TARGET;
	switch (exact) {
	case HSAN_PIE_API_ATTACHED:
		return HSAN_PIE_API_EXACT_TARGET;
	case HSAN_PIE_API_INACTIVE:
		return HSAN_PIE_API_NO_TARGET;
	case HSAN_PIE_API_ABSENT:
		if (!subport)
			return HSAN_PIE_API_NO_TARGET;
		switch (fallback) {
		case HSAN_PIE_API_ATTACHED:
			return HSAN_PIE_API_DEFAULT_TARGET;
		case HSAN_PIE_API_INACTIVE:
		case HSAN_PIE_API_ABSENT:
			return HSAN_PIE_API_NO_TARGET;
		default:
			return HSAN_PIE_API_UNDETERMINED;
		}
	default:
		return HSAN_PIE_API_UNDETERMINED;
	}
}

#endif
