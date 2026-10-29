#
# OpenWrt package for DAWN, built from the source in this repository
# (../../src) rather than downloaded. Based on net/dawn in openwrt/packages.
#
# The version is deliberately newer than the official package so a routine
# upgrade does not silently replace a test build with the stock one.
# CI overrides PKG_RELEASE with the workflow run number.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=dawn
PKG_VERSION:=2026.10.22
PKG_RELEASE:=1

PKG_MAINTAINER:=Nick Hainke <vincent@systemli.org>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE

PKG_BUILD_FLAGS:=no-lto

# Repository root, resolved through the feed symlinks
DAWN_SRC_DIR:=$(abspath $(CURDIR)/../..)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/dawn
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Wireless
  TITLE:=Decentralized wifi controller
  URL:=https://github.com/berlin-open-wireless-lab/DAWN.git
  DEPENDS:=$(DRV_DEPENDS) +libubus +libubox +libblobmsg-json +libuci +libgcrypt +libiwinfo +umdns
endef

define Package/dawn/description
  This package implements a decentralized wireless daemon.
  Development build with network message hardening.
  Also installs the public keys this repository's CI signs its packages
  with (/etc/apk/keys/dawn-dev.pem, /etc/opkg/keys/af358956e09d0655), so
  later builds install without --allow-untrusted, including through
  LuCI's "Upload Package...".
endef

define Package/dawn/conffiles
/etc/config/dawn
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(DAWN_SRC_DIR)/CMakeLists.txt $(DAWN_SRC_DIR)/LICENSE $(DAWN_SRC_DIR)/src $(PKG_BUILD_DIR)/
endef

define Package/dawn/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/dawn $(1)/usr/sbin/dawn
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/dawn.init $(1)/etc/init.d/dawn
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(DAWN_SRC_DIR)/dawn-config $(1)/etc/config/dawn
	$(INSTALL_DIR) $(1)/etc/apk/keys $(1)/etc/opkg/keys
	$(INSTALL_DATA) $(DAWN_SRC_DIR)/keys/dawn-dev.pem $(1)/etc/apk/keys/dawn-dev.pem
	$(INSTALL_DATA) $(DAWN_SRC_DIR)/keys/af358956e09d0655 $(1)/etc/opkg/keys/af358956e09d0655
endef

$(eval $(call BuildPackage,dawn))
