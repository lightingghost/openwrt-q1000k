# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-xgspon
PKG_RELEASE:=12
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
/etc/config/xgspon
endef

define Package/q1000k-xgspon/preinst
#!/bin/sh
# Preserve the old conffile before apk removes/renames obsolete package files.
root="$${IPKG_INSTROOT}"
if [ ! -e "$$root/etc/config/xgspon" ] && \
   [ -f "$$root/etc/config/q1000k-xgspon" ] && \
   [ ! -e "$$root/etc/xgspon-legacy-config" ]; then
	umask 077
	cp "$$root/etc/config/q1000k-xgspon" "$$root/etc/xgspon-legacy-config" || exit 1
fi
exit 0
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror $(TARGET_CPPFLAGS) \
		-o $(PKG_BUILD_DIR)/q1000k-pon-factory $(PKG_BUILD_DIR)/factory.c $(TARGET_LDFLAGS)
endef

define Package/q1000k-xgspon/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/config $(1)/lib/q1000k-xgspon $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/q1000k-pon-factory $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/q1000k-xgspon $(1)/usr/sbin/
	$(INSTALL_CONF) ./files/xgspon.config $(1)/etc/config/xgspon
	$(INSTALL_DIR) $(1)/usr/share/xgspon $(1)/etc/uci-defaults
	$(INSTALL_CONF) ./files/xgspon.config $(1)/usr/share/xgspon/defaults
	$(INSTALL_BIN) ./files/migrate-config $(1)/etc/uci-defaults/10-xgspon-config
	$(INSTALL_BIN) ./files/omci-config $(1)/usr/libexec/q1000k-omci-config
	$(INSTALL_DATA) ./files/common.sh $(1)/lib/q1000k-xgspon/
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/keep $(1)/lib/upgrade/keep.d/q1000k-xgspon
endef

$(eval $(call BuildPackage,q1000k-xgspon))
