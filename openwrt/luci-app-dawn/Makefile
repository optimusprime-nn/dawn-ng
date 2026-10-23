include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for DAWN (with Setup Check)
LUCI_DEPENDS:=+luci-base +dawn

PKG_LICENSE:=Apache-2.0

# Built outside the LuCI tree: pin the version (newer than the feed's) and
# use luci.mk from the installed luci feed. CI overrides PKG_RELEASE.
PKG_VERSION:=26.296.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Nick Hainke <vincent@systemli.org>

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature

