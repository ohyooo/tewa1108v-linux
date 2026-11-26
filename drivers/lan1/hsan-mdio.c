// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon HSAN MDIO (Tiangong0): Linux 7.3.5 clean-room driver.
 * Register mapping reconstructed from vendor hi_kmdio.ko (ARMv7 5.10).
 * Tested here by source/binary audit and AArch64 build, NOT yet on hardware.
 * Only Clause 22 is supported. Clause 45 requires separate qualification.
 * This bus performs MMIO commands, but contains no DMA, PHY reset, or NAND I/O.
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/slab.h>

#define HSAN_MDIO_MODE          0x00
#define HSAN_MDIO_STATUS        0x20
#define HSAN_MDIO_BUSY          0x2c
#define HSAN_MDIO_COMMAND       0x30
#define HSAN_MDIO_READ_DATA     0x38
#define HSAN_MDIO_BUSY_MASK     GENMASK(18, 16)
#define HSAN_MDIO_FINISHED      (BIT(0) | BIT(1))
#define HSAN_MDIO_C22_READ      0x80000000U
#define HSAN_MDIO_C22_WRITE     0xc0000000U
#define HSAN_MDIO_TIMEOUT_US    10000
#define HSAN_MDIO_PHY3_ADDR     3
#define HSAN_MDIO_PHY3_ID       0x20669a22U

struct hsan_mdio {
	void __iomem *base;
	struct mii_bus *bus;
};

static int hsan_mdio_wait_idle(struct hsan_mdio *priv)
{
	u32 value;

	return readl_poll_timeout(priv->base + HSAN_MDIO_BUSY, value,
				 !(value & HSAN_MDIO_BUSY_MASK), 1,
				 HSAN_MDIO_TIMEOUT_US);
}

/* Pure local C22 value construction. The caller retains all bus ordering. */
static int hsan_mdio_c22_command_value(int phy_id, int regnum, u16 data,
                                        bool write, u32 *command)
{
	if ((unsigned int)phy_id > 31 || (unsigned int)regnum > 31)
		return -EINVAL;

	/* OEM C22 read/write helpers use distinct opcodes. Read ignores data. */
	*command = (write ? HSAN_MDIO_C22_WRITE : HSAN_MDIO_C22_READ) |
		   ((u32)regnum << 24) | ((u32)phy_id << 16) |
		   (write ? data : 0);
	return 0;
}

static int hsan_mdio_command(struct mii_bus *bus, int phy_id, int regnum,
			     u16 data, bool write)
{
	struct hsan_mdio *priv = bus->priv;
	u32 status, command;
	int ret;

	ret = hsan_mdio_c22_command_value(phy_id, regnum, data, write, &command);
	if (ret)
		return ret;

	ret = hsan_mdio_wait_idle(priv);
	if (ret) {
		dev_warn_ratelimited(bus->parent,
			"MDIO C22 pre-command busy timeout: phy=%d reg=%d error=%d\n",
			phy_id, regnum, ret);
		return ret;
	}

	/* OEM STATUS is write-one-to-ack. Clear a previous completed
	 * transaction BEFORE submitting a new one; otherwise stale status
	 * could make the next read return the last command's data.
	 */
	status = readl(priv->base + HSAN_MDIO_STATUS);
	if (status & HSAN_MDIO_FINISHED) {
        writel(status, priv->base + HSAN_MDIO_STATUS);
		/* A posted acknowledge must reach the bus before CMD. */
		status = readl(priv->base + HSAN_MDIO_STATUS);
		if (status & HSAN_MDIO_FINISHED) {
			dev_warn_ratelimited(bus->parent,
				"MDIO C22 stale completion status did not clear: %#x\n", status);
			return -EBUSY;
		}
	}

	/* OEM C22 helpers always select mode zero before command. */
	writel(0, priv->base + HSAN_MDIO_MODE);
	writel(command, priv->base + HSAN_MDIO_COMMAND);

	ret = readl_poll_timeout(priv->base + HSAN_MDIO_STATUS, status,
				 status & HSAN_MDIO_FINISHED, 1,
				 HSAN_MDIO_TIMEOUT_US);
	if (ret) {
		dev_warn_ratelimited(bus->parent,
			"MDIO C22 completion timeout: phy=%d reg=%d write=%d status=%#x\n",
			phy_id, regnum, write, status);
		return ret;
	}

	/* OEM acknowledges by writing status back, never a blind all-ones. */
    writel(status, priv->base + HSAN_MDIO_STATUS);
    status = readl(priv->base + HSAN_MDIO_STATUS);
    if (status & HSAN_MDIO_FINISHED) {
        dev_warn_ratelimited(bus->parent,
             "MDIO C22 completion ACK failed: phy=%d reg=%d status=%#x\n",
             phy_id, regnum, status);
        return -EUCLEAN;
    }
	/* Complete both read and write transactions only after idle;
	 * a command-status bit alone does not prove the bus is free.
	 */
	ret = hsan_mdio_wait_idle(priv);
	if (ret) {
		dev_warn_ratelimited(bus->parent,
			"MDIO C22 post-command busy timeout: phy=%d reg=%d error=%d\n",
			phy_id, regnum, ret);
		return ret;
	}
	if (write)
		return 0;
	return readl(priv->base + HSAN_MDIO_READ_DATA) & 0xffff;
}

static int hsan_mdio_read(struct mii_bus *bus, int phy_id, int regnum)
{
	return hsan_mdio_command(bus, phy_id, regnum, 0, false);
}

static int hsan_mdio_write(struct mii_bus *bus, int phy_id, int regnum, u16 val)
{
	return hsan_mdio_command(bus, phy_id, regnum, val, true);
}

/* Probe metadata only; resource_size() assumes an ordered interval. */
static bool hsan_mdio_resource_covers_registers(const struct resource *res)
{
	return res && res->end >= res->start &&
	       resource_size(res) >= HSAN_MDIO_READ_DATA + sizeof(u32);
}

static int hsan_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hsan_mdio *priv;
	struct mii_bus *bus;
	struct phy_device *phydev;
    struct resource *res;
	resource_size_t bus_start;
	u32 hwver;
	int bmsr_first, bmsr_second, phyid1, phyid2;
	int ret;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!hsan_mdio_resource_covers_registers(res)) {
        return dev_err_probe(dev, -EINVAL,
                 "MDIO MMIO window does not cover register +0x38\n");
    }
	/* ioremap retains the byte offset, and every CSR access is u32. */
	if (!IS_ALIGNED(res->start, sizeof(u32)))
		return dev_err_probe(dev, -EINVAL,
			     "MDIO MMIO base must be 32-bit aligned\n");
	bus_start = res->start;
	ret = of_property_read_u32(dev->of_node, "hwver", &hwver);
	if (ret || hwver != 0x200)
		return dev_err_probe(dev, -EINVAL,
				     "expected OEM MDIO hwver=0x200\n");

	bus = devm_mdiobus_alloc_size(dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;
	priv = bus->priv;
	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	bus->name = "HSAN MDIO C22 (vendor decoded)";
	bus->read = hsan_mdio_read;
	bus->write = hsan_mdio_write;
	bus->parent = dev;
	snprintf(bus->id, MII_BUS_ID_SIZE, "hsan-%08llx",
		 (unsigned long long)bus_start);
	priv->bus = bus;

	/* No CRG clock/reset writes: test only on a bootloader-initialized bus. */
	ret = of_mdiobus_register(bus, dev->of_node);
	if (ret)
		return dev_err_probe(dev, ret, "mdiobus registration failed\n");

	platform_set_drvdata(pdev, bus);
	phydev = mdiobus_get_phy(bus, HSAN_MDIO_PHY3_ADDR);
	if (phydev) {
		if (phydev->phy_id != HSAN_MDIO_PHY3_ID)
			dev_warn(dev, "PHY3 ID differs from observed OEM device: %#08x (expected %#08x)\n",
				 phydev->phy_id, HSAN_MDIO_PHY3_ID);
		dev_info(dev, "HSAN MDIO PHY3 discovered: phy_id=0x%08x\n", phydev->phy_id);
		phyid1 = mdiobus_read(bus, HSAN_MDIO_PHY3_ADDR, MII_PHYSID1);
		phyid2 = mdiobus_read(bus, HSAN_MDIO_PHY3_ADDR, MII_PHYSID2);
		if (phyid1 < 0 || phyid2 < 0 ||
            phyid1 == 0xffff || phyid2 == 0xffff)
			dev_warn(dev, "PHY3 live identity unavailable: %d/%d; PIE preflight remains gated\n",
				 phyid1, phyid2);
		else if (((u32)phyid1 << 16 | (u16)phyid2) != HSAN_MDIO_PHY3_ID)
			dev_warn(dev, "PHY3 live ID mismatch: %#08x (OEM %#08x)\n",
				 ((u32)phyid1 << 16 | (u16)phyid2), HSAN_MDIO_PHY3_ID);
		/* Link bit is latch-low; a second BMSR read reflects current link. */
		bmsr_first = mdiobus_read(bus, 3, MII_BMSR);
		bmsr_second = mdiobus_read(bus, 3, MII_BMSR);
        /* An unmapped/held-high MDIO read often returns 0xffff;
         * interpreting its link and negotiation bits as TRUE would
         * falsely report a connected PHY while the bus is invalid.
         */
		if (bmsr_first >= 0 && bmsr_second >= 0 &&
            bmsr_first != 0xffff && bmsr_second != 0xffff)
			dev_info(dev, "HSAN PHY3 BMSR: first=0x%04x now=0x%04x link=%s aneg_done=%s\n",
				 bmsr_first, bmsr_second,
				 (bmsr_second & BMSR_LSTATUS) ? "yes" : "no",
				 (bmsr_second & BMSR_ANEGCOMPLETE) ? "yes" : "no");
		else
			dev_warn(dev, "HSAN PHY3 BMSR unavailable/invalid: %d/%d\n",
				 bmsr_first, bmsr_second);
	} else
		dev_warn(dev, "HSAN MDIO C22 registered but PHY3 not discovered\n");
	return 0;
}

static void hsan_mdio_remove(struct platform_device *pdev)
{
	struct mii_bus *bus = platform_get_drvdata(pdev);

	mdiobus_unregister(bus);
}

static const struct of_device_id hsan_mdio_of_match[] = {
	{ .compatible = "hsan,mdio" },
	{}
};
MODULE_DEVICE_TABLE(of, hsan_mdio_of_match);

static struct platform_driver hsan_mdio_driver = {
	.probe = hsan_mdio_probe,
	.remove = hsan_mdio_remove,
	.driver = {
		.name = "hsan-mdio-c22",
        .of_match_table = hsan_mdio_of_match,
        .suppress_bind_attrs = true,
	},
};
module_platform_driver(hsan_mdio_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HSAN Stage48 MDIO C22 completion acknowledgement and PHY3 validity guards");
