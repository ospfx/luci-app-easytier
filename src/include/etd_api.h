/*
 * etd_api.h - 后端 API 契约
 *
 * 每个 HTTP 端点（原 Lua controller 里的 call() 函数）对应一个 handler。
 * 调用方有三条路径，均走同一套 handler：
 *   easytierctl api <method> k=v...   （供极薄 Lua 转发层调用）
 *   ubus call easytier <method> {...}
 *   easytierd 内部（daemon 自身调用）
 *
 * 路由名与原 Lua 端点名**逐字一致**，因此现有前端页面无需改动即可工作。
 */
#ifndef ETD_API_H
#define ETD_API_H

#include "etd.h"
#include "etd_cfg.h"
#include "etd_util.h"

typedef enum {
	ET_CT_JSON = 0,
	ET_CT_TEXT,
	ET_CT_HTML,
	ET_CT_NONE = -1,
} et_ctype;

/* 请求 */
typedef struct {
	const char *method;
	const et_list *params; /* "k=v" 形式，key 不重复 */
	const char *body;      /* POST 原始体（JSON 或 form），可为 NULL */
	size_t body_len;
} et_req;

/* 响应 */
typedef struct {
	et_ctype type;
	et_buf body;
	int status; /* HTTP 状态码，默认 200 */
} et_res;

const char *et_req_get(const et_req *r, const char *key);

void et_res_init(et_res *r, et_ctype type);
void et_res_free(et_res *r);
/* 便捷输出 */
void et_res_text(et_res *r, const char *s);

/* JSON 片段写入（保证逗号正确） */
void et_json_begin(et_res *r);            /* "{"  */
void et_json_end(et_res *r);              /* "}"  */
void et_json_kv_str(et_res *r, const char *k, const char *v);
void et_json_kv_bool(et_res *r, const char *k, bool v);
void et_json_kv_int(et_res *r, const char *k, long long v);
void et_json_kv_raw(et_res *r, const char *k, const char *raw);
/* 输出 {success:bool, message:...} */
void et_json_result(et_res *r, bool ok, const char *msg);

/* 从 JSON body 取字符串字段（malloc），缺失返回 NULL */
char *et_body_json_str(const et_req *r, const char *key);

/* ---------------- handler ---------------- */
typedef int (*et_handler)(const et_req *req, et_res *res);

typedef struct {
	const char *name;
	et_handler fn;
	const char *acl; /* "read" / "write"，文档用 */
} et_route;

extern const et_route et_routes[];
extern const size_t et_routes_len;
/* 名称分发（未知方法返回 -ENOENT） */
int et_api_dispatch(const et_req *req, et_res *res);

/* ============ 状态与信息（owner: api-surface） ============ */
int et_api_status(const et_req *req, et_res *res);
int et_api_conninfo(const et_req *req, et_res *res);
int et_api_tun_info(const et_req *req, et_res *res);
int et_api_disk_space(const et_req *req, et_res *res);

/* ============ 日志 ============ */
int et_api_log_get(const et_req *req, et_res *res);
int et_api_log_size(const et_req *req, et_res *res);
int et_api_log_clear(const et_req *req, et_res *res);
int et_api_wlog_get(const et_req *req, et_res *res);
int et_api_wlog_size(const et_req *req, et_res *res);
int et_api_wlog_clear(const et_req *req, et_res *res);
int et_api_version_cache_clear(const et_req *req, et_res *res);

/* ============ 上传/下载/安装 ============ */
int et_api_upload_config_get(const et_req *req, et_res *res);
int et_api_upload_config_save(const et_req *req, et_res *res);
int et_api_upload_binary(const et_req *req, et_res *res);
int et_api_download_start(const et_req *req, et_res *res);
int et_api_download_progress(const et_req *req, et_res *res);
int et_api_download_cancel(const et_req *req, et_res *res);

/* ============ Web 控制台 ============ */
int et_api_web_config_get(const et_req *req, et_res *res);
int et_api_web_config_save(const et_req *req, et_res *res);
int et_api_web_status(const et_req *req, et_res *res);
int et_api_web_db_reset(const et_req *req, et_res *res);

/* ============ 服务控制（owner: daemon-rt） ============ */
int et_api_service_restart(const et_req *req, et_res *res);
int et_api_toggle_core(const et_req *req, et_res *res);
int et_api_toggle_web(const et_req *req, et_res *res);

/* ============ 配置读写与表单（owner: web-bridge） ============ */
int et_api_config_get(const et_req *req, et_res *res);
int et_api_config_save(const et_req *req, et_res *res);
int et_api_config_render(const et_req *req, et_res *res);
int et_api_schema(const et_req *req, et_res *res);

#endif /* ETD_API_H */
