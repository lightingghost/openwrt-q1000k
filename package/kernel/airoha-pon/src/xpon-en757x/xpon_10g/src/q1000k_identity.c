// SPDX-License-Identifier: GPL-2.0-only
/* Immutable identity supplied by the factory-aware userspace launcher.
 * No raw NAND access or synthetic identity fallback belongs in this module.
 */
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <net/xpon/omci.h>
#include <xpon_public_const.h>
#include "common/q1000k_identity.h"

static char *pon_vendor_id_hex;
module_param(pon_vendor_id_hex, charp, 0);
MODULE_PARM_DESC(pon_vendor_id_hex, "Optional OMCI vendor_id, up to 4 ASCII bytes as hex");
static char *pon_hardware_version_hex;
module_param(pon_hardware_version_hex, charp, 0);
MODULE_PARM_DESC(pon_hardware_version_hex, "Optional OMCI hardware_version, up to 14 ASCII bytes as hex");
static char *pon_software0_hex;
module_param(pon_software0_hex, charp, 0);
MODULE_PARM_DESC(pon_software0_hex, "Optional OMCI software0, up to 14 ASCII bytes as hex");
static char *pon_software1_hex;
module_param(pon_software1_hex, charp, 0);
MODULE_PARM_DESC(pon_software1_hex, "Optional OMCI software1, up to 14 ASCII bytes as hex");
static char *pon_logical_onu_id_hex;
module_param(pon_logical_onu_id_hex, charp, 0);
MODULE_PARM_DESC(pon_logical_onu_id_hex, "Optional OMCI logical_onu_id, up to 24 ASCII bytes as hex");
static char *pon_logical_password_hex;
module_param(pon_logical_password_hex, charp, 0);
MODULE_PARM_DESC(pon_logical_password_hex, "Optional OMCI logical_password, up to 12 ASCII bytes as hex");
static int pon_sync_circuit_pack = -1;
module_param(pon_sync_circuit_pack, int, 0);
MODULE_PARM_DESC(pon_sync_circuit_pack, "Optional advertised OMCI sync_circuit_pack: 0 or 1; -1 keeps the native default");
static int pon_active_bank = -1;
module_param(pon_active_bank, int, 0);
MODULE_PARM_DESC(pon_active_bank, "Optional advertised OMCI active_bank: 0 or 1; -1 keeps the native default");
static int pon_committed_bank = -1;
module_param(pon_committed_bank, int, 0);
MODULE_PARM_DESC(pon_committed_bank, "Optional advertised OMCI committed_bank: 0 or 1; -1 keeps the native default");

static int pon_fix_vlans;
module_param(pon_fix_vlans, int, 0);
MODULE_PARM_DESC(pon_fix_vlans, "Enable subscriber VLAN-0 normalization (0 or 1)");

static int pon_omcc_version = -1;
module_param(pon_omcc_version, int, 0);
MODULE_PARM_DESC(pon_omcc_version, "Immutable OMCI omcc_version presentation");
static int pon_uni_slot = 1;
module_param(pon_uni_slot, int, 0);
MODULE_PARM_DESC(pon_uni_slot, "Immutable OMCI pon_slot presentation");
static int pon_olt_profile = -1;
module_param(pon_olt_profile, int, 0);
MODULE_PARM_DESC(pon_olt_profile, "Immutable OMCI olt_profile presentation");
static char *pon_iphost_mac;
module_param(pon_iphost_mac, charp, 0);
MODULE_PARM_DESC(pon_iphost_mac, "Optional OMCI IP host identity");
static char *pon_iphost_hostname_hex;
module_param(pon_iphost_hostname_hex, charp, 0);
MODULE_PARM_DESC(pon_iphost_hostname_hex, "Optional OMCI IP host identity");
static char *pon_iphost_domain_hex;
module_param(pon_iphost_domain_hex, charp, 0);
MODULE_PARM_DESC(pon_iphost_domain_hex, "Optional OMCI IP host identity");

static char *wan_mac;
static char *pon_serial;
static char *pon_reg_id;
static char *pon_equipment_id_hex;
static char *pon_omci_version_hex;
module_param(pon_equipment_id_hex, charp, 0);
MODULE_PARM_DESC(pon_equipment_id_hex, "Optional OMCI equipment ID: up to 20 printable ASCII bytes encoded as hex");
module_param(pon_omci_version_hex, charp, 0);
MODULE_PARM_DESC(pon_omci_version_hex, "Optional OMCI version: up to 14 printable ASCII bytes encoded as hex");
module_param(pon_reg_id, charp, 0);
MODULE_PARM_DESC(pon_reg_id, "XGS-PON registration ID: exactly 36 bytes as 72 hex digits (required)");
module_param(wan_mac, charp, 0);
MODULE_PARM_DESC(wan_mac, "Validated factory or configured WAN MAC (required)");
module_param(pon_serial, charp, 0);
MODULE_PARM_DESC(pon_serial, "Validated FSAN: four vendor characters and eight hex digits (required)");

static unsigned char identity_mac[ETH_ALEN];
static unsigned char identity_serial[8];
static unsigned char identity_registration[36];
static struct omci_identity identity_overrides;
static bool identity_ready;

static int q1000k_identity_text(const char *hex, unsigned char *value, size_t capacity)
{
	size_t len, i;

	if (!hex || !*hex) return 0;
	len = strnlen(hex, capacity * 2 + 1);
	if (len > capacity * 2 || (len & 1) || hex2bin(value, hex, len / 2))
		return -EINVAL;
	for (i = 0; i < len / 2; i++)
		if (value[i] < 0x20 || value[i] > 0x7e) return -EINVAL;
	return 1;
}

int q1000k_pon_identity_init(void)
{
	unsigned char mac[ETH_ALEN], serial[8], registration[36] = {};
	struct omci_identity overrides = {};
	int i, ret;

	identity_ready = false;
	memset(identity_mac, 0, sizeof(identity_mac));
	memset(identity_serial, 0, sizeof(identity_serial));
	memset(identity_registration, 0, sizeof(identity_registration));
	memset(&identity_overrides, 0, sizeof(identity_overrides));
	if (!of_machine_is_compatible("quantum,q1000k-ubi"))
		return -ENODEV;
	if (pon_fix_vlans < 0 || pon_fix_vlans > 1) return -EINVAL;
	if (pon_omcc_version != -1 && (pon_omcc_version < 0x80 || pon_omcc_version > 0xbf)) return -EINVAL;
	if (pon_uni_slot < 1 || pon_uni_slot > 254 || pon_uni_slot == 128) return -EINVAL;
	if (pon_olt_profile != -1 && (pon_olt_profile < 1 || pon_olt_profile > 7)) return -EINVAL;
	overrides.pon_slot = pon_uni_slot;
	if (pon_uni_slot != 1) overrides.valid |= OMCI_IDENTITY_F_PON_SLOT;
	if (pon_omcc_version >= 0) {
		overrides.omcc_version = pon_omcc_version;
		overrides.valid |= OMCI_IDENTITY_F_OMCC_VERSION;
	}
	if (pon_olt_profile >= 0) {
		overrides.olt_profile = pon_olt_profile;
		overrides.valid |= OMCI_IDENTITY_F_OLT_PROFILE;
	}
	if (pon_iphost_mac && *pon_iphost_mac) {
		if (strlen(pon_iphost_mac) != 17 || !mac_pton(pon_iphost_mac, overrides.iphost_mac) ||
		    !is_valid_ether_addr(overrides.iphost_mac)) return -EINVAL;
		overrides.valid |= OMCI_IDENTITY_F_IPHOST_MAC;
	}
	ret = q1000k_identity_text(pon_iphost_hostname_hex, overrides.iphost_hostname, sizeof(overrides.iphost_hostname));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_IPHOST_HOSTNAME;
	ret = q1000k_identity_text(pon_iphost_domain_hex, overrides.iphost_domain, sizeof(overrides.iphost_domain));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_IPHOST_DOMAIN;

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
	ret = q1000k_identity_text(pon_equipment_id_hex, overrides.equipment_id,
				   sizeof(overrides.equipment_id));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_EQUIPMENT_ID;
	ret = q1000k_identity_text(pon_omci_version_hex, overrides.version,
				   sizeof(overrides.version));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_VERSION;
	overrides.presentation_source = OMCI_CONFIG_SOURCE_DRIVER;
	ret = q1000k_identity_text(pon_vendor_id_hex, overrides.vendor_id, sizeof(overrides.vendor_id));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_VENDOR_ID;
	if (ret) {
		if (strlen(pon_vendor_id_hex) != 8) return -EINVAL;
		for (i = 0; i < 4; i++) {
			u8 c = overrides.vendor_id[i];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			      (c >= '0' && c <= '9'))) return -EINVAL;
		}
		overrides.vendor_source = OMCI_CONFIG_SOURCE_DRIVER;
	}
	ret = q1000k_identity_text(pon_hardware_version_hex, overrides.hardware_version, sizeof(overrides.hardware_version));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_HARDWARE_VERSION;
	ret = q1000k_identity_text(pon_software0_hex, overrides.software_version[0], sizeof(overrides.software_version[0]));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_SOFTWARE_VERSION_0;
	ret = q1000k_identity_text(pon_software1_hex, overrides.software_version[1], sizeof(overrides.software_version[1]));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_SOFTWARE_VERSION_1;
	ret = q1000k_identity_text(pon_logical_onu_id_hex, overrides.logical_onu_id, sizeof(overrides.logical_onu_id));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_LOGICAL_ONU_ID;
	ret = q1000k_identity_text(pon_logical_password_hex, overrides.logical_password, sizeof(overrides.logical_password));
	if (ret < 0) return ret;
	if (ret) overrides.valid |= OMCI_IDENTITY_F_LOGICAL_PASSWORD;
	if (pon_sync_circuit_pack < -1 || pon_sync_circuit_pack > 1) return -EINVAL;
	if (pon_sync_circuit_pack >= 0) {
		overrides.sync_circuit_pack = pon_sync_circuit_pack;
		overrides.valid |= OMCI_IDENTITY_F_SYNC_CIRCUIT_PACK;
	}
	if (pon_active_bank < -1 || pon_active_bank > 1) return -EINVAL;
	if (pon_active_bank >= 0) {
		overrides.active_bank = pon_active_bank;
		overrides.valid |= OMCI_IDENTITY_F_ACTIVE_BANK;
	}
	if (pon_committed_bank < -1 || pon_committed_bank > 1) return -EINVAL;
	if (pon_committed_bank >= 0) {
		overrides.committed_bank = pon_committed_bank;
		overrides.valid |= OMCI_IDENTITY_F_COMMITTED_BANK;
	}
	if (strlen(pon_reg_id) != sizeof(registration) * 2 ||
	    hex2bin(registration, pon_reg_id, sizeof(registration))) {
		memzero_explicit(registration, sizeof(registration));
		return -EINVAL;
	}
	memcpy(identity_mac, mac, sizeof(mac));
	memcpy(identity_serial, serial, sizeof(serial));
	memcpy(identity_registration, registration, sizeof(registration));
	if ((overrides.valid & (OMCI_IDENTITY_F_IPHOST_HOSTNAME | OMCI_IDENTITY_F_IPHOST_DOMAIN)) &&
	    !(overrides.valid & OMCI_IDENTITY_F_IPHOST_MAC)) {
		memcpy(overrides.iphost_mac, mac, sizeof(mac));
		overrides.valid |= OMCI_IDENTITY_F_IPHOST_MAC;
	}
	identity_overrides = overrides;
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

int q1000k_pon_get_omci_overrides(struct omci_identity *identity)
{
	if (!identity) return -EINVAL;
	if (!identity_ready) return -ENODATA;
	if (identity_overrides.valid & OMCI_IDENTITY_F_EQUIPMENT_ID) {
		memcpy(identity->equipment_id, identity_overrides.equipment_id,
		       sizeof(identity->equipment_id));
		identity->equipment_source = OMCI_CONFIG_SOURCE_DRIVER;
	}
	if (identity_overrides.valid & OMCI_IDENTITY_F_VERSION) {
		memcpy(identity->version, identity_overrides.version, sizeof(identity->version));
		identity->version_source = OMCI_CONFIG_SOURCE_DRIVER;
	}
	if (identity_overrides.valid & OMCI_IDENTITY_F_VENDOR_ID)
		memcpy(identity->vendor_id, identity_overrides.vendor_id, sizeof(identity->vendor_id));
	if (identity_overrides.valid & OMCI_IDENTITY_F_HARDWARE_VERSION)
		memcpy(identity->hardware_version, identity_overrides.hardware_version, sizeof(identity->hardware_version));
	if (identity_overrides.valid & OMCI_IDENTITY_F_SOFTWARE_VERSION_0)
		memcpy(identity->software_version[0], identity_overrides.software_version[0], sizeof(identity->software_version[0]));
	if (identity_overrides.valid & OMCI_IDENTITY_F_SOFTWARE_VERSION_1)
		memcpy(identity->software_version[1], identity_overrides.software_version[1], sizeof(identity->software_version[1]));
	if (identity_overrides.valid & OMCI_IDENTITY_F_LOGICAL_ONU_ID)
		memcpy(identity->logical_onu_id, identity_overrides.logical_onu_id, sizeof(identity->logical_onu_id));
	if (identity_overrides.valid & OMCI_IDENTITY_F_LOGICAL_PASSWORD)
		memcpy(identity->logical_password, identity_overrides.logical_password, sizeof(identity->logical_password));
	if (identity_overrides.valid & OMCI_IDENTITY_F_SYNC_CIRCUIT_PACK)
		identity->sync_circuit_pack = identity_overrides.sync_circuit_pack;
	if (identity_overrides.valid & OMCI_IDENTITY_F_ACTIVE_BANK)
		identity->active_bank = identity_overrides.active_bank;
	if (identity_overrides.valid & OMCI_IDENTITY_F_COMMITTED_BANK)
		identity->committed_bank = identity_overrides.committed_bank;
	if (identity_overrides.valid & OMCI_IDENTITY_F_VENDOR_ID)
		identity->vendor_source = OMCI_CONFIG_SOURCE_DRIVER;
	if (identity_overrides.valid & ~0x1fU)
		identity->presentation_source = OMCI_CONFIG_SOURCE_DRIVER;
	if (identity_overrides.valid & OMCI_IDENTITY_F_OMCC_VERSION)
		identity->omcc_version = identity_overrides.omcc_version;
	if (identity_overrides.valid & OMCI_IDENTITY_F_PON_SLOT)
		identity->pon_slot = identity_overrides.pon_slot;
	if (identity_overrides.valid & OMCI_IDENTITY_F_OLT_PROFILE)
		identity->olt_profile = identity_overrides.olt_profile;
	if (identity_overrides.valid & OMCI_IDENTITY_F_IPHOST_MAC)
		memcpy(identity->iphost_mac, identity_overrides.iphost_mac, sizeof(identity->iphost_mac));
	if (identity_overrides.valid & OMCI_IDENTITY_F_IPHOST_HOSTNAME)
		memcpy(identity->iphost_hostname, identity_overrides.iphost_hostname, sizeof(identity->iphost_hostname));
	if (identity_overrides.valid & OMCI_IDENTITY_F_IPHOST_DOMAIN)
		memcpy(identity->iphost_domain, identity_overrides.iphost_domain, sizeof(identity->iphost_domain));
	identity->valid |= identity_overrides.valid;
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

int q1000k_pon_fix_vlans(void)
{
	return identity_ready && pon_fix_vlans;
}

int q1000k_pon_uni_slot(void)
{
	return pon_uni_slot;
}
