# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-xgspon-service
PKG_RELEASE:=8
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon-service
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Experimental Q1000K XGS-PON lifecycle service
  DEPENDS:=@BROKEN @TARGET_airoha_an7581 +q1000k-xgspon +q1000k-omci-tools +kmod-airoha-xpon-en757x
endef

define Package/q1000k-xgspon-service/description
 Automatic TX-disabled optical controller monitoring, waiting for verified
 OEM firmware and unit calibration. Explicitly enabled registration adds
 the PHY, MAC and OMCI modules and requires subscriber identity and an
 explicit native PON lower device. Private activation RAM builds can provide
 continuous LAN/WAN service. No restart after hardware failure.
endef

define Build/Compile
endef

define Package/q1000k-xgspon-service/install
	$(INSTALL_DIR) $(1)/usr/libexec $(1)/etc/init.d
	$(INSTALL_BIN) ./files/run $(1)/usr/libexec/q1000k-xgspon-run
	$(INSTALL_BIN) ./files/init $(1)/etc/init.d/q1000k-xgspon
endef

$(eval $(call BuildPackage,q1000k-xgspon-service))
