/*
 * etd_api.c - API 公共层：请求解析、响应构造、路由分发
 */
#include "etd_api.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *et_req_get(const et_req *r, const char *key)
{
	if (!r || !r->params || !key)
		return NULL;
	size_t kl = strlen(key);
	for (size_t i = 0; i < r->params->n; i++) {
		const char *kv = r->params->v[i];
		if (!kv)
			continue;
		if (strncmp(kv, key, kl) == 0 && kv[kl] == '=')
			return kv + kl + 1;
	}
	return NULL;
}

void et_res_init(et_res *r, et_ctype type)
{
	r->type = type;
	r->status = 200;
	et_buf_init(&r->body);
}

void et_res_free(et_res *r)
{
	et_buf_free(&r->body);
}

void et_res_text(et_res *r, const char *s)
{
	et_buf_add(&r->body, s ? s : "");
}

/* 判断当前是否为空对象/空数组的起始，用于决定是否补逗号 */
static bool et_json_need_comma(et_res *r)
{
	if (r->body.len == 0)
		return false;
	char last = r->body.p[r->body.len - 1];
	return last != '{' && last != '[';
}

void et_json_begin(et_res *r)
{
	et_buf_addc(&r->body, '{');
}

void et_json_end(et_res *r)
{
	et_buf_addc(&r->body, '}');
}

void et_json_kv_raw(et_res *r, const char *k, const char *raw)
{
	if (et_json_need_comma(r))
		et_buf_addc(&r->body, ',');
	et_buf_addc(&r->body, '"');
	et_buf_add_json(&r->body, k);
	et_buf_add(&r->body, "\":");
	et_buf_add(&r->body, raw ? raw : "null");
}

void et_json_kv_str(et_res *r, const char *k, const char *v)
{
	if (et_json_need_comma(r))
		et_buf_addc(&r->body, ',');
	et_buf_addc(&r->body, '"');
	et_buf_add_json(&r->body, k);
	et_buf_add(&r->body, "\":\"");
	if (v)
		et_buf_add_json(&r->body, v);
	et_buf_addc(&r->body, '"');
}

void et_json_kv_bool(et_res *r, const char *k, bool v)
{
	et_json_kv_raw(r, k, v ? "true" : "false");
}

void et_json_kv_int(et_res *r, const char *k, long long v)
{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%lld", v);
	et_json_kv_raw(r, k, tmp);
}

void et_json_result(et_res *r, bool ok, const char *msg)
{
	r->type = ET_CT_JSON;
	et_json_begin(r);
	et_json_kv_bool(r, "success", ok);
	if (msg)
		et_json_kv_str(r, "message", msg);
	et_json_end(r);
}

char *et_body_json_str(const et_req *r, const char *key)
{
	if (!r || !r->body || !key)
		return NULL;
	json_object *root = json_tokener_parse(r->body);
	if (!root)
		return NULL;
	json_object *v = NULL;
	if (!json_object_object_get_ex(root, key, &v)) {
		json_object_put(root);
		return NULL;
	}
	char *out = NULL;
	if (json_object_is_type(v, json_type_string))
		out = strdup(json_object_get_string(v));
	else if (json_object_is_type(v, json_type_int))
		out = strdup(json_object_to_json_string(v));
	else if (json_object_is_type(v, json_type_boolean))
		out = strdup(json_object_get_boolean(v) ? "1" : "0");
	json_object_put(root);
	return out;
}

int et_api_dispatch(const et_req *req, et_res *res)
{
	if (!req || !req->method)
		return ET_EINVAL;
	for (size_t i = 0; i < et_routes_len; i++) {
		if (req->method && strcmp(et_routes[i].name, req->method) == 0)
			return et_routes[i].fn(req, res);
	}
	return ET_ENOENT;
}
