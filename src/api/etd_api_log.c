/*
 * etd_api_log.c - 日志读取/大小/清空，版本缓存清理
 *
 * 对应原 controller 的 get_log / get_log_size / clear_log /
 * get_wlog / get_wlog_size / clear_wlog / clear_version_cache。
 *
 * 两处必须守住的语义：
 *   1. 日志以纯文本返回并去掉 ANSI 转义（旧实现 sed 's/\x1b\[[0-9;]*m//g'）；
 *   2. 清空是原地截断而不是 unlink —— easytier-core 进程的 fd 仍指向旧 inode，
 *      unlink 会让日志看起来"清空后又复活"且继续占用磁盘。
 */
#include "etd_api_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int log_get(const char *path, et_res *res)
{
	char *raw = et_read_file(path, NULL);
	char *clean;

	res->type = ET_CT_TEXT;
	if (!raw) {
		/* 文件还不存在时按空日志处理，页面上是"暂无日志" */
		et_res_text(res, "");
		return ET_OK;
	}
	clean = et_strip_ansi(raw);
	free(raw);
	if (!clean) {
		et_res_text(res, "");
		return ET_OK;
	}
	et_res_text(res, clean);
	free(clean);
	return ET_OK;
}

static int log_size(const char *path, et_res *res)
{
	long long sz = et_file_size(path);

	if (sz < 0)
		sz = 0;
	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_int(res, "size", sz);
	et_json_end(res);
	return ET_OK;
}

/* 原地截断：fork 与否都留同一个 inode，写入方（core/web）不受影响 */
static int log_clear(const char *path, et_res *res)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd >= 0)
		close(fd);
	res->type = ET_CT_TEXT;
	et_res_text(res, "");
	return fd >= 0 ? ET_OK : ET_ERR;
}

int et_api_log_get(const et_req *req, et_res *res)
{
	(void)req;
	return log_get(ET_LOG_CORE, res);
}

int et_api_log_size(const et_req *req, et_res *res)
{
	(void)req;
	return log_size(ET_LOG_CORE, res);
}

int et_api_log_clear(const et_req *req, et_res *res)
{
	(void)req;
	return log_clear(ET_LOG_CORE, res);
}

int et_api_wlog_get(const et_req *req, et_res *res)
{
	(void)req;
	return log_get(ET_LOG_WEB, res);
}

int et_api_wlog_size(const et_req *req, et_res *res)
{
	(void)req;
	return log_size(ET_LOG_WEB, res);
}

int et_api_wlog_clear(const et_req *req, et_res *res)
{
	(void)req;
	return log_clear(ET_LOG_WEB, res);
}

/* type=core → 清 easytiernew.tag + easytier.tag；type=web → easytiernew.tag + easytierweb.tag */
int et_api_version_cache_clear(const et_req *req, et_res *res)
{
	char *type = et_api_arg(req, "type");

	if (type) {
		if (!strcmp(type, "core")) {
			et_unlink(ET_TAG_LATEST);
			et_unlink(ET_TAG_CORE);
		} else if (!strcmp(type, "web")) {
			et_unlink(ET_TAG_LATEST);
			et_unlink(ET_TAG_WEB);
		}
	}
	free(type);

	/* 旧实现无论 type 是否有效都回 "OK" 纯文本 */
	res->type = ET_CT_TEXT;
	et_res_text(res, "OK");
	return ET_OK;
}
