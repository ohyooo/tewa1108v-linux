/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_PFE_PA_PORT_PLAN_H
#define HSAN_PFE_PA_PORT_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 hsan_pa_u32;
typedef u8 hsan_pa_u8;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t hsan_pa_u32;
typedef uint8_t hsan_pa_u8;
#endif

/* These are logical CSR addresses, not kernel pointers or physical addresses.
 * No operation on them is performed by the helpers.
 */
#define HSAN_PFE_PA_PORT_TABLE_ID       0x116U
#define HSAN_PFE_PA_PORT_ROWS           16U
#define HSAN_PFE_PA_PORT_PAYLOAD_BYTES  4U
#define HSAN_PFE_PA_PORT_BOOT_ROWS      14U
#define HSAN_PFE_PA_PORT_TAB_BASE       0x07040000U
#define HSAN_PFE_PA_PORT_TAB_WORDS      4U
#define HSAN_PFE_PA_PORT_TAB_STRIDE     16U

static inline bool hsan_pfe_pa_port_row_plan(hsan_pa_u32 port,
                                             hsan_pa_u32 *logical_base)
{
	if (!logical_base || port >= HSAN_PFE_PA_PORT_ROWS)
		return false;
	*logical_base = HSAN_PFE_PA_PORT_TAB_BASE + port * HSAN_PFE_PA_PORT_TAB_STRIDE;
	return true;
}

/* OEM port_config_read @.text+0x7254: update ONLY output bits 0..1.
 * Word 0 bit16 -> output bit0, word 1 bit12 -> output bit1. The staged
 * words are numerical CSR values in ascending logical address order.
 * No qualification of the sample, semantics of the two fields, lookup
 * activation, packet routing, or hardware owner/ack is inferred.
 */
static inline bool hsan_pfe_pa_port_decode(hsan_pa_u32 port,
				   const hsan_pa_u32 *words, size_t nr_words,
				   hsan_pa_u8 prior, hsan_pa_u8 *result)
{
	if (!result || !words || nr_words != HSAN_PFE_PA_PORT_TAB_WORDS ||
	    port >= HSAN_PFE_PA_PORT_ROWS)
		return false;
	*result = (hsan_pa_u8)((prior & 0xfcU) | ((words[0] >> 16) & 1U) |
				    (((words[1] >> 12) & 1U) << 1));
	return true;
}

#endif
