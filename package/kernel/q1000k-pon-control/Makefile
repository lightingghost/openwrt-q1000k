# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=q1000k-pon-control
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/q1000k-pon-control
  SUBMENU:=Network Devices
  TITLE:=Q1000K EN7573 optical controller bring-up
  DEPENDS:=@TARGET_airoha_an7581 +kmod-i2c-core +kmod-crypto-sha256
  FILES:=$(PKG_BUILD_DIR)/q1000k-pon-control.ko
  KCONFIG:=
endef

define KernelPackage/q1000k-pon-control/description
 Board selection, controller detection and verified EN7573 firmware loading.
 Optical transmission remains disabled. No automatic loading or PON MAC is
 included; this is an optional development component.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,q1000k-pon-control))
