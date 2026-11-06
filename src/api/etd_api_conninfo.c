/*
 * etd_api_conninfo.c - 连接信息（node/peer/route/... 分区拉取）
 *
 * 对应原 controller 的 act_conninfo。
 *
 * 历史：旧实现按分区 fork 一次上游 easytier-cli（路由器上约 200ms/进程），前端一次只请求
 * 当前显示的分区，再叠加 3 秒 TTL 缓存。C 重写后子命令已经在 lib/etd_cli_*.c 里，
 * 这里改为**进程内**直接调用 et_cli_render_part()：一次 RPC 往返（几毫秒），
 * 不再 fork、不再依赖 /usr/bin/easytier-cli。
 * 分区名与子命令的映射、缓存文件名、首行时间戳格式、未运行时清缓存，都与原来一致，
 * 旧前端无需改动（仍然按 Markdown 表格 | a | b | 解析）。
 */
#include "etd_api_priv.h"
#include "etd_cli.h"
#include "etd_uci.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 原 CONNINFO_CLI_PARTS：分区名 → 子命令（现在用于选渲染函数） */
static const struct {
	const char *name;
	const char *cmd;
} CLI_PARTS[] = {
	{ "node", "node" },
	{ "peer", "peer" },
	{ "route", "route" },
	{ "connector", "connector" },
	{ "stun", "stun" },
	{ "peer_center", "peer-center" },
	{ "vpn_portal", "vpn-portal" },
	{ "proxy", "proxy" },
	{ "acl", "acl stats" },
	{ "mapped_listener", "mapped-listener" },
	{ "stats", "stats" },
};

#define CLI_PARTS_N (sizeof(CLI_PARTS) / sizeof(CLI_PARTS[0]))

#define MSG_NOT_RUNNING "错误：程序未运行！请先启动程序后刷新"

static void cache_path(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", ET_CONNINFO_DIR, name);
}

/* 缓存格式："<unix时间戳>\n<内容>"；超过 TTL 视为未命中 */
static char *cache_read(const char *name)
{
	char path[256];
	char *s, *nl, *data;
	long ts = 0;

	cache_path(path, sizeof(path), name);
	s = et_read_file(path, NULL);
	if (!s)
		return NULL;
	nl = strchr(s, '\n');
	if (!nl || !et_parse_int(s, &ts) ||
	    (long)time(NULL) - ts > ET_CONNINFO_TTL) {
		free(s);
		return NULL;
	}
	data = strdup(nl + 1);
	free(s);
	return data ? data : strdup("");
}

static void cache_write(const char *name, const char *data)
{
	char path[256];
	et_buf b;

	if (et_mkdir_p(ET_CONNINFO_DIR, 0755) != ET_OK)
		return;
	cache_path(path, sizeof(path), name);
	et_buf_init(&b);
	et_buf_addf(&b, "%ld\n", (long)time(NULL));
	et_buf_add(&b, data ? data : "");
	et_write_file(path, b.p ? b.p : "", b.len);
	et_buf_free(&b);
}

/* 从 UCI 取 rpc_portal 端口（默认 15888），拼成 CLI 用的 -p 地址。
 * core 用 "-r <port>" 启动，用户改过端口时状态页必须跟着改，否则连不上。 */
static void conninfo_rpc_addr(char *out, size_t cap)
{
	et_uci *u;
	char *port;

	snprintf(out, cap, "127.0.0.1:15888");
	u = et_uci_open();
	if (!u)
		return;
	port = et_uci_get_et(u, "rpc_portal");
	et_uci_close(u);
	if (!port)
		return;
	if (port[0] && strcmp(port, "0"))
		snprintf(out, cap, "127.0.0.1:%s", port);
	free(port);
}

/* 进程内渲染一个分区（带缓存）。失败时给前端一条可读的错误文本。 */
static char *render_part(const char *name)
{
	char rpc[64];
	char *data = cache_read(name);
	et_buf b;
	int rc;

	if (data)
		return data;

	conninfo_rpc_addr(rpc, sizeof(rpc));
	et_buf_init(&b);
	rc = et_cli_render_part_ex(name, rpc, &b);
	if (rc || !b.p || !b.len) {
		et_buf_free(&b);
		et_buf_init(&b);
		et_buf_addf(&b, "easytier-cli: failed to get %s\n", name);
	}
	data = et_buf_steal(&b);
	if (!data)
		data = strdup("");
	cache_write(name, data);
	return data;
}

/* 命令行是否以配置文件启动：--config-file ... 或 -c /xxx */
static bool uses_config_file(const char *cmdline)
{
	if (!cmdline)
		return false;
	if (strstr(cmdline, "--config-file"))
		return true;
	for (const char *p = cmdline; (p = strstr(p, "-c")) != NULL; p += 2) {
		const char *q = p + 2;

		if (p > cmdline && p[-1] != ' ' && p[-1] != '\t')
			continue; /* 只认独立参数，避免命中 -cfg 之类 */
		if (*q != ' ' && *q != '\t')
			continue;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q == '/')
			return true;
	}
	return false;
}

/* 解析 parts="node,peer"：只认 [A-Za-z0-9_] 组成的词，其余字符都是分隔符 */
static void parse_parts(const char *parts, bool *want_cli, bool *want_cmdline)
{
	const char *p = parts;

	memset(want_cli, 0, CLI_PARTS_N * sizeof(bool));
	*want_cmdline = false;
	while (p && *p) {
		const char *st;
		size_t n;

		while (*p && !(isalnum((unsigned char)*p) || *p == '_'))
			p++;
		st = p;
		while (isalnum((unsigned char)*p) || *p == '_')
			p++;
		n = (size_t)(p - st);
		if (n == 0)
			break;
		if (n == 7 && !strncmp(st, "cmdline", 7)) {
			*want_cmdline = true;
			continue;
		}
		for (size_t i = 0; i < CLI_PARTS_N; i++) {
			if (strlen(CLI_PARTS[i].name) == n &&
			    !strncmp(st, CLI_PARTS[i].name, n)) {
				want_cli[i] = true;
				break;
			}
		}
	}
}

int et_api_conninfo(const et_req *req, et_res *res)
{
	const char *parts = et_req_get(req, "parts");
	char *body_parts = NULL;
	bool want_cli[CLI_PARTS_N];
	bool want_cmdline = false;
	pid_t pid;

	if (!parts || !*parts) {
		body_parts = et_body_json_str(req, "parts");
		if (body_parts && *body_parts)
			parts = body_parts;
	}

	if (parts && *parts) {
		parse_parts(parts, want_cli, &want_cmdline);
	} else {
		/* 不传 parts：加载除 stun 外的全部分区（stun 会实时访问 STUN 服务器） */
		for (size_t i = 0; i < CLI_PARTS_N; i++)
			want_cli[i] = strcmp(CLI_PARTS[i].name, "stun") != 0;
		want_cmdline = true;
	}

	pid = et_proc_pid_comm("easytier-core");

	res->type = ET_CT_JSON;
	et_json_begin(res);

	if (pid > 0) {
		for (size_t i = 0; i < CLI_PARTS_N; i++) {
			char *data;

			if (!want_cli[i])
				continue;
			data = render_part(CLI_PARTS[i].name);
			et_json_kv_str(res, CLI_PARTS[i].name, data);
			free(data);
		}
		if (want_cmdline) {
			char *cl = et_proc_cmdline(pid);
			char *cfg = NULL;

			if (!cl)
				cl = strdup("");
			et_json_kv_str(res, "cmdline", cl);
			if (uses_config_file(cl))
				cfg = et_read_file(ET_FILE_TOML, NULL);
			et_json_kv_str(res, "config_file", cfg ? cfg : "");
			free(cfg);
			free(cl);
		}
	} else {
		/* 未运行时清掉缓存，避免下次启动后读到旧的错误输出 */
		for (size_t i = 0; i < CLI_PARTS_N; i++) {
			if (want_cli[i])
				et_json_kv_str(res, CLI_PARTS[i].name,
					       MSG_NOT_RUNNING);
		}
		if (want_cmdline)
			et_json_kv_str(res, "cmdline", MSG_NOT_RUNNING);
		et_rm_rf(ET_CONNINFO_DIR);
	}

	et_json_end(res);

	free(body_parts);
	return ET_OK;
}