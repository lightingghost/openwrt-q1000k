// SPDX-License-Identifier: GPL-2.0-only
/* Appended to the real core.c in the disposable UML fixture. */
#ifndef CONFIG_UML
#error This fixture must never run on a physical device.
#endif

static int telemetry_test_error;
static int telemetry_test_sample(struct omci_device *odev, struct omci_telemetry *sample)
{
	/* Deliberately publish partial data even when returning failure. */
	sample->valid = OMCI_TELEMETRY_F_FEC_UPSTREAM;
	sample->upstream_fec = OMCI_FEC_STATUS_UP;
	sample->bosa_temperature_mc = 45000;
	return telemetry_test_error;
}

int q1000k_omci_telemetry_test(void)
{
	struct omci_device_ops ops = { .get_telemetry = telemetry_test_sample };
	static struct omci_device odev;
	struct nlattr *attrs[OMCI_ATTR_MAX + 1];
	struct sk_buff *msg;
	unsigned int i;
	int ret = 0, mode;

	/* MIB diagnostics redact credentials without altering the actual OLT MIB. */
	{
		struct omci_mib_object object = { .class_id = OMCI_CLASS_ONU_G };
		struct nlattr *data;
		memset(object.data, 'x', sizeof(object.data));
		msg = alloc_skb(512, GFP_KERNEL);
		if (!msg) return -ENOMEM;
		ret = omci_put_mib_object(msg, &object, 0, "ONU-G");
		data = nla_find((struct nlattr *)msg->data, msg->len, OMCI_ATTR_ATTR_DATA);
		if (ret || !data || nla_len(data) != OMCI_MAX_ATTR_DATA ||
		    memchr_inv((u8 *)nla_data(data) + 32, 0, 36) ||
		    ((u8 *)nla_data(data))[0] != 'x' || object.data[56] != 'x') {
			kfree_skb(msg);
			return -EINVAL;
		}
		kfree_skb(msg);
	}
	odev.ops = &ops;
	for (mode = 0; mode < 4; mode++) {
		msg = alloc_skb(256, GFP_KERNEL);
		if (!msg) return -ENOMEM;
		ops.get_telemetry = mode == 3 ? NULL : telemetry_test_sample;
		telemetry_test_error = mode == 1 ? -EIO : mode == 2 ? -ENODATA : 0;
		ret = omci_put_telemetry(msg, &odev);
		if (!ret) ret = nla_parse(attrs, OMCI_ATTR_MAX, (struct nlattr *)msg->data,
					msg->len, NULL, NULL);
		if (!ret && (!attrs[OMCI_ATTR_TELEMETRY_VALID] ||
		    nla_get_u32(attrs[OMCI_ATTR_TELEMETRY_VALID]) !=
			(mode ? 0 : OMCI_TELEMETRY_F_FEC_UPSTREAM))) ret = -EBADMSG;
		if (!ret && !mode && (!attrs[OMCI_ATTR_FEC_UPSTREAM] ||
		    nla_get_u8(attrs[OMCI_ATTR_FEC_UPSTREAM]) != OMCI_FEC_STATUS_UP)) ret = -EBADMSG;
		/* Every optical field and downstream FEC stays absent, including
		 * provider-populated values without the corresponding valid bit.
		 */
		if (!ret) for (i = 0; i <= OMCI_ATTR_MAX; i++)
			if (i != OMCI_ATTR_TELEMETRY_VALID &&
			    (mode || i != OMCI_ATTR_FEC_UPSTREAM) && attrs[i]) ret = -EBADMSG;
		kfree_skb(msg);
		if (ret) return ret;
	}
	return 0;
}
