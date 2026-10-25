#
# easytierd - EasyTier 服务守护进程（C 重写版）
#
# 源码来自仓库根的 src/，交叉编译后安装 easytierd 与 easytierctl。
#
include $(TOPDIR)/rules.mk

PKG_NAME:=easytierd
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=ospfx

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/easytierd
	SECTION:=net
	CATEGORY:=Network
	TITLE:=EasyTier service daemon (C rewrite of luci-app-easytier backend)
	URL:=https://github.com/EasyTier/luci-app-easytier
	DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libjson-c
endef

define Package/easytierd/description
  EasyTier 服务的 C 语言实现：进程守护、配置管理、防火墙与网络接口编排、
  状态采集、日志、二进制下载/上传安装。替代原 Lua + Shell 后端。
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/../src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -std=gnu11 -D_GNU_SOURCE -Iinclude -Wall -Wextra -Werror" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		STAGING="$(STAGING_DIR)/usr" \
		LIBS="-lubus -lubox -lblobmsg_json -luci -ljson-c" \
		link
endef

define Package/easytierd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/easytierd $(1)/usr/sbin/easytierd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/easytierctl $(1)/usr/sbin/easytierctl
endef

$(eval $(call BuildPackage,easytierd))
