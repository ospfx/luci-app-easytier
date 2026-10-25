/*
 * etd_api_web.c - Web 控制台（easytier-web）配置、状态与数据库重置
 *
 * 对应原 controller 的 get_web_config / save_web_config /
 * check_web_status / reset_database。
 *
 * 字段与默认值逐字照抄旧实现（前端按字符串 "0"/"1"/"off" 判断勾选状态，
 * 任何类型变化都会让页面静默错位），因此这里统一以字符串读写 UCI。
 */
#include "etd_api_priv.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
	const char *key;
	const char *def;
} WEB_FIELDS[] = {
	{ "web_enabled", "0" },
	{ "web_db_path", "/etc/easytier/et.db" },
	{ "web_protocol", "udp" },
	{ "web_port", "22020" },
	{ "web_fw_web", "0" },
	{ "web_api_port", "11211" },
	{ "web_html_port", "11211" },
	{ "web_api_addr", "0.0.0.0" },
	{ "web_html_addr", "0.0.0.0" },
	{ "web_fw_api", "0" },
	{ "web_api_host", "" },
	{ "web_geoip_db", "" },
	{ "web_disable_registration", "0" },
	{ "web_allow_auto_create_user", "0" },
	/* EasyTier 2.7.0 新增 */
	{ "web_heartbeat_min_response_ms", "" },
	{ "web_heartbeat_timeout_ms", "" },
	{ "web_gateway_peer_url", "" },
	{ "web_gateway_relay_data", "0" },
	{ "web_weblog", "off" },
	/* OIDC */
	{ "web_oidc_enabled", "0" },
	{ "web_oidc_issuer_url", "" },
	{ "web_oidc_client_id", "" },
	{ "web_oidc_client_secret", "" },
	{ "web_oidc_redirect_url", "" },
	{ "web_oidc_username_claim", "preferred_username" },
	{ "web_oidc_scopes", "openid profile" },
	{ "web_oidc_frontend_base_url", "" },
	{ "web_oidc_disable_pkce", "0" },
	/* Webhook */
	{ "web_webhook_enabled", "0" },
	{ "web_webhook_url", "" },
	{ "web_webhook_secret", "" },
	{ "web_internal_auth_token", "" },
	{ "web_web_instance_id", "" },
	{ "web_web_instance_api_base_url", "" },
};

#define WEB_FIELDS_N (sizeof(WEB_FIELDS) / sizeof(WEB_FIELDS[0]))

int et_api_web_config_get(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();

	(void)req;
	res->type = ET_CT_JSON;
	et_json_begin(res);
	for (size_t i = 0; i < WEB_FIELDS_N; i++) {
		char *v = u ? et_uci_get_et(u, WEB_FIELDS[i].key) : NULL;

		et_json_kv_str(res, WEB_FIELDS[i].key,
			       (v && *v) ? v : WEB_FIELDS[i].def);
		free(v);
	}
	et_json_end(res);
	et_uci_close(u);
	return ET_OK;
}

int et_api_web_config_save(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();
	int rc = ET_OK;

	if (!u) {
		et_json_result(res, false, "uci 配置不可用");
		return ET_ERR;
	}
	for (size_t i = 0; i < WEB_FIELDS_N; i++) {
		char *v = et_api_arg(req, WEB_FIELDS[i].key);
		/* 前端总是提交全部字段；缺失时按默认值落库，与旧实现 formvalue or 默认 一致 */
		int r = et_uci_set_et(u, WEB_FIELDS[i].key,
				      (v && *v) ? v : WEB_FIELDS[i].def);

		free(v);
		if (r != ET_OK && rc == ET_OK)
			rc = r;
	}
	if (rc == ET_OK)
		rc = et_uci_commit_et(u);
	et_uci_close(u);

	if (rc != ET_OK) {
		et_json_result(res, false, "配置写入失败");
		return ET_ERR;
	}
	et_json_result(res, true, NULL);
	return ET_OK;
}

int et_api_web_status(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();
	char *port = u ? et_uci_get_et(u, "web_html_port") : NULL;

	(void)req;
	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "running", et_api_pid_web() > 0);
	et_json_kv_str(res, "port", (port && *port) ? port : "11211");
	et_json_end(res);

	free(port);
	et_uci_close(u);
	return ET_OK;
}

/* 路径白名单式校验：必须是绝对路径，且不得落在系统目录下（旧实现同款前缀判断） */
static bool db_path_safe(const char *p)
{
	static const char *const bad[] = {
		"/bin", "/sbin", "/usr/bin", "/usr/sbin", "/lib", "/boot"
	};

	if (!p || p[0] != '/' || !strcmp(p, "/"))
		return false;
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (et_starts_with(p, bad[i]))
			return false;
	}
	return true;
}

int et_api_web_db_reset(const et_req *req, et_res *res)
{
	char *db_path = et_api_arg(req, "db_path");
	et_list deleted;
	char *files[3] = { NULL, NULL, NULL };

	if (!db_path || !*db_path) {
		et_json_result(res, false, "Missing db_path parameter");
		free(db_path);
		return ET_EINVAL;
	}
	if (!db_path_safe(db_path)) {
		et_json_result(res, false, "Invalid database path");
		free(db_path);
		return ET_EINVAL;
	}

	/* SQLite 的 -wal/-shm 必须一并删掉，否则重建时会把旧数据写回去 */
	et_list_init(&deleted);
	for (size_t i = 0; i < 3; i++) {
		if (i == 0) {
			files[i] = strdup(db_path);
		} else if (asprintf(&files[i], "%s-%s", db_path,
				    i == 1 ? "wal" : "shm") < 0) {
			files[i] = NULL;
		}
		if (!files[i])
			continue;
		if (et_unlink(files[i]) == ET_OK)
			et_list_add(&deleted, files[i]);
		free(files[i]);
	}

	res->type = ET_CT_JSON;
	et_json_begin(res);
	if (deleted.n > 0) {
		et_json_kv_bool(res, "success", true);
		et_json_kv_strlist(res, "deleted_files", &deleted);
		et_json_kv_str(res, "message", "数据库重置成功");
		et_json_end(res);
	} else {
		et_json_kv_bool(res, "success", false);
		et_json_kv_str(res, "message", "未找到数据库文件或删除失败");
		et_json_end(res);
	}
	et_list_free(&deleted);
	free(db_path);
	return ET_OK;
}
