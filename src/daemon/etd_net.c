/*
 * etd_net.c - 防火墙规则 / EasyTier 网络接口 / 转发规则编排
 *
 * 逐条对齐原实现 init.d/easytier 的 set_firewall()、start_web() 尾部的三条 web 规则、
 * stop_service() 的删除清单，以及"5 秒后后台 commit + reload"逻辑：
 *   1) 端口规则是"先删 section 再建"，永远覆盖（对应 uci -q delete + uci set）；
 *   2) network.EasyTier 同样先删后建；firewall.easytierzone 只在不存在时创建；
 *   3) 转发规则用子串包含判定 et_forward，命中则覆盖、未命中则删除；
 *   4) reload 延迟 5 秒（等拨号/网络事件稳定），同一时刻只允许一个 reload 在跑。
 *
 * 为什么这里直接用 libuci 而不是 etd_uci.c 的封装（原因实测确认，非风格偏好）：
 *   a) et_uci_ensure_sec() 的实现要求 section 已存在（uci_write_ptr 里
 *      `if (!ptr->s) return ET_ENOENT`），因此无法创建 firewall.easytier_tcp_udp
 *      这类新 section；而 `uci set firewall.X=rule` 的语义正是"不存在就建"。
 *   b) et_uci_changed() 基于 uci_package->has_delta，而 libuci 的 file backend 在
 *      uci_file_load() 里无条件写 `package->has_delta = confdir`（confdir 恒为 true），
 *      所以它恒为 true，不能用来判断"有没有改动"；`uci changes` 命令行看的是
 *      package->delta 链表，没有对外暴露。这里改成在模块内记录实际写入。
 *   c) 未提交的改动只活在当前 uci_context 内存里（uci CLI 是靠 /tmp/.uci 的
 *      delta 文件跨进程传递的）。因此 commit 必须在写入它的进程里完成，
 *      fork 出去的子进程只负责"延迟 5 秒执行 init.d reload"。
 */
#include "etd_net.h"
#include "etd_util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <uci.h>
#include <unistd.h>

#define ET_LOCK_STALE_SEC 60
#define ET_PKG_NETWORK "network"
#define ET_PKG_FIREWALL "firewall"

/* 本模块私有的 UCI 游标：写与 commit 必须落在同一个 package 实例上 */
static struct {
	struct uci_context *ctx;
	struct uci_package *network;
	struct uci_package *firewall;
	bool dirty_net; /* 本次进程写过 network */
	bool dirty_fw;	/* 本次进程写过 firewall */
} g_nu;

static const char *cfg_or(et_cfg *c, const char *name, const char *def)
{
	const char *v = et_cfg_str(c, name);
	return (v && *v) ? v : def;
}

static bool has_val(const char *s)
{
	return s && *s;
}

/* ---------------- libuci 薄封装 ---------------- */

static struct uci_package **nu_slot(const char *pkg)
{
	return strcmp(pkg, ET_PKG_NETWORK) == 0 ? &g_nu.network : &g_nu.firewall;
}

static bool *nu_dirty(const char *pkg)
{
	return strcmp(pkg, ET_PKG_NETWORK) == 0 ? &g_nu.dirty_net : &g_nu.dirty_fw;
}

static int nu_open(void)
{
	if (g_nu.ctx)
		return ET_OK;

	struct uci_context *ctx = uci_alloc_context();
	if (!ctx)
		return ET_ENOMEM;

	/* 与 etd_uci.c 一致的测试钩子：允许把配置目录指到别处 */
	const char *dir = getenv("ET_UCI_CONFDIR");
	if (dir && *dir && uci_set_confdir(ctx, dir) != UCI_OK) {
		uci_free_context(ctx);
		return ET_ERR;
	}
	g_nu.ctx = ctx;
	return ET_OK;
}

static struct uci_package *nu_pkg(const char *pkg)
{
	struct uci_package **slot;

	if (nu_open() != ET_OK)
		return NULL;
	slot = nu_slot(pkg);
	if (*slot)
		return *slot;
	if (uci_load(g_nu.ctx, pkg, slot) != UCI_OK) {
		*slot = NULL;
		return NULL;
	}
	return *slot;
}

/* 对应 `uci set <pkg>.<sec>=<type>`：不存在则创建，存在则（必要时）改类型 */
static int nu_ensure_sec(const char *pkg, const char *sec, const char *type)
{
	struct uci_package *p = nu_pkg(pkg);
	struct uci_ptr ptr = { .p = p, .section = sec };

	if (!p)
		return ET_ENOENT;
	if (uci_lookup_ptr(g_nu.ctx, &ptr, NULL, true) != UCI_OK)
		return ET_ERR;

	ptr.value = type;
	if (uci_set(g_nu.ctx, &ptr) != UCI_OK)
		return ET_ERR;

	*nu_dirty(pkg) = true;
	return ET_OK;
}

/* 对应 `uci set <pkg>.<sec>.<opt>=<val>` */
static int nu_set(const char *pkg, const char *sec, const char *opt,
		  const char *val)
{
	struct uci_package *p = nu_pkg(pkg);
	struct uci_ptr ptr = {
		.p = p,
		.section = sec,
		.option = opt,
		.value = val,
	};

	if (!p)
		return ET_ENOENT;
	if (uci_lookup_ptr(g_nu.ctx, &ptr, NULL, true) != UCI_OK)
		return ET_ERR;
	if (uci_set(g_nu.ctx, &ptr) != UCI_OK)
		return ET_ERR;

	*nu_dirty(pkg) = true;
	return ET_OK;
}

/* 对应 `uci -q delete <pkg>.<sec>`；不存在不算错 */
static int nu_del_sec(const char *pkg, const char *sec)
{
	struct uci_package *p = nu_pkg(pkg);
	struct uci_ptr ptr = { .p = p, .section = sec };

	if (!p)
		return ET_ENOENT;
	if (uci_lookup_ptr(g_nu.ctx, &ptr, NULL, true) != UCI_OK)
		return ET_ERR;
	if (!ptr.s)
		return ET_ENOENT;
	if (uci_delete(g_nu.ctx, &ptr) != UCI_OK)
		return ET_ERR;

	*nu_dirty(pkg) = true;
	return ET_OK;
}

/* 对应 `uci -q get <pkg>.<sec>`：section 是否存在 */
static bool nu_sec_exists(const char *pkg, const char *sec)
{
	struct uci_package *p = nu_pkg(pkg);
	struct uci_ptr ptr = { .p = p, .section = sec };

	if (!p)
		return false;
	if (uci_lookup_ptr(g_nu.ctx, &ptr, NULL, true) != UCI_OK)
		return false;
	return ptr.s != NULL;
}

static int nu_commit(const char *pkg)
{
	struct uci_package **slot = nu_slot(pkg);

	if (!nu_pkg(pkg))
		return ET_ENOENT;
	/* uci_commit 会重排 package 指针，直接传槽位让它就地更新 */
	if (uci_commit(g_nu.ctx, slot, false) != UCI_OK)
		return ET_ERR;
	return ET_OK;
}

/* ---------------- 单条 rule ---------------- */

/* 对应：uci -q delete firewall.<sec> ; uci set firewall.<sec>=rule ; 逐个 option */
static int fw_rule(const char *sec, const char *proto, const char *port,
		   const char *logfile)
{
	if (!has_val(sec) || !has_val(port))
		return ET_EINVAL;

	et_log_to(logfile, "添加防火墙规则 %s 放行端口 %s", sec, port);

	nu_del_sec(ET_PKG_FIREWALL, sec);
	if (nu_ensure_sec(ET_PKG_FIREWALL, sec, "rule") < 0)
		return ET_ERR;

	nu_set(ET_PKG_FIREWALL, sec, "name", sec);
	nu_set(ET_PKG_FIREWALL, sec, "target", "ACCEPT");
	nu_set(ET_PKG_FIREWALL, sec, "src", "wan");
	nu_set(ET_PKG_FIREWALL, sec, "proto", proto);
	nu_set(ET_PKG_FIREWALL, sec, "dest_port", port);
	nu_set(ET_PKG_FIREWALL, sec, "enabled", "1");
	return ET_OK;
}

/* 对应：if [ -z "$(uci -q get firewall.easytierzone)" ] ; then uci set ... ; fi
 * 已存在时一个字段都不动（旧实现就是"已存在则不覆盖"）。 */
static void fw_zone_ensure(void)
{
	if (nu_sec_exists(ET_PKG_FIREWALL, "easytierzone"))
		return;

	et_log_to(ET_LOG_CORE,
		  "添加防火墙规则，放行网络接口 EasyTier 允许出入转发，开启IP动态伪装 MSS钳制");

	if (nu_ensure_sec(ET_PKG_FIREWALL, "easytierzone", "zone") < 0)
		return;
	nu_set(ET_PKG_FIREWALL, "easytierzone", "input", "ACCEPT");
	nu_set(ET_PKG_FIREWALL, "easytierzone", "output", "ACCEPT");
	nu_set(ET_PKG_FIREWALL, "easytierzone", "forward", "ACCEPT");
	nu_set(ET_PKG_FIREWALL, "easytierzone", "masq", "1");
	nu_set(ET_PKG_FIREWALL, "easytierzone", "mtu_fix", "1");
	nu_set(ET_PKG_FIREWALL, "easytierzone", "name", "EasyTier");
	nu_set(ET_PKG_FIREWALL, "easytierzone", "network", "EasyTier");
}

/* 对应 uci set firewall.<sec>=forwarding ; uci set .dest ; uci set .src
 * 命中就覆盖（旧实现没有先 delete，所以是覆盖语义）。 */
static void fw_forward(const char *sec, const char *dest, const char *src,
		       bool on, const char *msg)
{
	if (!on) {
		nu_del_sec(ET_PKG_FIREWALL, sec);
		return;
	}
	et_log_to(ET_LOG_CORE, "%s", msg);
	if (nu_ensure_sec(ET_PKG_FIREWALL, sec, "forwarding") < 0)
		return;
	nu_set(ET_PKG_FIREWALL, sec, "dest", dest);
	nu_set(ET_PKG_FIREWALL, sec, "src", src);
}

/* 旧实现用 ${et_forward//etfwlan/} != "$et_forward" 判定，即"包含"语义 */
static bool contains(const char *hay, const char *needle)
{
	return hay && needle && strstr(hay, needle) != NULL;
}

/* et_forward 在 UCI 里是 list，`uci -q get` 取到的是空格连接的整体；
 * 这里逐个元素做子串判定，效果一致（单个元素里写多个值时也能命中）。 */
static bool cfg_contains(et_cfg *c, const char *name, const char *needle)
{
	const et_list *l = et_cfg_list(c, name);

	if (l) {
		for (size_t i = 0; i < l->n; i++) {
			if (contains(l->v[i], needle))
				return true;
		}
	}
	return contains(et_cfg_str(c, name), needle);
}

int et_net_apply_firewall(et_cfg *c, const et_fw_ports *override)
{
	if (!c)
		return ET_EINVAL;

	if (strcmp(cfg_or(c, "auto_config_firewall", "1"), "1") != 0) {
		et_log_to(ET_LOG_CORE, "自动防火墙配置已禁用，跳过配置");
		return ET_OK;
	}

	const char *tcp = override && override->tcp ? override->tcp
						    : cfg_or(c, "tcp_port", NULL);
	const char *udp = override && override->udp ? override->udp
						    : cfg_or(c, "udp_port", NULL);
	const char *ws = override && override->ws ? override->ws
						  : cfg_or(c, "ws_port", NULL);
	const char *wss = override && override->wss ? override->wss
						    : cfg_or(c, "wss_port", NULL);
	const char *wg = override && override->wg ? override->wg
						  : cfg_or(c, "wg_port", NULL);
	const char *quic = override && override->quic ? override->quic
						      : cfg_or(c, "quic_port", NULL);
	const char *tunname = override && override->tunname ? override->tunname
							   : cfg_or(c, "tunname", NULL);

	/* listenermode=ON 或 web 模式下，四个端口全空时补默认值。
	 * 旧实现在 core argv 拼好之后才补，所以只影响防火墙，不影响启动参数；
	 * listenermode 缺省时不补（旧实现 `[ "$listenermode" = "ON" ]` 为假）。 */
	bool listener_on = !strcmp(cfg_or(c, "listenermode", ""), "ON");
	bool web_mode = !strcmp(cfg_or(c, "etcmd", "etcmd"), "web");
	if (listener_on || web_mode) {
		if (!has_val(tcp) && !has_val(ws) && !has_val(wss) &&
		    !has_val(wg)) {
			tcp = ET_DEFAULT_TCP_PORT;
			ws = ET_DEFAULT_WS_PORT;
			wss = ET_DEFAULT_WSS_PORT;
			wg = ET_DEFAULT_WG_PORT;
		}
	}

	if (has_val(tcp))
		fw_rule("easytier_tcp_udp", "tcp udp", tcp, ET_LOG_CORE);
	if (has_val(udp))
		fw_rule("easytier_udp", "udp", udp, ET_LOG_CORE);
	if (has_val(wss))
		fw_rule("easytier_wss", "tcp", wss, ET_LOG_CORE);
	if (has_val(ws))
		fw_rule("easytier_ws", "tcp", ws, ET_LOG_CORE);
	if (has_val(wg))
		fw_rule("easytier_wg", "udp", wg, ET_LOG_CORE);
	if (has_val(quic))
		fw_rule("easytier_quic", "tcp udp", quic, ET_LOG_CORE);

	/* 旧实现：socks5 端口不再自动放行（无认证代理暴露公网有风险），这里同样不建。 */

	if (!has_val(tunname))
		tunname = "tun0";

	if (!strcmp(cfg_or(c, "auto_config_interface", "1"), "1")) {
		et_log_to(ET_LOG_CORE, "自动网络接口配置已启用");

		const char *ipaddr = cfg_or(c, "ipaddr", NULL);
		const char *netmask = cfg_or(c, "interface_netmask", NULL);

		/* 旧实现先 delete 再判空，判空永远成立，等价于每次重建 */
		nu_del_sec(ET_PKG_NETWORK, "EasyTier");
		if (nu_ensure_sec(ET_PKG_NETWORK, "EasyTier", "interface") < 0)
			return ET_ERR;

		if (!has_val(ipaddr)) {
			nu_set(ET_PKG_NETWORK, "EasyTier", "proto", "none");
		} else {
			nu_set(ET_PKG_NETWORK, "EasyTier", "proto", "static");
			nu_set(ET_PKG_NETWORK, "EasyTier", "ipaddr", ipaddr);
			if (!has_val(netmask))
				netmask = "255.0.0.0";
			nu_set(ET_PKG_NETWORK, "EasyTier", "netmask", netmask);
		}
		et_log_to(ET_LOG_CORE,
			  "添加网络接口 EasyTier 绑定虚拟接口 %s，子网掩码: %s",
			  tunname, has_val(netmask) ? netmask : "255.0.0.0");
		nu_set(ET_PKG_NETWORK, "EasyTier", "device", tunname);
		nu_set(ET_PKG_NETWORK, "EasyTier", "ifname", tunname);

		fw_zone_ensure();
	} else {
		et_log_to(ET_LOG_CORE, "自动网络接口配置已禁用，不创建网络接口");
		nu_del_sec(ET_PKG_NETWORK, "EasyTier");
		nu_del_sec(ET_PKG_FIREWALL, "easytierzone");
	}

	fw_forward("easytierfwlan", "lan", "EasyTier",
		   cfg_contains(c, "et_forward", "etfwlan"),
		   "允许从虚拟网络 EasyTier 到局域网 lan 的流量");
	fw_forward("easytierfwwan", "wan", "EasyTier",
		   cfg_contains(c, "et_forward", "etfwwan"),
		   "允许从虚拟网络 EasyTier 到广域网 wan 的流量");
	fw_forward("lanfweasytier", "EasyTier", "lan",
		   cfg_contains(c, "et_forward", "lanfwet"),
		   "允许从局域网 lan 到虚拟网络 EasyTier 的流量");
	fw_forward("wanfweasytier", "EasyTier", "wan",
		   cfg_contains(c, "et_forward", "wanfwet"),
		   "允许从广域网 wan 到虚拟网络 EasyTier 的流量");

	return ET_OK;
}

int et_net_apply_web_firewall(et_cfg *c)
{
	if (!c)
		return ET_EINVAL;

	const char *web_port = cfg_or(c, "web_port", NULL);
	const char *api_port = cfg_or(c, "web_api_port", NULL);
	const char *html_port = cfg_or(c, "web_html_port", NULL);
	bool fw_web = !strcmp(cfg_or(c, "web_fw_web", "0"), "1");
	bool fw_api = !strcmp(cfg_or(c, "web_fw_api", "0"), "1");

	if (has_val(web_port) && fw_web) {
		et_log_to(ET_LOG_WEB,
			  "添加防火墙规则 easytier_web 放行服务端口 %s", web_port);
		fw_rule("easytier_webserver", "tcp udp", web_port, ET_LOG_WEB);
	}
	if (has_val(api_port) && fw_api) {
		et_log_to(ET_LOG_WEB, "添加防火墙规则 easytier_web 放行API端口 %s",
			  api_port);
		fw_rule("easytier_webapi", "tcp", api_port, ET_LOG_WEB);
	}
	if (has_val(html_port) && fw_api &&
	    strcmp(html_port, has_val(api_port) ? api_port : "") != 0) {
		et_log_to(ET_LOG_WEB,
			  "添加防火墙规则 easytier_web 放行html端口 %s", html_port);
		fw_rule("easytier_webhtml", "tcp", html_port, ET_LOG_WEB);
	}
	return ET_OK;
}

int et_net_cleanup_all(void)
{
	nu_del_sec(ET_PKG_NETWORK, "EasyTier");

	static const char *secs[] = {
		"easytierzone",	  "easytierfwlan",    "easytierfwwan",
		"lanfweasytier",  "wanfweasytier",    "easytier_tcp",
		"easytier_udp",	  "easytier_tcp_udp", "easytier_wss",
		"easytier_ws",	  "easytier_wg",      "easytier_quic",
		"easytier_wireguard", "easytier_socks5",
	};
	for (size_t i = 0; i < sizeof(secs) / sizeof(secs[0]); i++)
		nu_del_sec(ET_PKG_FIREWALL, secs[i]);
	return ET_OK;
}

int et_net_cleanup_web(void)
{
	nu_del_sec(ET_PKG_FIREWALL, "easytier_webserver");
	nu_del_sec(ET_PKG_FIREWALL, "easytier_webapi");
	nu_del_sec(ET_PKG_FIREWALL, "easytier_webhtml");
	return ET_OK;
}

int et_net_ip_forward_enable(void)
{
	/* /proc 不能 rename 覆盖，必须直接 write */
	int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
	if (fd < 0)
		return ET_ERR;
	const char v[] = "1\n";
	ssize_t w = write(fd, v, sizeof(v) - 1);
	close(fd);
	return w == (ssize_t)(sizeof(v) - 1) ? ET_OK : ET_ERR;
}

/* ---------------- 延迟重载 ---------------- */

static void et_reload_initd(const char *name)
{
	char path[64];
	snprintf(path, sizeof(path), "/etc/init.d/%s", name);
	if (!et_file_exists(path))
		return;
	char *argv[] = { path, (char *)"reload", NULL };
	et_run(argv, NULL, NULL, 30);
}

/* 子进程：等 5 秒再 reload，锁文件防止并发重载 */
static void et_reload_delayed(void)
{
	sleep(5);

	et_mkdir_p("/var/lock", 0755);
	int fd = open(ET_LOCK_RELOAD, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0 && errno == EEXIST) {
		/* 上次 reload 被中途杀掉会留下锁文件；超过 60 秒视为残留并接管，
		 * 否则这个锁会让 reload 永远不再生效。 */
		struct stat st;
		if (stat(ET_LOCK_RELOAD, &st) == 0 &&
		    time(NULL) - st.st_mtime > ET_LOCK_STALE_SEC) {
			unlink(ET_LOCK_RELOAD);
			fd = open(ET_LOCK_RELOAD, O_WRONLY | O_CREAT | O_EXCL,
				  0644);
		}
	}
	if (fd < 0)
		return;
	close(fd);

	/* 未提交改动已由父进程 commit，这里只负责让 netifd / fw3 重新生成规则 */
	if (g_nu.dirty_net)
		et_reload_initd("network");
	if (g_nu.dirty_fw)
		et_reload_initd("firewall");

	unlink(ET_LOCK_RELOAD);
	et_log_to(ET_LOG_CORE, "网络配置重载完成");
}

void et_net_schedule_reload(const char *why)
{
	if (!g_nu.dirty_net && !g_nu.dirty_fw)
		return;

	if (why && *why)
		et_log_to(ET_LOG_CORE, "%s", why);

	/* 必须在本进程 commit：libuci 的未提交改动只在该 uci_context 的内存里 */
	if (g_nu.dirty_net)
		nu_commit(ET_PKG_NETWORK);
	if (g_nu.dirty_fw)
		nu_commit(ET_PKG_FIREWALL);

	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		/* 双 fork：中间进程立刻退出，真正的重载进程脱离父进程。
		 * 子进程继承父进程的 g_nu.dirty_* 副本，据此决定 reload 哪一个。 */
		pid_t pid2 = fork();
		if (pid2 != 0)
			_exit(0);
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		et_reload_delayed();
		_exit(0);
	}
	/* 回收中间进程，避免留下僵尸 */
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;

	g_nu.dirty_net = false;
	g_nu.dirty_fw = false;
}
