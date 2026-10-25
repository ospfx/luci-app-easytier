/*
 * etd_argv.c - easytier-core / easytier-web 命令行拼装
 *
 * 保真度说明（务必对照 luci-app-easytier/root/etc/init.d/easytier）：
 *   - argv 顺序 = et_schema[] 的行顺序 = init.d start_et() 里 core_cmd 的追加顺序
 *   - 旧实现是把参数拼进一个字符串再交给 `/bin/sh -c`，所以 shell 会做二次分词；
 *     这里直接把"最终 argv"算出来，语义等价的前提是每个值本身不含空白
 *   - `--console-log-level` 不在本文件拼装：由调用方在末尾追加（旧实现的
 *     `... ${core_cmd} --console-log-level ${log}`），两种启动方式都追加
 */
#include "etd_cfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

static void av_flag(et_list *av, const char *flag)
{
	et_list_add(av, flag);
}

static void av_pair(et_list *av, const char *flag, const char *val)
{
	et_list_add(av, flag);
	et_list_add(av, val);
}

static void note(et_buf *notes, const char *fmt, ...)
{
	va_list ap;

	if (!notes)
		return;
	va_start(ap, fmt);
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	et_buf_add(notes, msg);
	et_buf_addc(notes, '\n');
}

/* 通用发射规则；val/list 由调用方按字段类型给出 */
static void emit_generic(et_list *av, const char *flag, enum et_emit e,
			 const char *val, const et_list *list)
{
	if (!flag)
		return;

	switch (e) {
	case ET_E_VAL:
		if (val && *val)
			av_pair(av, flag, val);
		break;
	case ET_E_YES: /* [ "$x" = "0" ] || flag */
		if (!val || strcmp(val, "0") != 0)
			av_flag(av, flag);
		break;
	case ET_E_NO: /* [ "$x" = "1" ] || flag */
		if (!val || strcmp(val, "1") != 0)
			av_flag(av, flag);
		break;
	case ET_E_ONE: /* [ "$x" = "1" ] && flag */
		if (val && strcmp(val, "1") == 0)
			av_flag(av, flag);
		break;
	case ET_E_YESVAL: /* [ "$x" = "0" ] || "flag true" */
		if (!val || strcmp(val, "0") != 0)
			av_pair(av, flag, "true");
		break;
	case ET_E_ONEVAL: /* [ "$x" = "1" ] && "flag true" */
		if (val && strcmp(val, "1") == 0)
			av_pair(av, flag, "true");
		break;
	case ET_E_BOOLVAL: /* 始终 "flag true|false" */
		av_pair(av, flag, (val && strcmp(val, "0") == 0) ? "false" : "true");
		break;
	case ET_E_REPEAT:
		if (list) {
			for (size_t i = 0; i < list->n; i++)
				av_pair(av, flag, list->v[i]);
		}
		break;
	case ET_E_REPEAT_JOIN: {
		et_buf b;

		if (!list || list->n == 0)
			break;
		et_buf_init(&b);
		for (size_t i = 0; i < list->n; i++) {
			if (i)
				et_buf_addc(&b, ' ');
			et_buf_add(&b, list->v[i]);
		}
		et_list_add(av, flag);
		et_list_add(av, b.p ? b.p : "");
		et_buf_free(&b);
		break;
	}
	case ET_E_NONE:
	case ET_E_CUSTOM:
		break;
	}
}

/* ---------------- core 特殊规则 ---------------- */

/* <scheme>_port -> -l <scheme>:<port>；tcp 额外再输出一条 -l udp:<port>
 * （init.d:546-547，旧实现用同一个 tcp_port 开 TCP 与 UDP 两个监听） */
static void emit_port(et_list *av, et_cfg *c, const et_field *f)
{
	static const char suffix[] = "_port";
	const char *v = et_cfg_str(c, f->name);
	size_t nl, sl = sizeof(suffix) - 1;
	char scheme[16];

	if (!v || !*v)
		return;
	nl = strlen(f->name);
	if (nl <= sl || strcmp(f->name + nl - sl, suffix) != 0)
		return;
	snprintf(scheme, sizeof(scheme), "%.*s", (int)(nl - sl), f->name);

	{
		char *arg = NULL;

		if (asprintf(&arg, "%s:%s", scheme, v) < 0)
			return;
		et_list_add(av, "-l");
		et_list_add_owned(av, arg);
	}
	if (strcmp(scheme, "tcp") == 0) {
		char *arg = NULL;

		if (asprintf(&arg, "udp:%s", v) < 0)
			return;
		et_list_add(av, "-l");
		et_list_add_owned(av, arg);
	}
}

/* relay_network=1 时才发射 --relay-network-whitelist；whitelist 为空时
 * 输出**裸旗标**（init.d:513-525 的 else 分支，属旧实现怪行为，按要求保留） */
static void emit_relay_whitelist(et_list *av, et_cfg *c)
{
	const char *v = et_cfg_str(c, "relay_network");
	const et_list *wl = et_cfg_list(c, "whitelist");

	if (!v || strcmp(v, "1") != 0)
		return;
	if (wl && wl->n) {
		for (size_t i = 0; i < wl->n; i++)
			av_pair(av, "--relay-network-whitelist", wl->v[i]);
	} else {
		av_flag(av, "--relay-network-whitelist");
	}
}

/* EasyTier 2.7.0 起 --vpn-portal 只接受 wg://主机:端口；
 * wg://主机:端口/网段（2.6.4 旧写法）与缺私钥的情况都要跳过并写 notes
 * （init.d:556-575） */
static void emit_vpn_portal(et_list *av, et_cfg *c, et_buf *notes)
{
	const char *v = et_cfg_str(c, "vpn_portal");
	const char *key = et_cfg_str(c, "vpn_portal_private_key");

	if (!v || !*v)
		return;
	if (!et_starts_with(v, "wg://")) {
		note(notes, "vpn_portal: 地址 %s 格式不正确，应为 wg://主机:端口，已跳过门户启动", v);
		return;
	}
	if (strchr(v + 5, '/')) {
		note(notes, "vpn_portal: 地址 %s 是 2.6.4 旧写法（网段写在 URL 里），"
			    "EasyTier 2.7.0 起只接受 wg://主机:端口，已跳过门户启动；"
			    "请把客户端网段填入 VPN 门户客户端列表", v);
		return;
	}
	if (!key || !*key) {
		note(notes, "vpn_portal: 已配置 VPN 门户但未填写门户私钥，"
			    "EasyTier 2.7.0 起门户必须有私钥，已跳过门户启动");
		return;
	}
	av_pair(av, "--vpn-portal", v);
	/* 私钥不进程命令行传递（旧实现用 procd_set_param env 下发） */
	note(notes, "vpn_portal_private_key 需由调用方以环境变量 ET_VPN_PORTAL_PRIVATE_KEY 下发给"
		    " easytier-core，不出现在 argv 中");
}

static void emit_core_custom(et_list *av, et_cfg *c, const et_field *f,
			     et_buf *notes)
{
	const char *name = f->name;
	const char *v;

	if (!strcmp(name, "tcp_port") || !strcmp(name, "ws_port") ||
	    !strcmp(name, "wss_port") || !strcmp(name, "wg_port") ||
	    !strcmp(name, "quic_port")) {
		emit_port(av, c, f);
		return;
	}
	if (!strcmp(name, "relay_network")) {
		emit_relay_whitelist(av, c);
		return;
	}
	if (!strcmp(name, "vpn_portal")) {
		emit_vpn_portal(av, c, notes);
		return;
	}
	if (!strcmp(name, "listenermode")) {
		/* [ "$listenermode" = "ON" ] || --no-listener（init.d:553） */
		v = et_cfg_str(c, name);
		if (!v || strcmp(v, "ON") != 0)
			av_flag(av, "--no-listener");
		return;
	}
	if (!strcmp(name, "default_protocol")) {
		/* [ "$default_protocol" = "-" ] || --default-protocol $x（init.d:587） */
		v = et_cfg_str(c, name);
		if (!v || !*v) {
			note(notes, "default_protocol: 空值，已跳过 --default-protocol"
				    "（旧实现会输出裸旗标，easytier-core 会因缺少参数报错）");
			return;
		}
		if (strcmp(v, "-") != 0)
			av_pair(av, "--default-protocol", v);
		return;
	}
	if (!strcmp(name, "multi_thread")) {
		/* 2.7.0 起默认值变了，旧实现始终显式传值（init.d:590-595） */
		v = et_cfg_str(c, name);
		av_pair(av, "--multi-thread", (v && strcmp(v, "0") == 0) ? "false" : "true");
		return;
	}
	if (!strcmp(name, "comp")) {
		/* [ "$comp" = "none" ] || --compression $comp（init.d:605） */
		v = et_cfg_str(c, name);
		if (v && *v && strcmp(v, "none") != 0)
			av_pair(av, "--compression", v);
		return;
	}
	if (!strcmp(name, "bind_device")) {
		/* init.d:606-607 两行独立生效：非 "0" 出 true，非 "1" 出 false */
		v = et_cfg_str(c, name);
		if (!v || strcmp(v, "0") != 0)
			av_pair(av, "--bind-device", "true");
		if (!v || strcmp(v, "1") != 0)
			av_pair(av, "--bind-device", "false");
		return;
	}
	if (!strcmp(name, "encryption_algorithm")) {
		/* init.d:616-618：非空且不等于 aes-gcm 才输出 */
		v = et_cfg_str(c, name);
		if (v && *v && strcmp(v, "aes-gcm") != 0)
			av_pair(av, "--encryption-algorithm", v);
		return;
	}
}

/* ---------------- web 特殊规则 ---------------- */

static void emit_web_custom(et_list *av, const et_field *f, const char *v)
{
	if (!strcmp(f->name, "web_html_port")) {
		/* [ -z "$html_port" ] && --no-web || -l $html_port（init.d:812-816） */
		if (!v || !*v)
			av_flag(av, "--no-web");
		else
			av_pair(av, "-l", v);
		return;
	}
	if (!strcmp(f->name, "web_api_addr") || !strcmp(f->name, "web_html_addr")) {
		/* 非空且不是 0.0.0.0 才输出（init.d:818-819） */
		if (v && *v && strcmp(v, "0.0.0.0") != 0)
			av_pair(av, f->web_flag, v);
		return;
	}
	if (!strcmp(f->name, "web_weblog")) {
		/* [ "$weblog" != "off" ] && --console-log-level $weblog（init.d:847-849） */
		if (v && *v && strcmp(v, "off") != 0)
			av_pair(av, "--console-log-level", v);
		return;
	}
}

/* OIDC / Webhook 两组受各自 enabled 开关控制（init.d:828-846） */
static bool web_gate_ok(et_cfg *c, const char *name)
{
	if (et_starts_with(name, "web_oidc_") && strcmp(name, "web_oidc_enabled") != 0)
		return et_cfg_is01(c, "web_oidc_enabled");
	if (et_starts_with(name, "web_webhook_") && strcmp(name, "web_webhook_enabled") != 0)
		return et_cfg_is01(c, "web_webhook_enabled");
	return true;
}

/* ---------------- 机器 ID ---------------- */

/* 对齐 start_et()（init.d:662-668）：
 *   文件不存在则创建；内容为空则用 /proc/sys/kernel/random/uuid 生成并落盘；
 *   最后以"文件里的内容"为准（写盘失败就不下发 --machine-id）。
 *
 * 与旧实现的差异（有意）：父目录不存在时先 mkdir -p；并且**落盘失败也照样返回
 * 刚生成的值**——落盘只是持久化，不该因为 /etc/easytier 不存在就让 --machine-id
 * 消失（旧实现 touch 失败后 cat 重定向也失败，于是 et_uuid 为空，Web 模式退化）。 */
static char *machine_uuid(void)
{
	char *s = et_read_file(ET_FILE_MACHINE_ID, NULL);

	if (s) {
		et_trim_newline(s);
		for (char *p = s; *p; p++)
			if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
				memmove(p, p + 1, strlen(p));
				p--;
			}
		if (*s)
			return s;
		free(s);
	}

	char *u = et_read_file("/proc/sys/kernel/random/uuid", NULL);
	if (!u)
		return NULL;
	et_trim_newline(u);
	for (char *p = u; *p; p++)
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			memmove(p, p + 1, strlen(p));
			p--;
		}
	if (!*u) {
		free(u);
		return NULL;
	}

	/* 尽力持久化：目录不存在则创建；失败不影响本次启动 */
	char *dir = et_dirname_keep_slash(ET_FILE_MACHINE_ID);
	if (dir) {
		et_mkdir_p(dir, 0755);
		free(dir);
	}
	et_buf b;
	et_buf_init(&b);
	et_buf_add(&b, u);
	et_buf_addc(&b, '\n');
	bool saved = et_write_file(ET_FILE_MACHINE_ID, b.p ? b.p : "", b.len) == ET_OK;
	et_buf_free(&b);

	/* 落盘成功则以文件内容为准（与旧实现 cat 回读一致），否则直接用生成值 */
	if (saved) {
		char *back = et_read_file(ET_FILE_MACHINE_ID, NULL);

		if (back) {
			et_trim_newline(back);
			if (*back) {
				free(u);
				return back;
			}
			free(back);
		}
	}
	return u;
}

/* ---------------- 对外接口 ---------------- */

static char **av_finish(et_list *av, const char *def_bin)
{
	if (!av->v || av->n == 0)
		et_list_add(av, def_bin);
	return av->v; /* et_list.v 始终以 NULL 结尾 */
}

char **et_argv_core(et_cfg *c, const char *bin, et_buf *notes)
{
	et_list av;
	const char *mode;

	et_list_init(&av);
	et_list_add(&av, (bin && *bin) ? bin : ET_PATH_CORE_DEF);
	if (!c)
		return av_finish(&av, ET_PATH_CORE_DEF);

	mode = et_cfg_str(c, "etcmd");
	if (!mode || !*mode)
		mode = "etcmd";

	if (strcmp(mode, "config") == 0) {
		/* 配置文件启动方式（init.d:461-463 → check_config） */
		av_pair(&av, "-c", ET_FILE_TOML);
		note(notes, "etcmd=config: 使用 %s 启动，调用方需追加 --console-log-level", ET_FILE_TOML);
		return av_finish(&av, ET_PATH_CORE_DEF);
	}

	if (strcmp(mode, "web") == 0) {
		/* WEB 控制台启动（init.d:659-682） */
		char *uuid = machine_uuid();

		if (uuid && *uuid)
			av_pair(&av, "--machine-id", uuid);
		free(uuid);

		const char *wc = et_cfg_str(c, "web_config");
		if (wc && *wc) {
			if (strstr(wc, "://"))
				av_pair(&av, "-w", wc);
			else
				note(notes, "web_config: 地址 %s 不是完整 URL，EasyTier 2.7.0 起必须填写完整地址"
					    "（例如 udp://config-server.easytier.cn:22020/账户名），已跳过 Web 客户端启动", wc);
		} else {
			note(notes, "web_config: 未填写 Web 服务器地址，已跳过 -w");
		}

		const char *dn = et_cfg_str(c, "desvice_name");
		if (dn && *dn)
			av_pair(&av, "--hostname", dn);
		return av_finish(&av, ET_PATH_CORE_DEF);
	}

	/* 默认方式（etcmd）：严格按 init.d 的追加顺序 */
	for (size_t i = 0; i < et_schema_len; i++) {
		const et_field *f = &et_schema[i];

		if (!f->core_flag || f->core_emit == ET_E_NONE)
			continue;
		if (f->core_emit == ET_E_CUSTOM) {
			/* web_config 只在 web 模式下用，默认模式不参与 */
			if (!strcmp(f->name, "web_config"))
				continue;
			emit_core_custom(&av, c, f, notes);
			continue;
		}
		emit_generic(&av, f->core_flag, f->core_emit, et_cfg_str(c, f->name),
			     et_cfg_list(c, f->name));
	}

	/* extra_args：逐行原样，行内按空白切成多个参数（旧实现拼进 sh -c 后由 shell 分词） */
	for (size_t i = 0; i < c->extra_args.n; i++) {
		const char *line = c->extra_args.v[i];

		if (et_is_blank(line))
			continue;
		et_list_split_ws(&av, line);
	}

	note(notes, "--console-log-level 未拼装：请调用方在末尾追加 \"--console-log-level <log>\"（log 默认 off）");
	return av_finish(&av, ET_PATH_CORE_DEF);
}

char **et_argv_web(et_cfg *c, const char *bin, et_buf *notes)
{
	et_list av;

	et_list_init(&av);
	et_list_add(&av, (bin && *bin) ? bin : ET_PATH_WEB_DEF);
	if (!c)
		return av_finish(&av, ET_PATH_WEB_DEF);

	for (size_t i = 0; i < et_schema_len; i++) {
		const et_field *f = &et_schema[i];
		const char *v;

		if (!f->web_flag || f->web_emit == ET_E_NONE)
			continue;
		if (!web_gate_ok(c, f->name))
			continue;
		v = et_cfg_str(c, f->name);
		if (f->web_emit == ET_E_CUSTOM)
			emit_web_custom(&av, f, v);
		else
			emit_generic(&av, f->web_flag, f->web_emit, v,
				     et_cfg_list(c, f->name));
	}

	note(notes, "easytier-web: 旧实现仅在 weblog != off 时追加 --console-log-level（已按此拼装）");
	return av_finish(&av, ET_PATH_WEB_DEF);
}

void et_argv_free(char **argv)
{
	if (!argv)
		return;
	for (size_t i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

static bool is_secret_flag(const char *a)
{
	static const char *const flags[] = {
		"--network-secret",
		"--vpn-portal-private-key",
		"--oidc-client-secret",
		"--webhook-secret",
		"--internal-auth-token",
		NULL,
	};

	for (size_t i = 0; flags[i]; i++) {
		size_t n = strlen(flags[i]);

		if (strncmp(a, flags[i], n) == 0 && (a[n] == '\0' || a[n] == '='))
			return true;
	}
	return false;
}

/* 拼接命令行；mask_secret=true 时敏感参数的值替换为 *** */
static char *argv_join(char **argv, bool mask_secret)
{
	et_buf b;

	if (!argv)
		return NULL;
	et_buf_init(&b);
	for (size_t i = 0; argv[i]; i++) {
		if (i)
			et_buf_addc(&b, ' ');

		/* 形如 --secret value：值整体脱敏 */
		if (mask_secret && i > 0 && is_secret_flag(argv[i - 1])) {
			et_buf_add(&b, "'***'");
			continue;
		}
		/* 形如 --secret=value：只脱敏等号右侧 */
		const char *a = argv[i];
		if (mask_secret && is_secret_flag(a)) {
			const char *eq = strchr(a, '=');

			if (eq) {
				et_buf_addc(&b, '\'');
				et_buf_addn(&b, a, (size_t)(eq - a) + 1);
				et_buf_add(&b, "***'");
				continue;
			}
		}
		char *q = et_shell_quote(a);
		et_buf_add(&b, q ? q : "");
		free(q);
	}
	if (!b.p)
		et_buf_add(&b, "");
	return et_buf_steal(&b);
}

char *et_argv_join(char **argv)
{
	return argv_join(argv, false);
}

char *et_argv_visible(char **argv)
{
	return argv_join(argv, true);
}
