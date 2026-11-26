// SPDX-License-Identifier: GPL-2.0-only
/*
 * TEWA-1108V LAN1 (OEM port 10) GEMAC Linux 7.3.5 hardware discovery.
 * The upstream PHY + MDIO and PIE belong to separate devices; this driver
 * must NOT claim a second net_device while hsan-net-v300 owns lan1.
 *
 * Source: original hi_kmac.ko hi_mac_hw_mtu @.text+0x38c,
 * hi_mac_hw_set_pause @.text+0x1bc, hi_mac_hw_config @.text+0x258,
 * hi_mac_net_open @.text+0x3e1c. All original methods call the vendor's
 * indirect read/write CSR hooks. No values are written until link-mode,
 * clock/reset and route sequencing are fully qualified against hardware.
 */
#include <linux/atomic.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/mii.h>
#include <linux/platform_device.h>
#include "hsan-gemac-lan1.h"

#define HSAN_MAC_BASE         0x14400000U
#define HSAN_MAC_LENGTH       0x10000U
#define HSAN_MAC_PORT         10U
#define HSAN_MAC_PHY_ADDR     3U
#define HSAN_MAC_PHY_ID       0x20669a22U
#define HSAN_MAC_MDIO_BASE    0x14600000U
#define HSAN_MAC_MDIO_LENGTH  0x1000U
#define HSAN_MAC_MDIO_HWVER   0x200U
#define HSAN_MAC_CONFIG_REG   0x00000100U
#define HSAN_MAC_MTU_MASK     GENMASK(13, 0)
#define HSAN_MAC_PAUSE_MASK   (BIT(28) | BIT(29))
#define HSAN_MAC_GATE         0x14001020U
#define HSAN_MAC_GATE_BIT     BIT(7)

struct hsan_mac_lan1 {
	void __iomem *regs;
	void __iomem *gate;
	struct device *dev;
    bool link_program_attempted;
    bool link_programmed;
    bool link_hw_uncertain;
    bool link_register_drift_latched;
    u32 link_speed_mbps;
    bool link_full_duplex;
    bool link_rx_pause;
    bool link_tx_pause;
};

/* The board exposes a single OEM LAN1 GEMAC. Protect read-only snapshots
 * against concurrent platform driver unbind and duplicated port owners.
 */
static DEFINE_MUTEX(hsan_mac_owner_lock);
static struct hsan_mac_lan1 *hsan_mac_owner;
/* Binding identity, not a hardware generation. Protected by owner_lock. */
static u64 hsan_mac_owner_generation;
/* OEM hi_mac_hw_config is an irreversible MMIO strobe; retaining the
 * module owner prevents rmmod/reload from resetting its one-shot latch.
 * This is NOT proof that GEMAC DMA and PA are quiescent at driver removal.
 */
static atomic_t hsan_mac_mmio_lifetime_pinned = ATOMIC_INIT(0);
static void hsan_mac_pin_hw_lifetime(void)
{
    if (atomic_cmpxchg(&hsan_mac_mmio_lifetime_pinned, 0, 1) == 0) {
        __module_get(THIS_MODULE);
        pr_warn("hsan-gemac-lan1: OEM MAC MMIO attempted; module pinned until hardware reset\n");
    }
}

int hsan_gemac_lan1_snapshot(struct hsan_gemac_snapshot *snapshot)
{
    struct hsan_mac_lan1 *mac;
    u32 v;
    int ret = 0;

    if (!snapshot)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    mutex_lock(&hsan_mac_owner_lock);
    mac = hsan_mac_owner;
    if (!mac) {
        ret = -ENODEV;
        goto out;
    }
    /* The gate mapping is owned by the GEMAC device and remains live
     * while hsan_mac_owner_lock protects the published snapshot owner.
     */
    v = readl(mac->gate);
    if (!(v & HSAN_MAC_GATE_BIT)) {
        ret = -EHOSTDOWN;
        goto out;
    }
    snapshot->config_csr = readl(mac->regs + HSAN_MAC_CONFIG_REG);
    /* Stage49: a one-time MAC write/readback at ndo_open does not
     * establish the *current* GEMAC mode. Read four persistent OEM
     * register fields on every observed link snapshot. Do not rewrite
     * the MAC or pretend the missing hot renegotiation path is safe.
     */
    if (mac->link_programmed && !mac->link_hw_uncertain) {
        u32 expected_mode = (mac->link_speed_mbps == 1000 ? 2U :
                             (mac->link_speed_mbps == 100 ? 1U : 0U)) |
                            (mac->link_full_duplex ? BIT(2) : 0U);
        u32 expected_pause = (mac->link_rx_pause ? BIT(29) : 0U) |
                             (mac->link_tx_pause ? BIT(28) : 0U);

        if (readl(mac->regs + 0x14) != expected_mode ||
            (readl(mac->regs + 0x40) &
             (BIT(24) | GENMASK(15, 8))) != BIT(24) ||
            (snapshot->config_csr & HSAN_MAC_PAUSE_MASK) != expected_pause ||
            readl(mac->regs + 0x48) != 0) {
            mac->link_hw_uncertain = true;
            mac->link_register_drift_latched = true;
            dev_err(mac->dev,
                    "live GEMAC register mode changed after one-shot OEM init; PIE DMA must be quarantined\n");
        } else {
            snapshot->link_register_readback_valid = true;
        }
    }
    snapshot->link_register_drift_latched = mac->link_register_drift_latched;
    snapshot->port = HSAN_MAC_PORT;
    snapshot->phy_addr = HSAN_MAC_PHY_ADDR;
    snapshot->gate_on = true;
    snapshot->mac_link_programmed = mac->link_programmed;
    snapshot->link_hw_attempted = mac->link_program_attempted;
    snapshot->link_hw_uncertain = mac->link_hw_uncertain;
    snapshot->link_module_pinned = atomic_read(&hsan_mac_mmio_lifetime_pinned);
    snapshot->programmed_speed_mbps = mac->link_speed_mbps;
    snapshot->programmed_full_duplex = mac->link_full_duplex;
    snapshot->programmed_rx_pause = mac->link_rx_pause;
    snapshot->programmed_tx_pause = mac->link_tx_pause;
out:
    mutex_unlock(&hsan_mac_owner_lock);
    return ret;
}
/* hi_kmac.ko hi_mac_hw_get_stats64 @.text+0x914 uses four low/high
 * register pairs, in the OEM-return order 0x400, 0x4d0, 0x4c0, 0x588.
 * The original reads low then high, with no guaranteed atomic latch.
 * Never present these words as proved GEMAC TX/RX bytes or proof of STOP.
 * Opt-in strictly read-only diagnostics are serialized with owner removal.
 */
int hsan_gemac_lan1_oem_counters(struct hsan_gemac_oem_counters *out)
{
    static const u16 lo_reg[HSAN_GEMAC_OEM_COUNTER_PAIRS] = {
        0x400, 0x4d0, 0x4c0, 0x588,
    };
    struct hsan_mac_lan1 *mac;
    unsigned int i;
    int ret = 0;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    mutex_lock(&hsan_mac_owner_lock);
    mac = hsan_mac_owner;
    if (!mac) {
        ret = -ENODEV;
        goto out_unlock;
    }
    out->requested = of_property_read_bool(mac->dev->of_node,
                           "hsan,experimental-read-gemac-oem-counters");
    if (!out->requested)
        goto out_unlock;
    if (!(readl(mac->gate) & HSAN_MAC_GATE_BIT)) {
        ret = -EHOSTDOWN;
        goto out_unlock;
    }
    out->sample_begin_ns = ktime_get_ns();
    for (i = 0; i < ARRAY_SIZE(lo_reg); i++) {
        u32 low = readl(mac->regs + lo_reg[i]);
        u32 high = readl(mac->regs + lo_reg[i] + sizeof(u32));

        out->raw[i] = ((u64)high << 32) | low;
    }
    out->sample_end_ns = ktime_get_ns();
    out->valid = true;
out_unlock:
    mutex_unlock(&hsan_mac_owner_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_gemac_lan1_oem_counters);

EXPORT_SYMBOL_GPL(hsan_gemac_lan1_snapshot);

/* MDIO1 also has a child at address 3, but on a different bus and
 * Clause 45. This reads OF metadata only, not provider readiness or
 * hardware. Return a referenced, board-matched Clause 22 PHY node.
 */
static struct device_node *hsan_gemac_lan1_phy_node(struct device_node *mac)
{
    struct device_node *phy, *bus;
    struct resource res;
    u32 addr, hwver;
    int ret;

    phy = of_parse_phandle(mac, "phy-handle", 0);
    if (!phy)
        return ERR_PTR(-ENODEV);
    if (!of_device_is_available(phy)) {
        ret = -ENODEV;
        goto put_phy;
    }
    if (!of_device_is_compatible(phy, "ethernet-phy-ieee802.3-c22") ||
        of_device_is_compatible(phy, "ethernet-phy-ieee802.3-c45")) {
        ret = -EINVAL;
        goto put_phy;
    }
    ret = of_property_read_u32(phy, "reg", &addr);
    if (ret)
        goto put_phy;
    if (addr != HSAN_MAC_PHY_ADDR) {
        ret = -EINVAL;
        goto put_phy;
    }
    bus = of_get_parent(phy);
    if (!bus) {
        ret = -EINVAL;
        goto put_phy;
    }
    if (!of_device_is_available(bus)) {
        ret = -ENODEV;
        goto put_bus;
    }
    if (!of_device_is_compatible(bus, "hsan,mdio")) {
        ret = -EINVAL;
        goto put_bus;
    }
    ret = of_property_read_u32(bus, "hwver", &hwver);
    if (ret)
        goto put_bus;
    if (hwver != HSAN_MAC_MDIO_HWVER) {
        ret = -EINVAL;
        goto put_bus;
    }
    ret = of_address_to_resource(bus, 0, &res);
    if (ret)
        goto put_bus;
    if (resource_type(&res) != IORESOURCE_MEM ||
        res.start != HSAN_MAC_MDIO_BASE || res.end < res.start ||
        resource_size(&res) != HSAN_MAC_MDIO_LENGTH)
        ret = -EINVAL;

put_bus:
    of_node_put(bus);
    if (!ret)
        return phy;
put_phy:
    of_node_put(phy);
    return ERR_PTR(ret);
}

/* Defer only for a board-matched node awaiting registration. */
static struct phy_device *hsan_gemac_lan1_find_phy(struct device_node *mac)
{
    struct device_node *np = hsan_gemac_lan1_phy_node(mac);
    struct phy_device *phy;

    if (IS_ERR(np))
        return ERR_CAST(np);
    phy = of_phy_find_device(np);
    of_node_put(np);
    if (!phy)
        return ERR_PTR(-EPROBE_DEFER);
    if (phy->is_c45 || phy->mdio.addr != HSAN_MAC_PHY_ADDR ||
        phy->phy_id != HSAN_MAC_PHY_ID) {
        put_device(&phy->mdio.dev);
        return ERR_PTR(-ENODEV);
    }
    return phy;
}

/* No PHY state machine, MAC write or phylink policy is inferred. The PHY
 * device is reference-counted while Clause 22 link state is sampled twice
 * (BMSR latch-low). MAC lifetime is protected by the owner mutex.
 */
static int hsan_gemac_lan1_link_sample(struct hsan_gemac_link_snapshot *snapshot,
                                      u64 *generation)
{
	struct phy_device *phy;
	int bmcr, bmsr, first_bmsr, ret = 0;
    int adv, lpa, adv1000, lpa1000;
    unsigned int common, mode;
    u8 flow;

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	mutex_lock(&hsan_mac_owner_lock);
	if (!hsan_mac_owner) {
		ret = -ENODEV;
		goto out;
	}
	if (!(readl(hsan_mac_owner->gate) & HSAN_MAC_GATE_BIT)) {
		ret = -EHOSTDOWN;
		goto out;
	}
	phy = hsan_gemac_lan1_find_phy(hsan_mac_owner->dev->of_node);
	if (IS_ERR(phy)) {
		ret = PTR_ERR(phy);
		goto out;
	}
    /* The latch-low read is data on success, not the function status. */
    first_bmsr = phy_read(phy, MII_BMSR);
    if (first_bmsr < 0 || first_bmsr == 0xffff) {
        ret = first_bmsr < 0 ? first_bmsr : -ENODATA;
        goto put_phy;
    }
	bmsr = phy_read(phy, MII_BMSR);
	bmcr = phy_read(phy, MII_BMCR);
	if (bmsr < 0 || bmcr < 0) {
		ret = bmsr < 0 ? bmsr : bmcr;
		goto put_phy;
	}
    if (bmsr == 0xffff || bmcr == 0xffff) {
        ret = -ENODATA;
        goto put_phy;
    }
	snapshot->phy_id = phy->phy_id;
	snapshot->bmsr = bmsr;
	snapshot->bmcr = bmcr;
	snapshot->link_up = !!(bmsr & BMSR_LSTATUS);
	snapshot->aneg_enabled = !!(bmcr & BMCR_ANENABLE);
	snapshot->aneg_complete = !!(bmsr & BMSR_ANEGCOMPLETE);
    /* phylib normally owns this negotiation state. The board MAC has
     * not been granted a speed/duplex write sequence yet; report only a
     * live, standard Clause-22 result, never arm the GEMAC from it.
     */
    if (!snapshot->link_up)
        goto put_phy;
    if (!snapshot->aneg_enabled) {
        if ((bmcr & BMCR_SPEED1000) && (bmcr & BMCR_SPEED100)) {
            ret = -EINVAL;
            goto put_phy;
        }
        snapshot->speed_mbps = (bmcr & BMCR_SPEED1000) ? 1000 :
                               ((bmcr & BMCR_SPEED100) ? 100 : 10);
        snapshot->full_duplex = !!(bmcr & BMCR_FULLDPLX);
        snapshot->resolved = true;
        goto put_phy;
    }
    if (!snapshot->aneg_complete)
        goto put_phy;
    adv = phy_read(phy, MII_ADVERTISE);
    lpa = phy_read(phy, MII_LPA);
    adv1000 = phy_read(phy, MII_CTRL1000);
    lpa1000 = phy_read(phy, MII_STAT1000);
    if (adv < 0 || lpa < 0 || adv1000 < 0 || lpa1000 < 0) {
        ret = -EIO;
        goto put_phy;
    }
    if ((adv == 0xffff) || (lpa == 0xffff) ||
        (adv1000 == 0xffff) || (lpa1000 == 0xffff)) {
        ret = -ENODATA;
        goto put_phy;
    }
    /* Linux PHYLIB treats a reported 1000BASE-T master/slave
     * resolution failure as no usable negotiated link.
     */
    if (lpa1000 & LPA_1000MSFAIL) {
        ret = -ENOLINK;
        goto put_phy;
    }
    if ((adv1000 & ADVERTISE_1000FULL) &&
        (lpa1000 & LPA_1000FULL)) {
        snapshot->speed_mbps = 1000;
        snapshot->full_duplex = true;
    } else if ((adv1000 & ADVERTISE_1000HALF) &&
               (lpa1000 & LPA_1000HALF)) {
        snapshot->speed_mbps = 1000;
    } else {
        common = adv & lpa & ADVERTISE_ALL;
        if (!common) {
            ret = -ENOLINK;
            goto put_phy;
        }
        mode = mii_nway_result(common);
        snapshot->speed_mbps = (mode & LPA_100) ? 100 : 10;
        snapshot->full_duplex = !!(mode & LPA_DUPLEX);
    }
    if (snapshot->full_duplex) {
        flow = mii_resolve_flowctrl_fdx(adv, lpa);
        snapshot->rx_pause = !!(flow & FLOW_CTRL_RX);
        snapshot->tx_pause = !!(flow & FLOW_CTRL_TX);
    }
    snapshot->resolved = true;
put_phy:
	put_device(&phy->mdio.dev);
out:
	if (!ret && generation)
		*generation = hsan_mac_owner_generation;
	mutex_unlock(&hsan_mac_owner_lock);
	return ret;
}
int hsan_gemac_lan1_link_snapshot(struct hsan_gemac_link_snapshot *snapshot)
{
    return hsan_gemac_lan1_link_sample(snapshot, NULL);
}
EXPORT_SYMBOL_GPL(hsan_gemac_lan1_link_snapshot);

int hsan_gemac_lan1_get_phy(struct phy_device **out)
{
    struct phy_device *phy;
    int ret = 0;

    if (!out)
        return -EINVAL;
    *out = NULL;
    mutex_lock(&hsan_mac_owner_lock);
    if (!hsan_mac_owner) {
        ret = -ENODEV;
        goto unlock;
    }
    phy = hsan_gemac_lan1_find_phy(hsan_mac_owner->dev->of_node);
    if (IS_ERR(phy)) {
        ret = PTR_ERR(phy);
        goto unlock;
    }
    *out = phy;
unlock:
    mutex_unlock(&hsan_mac_owner_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_gemac_lan1_get_phy);

/* OEM hi_kmac.ko hi_mac_hw_config (.text+0x258): the indirect
 * register hooks write +0x18=1, +0x14=(speed code | duplex<<2),
 * +0x18=0, +0x1c=1 and +0x40=(old & ~(BIT24 | 0xff00)) | BIT24.
 * hi_mac_hw_set_pause (.text+0x1bc) writes +0x100 bits 29/28.
 *
 * All registers are inside the verified GEMAC MMIO resource. Programming
 * is an explicitly opted-in, ONE-SHOT act: there is no demonstrated
 * 5.10->7.3 hot renegotiation/reset or PHY power sequencing yet.
 * This is not a replacement for a fully integrated phylink state machine.
 */
int hsan_gemac_lan1_program_link(void)
{
    struct hsan_gemac_link_snapshot link;
    struct hsan_mac_lan1 *mac;
    u32 reg40, reg100, mode, check, expected40, silent_before;
    u64 generation;
    int ret;

    /* First sample the Clause22 state without holding a second MAC lock.
     * The snapshot accessor protects its own device lifetime.
     */
    ret = hsan_gemac_lan1_link_sample(&link, &generation);
    if (ret)
        return ret;
    if (!link.link_up || !link.resolved ||
        (link.aneg_enabled && !link.aneg_complete))
        return -ENOLINK;
    if (link.speed_mbps != 1000 && link.speed_mbps != 100 &&
        link.speed_mbps != 10)
        return -EINVAL;
    mutex_lock(&hsan_mac_owner_lock);
    mac = hsan_mac_owner;
    /* A PHY sample from a previous binding must not justify writes. */
    if (!mac || hsan_mac_owner_generation != generation) {
        ret = -ENODEV;
        goto out;
    }
    if (!of_property_read_bool(mac->dev->of_node,
                  "hsan,experimental-program-oem-gemac-link")) {
        ret = -EACCES;
        goto out;
    }
    if (mac->link_program_attempted) {
        ret = -EALREADY;
        goto out;
    }
    if (!(readl(mac->gate) & HSAN_MAC_GATE_BIT)) {
        ret = -EHOSTDOWN;
        goto out;
    }
    reg100 = readl(mac->regs + HSAN_MAC_CONFIG_REG);
    if ((reg100 & HSAN_MAC_MTU_MASK) != HSAN_GEMAC_LAN1_FRAME_LIMIT) {
        ret = -EMSGSIZE;
        goto out;
    }
    reg40 = readl(mac->regs + 0x40);
    silent_before = readl(mac->regs + 0x48);
    mode = (link.speed_mbps == 1000 ? 2U :
           (link.speed_mbps == 100 ? 1U : 0U));
    if (link.full_duplex)
        mode |= BIT(2);
    /* Latch BEFORE first irreversible write, including any failed
     * MMIO readback; do not auto-replay across a failed probe.
     */
    mac->link_program_attempted = true;
    mac->link_hw_uncertain = true;
    hsan_mac_pin_hw_lifetime();
    writel(1, mac->regs + 0x18);
    writel(mode, mac->regs + 0x14);
    writel(0, mac->regs + 0x18);
    writel(1, mac->regs + 0x1c);
    expected40 = (reg40 & ~(BIT(24) | GENMASK(15, 8))) | BIT(24);
    writel(expected40, mac->regs + 0x40);
    /* OEM hi_mac_link_up @0x44ec calls ops+0x44 with silent=0.
     * hi_mac_set_silent @0x54 masks its bool and performs a full write
     * through hi_reg_hw_write to +0x48. This precedes pause setup and
     * is not equivalent to speed/duplex configuration alone.
     */
    writel(0, mac->regs + 0x48);
    reg100 &= ~HSAN_MAC_PAUSE_MASK;
    if (link.rx_pause)
        reg100 |= BIT(29);
    if (link.tx_pause)
        reg100 |= BIT(28);
    writel(reg100, mac->regs + HSAN_MAC_CONFIG_REG);
    check = readl(mac->regs + HSAN_MAC_CONFIG_REG);
    /* +0x18 and +0x1c are command/strobe registers in the OEM
     * write sequence; their steady-state readback is unqualified.
     * Only verify the independently established persistent fields.
     */
    if (readl(mac->regs + 0x48) != 0 ||
        readl(mac->regs + 0x14) != mode ||
        (readl(mac->regs + 0x40) & (BIT(24) | GENMASK(15, 8))) !=
        (expected40 & (BIT(24) | GENMASK(15, 8))) ||
        (check & (HSAN_MAC_MTU_MASK | HSAN_MAC_PAUSE_MASK)) !=
        (reg100 & (HSAN_MAC_MTU_MASK | HSAN_MAC_PAUSE_MASK))) {
        dev_err(mac->dev, "OEM GEMAC MAC configuration readback mismatch (%08x/%08x), retry forbidden\n",
                check, reg100);
        ret = -EUCLEAN;
        goto out;
    }
    dev_info(mac->dev, "OEM GEMAC link-up silent register: before=%#x after=%#x\n",
             silent_before, readl(mac->regs + 0x48));
    mac->link_speed_mbps = link.speed_mbps;
    mac->link_full_duplex = link.full_duplex;
    mac->link_rx_pause = link.rx_pause;
    mac->link_tx_pause = link.tx_pause;
    /* Do NOT publish link_programmed until a second, independently
     * sampled PHY state agrees. Otherwise PHYLIB can briefly bring
     * carrier up between register readback and the second MDIO sample.
     */
    ret = 0;
out:
    mutex_unlock(&hsan_mac_owner_lock);
    if (!ret) {
        struct hsan_gemac_link_snapshot verify;
        struct hsan_mac_lan1 *current_mac;
        u64 verified_generation;
        int sampled = hsan_gemac_lan1_link_sample(&verify, &verified_generation);

        if (sampled)
            return sampled;
        if (verified_generation != generation)
            return -ENODEV;
        if (!verify.link_up || !verify.resolved ||
            verify.speed_mbps != link.speed_mbps ||
            verify.full_duplex != link.full_duplex ||
            verify.rx_pause != link.rx_pause ||
            verify.tx_pause != link.tx_pause)
            return -EAGAIN;

        /* Reacquire the owner and reject replacement, including a devm
         * allocation reused at exactly the same address.
         */
        mutex_lock(&hsan_mac_owner_lock);
        current_mac = hsan_mac_owner;
        if (!current_mac || hsan_mac_owner_generation != generation)
            ret = -ENODEV;
        else {
            current_mac->link_hw_uncertain = false;
            current_mac->link_programmed = true;
            dev_warn(current_mac->dev, "OEM GEMAC link configured: %uMbps %s-duplex, pause RX=%d TX=%d; dynamic renegotiation still unqualified\n",
                     link.speed_mbps, link.full_duplex ? "full" : "half",
                     link.rx_pause, link.tx_pause);
        }
        mutex_unlock(&hsan_mac_owner_lock);
    }
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_gemac_lan1_program_link);

/* Pure bit helpers for deterministic host-side OEM machine comparisons.
 * These helpers are never applied to hardware by this discovery driver.
 */
static u32 __maybe_unused hsan_mac_mtu_reg(u32 previous, u32 frame_without_fcs)
{
	return (previous & ~HSAN_MAC_MTU_MASK) |
	       ((frame_without_fcs + ETH_FCS_LEN) & HSAN_MAC_MTU_MASK);
}
static u32 __maybe_unused hsan_mac_pause_reg(u32 previous,
					     bool rx_pause, bool tx_pause)
{
	return (previous & ~HSAN_MAC_PAUSE_MASK) |
	       ((u32)rx_pause << 29) | ((u32)tx_pause << 28);
}

static int hsan_mac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hsan_mac_lan1 *mac;
	struct resource *res;
	struct device_node *phy;
	const char *interface;
	u32 port, mask, v;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != HSAN_MAC_BASE ||
	    resource_size(res) != HSAN_MAC_LENGTH)
		return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "port", &port);
	if (ret)
		return ret;
	if (port != HSAN_MAC_PORT)
		return -EINVAL;
	ret = of_property_read_string(dev->of_node, "phy-mode", &interface);
	if (ret)
		return ret;
	if (strcmp(interface, "gmii"))
		return -EINVAL;
	phy = hsan_gemac_lan1_phy_node(dev->of_node);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	of_node_put(phy);
	mac = devm_kzalloc(dev, sizeof(*mac), GFP_KERNEL);
	if (!mac)
		return -ENOMEM;
	mac->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mac->regs))
		return PTR_ERR(mac->regs);
	mac->gate = devm_ioremap(dev, HSAN_MAC_GATE, sizeof(u32));
	if (!mac->gate)
		return -ENOMEM;
	mac->dev = dev;
	mutex_lock(&hsan_mac_owner_lock);
	if (hsan_mac_owner) {
		mutex_unlock(&hsan_mac_owner_lock);
		return -EBUSY;
	}
	if (unlikely(hsan_mac_owner_generation == U64_MAX)) {
		mutex_unlock(&hsan_mac_owner_lock);
		return -EOVERFLOW;
	}
	hsan_mac_owner_generation++;
	hsan_mac_owner = mac;
	mutex_unlock(&hsan_mac_owner_lock);
	platform_set_drvdata(pdev, mac);

	/* Read the independent GEMAC clock gate BEFORE reading any GEMAC CSR.
	 * Never infer physical gate state from experimental clock-provider DT.
	 */
	mask = readl(mac->gate);
	if (!(mask & HSAN_MAC_GATE_BIT)) {
		dev_warn(dev, "LAN1 gate off (%08x): GEMAC CSR read avoided\n", mask);
		return 0;
	}
	v = readl(mac->regs + HSAN_MAC_CONFIG_REG);
	dev_info(dev,
		 "LAN1 OEM port%u GMII, GEMAC reg0x100=%08x mtu-field=%u pause-rx=%u pause-tx=%u; no register writes\n",
		 port, v, (u32)(v & HSAN_MAC_MTU_MASK),
		 !!(v & BIT(29)), !!(v & BIT(28)));
	return 0;
}
static void hsan_mac_remove(struct platform_device *pdev)
{
	struct hsan_mac_lan1 *mac = platform_get_drvdata(pdev);

	mutex_lock(&hsan_mac_owner_lock);
	if (hsan_mac_owner == mac)
		hsan_mac_owner = NULL;
	mutex_unlock(&hsan_mac_owner_lock);
	platform_set_drvdata(pdev, NULL);
}
/* Suspend after an OEM MAC strobe could drop its clock while stale
 * PHY/PIE DMA state still references this configuration. No qualified
 * resume replay exists, so reject deep sleep until board reset.
 */
static int hsan_mac_pm_suspend(struct device *dev)
{
    (void)dev;
    return atomic_read(&hsan_mac_mmio_lifetime_pinned) ? -EBUSY : 0;
}

static const struct dev_pm_ops hsan_mac_pm_ops = {
    .suspend = hsan_mac_pm_suspend,
    .freeze = hsan_mac_pm_suspend,
    .poweroff = hsan_mac_pm_suspend,
};

static const struct of_device_id hsan_mac_match[] = {
	{ .compatible = "hsan,mac" }, {}
};
MODULE_DEVICE_TABLE(of, hsan_mac_match);
static struct platform_driver hsan_mac_driver = {
	.probe = hsan_mac_probe, .remove = hsan_mac_remove,
    .driver = {.name = "hsan-gemac-lan1", .of_match_table = hsan_mac_match,
               .suppress_bind_attrs = true, .pm = &hsan_mac_pm_ops},
};
module_platform_driver(hsan_mac_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V Stage49 GEMAC live mode readback and irreversible drift latch");
