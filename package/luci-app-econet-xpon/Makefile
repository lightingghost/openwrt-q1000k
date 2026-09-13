include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-econet-xpon
PKG_VERSION:=1
PKG_RELEASE:=2
PKG_MAINTAINER:=AK Sharma <maskachoska@yahoo.com>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=COPYING

LUCI_TITLE:=Q1000K XGS-PON diagnostics and identity
LUCI_MAINTAINER:=$(PKG_MAINTAINER)
LUCI_DEPENDS:=+luci-base +q1000k-xgspon
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot package discovery
