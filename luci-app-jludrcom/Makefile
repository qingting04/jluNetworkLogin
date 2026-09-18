include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-jludrcom
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

LUCI_TITLE:=吉林大学 DrCOM 客户端
LUCI_DEPENDS:=+drcomd
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

$(eval $(call BuildPackage,luci-app-jludrcom))
