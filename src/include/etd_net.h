/*
 * etd_net.h - 防火墙与网络接口编排（私有头，仅 daemon 内部使用）
 *
 * 对应原实现 init.d/easytier 的 set_firewall()、start_web() 尾部的 web 规则、
 * stop_service() 里的删除清单，以及"延迟重载网络/防火墙"逻辑。
 * 所有 section 名、option 名、取值都与旧实现逐字一致。
 *
 * 实现说明：这里自持一个 libuci 游标（不用 etd_uci 封装），原因见 etd_net.c 顶部注释
 * —— 封装层无法创建 section，且未提交改动不能跨进程共享。
 */
#ifndef ETD_NET_H
#define ETD_NET_H

#include "etd.h"
#include "etd_cfg.h"

/* 需要放行的端口集合。
 * 普通模式从 UCI 推导；config 模式（-c config.toml）由 toml 覆盖，
 * 与旧实现 check_config() 从 toml grep 端口的做法一致。 */
typedef struct {
	const char *tcp;
	const char *udp;
	const char *ws;
	const char *wss;
	const char *wg;
	const char *quic;
	const char *tunname; /* 覆盖 --dev-name / network.EasyTier.device */
} et_fw_ports;

/* 应用 set_firewall() 的全部规则。
 * override 为 NULL 时端口与 tunname 全部按 UCI 推导（含 listenermode=ON 的默认端口补全）；
 * override 非 NULL 时用其中的值，但默认端口补全仍然生效。 */
int et_net_apply_firewall(et_cfg *c, const et_fw_ports *override);

/* start_web() 尾部的三条 web 防火墙规则（easytier_webserver/webapi/webhtml） */
int et_net_apply_web_firewall(et_cfg *c);

/* stop_service() 的删除清单：network.EasyTier + 全部 firewall.easytier* */
int et_net_cleanup_all(void);
/* stop_service() 中 web_enabled=0 的删除清单 */
int et_net_cleanup_web(void);

/* 本进程写过 network/firewall 时：commit + 5 秒后后台 reload（锁文件防重复）。
 * why 为写入 core 日志的说明文案。 */
void et_net_schedule_reload(const char *why);

/* 等价 sysctl -w net.ipv4.ip_forward=1；失败不致命 */
int et_net_ip_forward_enable(void);

#endif /* ETD_NET_H */
