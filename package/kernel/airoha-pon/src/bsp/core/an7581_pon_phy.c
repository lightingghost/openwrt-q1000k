// SPDX-License-Identifier: GPL-2.0-only
/* Q1000K internal optical PHY resources. Probe performs no hardware writes. */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/rcupdate.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <an7581_pon_phy.h>
#include <ecnt_pon_phy_api.h>
#include <ecnt_hook/ecnt_hook_pon_phy.h>

struct an7581_pon_phy {
	struct device *dev;
	void __iomem *base[3];
	struct reset_control *reset;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins;
	int irq;
	int fault;
	bool resetting;
};

static const u32 phy_address[] = { 0x1faf0000, 0x1fa8a000, 0x1fa8b000 };
static const u32 phy_size[] = { 0x1fff, 0x1000, 0x1000 };
static DEFINE_RWLOCK(phy_lock);
static DEFINE_MUTEX(phy_lifecycle);
static struct an7581_pon_phy *pon_phy;
static void an7581_phy_fault(int error);

static int an7581_phy_decode(u32 reg, u32 *offset)
{
	unsigned int bank;

	if ((reg & 0xe0000000) == 0xa0000000)
		reg &= 0x1fffffff;
	if (reg & 3)
		return -EINVAL;
	for (bank = 0; bank < ARRAY_SIZE(phy_address); bank++) {
		if (reg >= phy_address[bank] &&
		    reg - phy_address[bank] <= phy_size[bank] - sizeof(u32)) {
			*offset = reg - phy_address[bank];
			return bank;
		}
	}
	/* In particular, never map copper XFI, FPGA or the shared SCU here. */
	return -EINVAL;
}

int an7581_pon_phy_read(u32 reg, u32 *value)
{
	unsigned long flags;
	u32 offset;
	int bank = an7581_phy_decode(reg, &offset), ret = -ENODEV;

	if (!value || bank < 0)
		return -EINVAL;
	read_lock_irqsave(&phy_lock, flags);
	if (pon_phy) {
		ret = -EBUSY;
		if (!pon_phy->resetting) {
			*value = readl(pon_phy->base[bank] + offset);
			ret = 0;
		}
	}
	read_unlock_irqrestore(&phy_lock, flags);
	return ret;
}
EXPORT_SYMBOL(an7581_pon_phy_read);

/* All writers share one lock, including masked updates and legacy callers.
 * Full writes must not read first: some PHY registers acknowledge W1C bits.
 */
static int an7581_phy_write(u32 reg, u32 mask, u32 value, bool masked)
{
	unsigned long flags;
	u32 offset;
	int bank = an7581_phy_decode(reg, &offset), ret = -ENODEV;

	if (bank < 0)
		return bank;
	write_lock_irqsave(&phy_lock, flags);
	if (pon_phy && pon_phy->resetting)
		ret = -EBUSY;
	else if (pon_phy) {
		if (masked)
			value = (readl(pon_phy->base[bank] + offset) & ~mask) |
				(value & mask);
		writel(value, pon_phy->base[bank] + offset);
		ret = 0;
	}
	write_unlock_irqrestore(&phy_lock, flags);
	return ret;
}

int an7581_pon_phy_write(u32 reg, u32 value)
{
	return an7581_phy_write(reg, ~0U, value, false);
}
EXPORT_SYMBOL(an7581_pon_phy_write);

int an7581_pon_phy_update(u32 reg, u32 end, u32 start, u32 value)
{
	u32 mask;
	int ret;

	if (end >= 32 || start > end) {
		ret = -EINVAL;
		goto out;
	}
	mask = (~0U >> (31 - end)) & (~0U << start);
	if (value & ~(mask >> start)) {
		ret = -ERANGE;
		goto out;
	}
	ret = an7581_phy_write(reg, mask, value << start, mask != ~0U);
out:
	an7581_phy_fault(ret);
	return ret;
}
EXPORT_SYMBOL(an7581_pon_phy_update);

static void an7581_phy_fault(int error)
{
	unsigned long flags;

	if (!error)
		return;
	write_lock_irqsave(&phy_lock, flags);
	if (pon_phy && !pon_phy->fault)
		pon_phy->fault = error;
	write_unlock_irqrestore(&phy_lock, flags);
	pr_err_ratelimited("an7581-pon-phy: register access failed: %d\n", error);
}

int an7581_pon_phy_status(void)
{
	unsigned long flags;
	int ret;

	read_lock_irqsave(&phy_lock, flags);
	ret = pon_phy ? pon_phy->resetting ? -EBUSY : pon_phy->fault : -ENODEV;
	read_unlock_irqrestore(&phy_lock, flags);
	return ret;
}
EXPORT_SYMBOL(an7581_pon_phy_status);

u32 get_pon_phy_data(u32 reg)
{
	u32 value = ~0U;

	an7581_phy_fault(an7581_pon_phy_read(reg, &value));
	return value;
}
EXPORT_SYMBOL(get_pon_phy_data);

void set_pon_phy_data(u32 reg, u32 value)
{
	an7581_phy_fault(an7581_pon_phy_write(reg, value));
}
EXPORT_SYMBOL(set_pon_phy_data);

struct device *get_pon_phy_dev(void)
{
	unsigned long flags;
	struct device *dev;

	read_lock_irqsave(&phy_lock, flags);
	dev = pon_phy ? pon_phy->dev : NULL;
	read_unlock_irqrestore(&phy_lock, flags);
	return dev;
}
EXPORT_SYMBOL(get_pon_phy_dev);

int get_pon_phy_irq(void)
{
	unsigned long flags;
	int irq;

	read_lock_irqsave(&phy_lock, flags);
	irq = pon_phy ? pon_phy->irq : -ENODEV;
	read_unlock_irqrestore(&phy_lock, flags);
	return irq;
}
EXPORT_SYMBOL(get_pon_phy_irq);

int an7581_pon_phy_reset(void)
{
	struct an7581_pon_phy *priv;
	unsigned long flags;
	int ret = -ENODEV;

	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	mutex_lock(&phy_lifecycle);
	write_lock_irqsave(&phy_lock, flags);
	priv = pon_phy;
	if (priv) {
		ret = priv->fault;
		if (!ret)
			priv->resetting = true;
	}
	write_unlock_irqrestore(&phy_lock, flags);
	if (ret)
		goto out;

	/* Reset-controller calls may sleep. No IRQ-disabled lock is held, and
	 * the lifecycle mutex pins the provider while MMIO access is excluded.
	 */
	ret = reset_control_assert(priv->reset);
	if (!ret) {
		ret = reset_control_status(priv->reset);
		ret = ret < 0 ? ret : ret == 1 ? 0 : -EIO;
	}
	if (!ret) {
		udelay(1);
		ret = reset_control_deassert(priv->reset);
	}
	if (!ret) {
		ret = reset_control_status(priv->reset);
		ret = ret < 0 ? ret : ret == 0 ? 0 : -EIO;
	}
	if (ret)
		reset_control_assert(priv->reset); /* Containment, never clear error. */
	write_lock_irqsave(&phy_lock, flags);
	if (ret && !priv->fault)
		priv->fault = ret;
	priv->resetting = false;
	write_unlock_irqrestore(&phy_lock, flags);
out:
	mutex_unlock(&phy_lifecycle);
	return ret;
}
EXPORT_SYMBOL(an7581_pon_phy_reset);

int an7581_pon_phy_prepare_pins(void)
{
	int ret = -ENODEV;

	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	mutex_lock(&phy_lifecycle);
	if (pon_phy) {
		ret = an7581_pon_phy_status();
		if (!ret)
			ret = pinctrl_select_state(pon_phy->pinctrl, pon_phy->pins);
	}
	an7581_phy_fault(ret);
	mutex_unlock(&phy_lifecycle);
	return ret;
}
EXPORT_SYMBOL(an7581_pon_phy_prepare_pins);

void (*ledTurnOff_hook)(u8 led_no);
EXPORT_SYMBOL(ledTurnOff_hook);
void (*set_pon_phy_mode_config)(Xpon_Phy_Mode_t mode, int tx_enable);
EXPORT_SYMBOL(set_pon_phy_mode_config);
void (*set_pon_phy_start)(void);
EXPORT_SYMBOL(set_pon_phy_start);
void (*set_pon_phy_stop)(void);
EXPORT_SYMBOL(set_pon_phy_stop);
void (*get_pon_phy_trans_status)(PHY_Trans_Status_t *status);
EXPORT_SYMBOL(get_pon_phy_trans_status);

static int an7581_pon_phy_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "digital", "ana", "pma" };
	struct an7581_pon_phy *priv;
	unsigned long flags;
	int bank, ret = 0;

	if (!of_machine_is_compatible("quantum,q1000k-ubi"))
		return -ENODEV;
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;
	priv->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(priv->pinctrl))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->pinctrl),
				     "cannot acquire PON pins\n");
	priv->pins = pinctrl_lookup_state(priv->pinctrl, "pon");
	if (IS_ERR(priv->pins))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->pins),
				     "missing PON pin state\n");
	for (bank = 0; bank < ARRAY_SIZE(names); bank++) {
		struct resource *res = platform_get_resource_byname(pdev,
						IORESOURCE_MEM, names[bank]);

		if (!res || res->start != phy_address[bank] ||
		    resource_size(res) != phy_size[bank])
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid %s PHY window\n", names[bank]);
		priv->base[bank] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->base[bank]))
			return PTR_ERR(priv->base[bank]);
	}
	/* OEM get_pon_phy_irq() obtains SerDes-common IRQ 0, GIC SPI 43.
	 * This is distinct from MAC SPI 42 and Ethernet PCS SPI 66.
	 */
	priv->irq = platform_get_irq_byname(pdev, "phy");
	if (priv->irq < 0)
		return priv->irq;
	priv->reset = devm_reset_control_get_exclusive(&pdev->dev, "phy");
	if (IS_ERR(priv->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->reset),
				     "cannot acquire optical PHY reset\n");
	mutex_lock(&phy_lifecycle);
	write_lock_irqsave(&phy_lock, flags);
	if (pon_phy)
		ret = -EBUSY;
	else
		pon_phy = priv;
	write_unlock_irqrestore(&phy_lock, flags);
	mutex_unlock(&phy_lifecycle);
	return ret;
}

static void an7581_pon_phy_remove(struct platform_device *pdev)
{
	unsigned long flags;

	mutex_lock(&phy_lifecycle);
	write_lock_irqsave(&phy_lock, flags);
	pon_phy = NULL;
	write_unlock_irqrestore(&phy_lock, flags);
	mutex_unlock(&phy_lifecycle);
}

static const struct of_device_id an7581_pon_phy_match[] = {
	{ .compatible = "quantum,q1000k-pon-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_pon_phy_match);

static struct platform_driver an7581_pon_phy_driver = {
	.probe = an7581_pon_phy_probe,
	.remove = an7581_pon_phy_remove,
	.driver = {
		.name = "an7581-pon-phy",
		.of_match_table = an7581_pon_phy_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(an7581_pon_phy_driver);
