# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-omci-tools
PKG_RELEASE:=5
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-omci-tools
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Q1000K kernel OMCI management command
  DEPENDS:=@TARGET_airoha_an7581 +kmod-q1000k-omci +libmnl +q1000k-xgspon
endef

define Package/q1000k-omci-tools/description
 The omci command provides JSON status, managed-entity inspection and
 validated configuration through the kernel OMCI Generic Netlink interface.
 It does not start a second OMCI daemon or load optical modules.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -I$(STAGING_DIR)/usr/include/q1000k-omci/uapi -Wall -Wextra -Werror \
		-o $(PKG_BUILD_DIR)/q1000k-omci $(PKG_BUILD_DIR)/omci.c $(TARGET_LDFLAGS) -lmnl -lm
endef

define Package/q1000k-omci-tools/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/q1000k-omci $(1)/usr/sbin/omci
	$(LN) omci $(1)/usr/sbin/q1000k-omci
endef

$(eval $(call BuildPackage,q1000k-omci-tools))
