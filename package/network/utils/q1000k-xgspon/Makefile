# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-xgspon
PKG_RELEASE:=9
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Q1000K XGS-PON factory data and diagnostics
  DEPENDS:=@TARGET_airoha_an7581 +jshn
endef

define Package/q1000k-xgspon/description
 Factory-data access, diagnostics and explicit EN7573 controller bring-up.
 The companion q1000k-xgspon-service package automatically loads TX-disabled
 optical monitoring and supervises explicitly enabled experimental MAC/OMCI
 startup. Installing this base package alone does not load PON modules.
endef

define Package/q1000k-xgspon/conffiles
/etc/config/q1000k-xgspon
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror $(TARGET_CPPFLAGS) \
		-o $(PKG_BUILD_DIR)/q1000k-pon-factory $(PKG_BUILD_DIR)/factory.c $(TARGET_LDFLAGS)
endef

define Package/q1000k-xgspon/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/config $(1)/lib/q1000k-xgspon $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/q1000k-pon-factory $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/q1000k-xgspon $(1)/usr/sbin/
	$(INSTALL_CONF) ./files/q1000k-xgspon.config $(1)/etc/config/q1000k-xgspon
	$(INSTALL_BIN) ./files/omci-config $(1)/usr/libexec/q1000k-omci-config
	$(INSTALL_DATA) ./files/common.sh $(1)/lib/q1000k-xgspon/
endef

$(eval $(call BuildPackage,q1000k-xgspon))
