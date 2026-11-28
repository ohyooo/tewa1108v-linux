// SPDX-License-Identifier: GPL-2.0-only
/*
 * TEWA1108V Linux 7.3.5 LSW CSR registry, recovered from original
 * hi_lsw_pub.ko: hi_lsw_reg_module @0x0, hi_lsw_get_addr @0x8c,
 * hi_lsw_write_tab128 @0x268, hi_lsw_read/write @0x2d4/0x354.
 *
 * No physical register is touched at module insertion.  All write calls
 * are blocked unless the kernel module's boot-time parameter is enabled
 * explicitly; bank mapping never implies a complete routing pipeline.
 */
#include <linux/errno.h>
#include <linux/bitmap.h>
#include <linux/xarray.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/types.h>
#include "hsan-lsw-csr.h"

struct hsan_bank {
	struct device *owner;
	void __iomem *regs;
	resource_size_t start;
	resource_size_t bytes;
};

/* Register/unregister and CSR I/O share a non-sleeping IRQ-safe lock. */
static DEFINE_SPINLOCK(hsan_lsw_lock);
/* Acquired before the bank lock for EQM bank registration/removal and for
 * complete EQM command/FIFO transactions; prevents mid-command unmapping.
 */
static DEFINE_MUTEX(hsan_lsw_eqm_lock);
/* OEM MCC 255-word stream spans PFE banks 0x05 and 0x02. Serialize
 * registration, generic readers and the entire one-shot transaction.
 * MCC is independent of the FAM/EQM and NAPT lock domains.
 */
static DEFINE_MUTEX(hsan_lsw_mcc_lock);
static bool hsan_lsw_mcc_poisoned;
static atomic_t hsan_lsw_mcc_module_pinned = ATOMIC_INIT(0);
static bool hsan_lsw_mcc_bank(u8 bank)
{
    return bank == 0x02 || bank == 0x05;
}
static bool hsan_lsw_mcc_private_span(u32 address, u32 bytes)
{
    static const u32 control[] = {
        0x0500000cU, 0x0200009cU, 0x02000094U,
        0x02000098U, 0x02000090U,
    };
    u64 first = address, last;
    unsigned int i;

    if (!bytes)
        return false;
    last = first + bytes - 1U;
    if (first <= 0x050553fcU && last >= 0x05055004U)
        return true;
    for (i = 0; i < ARRAY_SIZE(control); ++i)
        if (first <= control[i] && last >= control[i])
            return true;
    return false;
}
/* An aligned 128-bit transaction can start BEFORE the FIFO/command
 * register and still overlap it. Checking only the first word was unsafe.
 */
static bool hsan_lsw_eqm_private_span(u32 address, u32 bytes)
{
    u64 first = address;

    return bytes && first <= 0x42000073U &&
           first + bytes - 1U >= 0x42000048U;
}
/* The OEM FAM arm touches banks 0x40, 0x42 and 0x43 as ONE operation.
 * Lock order: fam -> eqm -> bank spinlock. All public per-bank operations
 * on these banks must serialize against the complete arm, including bank
 * register/unregister and the EQM command engine. check_span() is only a
 * non-reserving preflight and deliberately does not acquire this mutex.
 */
static DEFINE_MUTEX(hsan_lsw_fam_lock);
static bool hsan_lsw_fam_poisoned;
/* The FAM init ACK is NOT a DMA STOP ACK. The CPU publication fence
 * survives all driver lifetimes within the boot and is callable from the
 * PIE RX IRQ/quarantine spinlock without sleeping or touching MMIO.
 */
static atomic_t hsan_lsw_fam_cpu_stopped = ATOMIC_INIT(0);
static atomic64_t hsan_lsw_fam_write_rejections = ATOMIC64_INIT(0);
static atomic64_t hsan_lsw_fam_publish_rejections = ATOMIC64_INIT(0);
static atomic64_t hsan_lsw_fam_publish_admissions = ATOMIC64_INIT(0);
static atomic64_t hsan_lsw_fam_reclaim_rejections = ATOMIC64_INIT(0);
static atomic64_t hsan_lsw_fam_reclaim_admissions = ATOMIC64_INIT(0);
static atomic64_t hsan_lsw_fam_stop_invocations = ATOMIC64_INIT(0);
/* Caller holds hsan_lsw_lock. Fencing and each MMIO write share this
 * lock, so no CSR write may be newly admitted AFTER the fence completes.
 * This only serializes CPU writes; posted bus traffic/DMA can remain live.
 */
static bool hsan_lsw_cpu_write_allowed_locked(void)
{
    if (!atomic_read(&hsan_lsw_fam_cpu_stopped))
        return true;
    atomic64_inc(&hsan_lsw_fam_write_rejections);
    return false;
}
static atomic_t hsan_lsw_fam_module_pinned = ATOMIC_INIT(0);
static bool hsan_lsw_fam_bank(u8 bank)
{
    return bank == 0x40 || bank == 0x42 || bank == 0x43;
}
/* The init sequencer owns these ordinary CSRs after its first MMIO.
 * Do not permit later generic callers to mutate them while the FAM may
 * still own a coherent 640KiB buffer. EQM FIFO operations are distinct.
 */
static bool hsan_lsw_fam_private_span(u32 address, u32 bytes)
{
    static const u32 csrs[] = {
        0x40000028U, 0x4000001cU, 0x43000504U, 0x42000388U,
        0x420000a4U, 0x40000200U, 0x43000514U, 0x4300050cU,
        0x43000530U, 0x43000534U, 0x43000510U, 0x40000044U,
        0x43000098U, 0x430000a4U,
    };
    u64 first = address, last;
    unsigned int i;

    if (!bytes || !hsan_lsw_fam_bank(address >> 24))
        return false;
    last = first + bytes - 1;
    for (i = 0; i < ARRAY_SIZE(csrs); i++)
        if (first <= csrs[i] && last >= csrs[i])
            return true;
    return false;
}

/* NAPT 0x03000060 is a stateful command/ACK engine, not a normal CSR.
 * A transaction holds this mutex while also excluding register removal.
 */
static DEFINE_MUTEX(hsan_lsw_napt_lock);
static bool hsan_lsw_napt_poisoned;
static bool hsan_lsw_napt_partition_complete;
static bool hsan_lsw_napt_gro_attempted;
static u32 hsan_lsw_napt_gro_id = U32_MAX;
static bool hsan_lsw_napt_flow_uncertain;
static bool hsan_lsw_napt_gro_removed;
static u32 hsan_lsw_napt_gro_words[19];
struct hsan_managed_gro_flow {
    u32 id;
    u32 words[19];
    bool attempted;
    bool removed;
    bool valid;
};
static struct hsan_managed_gro_flow hsan_managed_gro[4];
static bool hsan_managed_gro_enabled;

static atomic_t hsan_lsw_napt_module_pinned = ATOMIC_INIT(0);
static bool hsan_lsw_napt_private_span(u32 address, u32 bytes)
{
    static const u32 private_csrs[] = {
        0x03000060U, 0x03000068U, 0x03000130U,
        0x03000134U, 0x03000138U, 0x0300013cU,
    };
    u64 first = address, last;
    unsigned int i;

    if (!bytes)
        return false;
    last = first + bytes - 1;
    if ((first >> 24) != 3 || (last >> 24) != 3)
        return false;
    if ((first <= 0x0300012fU && last >= 0x0300008cU) ||
        (first <= 0x03000183U && last >= 0x03000180U) ||
        (first <= 0x03000297U && last >= 0x03000280U) ||
        (first <= 0x030002efU && last >= 0x030002d4U))
        return true;
    for (i = 0; i < ARRAY_SIZE(private_csrs); i++)
        if (first <= private_csrs[i] && last >= private_csrs[i])
            return true;
    return false;
}
static struct hsan_bank hsan_banks[256];
/* IFC command, parameter and table data registers form one indivisible
 * protocol transaction (OEM ifc_dump). The generic CSR functions must
 * never bypass hsan_lsw_ifc_commit(), including a 128-bit row overlapping
 * a protected region from its preceding word. The preflight-only span API
 * remains available to the IFC owner before committing the batch.
 */
static bool hsan_lsw_ifc_private_span(u32 logical_address, u32 bytes)
{
    u64 first = logical_address;
    u64 last;

    if (!bytes)
        return false;
    last = first + bytes - 1;
    if ((first >> 24) != 0 || (last >> 24) != 0)
        return false;
    return (first <= 0x120U && last >= 0x120U) ||
           (first <= 0x154U && last >= 0x138U) ||
           (first < 0x54000U && last >= 0x50000U);
}

static bool experimental_allow_lsw_writes;
module_param_named(experimental_allow_lsw_writes,
		   experimental_allow_lsw_writes, bool, 0400);
MODULE_PARM_DESC(experimental_allow_lsw_writes,
	"Dangerous: allow write to manually registered LSW CSR banks (default off)");

static int hsan_lsw_access_locked(u32 code, struct hsan_bank **bank_out,
				   u32 *byte_offset)
{
	struct hsan_bank *b = &hsan_banks[code >> 24];
	u32 offset = code & 0x00ffffffU;

	if (!b->owner || !b->regs)
		return -ENODEV;
	/* The original checks only offset<len, permitting truncated 32-bit
	 * accesses.  Fix: require alignment and the *entire word* to fit.
	 */
	if ((offset & 3) || b->bytes < sizeof(u32) ||
	    (resource_size_t)offset > b->bytes - sizeof(u32))
		return -ERANGE;
	*bank_out = b;
	*byte_offset = offset;
	return 0;
}

int hsan_lsw_register_bank(struct device *owner, u8 bank,
			  void __iomem *regs, resource_size_t start,
			  resource_size_t bytes)
{
	struct hsan_bank *slot;
	int ret = 0;
	unsigned long flags;
	resource_size_t end;
	unsigned int i;

	if (!owner || !regs || bytes < 4 || bytes > 0x01000000U ||
	    !IS_ALIGNED(start, 4) || !IS_ALIGNED(bytes, 4) ||
	    check_add_overflow(start, bytes - 1, &end))
		return -EINVAL;
	if (hsan_lsw_fam_bank(bank))
		mutex_lock(&hsan_lsw_fam_lock);
	if (bank == 0x42)
		mutex_lock(&hsan_lsw_eqm_lock);
	if (bank == 0x03)
		mutex_lock(&hsan_lsw_napt_lock);
	if (hsan_lsw_mcc_bank(bank))
		mutex_lock(&hsan_lsw_mcc_lock);
	spin_lock_irqsave(&hsan_lsw_lock, flags);
	slot = &hsan_banks[bank];
	if (slot->owner) {
		ret = -EBUSY;
		goto out;
	}
	/* Reject physical aliases across different logical banks and modules. */
	for (i = 0; i < ARRAY_SIZE(hsan_banks); i++) {
		struct hsan_bank *other = &hsan_banks[i];

		if (!other->owner)
			continue;
		if (start <= other->start + other->bytes - 1 &&
		    other->start <= end) {
			ret = -EADDRINUSE;
			goto out;
		}
	}
	slot->owner = owner;
	slot->regs = regs;
	slot->start = start;
	slot->bytes = bytes;
out:
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	if (bank == 0x42)
		mutex_unlock(&hsan_lsw_eqm_lock);
	if (hsan_lsw_fam_bank(bank))
		mutex_unlock(&hsan_lsw_fam_lock);
	if (bank == 0x03)
		mutex_unlock(&hsan_lsw_napt_lock);
	if (hsan_lsw_mcc_bank(bank))
		mutex_unlock(&hsan_lsw_mcc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_register_bank);

void hsan_lsw_unregister_bank(struct device *owner, u8 bank)
{
	struct hsan_bank *slot;
	unsigned long flags;

	if (hsan_lsw_fam_bank(bank))
		mutex_lock(&hsan_lsw_fam_lock);
	if (bank == 0x42)
		mutex_lock(&hsan_lsw_eqm_lock);
	if (bank == 0x03)
		mutex_lock(&hsan_lsw_napt_lock);
	if (hsan_lsw_mcc_bank(bank))
		mutex_lock(&hsan_lsw_mcc_lock);
	spin_lock_irqsave(&hsan_lsw_lock, flags);
	slot = &hsan_banks[bank];
	if (slot->owner == owner) {
		/* A multi-register EQM transaction cannot be in flight here. */
		slot->owner = NULL;
		slot->regs = NULL;
		slot->bytes = 0;
		slot->start = 0;
	}
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	if (bank == 0x42)
		mutex_unlock(&hsan_lsw_eqm_lock);
	if (hsan_lsw_fam_bank(bank))
		mutex_unlock(&hsan_lsw_fam_lock);
	if (bank == 0x03)
		mutex_unlock(&hsan_lsw_napt_lock);
	if (hsan_lsw_mcc_bank(bank))
		mutex_unlock(&hsan_lsw_mcc_lock);
}
EXPORT_SYMBOL_GPL(hsan_lsw_unregister_bank);

bool hsan_lsw_bank_mapped(u8 bank)
{
	bool present;
	unsigned long flags;

	spin_lock_irqsave(&hsan_lsw_lock, flags);
	present = !!hsan_banks[bank].owner;
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	return present;
}
EXPORT_SYMBOL_GPL(hsan_lsw_bank_mapped);

static int hsan_lsw_read_raw(u32 logical_address, u32 *value)
{
	struct hsan_bank *bank;
	u32 offset;
	int ret;
	unsigned long flags;

	if (!value)
		return -EINVAL;
	spin_lock_irqsave(&hsan_lsw_lock, flags);
	ret = hsan_lsw_access_locked(logical_address, &bank, &offset);
	if (!ret)
		*value = readl(bank->regs + offset);
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	return ret;
}

/* EQM's read FIFO pops data. External direct reads must not compete with
 * hsan_lsw_eqm_read_table() even if the caller did not intend a command.
 * All other bank-0x42 reads are serialized with the full transaction; only
 * the internal EQM implementation may call the raw read while owning it.
 */
int hsan_lsw_read(u32 logical_address, u32 *value)
{
    int ret;
    bool eqm = (logical_address >> 24) == 0x42U;
    bool fam = hsan_lsw_fam_bank(logical_address >> 24);
    bool napt = (logical_address >> 24) == 0x03U;
    bool mcc = hsan_lsw_mcc_bank(logical_address >> 24);

    if (logical_address == 0x4200004cU)
        return -EOPNOTSUPP;
    if (fam)
        mutex_lock(&hsan_lsw_fam_lock);
    if (eqm)
        mutex_lock(&hsan_lsw_eqm_lock);
    if (napt)
        mutex_lock(&hsan_lsw_napt_lock);
    if (mcc)
        mutex_lock(&hsan_lsw_mcc_lock);
    ret = hsan_lsw_read_raw(logical_address, value);
    if (mcc)
        mutex_unlock(&hsan_lsw_mcc_lock);
    if (napt)
        mutex_unlock(&hsan_lsw_napt_lock);
    if (eqm)
        mutex_unlock(&hsan_lsw_eqm_lock);
    if (fam)
        mutex_unlock(&hsan_lsw_fam_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_read);

/* Stage78: only the exact owner-bound, one-shot paired transaction may
 * physically change either half of OEM PUB table 0x102 (31 entries).
 * Exclude ordinary CSR writes, RMW, generic tab128 and their preflights.
 * This is the full 16-byte row span, not only its first register word.
 */
static bool hsan_lsw_pub_private_span(u32 address, u32 bytes)
{
    static const u32 start[2] = { 0x0b05b000U, 0x0b05c000U };
    u64 last = (u64)address + bytes - 1U;
    unsigned int i;

    if (!bytes)
        return false;
    if (address < 0x0b05a900U && last >= 0x0b059000U)
        return true;
    for (i = 0; i < ARRAY_SIZE(start); i++)
        if (address < start[i] + 32U * 16U && last >= start[i])
            return true;
    return false;
}

/* Write access is separated so the EQM transaction engine can own its
 * FIFO and command ports without permitting unsynchronised raw writes.
 */
static int hsan_lsw_write_raw(u32 logical_address, u32 value)
{
	struct hsan_bank *bank;
	u32 offset;
	int ret;
	unsigned long flags;

	if (hsan_lsw_pub_private_span(logical_address, 4))
		return -EOPNOTSUPP;
	if (!READ_ONCE(experimental_allow_lsw_writes))
		return -EACCES;
	spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
    } else {
        ret = hsan_lsw_access_locked(logical_address, &bank, &offset);
    }
	if (!ret) {
        writel(value, bank->regs + offset);
        /* Register contents need not be readback-stable. wmb() only
         * orders MMIO publication before the shared CPU STOP lock opens;
         * it is NOT a device acknowledgement or DMA ownership return.
         */
        wmb();
    }
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	return ret;
}
int hsan_lsw_write(u32 logical_address, u32 value)
{
    int ret;

    /* The whole EQM register bank is a shared transaction domain. Other
     * register writes must not race a command, even outside FIFO offsets.
     */
    if (hsan_lsw_ifc_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_napt_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_mcc_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_pub_private_span(logical_address, sizeof(u32)) ||
        logical_address == 0x42000048U ||
        logical_address == 0x4200005cU ||
        logical_address == 0x4200004cU)
        return -EOPNOTSUPP;
    if (hsan_lsw_fam_bank(logical_address >> 24))
        mutex_lock(&hsan_lsw_fam_lock);
    if ((logical_address >> 24) == 0x42)
        mutex_lock(&hsan_lsw_eqm_lock);
    if ((logical_address >> 24) == 0x03)
        mutex_lock(&hsan_lsw_napt_lock);
    if (hsan_lsw_mcc_bank(logical_address >> 24))
        mutex_lock(&hsan_lsw_mcc_lock);
    if (hsan_lsw_fam_poisoned &&
        hsan_lsw_fam_private_span(logical_address, 4))
        ret = -EUCLEAN;
    else
        ret = hsan_lsw_write_raw(logical_address, value);
    if (hsan_lsw_mcc_bank(logical_address >> 24))
        mutex_unlock(&hsan_lsw_mcc_lock);
    if ((logical_address >> 24) == 0x03)
        mutex_unlock(&hsan_lsw_napt_lock);
    if ((logical_address >> 24) == 0x42)
        mutex_unlock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_fam_bank(logical_address >> 24))
        mutex_unlock(&hsan_lsw_fam_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_write);

/* Keep ownership and the value verification within ONE IRQ-safe bank lock.
 * A second writer or platform-driver removal may not interleave between
 * writel() and readl(). The EQM FIFO/command ports are deliberately not
 * exposed through this helper: they require their own transaction lock.
 */
int hsan_lsw_write_verify(u32 logical_address, u32 value)
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 offset;
    int ret;

    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    if (hsan_lsw_ifc_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_napt_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_mcc_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_pub_private_span(logical_address, sizeof(u32)) ||
        logical_address == 0x42000048U ||
        logical_address == 0x4200004cU ||
        logical_address == 0x4200005cU)
        return -EOPNOTSUPP;
    if (hsan_lsw_fam_bank(logical_address >> 24))
        mutex_lock(&hsan_lsw_fam_lock);
    if ((logical_address >> 24) == 0x42)
        mutex_lock(&hsan_lsw_eqm_lock);
    if ((logical_address >> 24) == 0x03)
        mutex_lock(&hsan_lsw_napt_lock);
    if (hsan_lsw_mcc_bank(logical_address >> 24))
        mutex_lock(&hsan_lsw_mcc_lock);
    if (hsan_lsw_fam_poisoned &&
        hsan_lsw_fam_private_span(logical_address, 4)) {
        ret = -EUCLEAN;
        goto unlock_banks;
    }
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
    } else {
        ret = hsan_lsw_access_locked(logical_address, &bank, &offset);
    }
    if (!ret) {
        writel(value, bank->regs + offset);
        if (readl(bank->regs + offset) != value)
            ret = -EUCLEAN;
    }
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
unlock_banks:
    if ((logical_address >> 24) == 0x42)
        mutex_unlock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_fam_bank(logical_address >> 24))
        mutex_unlock(&hsan_lsw_fam_lock);
    if (hsan_lsw_mcc_bank(logical_address >> 24))
        mutex_unlock(&hsan_lsw_mcc_lock);
    if ((logical_address >> 24) == 0x03)
        mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_write_verify);

/* Atomic, resource-owned read/modify/write for audited field changes.
 * Prevent other CSR clients from losing concurrent updates and reject the
 * EQM command/FIFO ports, which require the transaction mutex instead.
 */
/* The readback must be covered by the SAME IRQ-safe bank lock as
 * read/modify/write; a second driver cannot invalidate the verification
 * between the write and its comparison. EQM still owns its outer mutex.
 */
static int hsan_lsw_update_bits_common(u32 logical_address, u32 mask,
                                      u32 value, bool verify)
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 offset, original, updated;
    int ret;

    if (!mask)
        return -EINVAL;
    if (hsan_lsw_ifc_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_napt_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_mcc_private_span(logical_address, sizeof(u32)) ||
        hsan_lsw_pub_private_span(logical_address, sizeof(u32)) ||
        logical_address == 0x42000048U ||
        logical_address == 0x4200004cU ||
        logical_address == 0x4200005cU)
        return -EOPNOTSUPP;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    if (hsan_lsw_fam_bank(logical_address >> 24))
        mutex_lock(&hsan_lsw_fam_lock);
    if ((logical_address >> 24) == 0x42U)
        mutex_lock(&hsan_lsw_eqm_lock);
    if ((logical_address >> 24) == 0x03U)
        mutex_lock(&hsan_lsw_napt_lock);
    if (hsan_lsw_mcc_bank(logical_address >> 24))
        mutex_lock(&hsan_lsw_mcc_lock);
    if (hsan_lsw_fam_poisoned &&
        hsan_lsw_fam_private_span(logical_address, 4)) {
        ret = -EUCLEAN;
        goto unlock_banks;
    }
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
    } else {
        ret = hsan_lsw_access_locked(logical_address, &bank, &offset);
    }
    if (!ret) {
        original = readl(bank->regs + offset);
        updated = (original & ~mask) | (value & mask);
        if (updated != original) {
            writel(updated, bank->regs + offset);
            /* Complete CPU MMIO ordering before releasing the shared
             * FAM STOP publication lock; this is NOT a HW STOP ACK.
             */
            wmb();
        }
        if (verify && ((readl(bank->regs + offset) ^ updated) & mask))
            ret = -EUCLEAN;
    }
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
unlock_banks:
    if ((logical_address >> 24) == 0x42U)
        mutex_unlock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_fam_bank(logical_address >> 24))
        mutex_unlock(&hsan_lsw_fam_lock);
    if (hsan_lsw_mcc_bank(logical_address >> 24))
        mutex_unlock(&hsan_lsw_mcc_lock);
    if ((logical_address >> 24) == 0x03U)
        mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}

int hsan_lsw_update_bits(u32 logical_address, u32 mask, u32 value)
{
    return hsan_lsw_update_bits_common(logical_address, mask, value, false);
}
EXPORT_SYMBOL_GPL(hsan_lsw_update_bits);

int hsan_lsw_update_bits_verify(u32 logical_address, u32 mask, u32 value)
{
    return hsan_lsw_update_bits_common(logical_address, mask, value, true);
}
EXPORT_SYMBOL_GPL(hsan_lsw_update_bits_verify);

int hsan_lsw_write_tab128(u32 base, u32 row, const u32 words[4])
{
	struct hsan_bank *bank;
	u32 logical, offset;
	int i, ret;
	unsigned long flags;

	if (!words || !base || row >= (0x01000000U >> 4))
		return -EINVAL;
	if (check_add_overflow(base, row << 4, &logical))
		return -EOVERFLOW;
	if (hsan_lsw_pub_private_span(logical, 16))
		return -EOPNOTSUPP;
	/* A row is one 16-byte transaction. Never span logical banks. */
	if ((logical >> 24) != (((u64)logical + 15U) >> 24))
		return -ERANGE;
	if (!READ_ONCE(experimental_allow_lsw_writes))
		return -EACCES;
	/* EQM is a shared FIFO/command engine. A 128-bit table write is not
	 * permitted to bypass its mutex by straddling the command ports.
	 */
	if (hsan_lsw_ifc_private_span(logical, 4 * sizeof(u32)) ||
        hsan_lsw_napt_private_span(logical, 4 * sizeof(u32)) ||
        hsan_lsw_mcc_private_span(logical, 4 * sizeof(u32)) ||
        hsan_lsw_eqm_private_span(logical, 4 * sizeof(u32)))
		return -EOPNOTSUPP;
	if (hsan_lsw_fam_bank(logical >> 24))
		mutex_lock(&hsan_lsw_fam_lock);
	if ((logical >> 24) == 0x42)
		mutex_lock(&hsan_lsw_eqm_lock);
	if ((logical >> 24) == 0x03)
		mutex_lock(&hsan_lsw_napt_lock);
    if (hsan_lsw_mcc_bank(logical >> 24))
        mutex_lock(&hsan_lsw_mcc_lock);

	/* Hold the IRQ-safe bank lock across all four writes: probe/remove
	 * cannot unmap mid-row, and another table caller cannot interleave.
	 * Validate the ENTIRE 16-byte window before writing any word;
	 * previously the fourth word could fail after three MMIO writes.
	 */
    if (hsan_lsw_fam_poisoned &&
        hsan_lsw_fam_private_span(logical, 4 * sizeof(u32))) {
        ret = -EUCLEAN;
        goto unlock_banks;
    }
	spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
        goto out;
    }
	ret = hsan_lsw_access_locked(logical, &bank, &offset);
	if (ret)
		goto out;
	if (bank->bytes < 4 * sizeof(u32) ||
	    (resource_size_t)offset > bank->bytes - 4 * sizeof(u32)) {
		ret = -ERANGE;
		goto out;
	}
	/* OEM hi_lsw_write_tab128 @.text+0x268: MSW first. */
	for (i = 3; i >= 0; --i)
		writel(words[i], bank->regs + offset + i * sizeof(u32));
    /* Preserve the OEM 3->2->1->0 publication order before another
     * CPU can enter this CSR owner. Bus DMA quiescence is NOT implied.
     */
    wmb();
out:
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
unlock_banks:
	if ((logical >> 24) == 0x42)
		mutex_unlock(&hsan_lsw_eqm_lock);
	if (hsan_lsw_fam_bank(logical >> 24))
		mutex_unlock(&hsan_lsw_fam_lock);
	if (hsan_lsw_mcc_bank(logical >> 24))
		mutex_unlock(&hsan_lsw_mcc_lock);
	if ((logical >> 24) == 0x03)
		mutex_unlock(&hsan_lsw_napt_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_write_tab128);

/* OEM hi_lsw_pub.ko hi_lsw_read_tab128 @0x1fc reads the four
 * 32-bit words at base+(row<<4) in increasing address order.
 * Unlike four independent hsan_lsw_read() calls this is one register
 * ownership transaction, without a mid-row unmap or competing writer.
 * FIFO, command and write-strobe windows are excluded.
 */
int hsan_lsw_read_tab128(u32 base, u32 row, u32 words[4])
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 logical, offset;
    unsigned int i;
    int ret;

    if (!words || !base || !IS_ALIGNED(base, 16) ||
        row >= (0x01000000U >> 4))
        return -EINVAL;
    if (check_add_overflow(base, row << 4, &logical))
        return -EOVERFLOW;
    if ((logical >> 24) != (((u64)logical + 15U) >> 24))
        return -ERANGE;
    if (hsan_lsw_eqm_private_span(logical, 16) ||
        hsan_lsw_ifc_private_span(logical, 16) ||
        hsan_lsw_napt_private_span(logical, 16) ||
        hsan_lsw_fam_private_span(logical, 16))
        return -EOPNOTSUPP;
    if (hsan_lsw_fam_bank(logical >> 24))
        mutex_lock(&hsan_lsw_fam_lock);
    if ((logical >> 24) == 0x42)
        mutex_lock(&hsan_lsw_eqm_lock);
    if ((logical >> 24) == 0x03)
        mutex_lock(&hsan_lsw_napt_lock);
    if (hsan_lsw_mcc_bank(logical >> 24))
        mutex_lock(&hsan_lsw_mcc_lock);
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_access_locked(logical, &bank, &offset);
    if (!ret && (bank->bytes < 16 ||
                 (resource_size_t)offset > bank->bytes - 16))
        ret = -ERANGE;
    if (!ret)
        for (i = 0; i < 4; ++i)
            words[i] = readl(bank->regs + offset + i * 4U);
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    if (hsan_lsw_mcc_bank(logical >> 24))
        mutex_unlock(&hsan_lsw_mcc_lock);
    if ((logical >> 24) == 0x03)
        mutex_unlock(&hsan_lsw_napt_lock);
    if ((logical >> 24) == 0x42)
        mutex_unlock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_fam_bank(logical >> 24))
        mutex_unlock(&hsan_lsw_fam_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_read_tab128);

/* OEM hi_lsw_pfe.ko l3intf_write @.text+0x3b38..0x3c74:
 * two ordered table writes, first bank11+0x05b000, then +0x05c000.
 * The CSR owner permits ONE guarded compare-and-commit transaction.
 * Preimage comparison and both MMIO spans are checked before either
 * row is modified. Holding the bank spinlock prevents competing CPU
 * CSR access/removal. It cannot make eight MMIO writes atomic to a
 * hardware reader, prove forwarding activation, or provide STOP ACK.
 */
/* Only one board-observed L3 interface entry may be physically
 * submitted during this boot. In particular no second caller can
 * replay it after an uncertain first write/readback transaction.
 */
static bool hsan_lsw_l3intf_pair_written;
static int hsan_lsw_l3intf_pair_prepare_locked(struct device *owner, u32 row,
                                              const u32 *before[2],
                                              struct hsan_bank *bank[2],
                                              u32 offsets[2])
{
    const u32 bases[2] = { 0x0b05b000U, 0x0b05c000U };
    u32 logical;
    unsigned int i, j;
    int ret;

    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    if (!hsan_lsw_cpu_write_allowed_locked())
        return -ESHUTDOWN;
    if (hsan_lsw_l3intf_pair_written)
        return -EALREADY;
    /* Both tables live in bank 0x0b. Access_locked() handles the mapped
     * owner; validate the FULL 16-byte window of each row before writing.
     */
    for (i = 0; i < ARRAY_SIZE(bases); i++) {
        logical = bases[i] + row * 16U;
        ret = hsan_lsw_access_locked(logical, &bank[i], &offsets[i]);
        if (ret)
            return ret;
        if (bank[i]->owner != owner)
            return -EPERM;
        if (bank[i]->bytes < 16U ||
            (resource_size_t)offsets[i] > bank[i]->bytes - 16U)
            return -ERANGE;
    }
    for (i = 0; i < ARRAY_SIZE(bases); i++) {
        for (j = 0; j < 4; j++) {
            if (readl(bank[i]->regs + offsets[i] + j * 4U) !=
                before[i][j])
                return -ESTALE;
        }
    }
    return 0;
}

int hsan_lsw_l3intf_pair_preflight(struct device *owner, u32 row,
                                  const u32 before_a[4],
                                  const u32 before_b[4])
{
    const u32 *before[2] = { before_a, before_b };
    struct hsan_bank *bank[2];
    u32 offsets[2];
    unsigned long flags;
    int ret;

    if (!owner || !before_a || !before_b || !row || row > 31U)
        return -EINVAL;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_l3intf_pair_prepare_locked(owner, row, before,
                                              bank, offsets);
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_l3intf_pair_preflight);

int hsan_lsw_l3intf_pair_commit(struct device *owner, u32 row,
                                const u32 before_a[4], const u32 before_b[4],
                                const u32 after_a[4], const u32 after_b[4],
                                bool *submitted, bool *readback_ok)
{
    const u32 *before[2] = { before_a, before_b };
    const u32 *after[2] = { after_a, after_b };
    struct hsan_bank *bank[2];
    u32 offsets[2];
    unsigned long flags;
    unsigned int i, j;
    int ret;

    /* Even an invalid call must not leave stale success flags behind. */
    if (submitted)
        *submitted = false;
    if (readback_ok)
        *readback_ok = false;
    if (!owner || !before_a || !before_b || !after_a || !after_b ||
        !submitted || !readback_ok || !row || row > 31U)
        return -EINVAL;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_l3intf_pair_prepare_locked(owner, row, before,
                                              bank, offsets);
    if (ret)
        goto unlock;
    /* No fallible operations between first MMIO and final readback.
     * OEM write_tab128 uses MSW first: A[3..0], then B[3..0].
     */
    hsan_lsw_l3intf_pair_written = true;
    /* Keep the central once-only latch alive even for a consumer other
     * than hsan-lsw-pfe; its caller's module pin is not this API's policy.
     */
    __module_get(THIS_MODULE);
    *submitted = true;
    for (i = 0; i < ARRAY_SIZE(after); i++)
        for (j = 4; j-- > 0; )
            writel(after[i][j], bank[i]->regs + offsets[i] + j * 4U);
    wmb();
    for (i = 0; i < ARRAY_SIZE(after); i++) {
        for (j = 0; j < 4; j++) {
            if (readl(bank[i]->regs + offsets[i] + j * 4U) !=
                after[i][j]) {
                ret = -EUCLEAN;
                goto unlock;
            }
        }
    }
    *readback_ok = true;
unlock:
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_l3intf_pair_commit);

/* One-shot vendor MCC register stream (hi_pfe_mcc_init @0x6c5c).
 * Its 255 contiguous bank5 words and five bank2/5 control registers
 * form ONE transaction. Calls to generic CSR write paths cannot bypass
 * the command owner. As there is no qualified MCC reset/ACK, the module
 * remains pinned and the stream is never replayed after any attempt.
 */
int hsan_lsw_mcc_health(void)
{
    int ret;

    mutex_lock(&hsan_lsw_mcc_lock);
    ret = hsan_lsw_mcc_poisoned ? -EUCLEAN : 0;
    mutex_unlock(&hsan_lsw_mcc_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_mcc_health);

static int hsan_lsw_mcc_update_raw(u32 csr, u32 mask, u32 value)
{
    u32 old, readback, updated;
    int ret;

    ret = hsan_lsw_read_raw(csr, &old);
    if (ret)
        return ret;
    updated = (old & ~mask) | (value & mask);
    if (updated != old) {
        ret = hsan_lsw_write_raw(csr, updated);
        if (ret)
            return ret;
    }
    ret = hsan_lsw_read_raw(csr, &readback);
    return ret ? ret : (((readback ^ updated) & mask) ? -EUCLEAN : 0);
}

int hsan_lsw_mcc_commit(u32 *rows_written)
{
    static const u32 control[] = {
        0x0500000cU, 0x0200009cU, 0x02000094U,
        0x02000098U, 0x02000090U,
    };
    struct hsan_bank *bank;
    unsigned long flags;
    unsigned int i;
    u32 offset, readback;
    int ret = 0;

    if (!rows_written)
        return -EINVAL;
    *rows_written = 0;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_mcc_lock);
    if (hsan_lsw_mcc_poisoned) {
        ret = -EUCLEAN;
        goto out;
    }
    /* Check the entire 255-word range and every control register before
     * making hardware changes. Two physical banks are already mapped.
     */
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_access_locked(0x05055004U, &bank, &offset);
    if (!ret && (bank->bytes < 255U * sizeof(u32) ||
                 (resource_size_t)offset > bank->bytes - 255U * sizeof(u32)))
        ret = -ERANGE;
    for (i = 0; !ret && i < ARRAY_SIZE(control); ++i)
        ret = hsan_lsw_access_locked(control[i], &bank, &offset);
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    if (ret)
        goto out;
    hsan_lsw_mcc_poisoned = true;
    if (!atomic_xchg(&hsan_lsw_mcc_module_pinned, 1))
        __module_get(THIS_MODULE);
    for (i = 1; i <= 255; ++i) {
        ret = hsan_lsw_write_raw(0x05055004U + 4U * (i - 1), 0x200U | i);
        if (ret)
            goto out;
        ++*rows_written;
        if (!(i & 63))
            cond_resched();
    }
    ret = hsan_lsw_write_raw(0x0500000cU, 0);
    if (ret)
        goto out;
    ret = hsan_lsw_write_raw(0x0200009cU, 17);
    if (ret)
        goto out;
    ret = hsan_lsw_mcc_update_raw(0x02000094U, GENMASK(18, 16), BIT(17));
    if (ret)
        goto out;
    ret = hsan_lsw_mcc_update_raw(0x02000098U, GENMASK(18, 16), BIT(17));
    if (ret)
        goto out;
    ret = hsan_lsw_write_raw(0x02000090U, 1);
    if (ret)
        goto out;
    /* Verify stored rows/config only; 0x02000090 is the OEM enable
     * command, not an invented MCC forwarding or drain acknowledgement
     */
    for (i = 1; i <= 255; ++i) {
        ret = hsan_lsw_read_raw(0x05055004U + 4U * (i - 1), &readback);
        if (ret)
            goto out;
        if (readback != (0x200U | i)) {
            ret = -EUCLEAN;
            goto out;
        }
    }
    for (i = 0; i < ARRAY_SIZE(control) - 1; ++i) {
        u32 mask = i < 2 ? U32_MAX : GENMASK(18, 16);
        u32 expected = i == 0 ? 0 : i == 1 ? 17 : BIT(17);

        ret = hsan_lsw_read_raw(control[i], &readback);
        if (ret)
            goto out;
        if ((readback & mask) != expected) {
            ret = -EUCLEAN;
            goto out;
        }
    }
out:
    mutex_unlock(&hsan_lsw_mcc_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_mcc_commit);

/* OEM nhp_write @0x3ca0, one guarded PUB 0x101 row per boot */
static bool hsan_lsw_nhp_written;
int hsan_lsw_nhp_commit(struct device *owner, u32 row,
                        const u32 before[4], const u32 after[4])
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 offset, i;
    int ret;

    if (!owner || !before || !after || !row || row > 399U)
        return -EINVAL;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
        goto out;
    }
    if (hsan_lsw_nhp_written) {
        ret = -EALREADY;
        goto out;
    }
    ret = hsan_lsw_access_locked(0x0b059000U + row * 16U, &bank, &offset);
    if (ret)
        goto out;
    if (bank->owner != owner || bank->bytes < 16U ||
        (resource_size_t)offset > bank->bytes - 16U) {
        ret = -EPERM;
        goto out;
    }
    for (i = 0; i < 4; i++) {
        if (readl(bank->regs + offset + i * 4) != before[i]) {
            ret = -EAGAIN;
            goto out;
        }
    }
    hsan_lsw_nhp_written = true;
    __module_get(THIS_MODULE);
    /* Original write_tab128 publishes the low word last */
    for (i = 4; i-- > 0;)
        writel(after[i], bank->regs + offset + i * 4);
    wmb();
    for (i = 0; i < 4; i++) {
        if (readl(bank->regs + offset + i * 4) != after[i]) {
            ret = -EUCLEAN;
            goto out;
        }
    }
    ret = 0;
out:
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_nhp_commit);

/* OEM hi_lsw_pfe.ko ifc_dump @.text+0x4c0 and +0x58c:
 * seven IFC parameter registers (0x138..0x154, skipping 0x150), a
 * 0x120 command with index [8:0] and bits 31:30 set, and up to 1001
 * status polls (bit31 busy, bit29 fault). On success, two 128-bit
 * payload rows are written to bank0 table 0x50000 at index*2/+1.
 *
 * Hold ONE IRQ-safe bank lock for the full command and both table rows.
 * Individual hsan_lsw_write_tab128() calls would permit unregistration
 * and unrelated IFC traffic to interleave between the two halves.
 * If a write is attempted and the command fails, latch uncertainty and pin
 * this module; callers may not replay a partially committed table. A full
 * command batch without table ACK is explicitly quarantined by its owner.
 */
#define HSAN_IFC_CONTROL_CSR 0x00000120U
#define HSAN_IFC_TABLE_BASE  0x00050000U
#define HSAN_IFC_POLL_LIMIT  1001U
#define HSAN_IFC_MAX_INDEX   0x1ffU
static const u32 hsan_ifc_param_offsets[] = {
    0x138U, 0x13cU, 0x140U, 0x144U, 0x148U, 0x14cU, 0x154U,
};
static bool hsan_lsw_ifc_poisoned; /* protected by hsan_lsw_lock */
static atomic_t hsan_lsw_ifc_module_pinned = ATOMIC_INIT(0);

/* Once an IFC command may have changed hardware without an audited reset or
 * table readback, keep this owner module resident. Otherwise rmmod/reload
 * would erase the poison latch while the physical IFC engine may retain state.
 */
static void hsan_lsw_ifc_pin_module(void)
{
    if (atomic_cmpxchg(&hsan_lsw_ifc_module_pinned, 0, 1) == 0)
        __module_get(THIS_MODULE);
}

int hsan_lsw_ifc_quarantine(void)
{
    unsigned long flags;

    spin_lock_irqsave(&hsan_lsw_lock, flags);
    hsan_lsw_ifc_poisoned = true;
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    hsan_lsw_ifc_pin_module();
    pr_err("hsan-lsw-csr: IFC command engine quarantined; module pinned until reboot\n");
    return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_ifc_quarantine);

int hsan_lsw_ifc_health(void)
{
    unsigned long flags;
    bool poisoned;

    spin_lock_irqsave(&hsan_lsw_lock, flags);
    poisoned = hsan_lsw_ifc_poisoned;
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return poisoned ? -EUCLEAN : 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_ifc_health);

static int hsan_lsw_ifc_commit_ordered(u16 index, const u32 control[7],
                        const u32 data[8], u32 *last_status, bool dynamic)
{
    struct hsan_bank *bank, *check;
    unsigned long flags;
    u32 offset, table_offset, status = 0, command;
    unsigned int i, j;
    bool issued = false;
    bool pin_module = false;
    int ret;

    if (!control || !data || index > HSAN_IFC_MAX_INDEX)
        return -EINVAL;
    if (last_status)
        *last_status = 0;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
        goto out;
    }
    if (hsan_lsw_ifc_poisoned) {
        ret = -EUCLEAN;
        goto out;
    }
    ret = hsan_lsw_access_locked(HSAN_IFC_CONTROL_CSR, &bank, &offset);
    if (ret)
        goto out;
    ret = hsan_lsw_access_locked(HSAN_IFC_TABLE_BASE +
                 (u32)index * 2U * 16U, &check, &table_offset);
    if (ret)
        goto out;
    if (bank != check || check->bytes < 8U * sizeof(u32) ||
        (resource_size_t)table_offset > check->bytes - 8U * sizeof(u32)) {
        ret = -ERANGE;
        goto out;
    }
    for (i = 0; i < ARRAY_SIZE(hsan_ifc_param_offsets); i++) {
        u32 reg_offset;

        ret = hsan_lsw_access_locked(hsan_ifc_param_offsets[i],
                                     &check, &reg_offset);
        if (ret)
            goto out;
        if (check != bank) {
            ret = -ERANGE;
            goto out;
        }
    }
    status = readl(bank->regs + (HSAN_IFC_CONTROL_CSR & 0xffffffU));
    if (status & BIT(31)) {
        ret = -EBUSY;
        goto out;
    }
    if (status & BIT(29)) {
        /* Pre-existing fault means unverified IFC hardware history even
         * when this invocation has not yet issued its first parameter.
         * Keep the poison latch resident across rmmod/reload attempts.
         */
        hsan_lsw_ifc_poisoned = true;
        pin_module = true;
        ret = -EUCLEAN;
        goto out;
    }
    /* OEM dynamic ifc_write encodes/publishes both action halves before
     * submitting the key. Factory helper @0x58c uses the reverse order
     * Keep the entire native transaction under the same ownership lock
     */
    issued = true;
    if (dynamic) {
        for (j = 0; j < 2; j++)
            for (i = 4; i > 0; i--)
                writel(data[j * 4 + i - 1],
                       bank->regs + table_offset + (j * 4 + i - 1) * 4U);
        wmb();
    }
    for (i = 0; i < ARRAY_SIZE(hsan_ifc_param_offsets); i++) {
        writel(control[i], bank->regs + hsan_ifc_param_offsets[i]);
    }
    command = (status & ~0xc00001ffU) | 0xc0000000U | index;
    wmb();
    writel(command, bank->regs + (HSAN_IFC_CONTROL_CSR & 0xffffffU));
    for (i = 0; i < HSAN_IFC_POLL_LIMIT; i++) {
        status = readl(bank->regs + (HSAN_IFC_CONTROL_CSR & 0xffffffU));
        if (!(status & BIT(31)))
            break;
        cpu_relax();
    }
    if (status & BIT(31)) {
        ret = -ETIMEDOUT;
        goto out;
    }
    if (status & BIT(29)) {
        ret = -EIO;
        goto out;
    }
    /* hi_lsw_write_tab128 writes MSW first; each half is 16 bytes. */
    if (!dynamic)
        for (j = 0; j < 2; j++)
            for (i = 4; i > 0; i--)
                writel(data[j * 4 + i - 1],
                       bank->regs + table_offset + (j * 4 + i - 1) * 4U);
    wmb();
    /* OEM hi_lsw_read_tab128 @0x1fc reads the same 128-bit MMIO
     * window in low-to-high word order. A mismatch is a hard fault:
     * never replay an IFC command against unknown hardware state.
     * A match proves only the current CPU-visible address window, NOT
     * IFC rule activation, hardware table forwarding or DMA quiescence.
     */
    for (i = 0; i < 8; i++) {
        if (readl(bank->regs + table_offset + i * sizeof(u32)) != data[i]) {
            ret = -EUCLEAN;
            goto out;
        }
    }
    ret = 0;
out:
    if (issued) {
        /* A successful row commit is also an irreversible command. Keep
         * this CSR owner loaded from the FIRST hardware write onward,
         * including between the 12 OEM default rows. The PFE owner may
         * explicitly quarantine the engine after the full batch; pinning
         * alone never asserts that any IFC forwarding is operational.
         */
        pin_module = true;
        if (ret)
            hsan_lsw_ifc_poisoned = true;
    }
    if (last_status)
        *last_status = status;
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    if (pin_module)
        hsan_lsw_ifc_pin_module();
    return ret;
}
int hsan_lsw_ifc_commit(u16 index, const u32 control[7],
                        const u32 data[8], u32 *last_status)
{
    return hsan_lsw_ifc_commit_ordered(index, control, data, last_status, false);
}
EXPORT_SYMBOL_GPL(hsan_lsw_ifc_commit);

int hsan_lsw_ifc_dynamic_commit(u16 index, const u32 control[7],
                        const u32 data[8], u32 *last_status)
{
    return hsan_lsw_ifc_commit_ordered(index, control, data, last_status, true);
}
EXPORT_SYMBOL_GPL(hsan_lsw_ifc_dynamic_commit);

/* Exact read-only 32-byte IFC MMIO window, under the same bank lock as
 * IFC's irreversible dual-row writes. An unchanged window does NOT prove
 * active hash lookup, table backing, PFE routing, or DMA quiescence.
 */
int hsan_lsw_ifc_read_window(u16 index, u32 words[8])
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 reg, offset;
    unsigned int i;
    int ret;

    if (!words || index > HSAN_IFC_MAX_INDEX)
        return -EINVAL;
    reg = HSAN_IFC_TABLE_BASE + (u32)index * 32U;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_access_locked(reg, &bank, &offset);
    if (!ret && (bank->bytes < 32 ||
                 (resource_size_t)offset > bank->bytes - 32))
        ret = -ERANGE;
    if (!ret)
        for (i = 0; i < 8; i++)
            words[i] = readl(bank->regs + offset + i * 4U);
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_ifc_read_window);

/* Reject malformed or unregistered logical register ranges before any
 * multi-write operation. A check is not a reservation: the calling owner
 * must keep its bank registered until its subsequent writes complete.
 */
int hsan_lsw_check_span(u32 logical_address, u32 bytes, bool for_write)
{
	struct hsan_bank *bank;
	unsigned long flags;
	u32 offset;
	int ret;

	if (!bytes || !IS_ALIGNED(bytes, 4) ||
	    !IS_ALIGNED(logical_address, 4))
		return -EINVAL;
	if (((u64)logical_address + bytes - 1) > U32_MAX ||
	    (logical_address >> 24) !=
	    (((u64)logical_address + bytes - 1) >> 24))
		return -ERANGE;
	if (for_write && hsan_lsw_pub_private_span(logical_address, bytes))
		return -EOPNOTSUPP;
	if (for_write && !READ_ONCE(experimental_allow_lsw_writes))
		return -EACCES;
	spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (for_write && !hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
    } else {
        ret = hsan_lsw_access_locked(logical_address, &bank, &offset);
    }
	if (!ret && (bank->bytes < bytes ||
		     (resource_size_t)offset > bank->bytes - bytes))
		ret = -ERANGE;
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_check_span);

/* hi_pfe_napt_init @0x6448..0x6524 programs the observed partition
 * registers and submits TWO separate commands to 0x03000060:
 * 0x00080001 then 0x000c0001. hi_lsw_wait_clr(0x03000060,0,0x30d40)
 * checks BIT(0) clear, not the whole register equal zero. This is a
 * stateful engine and the command register is hidden from generic writes.
 *
 * All five partition words MUST come from one real board execution. In
 * particular, do not derive addresses or bitfields from Linux RAM size.
 * A cleared bit0 only qualifies completion of these command strobes; it
 * does NOT prove correct NAT table contents, forwarding or DMA STOP.
 */
#define HSAN_NAPT_COMMAND_CSR    0x03000060U
#define HSAN_NAPT_COMMAND1      0x00080001U
#define HSAN_NAPT_COMMAND2      0x000c0001U
#define HSAN_NAPT_POLL_LIMIT    1001U
static const u32 hsan_napt_layout_csrs[5] = {
    0x03000130U, 0x03000138U, 0x03000134U,
    0x0300013cU, 0x03000068U,
};

int hsan_lsw_napt_health(void)
{
    int ret;

    mutex_lock(&hsan_lsw_napt_lock);
    ret = hsan_lsw_napt_poisoned ? -EUCLEAN : 0;
    mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_health);

int hsan_lsw_napt_partition_commit(const u32 words[5], u32 *last_status)
{
    struct hsan_bank *bank;
    unsigned long flags;
    static const u32 commands[2] = {
        HSAN_NAPT_COMMAND1, HSAN_NAPT_COMMAND2,
    };
    u32 reg, status = ~0U;
    unsigned int i, j;
    int ret = 0;

    if (!words || !last_status)
        return -EINVAL;
    *last_status = 0;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_napt_lock);
    if (hsan_lsw_napt_poisoned) {
        ret = -EUCLEAN;
        goto out;
    }
    /* Validate the complete MMIO window under the bank-registration lock,
     * before the first operation. The outer NAPT mutex prevents unmapping
     * and interleaving with generic bank03 accesses until both ACKs finish.
     */
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    for (i = 0; i < ARRAY_SIZE(hsan_napt_layout_csrs); i++) {
        ret = hsan_lsw_access_locked(hsan_napt_layout_csrs[i], &bank, &reg);
        if (ret)
            break;
    }
    if (!ret)
        ret = hsan_lsw_access_locked(HSAN_NAPT_COMMAND_CSR, &bank, &reg);
    if (!ret) {
        status = readl(bank->regs + reg);
        if (status & BIT(0))
            ret = -EBUSY; /* stale command, prior HW owner or fault */
    }
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    if (ret)
        goto out;
    /* Before the first irreversible register write, permanently retain
     * the CSR module and reject a second transaction in this boot.
     */
    hsan_lsw_napt_poisoned = true;
    if (!atomic_xchg(&hsan_lsw_napt_module_pinned, 1))
        __module_get(THIS_MODULE);
    for (i = 0; i < ARRAY_SIZE(hsan_napt_layout_csrs); i++) {
        ret = hsan_lsw_write_raw(hsan_napt_layout_csrs[i], words[i]);
        if (ret)
            goto out;
        /* These five are stored configuration, not the command port.
         * Fail closed if the register values cannot be read back.
         */
        ret = hsan_lsw_read_raw(hsan_napt_layout_csrs[i], &status);
        if (ret)
            goto out;
        if (status != words[i]) {
            ret = -EUCLEAN;
            goto out;
        }
    }
    for (i = 0; i < ARRAY_SIZE(commands); i++) {
        ret = hsan_lsw_write_raw(HSAN_NAPT_COMMAND_CSR, commands[i]);
        if (ret)
            goto out;
        for (j = 0; j < HSAN_NAPT_POLL_LIMIT; j++) {
            ret = hsan_lsw_read_raw(HSAN_NAPT_COMMAND_CSR, &status);
            if (ret)
                goto out;
            if (!(status & BIT(0)))
                break;
            usleep_range(100, 200);
        }
        if (j == HSAN_NAPT_POLL_LIMIT) {
            ret = -ETIMEDOUT;
            goto out;
        }
    }
    hsan_lsw_napt_partition_complete = true;
out:
    *last_status = status;
    mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_partition_commit);

/* OEM IPv4 GRO add command @0x5ae0..0x5c94, restricted to resource1
 * The init latch remains sealed; command readiness comes only from both
 * completed partition commands, never from clearing an uncertainty flag
 */
static int hsan_lsw_napt_gro_v4_submit(const u32 words[19], u32 *hardware_id,
                                     bool restoring, u32 previous_id, unsigned int lane)
{
    static const struct { u32 csr; u8 word; } params[] = {
        {0x030000a8,1}, {0x030000b0,3}, {0x030000ac,2},
        {0x030000b4,4}, {0x030000b8,5}, {0x030000bc,6},
        {0x030000c0,7}, {0x030000c4,8}, {0x030000c8,9},
        {0x030000cc,10}, {0x030000d0,11}, {0x030002d8,12},
        {0x030002d4,13}, {0x030002dc,14}, {0x030002e0,15},
        {0x030002e4,16}, {0x030002e8,17}, {0x030002ec,18},
    };
    static const u8 zero_words[] = {5,6,7,9,10,11,13,14,16,17,18};
    struct hsan_bank *bank;
    unsigned long flags;
    unsigned int i;
    u32 offset, status;
    int ret;
    struct hsan_managed_gro_flow *record = NULL;
    bool *attempted = &hsan_lsw_napt_gro_attempted;
    bool *removed = &hsan_lsw_napt_gro_removed;
    u32 *flow_id = &hsan_lsw_napt_gro_id;
    u32 *flow_words = hsan_lsw_napt_gro_words;

    if (lane > 4 || !hardware_id)
        return -EINVAL;
    *hardware_id = U32_MAX;
    if (!words)
        return -EINVAL;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_napt_lock);
    if (!!lane != hsan_managed_gro_enabled) {
        ret = -EACCES;
        goto out;
    }
    if (lane) {
        record = &hsan_managed_gro[lane - 1];
        attempted = &record->attempted;
        removed = &record->removed;
        flow_id = &record->id;
        flow_words = record->words;
    }
    if (words[0] != 0x00010100U || words[1] != 0x04020000U ||
        words[2] != 0x50000004U || words[12] != 0x00000603U ||
        words[15] != (0x80000000U | ((lane ? lane : 1U) << 27))) {
        ret = -EINVAL;
        goto out;
    }
    for (i = 0; i < ARRAY_SIZE(zero_words); i++) {
        if (words[zero_words[i]]) {
            ret = -EINVAL;
            goto out;
        }
    }
    if (!hsan_lsw_napt_partition_complete) {
        ret = -EHOSTDOWN;
        goto out;
    }
    if (hsan_lsw_napt_flow_uncertain) {
        ret = -EUCLEAN;
        goto out;
    }
    if (restoring && (!*removed || previous_id != *flow_id)) {
        ret = -EINVAL;
        goto out;
    }
    if (!restoring && *attempted && (!lane || !*removed)) {
        ret = -EALREADY;
        goto out;
    }
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_access_locked(0x0300008cU, &bank, &offset);
    if (!ret)
        ret = hsan_lsw_access_locked(0x03000090U, &bank, &offset);
    for (i = 0; !ret && i < ARRAY_SIZE(params); i++)
        ret = hsan_lsw_access_locked(params[i].csr, &bank, &offset);
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    if (ret)
        goto out;
    ret = hsan_lsw_read_raw(0x0300008cU, &status);
    if (ret)
        goto out;
    if (status & BIT(24)) {
        ret = -EBUSY;
        goto out;
    }
    *attempted = true;
    hsan_lsw_napt_gro_attempted = true;
    hsan_lsw_napt_flow_uncertain = true;
    for (i = 0; i < ARRAY_SIZE(params); i++) {
        ret = hsan_lsw_write_raw(params[i].csr, words[params[i].word]);
        if (ret)
            goto out;
    }
    ret = hsan_lsw_write_raw(0x0300008cU, words[0] | BIT(24) | BIT(0));
    if (ret)
        goto out;
    for (i = 0; i < HSAN_NAPT_POLL_LIMIT; i++) {
        ret = hsan_lsw_read_raw(0x0300008cU, &status);
        if (ret)
            goto out;
        if (!(status & BIT(24)))
            break;
        usleep_range(100, 200);
    }
    if (i == HSAN_NAPT_POLL_LIMIT) {
        ret = -ETIMEDOUT;
        goto out;
    }
    ret = hsan_lsw_read_raw(0x03000090U, &status);
    if (ret)
        goto out;
    if (!(status & BIT(21))) {
        ret = -EIO;
        goto out;
    }
    if (lane) {
        for (i = 0; i < ARRAY_SIZE(hsan_managed_gro); i++) {
            if (&hsan_managed_gro[i] != record && hsan_managed_gro[i].valid &&
                hsan_managed_gro[i].id == (status & GENMASK(20, 0))) {
                ret = -EUCLEAN;
                goto out;
            }
        }
    }
    *hardware_id = status & GENMASK(20, 0);
    *flow_id = *hardware_id;
    if (record)
        record->valid = true;
    if (!restoring)
        memcpy(flow_words, words, sizeof(hsan_lsw_napt_gro_words));
    *removed = false;
    hsan_lsw_napt_flow_uncertain = false;
out:
    mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
int hsan_lsw_napt_gro_v4_commit(const u32 words[19], u32 *hardware_id)
{
    return hsan_lsw_napt_gro_v4_submit(words, hardware_id, false, U32_MAX, 0);
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_gro_v4_commit);

/* Only a completed removal permits restoring the immutable original flow
 * Partial commands, query timeouts and the initial one-shot latch stay sealed
 */
int hsan_lsw_napt_gro_v4_restore(u32 previous_id, u32 *hardware_id)
{
    return hsan_lsw_napt_gro_v4_submit(hsan_lsw_napt_gro_words, hardware_id,
                                       true, previous_id, 0);
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_gro_v4_restore);

/* Managed mode owns four independent GRO lanes and the common L3/NHP1
 * A completed delete permits reusing its lane; uncertainty never permits it
 * The legacy one-shot latch remains set for every issued add transaction
 */
int hsan_lsw_napt_gro_managed_enable(void)
{
    unsigned int i;
    int ret = 0;

    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_napt_lock);
    if (!hsan_lsw_napt_partition_complete)
        ret = -EHOSTDOWN;
    else if (hsan_lsw_napt_flow_uncertain)
        ret = -EUCLEAN;
    else if (hsan_lsw_napt_gro_attempted || hsan_managed_gro_enabled)
        ret = -EALREADY;
    else {
        for (i = 0; i < ARRAY_SIZE(hsan_managed_gro); i++)
            hsan_managed_gro[i].id = U32_MAX;
        hsan_managed_gro_enabled = true;
    }
    mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_gro_managed_enable);

int hsan_lsw_napt_gro_managed_add(unsigned int lane, const u32 words[19], u32 *id)
{
    if (!lane || lane > 4)
        return -EINVAL;
    return hsan_lsw_napt_gro_v4_submit(words, id, false, U32_MAX, lane);
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_gro_managed_add);

static struct hsan_managed_gro_flow *hsan_lsw_managed_gro_find(u32 id)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(hsan_managed_gro); i++)
        if (hsan_managed_gro[i].valid && hsan_managed_gro[i].id == id)
            return &hsan_managed_gro[i];
    return NULL;
}

static bool hsan_lsw_gro_id_owned(u32 id)
{
    return hsan_managed_gro_enabled ? hsan_lsw_managed_gro_find(id) != NULL :
                                     id == hsan_lsw_napt_gro_id;
}

/* OEM get_age @0x5f70 queries the encoded hardware ID, not a RAM offset
 * This command only snapshots a flow; it neither refreshes nor reinstalls it
 */
int hsan_lsw_napt_gro_v4_snapshot(u32 hardware_id, u32 *status, u32 *data)
{
    unsigned int i;
    int ret;

    if (!status || !data || hardware_id > GENMASK(20, 0))
        return -EINVAL;
    *status = 0;
    *data = 0;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_napt_lock);
    if (!hsan_lsw_napt_partition_complete || !hsan_lsw_napt_gro_attempted) {
        ret = -EHOSTDOWN;
        goto out;
    }
    if (!hsan_lsw_gro_id_owned(hardware_id)) {
        ret = -EINVAL;
        goto out;
    }
    if (hsan_lsw_napt_flow_uncertain) {
        ret = -EUCLEAN;
        goto out;
    }
    ret = hsan_lsw_check_span(0x030000d4U, 12, false);
    if (ret)
        goto out;
    ret = hsan_lsw_read_raw(0x030000d4U, status);
    if (ret)
        goto out;
    if (*status & BIT(31)) {
        ret = -EBUSY;
        goto out;
    }
    hsan_lsw_napt_flow_uncertain = true;
    ret = hsan_lsw_write_raw(0x030000d4U, BIT(31) | hardware_id);
    if (ret)
        goto out;
    for (i = 0; i < HSAN_NAPT_POLL_LIMIT; i++) {
        ret = hsan_lsw_read_raw(0x030000d4U, status);
        if (ret)
            goto out;
        if (!(*status & BIT(31)))
            break;
        usleep_range(100, 200);
    }
    if (i == HSAN_NAPT_POLL_LIMIT) {
        ret = -ETIMEDOUT;
        goto out;
    }
    hsan_lsw_napt_flow_uncertain = false;
    if (*status & BIT(29)) {
        ret = -ENOENT;
        goto out;
    }
    ret = hsan_lsw_read_raw(0x030000dcU, data);
out:
    mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_gro_v4_snapshot);

/* OEM IPv4 delete @0x5910..0x5ad0, for this boot's sole owned flow
 * Command completion removes a flow, not DMA ownership or GRO buffers
 * Neither the one-shot add latch nor any resource quarantine is cleared
 */
int hsan_lsw_napt_gro_v4_remove(u32 hardware_id)
{
    static const u32 clear_csrs[] = {
        0x03000104, 0x03000108, 0x0300010c, 0x03000110,
        0x03000114, 0x03000118, 0x0300011c, 0x03000120,
        0x03000124, 0x03000128, 0x0300012c, 0x03000180,
        0x03000280, 0x03000284, 0x03000288, 0x0300028c,
        0x03000290, 0x03000294,
    };
    unsigned int i;
    u32 status;
    int ret;
    struct hsan_managed_gro_flow *record;
    bool *removed = &hsan_lsw_napt_gro_removed;

    if (hardware_id > GENMASK(20, 0))
        return -EINVAL;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_napt_lock);
    record = hsan_lsw_managed_gro_find(hardware_id);
    if (hsan_managed_gro_enabled && record)
        removed = &record->removed;
    if (!hsan_lsw_napt_partition_complete ||
        !hsan_lsw_gro_id_owned(hardware_id)) {
        ret = -EHOSTDOWN;
        goto out;
    }
    if (hsan_lsw_napt_flow_uncertain) {
        ret = -EUCLEAN;
        goto out;
    }
    if (*removed) {
        ret = -EALREADY;
        goto out;
    }
    ret = hsan_lsw_check_span(0x030000d4, 4, false);
    for (i = 0; !ret && i < ARRAY_SIZE(clear_csrs); i++)
        ret = hsan_lsw_check_span(clear_csrs[i], 4, false);
    if (ret)
        goto out;
    ret = hsan_lsw_read_raw(0x030000d4, &status);
    if (ret)
        goto out;
    if (status & BIT(31)) {
        ret = -EBUSY;
        goto out;
    }
    hsan_lsw_napt_flow_uncertain = true;
    for (i = 0; i < ARRAY_SIZE(clear_csrs); i++) {
        ret = hsan_lsw_write_raw(clear_csrs[i], 0);
        if (ret)
            goto out;
    }
    ret = hsan_lsw_write_raw(0x030000d4, BIT(31) | BIT(30) | hardware_id);
    if (ret)
        goto out;
    for (i = 0; i < HSAN_NAPT_POLL_LIMIT; i++) {
        ret = hsan_lsw_read_raw(0x030000d4, &status);
        if (ret)
            goto out;
        if (!(status & BIT(31)))
            break;
        usleep_range(100, 200);
    }
    if (i == HSAN_NAPT_POLL_LIMIT) {
        ret = -ETIMEDOUT;
        goto out;
    }
    hsan_lsw_napt_flow_uncertain = false;
    if (status & BIT(29)) {
        ret = -ENOENT;
        goto out;
    }
    *removed = true;
    if (hsan_managed_gro_enabled && record)
        record->valid = false;
out:
    mutex_unlock(&hsan_lsw_napt_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_napt_gro_v4_remove);




/* hi_fam_init_buf @0x21a0..0x2570: complete audited register sequence.
 * This is a SINGLE bank40/42/43 transaction rather than independently
 * locked CSR writes. A concurrent EQM FIFO command, even via another owner,
 * and ordinary CSR I/O on those three banks wait for the entire sequence.
 * The default-off module parameter AND DP's DT opt-in must both be present.
 * Completion is ONLY 0x43000000 bit0 observed set, NOT DMA quiescence.
 */
#define HSAN_FAM_STATUS_CSR 0x43000000U
#define HSAN_FAM_POLL_LIMIT 1000U
static const u32 hsan_fam_csrs[] = {
    0x40000028U, 0x4000001cU, 0x43000504U, 0x42000388U,
    0x420000a4U, 0x40000200U, 0x43000514U, 0x4300050cU,
    0x43000530U, 0x43000534U, 0x43000510U, 0x40000044U,
    0x43000098U, 0x430000a4U, HSAN_FAM_STATUS_CSR,
};

void hsan_lsw_fam_cpu_stop_fence(void)
{
    unsigned long flags;

    /* This may run from PIE IRQ/quarantine: no mutex or sleeping wait.
     * Linearize against all CSR write admissions using the bank spinlock.
     * Existing MMIO already sent to the bus is NOT cancelled here.
     */
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    atomic64_inc(&hsan_lsw_fam_stop_invocations);
    atomic_set(&hsan_lsw_fam_cpu_stopped, 1);
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_cpu_stop_fence);

/* A successful begin holds the exact spinlock used by the FAM CPU STOP
 * setter and every CSR write. Until end, the setter cannot return. This
 * closes the old check-then-publish race across PIE and external owners.
 * It is solely a CPU MMIO publication ordering contract, NOT an IOMMU,
 * DMA engine, bus write completion, or HW STOP/drain acknowledgement.
 */
/* OEM hi_ips_queue_init, executed once while PIE RX remains disabled. */
static bool hsan_ips_queues_verified;
static bool hsan_ips_init_attempted;
static bool hsan_ips_enable_attempted;

int hsan_lsw_ips_queue_init(void)
{
    static const u32 regs[] = {
        0x44000078, 0x44000200, 0x4400020c, 0x44000218,
        0x44000208, 0x44000214, 0x44000220, 0x44000634,
    };
    static const u32 values[] = {
        0, 0x00100001, 0x00113f00, 0x001200fe,
        0x0007ffff, 0x0007ffff, 0x0007ffff, 0x00011207,
    };
    struct hsan_bank *banks[ARRAY_SIZE(regs)];
    u32 offsets[ARRAY_SIZE(regs)];
    unsigned long flags;
    unsigned int i;
    int ret = 0;

    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
        goto out;
    }
    if (hsan_ips_init_attempted) {
        ret = -EALREADY;
        goto out;
    }
    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        ret = hsan_lsw_access_locked(regs[i], &banks[i], &offsets[i]);
        if (ret)
            goto out;
    }
    hsan_ips_init_attempted = true;
    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        writel(values[i], banks[i]->regs + offsets[i]);
        if (readl(banks[i]->regs + offsets[i]) != values[i]) {
            ret = -EUCLEAN;
            goto out;
        }
    }
    hsan_ips_queues_verified = true;
out:
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_ips_queue_init);

/* OEM hi_dp_probe @0xfb4 enables IPS before FAM init and queue init.
 * Called only after the owner's verified cold reset; PIE RX is disabled.
 */
int hsan_lsw_ips_boot_enable(void)
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 offset;
    int ret;

    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    if (!hsan_lsw_cpu_write_allowed_locked()) {
        ret = -ESHUTDOWN;
        goto out;
    }
    if (hsan_ips_enable_attempted) {
        ret = -EALREADY;
        goto out;
    }
    ret = hsan_lsw_access_locked(0x4400007c, &bank, &offset);
    if (ret)
        goto out;
    hsan_ips_enable_attempted = true;
    writel(1, bank->regs + offset);
    ret = readl(bank->regs + offset) == 1 ? 0 : -EUCLEAN;
out:
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_ips_boot_enable);

/* Caller holds the FAM publication guard: never recursively acquire it. */
int hsan_lsw_ips_enable_guarded(struct hsan_lsw_fam_publish_guard *guard)
{
    struct hsan_bank *bank;
    u32 offset;
    int ret;

    if (!guard || !guard->held)
        return -EINVAL;
    lockdep_assert_held(&hsan_lsw_lock);
    if (!READ_ONCE(experimental_allow_lsw_writes) ||
        !hsan_ips_queues_verified)
        return -EACCES;
    if (!hsan_lsw_cpu_write_allowed_locked())
        return -ESHUTDOWN;
    ret = hsan_lsw_access_locked(0x4400007c, &bank, &offset);
    if (ret)
        return ret;
    if (!hsan_ips_enable_attempted) {
        hsan_ips_enable_attempted = true;
        writel(1, bank->regs + offset);
    }
    return readl(bank->regs + offset) == 1 ? 0 : -EUCLEAN;
}
EXPORT_SYMBOL_GPL(hsan_lsw_ips_enable_guarded);

int hsan_lsw_fam_publish_begin(struct hsan_lsw_fam_publish_guard *guard)
{
    if (!guard)
        return -EINVAL;
    guard->held = false;
    spin_lock_irqsave(&hsan_lsw_lock, guard->irq_flags);
    if (atomic_read(&hsan_lsw_fam_cpu_stopped)) {
        atomic64_inc(&hsan_lsw_fam_publish_rejections);
        spin_unlock_irqrestore(&hsan_lsw_lock, guard->irq_flags);
        return -ESHUTDOWN;
    }
    atomic64_inc(&hsan_lsw_fam_publish_admissions);
    guard->held = true;
    return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_publish_begin);

/* Reclaim is NOT a producer publication. Use the same IRQ-safe STOP
 * serialization lock, but distinct counters, so a TX completion cannot
 * inflate the number of admitted hardware descriptor publications.
 * This permits CPU reclamation only while the software FAM STOP fence
 * remains open; the hardware consumer/descriptor checks belong to PIE.
 */
int hsan_lsw_fam_reclaim_begin(struct hsan_lsw_fam_publish_guard *guard)
{
    if (!guard)
        return -EINVAL;
    guard->held = false;
    spin_lock_irqsave(&hsan_lsw_lock, guard->irq_flags);
    if (atomic_read(&hsan_lsw_fam_cpu_stopped)) {
        atomic64_inc(&hsan_lsw_fam_reclaim_rejections);
        spin_unlock_irqrestore(&hsan_lsw_lock, guard->irq_flags);
        return -ESHUTDOWN;
    }
    atomic64_inc(&hsan_lsw_fam_reclaim_admissions);
    guard->held = true;
    return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_reclaim_begin);

/* Completion observation is neither an RX/TX producer publication nor a
 * DMA mapping reclaim. Stage75 needs to exclude the STOP setter while
 * sampling the hardware RPTR, without falsifying either class of counter.
 * This CPU lock does not quiesce the physical FAM or PIE DMA engines.
 */
static int hsan_lsw_fam_uncounted_begin(struct hsan_lsw_fam_publish_guard *guard)
{
    if (!guard)
        return -EINVAL;
    guard->held = false;
    spin_lock_irqsave(&hsan_lsw_lock, guard->irq_flags);
    if (atomic_read(&hsan_lsw_fam_cpu_stopped)) {
        spin_unlock_irqrestore(&hsan_lsw_lock, guard->irq_flags);
        return -ESHUTDOWN;
    }
    guard->held = true;
    return 0;
}

int hsan_lsw_fam_observe_begin(struct hsan_lsw_fam_publish_guard *guard)
{
    return hsan_lsw_fam_uncounted_begin(guard);
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_observe_begin);

int hsan_lsw_fam_irq_service_begin(struct hsan_lsw_fam_publish_guard *guard)
{
    return hsan_lsw_fam_uncounted_begin(guard);
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_irq_service_begin);

void hsan_lsw_fam_publish_end(struct hsan_lsw_fam_publish_guard *guard)
{
    if (!guard || !guard->held)
        return;
    /* Either a producer CPU write or an already-consumer-validated
     * mapping release has finished before FAM STOP can linearize. This
     * CPU serialization does not establish a hardware DMA drain ACK.
     */
    wmb();
    guard->held = false;
    spin_unlock_irqrestore(&hsan_lsw_lock, guard->irq_flags);
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_publish_end);

void hsan_lsw_fam_fence_snapshot(struct hsan_lsw_fam_fence_snapshot *out)
{
    if (!out)
        return;
    out->cpu_fenced = atomic_read(&hsan_lsw_fam_cpu_stopped);
    out->rejected_write_operations =
        atomic64_read(&hsan_lsw_fam_write_rejections);
    out->rejected_dma_publications =
        atomic64_read(&hsan_lsw_fam_publish_rejections);
    out->admitted_cpu_publications =
        atomic64_read(&hsan_lsw_fam_publish_admissions);
    out->rejected_tx_reclaims =
        atomic64_read(&hsan_lsw_fam_reclaim_rejections);
    out->admitted_tx_reclaims =
        atomic64_read(&hsan_lsw_fam_reclaim_admissions);
    out->stop_fence_invocations =
        atomic64_read(&hsan_lsw_fam_stop_invocations);
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_fence_snapshot);

int hsan_lsw_fam_health(void)
{
    int ret;

    mutex_lock(&hsan_lsw_fam_lock);
    ret = hsan_lsw_fam_poisoned ? -EUCLEAN : 0;
    mutex_unlock(&hsan_lsw_fam_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_health);

int hsan_lsw_fam_read_activity(struct hsan_lsw_fam_activity *out)
{
    struct hsan_bank *first, *second, *status_bank;
    unsigned long flags;
    u32 a, b, status_offset;
    int ret;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    /* Serialize with a complete FAM arm and with bank teardown. The
     * two reads are NOT a latched hardware snapshot and neither proves
     * an IRQ drain, DMA STOP, or a buffer ownership transfer.
     */
    mutex_lock(&hsan_lsw_fam_lock);
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = hsan_lsw_access_locked(0x430060d4U, &first, &a);
    if (ret)
        goto unlock;
    ret = hsan_lsw_access_locked(0x430061acU, &second, &b);
    if (ret)
        goto unlock;
    ret = hsan_lsw_access_locked(HSAN_FAM_STATUS_CSR,
                                 &status_bank, &status_offset);
    if (ret)
        goto unlock;
    if (first != second || first != status_bank) {
        ret = -ERANGE;
        goto unlock;
    }
    out->sample_begin_ns = ktime_get_ns();
    out->raw_init_ack_status = readl(status_bank->regs + status_offset);
    out->raw_60d4 = readl(first->regs + a);
    out->raw_61ac = readl(second->regs + b);
    out->sample_end_ns = ktime_get_ns();
    out->value_21bit = out->raw_60d4 & GENMASK(20, 0);
    out->value_14bit = out->raw_61ac & GENMASK(13, 0);
unlock:
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    mutex_unlock(&hsan_lsw_fam_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_read_activity);

static int hsan_fam_verify_raw(u32 csr, u32 value)
{
    u32 readback;
    int ret = hsan_lsw_write_raw(csr, value);

    if (ret)
        return ret;
    ret = hsan_lsw_read_raw(csr, &readback);
    return ret ? ret : (readback == value ? 0 : -EUCLEAN);
}

static int hsan_fam_update_raw(u32 csr, u32 mask, u32 value)
{
    u32 old, readback, updated;
    int ret;

    ret = hsan_lsw_read_raw(csr, &old);
    if (ret)
        return ret;
    updated = (old & ~mask) | (value & mask);
    if (updated != old) {
        ret = hsan_lsw_write_raw(csr, updated);
        if (ret)
            return ret;
    }
    ret = hsan_lsw_read_raw(csr, &readback);
    return ret ? ret : (((readback ^ updated) & mask) ? -EUCLEAN : 0);
}

int hsan_lsw_fam_commit(const struct hsan_lsw_fam_config *cfg,
                        u32 *polls, u32 *last_status)
{
    struct hsan_bank *bank;
    unsigned long flags;
    u32 status = 0, offset;
    unsigned int i;
    int ret;

    if (!cfg || !polls || !last_status)
        return -EINVAL;
    *polls = 0;
    *last_status = 0;
    if (cfg->unit14 > 0x3fff || cfg->low16 > 16320 ||
        cfg->mode > 3 || cfg->count16 > 16384 ||
        cfg->entries < 64 || cfg->entries > 131072 ||
        !IS_ALIGNED(cfg->entries, 32))
        return -ERANGE;
    if (!READ_ONCE(experimental_allow_lsw_writes))
        return -EACCES;
    mutex_lock(&hsan_lsw_fam_lock);
    mutex_lock(&hsan_lsw_eqm_lock);
    if (atomic_read(&hsan_lsw_fam_cpu_stopped)) {
        /* A new FAM arm after the PIE CPU STOP fence could revive
         * cached DMA mappings even though no HW STOP was proven.
         */
        ret = -ESHUTDOWN;
        goto out;
    }
    if (hsan_lsw_fam_poisoned) {
        ret = -EUCLEAN;
        goto out;
    }
    /* All 15 register windows and the existing status must be validated
     * BEFORE the first write. Both mapping and other FAM/EQM API calls
     * are excluded by the two outer mutexes for the entire transaction.
     */
    spin_lock_irqsave(&hsan_lsw_lock, flags);
    ret = 0;
    for (i = 0; i < ARRAY_SIZE(hsan_fam_csrs); i++) {
        ret = hsan_lsw_access_locked(hsan_fam_csrs[i], &bank, &offset);
        if (ret)
            break;
    }
    if (!ret) {
        ret = hsan_lsw_access_locked(HSAN_FAM_STATUS_CSR,
                                     &bank, &offset);
        if (!ret) {
            status = readl(bank->regs + offset);
            if (status & BIT(0))
                ret = -EBUSY; /* stale ACK: owner cannot be established */
        }
    }
    spin_unlock_irqrestore(&hsan_lsw_lock, flags);
    if (ret)
        goto out;

    hsan_lsw_fam_poisoned = true; /* no retry, even after a completed ACK */
    if (atomic_cmpxchg(&hsan_lsw_fam_module_pinned, 0, 1) == 0)
        __module_get(THIS_MODULE);
#define FAM_VERIFY(_csr, _value) do { \
    ret = hsan_fam_verify_raw((_csr), (_value)); \
    if (ret) goto out; \
} while (0)
    FAM_VERIFY(0x40000028U, cfg->memory_base_reg);
    FAM_VERIFY(0x4000001cU, 65536U);
    FAM_VERIFY(0x43000504U, cfg->unit14);
    ret = hsan_fam_update_raw(0x42000388U, GENMASK(15, 0), cfg->unit14);
    if (ret)
        goto out;
    FAM_VERIFY(0x420000a4U, cfg->low16);
    FAM_VERIFY(0x40000200U, cfg->mode);
    FAM_VERIFY(0x43000514U, cfg->dma_bus_addr);
    FAM_VERIFY(0x4300050cU, cfg->entries);
    ret = hsan_fam_update_raw(0x42000388U, GENMASK(31, 16),
                               cfg->count16 << 16);
    if (ret)
        goto out;
    FAM_VERIFY(0x43000530U, 0);
    FAM_VERIFY(0x43000534U, 0);
#undef FAM_VERIFY
    /* The start CSR is a strobe, not a readback-stable register. */
    ret = hsan_lsw_write_raw(0x43000510U, 1);
    if (ret)
        goto out;
    for (i = 0; i < HSAN_FAM_POLL_LIMIT; i++) {
        ret = hsan_lsw_read_raw(HSAN_FAM_STATUS_CSR, &status);
        if (ret)
            goto out;
        if (status & BIT(0))
            break;
        usleep_range(100, 200);
    }
    *polls = i + (i < HSAN_FAM_POLL_LIMIT);
    if (i == HSAN_FAM_POLL_LIMIT) {
        ret = -ETIMEDOUT;
        goto out;
    }
    ret = hsan_lsw_write_raw(0x40000044U, 1);
    if (ret)
        goto out;
    ret = hsan_lsw_write_raw(0x40000044U, 0);
    if (ret)
        goto out;
    ret = hsan_fam_verify_raw(0x43000098U, 1);
    if (ret)
        goto out;
    ret = hsan_fam_verify_raw(0x430000a4U, 1);
out:
    *last_status = status;
    mutex_unlock(&hsan_lsw_eqm_lock);
    mutex_unlock(&hsan_lsw_fam_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_fam_commit);

/* hi_lsw_dp: hi_eqm_write_tab @0x1d04, read_tab @0x1e1c and
 * hi_eqm_tab_ready @0x1f34. OEM writes N=min(nr_words,5) words to
 * separate staging registers, then commits to 0x42000048, polls bit31
 * for up to 1001 reads, and treats bit29 as a hardware error. The exact
 * read/write address pairs are in OEM .rodata+0x204, relocation symbol
 * index 178 at write+0x1d54/read+0x1ea8. These are NOT one FIFO address.
 * This port returns Linux errnos rather than OEM's
 * proprietary negative codes; it does not retry a failed command.
 *
 * EQM is a shared FIFO/command resource: serialize all transactions
 * across DP and future EQM clients. The per-register spinlock protects
 * the MMIO mapping against concurrent unmap; the outer mutex makes
 * multi-register transactions indivisible from other EQM API calls.
 * A previous already-committed HW command cannot be rolled back in SW.
 */
#define HSAN_EQM_COMMAND 0x42000048U
static const u32 hsan_eqm_write_words[] = {
    0x4200005cU, 0x42000060U, 0x42000064U, 0x42000068U, 0x42000070U,
};
static const u32 hsan_eqm_read_words[] = {
    0x4200004cU, 0x42000050U, 0x42000054U, 0x42000058U, 0x4200006cU,
};
static_assert(ARRAY_SIZE(hsan_eqm_write_words) == HSAN_LSW_EQM_DATA_WORDS);
static_assert(ARRAY_SIZE(hsan_eqm_read_words) == HSAN_LSW_EQM_DATA_WORDS);
#define HSAN_EQM_STATUS_BUSY BIT(31)
#define HSAN_EQM_STATUS_ERROR BIT(29)
#define HSAN_EQM_POLL_ATTEMPTS 1001U
/* A timed-out or partly issued EQM command has unknown hardware state.
 * Once poisoned, reject every subsequent transaction until module reload;
 * no automated retry can safely reconstruct the shared FIFO state.
 */
static bool hsan_lsw_eqm_poisoned;
static atomic_t hsan_lsw_eqm_module_pinned = ATOMIC_INIT(0);

static void hsan_lsw_eqm_pin_module(void)
{
    if (atomic_cmpxchg(&hsan_lsw_eqm_module_pinned, 0, 1) == 0) {
        __module_get(THIS_MODULE);
        pr_warn("hsan-lsw-csr: EQM hardware command issued; owner pinned until board reset\n");
    }
}

int hsan_lsw_eqm_health(void)
{
	int ret;

	mutex_lock(&hsan_lsw_fam_lock);
    mutex_lock(&hsan_lsw_eqm_lock);
	ret = hsan_lsw_eqm_poisoned ? -EUCLEAN : 0;
	mutex_unlock(&hsan_lsw_eqm_lock);
    mutex_unlock(&hsan_lsw_fam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_eqm_health);

static void hsan_lsw_eqm_record_failure(int ret, bool issued)
{
	if (!ret || (!issued && ret != -ETIMEDOUT && ret != -EIO &&
		     ret != -EUCLEAN))
		return;
	if (!hsan_lsw_eqm_poisoned)
		pr_err("hsan-lsw-csr: EQM hardware state uncertain after %d, command engine quarantined\n", ret);
	hsan_lsw_eqm_poisoned = true;
}

static int hsan_lsw_eqm_wait_ready(u32 *last_status)
{
    u32 status = 0;
    unsigned int i;
    int ret;

    for (i = 0; i < HSAN_EQM_POLL_ATTEMPTS; i++) {
        ret = hsan_lsw_read_raw(HSAN_EQM_COMMAND, &status);
        if (ret)
            return ret;
        if (last_status)
            *last_status = status;
        if (!(status & HSAN_EQM_STATUS_BUSY))
            return (status & HSAN_EQM_STATUS_ERROR) ? -EIO : 0;
        cpu_relax();
    }
    return -ETIMEDOUT;
}

static int hsan_lsw_eqm_preflight(bool is_write, u32 nr_words)
{
    const u32 *registers = is_write ? hsan_eqm_write_words : hsan_eqm_read_words;
    unsigned int i;
    int ret;

    /* The command write is mandatory for both read and write requests. */
    ret = hsan_lsw_check_span(HSAN_EQM_COMMAND, sizeof(u32), true);
    if (ret)
        return ret;
    for (i = 0; i < nr_words; i++) {
        ret = hsan_lsw_check_span(registers[i], sizeof(u32), is_write);
        if (ret)
            return ret;
    }
    return 0;
}

/* EQM callers may share one command engine. The locked helpers keep the
 * entire FIFO/command/readback sequence atomic against other EQM clients.
 * Each request still respects the global experimental MMIO-write gate.
 */
static int hsan_lsw_eqm_write_locked(u8 table, u16 row, const u32 *words,
                                    u32 nr_words, u32 *last_status,
                                    bool *issued)
{
    unsigned int i;
    int ret;

    ret = hsan_lsw_eqm_preflight(true, nr_words);
    if (ret)
        return ret;
    ret = hsan_lsw_eqm_wait_ready(last_status);
    if (ret)
        return ret;
    for (i = 0; i < nr_words; ++i) {
        *issued = true;
        ret = hsan_lsw_write_raw(hsan_eqm_write_words[i], words[i]);
        if (ret)
            return ret;
    }
    wmb();
    ret = hsan_lsw_write_raw(HSAN_EQM_COMMAND,
                             0xc0000000U | ((u32)table << 20) | row);
    return ret ? ret : hsan_lsw_eqm_wait_ready(last_status);
}

static int hsan_lsw_eqm_read_locked(u8 table, u16 row, u32 *words,
                                   u32 nr_words, u32 *last_status,
                                   bool *issued)
{
    unsigned int i;
    int ret;

    ret = hsan_lsw_eqm_preflight(false, nr_words);
    if (ret)
        return ret;
    ret = hsan_lsw_eqm_wait_ready(last_status);
    if (ret)
        return ret;
    *issued = true;
    ret = hsan_lsw_write_raw(HSAN_EQM_COMMAND,
                             0x80000000U | ((u32)table << 20) | row);
    if (ret)
        return ret;
    ret = hsan_lsw_eqm_wait_ready(last_status);
    if (ret)
        return ret;
    for (i = 0; i < nr_words; i++) {
        ret = hsan_lsw_read_raw(hsan_eqm_read_words[i], &words[i]);
        if (ret)
            return ret;
    }
    return 0;
}

static bool hsan_lsw_eqm_args_valid(u8 table, const u32 *words,
                                    u32 nr_words)
{
    return words && nr_words && nr_words <= HSAN_LSW_EQM_DATA_WORDS &&
           table < 128;
}

int hsan_lsw_eqm_write_table(u8 table, u16 row, const u32 *words,
                             u32 nr_words, u32 *last_status)
{
    bool issued = false;
    int ret;

    if (!hsan_lsw_eqm_args_valid(table, words, nr_words))
        return -EINVAL;
    if (last_status)
        *last_status = 0;
    mutex_lock(&hsan_lsw_fam_lock);
    mutex_lock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_eqm_poisoned) {
        ret = -EUCLEAN;
    } else {
        ret = hsan_lsw_eqm_write_locked(table, row, words, nr_words,
                                        last_status, &issued);
        hsan_lsw_eqm_record_failure(ret, issued);
    }
    mutex_unlock(&hsan_lsw_eqm_lock);
    mutex_unlock(&hsan_lsw_fam_lock);
    /* A pre-existing busy/error state is also an unqualified hardware
     * lifetime, even if this call never wrote its first FIFO word.
     */
    if (issued || ret == -EIO || ret == -ETIMEDOUT || ret == -EUCLEAN)
        hsan_lsw_eqm_pin_module();
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_eqm_write_table);

int hsan_lsw_eqm_read_table(u8 table, u16 row, u32 *words,
                            u32 nr_words, u32 *last_status)
{
    bool issued = false;
    int ret;

    if (!hsan_lsw_eqm_args_valid(table, words, nr_words))
        return -EINVAL;
    if (last_status)
        *last_status = 0;
    mutex_lock(&hsan_lsw_fam_lock);
    mutex_lock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_eqm_poisoned) {
        ret = -EUCLEAN;
    } else {
        ret = hsan_lsw_eqm_read_locked(table, row, words, nr_words,
                                       last_status, &issued);
        hsan_lsw_eqm_record_failure(ret, issued);
    }
    mutex_unlock(&hsan_lsw_eqm_lock);
    mutex_unlock(&hsan_lsw_fam_lock);
    /* A pre-existing busy/error state is also an unqualified hardware
     * lifetime, even if this call never wrote its first FIFO word.
     */
    if (issued || ret == -EIO || ret == -ETIMEDOUT || ret == -EUCLEAN)
        hsan_lsw_eqm_pin_module();
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_eqm_read_table);

/* Explicit opt-in caller only. A matching hardware readback proves one
 * EQM table row's observable contents, NOT FAM/PFE forwarding or DMA.
 * Reads/writes are kept under one EQM mutex to prevent a concurrent client
 * from modifying the same shared FIFO between commit and verification.
 */
int hsan_lsw_eqm_write_verify_table(u8 table, u16 row, const u32 *words,
                                    const u32 *expected_readback,
                                    u32 nr_words, u32 *last_status)
{
    u32 observed[HSAN_LSW_EQM_DATA_WORDS] = { 0 };
    unsigned int i;
    bool issued = false;
    int ret;

    if (!expected_readback || !hsan_lsw_eqm_args_valid(table, words, nr_words))
        return -EINVAL;
    if (last_status)
        *last_status = 0;
    mutex_lock(&hsan_lsw_fam_lock);
    mutex_lock(&hsan_lsw_eqm_lock);
    if (hsan_lsw_eqm_poisoned) {
        ret = -EUCLEAN;
        goto out;
    }
    /* Reject missing readback before committing the irreversible write. */
    ret = hsan_lsw_eqm_preflight(false, nr_words);
    if (ret)
        goto out;
    ret = hsan_lsw_eqm_write_locked(table, row, words, nr_words,
                                    last_status, &issued);
    if (ret)
        goto out;
    ret = hsan_lsw_eqm_read_locked(table, row, observed, nr_words,
                                   last_status, &issued);
    if (!ret && table == 19 && row == 0 && nr_words == 5)
        pr_info("hsan-lsw-csr: EQM table19 readback words=%08x,%08x,%08x,%08x,%08x\n",
                observed[0], observed[1], observed[2], observed[3], observed[4]);
    if (!ret) {
        for (i = 0; i < nr_words; i++) {
            if (observed[i] == expected_readback[i])
                continue;
            pr_err("hsan-lsw-csr: EQM mismatch table=%u row=%u word=%u expected=%#x observed=%#x\n",
                   table, row, i, expected_readback[i], observed[i]);
            ret = -EUCLEAN;
            break;
        }
    }
out:
    hsan_lsw_eqm_record_failure(ret, issued);
    mutex_unlock(&hsan_lsw_eqm_lock);
    mutex_unlock(&hsan_lsw_fam_lock);
    /* A pre-existing busy/error state is also an unqualified hardware
     * lifetime, even if this call never wrote its first FIFO word.
     */
    if (issued || ret == -EIO || ret == -ETIMEDOUT || ret == -EUCLEAN)
        hsan_lsw_eqm_pin_module();
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_eqm_write_verify_table);

/* OEM hi_lsw_pub hi_tab_mgr_create @0x1ea8 / delete @0x18b4.
 * This registry stages the exact audited logical ID and count/width on
 * arm64, but does NOT replace OEM handler callbacks, DFX, HW table backing
 * or the actual physical LSW table writes. In particular, the 52-byte A32
 * config contains 32-bit function addresses that are NOT portable to arm64.
 * Hold the mutex during membership changes; no MMIO or IRQ callbacks here.
 */
struct hsan_lsw_shadow_table {
	struct list_head link;
	struct device *owner;
	u16 id;
	u8 oem_flags26;
	char name[32];
	u32 entries;
	u32 entry_bytes;
	u32 reserved_rows;	/* Linux-side row ownership only; no HW table callback. */
	unsigned long *allocated;
	void *cells;
    bool sparse;
    struct xarray sparse_cells; /* NAPT rows: allocate only on first write */
};
static LIST_HEAD(hsan_lsw_shadow_tables);
static DEFINE_MUTEX(hsan_lsw_shadow_lock);

static struct hsan_lsw_shadow_table *
hsan_lsw_shadow_find_locked(u16 id)
{
	struct hsan_lsw_shadow_table *t;

	list_for_each_entry(t, &hsan_lsw_shadow_tables, link)
		if (t->id == id)
			return t;
	return NULL;
}

/* OEM NAPT can expose >500k logical rows. Its table metadata and
 * row allocation bitmap must not allocate tens of MiB of zero payload.
 * Dense tables retain their original 256 KiB limit; only sparse owners
 * can use the extended logical limit. Neither format performs HW I/O.
 */
static int hsan_lsw_shadow_table_create_internal(struct device *owner, u16 id,
             const char *name, u32 entries, u32 entry_bytes,
             u8 oem_flags26, bool sparse)
{
	struct hsan_lsw_shadow_table *table;
	size_t total;
	int ret = 0;

	if (!owner || !name || !name[0] || strnlen(name, 32) >= 32 ||
	    id >= 0x2000 || (id & 0xc0) || !entries ||
	    !entry_bytes || !IS_ALIGNED(entry_bytes, sizeof(u32)) ||
	    check_mul_overflow((size_t)entries, (size_t)entry_bytes, &total) ||
        (sparse ? (entries > SZ_1M || entry_bytes > 128 || total > SZ_128M) :
                  total > SZ_256K))
		return -EINVAL;
	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
    table->sparse = sparse;
    xa_init(&table->sparse_cells);
    if (!sparse) {
        table->cells = kvzalloc(total, GFP_KERNEL);
        if (!table->cells) {
            kfree(table);
            return -ENOMEM;
        }
    }
    /* bitmap_zalloc uses kmalloc; 525k entries needs a non-contiguous
     * 64 KiB bitmap on constrained ARM boards. kvzalloc also serves small
     * dense tables, and kvfree supports both backing allocations.
     */
    table->allocated = kvzalloc(BITS_TO_LONGS(entries) *
                               sizeof(unsigned long), GFP_KERNEL);
    if (!table->allocated) {
        kvfree(table->cells);
        kfree(table);
        return -ENOMEM;
    }
	table->owner = owner;
	table->id = id;
	table->entries = entries;
	table->entry_bytes = entry_bytes;
	table->oem_flags26 = oem_flags26;
	strscpy(table->name, name, sizeof(table->name));
	mutex_lock(&hsan_lsw_shadow_lock);
	if (hsan_lsw_shadow_find_locked(id))
		ret = -EEXIST;
	else
		list_add_tail(&table->link, &hsan_lsw_shadow_tables);
	mutex_unlock(&hsan_lsw_shadow_lock);
	if (ret) {
        kvfree(table->allocated);
        kvfree(table->cells);
        xa_destroy(&table->sparse_cells);
        kfree(table);
    }
    return ret;
}

int hsan_lsw_shadow_table_create(struct device *owner, u16 id,
                                 const char *name, u32 entries,
                                 u32 entry_bytes, u8 oem_flags26)
{
    return hsan_lsw_shadow_table_create_internal(owner, id, name, entries,
                                                   entry_bytes, oem_flags26, false);
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_create);

int hsan_lsw_shadow_table_create_sparse(struct device *owner, u16 id,
                                        const char *name, u32 entries,
                                        u32 entry_bytes, u8 oem_flags26)
{
    /* Only the two decoded OEM NAPT tables use this lifetime mode. */
    if (!((id == 0x105 && entry_bytes == 40) ||
          (id == 0x106 && entry_bytes == 64)))
        return -EINVAL;
    return hsan_lsw_shadow_table_create_internal(owner, id, name, entries,
                                                   entry_bytes, oem_flags26, true);
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_create_sparse);

int hsan_lsw_shadow_table_delete(struct device *owner, u16 id)
{
	struct hsan_lsw_shadow_table *table;
	int ret = 0;

	if (!owner)
		return -EINVAL;
	mutex_lock(&hsan_lsw_shadow_lock);
	table = hsan_lsw_shadow_find_locked(id);
	if (!table)
		ret = -ENOENT;
	else if (table->owner != owner)
		ret = -EPERM;
	else
		list_del(&table->link);
	mutex_unlock(&hsan_lsw_shadow_lock);
	if (!ret) {
		if (table->reserved_rows)
			pr_warn("hsan-lsw-csr: deleting table %#x with %u reserved rows\n",
				table->id, table->reserved_rows);
        if (table->sparse) {
            unsigned long row;
            void *cell;

            xa_for_each(&table->sparse_cells, row, cell) {
                memzero_explicit(cell, table->entry_bytes);
                kfree(cell);
            }
        }
        xa_destroy(&table->sparse_cells);
        kvfree(table->allocated);
        kvfree(table->cells);
        kfree(table);
    }
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_delete);


/* Software table-manager row ownership.  These APIs port the resource
 * reservation part of hi_tab_mgr_alloc_hash/direct to Linux, but deliberately
 * do not claim the original per-table HW callbacks or physical table writes.
 * All membership and bitmap operations are serialized with table deletion.
 */
int hsan_lsw_shadow_table_reserve_range(struct device *owner, u16 id,
                                        u32 first, u32 count)
{
    struct hsan_lsw_shadow_table *table;
    unsigned long found;
    int ret = 0;

    if (!owner || !count)
        return -EINVAL;
    mutex_lock(&hsan_lsw_shadow_lock);
    table = hsan_lsw_shadow_find_locked(id);
    if (!table)
        ret = -ENOENT;
    else if (table->owner != owner)
        ret = -EPERM;
    else if (first >= table->entries || count > table->entries - first)
        ret = -ERANGE;
    else {
        found = bitmap_find_next_zero_area(table->allocated,
                                            table->entries,
                                            first, count, 0);
        if (found != first)
            ret = -EBUSY;
        else {
            bitmap_set(table->allocated, first, count);
            table->reserved_rows += count;
        }
    }
    mutex_unlock(&hsan_lsw_shadow_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_reserve_range);

int hsan_lsw_shadow_table_alloc_range(struct device *owner, u16 id,
                                      u32 count, u32 alignment, u32 *first)
{
    struct hsan_lsw_shadow_table *table;
    unsigned long found;
    unsigned long mask;
    int ret = 0;

    if (!owner || !count || !first || !is_power_of_2(alignment))
        return -EINVAL;
    mask = alignment - 1;
    mutex_lock(&hsan_lsw_shadow_lock);
    table = hsan_lsw_shadow_find_locked(id);
    if (!table)
        ret = -ENOENT;
    else if (table->owner != owner)
        ret = -EPERM;
    else if (count > table->entries)
        ret = -ENOSPC;
    else {
        found = bitmap_find_next_zero_area(table->allocated,
                                            table->entries, 0,
                                            count, mask);
        if (found >= table->entries || count > table->entries - found)
            ret = -ENOSPC;
        else {
            bitmap_set(table->allocated, found, count);
            table->reserved_rows += count;
            *first = found;
        }
    }
    mutex_unlock(&hsan_lsw_shadow_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_alloc_range);

int hsan_lsw_shadow_table_release_range(struct device *owner, u16 id,
                                        u32 first, u32 count)
{
    struct hsan_lsw_shadow_table *table;
    unsigned long end;
    int ret = 0;

    if (!owner || !count)
        return -EINVAL;
    mutex_lock(&hsan_lsw_shadow_lock);
    table = hsan_lsw_shadow_find_locked(id);
    if (!table)
        ret = -ENOENT;
    else if (table->owner != owner)
        ret = -EPERM;
    else if (first >= table->entries || count > table->entries - first)
        ret = -ERANGE;
    else {
        end = first + count;
        if (find_next_zero_bit(table->allocated, end, first) != end)
            ret = -ENOENT;
        else {
            /* A released hardware-independent row allocation must not
             * retain the previous owner's bytes. Dense tables scrub the
             * whole range; lazy NAPT frees only allocated cells. No HW
             * table clear is implied by this CPU-only operation.
             */
            if (table->sparse) {
                unsigned long idx = first;
                void *cell;

                /* Iterate only allocated sparse payloads, not every
                 * logical NAPT row in a potentially 1M-row release.
                 * All modifications remain covered by the owner mutex.
                 */
                while ((cell = xa_find(&table->sparse_cells, &idx,
                                       end - 1, XA_PRESENT))) {
                    xa_erase(&table->sparse_cells, idx);
                    memzero_explicit(cell, table->entry_bytes);
                    kfree(cell);
                    idx++;
                }
            } else {
                memset((u8 *)table->cells +
                       (size_t)first * table->entry_bytes, 0,
                       (size_t)count * table->entry_bytes);
            }
            bitmap_clear(table->allocated, first, count);
            table->reserved_rows -= count;
        }
    }
    mutex_unlock(&hsan_lsw_shadow_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_release_range);

/* Shadow access is software-only and atomic with table deletion. Width,
 * owner and row are checked before any memcpy; no CSR/MMIO is touched.
 */
static int hsan_lsw_shadow_cell(struct device *owner, u16 id, u32 row,
                                void *data, size_t bytes, bool write)
{
    struct hsan_lsw_shadow_table *table;
    int ret = 0;

    if (!owner || !data || !bytes)
        return -EINVAL;
    mutex_lock(&hsan_lsw_shadow_lock);
    table = hsan_lsw_shadow_find_locked(id);
    if (!table)
        ret = -ENOENT;
    else if (table->owner != owner)
        ret = -EPERM;
    else if (row >= table->entries || bytes != table->entry_bytes)
        ret = -ERANGE;
    /* Sparse NAPT cells are flow owners. Reject a CPU-only write to a
     * row that was never reserved by the table allocator. A real HW flow
     * operation remains independently unimplemented and forbidden.
     */
    else if (table->sparse && write &&
             !test_bit(row, table->allocated))
        ret = -EPERM;
    else if (table->sparse) {
        void *cell = xa_load(&table->sparse_cells, row);

        if (write) {
            if (!cell) {
                cell = kmemdup(data, bytes, GFP_KERNEL);
                if (!cell)
                    ret = -ENOMEM;
                else {
                    void *stored = xa_store(&table->sparse_cells, row,
                                            cell, GFP_KERNEL);
                    if (xa_is_err(stored)) {
                        ret = xa_err(stored);
                        kfree(cell);
                    }
                }
            } else {
                memcpy(cell, data, bytes);
            }
        } else if (cell) {
            memcpy(data, cell, bytes);
        } else {
            memset(data, 0, bytes);
        }
    } else if (write) {
        memcpy((u8 *)table->cells + (size_t)row * bytes, data, bytes);
    } else {
        memcpy(data, (u8 *)table->cells + (size_t)row * bytes, bytes);
    }
    mutex_unlock(&hsan_lsw_shadow_lock);
    return ret;
}

int hsan_lsw_shadow_table_write(struct device *owner, u16 id, u32 row,
                                const void *data, size_t bytes)
{
    return hsan_lsw_shadow_cell(owner, id, row, (void *)data, bytes, true);
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_write);

int hsan_lsw_shadow_table_read(struct device *owner, u16 id, u32 row,
                               void *data, size_t bytes)
{
    return hsan_lsw_shadow_cell(owner, id, row, data, bytes, false);
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_read);

/* CPU-only allocation evidence for diagnosing NAPT row ownership. No
 * physical table or address is ever exposed and no MMIO is performed.
 * The table's owner is checked under the same mutex as deletion, row
 * reservations, writes and releases; snapshots are never stale pointers.
 */
int hsan_lsw_shadow_table_usage(struct device *owner, u16 id,
                                u32 *reserved, u32 *materialized)
{
    struct hsan_lsw_shadow_table *table;
    unsigned long idx;
    void *cell;
    int ret = 0;

    if (!owner || !reserved || !materialized)
        return -EINVAL;
    *reserved = 0;
    *materialized = 0;
    mutex_lock(&hsan_lsw_shadow_lock);
    table = hsan_lsw_shadow_find_locked(id);
    if (!table)
        ret = -ENOENT;
    else if (table->owner != owner)
        ret = -EPERM;
    else {
        *reserved = table->reserved_rows;
        if (table->sparse)
            xa_for_each(&table->sparse_cells, idx, cell)
                ++*materialized;
        else
            *materialized = table->entries;
    }
    mutex_unlock(&hsan_lsw_shadow_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(hsan_lsw_shadow_table_usage);

static atomic_t hsan_lsw_pie_boot_state = ATOMIC_INIT(0);

int hsan_lsw_pie_boot_reset_publish(u32 gro_channels)
{
	if (gro_channels > 4)
		return -EINVAL;
	return atomic_cmpxchg(&hsan_lsw_pie_boot_state, 0, BIT(8) | gro_channels) ?
		-EALREADY : 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pie_boot_reset_publish);

void hsan_lsw_pie_boot_reset_invalidate(void)
{
	atomic_set(&hsan_lsw_pie_boot_state, 0);
}
EXPORT_SYMBOL_GPL(hsan_lsw_pie_boot_reset_invalidate);

int hsan_lsw_pie_boot_geometry(u32 *gro_start, u32 *gro_count)
{
	u32 state = atomic_read(&hsan_lsw_pie_boot_state);

	if (!gro_start || !gro_count)
		return -EINVAL;
	if (!(state & BIT(8)))
		return -ENODEV;
	*gro_start = 4;
	*gro_count = state & 0xffU;
	return 0;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pie_boot_geometry);

bool hsan_lsw_pipeline_operational(void)
{
	/* Registration and clock presence alone cannot demonstrate that
	 * hi_fam_init_buf, hi_queue_init, hi_pie_map_init and the full
	 * hi_pfe_* subsystem initializers have completed.  No bypass.
	 */
	return false;
}
EXPORT_SYMBOL_GPL(hsan_lsw_pipeline_operational);

static int __init hsan_lsw_init(void)
{
	pr_info("hsan-lsw-csr: 256-bank OEM mapping; physical writes %s; pipeline not armed\n",
		experimental_allow_lsw_writes ? "EXPLICITLY ENABLED" : "disabled");
	return 0;
}
static void __exit hsan_lsw_exit(void)
{
	unsigned int i, still_mapped = 0;
	unsigned long flags;

	spin_lock_irqsave(&hsan_lsw_lock, flags);
	for (i = 0; i < ARRAY_SIZE(hsan_banks); i++)
		still_mapped += !!hsan_banks[i].owner;
	spin_unlock_irqrestore(&hsan_lsw_lock, flags);
	if (still_mapped)
		pr_warn("hsan-lsw-csr: %u banks still registered at unload\n", still_mapped);
	mutex_lock(&hsan_lsw_shadow_lock);
	if (!list_empty(&hsan_lsw_shadow_tables))
		pr_warn("hsan-lsw-csr: software table owners remain registered on unload\n");
	mutex_unlock(&hsan_lsw_shadow_lock);
}
module_init(hsan_lsw_init);
module_exit(hsan_lsw_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEWA1108V Stage49 CSR FAM IRQ-safe stop fence and irreversible IFC ownership");
