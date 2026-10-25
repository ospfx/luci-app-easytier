/*
 * etd_api_priv.h - 对外 API 面的私有公共件
 *
 * 只服务于 src/api/etd_api_{status,conninfo,log,web,ops}.c，不属于对外契约。
 * 这里只放真正被多个 handler 复用的零碎工具：参数取值、JSON 数组输出、
 * 运行时长与版本缓存的读写。稍重的逻辑都留在各自的 .c 里。
 */
#ifndef ETD_API_PRIV_H
#define ETD_API_PRIV_H

#include "etd_api.h"
#include "etd_cfg.h"
#include "etd_proc.h"
#include "etd_uci.h"
#include "etd_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 取请求参数：JSON 体优先，其次 k=v 查询串。返回 malloc 串，调用者 free。
 * 旧实现同一端点既有 formvalue 又有 json.parse 两条来源，这里统一顺序，
 * 让 handler 不必关心请求来自哪条转发路径。 */
static inline char *et_api_arg(const et_req *req, const char *key)
{
	char *v;
	const char *p;

	if (!req || !key)
		return NULL;
	v = et_body_json_str(req, key);
	if (v)
		return v;
	p = et_req_get(req, key);
	return p ? strdup(p) : NULL;
}

/* 输出 JSON 字符串数组：{"k":["a","b"]} */
static inline void et_json_kv_strv(et_res *r, const char *k, char *const *v,
				   size_t n)
{
	et_buf b;
	size_t i;

	et_buf_init(&b);
	et_buf_addc(&b, '[');
	for (i = 0; i < n; i++) {
		if (i)
			et_buf_addc(&b, ',');
		et_buf_addc(&b, '"');
		et_buf_add_json(&b, v[i] ? v[i] : "");
		et_buf_addc(&b, '"');
	}
	et_buf_addc(&b, ']');
	et_json_kv_raw(r, k, b.p ? b.p : "[]");
	et_buf_free(&b);
}

static inline void et_json_kv_strlist(et_res *r, const char *k,
				      const et_list *l)
{
	if (!l) {
		et_json_kv_raw(r, k, "[]");
		return;
	}
	et_json_kv_strv(r, k, l->v, l->n);
}

/* /tmp/easytier_time 这类"起始时间戳"文件 → "3天 04小时05分06秒"。
 * 文件缺失、内容无数字、系统时间回拨都按 0 处理，与原 calc_uptime 一致。 */
static inline char *et_api_uptime_text(const char *path)
{
	char *content = et_read_file(path, NULL);
	char out[128];
	const char *p;
	long start, elapsed;

	if (!content)
		return strdup("");
	p = content;
	while (*p && (*p < '0' || *p > '9'))
		p++;
	if (!*p) {
		free(content);
		return strdup("");
	}
	start = strtol(p, NULL, 10);
	elapsed = (long)time(NULL) - start;
	free(content);
	et_format_uptime(elapsed, out, sizeof(out));
	return strdup(out);
}

/* 读版本缓存，去掉所有 \r\n（旧实现 gsub("[\r\n]+", "")）；
 * 文件不存在或内容为空返回 NULL —— 调用方据此决定是否重新探测。 */
static inline char *et_api_tag_read(const char *path)
{
	char *s = et_read_file(path, NULL);
	char *d;

	if (!s)
		return NULL;
	d = s;
	for (const char *p = s; *p; p++) {
		if (*p == '\n' || *p == '\r')
			continue;
		*d++ = *p;
	}
	*d = '\0';
	if (d == s) {
		free(s);
		return NULL;
	}
	return s;
}

/* 写版本缓存：et_write_file 走 "临时文件 + rename"，并发写不会读到半个文件 */
static inline int et_api_tag_write(const char *path, const char *val)
{
	return et_write_file(path, val ? val : "", val ? strlen(val) : 0);
}

/* easytier-web 进程号。
 * comm 被内核截断到 15 字节，直接装 easytier-web-embed 时会变成
 * "easytier-web-em"，与 pgrep easytier-web 的前缀语义等价，故两次精确查。 */
pid_t et_api_pid_web(void);

/* 释放 et_uci_get_et_list() 返回的 char** */
static inline void et_api_free_strv(char **v)
{
	if (!v)
		return;
	for (char **p = v; *p; p++)
		free(*p);
	free(v);
}

#endif /* ETD_API_PRIV_H */
