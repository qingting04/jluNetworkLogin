include $(TOPDIR)/rules.mk

PKG_NAME:=jluNetworkLogin
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/jluNetworkLogin
  SECTION:=net
  CATEGORY:=Network
  TITLE:=JLU DrCOM client daemon
  DEPENDS:=+libubus +libubox +libuci
endef

define Package/jluNetworkLogin/description
  Lightweight DrCOM client daemon (challenge/login/keepalive) for campus networks.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -std=c11 -D_GNU_SOURCE \
		-I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/jlu-network-login \
		$(PKG_BUILD_DIR)/drcomd.c \
		$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -lubus -lubox -luci
endef

define Package/jluNetworkLogin/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/jlu-network-login $(1)/usr/sbin/jlu-network-login
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/jlu-network-login.init $(1)/etc/init.d/jlu-network-login
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/jlu-network-login.config $(1)/etc/config/jlu-network-login
endef

$(eval $(call BuildPackage,jluNetworkLogin))
