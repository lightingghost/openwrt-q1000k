# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-pon-firmware
PKG_RELEASE:=1
PKG_LICENSE:=Proprietary

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-pon-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Q1000K optical controller MD32 firmware
  DEPENDS:=@TARGET_airoha_an7581
  PKGARCH:=all
endef

define Package/q1000k-pon-firmware/description
 Stock Q1000K EN7573 optical controller program and initial data, verified
 against the OEM NAND root filesystem and vendor update. Unit calibration,
 identity, serial numbers, MAC addresses and ISP settings are not included.
endef

define Build/Compile
endef

define Package/q1000k-pon-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/airoha/q1000k
	$(INSTALL_DATA) ./files/A60993.elf.pm $(1)/lib/firmware/airoha/q1000k/
	$(INSTALL_DATA) ./files/A60993.elf.dm $(1)/lib/firmware/airoha/q1000k/
endef

$(eval $(call BuildPackage,q1000k-pon-firmware))
