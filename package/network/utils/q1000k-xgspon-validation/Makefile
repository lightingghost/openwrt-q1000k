# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
PKG_NAME:=q1000k-xgspon-validation
PKG_RELEASE:=13
PKG_LICENSE:=GPL-2.0-only
include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon-validation
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Q1000K RAM activation and WAN validation
  DEPENDS:=@TARGET_airoha_an7581 +q1000k-xgspon-bench +q1000k-xgspon-wan +ip-full +iputils-ping +iperf3 +curl
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -Werror \
		-o $(PKG_BUILD_DIR)/q1000k-pd-source $(PKG_BUILD_DIR)/pd-source.c
endef

define Package/q1000k-xgspon-validation/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec $(1)/usr/share/q1000k-bench
	$(INSTALL_BIN) ./files/validate $(1)/usr/sbin/q1000k-pon-validate
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/q1000k-pd-source $(1)/usr/libexec/q1000k-pd-source
	$(INSTALL_DATA) ./files/capabilities.json $(1)/usr/share/q1000k-bench/capabilities.json
endef

$(eval $(call BuildPackage,q1000k-xgspon-validation))
