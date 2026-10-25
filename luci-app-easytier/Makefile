#
# Copyright (C) 2008-2014 The LuCI Team <luci@lists.subsignal.org>
#
# This is free software, licensed under the Apache License, Version 2.0 .
#
# 前端桥接层安装内容（luci.mk 会把 luasrc/ 与 root/ 整树装进包）：
#   luasrc/controller/easytier.lua      → /usr/lib/lua/luci/controller/easytier.lua（极薄转发壳）
#   luasrc/view/easytier/*.htm          → /usr/lib/lua/luci/view/easytier/
#   root/usr/share/rpcd/acl.d/*.json    → rpcd ACL（UCI easytier + easytierctl exec）
#   root/etc/uci-defaults/luci-easytier → 首次安装注册 ucitrack 并 enable 服务
#   root/usr/share/easytier/i18n/*.json → C 侧渲染用的 i18n 字典
#                                         （tools/i18n_build.py 从 po/ 生成后提交）
#   root/etc/init.d/easytier            → 极薄委托脚本（转发给 easytierd）
#
# 业务逻辑全部在 easytierd/easytierctl（C）里，因此运行时依赖 +easytierd。
#

include $(TOPDIR)/rules.mk
-include $(dir $(lastword $(MAKEFILE_LIST)))../version.mk

PKG_VERSION:=$(or $(EASYTIER_VERSION),2.7.0)
PKG_RELEASE:=1

PKG_PO_VERSION:=$(PKG_VERSION)

LUCI_TITLE:=LuCI support for EasyTier
LUCI_DEPENDS:=+kmod-tun +luci-compat +easytierd
LUCI_PKGARCH:=all

PKG_NAME:=luci-app-easytier

define Package/$(PKG_NAME)/conffiles
/etc/config/easytier
/etc/easytier/
endef

define Package/$(PKG_NAME)/prerm
#!/bin/sh
if [ -f /etc/config/easytier ] ; then
  echo "备份luci配置文件/etc/config/easytier到/tmp/easytier_backup"
  echo "不重启设备之前再次安装luci-app-easytier 配置不丢失,无需重新配置"
  cp -rf /etc/config/easytier /tmp/easytier_backup
fi

if ls /etc/easytier/* >/dev/null 2>&1; then
  echo "备份easytier配置目录/etc/easytier到/tmp/et_config_backup"
  echo "不重启设备之前再次安装luci-app-easytier 配置不丢失,无需重新配置"
  cp -rf /etc/easytier /tmp/et_config_backup
fi

endef

define Package/$(PKG_NAME)/postinst
#!/bin/sh
chmod +x /etc/init.d/easytier
if [ -f /tmp/easytier_backup ] ; then
  echo "发现luci备份配置文件/tmp/easytier_backup，开始恢复到/etc/config/easytier"
  rm -rf /etc/config/easytier
  mv -f /tmp/easytier_backup /etc/config/easytier
  echo "请前往 VPN - EasyTier 界面进行重启插件"
fi
if [ -d /tmp/et_config_backup ] ; then
  echo "发现easytier备份配置目录/tmp/et_config_backup，开始恢复到/etc/easytier"
  rm -rf /etc/easytier
  mv -f /tmp/et_config_backup /etc/easytier
  echo "请前往 VPN - EasyTier 界面进行重启插件"
fi
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
