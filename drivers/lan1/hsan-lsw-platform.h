/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HSAN_LSW_PLATFORM_H
#define HSAN_LSW_PLATFORM_H
#include <linux/platform_device.h>
#include <linux/types.h>

struct hsan_lsw_window {
	u8 bank;
	const char *name;
	resource_size_t phys;
	resource_size_t bytes;
};
struct hsan_lsw_mapping {
	struct hsan_lsw_window const *windows;
	unsigned int nr_windows;
};
int hsan_lsw_map_windows(struct platform_device *pdev,
			 const struct hsan_lsw_mapping *cfg);
void hsan_lsw_unmap_windows(struct platform_device *pdev,
			   const struct hsan_lsw_mapping *cfg);
#endif
