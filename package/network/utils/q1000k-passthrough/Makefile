# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=q1000k-passthrough
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/q1000k-passthrough
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Q1000K IPv4 public-address handoff integration
  DEPENDS:=@TARGET_airoha_an7581 +q1000k-xgspon +firewall4 +ip-full +nftables-json +jsonfilter +flock +conntrack +dnsmasq +odhcpd-ipv6only
endef

define Package/q1000k-passthrough/description
 Reconcile the existing routed PON WAN lease with a selected downstream
 DHCPv4 client. Router mode is the default. IPv6 /61 delegation uses native
 netifd and odhcpd configuration independently; no additional DHCP server.
endef

define Build/Compile
endef

define Package/q1000k-passthrough/install
	$(INSTALL_DIR) $(1)/usr/libexec $(1)/etc/config $(1)/etc/init.d $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/ipv4 $(1)/usr/libexec/q1000k-passthrough-ipv4
	$(INSTALL_BIN) ./files/ipv6-transition $(1)/usr/libexec/q1000k-ipv6-transition
	$(INSTALL_BIN) ./files/watch-config $(1)/usr/libexec/q1000k-passthrough-watch-config
	$(INSTALL_BIN) ./files/init $(1)/etc/init.d/q1000k-passthrough
	$(INSTALL_DATA) ./files/hotplug $(1)/etc/hotplug.d/iface/95-q1000k-passthrough
	$(INSTALL_DIR) $(1)/usr/share/nftables.d/table-pre
	$(INSTALL_DIR) $(1)/usr/share/nftables.d/chain-pre/srcnat_wan
	$(INSTALL_DIR) $(1)/usr/share/nftables.d/chain-pre/forward_wan
	$(INSTALL_DATA) ./files/fw4-set.nft $(1)/usr/share/nftables.d/table-pre/30-q1000k-passthrough.nft
	$(INSTALL_DATA) ./files/fw4-nat.nft $(1)/usr/share/nftables.d/chain-pre/srcnat_wan/30-q1000k-passthrough.nft
	$(INSTALL_DATA) ./files/fw4-forward.nft $(1)/usr/share/nftables.d/chain-pre/forward_wan/30-q1000k-passthrough.nft
endef

$(eval $(call BuildPackage,q1000k-passthrough))
