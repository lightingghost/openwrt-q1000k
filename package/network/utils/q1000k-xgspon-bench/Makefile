# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-xgspon-bench
PKG_RELEASE:=8
PKG_LICENSE:=GPL-2.0-only
include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon-bench
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Q1000K RAM bench checks with optical TX inhibited
  DEPENDS:=@TARGET_airoha_an7581 +q1000k-xgspon +q1000k-omci-tools +kmod-q1000k-pon-control +kmod-airoha-xpon-en757x
endef

define Build/Compile
endef

define Package/q1000k-xgspon-bench/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/uci-defaults $(1)/etc/sysctl.d
	$(INSTALL_BIN) ./files/bench $(1)/usr/sbin/q1000k-pon-bench
	$(INSTALL_BIN) ./files/defaults $(1)/etc/uci-defaults/99-q1000k-xgspon-bench
	$(INSTALL_DATA) ./files/sysctl.conf $(1)/etc/sysctl.d/99-q1000k-xgspon-bench.conf
endef

$(eval $(call BuildPackage,q1000k-xgspon-bench))
