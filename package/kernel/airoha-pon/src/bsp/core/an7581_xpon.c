// SPDX-License-Identifier: GPL-2.0-only
/* AN7581 PON MAC register/IRQ provider. No clocks, resets or DMA are changed
 * by probe. Optical startup and shared FE/QDMA ownership belong to consumers.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/reset.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <an7581_xpon.h>
#include <an7581_xpon_map.h>

struct an7581_xpon {
	struct device *dev;
	void __iomem *base[AN7581_XPON_BANKS];
	int irq[2];
	bool mac_fault;
	bool resetting;
	struct reset_control *reset;
};

static DEFINE_RWLOCK(xpon_lock);
static DEFINE_MUTEX(xpon_lifecycle);
static struct an7581_xpon *xpon;

/* These mixed control/status registers require a single owner. Never allow a
 * legacy read/modify/write to race stop transactions or replay status bits.
 */
#define AN7581_XPON_STOP_REG 0x5004
#define AN7581_XPON_TX_FIFO_REG 0x5814
#define AN7581_XPON_STOP_CONTROL 0x01011111U
#define AN7581_XPON_STOP_SUPPORTED 0x01010101U
#define AN7581_XPON_STOP_POLLS 3000

int an7581_xpon_mac_stop(u32 controls, bool hold)
{
	unsigned long flags;
	u32 value, desired, done = 0;
	unsigned int retry;
	int ret = -ENODEV;

	if (!controls || (controls & ~AN7581_XPON_STOP_SUPPORTED))
		return -EINVAL;
	if (controls & AN7581_XPON_MBI_RX_STOP)
		done |= 1U << 14;
	if (controls & AN7581_XPON_MBI_TX_STOP)
		done |= 1U << 15;
	if (controls & AN7581_XPON_MPI_RX_STOP)
		done |= 1U << 30;
	if (controls & AN7581_XPON_MPI_TX_STOP)
		done |= 1U << 31;

	/* Callers include protocol IRQ/tasklet paths. Serialize the complete
	 * bounded transaction, including other legacy register accesses/removal.
	 */
	write_lock_irqsave(&xpon_lock, flags);
	if (!xpon)
		goto out;
	if (xpon->resetting) {
		ret = -EBUSY;
		goto out;
	}
	if (xpon->mac_fault && !hold) {
		ret = -EIO;
		goto out;
	}
	value = readl(xpon->base[1] + (AN7581_XPON_STOP_REG & 0xfff));
	if (value == ~0U) {
		ret = -EIO;
		goto fault;
	}
	desired = value & AN7581_XPON_STOP_CONTROL;
	if (hold)
		desired |= controls;
	else
		desired &= ~controls;
	writel(desired, xpon->base[1] + (AN7581_XPON_STOP_REG & 0xfff));
	ret = -ETIMEDOUT;
	for (retry = 0; retry < AN7581_XPON_STOP_POLLS; retry++) {
		value = readl(xpon->base[1] + (AN7581_XPON_STOP_REG & 0xfff));
		if (value == ~0U ||
		    (value & AN7581_XPON_STOP_CONTROL) != desired) {
			ret = -EIO;
			break;
		}
		if (!hold || (value & done) == done) {
			ret = xpon->mac_fault ? -EIO : 0;
			goto out;
		}
		udelay(1);
	}
fault:
	xpon->mac_fault = true;
out:
	write_unlock_irqrestore(&xpon_lock, flags);
	return ret;
}
EXPORT_SYMBOL(an7581_xpon_mac_stop);

int an7581_xpon_mac_wait_tx_empty(void)
{
	unsigned long flags;
	u32 stop, value;
	unsigned int retry;
	int ret = -ENODEV;

	write_lock_irqsave(&xpon_lock, flags);
	if (!xpon)
		goto out;
	if (xpon->resetting) {
		ret = -EBUSY;
		goto out;
	}
	ret = -EIO;
	if (xpon->mac_fault)
		goto out;
	stop = readl(xpon->base[1] + (AN7581_XPON_STOP_REG & 0xfff));
	if (stop == ~0U)
		goto fault;
	/* Stop ingress from FE, but leave the optical side able to drain. */
	ret = -EBUSY;
	if (!(stop & AN7581_XPON_MBI_TX_STOP) || !(stop & (1U << 15)))
		goto out;
	ret = -ETIMEDOUT;
	for (retry = 0; retry < AN7581_XPON_STOP_POLLS; retry++) {
		value = readl(xpon->base[1] + (AN7581_XPON_TX_FIFO_REG & 0xfff));
		if (value == ~0U) {
			ret = -EIO;
			goto fault;
		}
		if (!(value & 0xffff)) {
			ret = 0;
			goto out;
		}
		/* An already stopped transmitter may be empty at startup. If it
		 * contains data, never reopen it implicitly to force a drain.
		 */
		if (stop & AN7581_XPON_MPI_TX_STOP) {
			ret = -EBUSY;
			goto out;
		}
		udelay(1);
	}
fault:
	xpon->mac_fault = true;
out:
	write_unlock_irqrestore(&xpon_lock, flags);
	return ret;
}
EXPORT_SYMBOL(an7581_xpon_mac_wait_tx_empty);

u32 get_xpon_data(u32 reg)
{
	unsigned long flags;
	unsigned int offset;
	u32 value = ~0U;
	int bank = an7581_xpon_decode(reg, &offset);
	bool ready;

	if (bank < 0) {
		pr_err_ratelimited("an7581-xpon: invalid register read %#x\n", reg);
		return value;
	}
	read_lock_irqsave(&xpon_lock, flags);
	ready = xpon && !xpon->resetting;
	if (ready)
		value = readl(xpon->base[bank] + offset);
	read_unlock_irqrestore(&xpon_lock, flags);
	if (!ready)
		pr_err_ratelimited("an7581-xpon: register read without provider\n");
	return value;
}
EXPORT_SYMBOL(get_xpon_data);

void set_xpon_data(u32 reg, u32 value)
{
	unsigned long flags;
	unsigned int offset;
	int bank = an7581_xpon_decode(reg, &offset);
	bool ready;

	if (bank < 0 || reg == AN7581_XPON_STOP_REG) {
		pr_err_ratelimited("an7581-xpon: invalid register write %#x\n", reg);
		return;
	}
	read_lock_irqsave(&xpon_lock, flags);
	ready = xpon && !xpon->resetting;
	if (ready)
		writel(value, xpon->base[bank] + offset);
	read_unlock_irqrestore(&xpon_lock, flags);
	if (!ready)
		pr_err_ratelimited("an7581-xpon: register write without provider\n");
}
EXPORT_SYMBOL(set_xpon_data);

int get_xpon_irq(int index)
{
	unsigned long flags;
	int irq;

	if (index < 0 || index >= 2)
		return -EINVAL;
	read_lock_irqsave(&xpon_lock, flags);
	irq = xpon ? xpon->irq[index] : -ENODEV;
	read_unlock_irqrestore(&xpon_lock, flags);
	return irq;
}
EXPORT_SYMBOL(get_xpon_irq);

struct device *get_xpon_dev(void)
{
	unsigned long flags;
	struct device *dev;

	read_lock_irqsave(&xpon_lock, flags);
	dev = xpon ? xpon->dev : NULL;
	read_unlock_irqrestore(&xpon_lock, flags);
	return dev;
}
EXPORT_SYMBOL(get_xpon_dev);

int an7581_xpon_status(void)
{
	unsigned long flags;
	int ret;

	read_lock_irqsave(&xpon_lock, flags);
	ret = xpon ? xpon->resetting ? -EBUSY : xpon->mac_fault ? -EIO : 0 : -ENODEV;
	read_unlock_irqrestore(&xpon_lock, flags);
	return ret;
}
EXPORT_SYMBOL(an7581_xpon_status);

void an7581_xpon_invalidate(void)
{
	unsigned long flags;

	write_lock_irqsave(&xpon_lock, flags);
	if (xpon)
		xpon->mac_fault = true;
	write_unlock_irqrestore(&xpon_lock, flags);
}
EXPORT_SYMBOL(an7581_xpon_invalidate);

int an7581_xpon_reset(void)
{
	struct an7581_xpon *priv;
	unsigned long flags;
	int ret = -ENODEV;

	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	mutex_lock(&xpon_lifecycle);
	write_lock_irqsave(&xpon_lock, flags);
	priv = xpon;
	if (priv) {
		ret = priv->mac_fault ? -EIO : 0;
		if (!ret)
			priv->resetting = true;
	}
	write_unlock_irqrestore(&xpon_lock, flags);
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
	write_lock_irqsave(&xpon_lock, flags);
	if (ret && !priv->mac_fault)
		priv->mac_fault = true;
	priv->resetting = false;
	write_unlock_irqrestore(&xpon_lock, flags);
out:
	mutex_unlock(&xpon_lifecycle);
	return ret;
}
EXPORT_SYMBOL(an7581_xpon_reset);

static int an7581_xpon_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "gpon", "xgpon", "epon" };
	static const char * const irqs[] = { "mac", "dying-gasp" };
	struct an7581_xpon *priv;
	unsigned long flags;
	int i, ret = 0;

	if (!of_machine_is_compatible("quantum,q1000k-ubi"))
		return -ENODEV;
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;
	for (i = 0; i < AN7581_XPON_BANKS; i++) {
		struct resource *res = platform_get_resource_byname(pdev,
						IORESOURCE_MEM, names[i]);

		if (!res || res->start != 0x1fb64000 + i * 0x1000 ||
		    resource_size(res) != an7581_xpon_sizes[i])
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid %s register window\n", names[i]);
		priv->base[i] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->base[i]))
			return PTR_ERR(priv->base[i]);
	}
	for (i = 0; i < ARRAY_SIZE(irqs); i++) {
		priv->irq[i] = platform_get_irq_byname(pdev, irqs[i]);
		if (priv->irq[i] < 0)
			return priv->irq[i];
	}
	priv->reset = devm_reset_control_get_exclusive(&pdev->dev, "mac");
	if (IS_ERR(priv->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->reset),
				     "cannot acquire PON MAC reset\n");
	/* Do not publish partially initialized resources after probe failure. */
	mutex_lock(&xpon_lifecycle);
	write_lock_irqsave(&xpon_lock, flags);
	if (xpon)
		ret = -EBUSY;
	else
		xpon = priv;
	write_unlock_irqrestore(&xpon_lock, flags);
	mutex_unlock(&xpon_lifecycle);
	return ret;
}

static void an7581_xpon_remove(struct platform_device *pdev)
{
	unsigned long flags;

	/* Wait for reset and register accesses before releasing mappings. */
	mutex_lock(&xpon_lifecycle);
	write_lock_irqsave(&xpon_lock, flags);
	xpon = NULL;
	write_unlock_irqrestore(&xpon_lock, flags);
	mutex_unlock(&xpon_lifecycle);
}

static const struct of_device_id an7581_xpon_match[] = {
	{ .compatible = "quantum,q1000k-pon-mac" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_xpon_match);

static struct platform_driver an7581_xpon_driver = {
	.probe = an7581_xpon_probe,
	.remove = an7581_xpon_remove,
	.driver = {
		.name = "an7581-xpon",
		.of_match_table = an7581_xpon_match,
		/* Legacy consumers retain a borrowed device pointer. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(an7581_xpon_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Q1000K AN7581 PON MAC resource provider");
