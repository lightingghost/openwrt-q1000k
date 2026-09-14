# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-xgspon-wan
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-xgspon-wan
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Optional Q1000K optical DHCP WAN configuration
  DEPENDS:=@BROKEN @TARGET_airoha_an7581 +q1000k-xgspon-service +netifd +firewall4 +odhcp6c +uci
endef

define Package/q1000k-xgspon-wan/description
 Creates inactive DHCP and DHCPv6 interfaces on the UNI-facing pon device
 and associates them with the existing WAN firewall zone. Preserves existing
 LAN and WAN configuration. Does not activate interfaces or optical hardware.
endef

define Build/Compile
endef

define Package/q1000k-xgspon-wan/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/defaults $(1)/etc/uci-defaults/90-q1000k-xgspon-wan
endef

$(eval $(call BuildPackage,q1000k-xgspon-wan))
