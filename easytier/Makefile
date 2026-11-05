# 版本号跟随仓库根的 version.mk（EASYTIER_VERSION）。
# 注意：必须在 include rules.mk 之前取 $(lastword $(MAKEFILE_LIST))，否则拿到的是
# OpenWrt 的头文件路径，-include 会静默失败，版本号一直吃下面的兜底值。
ETD_MK_DIR:=$(dir $(lastword $(MAKEFILE_LIST)))

include $(TOPDIR)/rules.mk
-include $(ETD_MK_DIR)../version.mk

PKG_NAME:=easytier
PKG_VERSION:=$(or $(EASYTIER_VERSION),2.7.0)

ifeq ($(ARCH),mipsel)
	APP_ARCH:=mipsel
endif
ifeq ($(ARCH),mips)
	APP_ARCH:=mips
endif
ifeq ($(ARCH),arm)
	APP_ARCH:=arm
endif
ifeq ($(BOARD),kirkwood)
	APP_ARCH:=arm
endif
ifeq ($(ARCH),armv7)
	APP_ARCH:=armv7
endif
ifeq ($(ARCH),aarch64)
	APP_ARCH:=aarch64
endif
ifeq ($(ARCH),arm64)
	APP_ARCH:=aarch64
endif
ifeq ($(ARCH),armv8)
	APP_ARCH:=aarch64
endif
ifeq ($(ARCH),x86_64)
	APP_ARCH:=x86_64
endif

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

RSTRIP:=:

define Package/$(PKG_NAME)
	SECTION:=net
	CATEGORY:=Network
	TITLE:=A simple, decentralized mesh VPN with WireGuard support.
	DEPENDS:=@(x86_64||arm||aarch64||mipsel||mips) +kmod-tun
	URL:=https://github.com/EasyTier/EasyTier
	MENU:=1
endef

define Package/$(PKG_NAME)/description
  A simple, decentralized mesh VPN with WireGuard support.
endef

define Package/$(PKG_NAME)/config
	config EASYTIER_INCLUDE_WEBCONSOLE
		bool "Include Web Console (easytier-web)"
		depends on PACKAGE_$(PKG_NAME)
		depends on !(mips || mipsel)
		default y
		help
		  Install the easytier-web web console.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	if [ ! -f $(PKG_BUILD_DIR)/easytier-core ]; then \
		wget https://github.com/EasyTier/EasyTier/releases/download/v$(PKG_VERSION)/$(PKG_NAME)-linux-$(APP_ARCH)-v$(PKG_VERSION).zip -O $(PKG_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION).zip; \
		unzip -o -j $(PKG_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION).zip -d $(PKG_BUILD_DIR); \
		rm -f $(PKG_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION).zip; \
	fi
endef

define Build/Compile
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(CP) $(PKG_BUILD_DIR)/easytier-core $(1)/usr/bin/
	chmod 0755 $(1)/usr/bin/easytier-core
	$(CP) $(PKG_BUILD_DIR)/easytier-cli $(1)/usr/bin/
	chmod 0755 $(1)/usr/bin/easytier-cli
ifdef CONFIG_EASYTIER_INCLUDE_WEBCONSOLE
	$(CP) $(PKG_BUILD_DIR)/easytier-web-embed $(1)/usr/bin/easytier-web
	chmod 0755 $(1)/usr/bin/easytier-web
endif
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
