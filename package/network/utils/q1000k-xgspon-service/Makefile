# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-xgspon-service
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon-service
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Experimental Q1000K XGS-PON lifecycle service
  DEPENDS:=@BROKEN @TARGET_airoha_an7581 +q1000k-xgspon +q1000k-omci-tools +kmod-airoha-xpon-en757x
endef

define Package/q1000k-xgspon-service/description
 Explicitly enabled procd supervision of the Q1000K controller, PHY, MAC
 and OMCI modules. Requires matching enabled board resources, OEM firmware,
 factory calibration, a registration ID and an explicit running native PON
 lower device. Disabled by default; no automatic restart after failure.
endef

define Build/Compile
endef

define Package/q1000k-xgspon-service/install
	$(INSTALL_DIR) $(1)/usr/libexec $(1)/etc/init.d
	$(INSTALL_BIN) ./files/run $(1)/usr/libexec/q1000k-xgspon-run
	$(INSTALL_BIN) ./files/init $(1)/etc/init.d/q1000k-xgspon
endef

$(eval $(call BuildPackage,q1000k-xgspon-service))
