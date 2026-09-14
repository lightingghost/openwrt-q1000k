// SPDX-License-Identifier: GPL-2.0-only
/* Immutable identity supplied by the factory-aware userspace launcher.
 * No raw NAND access or synthetic identity fallback belongs in this module.
 */
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <xpon_public_const.h>
#include "common/q1000k_identity.h"

static char *wan_mac;
static char *pon_serial;
static char *pon_reg_id;
module_param(pon_reg_id, charp, 0);
MODULE_PARM_DESC(pon_reg_id, "XGS-PON registration ID: exactly 36 bytes as 72 hex digits (required)");
module_param(wan_mac, charp, 0);
MODULE_PARM_DESC(wan_mac, "Validated factory or configured WAN MAC (required)");
module_param(pon_serial, charp, 0);
MODULE_PARM_DESC(pon_serial, "Validated FSAN: four vendor characters and eight hex digits (required)");

static unsigned char identity_mac[ETH_ALEN];
static unsigned char identity_serial[8];
static unsigned char identity_registration[36];
static bool identity_ready;

int q1000k_pon_identity_init(void)
{
	unsigned char mac[ETH_ALEN], serial[8], registration[36] = {};
	int i;

	identity_ready = false;
	memset(identity_mac, 0, sizeof(identity_mac));
	memset(identity_serial, 0, sizeof(identity_serial));
	memset(identity_registration, 0, sizeof(identity_registration));
	if (!of_machine_is_compatible("quantum,q1000k-ubi"))
		return -ENODEV;
	if (!wan_mac || !pon_serial || !pon_reg_id)
		return -ENODATA;
	if (strlen(wan_mac) != 17 || !mac_pton(wan_mac, mac) ||
	    !is_valid_ether_addr(mac) || strlen(pon_serial) != 12)
		return -EINVAL;
	for (i = 0; i < 4; i++) {
		unsigned char c = pon_serial[i];

		/* Match the factory/backend's ASCII vendor-character contract. */
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9')))
			return -EINVAL;
		serial[i] = c;
	}
	if (hex2bin(serial + 4, pon_serial + 4, 4))
		return -EINVAL;
	if (strlen(pon_reg_id) != sizeof(registration) * 2 ||
	    hex2bin(registration, pon_reg_id, sizeof(registration))) {
		memzero_explicit(registration, sizeof(registration));
		return -EINVAL;
	}
	memcpy(identity_mac, mac, sizeof(mac));
	memcpy(identity_serial, serial, sizeof(serial));
	memcpy(identity_registration, registration, sizeof(registration));
	memzero_explicit(registration, sizeof(registration));
	identity_ready = true;
	return 0;
}

int get_ethaddr(unsigned char *addr, int len)
{
	if (!addr || len != sizeof(identity_mac))
		return -EINVAL;
	if (!identity_ready)
		return -ENODATA;
	memcpy(addr, identity_mac, sizeof(identity_mac));
	return 0;
}

int q1000k_pon_get_serial(unsigned char *serial, int len)
{
	if (!serial || len != sizeof(identity_serial))
		return -EINVAL;
	if (!identity_ready)
		return -ENODATA;
	memcpy(serial, identity_serial, sizeof(identity_serial));
	return 0;
}

int q1000k_pon_get_registration(unsigned char *registration, int len)
{
	if (!registration || len != sizeof(identity_registration))
		return -EINVAL;
	if (!identity_ready)
		return -ENODATA;
	memcpy(registration, identity_registration, sizeof(identity_registration));
	return 0;
}

char get_onutype(void)
{
	/* OEM boot log: onu_type=71. XGS-PON mode 7, SFU type 1, no
	 * vendor combo/BBF247 flags. Two EN7573 devices do not imply combo bit 2.
	 * MAC startup validates the board, identity and mode before calling this.
	 */
	return (XMCS_IF_WAN_DETECT_MODE_XGSPON << 4) | 1;
}
