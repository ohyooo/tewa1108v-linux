// SPDX-License-Identifier: GPL-2.0-only
/* Cross-platform OEM window registration, no reset/clock/MMIO writes. */
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include "hsan-lsw-csr.h"
#include "hsan-lsw-platform.h"

/* Validate the whole OEM window set before mapping ANY registers.
 * Stage25 checked one window at a time, so a late malformed entry could
 * leave a partially registered bank set until the rollback path ran.
 * It is also important not to alias two logical banks onto overlapping
 * physical MMIO windows in any mapping (including a modified FDT).
 */
static int hsan_lsw_preflight_windows(struct platform_device *pdev,
                                       const struct hsan_lsw_mapping *cfg)
{
    unsigned int i, j;

    if (!cfg || !cfg->windows || !cfg->nr_windows || cfg->nr_windows > 256)
        return -EINVAL;
    for (i = 0; i < cfg->nr_windows; ++i) {
        const struct hsan_lsw_window *a = &cfg->windows[i];
        struct resource *res;
        resource_size_t end;

        if (!a->name || !a->name[0] || a->bytes < sizeof(u32) ||
            a->bytes > 0x01000000U ||
            !IS_ALIGNED(a->phys, sizeof(u32)) ||
            !IS_ALIGNED(a->bytes, sizeof(u32)) ||
            check_add_overflow(a->phys, a->bytes - 1, &end))
            return -EINVAL;
        if (hsan_lsw_bank_mapped(a->bank)) {
            dev_err(&pdev->dev, "LSW logical bank %#x already owned\n", a->bank);
            return -EBUSY;
        }
        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, a->name);
        if (!res || res->start != a->phys || resource_size(res) != a->bytes) {
            dev_err(&pdev->dev, "incorrect OEM LSW window %s bank=%#x\n",
                    a->name, a->bank);
            return -EINVAL;
        }
        for (j = 0; j < i; ++j) {
            const struct hsan_lsw_window *b = &cfg->windows[j];
            resource_size_t b_end = b->phys + b->bytes - 1;

            if (a->bank == b->bank || !strcmp(a->name, b->name) ||
                (a->phys <= b_end && b->phys <= end)) {
                dev_err(&pdev->dev,
                        "duplicated/overlapping LSW windows %s and %s\n",
                        b->name, a->name);
                return -EINVAL;
            }
        }
    }
    return 0;
}

int hsan_lsw_map_windows(struct platform_device *pdev,
			 const struct hsan_lsw_mapping *cfg)
{
	struct device *dev = &pdev->dev;
	unsigned int i;
	int ret;

	ret = hsan_lsw_preflight_windows(pdev, cfg);
	if (ret)
		return ret;
	for (i = 0; i < cfg->nr_windows; i++) {
		const struct hsan_lsw_window *w = &cfg->windows[i];
		struct resource *res;
		void __iomem *regs;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, w->name);
		if (!res || res->start != w->phys ||
		    resource_size(res) != w->bytes) {
			dev_err(dev, "bad/missing OEM LSW %s bank %#x\n",
				w->name, w->bank);
			ret = -EINVAL;
			goto rollback;
		}
		regs = devm_ioremap_resource(dev, res);
		if (IS_ERR(regs)) {
			ret = PTR_ERR(regs);
			goto rollback;
		}
		/* The CSR owner now checks physical overlap with ALL previously
		 * published banks, including other DP/PFE modules. A failed
		 * registration aborts and unmaps only our registered banks.
		 */
		ret = hsan_lsw_register_bank(dev, w->bank, regs,
					     res->start, resource_size(res));
		if (ret)
			goto rollback;
		dev_info(dev, "LSW bank %#02x %s registered @%pa size=%pa (read-only boot)\n",
			 w->bank, w->name, &res->start, &w->bytes);
	}
	return 0;
rollback:
	while (i--)
		hsan_lsw_unregister_bank(dev, cfg->windows[i].bank);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_map_windows);
void hsan_lsw_unmap_windows(struct platform_device *pdev,
			   const struct hsan_lsw_mapping *cfg)
{
	unsigned int i;

	for (i = 0; i < cfg->nr_windows; i++)
		hsan_lsw_unregister_bank(&pdev->dev, cfg->windows[i].bank);
}
EXPORT_SYMBOL_GPL(hsan_lsw_unmap_windows);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V Stage28 LSW full-window/bank-owner preflight registrar");
