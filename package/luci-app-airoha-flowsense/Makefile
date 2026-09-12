# This is free software, licensed under the Apache License, Version 2.0 .

include $(TOPDIR)/rules.mk

PKG_VERSION:=1.1.9
PKG_RELEASE:=9
PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=LICENSE

LUCI_TITLE:=LuCI Airoha FlowSense — Offload & PPE Monitor
LUCI_DEPENDS:=+luci-base +jshn +jsonfilter +ip-full +ip-bridge +iputils-ping +tc-tiny +kmod-br-netfilter @TARGET_airoha_an7581

define Package/luci-app-airoha-flowsense/conffiles
/etc/config/npu-monitor
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
