/*
 * etd_api_config.c - 配置读写与表单渲染（owner: web-bridge）
 *
 * 四个 handler：
 *   config_get    读取全部配置（JSON，供未来动态 UI / 调试）
 *   config_save   接收表单 k=v，写回 UCI（复用 et_cfg_save），返回 {success,message}
 *   config_render 按 et_schema[] 的 tab 分组生成 HTML 片段（可直接放进 LuCI 页面）
 *   schema        输出 JSON 版 schema（文档 / 测试 / 动态 UI 用）
 *
 * 渲染只依赖 et_schema[] 这一个数据源：字段顺序 = et_schema 的行顺序，
 * 分组 = et_field.tab。tab → 标题的映射、以及旧 CBI 里那几项"不在 schema
 * 里"的特殊字段（config.toml 文本、machine-id）都在本文件里处理，
 * 不改 etd_schema.c。
 */
#include "etd_api.h"
#include "etd_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- tab 分组 ----------------
 * et_schema 的 tab 有 general/privacy/web/system 四种：
 *   general  → 基本设置（网络名、IP、监听端口、启动方式、日志、开关）
 *   privacy  → 高级设置（其余所有 core 参数）
 *   web      → Web 控制台（easytier-web-embed 参数）
 *   system   → 不渲染：二进制路径与下载镜像在"上传程序"页管理，避免两处编辑
 */
typedef struct {
	const char *id;
	const char *title; /* i18n msgid */
} et_tab_def;

static const et_tab_def et_tabs[] = {
	{ "general", "General Settings" },
	{ "privacy", "Advanced Settings" },
	{ "web", "Web Console" },
};

/* 旧 CBI 里用 cfgvalue/write 自定义读写的两个"伪字段"（不在 schema 中） */
typedef struct {
	const char *name;
	const char *label;
	const char *help;
	const char *tab;
	const char *dep;
	bool textarea;
	const char *file; /* 值来源 / 保存目标 */
} et_extra;

static const et_extra et_extras[] = {
	{ "et_config", "Configuration File",
	  "The configuration file is located at /etc/easytier/config.toml<br>"
	  "The command-line startup parameters and the parameters in this configuration file are not synchronized<br>"
	  "Make sure to specify the TUN interface name and port to enable automatic firewall allowance",
	  "general", "etcmd:config", true, ET_FILE_TOML },
	{ "uuid", "UUID",
	  "Unique identifier used to recognize this device when connecting to the web console, for issuing configuration files",
	  "general", "etcmd:web", false, ET_FILE_MACHINE_ID },
};

/* schema.def 为 NULL、但旧 CBI 有 .default 的项：只影响表单初值，不影响 argv */
static const struct {
	const char *name;
	const char *val;
} et_form_def[] = {
	{ "listenermode", "ON" },
	{ "tcp_port", ET_DEFAULT_TCP_PORT },
	{ "ws_port", ET_DEFAULT_WS_PORT },
	{ "wss_port", ET_DEFAULT_WSS_PORT },
	{ "default_protocol", "-" },
	{ "checktime", "2" }, /* 旧 init.d 在 checktime 为空时回落 2 分钟 */
};

/* 输入框占位提示（取自旧 CBI 的 placeholder） */
static const struct {
	const char *name;
	const char *ph;
} et_placeholders[] = {
	{ "network_name", "easytier-name" },
	{ "network_secret", "easytier-password" },
	{ "web_config", "udp://config-server.easytier.cn:22020/my-token" },
	{ "ipaddr", "10.0.0.1" },
	{ "ip6addr", "2001:db8::1" },
	{ "rpc_portal", "15888" },
	{ "rpc_portal_whitelist", "127.0.0.1/32,127.0.0.0/8,::1/128" },
	{ "wg_port", "11011" },
	{ "instance_name", "default" },
	{ "vpn_portal", "wg://0.0.0.0:11011" },
	{ "vpn_portal_clients", "client1=10.14.14.1/24" },
	{ "mtu", "1300" },
	{ "tunname", "tun0" },
	{ "socks_port", "1080" },
	{ "interface_netmask", "255.0.0.0" },
	{ "manual_routes", "192.168.0.0/16" },
	{ "stun_servers", "stun.miwifi.com:3478" },
	{ "tcp_stun_servers", "stun.miwifi.com:3478" },
	{ "extra_args", "--tcp-whitelist=80" },
	{ "tld_dns_zone", "et.net." },
};

/* 显隐依赖，语法 "key:value|key:value"（| 为"或"，与 CBI 多次 depends 的语义一致）。
 * 只保留分组性开关；其余字段始终可见，避免复刻 CBI 容易出错的逐字段 depends。 */
static const struct {
	const char *name;
	const char *dep;
} et_deps[] = {
	{ "web_config", "etcmd:web" },
	{ "tcp_port", "listenermode:ON|etcmd:web" },
	{ "ws_port", "listenermode:ON|etcmd:web" },
	{ "wss_port", "listenermode:ON|etcmd:web" },
	{ "wg_port", "listenermode:ON|etcmd:web" },
	{ "quic_port", "listenermode:ON|etcmd:web" },
	{ "mapped_listeners", "listenermode:ON" },
	{ "whitelist", "relay_network:1" },
	{ "tld_dns_zone", "accept_dns:1" },
	{ "checkip", "check:1" },
	{ "checktime", "check:1" },
	{ "interface_netmask", "auto_config_interface:1" },
	{ "web_oidc_issuer_url", "web_oidc_enabled:1" },
	{ "web_oidc_client_id", "web_oidc_enabled:1" },
	{ "web_oidc_client_secret", "web_oidc_enabled:1" },
	{ "web_oidc_redirect_url", "web_oidc_enabled:1" },
	{ "web_oidc_username_claim", "web_oidc_enabled:1" },
	{ "web_oidc_scopes", "web_oidc_enabled:1" },
	{ "web_oidc_frontend_base_url", "web_oidc_enabled:1" },
	{ "web_oidc_disable_pkce", "web_oidc_enabled:1" },
	{ "web_webhook_url", "web_webhook_enabled:1" },
	{ "web_webhook_secret", "web_webhook_enabled:1" },
	{ "web_internal_auth_token", "web_webhook_enabled:1" },
	{ "web_web_instance_id", "web_webhook_enabled:1" },
	{ "web_web_instance_api_base_url", "web_webhook_enabled:1" },
};

/* 枚举值的显示文案（value → i18n msgid）；未列出的直接显示 value */
static const struct {
	const char *val;
	const char *msgid;
} et_enum_label[] = {
	{ "-", "default" },
	{ "none", "default" },
	{ "ON", "Listen" },
	{ "OFF", "Do Not Listen" },
	{ "off", "Off" },
	{ "error", "Error" },
	{ "warn", "Warning" },
	{ "info", "Info" },
	{ "debug", "Debug" },
	{ "trace", "Trace" },
	{ "etcmd", "Default" },
	{ "config", "Configuration File" },
	{ "web", "Web Configuration" },
};

/* et_forward 是固定取值的多选（旧 CBI 用 MultiValue），渲染成勾选组而不是自由文本，
 * 避免用户手输 etfwlan 之类的魔法值。其它 list 字段仍是可增删的自由输入行。 */
static const struct {
	const char *val;
	const char *label;
} et_forward_opts[] = {
	{ "etfwlan", "Allow traffic from EasyTier virtual network to LAN" },
	{ "etfwwan", "Allow traffic from EasyTier virtual network to WAN" },
	{ "lanfwet", "Allow traffic from LAN to EasyTier virtual network" },
	{ "wanfwet", "Allow traffic from WAN to EasyTier virtual network" },
};

#define ET_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---------------- 小工具 ---------------- */

static const char *tab_title(const char *id)
{
	if (!id)
		return NULL;
	for (size_t i = 0; i < ET_ARRAY_LEN(et_tabs); i++) {
		if (!strcmp(et_tabs[i].id, id))
			return et_tabs[i].title;
	}
	return NULL;
}

static bool tab_rendered(const char *id)
{
	return tab_title(id) != NULL;
}

static const char *dep_of(const char *name)
{
	for (size_t i = 0; i < ET_ARRAY_LEN(et_deps); i++) {
		if (!strcmp(et_deps[i].name, name))
			return et_deps[i].dep;
	}
	return NULL;
}

static const char *placeholder_of(const char *name)
{
	for (size_t i = 0; i < ET_ARRAY_LEN(et_placeholders); i++) {
		if (!strcmp(et_placeholders[i].name, name))
			return et_placeholders[i].ph;
	}
	return NULL;
}

/* 表单初值：UCI 值 → schema 默认值（et_cfg 已填）→ CBI 表单默认值 */
static const char *form_value(et_cfg *cfg, const char *name)
{
	const char *v = et_cfg_str(cfg, name);

	if (v && *v)
		return v;
	for (size_t i = 0; i < ET_ARRAY_LEN(et_form_def); i++) {
		if (!strcmp(et_form_def[i].name, name))
			return et_form_def[i].val;
	}
	return v ? v : "";
}

/* 设备名默认值（旧 CBI: /proc/device-tree/model → hostname → OpenWrt，空格换下划线） */
static char *device_name_default(void)
{
	char *model = et_read_file("/proc/device-tree/model", NULL);
	char *host = et_read_file("/proc/sys/kernel/hostname", NULL);
	char *src = (model && *model) ? model : host;
	char *out;

	if (!src || !*src) {
		free(model);
		free(host);
		return strdup("OpenWrt");
	}
	et_trim_newline(src);
	for (char *p = src; *p; p++) {
		if (isspace((unsigned char)*p))
			*p = '_';
	}
	out = strdup(src);
	free(model);
	free(host);
	return out;
}

static void add_dep_attr(et_buf *b, const char *dep)
{
	if (!dep || !*dep)
		return;
	et_buf_add(b, " data-dep=\"");
	et_buf_add_html(b, dep);
	et_buf_addc(b, '"');
}

/* ---------------- HTML 渲染 ---------------- */

struct rctx {
	et_buf *b;
	et_cfg *cfg;
	char *devname;
};

static void open_value(struct rctx *c, const char *name, const char *label, const char *dep)
{
	et_buf *b = c->b;

	et_buf_add(b, "<div class=\"cbi-value\"");
	add_dep_attr(b, dep);
	et_buf_add(b, ">\n\t<label class=\"cbi-value-title\" for=\"et_");
	et_buf_add(b, name);
	et_buf_add(b, "\">");
	et_buf_add(b, et_i18n(label));
	et_buf_add(b, "</label>\n\t<div class=\"cbi-value-field\">\n");
}

static void close_value(struct rctx *c, const char *help)
{
	et_buf *b = c->b;

	if (help && *help) {
		/* help 是编译进二进制的固定文案，允许 <br> 等标签（与旧 CBI 一致） */
		et_buf_add(b, "\t\t<div class=\"cbi-value-description\">");
		et_buf_add(b, et_i18n(help));
		et_buf_add(b, "</div>\n");
	}
	et_buf_add(b, "\t</div>\n</div>\n");
}

static void emit_input(struct rctx *c, const char *name, const char *val,
		       const char *type, const char *ph, const char *extra_attr)
{
	et_buf *b = c->b;

	et_buf_addf(b, "\t\t<input type=\"%s\" class=\"cbi-input-text\" id=\"et_%s\" name=\"%s\"",
		    type, name, name);
	if (extra_attr)
		et_buf_add(b, extra_attr);
	et_buf_add(b, " value=\"");
	et_buf_add_html(b, val ? val : "");
	et_buf_addc(b, '"');
	if (ph && *ph) {
		et_buf_add(b, " placeholder=\"");
		et_buf_add_html(b, ph);
		et_buf_addc(b, '"');
	}
	et_buf_add(b, ">\n");
}

static void emit_enum(struct rctx *c, const et_field *f, const char *val)
{
	et_buf *b = c->b;
	et_list vals;

	et_list_init(&vals);
	et_list_split(&vals, f->enum_vals ? f->enum_vals : "", ",");

	et_buf_addf(b, "\t\t<select class=\"cbi-input-select\" id=\"et_%s\" name=\"%s\">\n",
		    f->name, f->name);
	if (!val || !*val)
		et_buf_add(b, "\t\t\t<option value=\"\" selected=\"selected\">--</option>\n");
	for (size_t i = 0; i < vals.n; i++) {
		const char *v = vals.v[i];
		const char *text = NULL;

		for (size_t j = 0; j < ET_ARRAY_LEN(et_enum_label); j++) {
			if (!strcmp(et_enum_label[j].val, v)) {
				text = et_enum_label[j].msgid;
				break;
			}
		}
		et_buf_add(b, "\t\t\t<option value=\"");
		et_buf_add_html(b, v);
		et_buf_add(b, "\"");
		if (val && !strcmp(val, v))
			et_buf_add(b, " selected=\"selected\"");
		et_buf_add(b, ">");
		et_buf_add(b, text ? et_i18n(text) : v);
		et_buf_add(b, "</option>\n");
	}
	et_buf_add(b, "\t\t</select>\n");
	et_list_free(&vals);
}

static void emit_bool(struct rctx *c, const char *name, const char *val)
{
	et_buf *b = c->b;
	bool on = val && (!strcmp(val, "1") || !strcmp(val, "true") || !strcmp(val, "on"));

	et_buf_addf(b, "\t\t<span class=\"et-switch\">\n"
		       "\t\t\t<input type=\"checkbox\" class=\"cbi-input-checkbox\" id=\"et_%s\" name=\"%s\" value=\"1\"",
		    name, name);
	if (on)
		et_buf_add(b, " checked=\"checked\"");
	et_buf_addf(b, ">\n\t\t\t<label for=\"et_%s\"></label>\n\t\t</span>\n", name);
}

static void emit_list_row(et_buf *b, const char *v)
{
	et_buf_add(b, "\t\t\t\t<div class=\"et-list-row\">"
		      "<input type=\"text\" class=\"cbi-input-text\" value=\"");
	et_buf_add_html(b, v ? v : "");
	et_buf_addf(b, "\"><button type=\"button\" class=\"cbi-button et-list-del\" "
		       "title=\"%s\">&#215;</button></div>\n", et_i18n("Delete"));
}

static void emit_list(struct rctx *c, const char *name)
{
	et_buf *b = c->b;
	const et_list *l = et_cfg_list(c->cfg, name);

	et_buf_addf(b, "\t\t<div class=\"et-list\" data-name=\"%s\">\n\t\t\t<div class=\"et-list-rows\">\n",
		    name);
	if (l && l->n) {
		for (size_t i = 0; i < l->n; i++)
			emit_list_row(b, l->v[i]);
	} else {
		emit_list_row(b, "");
	}
	et_buf_add(b, "\t\t\t</div>\n\t\t\t<button type=\"button\" class=\"cbi-button et-list-add\">+ ");
	et_buf_add(b, et_i18n("Add"));
	et_buf_add(b, "</button>\n\t\t</div>\n");
}

/* 固定取值的多选组（et_forward）：勾选框没有 name，值由页面 JS 汇总成 data-name */
static void emit_checks(struct rctx *c, const char *name)
{
	et_buf *b = c->b;
	const et_list *l = et_cfg_list(c->cfg, name);

	et_buf_addf(b, "\t\t<div class=\"et-checks\" data-name=\"%s\">\n", name);
	for (size_t i = 0; i < ET_ARRAY_LEN(et_forward_opts); i++) {
		bool on = false;

		for (size_t j = 0; l && j < l->n; j++) {
			if (!strcmp(l->v[j], et_forward_opts[i].val))
				on = true;
		}
		et_buf_addf(b, "\t\t\t<label><input type=\"checkbox\" value=\"%s\"%s> %s</label>\n",
			    et_forward_opts[i].val, on ? " checked=\"checked\"" : "",
			    et_i18n(et_forward_opts[i].label));
	}
	et_buf_add(b, "\t\t</div>\n");
}

static void emit_field(struct rctx *c, const et_field *f)
{
	const char *val = form_value(c->cfg, f->name);
	const char *ph = placeholder_of(f->name);

	if (f->kind == ET_K_LIST)
		val = NULL;
	if (!strcmp(f->name, "desvice_name")) {
		val = *val ? val : (c->devname ? c->devname : "");
		ph = c->devname;
	}

	open_value(c, f->name, f->label, dep_of(f->name));
	if (!strcmp(f->name, "et_forward")) {
		emit_checks(c, f->name);
	} else {
		switch (f->kind) {
		case ET_K_BOOL:
			emit_bool(c, f->name, val);
			break;
		case ET_K_ENUM:
			emit_enum(c, f, val);
			break;
		case ET_K_LIST:
			emit_list(c, f->name);
			break;
		case ET_K_PASS:
			emit_input(c, f->name, val, "password", ph, NULL);
			break;
		case ET_K_INT:
			emit_input(c, f->name, val, "text", ph, " inputmode=\"numeric\"");
			break;
		case ET_K_STR:
		default:
			emit_input(c, f->name, val, "text", ph, NULL);
			break;
		}
	}
	close_value(c, f->help);
}

static void emit_extra(struct rctx *c, const et_extra *x)
{
	et_buf *b = c->b;
	char *val = et_read_file(x->file, NULL);

	if (val)
		et_trim_newline(val);

	open_value(c, x->name, x->label, x->dep);
	if (x->textarea) {
		et_buf_addf(b, "\t\t<textarea class=\"cbi-input-text\" id=\"et_%s\" name=\"%s\" "
			       "rows=\"18\" wrap=\"off\">", x->name, x->name);
		et_buf_add_html(b, val ? val : "");
		et_buf_add(b, "</textarea>\n");
	} else {
		emit_input(c, x->name, val ? val : "", "text", NULL, NULL);
	}
	close_value(c, x->help);
	free(val);
}

static size_t tab_count(const char *tab)
{
	size_t n = 0;

	for (size_t i = 0; i < et_schema_len; i++) {
		if (et_schema[i].tab && !strcmp(et_schema[i].tab, tab))
			n++;
	}
	for (size_t i = 0; i < ET_ARRAY_LEN(et_extras); i++) {
		if (!strcmp(et_extras[i].tab, tab))
			n++;
	}
	return n;
}

static void render_form(et_buf *b, et_cfg *cfg)
{
	struct rctx c = { .b = b, .cfg = cfg, .devname = device_name_default() };

	et_i18n_init();

	et_buf_add(b, "<div class=\"et-config\">\n<div class=\"et-tabs\">\n");
	for (size_t i = 0; i < ET_ARRAY_LEN(et_tabs); i++) {
		if (!tab_count(et_tabs[i].id))
			continue;
		et_buf_addf(b, "\t<button type=\"button\" class=\"et-tab%s\" data-tab=\"%s\">%s</button>\n",
			    i == 0 ? " et-tab-active" : "", et_tabs[i].id, et_i18n(et_tabs[i].title));
	}
	et_buf_add(b, "</div>\n");

	for (size_t i = 0; i < ET_ARRAY_LEN(et_tabs); i++) {
		const char *tab = et_tabs[i].id;

		if (!tab_count(tab))
			continue;
		et_buf_addf(b, "<div class=\"et-tab-panel%s\" data-tab=\"%s\">\n"
			       "<div class=\"cbi-section\"><div class=\"cbi-section-node\">\n",
			    i == 0 ? " et-tab-panel-active" : "", tab);
		for (size_t j = 0; j < et_schema_len; j++) {
			const et_field *f = &et_schema[j];

			if (!f->tab || strcmp(f->tab, tab) != 0 || !tab_rendered(f->tab))
				continue;
			emit_field(&c, f);
		}
		for (size_t j = 0; j < ET_ARRAY_LEN(et_extras); j++) {
			if (!strcmp(et_extras[j].tab, tab))
				emit_extra(&c, &et_extras[j]);
		}
		et_buf_add(b, "</div></div>\n</div>\n");
	}
	et_buf_add(b, "</div>\n");

	free(c.devname);
}

/* ---------------- JSON 输出 ---------------- */

static void json_list_kv(et_res *res, const char *key, const et_list *l)
{
	et_buf b;

	et_buf_init(&b);
	et_buf_addc(&b, '[');
	for (size_t i = 0; l && i < l->n; i++) {
		if (i)
			et_buf_addc(&b, ',');
		et_buf_addc(&b, '"');
		et_buf_add_json(&b, l->v[i] ? l->v[i] : "");
		et_buf_addc(&b, '"');
	}
	et_buf_addc(&b, ']');
	et_json_kv_raw(res, key, b.p ? b.p : "[]");
	et_buf_free(&b);
}

static void emit_tabs_json(et_res *res)
{
	et_buf b;

	et_buf_init(&b);
	et_buf_addc(&b, '[');
	for (size_t i = 0; i < ET_ARRAY_LEN(et_tabs); i++) {
		if (i)
			et_buf_addc(&b, ',');
		et_buf_addf(&b, "{\"id\":\"%s\",\"title\":\"", et_tabs[i].id);
		et_buf_add_json(&b, et_tabs[i].title);
		et_buf_add(&b, "\"}");
	}
	et_buf_addc(&b, ']');
	et_json_kv_raw(res, "tabs", b.p ? b.p : "[]");
	et_buf_free(&b);
}

/* ---------------- handler: config_get ---------------- */

int et_api_config_get(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();
	et_cfg *cfg = et_cfg_load(u);
	et_res sub;

	(void)req;
	if (!cfg) {
		et_uci_close(u);
		et_json_result(res, false, "/etc/config/easytier not found");
		return ET_ERR;
	}

	et_res_init(&sub, ET_CT_JSON);
	et_json_begin(&sub);
	for (size_t i = 0; i < et_schema_len; i++) {
		const et_field *f = &et_schema[i];

		if (f->kind == ET_K_LIST)
			json_list_kv(&sub, f->name, et_cfg_list(cfg, f->name));
		else
			et_json_kv_str(&sub, f->name, et_cfg_str(cfg, f->name));
	}
	et_json_end(&sub);

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_raw(res, "config", sub.body.p);
	et_json_end(res);

	et_res_free(&sub);
	et_cfg_free(cfg);
	et_uci_close(u);
	return ET_OK;
}

/* ---------------- handler: schema ---------------- */

static void emit_field_json(et_res *res, const et_field *f)
{
	static const char *const kinds[] = { "str", "int", "bool", "enum", "list", "pass" };

	et_json_begin(res);
	et_json_kv_str(res, "name", f->name);
	et_json_kv_str(res, "kind", kinds[(int)f->kind]);
	if (f->def)
		et_json_kv_str(res, "def", f->def);
	if (f->label)
		et_json_kv_str(res, "label", f->label);
	if (f->help)
		et_json_kv_str(res, "help", f->help);
	if (f->tab)
		et_json_kv_str(res, "tab", f->tab);
	if (f->enum_vals) {
		et_buf b;

		et_buf_init(&b);
		et_buf_addc(&b, '[');
		et_list vals;

		et_list_init(&vals);
		et_list_split(&vals, f->enum_vals, ",");
		for (size_t i = 0; i < vals.n; i++) {
			if (i)
				et_buf_addc(&b, ',');
			et_buf_addc(&b, '"');
			et_buf_add_json(&b, vals.v[i]);
			et_buf_addc(&b, '"');
		}
		et_list_free(&vals);
		et_buf_addc(&b, ']');
		et_json_kv_raw(res, "enum", b.p ? b.p : "[]");
		et_buf_free(&b);
	}
	if (f->core_flag)
		et_json_kv_str(res, "core_flag", f->core_flag);
	if (f->web_flag)
		et_json_kv_str(res, "web_flag", f->web_flag);
	et_json_end(res);
}

int et_api_schema(const et_req *req, et_res *res)
{
	et_buf arr;
	et_res item;

	(void)req;
	et_buf_init(&arr);
	et_buf_addc(&arr, '[');
	for (size_t i = 0; i < et_schema_len; i++) {
		if (i)
			et_buf_addc(&arr, ',');
		et_res_init(&item, ET_CT_JSON);
		emit_field_json(&item, &et_schema[i]);
		et_buf_add(&arr, item.body.p ? item.body.p : "{}");
		et_res_free(&item);
	}
	et_buf_addc(&arr, ']');

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_int(res, "count", (long long)et_schema_len);
	emit_tabs_json(res);
	et_json_kv_raw(res, "fields", arr.p ? arr.p : "[]");
	et_json_end(res);

	et_buf_free(&arr);
	return ET_OK;
}

/* ---------------- handler: config_render ---------------- */

int et_api_config_render(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();
	et_cfg *cfg = et_cfg_load(u);

	(void)req;
	if (!cfg) {
		et_uci_close(u);
		res->type = ET_CT_HTML;
		et_res_text(res, "<div class=\"cbi-section\"><div class=\"cbi-value-field\">"
				 "Configuration file /etc/config/easytier not found</div></div>\n");
		return ET_ERR;
	}

	res->type = ET_CT_HTML;
	render_form(&res->body, cfg);

	et_cfg_free(cfg);
	et_uci_close(u);
	return ET_OK;
}

/* ---------------- handler: config_save ---------------- */

/* "a,b,c" 里是否包含 v */
static bool in_enum(const char *list, const char *v)
{
	et_list vals;
	bool found = false;

	et_list_init(&vals);
	et_list_split(&vals, list ? list : "", ",");
	for (size_t i = 0; i < vals.n && !found; i++) {
		if (!strcmp(vals.v[i], v))
			found = true;
	}
	et_list_free(&vals);
	return found;
}

static bool is_int_str(const char *v)
{
	if (!*v)
		return true;
	for (const char *p = v; *p; p++) {
		if (!isdigit((unsigned char)*p))
			return false;
	}
	return true;
}

/* 校验并归一化（bool 允许 on/off/true/false，其它保持原样） */
static bool norm_value(const et_field *f, const char *in, char *out, size_t n,
		       char *err, size_t errn)
{
	snprintf(out, n, "%s", in ? in : "");

	switch (f->kind) {
	case ET_K_BOOL:
		/* 空值当作关闭；同时容忍 on/off/true/false 写法 */
		if (!*out || !strcmp(out, "off") || !strcmp(out, "false"))
			snprintf(out, n, "0");
		else if (!strcmp(out, "on") || !strcmp(out, "true"))
			snprintf(out, n, "1");
		if (strcmp(out, "0") && strcmp(out, "1")) {
			snprintf(err, errn, "Invalid value for %s", f->name);
			return false;
		}
		return true;
	case ET_K_INT:
		if (!is_int_str(out)) {
			snprintf(err, errn, "Invalid value for %s: %.64s", f->name, out);
			return false;
		}
		return true;
	case ET_K_ENUM:
		if (*out && !in_enum(f->enum_vals, out)) {
			snprintf(err, errn, "Invalid value for %s: %.64s", f->name, out);
			return false;
		}
		return true;
	default:
		return true;
	}
}

/* 取可写的 et_list。extra_args 存在 cfg->extra_args（et_cfg_save 单独处理），
 * 其余 list 字段都在 items[] 里。 */
static et_list *writable_list(et_cfg *cfg, const char *name)
{
	if (!strcmp(name, "extra_args"))
		return &cfg->extra_args;
	for (size_t i = 0; i < cfg->n; i++) {
		if (cfg->items[i].f && !strcmp(cfg->items[i].f->name, name))
			return &cfg->items[i].list;
	}
	return NULL;
}

static et_cfg_item *writable_item(et_cfg *cfg, const char *name)
{
	for (size_t i = 0; i < cfg->n; i++) {
		if (cfg->items[i].f && !strcmp(cfg->items[i].f->name, name))
			return &cfg->items[i];
	}
	return NULL;
}

/* 表单用换行分隔多行输入；空行忽略，行尾空白去掉，行首只去空格与制表符 */
static void set_list_value(et_cfg *cfg, const char *name, const char *val)
{
	et_list *dst = writable_list(cfg, name);
	char *copy;
	char *line;
	char *save = NULL;

	if (!dst)
		return;
	for (size_t i = 0; i < dst->n; i++)
		free(dst->v[i]);
	dst->n = 0;

	copy = strdup(val ? val : "");
	if (!copy)
		return;
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		size_t len = strlen(line);

		while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
			line[--len] = '\0';
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line)
			et_list_add(dst, line);
	}
	free(copy);
}

/* 值有变化才写（避免每次保存都重写 config.toml / machine-id）。
 * strip_cr 对应旧 CBI 的 write 里 `value:gsub("\r\n", "\n")`。 */
static bool write_if_changed(const char *path, const char *val, bool strip_cr,
			     char *err, size_t errn)
{
	et_buf b;
	char *cur;
	bool ok = true;

	et_buf_init(&b);
	for (const char *p = val ? val : ""; *p; p++) {
		if (strip_cr && *p == '\r')
			continue;
		et_buf_addc(&b, *p);
	}

	cur = et_read_file(path, NULL);
	if (cur && !strcmp(cur, b.p ? b.p : "")) {
		free(cur);
		et_buf_free(&b);
		return true;
	}
	free(cur);

	if (et_mkdir_p(ET_DIR_ET, 0755) != ET_OK && !et_is_dir(ET_DIR_ET)) {
		snprintf(err, errn, "Cannot create %s", ET_DIR_ET);
		ok = false;
	} else if (et_write_file(path, b.p ? b.p : "", b.len) != ET_OK) {
		snprintf(err, errn, "Cannot write %s", path);
		ok = false;
	}
	et_buf_free(&b);
	return ok;
}

int et_api_config_save(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();
	et_cfg *cfg;
	bool changed = false;
	int applied = 0;
	char err[256] = "";

	if (!u) {
		et_json_result(res, false, "/etc/config/easytier not found");
		return ET_ERR;
	}
	cfg = et_cfg_load(u);
	if (!cfg) {
		et_uci_close(u);
		et_json_result(res, false, et_i18n("Save failed"));
		return ET_ERR;
	}

	for (size_t i = 0; req && req->params && i < req->params->n; i++) {
		const char *kv = req->params->v[i];
		const char *eq;
		char name[64];
		size_t nlen;
		const et_field *f;
		char norm[8192];

		if (!kv)
			continue;
		eq = strchr(kv, '=');
		if (!eq)
			continue;
		nlen = (size_t)(eq - kv);
		if (nlen == 0 || nlen >= sizeof(name))
			continue;
		memcpy(name, kv, nlen);
		name[nlen] = '\0';

		/* 两个伪字段：直接落到文件（与旧 CBI 的 cfgvalue/write 等价） */
		if (!strcmp(name, "et_config") || !strcmp(name, "uuid")) {
			const char *path = !strcmp(name, "et_config") ? ET_FILE_TOML : ET_FILE_MACHINE_ID;

			if (!write_if_changed(path, eq + 1, !strcmp(name, "et_config"), err, sizeof(err))) {
				et_json_result(res, false, err);
				et_cfg_free(cfg);
				et_uci_close(u);
				return ET_OK;
			}
			applied++;
			continue;
		}

		f = et_schema_find(name);
		if (!f)
			continue; /* 未知字段忽略，便于前端/后端版本不一致时平滑升级 */

		if (f->kind == ET_K_LIST) {
			set_list_value(cfg, name, eq + 1);
			applied++;
			continue;
		}
		if (!norm_value(f, eq + 1, norm, sizeof(norm), err, sizeof(err))) {
			et_json_result(res, false, err);
			et_cfg_free(cfg);
			et_uci_close(u);
			return ET_OK;
		}
		{
			et_cfg_item *it = writable_item(cfg, name);

			if (!it)
				continue;
			free(it->val);
			it->val = strdup(norm);
			if (!it->val) {
				et_json_result(res, false, et_i18n("Save failed"));
				et_cfg_free(cfg);
				et_uci_close(u);
				return ET_OK;
			}
			applied++;
		}
	}

	if (applied == 0) {
		et_json_result(res, false, "No changes to save");
		et_cfg_free(cfg);
		et_uci_close(u);
		return ET_OK;
	}

	if (et_cfg_save(cfg, u, &changed) != ET_OK) {
		et_json_result(res, false, et_i18n("Save failed"));
		et_cfg_free(cfg);
		et_uci_close(u);
		return ET_OK;
	}

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_str(res, "message", et_i18n("Configuration saved successfully"));
	et_json_kv_bool(res, "changed", changed);
	et_json_end(res);

	et_cfg_free(cfg);
	et_uci_close(u);
	return ET_OK;
}
