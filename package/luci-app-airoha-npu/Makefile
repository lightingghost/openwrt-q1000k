# This is free software, licensed under the Apache License, Version 2.0 .

include $(TOPDIR)/rules.mk

PKG_VERSION:=1.0.3
PKG_RELEASE:=4
PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=LICENSE

LUCI_TITLE:=LuCI Airoha SoC Status (NPU, CPU, Temperatures)
LUCI_DEPENDS:=+luci-base +jshn +jsonfilter +kmod-br-netfilter @TARGET_airoha_an7581

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
