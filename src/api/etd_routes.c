/*
 * etd_routes.c - 路由表
 *
 * 路由名与原 Lua controller 的端点名逐字一致，前端无需改动。
 * 新增路由时同步更新 luci-app-easytier/root/usr/share/rpcd/acl.d/ 的 ACL。
 */
#include "etd_api.h"

const et_route et_routes[] = {
	/* 状态与信息 */
	{ "api_status", et_api_status, "read" },
	{ "api_conninfo", et_api_conninfo, "read" },
	{ "get_tun_info", et_api_tun_info, "read" },
	{ "get_disk_space", et_api_disk_space, "read" },

	/* 日志 */
	{ "get_log", et_api_log_get, "read" },
	{ "get_log_size", et_api_log_size, "read" },
	{ "clear_log", et_api_log_clear, "write" },
	{ "get_wlog", et_api_wlog_get, "read" },
	{ "get_wlog_size", et_api_wlog_size, "read" },
	{ "clear_wlog", et_api_wlog_clear, "write" },
	{ "clear_version_cache", et_api_version_cache_clear, "write" },

	/* 上传/下载/安装 */
	{ "get_upload_config", et_api_upload_config_get, "read" },
	{ "save_upload_config", et_api_upload_config_save, "write" },
	{ "upload_binary", et_api_upload_binary, "write" },
	{ "download_easytier", et_api_download_start, "write" },
	{ "download_progress", et_api_download_progress, "read" },
	{ "cancel_download", et_api_download_cancel, "write" },

	/* Web 控制台 */
	{ "get_web_config", et_api_web_config_get, "read" },
	{ "save_web_config", et_api_web_config_save, "write" },
	{ "check_web_status", et_api_web_status, "read" },
	{ "reset_database", et_api_web_db_reset, "write" },

	/* 服务控制 */
	{ "restart_service", et_api_service_restart, "write" },
	{ "toggle_core", et_api_toggle_core, "write" },
	{ "toggle_web", et_api_toggle_web, "write" },

	/* 配置 */
	{ "config_get", et_api_config_get, "read" },
	{ "config_save", et_api_config_save, "write" },
	{ "config_render", et_api_config_render, "read" },
	{ "schema", et_api_schema, "read" },
};

const size_t et_routes_len = sizeof(et_routes) / sizeof(et_routes[0]);
