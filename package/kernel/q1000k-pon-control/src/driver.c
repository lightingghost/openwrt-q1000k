// SPDX-License-Identifier: GPL-2.0-only
#include <crypto/hash.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "en7573.h"

struct q1000k_pon {
	struct i2c_client *client;
	struct gpio_desc *power[2], *los[2]; /* GPON, XGS-PON */
	struct gpio_descs *select;
	struct en7573_io io;
	struct mutex lock;
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
	return ret < 0 ? ret : ret == 1 ? 0 : -EIO;
}

static void pon_delay(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static int pon_off(struct q1000k_pon *pon)
{
	int first, second;
	first = gpiod_set_value_cansleep(pon->power[0], 0);
	second = gpiod_set_value_cansleep(pon->power[1], 0);
	pon->initialized = false;
	pon->mode = first || second ? -2 : -1;
	return first ? first : second;
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
	if (!ret)
		pon->initialized = true;
	if (!ret)
		pon->stage = "initialized";
out:
	release_firmware(dm);
	release_firmware(pm);
	return ret;
}

static ssize_t operation_store(struct device *dev, struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	struct q1000k_pon *pon = dev_get_drvdata(dev);
	int ret = 0, other;
	mutex_lock(&pon->lock);
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
	u32 mcu, tx;
	int ret = 0;
	ssize_t size;
	mutex_lock(&pon->lock);
	if (pon->initialized) {
		ret = en7573_read_control(&pon->io, EN7573_MCU_ENABLE, &mcu);
		if (!ret)
			ret = en7573_read_control(&pon->io, EN7573_TX_CONTROL, &tx);
		if (!ret && (!(mcu & 1) || !(tx & EN7573_TX_DISABLE)))
			ret = -EIO;
		if (!ret) {
			ret = gpiod_get_value_cansleep(pon->los[1]);
			if (ret >= 0) {
				los = ret ? "true" : "false";
				ret = 0;
			}
		}
		if (ret) {
			int off_ret = pon_off(pon);
			if (off_ret)
				dev_err(dev, "power-off failed: %d\n", off_ret);
			pon->last_error = ret;
		}
	}
	size = sysfs_emit(buffer,
		"{\"schema_version\":1,\"mode\":\"%s\",\"gpon_detected\":%s,"
		"\"xgspon_detected\":%s,\"gpon_id\":%u,\"xgspon_id\":%u,"
		"\"checked_uptime\":%llu,\"md32_enabled\":%s,\"tx_disabled\":%s,"
		"\"calibration_supplied\":%s,\"firmware_verified\":%s,\"los\":%s,\"last_error\":%d,\"stage\":\"%s\"}\n",
		pon->mode == -2 ? "unknown" : pon->mode == -1 ? "off" : pon->mode ? "xgspon" : "gpon",
		detected_json(pon->detected[0]), detected_json(pon->detected[1]),
		pon->id[0], pon->id[1], pon->checked_at,
		pon->mode == -2 ? "null" : pon->initialized ? "true" : "false",
		pon->mode == -1 || pon->initialized ? "true" : "null",
		pon->calibration_valid ? "true" : "false",
		pon->initialized ? "true" : "false", los, pon->last_error, pon->stage);
	mutex_unlock(&pon->lock);
	return size;
}
static DEVICE_ATTR_RO(status);

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
	if (pon->mode != -1) {
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
	&dev_attr_operation.attr, &dev_attr_status.attr, NULL,
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
	pon = devm_kzalloc(dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;
	pon->client = client;
	pon->mode = -1;
	pon->stage = "off";
	pon->detected[0] = pon->detected[1] = -1;
	mutex_init(&pon->lock);
	pon->io = (struct en7573_io){ .ctx = pon, .read = pon_read,
		.write = pon_write, .delay_ms = pon_delay };
	pon->power[0] = devm_gpiod_get(dev, "gpon-enable", GPIOD_OUT_LOW);
	if (IS_ERR(pon->power[0]))
		return dev_err_probe(dev, PTR_ERR(pon->power[0]), "GPON enable GPIO\n");
	pon->power[1] = devm_gpiod_get(dev, "xgspon-enable", GPIOD_OUT_LOW);
	if (IS_ERR(pon->power[1]))
		return dev_err_probe(dev, PTR_ERR(pon->power[1]), "XGS-PON enable GPIO\n");
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
	return devm_device_add_group(dev, &pon_group);
}

static const struct of_device_id pon_of_match[] = {
	{ .compatible = "quantum,q1000k-pon-control" }, { }
};
MODULE_DEVICE_TABLE(of, pon_of_match);
static const struct i2c_device_id pon_ids[] = { { "q1000k-pon" }, { } };
MODULE_DEVICE_TABLE(i2c, pon_ids);

static void pon_shutdown(struct i2c_client *client)
{
	struct q1000k_pon *pon = i2c_get_clientdata(client);

	mutex_lock(&pon->lock);
	pon_shutdown_action(pon);
	mutex_unlock(&pon->lock);
}

static struct i2c_driver pon_driver = {
	.driver = { .name = "q1000k-pon-control", .of_match_table = pon_of_match },
	.probe = pon_probe, .shutdown = pon_shutdown, .id_table = pon_ids,
};
module_i2c_driver(pon_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Q1000K EN7573 controller bring-up with transmission disabled");
