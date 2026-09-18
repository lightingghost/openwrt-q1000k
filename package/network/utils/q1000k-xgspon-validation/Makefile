# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
PKG_NAME:=q1000k-xgspon-validation
PKG_RELEASE:=10
PKG_LICENSE:=GPL-2.0-only
include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon-validation
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Q1000K RAM activation and WAN validation
  DEPENDS:=@TARGET_airoha_an7581 +q1000k-xgspon-bench +q1000k-xgspon-wan +ip-full +iputils-ping +iperf3 +curl
endef

define Build/Compile
endef

define Package/q1000k-xgspon-validation/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/share/q1000k-bench
	$(INSTALL_BIN) ./files/validate $(1)/usr/sbin/q1000k-pon-validate
	$(INSTALL_DATA) ./files/capabilities.json $(1)/usr/share/q1000k-bench/capabilities.json
endef

$(eval $(call BuildPackage,q1000k-xgspon-validation))
