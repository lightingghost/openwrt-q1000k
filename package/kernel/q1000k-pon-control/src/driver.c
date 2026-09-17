// SPDX-License-Identifier: GPL-2.0-only
#include <crypto/hash.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/rcupdate.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "en7573.h"
#include "q1000k_pon.h"
#include "q1000k_pon_policy.h"

static bool validation_tx;
module_param(validation_tx, bool, 0400);
MODULE_PARM_DESC(validation_tx, "Activation-validation DT only: permit normal optical TX in this controller lifetime");

/* Immutable experiment selection; every nonzero option requires the bench
 * DT's hard TX inhibit before the controller can bind or touch hardware.
 */
static bool bench_md32_a0;
module_param(bench_md32_a0, bool, 0400);
MODULE_PARM_DESC(bench_md32_a0, "TX-inhibited bench: OEM A0 MD32 transport");
static unsigned int bench_rx_output;
module_param(bench_rx_output, uint, 0400);
MODULE_PARM_DESC(bench_rx_output, "TX-inhibited RX output: 0 unchanged, 1 400mV flat, 2 600mV flat, 3 600mV 2dB");

struct q1000k_pon {
	struct i2c_client *client;
	struct gpio_desc *power[2], *los[2]; /* GPON, XGS-PON */
	struct gpio_desc *board_tx_disable; /* NAND OEM GPIO38; optional outside bench */
	struct gpio_descs *select;
	struct en7573_io io;
	struct en7573_rx_output rx_output_original;
	struct en7573_oem_post oem_post_original;
	struct mutex lock;
	struct kref ref;
	bool dead, leased, tx_enabled, tx_inhibited;
	bool activation_bench, receiver_startup;
	int fault;
	u8 calibration[513];
	bool calibration_valid, initialized;
	int mode; /* -1 off, 0 GPON, 1 XGS-PON */
	int detected[2]; /* -1 not checked, 0 absent/wrong ID, 1 expected ID */
	u16 id[2];
	int last_error;
	const char *stage;
	u64 checked_at;
};

static int pon_read(void *ctx, u8 device, u16 reg, u8 *data, size_t len)
{
	struct q1000k_pon *pon = ctx;
	u8 address[] = { reg >> 8, reg };
	struct i2c_msg messages[] = {
		{ .addr = device, .len = 2, .buf = address },
		{ .addr = device, .flags = I2C_M_RD, .len = len, .buf = data },
	};
	int ret = i2c_transfer(pon->client->adapter, messages, ARRAY_SIZE(messages));
	if (ret != ARRAY_SIZE(messages))
		dev_err_ratelimited(&pon->client->dev,
			"I2C read failed: device=%#x reg=%#x bytes=%zu messages=%d expected=2 error=%d\n",
			device, reg, len, ret, ret < 0 ? ret : -EIO);
	return ret < 0 ? ret : ret == ARRAY_SIZE(messages) ? 0 : -EIO;
}

static int pon_write(void *ctx, u8 device, u16 reg, const u8 *data, size_t len)
{
	struct q1000k_pon *pon = ctx;
	u8 buffer[6] = { reg >> 8, reg };
	struct i2c_msg message = { .addr = device, .len = len + 2, .buf = buffer };
	int ret;
	if (len > 4)
		return -EINVAL;
	memcpy(buffer + 2, data, len);
	ret = i2c_transfer(pon->client->adapter, &message, 1);
	if (ret != 1)
		dev_err_ratelimited(&pon->client->dev,
			"I2C write failed: device=%#x reg=%#x bytes=%zu messages=%d expected=1 error=%d\n",
			device, reg, len, ret, ret < 0 ? ret : -EIO);
	return ret < 0 ? ret : ret == 1 ? 0 : -EIO;
}

static void pon_control_diagnostic(void *ctx, const char *stage, u8 device,
				   u16 reg, int error, u32 actual,
				   u32 expected, u32 mask)
{
	struct q1000k_pon *pon = ctx;

	if (!mask)
		dev_err_ratelimited(&pon->client->dev,
			"controller control failure: stage=%s device=%#x reg=%#x error=%d\n",
			stage, device, reg, error);
	else if (actual == ~0U)
		dev_err_ratelimited(&pon->client->dev,
			"controller control failure: stage=%s device=%#x reg=%#x error=%d actual=%#x validation=not-all-ones\n",
			stage, device, reg, error, actual);
	else
		dev_err_ratelimited(&pon->client->dev,
			"controller control failure: stage=%s device=%#x reg=%#x error=%d actual=%#x expected=%#x mask=%#x\n",
			stage, device, reg, error, actual, expected, mask);
}

static void pon_delay(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static int pon_board_gate(struct q1000k_pon *pon, bool disable)
{
	int ret;
	if (!pon->board_tx_disable) return 0;
	ret = gpiod_set_value_cansleep(pon->board_tx_disable, disable);
	if (ret) return ret;
	ret = gpiod_get_value_cansleep(pon->board_tx_disable);
	return ret < 0 ? ret : ret == disable ? 0 : -EIO;
}

static int pon_off(struct q1000k_pon *pon)
{
	int first, second, restore = 0, board = pon_board_gate(pon, true);

	/* Restoration requires TX off, including removal during active service.
	 * Power removal still runs if disable or restoration fails.
	 */
	if (pon->initialized && pon->tx_enabled &&
	    (pon->oem_post_original.saved || pon->rx_output_original.saved)) {
		restore = en7573_set_tx(&pon->io, false);
		if (!restore)
			pon->tx_enabled = false;
	}
	if (!restore && pon->oem_post_original.saved)
		restore = en7573_oem_post_init(&pon->io, &pon->oem_post_original, true);
	if (!restore && pon->rx_output_original.saved)
		restore = en7573_restore_rx_output(&pon->io, &pon->rx_output_original);
	first = gpiod_set_value_cansleep(pon->power[0], 0);
	second = gpiod_set_value_cansleep(pon->power[1], 0);
	/* Power removal contains even an I2C restoration failure. Preserve the
	 * error so the collector cannot claim verified register restoration.
	 */
	if (!first && !second)
		pon->rx_output_original.saved = false;
	if (!first && !second)
		pon->oem_post_original.saved = false;
	pon->initialized = false;
	pon->tx_enabled = false;
	pon->mode = first || second ? -2 : -1;
	return board ? board : restore ? restore : first ? first : second;
}

static int pon_select(struct q1000k_pon *pon, int mode)
{
	int ret, i;
	ret = pon_off(pon);
	if (ret)
		return ret;
	/* Both controllers are off while the paired selectors change. */
	for (i = 0; i < 2; i++) {
		ret = gpiod_set_value_cansleep(pon->select->desc[i], mode);
		if (ret)
			return ret;
	}
	ret = gpiod_set_value_cansleep(pon->power[mode], 1);
	if (ret) {
		pon->mode = -2;
		return ret;
	}
	pon->mode = mode;
	msleep(1000);
	ret = en7573_identify(&pon->io, &pon->id[mode]);
	pon->detected[mode] = !ret;
	pon->checked_at = ktime_get_boottime_seconds();
	return ret;
}

static int verify_firmware(struct device *dev, const char *name, size_t size,
			   const char *sha256, const struct firmware **firmware)
{
	struct crypto_shash *tfm;
	u8 expected[32], digest[32];
	int ret = request_firmware_direct(firmware, name, dev);
	if (ret)
		return ret;
	if ((*firmware)->size != size || hex2bin(expected, sha256, sizeof(expected)))
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	{
		SHASH_DESC_ON_STACK(desc, tfm);
		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, (*firmware)->data, size, digest);
		shash_desc_zero(desc);
	}
	crypto_free_shash(tfm);
	return ret ? ret : memcmp(expected, digest, sizeof(digest)) ? -EBADMSG : 0;
}

static int pon_initialize(struct q1000k_pon *pon)
{
	const struct firmware *pm = NULL, *dm = NULL;
	int ret;
	pon->stage = "inputs";
	if ((pon->io.md32_a0 || bench_rx_output) && !pon->tx_inhibited)
		return -EACCES;
	if (!pon->calibration_valid)
		return -ENODATA;
	/* Verify the complete input pair before changing GPIOs or chip state. */
	ret = verify_firmware(&pon->client->dev, "airoha/q1000k/A60993.elf.pm", 15232,
		"5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1", &pm);
	if (ret)
		goto out;
	ret = verify_firmware(&pon->client->dev, "airoha/q1000k/A60993.elf.dm", 56,
		"21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4", &dm);
	if (ret)
		goto out;
	pon->stage = "select-xgspon";
	ret = pon_select(pon, 1);
	if (ret)
		goto out;
	pon->stage = "memory";
	ret = en7573_load(&pon->io, pm->data, pm->size, dm->data, dm->size,
			    pon->calibration);
	if (ret)
		goto out;
	pon->stage = "mcu-start";
	ret = en7573_start_tx_disabled(&pon->io);
	if (!ret && bench_rx_output) {
		pon->stage = "rx-output";
		ret = en7573_apply_rx_output(&pon->io, bench_rx_output,
					    &pon->rx_output_original);
	}
	if (!ret)
		pon->initialized = true;
	if (!ret)
		pon->stage = "initialized";
out:
	release_firmware(dm);
	release_firmware(pm);
	return ret;
}

/* Kernel consumer lifecycle: all hardware operations hold pon->lock. */
static DEFINE_MUTEX(pon_registry_lock);
static struct q1000k_pon *pon_registered;

static int pon_context(void)
{
	return in_interrupt() || in_atomic() || irqs_disabled() ||
		rcu_preempt_depth() ||
		(IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()) ?
		-EWOULDBLOCK : 0;
}

static void pon_free(struct kref *ref)
{
	struct q1000k_pon *pon = container_of(ref, struct q1000k_pon, ref);

	memzero_explicit(pon->calibration, sizeof(pon->calibration));
	kfree(pon);
}

static void pon_drop_device_ref(void *data)
{
	struct q1000k_pon *pon = data;

	kref_put(&pon->ref, pon_free);
}

static int pon_check_locked(struct q1000k_pon *pon)
{
	struct en7573_state state;
	int ret;

	if (pon->dead)
		return -ENODEV;
	if (pon->fault)
		return pon->fault;
	if (!pon->initialized || pon->mode != 1)
		return -EAGAIN;
	ret = en7573_sample_state(&pon->io, &state);
	if (!ret && (!state.md32_enabled || state.tx_disabled == pon->tx_enabled)) {
		dev_err_ratelimited(&pon->client->dev,
			"controller health mismatch: md32_enabled=%d expected=1 tx_disabled=%d expected=%d\n",
			state.md32_enabled, state.tx_disabled, !pon->tx_enabled);
		ret = -EIO;
	}
	return ret;
}

static int pon_contain(struct q1000k_pon *pon, int error)
{
	int ret;

	if (!error)
		return 0;
	if (!pon->fault)
		pon->fault = error;
	ret = pon_off(pon);
	if (ret)
		dev_err(&pon->client->dev, "controller containment failed: %d\n", ret);
	pon->last_error = error;
	pon->stage = "fault";
	return error;
}

struct q1000k_pon *q1000k_pon_get(void)
{
	struct q1000k_pon *pon;
	int ret = pon_context();

	if (ret)
		return ERR_PTR(ret);
	mutex_lock(&pon_registry_lock);
	pon = pon_registered;
	if (!pon) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&pon->lock);
	ret = pon->leased ? -EBUSY : pon_check_locked(pon);
	if (!ret && pon->tx_enabled)
		ret = -EBUSY;
	if (!ret) {
		kref_get(&pon->ref);
		pon->leased = true;
	}
	mutex_unlock(&pon->lock);
out:
	mutex_unlock(&pon_registry_lock);
	return ret ? ERR_PTR(ret) : pon;
}
EXPORT_SYMBOL_GPL(q1000k_pon_get);

int q1000k_pon_check(struct q1000k_pon *pon)
{
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon))
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (ret && !pon->dead && pon->leased && ret != -EAGAIN)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_check);

int q1000k_pon_oem_post_init(struct q1000k_pon *pon, bool restore)
{
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon))
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret && (!pon->tx_inhibited || pon->tx_enabled))
		ret = -EACCES;
	if (!ret)
		ret = en7573_oem_post_init(&pon->io, &pon->oem_post_original, restore);
	if (ret && !pon->dead && pon->leased && ret != -EAGAIN)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_oem_post_init);

int q1000k_pon_receiver_startup(struct q1000k_pon *pon)
{
	u32 value;
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon))
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret && pon->receiver_startup) {
		if (pon->tx_enabled)
			ret = -EACCES;
		else if (!pon->oem_post_original.saved)
			ret = en7573_oem_post_init(&pon->io, &pon->oem_post_original, false);
		/* A normal MAC/PHY reactivation preserves the controller lifetime.
		 * Verify its owned bit rather than replacing the original snapshot.
		 */
		if (!ret)
			ret = en7573_read_control(&pon->io, 0x110, &value);
		if (!ret && (value == ~0U || !(value & 0x100)))
			ret = -EIO;
	}
	if (ret && !pon->dead && pon->leased && ret != -EAGAIN)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_receiver_startup);

int q1000k_pon_bench_reinitialize(struct q1000k_pon *pon)
{
	int ret = pon_context();
	if (ret) return ret;
	if (IS_ERR_OR_NULL(pon)) return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret && (!pon->activation_bench || !pon->tx_inhibited || pon->tx_enabled))
		ret = -EACCES;
	if (!ret) ret = pon_initialize(pon);
	if (!ret) ret = pon_check_locked(pon);
	if (ret && !pon->dead && pon->leased && ret != -EAGAIN)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_bench_reinitialize);

int q1000k_pon_get_tx(struct q1000k_pon *pon, bool *enabled)
{
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon) || !enabled)
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret)
		*enabled = pon->tx_enabled;
	else if (!pon->dead && pon->leased && ret != -EAGAIN)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_get_tx);

int q1000k_pon_get_tx_inhibit(struct q1000k_pon *pon, bool *inhibited)
{
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon) || !inhibited)
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret)
		*inhibited = pon->tx_inhibited;
	else if (!pon->dead && pon->leased && ret != -EAGAIN)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_get_tx_inhibit);

int q1000k_pon_get_rx_power(struct q1000k_pon *pon, u32 *nanowatts)
{
	u32 value;
	int sensor, ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon) || !nanowatts)
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret) {
		sensor = en7573_rx_power(&pon->io, &value);
		/* Also verify health when the sensor is not ready. */
		ret = pon_check_locked(pon);
		if (!ret)
			ret = sensor;
	}
	if (!ret)
		*nanowatts = value;
	else if (!pon->dead && pon->leased && ret != -EAGAIN && ret != -ENODATA)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_get_rx_power);

int q1000k_pon_tx_recipe(struct q1000k_pon *pon, unsigned int recipe,
                        struct en7573_tx_recipe *saved, bool restore)
{
 int ret = pon_context();
 if (ret) return ret;
 if (IS_ERR_OR_NULL(pon) || !saved) return -EINVAL;
 mutex_lock(&pon->lock);
 ret = !pon->leased ? -EPERM : pon_check_locked(pon);
 if (!ret && (!pon->activation_bench || pon->tx_inhibited || pon->tx_enabled))
  ret = -EACCES;
 if (!ret) ret = en7573_tx_recipe(&pon->io, pon->calibration, recipe, saved, restore);
 /* ENODATA before writes means an unavailable calibrated eye. */
 if (ret && !(ret == -ENODATA && !saved->count) && pon->leased && !pon->dead)
  pon_contain(pon, ret);
 mutex_unlock(&pon->lock);
 return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_tx_recipe);

int q1000k_pon_measure_mpd(struct q1000k_pon *pon, bool active,
                         struct en7573_mpd *sample)
{
 int ret = pon_context();
 if (ret) return ret;
 if (IS_ERR_OR_NULL(pon) || !sample) return -EINVAL;
 mutex_lock(&pon->lock);
 ret = !pon->leased ? -EPERM : pon_check_locked(pon);
 if (!ret && (!pon->activation_bench || pon->tx_inhibited)) ret = -EACCES;
 if (!ret) {
  int los = gpiod_get_value_cansleep(pon->los[1]);
  if (los <= 0) ret = los ?: -ENOLINK;
 }
 if (!ret) ret = en7573_measure_mpd(&pon->io, active, sample);
 if (!ret) ret = pon_check_locked(pon);
 if (ret && pon->leased && !pon->dead) pon_contain(pon, ret);
 mutex_unlock(&pon->lock);
 return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_measure_mpd);

/* Independent gate truth table, only for the disconnected finite owner.
 * bit0 releases internal TX_DISABLE; bit1 releases board GPIO38.
 */
int q1000k_pon_output_gates(struct q1000k_pon *pon, unsigned int gates)
{
	int ret = pon_context();
	if (ret) return ret;
	if (IS_ERR_OR_NULL(pon) || gates > 3) return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret && (!pon->activation_bench || pon->tx_inhibited || !pon->board_tx_disable)) ret = -EACCES;
	if (!ret && gates) {
		int los = gpiod_get_value_cansleep(pon->los[1]);
		if (los <= 0) ret = los ?: -ENOLINK;
	}
	/* External disable first; release it only after internal write/readback. */
	if (!ret) ret = pon_board_gate(pon, true);
	if (!ret) ret = en7573_set_tx(&pon->io, !!(gates & 1));
	if (!ret) pon->tx_enabled = !!(gates & 1);
	if (!ret) ret = pon_board_gate(pon, !(gates & 2));
	if (ret && pon->leased && !pon->dead) pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_output_gates);

int q1000k_pon_output_sample(struct q1000k_pon *pon, struct en7573_output_sample *s)
{
	int ret = pon_context();
	if (ret) return ret;
	if (IS_ERR_OR_NULL(pon) || !s) return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret) ret = en7573_output_sample(&pon->io, s);
	if (!ret && pon->board_tx_disable) {
		s->board_disabled = gpiod_get_value_cansleep(pon->board_tx_disable);
		if (s->board_disabled < 0) ret = s->board_disabled;
	}
	if (ret && pon->leased && !pon->dead) pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_output_sample);

int q1000k_pon_output_hold(struct q1000k_pon *pon, struct en7573_output_hold *s, bool restore)
{
	int ret = pon_context();
	if (ret) return ret;
	if (IS_ERR_OR_NULL(pon) || !s) return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret && (!pon->activation_bench || pon->tx_inhibited || pon->tx_enabled)) ret = -EACCES;
	if (!ret) {
		int los = gpiod_get_value_cansleep(pon->los[1]);
		if (los <= 0) ret = los ?: -ENOLINK;
	}
	if (!ret) ret = en7573_output_hold(&pon->io, s, restore);
	if (ret && pon->leased && !pon->dead) pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_output_hold);

int q1000k_pon_set_tx(struct q1000k_pon *pon, bool enable)
{
	const char *phase = "check";
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon))
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	/* Boot-time bench policy: no runtime control can grant TX permission. */
	if (!ret && enable && pon->tx_inhibited) {
		phase = "tx-inhibit-policy";
		ret = -EPERM;
	}
	if (!ret && !enable) ret = pon_board_gate(pon, true);
	if (!ret) {
		phase = "write-readback";
		ret = en7573_set_tx(&pon->io, enable);
	}
	if (!ret && enable) ret = pon_board_gate(pon, false);
	if (ret)
		dev_err_ratelimited(&pon->client->dev,
			"controller set-tx failed: enable=%d phase=%s error=%d prior_fault=%d initialized=%d leased=%d dead=%d\n",
			enable, phase, ret, pon->fault, pon->initialized,
			pon->leased, pon->dead);
	if (!ret)
		pon->tx_enabled = enable;
	else if (!pon->dead && pon->leased)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_set_tx);

int q1000k_pon_get_los(struct q1000k_pon *pon)
{
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon))
		return -EINVAL;
	mutex_lock(&pon->lock);
	ret = !pon->leased ? -EPERM : pon_check_locked(pon);
	if (!ret)
		ret = gpiod_get_value_cansleep(pon->los[1]);
	if (ret < 0 && !pon->dead && pon->leased)
		pon_contain(pon, ret);
	mutex_unlock(&pon->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_get_los);

int q1000k_pon_put(struct q1000k_pon *pon)
{
	int ret = pon_context();

	if (ret)
		return ret;
	if (IS_ERR_OR_NULL(pon))
		return -EINVAL;
	mutex_lock(&pon->lock);
	if (!pon->leased) {
		mutex_unlock(&pon->lock);
		return -EPERM;
	}
	ret = pon->dead ? -ENODEV : pon->fault;
	if (!pon->dead && pon->initialized) {
		int board = pon_board_gate(pon, true);
		int disable = en7573_set_tx(&pon->io, false);

		if (!disable)
			pon->tx_enabled = false;
		if (board || disable)
			pon_contain(pon, board ?: disable);
		if (!ret)
			ret = board ?: disable;
	}
	pon->leased = false;
	mutex_unlock(&pon->lock);
	kref_put(&pon->ref, pon_free);
	return ret;
}
EXPORT_SYMBOL_GPL(q1000k_pon_put);

static void pon_unpublish(struct q1000k_pon *pon)
{
	mutex_lock(&pon_registry_lock);
	if (pon_registered == pon)
		pon_registered = NULL;
	mutex_lock(&pon->lock);
	pon->dead = true;
	pon_off(pon);
	mutex_unlock(&pon->lock);
	mutex_unlock(&pon_registry_lock);
}
/* End kernel consumer lifecycle. */

static ssize_t operation_store(struct device *dev, struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	struct q1000k_pon *pon = dev_get_drvdata(dev);
	int ret = 0, other;
	mutex_lock(&pon->lock);
	if (pon->dead || pon->leased) {
		ret = pon->dead ? -ENODEV : -EBUSY;
		goto done;
	}
	if (sysfs_streq(buffer, "off")) {
		ret = pon_off(pon);
		pon->stage = "off";
	} else if (sysfs_streq(buffer, "detect")) {
		if (pon->initialized) {
			ret = -EBUSY;
			goto done;
		}
		pon->stage = "detect";
		ret = pon_select(pon, 0);
		other = pon_select(pon, 1);
		if (!ret)
			ret = other;
		other = pon_off(pon);
		if (!ret)
			ret = other;
	} else if (sysfs_streq(buffer, "initialize")) {
		if (pon->initialized) {
			ret = -EBUSY;
			goto done;
		}
		ret = pon_initialize(pon);
		if (!ret)
			pon->fault = 0;
	} else {
		ret = -EINVAL;
		goto done;
	}
	if (ret) {
		other = pon_off(pon);
		if (other)
			dev_err(dev, "power-off failed: %d\n", other);
	}
done:
	pon->last_error = ret;
	mutex_unlock(&pon->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(operation);

static const char *detected_json(int detected)
{
	return detected < 0 ? "null" : detected ? "true" : "false";
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	struct q1000k_pon *pon = dev_get_drvdata(dev);
	const char *los = "null";
	struct en7573_state state = { .md32_enabled = -1, .tx_disabled = -1 };
	int ret = 0;
	ssize_t size;
	mutex_lock(&pon->lock);
	if (pon->mode == -1) {
		state.md32_enabled = 0;
		state.tx_disabled = 1;
	}
	if (pon->initialized) {
		ret = en7573_sample_state(&pon->io, &state);
		if (!ret && (!state.md32_enabled || state.tx_disabled == pon->tx_enabled))
			ret = -EIO;
		if (!ret) {
			ret = gpiod_get_value_cansleep(pon->los[1]);
			if (ret >= 0) {
				los = ret ? "true" : "false";
				ret = 0;
			}
		}
	}
	size = sysfs_emit(buffer,
		"{\"schema_version\":1,\"bench_md32_a0\":%s,\"bench_rx_output\":%u,\"mode\":\"%s\",\"gpon_detected\":%s,"
		"\"xgspon_detected\":%s,\"gpon_id\":%u,\"xgspon_id\":%u,"
		"\"checked_uptime\":%llu,\"md32_enabled\":%s,\"tx_disabled\":%s,\"tx_inhibited\":%s,"
		"\"calibration_supplied\":%s,\"firmware_verified\":%s,\"los\":%s,\"last_error\":%d,\"stage\":\"%s\"}\n",
		pon->io.md32_a0 ? "true" : "false", bench_rx_output,
		pon->mode == -2 ? "unknown" : pon->mode == -1 ? "off" : pon->mode ? "xgspon" : "gpon",
		detected_json(pon->detected[0]), detected_json(pon->detected[1]),
		pon->id[0], pon->id[1], pon->checked_at,
		detected_json(state.md32_enabled), detected_json(state.tx_disabled),
		pon->tx_inhibited ? "true" : "false",
		pon->calibration_valid ? "true" : "false",
		pon->initialized ? "true" : "false", los, ret ? ret : pon->last_error, pon->stage);
	mutex_unlock(&pon->lock);
	return size;
}
static DEVICE_ATTR_RO(status);

static ssize_t receiver_status_show(struct device *dev,
				   struct device_attribute *attr, char *buffer)
{
	struct q1000k_pon *pon = dev_get_drvdata(dev);
	struct en7573_receiver sample;
	ssize_t ret;

	mutex_lock(&pon->lock);
	ret = pon_check_locked(pon);
	if (!ret)
		ret = en7573_sample_receiver(&pon->io, &sample);
	if (!ret)
		ret = sysfs_emit(buffer,
			"{\"schema_version\":1,\"controller_version\":3,\"receiver_status\":true,"
			"\"bench_md32_a0\":%s,\"bench_rx_output\":%u,"
			"\"sampled_ms\":%llu,\"mcu_a0\":%u,\"mcu_a2\":%u,"
			"\"apd_control\":%u,\"ocp_control\":%u,\"firmware_status\":%u,"
			"\"los_control\":%u,\"system_status\":%u,"
			"\"rx_output_control\":%u,\"rx_output_shape\":%u,\"ocp_status\":%u,"
			"\"temperature_raw\":%u,\"supply_raw\":%u,\"apd_voltage_raw\":%u,"
			"\"rssi_adc\":%u,\"rssi_current_raw\":%u,"
			"\"rx_output_mask_control\":64,\"rx_output_mask_shape\":1059012360,"
			"\"rx_output_saved\":%s,\"rx_output_original_control\":%u,"
			"\"rx_output_original_shape\":%u,\"oem_post_saved\":%s,"
			"\"oem_post_original_control\":%u}\n",
			pon->io.md32_a0 ? "true" : "false", bench_rx_output,
			ktime_get_boottime_ns() / 1000000, sample.mcu_a0, sample.mcu_a2,
			sample.apd, sample.ocp, sample.firmware, sample.los_control,
			sample.system_status, sample.rx_output_control, sample.rx_output_shape,
			sample.ocp_status, sample.temperature_raw, sample.supply_raw,
			sample.apd_voltage_raw, sample.rssi_adc, sample.rssi_current_raw,
			pon->rx_output_original.saved ? "true" : "false",
			pon->rx_output_original.control, pon->rx_output_original.shape,
			pon->oem_post_original.saved ? "true" : "false",
			pon->oem_post_original.control);
	mutex_unlock(&pon->lock);
	return ret;
}
static DEVICE_ATTR_RO(receiver_status);

static ssize_t transmitter_status_show(struct device *dev,
                                       struct device_attribute *attr, char *buffer)
{
 struct q1000k_pon *pon = dev_get_drvdata(dev);
 struct en7573_transmitter sample;
 unsigned int i;
 u64 begin, end;
 ssize_t len;
 int ret;
 mutex_lock(&pon->lock);
 begin = ktime_get_boottime_ns();
 ret = pon_check_locked(pon);
 if (!ret) ret = en7573_sample_transmitter(&pon->io, &sample);
 end = ktime_get_boottime_ns();
 if (ret) { mutex_unlock(&pon->lock); return ret; }
 len = sysfs_emit(buffer, "{\"transmitter_version\":1,\"begin_ns\":%llu,\"end_ns\":%llu,"
  "\"sensor_refresh_verified\":false,\"connector_emission_verified\":false,\"fields\":{", begin, end);
 for (i = 0; i < EN7573_TX_FIELDS; i++) {
  const struct en7573_tx_field *f = &en7573_tx_fields[i];
  char raw[16], value[16];
  /* A failed transfer has no raw word. Saturation retains its raw value. */
  if (sample.error[i] && sample.error[i] != -ENODATA) scnprintf(raw, sizeof(raw), "null");
  else scnprintf(raw, sizeof(raw), "%u", sample.raw[i]);
  if (sample.error[i]) scnprintf(value, sizeof(value), "null");
  else if (!f->scale) scnprintf(value, sizeof(value), "%d", (int)(s16)sample.raw[i] * 1000 / 256);
  else scnprintf(value, sizeof(value), "%u", sample.raw[i] * f->scale);
  len += sysfs_emit_at(buffer, len, "%s\"%s\":{\"reg\":%u,\"raw\":%s,\"valid\":%s,\"error\":%d,\"unit\":\"%s\",\"value\":%s}",
   i ? "," : "", f->name, f->reg, raw, sample.error[i] ? "false" : "true", sample.error[i], f->unit, value);
 }
 len += sysfs_emit_at(buffer, len, "}}\n");
 mutex_unlock(&pon->lock);
 return len;
}
static DEVICE_ATTR_RO(transmitter_status);

static ssize_t calibration_write(struct file *file, struct kobject *kobj,
				 const struct bin_attribute *attr, char *buffer,
				 loff_t offset, size_t count)
{
	struct q1000k_pon *pon = dev_get_drvdata(kobj_to_dev(kobj));
	int ret = 0;
	if (offset || count != sizeof(pon->calibration))
		return -EINVAL;
	if (!memchr_inv(buffer, 0, EN7573_CAL_SIZE) ||
	    !memchr_inv(buffer, 0xff, EN7573_CAL_SIZE))
		return -EINVAL;
	mutex_lock(&pon->lock);
	if (pon->dead || pon->leased || pon->mode != -1) {
		ret = -EBUSY;
	} else {
		memcpy(pon->calibration, buffer, count);
		pon->calibration_valid = true;
	}
	mutex_unlock(&pon->lock);
	return ret ? ret : count;
}
static BIN_ATTR_WO(calibration, 513);

static struct attribute *pon_attributes[] = {
	&dev_attr_operation.attr, &dev_attr_status.attr,
	&dev_attr_receiver_status.attr, &dev_attr_transmitter_status.attr, NULL,
};
static const struct bin_attribute *const pon_bin_attributes[] = {
	&bin_attr_calibration, NULL,
};
static const struct attribute_group pon_group = {
	.attrs = pon_attributes, .bin_attrs = pon_bin_attributes,
};

static void pon_shutdown_action(void *data)
{
	struct q1000k_pon *pon = data;
	int ret = pon_off(pon);
	if (ret)
		dev_err(&pon->client->dev, "power-off failed: %d\n", ret);
	memzero_explicit(pon->calibration, sizeof(pon->calibration));
}

static int pon_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct i2c_client *memory;
	struct q1000k_pon *pon;
	int ret;
	if (!of_machine_is_compatible("quantum,q1000k-ubi") || client->addr != EN7573_CONTROL)
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	pon = kzalloc(sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;
	kref_init(&pon->ref);
	ret = devm_add_action_or_reset(dev, pon_drop_device_ref, pon);
	if (ret)
		return ret;
	pon->client = client;
	pon->activation_bench = of_property_read_bool(dev->of_node, "quantum,activation-bench");
	ret = q1000k_pon_tx_policy(of_property_read_bool(dev->of_node, "quantum,tx-inhibit"),
				  pon->activation_bench, validation_tx, &pon->tx_inhibited);
	if (ret)
		return dev_err_probe(dev, ret, "TX permission requires the activation-validation DT\n");
	/* Preserve the older immutable-inhibit image's diagnostic controls.
	 * Normal startup and the new validation image receive the proven fix.
	 */
	pon->receiver_startup = pon->activation_bench || !pon->tx_inhibited;
	if (bench_rx_output > 3 || ((bench_md32_a0 || bench_rx_output) &&
				 !pon->tx_inhibited))
		return dev_err_probe(dev, -EACCES, "RX experiments require TX-inhibited bench and valid profile\n");
	pon->mode = -1;
	pon->stage = "off";
	pon->detected[0] = pon->detected[1] = -1;
	mutex_init(&pon->lock);
	pon->io = (struct en7573_io){ .ctx = pon, .md32_a0 = bench_md32_a0, .read = pon_read,
		.write = pon_write, .delay_ms = pon_delay, .diagnostic = pon_control_diagnostic };
	pon->power[0] = devm_gpiod_get(dev, "gpon-enable", GPIOD_OUT_LOW);
	if (IS_ERR(pon->power[0]))
		return dev_err_probe(dev, PTR_ERR(pon->power[0]), "GPON enable GPIO\n");
	pon->power[1] = devm_gpiod_get(dev, "xgspon-enable", GPIOD_OUT_LOW);
	if (IS_ERR(pon->power[1]))
		return dev_err_probe(dev, PTR_ERR(pon->power[1]), "XGS-PON enable GPIO\n");
	pon->board_tx_disable = devm_gpiod_get_optional(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(pon->board_tx_disable)) {
		ret = PTR_ERR(pon->board_tx_disable);
		pon->board_tx_disable = NULL;
		return dev_err_probe(dev, ret, "board TX-disable GPIO\n");
	}
	ret = devm_add_action_or_reset(dev, pon_shutdown_action, pon);
	if (ret)
		return ret;
	pon->select = devm_gpiod_get_array(dev, "select", GPIOD_OUT_LOW);
	if (IS_ERR(pon->select))
		return dev_err_probe(dev, PTR_ERR(pon->select), "controller selection GPIOs\n");
	if (pon->select->ndescs != 2)
		return -EINVAL;
	pon->los[0] = devm_gpiod_get(dev, "gpon-los", GPIOD_IN);
	if (IS_ERR(pon->los[0]))
		return dev_err_probe(dev, PTR_ERR(pon->los[0]), "GPON LOS GPIO\n");
	pon->los[1] = devm_gpiod_get(dev, "xgspon-los", GPIOD_IN);
	if (IS_ERR(pon->los[1]))
		return dev_err_probe(dev, PTR_ERR(pon->los[1]), "XGS-PON LOS GPIO\n");
	memory = devm_i2c_new_dummy_device(dev, client->adapter, EN7573_MEMORY);
	if (IS_ERR(memory))
		return dev_err_probe(dev, PTR_ERR(memory), "controller memory address\n");
	i2c_set_clientdata(client, pon);
	ret = devm_device_add_group(dev, &pon_group);
	if (ret)
		return ret;
	mutex_lock(&pon_registry_lock);
	if (pon_registered)
		ret = -EBUSY;
	else
		pon_registered = pon;
	mutex_unlock(&pon_registry_lock);
	return ret;
}

static const struct of_device_id pon_of_match[] = {
	{ .compatible = "quantum,q1000k-pon-control" }, { }
};
MODULE_DEVICE_TABLE(of, pon_of_match);
static const struct i2c_device_id pon_ids[] = { { "q1000k-pon" }, { } };
MODULE_DEVICE_TABLE(i2c, pon_ids);

static void pon_remove(struct i2c_client *client)
{
	pon_unpublish(i2c_get_clientdata(client));
}

static void pon_shutdown(struct i2c_client *client)
{
	struct q1000k_pon *pon = i2c_get_clientdata(client);

	pon_unpublish(pon);
}

static struct i2c_driver pon_driver = {
	.driver = { .name = "q1000k-pon-control", .of_match_table = pon_of_match },
	.probe = pon_probe, .remove = pon_remove, .shutdown = pon_shutdown, .id_table = pon_ids,
};
module_i2c_driver(pon_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Q1000K EN7573 controller with exclusive kernel consumer ownership");
