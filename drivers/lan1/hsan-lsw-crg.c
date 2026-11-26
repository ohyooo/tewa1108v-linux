// SPDX-License-Identifier: GPL-2.0-only
/* OEM hi_clk(hwver=1), hi_crg and hi_kreset semantics recovered from
 * the original kernel's embedded initramfs, not inferred from register names.
 */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

struct hsan_crg_reset {
	struct reset_controller_dev rcdev;
	void __iomem *base;
	u32 bytes;
	spinlock_t *lock;
};

struct hsan_crg {
	void __iomem *lsw;
	void __iomem *peri;
	spinlock_t lock;
	struct hsan_crg_reset resets[2];
};

static ssize_t register_status_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct hsan_crg *crg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "lsw_clk_24=%#x lsw_reset_28=%#x peri_reset_430=%#x\n",
			  readl(crg->lsw + 0x24), readl(crg->lsw + 0x28),
			  readl(crg->peri + 0x430));
}
static DEVICE_ATTR_RO(register_status);

static struct attribute *hsan_crg_attrs[] = {
	&dev_attr_register_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hsan_crg);

static int hsan_crg_xlate(struct reset_controller_dev *rcdev,
			const struct of_phandle_args *spec)
{
	struct hsan_crg_reset *rst = container_of(rcdev, typeof(*rst), rcdev);
	u32 offset = spec->args[0], bit = spec->args[1];

	if (spec->args_count != 2 || bit > 31 || !IS_ALIGNED(offset, 4) ||
	    offset > rst->bytes - sizeof(u32))
		return -EINVAL;
	return (offset << 8) | bit;
}

static int hsan_crg_reset_update(struct reset_controller_dev *rcdev,
				unsigned long id, bool assert)
{
	struct hsan_crg_reset *rst = container_of(rcdev, typeof(*rst), rcdev);
	void __iomem *reg = rst->base + (id >> 8);
	u32 mask = BIT(id & 31), value;
	unsigned long flags;

	spin_lock_irqsave(rst->lock, flags);
	value = readl(reg);
	writel(assert ? value & ~mask : value | mask, reg);
	value = readl(reg);
	spin_unlock_irqrestore(rst->lock, flags);
	return (!!(value & mask) == !assert) ? 0 : -EIO;
}

static int hsan_crg_assert(struct reset_controller_dev *rcdev,
			   unsigned long id)
{
	return hsan_crg_reset_update(rcdev, id, true);
}

static int hsan_crg_deassert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	return hsan_crg_reset_update(rcdev, id, false);
}

static int hsan_crg_status(struct reset_controller_dev *rcdev,
			   unsigned long id)
{
	struct hsan_crg_reset *rst = container_of(rcdev, typeof(*rst), rcdev);

	/* Linux status means asserted; OEM hi_crg_get_status returns enable. */
	return !(readl(rst->base + (id >> 8)) & BIT(id & 31));
}

static const struct reset_control_ops hsan_crg_reset_ops = {
	.assert = hsan_crg_assert,
	.deassert = hsan_crg_deassert,
	.status = hsan_crg_status,
};

static void hsan_crg_put_node(void *data)
{
	of_node_put(data);
}

static void hsan_crg_delete_clock(void *data)
{
	of_clk_del_provider(data);
}

static void hsan_crg_unpublish_reset(void *data)
{
	struct device_node *node = data;

	fwnode_dev_initialized(of_fwnode_handle(node), false);
}

static int hsan_crg_add_gate(struct device *dev, struct hsan_crg *crg,
			    struct device_node *node)
{
	const char *name, *parent;
	struct clk_hw *hw;
	u32 offset, bit;
	int ret;

	ret = of_property_read_string(node, "clock-output-names", &name);
	if (ret)
		return ret;
	if (of_property_read_u32(node, "reg-offset", &offset) ||
	    of_property_read_u32(node, "bit", &bit) ||
	    offset != 0x24 || (bit != 1 && bit != 2))
		return -EINVAL;
	parent = of_clk_get_parent_name(node, 0);
	if (!parent)
		return -EINVAL;
	of_node_get(node);
	ret = devm_add_action_or_reset(dev, hsan_crg_put_node, node);
	if (ret)
		return ret;
	hw = devm_clk_hw_register_gate(dev, name, parent, CLK_IGNORE_UNUSED,
				      crg->lsw + offset, bit, 0, &crg->lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	ret = of_clk_add_hw_provider(node, of_clk_hw_simple_get, hw);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, hsan_crg_delete_clock, node);
}

static int hsan_crg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct hsan_crg *crg;
	struct resource *res;
	u32 bank, hwver;
	int i, ret;

	if (of_property_read_u32(dev->of_node, "hwver", &hwver) || hwver != 1)
		return -EINVAL;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lsw");
	if (!res || res->start != 0x14980000 || resource_size(res) != 0x100)
		return -EINVAL;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "peri");
	if (!res || res->start != 0x10108000 || resource_size(res) != 0x1000)
		return -EINVAL;
	crg = devm_kzalloc(dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;
	spin_lock_init(&crg->lock);
	crg->lsw = devm_platform_ioremap_resource_byname(pdev, "lsw");
	if (IS_ERR(crg->lsw))
		return PTR_ERR(crg->lsw);
	crg->peri = devm_platform_ioremap_resource_byname(pdev, "peri");
	if (IS_ERR(crg->peri))
		return PTR_ERR(crg->peri);
	for (i = 0; i < ARRAY_SIZE(crg->resets); i++) {
		struct hsan_crg_reset *rst = &crg->resets[i];

		node = of_parse_phandle(dev->of_node, "hsan,reset-providers", i);
		if (!node)
			return -EINVAL;
		ret = devm_add_action_or_reset(dev, hsan_crg_put_node, node);
		if (ret)
			return ret;
		if (of_property_read_u32(node, "value", &bank) ||
		    bank != (i ? 2 : 0))
			return -EINVAL;
		rst->base = i ? crg->lsw : crg->peri;
		rst->bytes = i ? 0x100 : 0x1000;
		rst->lock = &crg->lock;
		rst->rcdev.owner = THIS_MODULE;
		rst->rcdev.dev = dev;
		rst->rcdev.of_node = node;
		rst->rcdev.ops = &hsan_crg_reset_ops;
		rst->rcdev.of_reset_n_cells = 2;
		rst->rcdev.of_xlate = hsan_crg_xlate;
		rst->rcdev.nr_resets = rst->bytes << 8;
		ret = devm_reset_controller_register(dev, &rst->rcdev);
		if (ret)
			return ret;
		/* Like the CCF child-node providers, this functional provider is
		 * registered by its CRG device rather than a child platform driver.
		 */
		fwnode_dev_initialized(of_fwnode_handle(node), true);
		ret = devm_add_action_or_reset(dev, hsan_crg_unpublish_reset, node);
		if (ret)
			return ret;
	}
	for_each_available_child_of_node(dev->of_node, node) {
		ret = hsan_crg_add_gate(dev, crg, node);
		if (ret) {
			of_node_put(node);
			return ret;
		}
	}
	platform_set_drvdata(pdev, crg);
	dev_info(dev, "OEM hwver1 LSW gates and active-low peri/LSW resets registered\n");
	return 0;
}

static const struct of_device_id hsan_crg_match[] = {
	{ .compatible = "hsan,tewa-lsw-crg" },
	{ }
};
MODULE_DEVICE_TABLE(of, hsan_crg_match);

static struct platform_driver hsan_crg_driver = {
	.probe = hsan_crg_probe,
	.driver = {
		.name = "hsan-lsw-crg",
		.of_match_table = hsan_crg_match,
		.dev_groups = hsan_crg_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(hsan_crg_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V verified OEM LSW clock and reset provider");
