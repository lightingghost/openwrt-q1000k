# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=q1000k-omci
PKG_RELEASE:=7
PKG_EXTMOD_SUBDIRS:=net/xpon
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Matheus Sampaio Queiroga <srherobrine20@gmail.com>

include $(INCLUDE_DIR)/package.mk

define KernelPackage/q1000k-omci
  SUBMENU:=Network Devices
  TITLE:=Experimental Q1000K PON and OMCI core
  DEPENDS:=@BROKEN @TARGET_airoha_an7581
  KCONFIG:=CONFIG_LEDS_CLASS=y
  FILES:= \
	$(PKG_BUILD_DIR)/net/xpon/xpon.ko \
	$(PKG_BUILD_DIR)/net/xpon/omci/omci.ko
  # The hardware provider must establish identity and authenticated transport.
  # No autoload or board activation from this package.
endef

define KernelPackage/q1000k-omci/description
 Experimental generic PON state and OMCI protocol core, imported from
 airoha_kernel and adapted for strict Q1000K transport and service handling.
 Does not provide a hardware driver or enable the optical transmitter.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/net/xpon" \
		CONFIG_XPON=m CONFIG_XPON_OMCI=m CONFIG_XPON_OAM= \
		KCFLAGS="-I$(PKG_BUILD_DIR)/include" modules
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/q1000k-omci
	$(CP) $(PKG_BUILD_DIR)/include/. $(1)/usr/include/q1000k-omci/
	$(INSTALL_DIR) $(1)/usr/include/net/xpon $(1)/usr/include/uapi/linux
	$(CP) $(PKG_BUILD_DIR)/include/net/xpon.h $(1)/usr/include/net/
	$(CP) $(PKG_BUILD_DIR)/include/net/xpon/omci.h $(1)/usr/include/net/xpon/
	$(CP) $(PKG_BUILD_DIR)/include/uapi/linux/{xpon,omci}.h $(1)/usr/include/uapi/linux/
	$(INSTALL_DIR) $(1)/usr/share/q1000k-omci
	$(CP) $(LINUX_DIR)/../symvers/q1000k-omci.symvers $(1)/usr/share/q1000k-omci/Module.symvers
endef

$(eval $(call KernelPackage,q1000k-omci))
