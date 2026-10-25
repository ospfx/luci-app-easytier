/*
 * etd_api_conninfo.c - 连接信息（node/peer/route/... 分区拉取）
 *
 * 对应原 controller 的 act_conninfo。
 *
 * easytier-cli 每调用一次都要新起一个进程（路由器上约 200ms 起），而 RPC 本身
 * 只要几毫秒，所以旧实现改成了"按分区按需加载 + 3 秒 TTL 缓存"，前端一次只
 * 请求当前显示的分区。这一层语义完全照搬：分区名与子命令的映射、缓存文件名、
 * 首行时间戳格式、未运行时清缓存，都与原来一致，旧前端无需改动。
 */
#include "etd_api_priv.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 原 CONNINFO_CLI_PARTS：分区名 → easytier-cli 子命令 */
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

/* 跑一个分区；子命令里带空格的（acl stats）按空格拆开传入 */
static char *run_part(const char *clibin, const char *cmd)
{
	et_list words;
	char *argv[8];
	et_buf out, err;
	size_t i;

	et_list_init(&words);
	et_list_split_ws(&words, cmd);
	if (words.n == 0 || words.n > 6)
		goto empty;

	argv[0] = (char *)clibin;
	for (i = 0; i < words.n; i++)
		argv[i + 1] = words.v[i];
	argv[words.n + 1] = NULL;

	et_buf_init(&out);
	et_buf_init(&err);
	/* 旧实现 2>&1 合并输出：错误信息也要原样给前端看 */
	et_run(argv, &out, &err, 10);
	if (err.p && err.len)
		et_buf_add(&out, err.p);
	if (!out.p)
		et_buf_add(&out, "");
	et_buf_free(&err);
	et_list_free(&words);
	return et_buf_steal(&out);

empty:
	et_list_free(&words);
	return strdup("");
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
	et_uci *u;
	char *core = NULL, *web = NULL, *clibin = NULL;
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

	u = et_uci_open();
	et_resolve_bins(u, &core, &web, &clibin);
	free(core);
	free(web);
	if (!clibin)
		clibin = strdup("/usr/bin/easytier-cli");

	pid = et_proc_pid_comm("easytier-core");

	res->type = ET_CT_JSON;
	et_json_begin(res);

	if (pid > 0) {
		for (size_t i = 0; i < CLI_PARTS_N; i++) {
			char *data;

			if (!want_cli[i])
				continue;
			data = cache_read(CLI_PARTS[i].name);
			if (!data) {
				data = run_part(clibin, CLI_PARTS[i].cmd);
				cache_write(CLI_PARTS[i].name, data);
			}
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

	free(clibin);
	free(body_parts);
	et_uci_close(u);
	return ET_OK;
}
