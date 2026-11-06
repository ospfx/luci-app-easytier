/*
 * etd_cli_common.c - CLI 公共设施：选项、Markdown 表格、JSON、RPC、格式化
 *
 * 版式/数值格式全部对照上游 easytier-cli（Rust tabled + humansize + serde_json）
 * 逐字复刻，见 docs/CLI-PORT.md。
 */
#include "etd_cli.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ET_CLI_RPC_DEFAULT "127.0.0.1:15888"
#define ET_CLI_TIMEOUT_MS 5000

void et_cli_opts_init(struct et_cli_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->rpc = ET_CLI_RPC_DEFAULT;
	o->format = "table";
}

int et_cli_want_json(const struct et_cli_opts *o)
{
	return o->format && !strcmp(o->format, "json");
}

int et_cli_puts(struct et_cli_ctx *ctx, const char *s)
{
	return et_buf_add(&ctx->out, s ? s : "");
}

int et_cli_printf(struct et_cli_ctx *ctx, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return ET_ERR;
	return et_buf_addn(&ctx->out, tmp, (size_t)n);
}

int et_cli_flush(struct et_cli_ctx *ctx)
{
	if (!ctx->out.p || !ctx->out.len)
		return 0;
	if (write(STDOUT_FILENO, ctx->out.p, ctx->out.len) < 0)
		return -1;
	return 0;
}

/* ================= 表格 ================= */

void et_table_init(et_table *t, int ncol, const char *const *hdr)
{
	memset(t, 0, sizeof(*t));
	t->ncol = ncol;
	t->hdr = (const char **)hdr;
}

int et_table_add_row_owned(et_table *t, char **cells)
{
	if (t->nrow >= t->cap) {
		int cap = t->cap ? t->cap * 2 : 8;
		char ***p = realloc(t->rows, (size_t)cap * sizeof(*p));

		if (!p)
			return -1;
		t->rows = p;
		t->cap = cap;
	}
	t->rows[t->nrow] = cells;
	t->nrow++;
	return 0;
}

int et_table_add_row(et_table *t, const char *const *cells)
{
	char **row = calloc((size_t)t->ncol, sizeof(char *));

	if (!row)
		return -1;
	for (int j = 0; j < t->ncol; j++) {
		row[j] = strdup(cells[j] ? cells[j] : "");
		if (!row[j]) {
			for (int k = 0; k < j; k++)
				free(row[k]);
			free(row);
			return -1;
		}
	}
	if (et_table_add_row_owned(t, row)) {
		for (int j = 0; j < t->ncol; j++)
			free(row[j]);
		free(row);
		return -1;
	}
	return 0;
}

void et_table_free(et_table *t)
{
	for (int i = 0; i < t->nrow; i++) {
		for (int j = 0; j < t->ncol; j++)
			free(t->rows[i][j]);
		free(t->rows[i]);
	}
	free(t->rows);
	memset(t, 0, sizeof(*t));
}

/* 单元格显示宽度：按字符（UTF-8 多字节算 1 格，对齐上游 UnicodeWidthStr 的
 * 近似；relay/hostname 等实际内容都是 ASCII）。多行取最长一行。 */
static size_t cell_width(const char *s)
{
	size_t best = 0, cur = 0;

	if (!s)
		return 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '\n') {
			if (cur > best)
				best = cur;
			cur = 0;
			continue;
		}
		if ((*p & 0xc0) == 0x80)
			continue; /* UTF-8 续字节 */
		cur++;
	}
	if (cur > best)
		best = cur;
	return best;
}

static int cell_lines(const char *s)
{
	int n = 1;

	if (!s)
		return 1;
	for (const char *p = s; *p; p++)
		if (*p == '\n')
			n++;
	return n;
}

/* 终端宽度；stdout 不是终端时返回 -1（上游 terminal_size() 返回 None） */
static int terminal_table_width(void)
{
	struct winsize ws;

	if (!isatty(STDOUT_FILENO))
		return -1;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
		return -1;
	return (int)ws.ws_col - 1;
}

/* 表格总宽：内容宽 + 3*列数 + 1（与上游 table_total_width 一致） */
static size_t table_total_width(const size_t *w, const int *active, int ncol)
{
	size_t total = 0;
	int cnt = 0;

	for (int j = 0; j < ncol; j++) {
		if (!active[j])
			continue;
		total += w[j];
		cnt++;
	}
	return total + (size_t)(3 * cnt) + 1;
}

/* 裁剪后的单元格：按显示宽度截断并加 "..." 后缀（tabled Width::truncate） */
static void truncate_cell(const char *s, size_t keep, char *dst, size_t cap)
{
	size_t cols = 0;
	size_t i = 0;

	while (s[i]) {
		if (s[i] == '\n') {
			/* 多行单元格不裁剪后续行，直接截断到首行 */
			break;
		}
		if (((unsigned char)s[i] & 0xc0) == 0x80) {
			i++;
			continue;
		}
		if (cols >= keep)
			break;
		cols++;
		i++;
	}
	if (i >= cap - 4)
		i = cap - 4;
	memcpy(dst, s, i);
	dst[i] = 0;
}

static void render_cell_line(et_buf *out, const char *cell, int line,
			     size_t width, size_t keep)
{
	/* 跳到第 line 行 */
	const char *p = cell ? cell : "";
	for (int i = 0; i < line && p; i++) {
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!p) {
		for (size_t k = 0; k < width; k++)
			et_buf_addc(out, ' ');
		return;
	}
	const char *end = strchr(p, '\n');
	size_t n = end ? (size_t)(end - p) : strlen(p);
	char tmp[1024];
	size_t shown;

	if (keep && cell_width(cell) > keep) {
		truncate_cell(p, keep < 4 ? 0 : keep, tmp, sizeof(tmp));
		/* 上游后缀固定是 "..."，计入宽度 */
		size_t tw = strlen(tmp);
		if (tw + 3 <= keep) {
			memcpy(tmp + tw, "...", 4);
			tw += 3;
			shown = tw;
		} else {
			shown = tw;
			tmp[shown] = 0;
		}
		et_buf_addn(out, tmp, shown);
	} else if (n >= sizeof(tmp)) {
		et_buf_addn(out, p, n);
		shown = n;
	} else {
		memcpy(tmp, p, n);
		tmp[n] = 0;
		et_buf_add(out, tmp);
		shown = cell_width(tmp);
	}
	for (size_t k = shown; k < width; k++)
		et_buf_addc(out, ' ');
}

int et_table_render(et_table *t, const struct et_cli_opts *o, et_buf *out)
{
	int ncol = t->ncol;
	size_t *w;
	int *active;
	int *dropped;
	size_t *keep;
	int ndrop = 0;
	int term;

	if (ncol <= 0)
		return 0;
	w = calloc((size_t)ncol, sizeof(*w));
	active = calloc((size_t)ncol, sizeof(*active));
	dropped = calloc((size_t)ncol, sizeof(*dropped));
	keep = calloc((size_t)ncol, sizeof(*keep));
	if (!w || !active || !dropped || !keep) {
		free(w);
		free(active);
		free(dropped);
		free(keep);
		return -1;
	}
	for (int j = 0; j < ncol; j++)
		active[j] = 1;

	for (int j = 0; j < ncol; j++) {
		w[j] = cell_width(t->hdr[j]);
		for (int i = 0; i < t->nrow; i++) {
			size_t cw = cell_width(t->rows[i][j]);

			if (cw > w[j])
				w[j] = cw;
		}
	}

	/* 按终端宽度决定丢列/截列。--no-trunc 或非终端时原样输出。 */
	term = o && o->no_trunc ? -1 : terminal_table_width();
	if (term > 0 && table_total_width(w, active, ncol) > (size_t)term) {
		/* 丢列顺序由各子命令的 opts 决定；这里用通用顺序：
		 * 从最后一列往前丢，直到宽度够（上游按 drop_columns 清单丢）。 */
		for (int j = ncol - 1; j > 0; j--) {
			if (table_total_width(w, active, ncol) <= (size_t)term)
				break;
			active[j] = 0;
			dropped[ndrop++] = j;
		}
	}
	(void)dropped;

	for (int j = 0; j < ncol; j++) {
		if (!active[j])
			continue;
		et_buf_add(out, "| ");
		et_buf_add(out, t->hdr[j] ? t->hdr[j] : "");
		for (size_t k = cell_width(t->hdr[j]); k < w[j]; k++)
			et_buf_addc(out, ' ');
		et_buf_add(out, " ");
	}
	et_buf_add(out, "|\n");

	for (int j = 0; j < ncol; j++) {
		if (!active[j])
			continue;
		et_buf_addc(out, '|');
		for (size_t k = 0; k < w[j] + 2; k++)
			et_buf_addc(out, '-');
	}
	et_buf_add(out, "|\n");

	for (int i = 0; i < t->nrow; i++) {
		int lines = 1;

		for (int j = 0; j < ncol; j++) {
			if (!active[j])
				continue;
			int l = cell_lines(t->rows[i][j]);

			if (l > lines)
				lines = l;
		}
		for (int l = 0; l < lines; l++) {
			for (int j = 0; j < ncol; j++) {
				if (!active[j])
					continue;
				et_buf_add(out, "| ");
				render_cell_line(out, t->rows[i][j], l, w[j],
						 keep[j]);
				et_buf_add(out, " ");
			}
			et_buf_add(out, "|\n");
		}
	}
	free(w);
	free(active);
	free(dropped);
	free(keep);
	return 0;
}

int et_table_print(et_table *t, const struct et_cli_opts *o)
{
	et_buf b;
	int rc;

	et_buf_init(&b);
	rc = et_table_render(t, o, &b);
	if (rc == 0 && b.len)
		fwrite(b.p ? b.p : "", 1, b.len, stdout);
	et_buf_free(&b);
	return rc;
}

int et_table_kv_print(const char *const *labels, const char *const *values,
		      int n, const struct et_cli_opts *o, et_buf *out)
{
	et_table t;
	int rc = 0;

	if (n <= 0)
		return 0;
	et_table_init(&t, 2, NULL);
	t.hdr = malloc(2 * sizeof(char *));
	if (!t.hdr)
		return -1;
	t.hdr[0] = labels[0] ? strdup(labels[0]) : strdup("");
	t.hdr[1] = values[0] ? strdup(values[0]) : strdup("");
	for (int i = 1; i < n; i++) {
		const char *cells[2] = { labels[i] ? labels[i] : "",
					 values[i] ? values[i] : "" };

		if (et_table_add_row(&t, cells)) {
			rc = -1;
			goto out;
		}
	}
	rc = et_table_render(&t, o, out);
out:
	free((char *)t.hdr[0]);
	free((char *)t.hdr[1]);
	free(t.hdr);
	t.hdr = NULL;
	et_table_free(&t);
	/* 表头是借用的，et_table_free 不会释放它 */
	return rc;
}

/* ================= JSON ================= */

void et_json_init(et_json *j, et_buf *out)
{
	memset(j, 0, sizeof(*j));
	j->out = out;
}

static void j_indent(et_json *j, int depth)
{
	for (int i = 0; i < depth; i++)
		et_buf_add(j->out, "  ");
}

/* 输出 key（含分隔）并处理逗号/换行 */
static void j_pre(et_json *j, const char *key)
{
	if (j->depth < 24 && j->first[j->depth])
		et_buf_addc(j->out, ',');
	if (j->depth < 24)
		j->first[j->depth] = 1;
	/* 根节点前不换行（上游 serde_json::to_string_pretty 首字符就是 '{'/'['） */
	if (j->depth > 0) {
		et_buf_addc(j->out, '\n');
		j_indent(j, j->depth);
	}
	if (key) {
		et_buf_addc(j->out, '"');
		et_buf_add_json(j->out, key);
		et_buf_add(j->out, "\": ");
	}
}

static void j_close(et_json *j)
{
	if (j->depth < 24)
		j->first[j->depth] = 0;
	j->depth--;
}

void et_json_obj_begin(et_json *j, const char *key)
{
	j_pre(j, key);
	et_buf_addc(j->out, '{');
	j->depth++;
	if (j->depth < 24)
		j->first[j->depth] = 0;
}

void et_json_obj_end(et_json *j)
{
	int empty = j->depth < 24 && !j->first[j->depth];

	j_close(j);
	if (!empty) {
		et_buf_addc(j->out, '\n');
		j_indent(j, j->depth);
	}
	et_buf_addc(j->out, '}');
}

void et_json_arr_begin(et_json *j, const char *key)
{
	j_pre(j, key);
	et_buf_addc(j->out, '[');
	j->depth++;
	if (j->depth < 24)
		j->first[j->depth] = 0;
}

void et_json_arr_end(et_json *j)
{
	int empty = j->depth < 24 && !j->first[j->depth];

	j_close(j);
	if (!empty) {
		et_buf_addc(j->out, '\n');
		j_indent(j, j->depth);
	}
	et_buf_addc(j->out, ']');
}

void et_json_str(et_json *j, const char *key, const char *v)
{
	j_pre(j, key);
	et_buf_addc(j->out, '"');
	et_buf_add_json(j->out, v ? v : "");
	et_buf_addc(j->out, '"');
}

void et_json_int(et_json *j, const char *key, long long v)
{
	j_pre(j, key);
	et_buf_addf(j->out, "%lld", v);
}

void et_json_uint(et_json *j, const char *key, unsigned long long v)
{
	j_pre(j, key);
	et_buf_addf(j->out, "%llu", v);
}

void et_json_double(et_json *j, const char *key, double v)
{
	char buf[48];
	size_t n;

	/* serde_json：f64 一定带小数点（0 输出 0.0），其余用最短往返表示 */
	et_fmt_f64(v, buf, sizeof(buf));
	n = strlen(buf);
	if (!strpbrk(buf, ".eE") && n + 3 < sizeof(buf)) {
		buf[n++] = '.';
		buf[n++] = '0';
		buf[n] = 0;
	}
	j_pre(j, key);
	et_buf_add(j->out, buf);
}

void et_json_bool(et_json *j, const char *key, int v)
{
	j_pre(j, key);
	et_buf_add(j->out, v ? "true" : "false");
}

void et_json_raw(et_json *j, const char *key, const char *raw)
{
	j_pre(j, key);
	et_buf_add(j->out, raw ? raw : "null");
}

void et_json_null(et_json *j, const char *key)
{
	j_pre(j, key);
	et_buf_add(j->out, "null");
}

/* ================= RPC ================= */

int et_cli_ctx_open(struct et_cli_ctx *ctx)
{
	if (ctx->opened)
		return 0;
	if (et_rpc_open(&ctx->rpc, ctx->opts.rpc ? ctx->opts.rpc
						  : ET_CLI_RPC_DEFAULT)) {
		fprintf(stderr,
			"easytier-cli: failed to connect to RPC server %s\n",
			ctx->opts.rpc ? ctx->opts.rpc : ET_CLI_RPC_DEFAULT);
		return -1;
	}
	ctx->opened = 1;
	return 0;
}

void et_cli_ctx_close(struct et_cli_ctx *ctx)
{
	if (ctx->opened) {
		et_rpc_close(&ctx->rpc);
		ctx->opened = 0;
	}
}

/* 把实例标识符塞进请求 field 1：
 *   指定 -i  -> {1: {1: UUID}}
 *   指定 -n  -> {1: {2: {1: name}}}
 *   未指定    -> 不写 field 1（旧 core 走默认实例）
 */
int et_cli_req_instance(struct et_cli_ctx *ctx, et_pb *req)
{
	et_pb ident, sel;

	if (!ctx->target)
		return 0;
	et_pb_init(&ident);
	et_pb_init(&sel);
	if (ctx->target->have_uuid) {
		if (et_pb_bytes(&ident, 1, ctx->target->uuid, 24))
			goto err;
	} else {
		if (et_pb_str(&sel, 1, ctx->target->name) ||
		    et_pb_msg(&ident, 2, &sel))
			goto err;
	}
	if (et_pb_msg(req, 1, &ident))
		goto err;
	et_pb_free(&ident);
	et_pb_free(&sel);
	return 0;
err:
	et_pb_free(&ident);
	et_pb_free(&sel);
	return -1;
}

int et_cli_instance_message(struct et_cli_ctx *ctx, et_pb *out)
{
	et_pb ident, sel;

	et_pb_init(&ident);
	et_pb_init(&sel);
	if (!ctx->target) {
		et_pb_free(&ident);
		et_pb_free(&sel);
		return 1;
	}
	if (ctx->target->have_uuid) {
		if (et_pb_bytes(&ident, 1, ctx->target->uuid, 24))
			goto err;
	} else {
		if (et_pb_str(&sel, 1, ctx->target->name) ||
		    et_pb_msg(&ident, 2, &sel))
			goto err;
	}
	*out = ident;
	et_pb_free(&sel);
	return 0;
err:
	et_pb_free(&ident);
	et_pb_free(&sel);
	return -1;
}

int et_cli_patch_config(struct et_cli_ctx *ctx, const et_pb *patch)
{
	et_pb req, ident, resp;
	int rc = -1;

	et_pb_init(&req);
	et_pb_init(&ident);
	et_pb_init(&resp);
	if (et_pb_msg(&req, 1, patch))
		goto out;
	if (et_cli_instance_message(ctx, &ident) == 0) {
		if (et_pb_msg(&req, 2, &ident))
			goto out;
	}
	if (et_cli_call(ctx, "ConfigRpc", 1, &req, &resp))
		goto out;
	rc = 0;
out:
	et_pb_free(&req);
	et_pb_free(&ident);
	et_pb_free(&resp);
	return rc;
}

/* 静默模式：某些命令（proxy 的多域探测）按上游语义忽略 RPC 错误 */
int et_cli_quiet;

/* 把 RpcResponse.error（error.proto）翻成一句人话 */
static void et_cli_report_rpc_error(const char *service, unsigned method,
				    const uint8_t *err, size_t n)
{
	static const char *kinds[] = {
		"other_error", "invalid_method_index", "invalid_service",
		"prost_decode_error", "prost_encode_error", "execute_error",
		"malformat_rpc_packet", "timeout"
	};
	char detail[256] = "";
	unsigned kind = 0;

	{
		et_pb_rd r;
		unsigned f, w;
		uint64_t v;
		const uint8_t *b = NULL;
		size_t l = 0;

		et_pb_rd_init(&r, err, n);
		while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
			if (w != 2 || f < 1 || f > 8)
				continue;
			kind = f;
			(void)et_pb_get_str(b, l, 1, detail, sizeof(detail));
			if (!detail[0]) {
				/* invalid_method_index / invalid_service 的
				 * 第一个字段是服务名 */
				(void)et_pb_get_str(b, l, 1, detail,
						    sizeof(detail));
			}
			break;
		}
	}
	if (et_cli_quiet)
		return;
	fprintf(stderr, "easytier-cli: rpc %s/%u failed: %s%s%s\n", service,
		method, kind >= 1 && kind <= 8 ? kinds[kind - 1] : "error",
		detail[0] ? ": " : "", detail);
}

int et_cli_call_domain(struct et_cli_ctx *ctx, const char *domain,
		       const char *service, unsigned method, const et_pb *req,
		       et_pb *resp)
{
	et_pb wrapper, whole;

	et_pb_init(&wrapper);
	et_pb_init(&whole);
	if (et_cli_ctx_open(ctx))
		goto err;
	if (et_pb_bytes(&wrapper, 2, req ? req->p : NULL, req ? req->len : 0) ||
	    et_pb_uint(&wrapper, 3, ET_CLI_TIMEOUT_MS))
		goto err;
	if (et_rpc_call_domain(&ctx->rpc, domain, service, method, wrapper.p,
			       wrapper.len, &whole))
		goto err;
	{
		const uint8_t *body = NULL;
		size_t blen = 0;
		const uint8_t *err = NULL;
		size_t elen = 0;

		if (et_pb_find(whole.p, whole.len, 2, 2, NULL, &err, &elen)) {
			et_cli_report_rpc_error(service, method, err, elen);
			goto err;
		}
		if (et_pb_find(whole.p, whole.len, 1, 2, NULL, &body, &blen)) {
			et_pb_reset(resp);
			if (blen) {
				if (resp->cap < blen) {
					uint8_t *p = realloc(resp->p, blen);

					if (!p)
						goto err;
					resp->p = p;
					resp->cap = blen;
				}
				memcpy(resp->p, body, blen);
				resp->len = blen;
			}
		}
	}
	et_pb_free(&wrapper);
	et_pb_free(&whole);
	return 0;
err:
	et_pb_free(&wrapper);
	et_pb_free(&whole);
	return -1;
}

int et_cli_call(struct et_cli_ctx *ctx, const char *service, unsigned method,
		const et_pb *req, et_pb *resp)
{
	et_pb wrapper, whole;

	et_pb_init(&wrapper);
	et_pb_init(&whole);
	if (et_cli_ctx_open(ctx))
		goto err;
	if (et_pb_bytes(&wrapper, 2, req ? req->p : NULL, req ? req->len : 0) ||
	    et_pb_uint(&wrapper, 3, ET_CLI_TIMEOUT_MS))
		goto err;
	if (et_rpc_call(&ctx->rpc, service, method, wrapper.p, wrapper.len,
			&whole))
		goto err;
	/* 响应外层是 RpcResponse{response=1, error=2}：错误要显式报出来，
	 * 不能当成"空结果"（否则服务名/方法序号写错时表现为表格空行）。 */
	{
		const uint8_t *body = NULL;
		size_t blen = 0;
		const uint8_t *err = NULL;
		size_t elen = 0;

		if (et_pb_find(whole.p, whole.len, 2, 2, NULL, &err, &elen)) {
			et_cli_report_rpc_error(service, method, err, elen);
			goto err;
		}
		if (et_pb_find(whole.p, whole.len, 1, 2, NULL, &body, &blen)) {
			et_pb_reset(resp);
			if (blen) {
				if (resp->cap < blen) {
					uint8_t *p = realloc(resp->p, blen);

					if (!p)
						goto err;
					resp->p = p;
					resp->cap = blen;
				}
				memcpy(resp->p, body, blen);
				resp->len = blen;
			}
		}
	}
	et_pb_free(&wrapper);
	et_pb_free(&whole);
	return 0;
err:
	et_pb_free(&wrapper);
	et_pb_free(&whole);
	return -1;
}

int et_cli_call_instance(struct et_cli_ctx *ctx, const char *service,
			 unsigned method, et_pb *resp)
{
	et_pb req;
	int rc;

	et_pb_init(&req);
	if (et_cli_req_instance(ctx, &req)) {
		et_pb_free(&req);
		return -1;
	}
	rc = et_cli_call(ctx, service, method, &req, resp);
	et_pb_free(&req);
	return rc;
}

/* WebClientService/5 ListNetworkInstance -> 实例 uuid 列表 */
static int list_instance_uuids(struct et_cli_ctx *ctx, et_pb *resp)
{
	et_pb req;

	et_pb_init(&req);
	if (et_cli_call(ctx, "WebClientService", 5, &req, resp)) {
		et_pb_free(&req);
		return -1;
	}
	et_pb_free(&req);
	return 0;
}

/* WebClientService/8 ListNetworkInstanceMeta -> 实例名 */
static int lookup_instance_name(struct et_cli_ctx *ctx, const uint8_t *uuid,
				char *out, size_t cap)
{
	et_pb req, list, resp;
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *b = NULL;
	size_t n = 0;
	int rc = -1;

	out[0] = 0;
	et_pb_init(&req);
	et_pb_init(&list);
	et_pb_init(&resp);
	/* ListNetworkInstanceMetaRequest { repeated UUID inst_ids = 1 }：
	 * UUID 本身就是一条消息字节，直接作为 field 1 写入，别再套一层。 */
	(void)list;
	if (et_pb_bytes(&req, 1, uuid, 24))
		goto out;
	/* ListNetworkInstanceMetaRequest{ repeated UUID inst_ids = 1 } 是包装层，
	 * 实际调用还要带上 {2:…,3:…}，这里直接手工拼一层。 */
	if (et_cli_call(ctx, "WebClientService", 8, &req, &resp))
		goto out;
	/* response = {1: repeated NetworkMeta} */
	et_pb_rd_init(&r, resp.p, resp.len);
	while (et_pb_next(&r, &f, &w, &v, &b, &n) > 0) {
		char name[128] = "";
		char netname[128] = "";
		const uint8_t *id = NULL;
		size_t idlen = 0;

		if (f != 1 || w != 2)
			continue;
		if (!et_pb_find(b, n, 1, 2, NULL, &id, &idlen) || idlen != 24)
			continue;
		if (memcmp(id, uuid, 24))
			continue;
		et_pb_get_str(b, n, 4, name, sizeof(name));
		et_pb_get_str(b, n, 2, netname, sizeof(netname));
		snprintf(out, cap, "%s", name[0] ? name : netname);
		rc = 0;
		break;
	}
out:
	et_pb_free(&req);
	et_pb_free(&list);
	et_pb_free(&resp);
	return rc;
}

void et_fmt_uuid(const uint8_t *msg, size_t n, char *out, size_t cap)
{
	uint64_t p[4] = { 0, 0, 0, 0 };

	if (!out || cap == 0)
		return;
	out[0] = 0;
	for (unsigned i = 0; i < 4; i++)
		(void)et_pb_get_u64(msg, n, i + 1, &p[i]);
	snprintf(out, cap, "%08x-%04x-%04x-%04x-%012llx",
		 (unsigned)p[0], (unsigned)(p[1] >> 16), (unsigned)(p[1] & 0xffff),
		 (unsigned)(p[2] >> 16),
		 ((unsigned long long)(p[2] & 0xffff) << 32) | p[3]);
}

int et_parse_uuid(const char *s, uint8_t out[24])
{
	uint64_t hi[2] = { 0, 0 };
	int nib = 0;

	if (!s)
		return -1;
	for (const char *p = s; *p; p++) {
		int v;

		if (*p == '-')
			continue;
		if (*p >= '0' && *p <= '9')
			v = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			v = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			v = *p - 'A' + 10;
		else
			return -1;
		if (nib >= 32)
			return -1;
		if (nib < 16)
			hi[0] = (hi[0] << 4) | (uint64_t)v;
		else
			hi[1] = (hi[1] << 4) | (uint64_t)v;
		nib++;
	}
	if (nib != 32)
		return -1;
	{
		uint32_t part[4] = { (uint32_t)(hi[0] >> 32), (uint32_t)hi[0],
				     (uint32_t)(hi[1] >> 32), (uint32_t)hi[1] };
		et_pb b;

		et_pb_init(&b);
		for (unsigned i = 0; i < 4; i++)
			(void)et_pb_uint(&b, i + 1, part[i]);
		if (b.len == 24)
			memcpy(out, b.p, 24);
		et_pb_free(&b);
	}
	return 0;
}

void et_cli_free_targets(struct et_cli_target *t, int n)
{
	free(t);
	(void)n;
}

int et_cli_resolve_targets(struct et_cli_ctx *ctx,
			   struct et_cli_target **out)
{
	et_pb resp;
	struct et_cli_target *arr = NULL;
	int n = 0, cap = 0;

	*out = NULL;
	/* -i / -n：单目标，不做扇出 */
	if (ctx->opts.instance_id || ctx->opts.instance_name) {
		arr = calloc(1, sizeof(*arr));
		if (!arr)
			return -1;
		if (ctx->opts.instance_id) {
			if (et_parse_uuid(ctx->opts.instance_id, arr[0].uuid))
				goto fail;
			arr[0].have_uuid = 1;
			snprintf(arr[0].id_str, sizeof(arr[0].id_str), "%s",
				 ctx->opts.instance_id);
		}
		if (ctx->opts.instance_name)
			snprintf(arr[0].name, sizeof(arr[0].name), "%s",
				 ctx->opts.instance_name);
		*out = arr;
		return 1;
	}

	if (et_cli_ctx_open(ctx))
		return -1;
	et_pb_init(&resp);
	if (list_instance_uuids(ctx, &resp)) {
		/* 老 core 没有 WebClientService：不退化成"无目标"，
		 * 直接按单实例、不指定 selector 的旧路径走 */
		et_pb_free(&resp);
		arr = calloc(1, sizeof(*arr));
		if (!arr)
			return -1;
		*out = arr;
		return 1;
	}
	{
		et_pb_rd r;
		unsigned f, w;
		uint64_t v;
		const uint8_t *b = NULL;
		size_t n2 = 0;

		et_pb_rd_init(&r, resp.p, resp.len);
		while (et_pb_next(&r, &f, &w, &v, &b, &n2) > 0) {
			if (f != 1 || w != 2 || n2 != 24)
				continue;
			if (n >= cap) {
				int nc = cap ? cap * 2 : 4;
				struct et_cli_target *p =
					realloc(arr, (size_t)nc * sizeof(*p));

				if (!p)
					goto fail;
				arr = p;
				cap = nc;
			}
			memset(&arr[n], 0, sizeof(arr[n]));
			memcpy(arr[n].uuid, b, 24);
			arr[n].have_uuid = 1;
			{
				et_pb_rd r2;
				unsigned f2, w2;
				uint64_t v2;

				et_pb_rd_init(&r2, b, 24);
				while (et_pb_next(&r2, &f2, &w2, &v2, NULL,
						  NULL) > 0)
					;
			}
			et_fmt_uuid(b, 24, arr[n].id_str,
				    sizeof(arr[n].id_str));
			(void)lookup_instance_name(ctx, b, arr[n].name,
						   sizeof(arr[n].name));
			n++;
		}
	}
	et_pb_free(&resp);
	if (n == 0) {
		free(arr);
		fprintf(stderr, "no running instances found\n");
		return -1;
	}
	/* 按 label 排序（上游 targets.sort_by_key(label)） */
	for (int i = 1; i < n; i++) {
		struct et_cli_target key = arr[i];
		int j = i - 1;

		while (j >= 0 && strcmp(arr[j].id_str, key.id_str) > 0) {
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = key;
	}
	*out = arr;
	return n;
fail:
	free(arr);
	et_pb_free(&resp);
	return -1;
}

void et_cli_print_target_header(const struct et_cli_target *t)
{
	if (!t)
		return;
	if (t->name[0] && t->id_str[0])
		printf("== %s (%s) ==\n", t->name, t->id_str);
	else if (t->name[0])
		printf("== %s ==\n", t->name);
	else if (t->id_str[0])
		printf("== %s ==\n", t->id_str);
	else
		printf("== selected instance ==\n");
}

/* ================= protobuf 便捷读取 ================= */

int et_pb_get_str(const uint8_t *d, size_t n, unsigned field, char *out,
		  size_t cap)
{
	const uint8_t *b = NULL;
	size_t len = 0;

	if (!out || cap == 0)
		return -1;
	out[0] = 0;
	if (!et_pb_find(d, n, field, 2, NULL, &b, &len))
		return -1;
	if (len >= cap)
		len = cap - 1;
	if (len)
		memcpy(out, b, len);
	out[len] = 0;
	return (int)len;
}

int et_pb_get_u64(const uint8_t *d, size_t n, unsigned field, uint64_t *out)
{
	uint64_t v = 0;

	if (!et_pb_find(d, n, field, 0, &v, NULL, NULL))
		return -1;
	if (out)
		*out = v;
	return 0;
}

int et_pb_foreach_msg(const uint8_t *d, size_t n, unsigned field,
		      et_pb_msg_cb cb, void *ud)
{
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *b = NULL;
	size_t l = 0;
	int rc = 0;

	et_pb_rd_init(&r, d, n);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
		if (f != field || w != 2)
			continue;
		rc = cb(b, l, ud);
		if (rc)
			break;
	}
	return rc;
}

struct nth_str_ctx {
	int want;
	int idx;
	char *out;
	size_t cap;
	int got;
};

static int nth_str_cb(const char *s, size_t n, void *ud)
{
	struct nth_str_ctx *c = ud;

	(void)n;
	if (c->idx++ != c->want)
		return 0;
	snprintf(c->out, c->cap, "%s", s);
	c->got = 1;
	return 1;
}

int et_pb_get_str_n(const uint8_t *d, size_t n, unsigned field, int idx,
		    char *out, size_t cap)
{
	struct nth_str_ctx c;

	c.want = idx;
	c.idx = 0;
	c.out = out;
	c.cap = cap;
	c.got = 0;
	out[0] = 0;
	(void)et_pb_foreach_str(d, n, field, nth_str_cb, &c);
	return c.got ? (int)strlen(out) : -1;
}

int et_pb_foreach_str(const uint8_t *d, size_t n, unsigned field,
		      et_pb_str_cb cb, void *ud)
{
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *b = NULL;
	size_t l = 0;
	int rc = 0;

	et_pb_rd_init(&r, d, n);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
		char tmp[512];
		size_t c;

		if (f != field || w != 2)
			continue;
		c = l < sizeof(tmp) - 1 ? l : sizeof(tmp) - 1;
		memcpy(tmp, b, c);
		tmp[c] = 0;
		rc = cb(tmp, c, ud);
		if (rc)
			break;
	}
	return rc;
}

/* base64（标准字母表 + '=' 填充）：trusted_keys.pubkey 等二进制字段展示用 */
void et_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;

	if (!out || cap == 0)
		return;
	out[0] = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		int rem = (int)(n - i);

		if (rem > 1)
			v |= (uint32_t)in[i + 1] << 8;
		if (rem > 2)
			v |= in[i + 2];
		if (o + 4 >= cap)
			break;
		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = rem > 1 ? tbl[(v >> 6) & 0x3f] : '=';
		out[o++] = rem > 2 ? tbl[v & 0x3f] : '=';
	}
	out[o] = 0;
}

/* ================= 格式化 ================= */

void et_fmt_size(unsigned long long bytes, char *out, size_t cap)
{
	static const char *units[] = { "B", "kB", "MB", "GB", "TB", "PB", "EB" };
	double v = (double)bytes;
	int u = 0;
	char buf[64];

	while (v >= 1000.0 && u < 6) {
		v /= 1000.0;
		u++;
	}
	if (u == 0) {
		snprintf(out, cap, "%llu B", bytes);
		return;
	}
	/* humansize::DECIMAL 固定两位小数（实测上游输出 8.50 kB / 5.51 kB） */
	snprintf(buf, sizeof(buf), "%.2f", v);
	snprintf(out, cap, "%s %s", buf, units[u]);
}

const char *et_nat_name(unsigned long long v)
{
	static const char *names[] = { "Unknown",        "OpenInternet",
				       "NoPat",          "FullCone",
				       "Restricted",     "PortRestricted",
				       "Symmetric",      "SymUdpFirewall",
				       "SymmetricEasyInc", "SymmetricEasyDec" };

	if (v < sizeof(names) / sizeof(names[0]))
		return names[v];
	return "Unknown";
}

void et_fmt_f64(double v, char *out, size_t cap)
{
	char buf[64];
	char *dot;

	snprintf(buf, sizeof(buf), "%.3f", v);
	dot = strchr(buf, '.');
	if (dot) {
		char *end = buf + strlen(buf) - 1;

		while (end > dot && *end == '0')
			*end-- = 0;
		if (end == dot)
			*end = 0;
	}
	snprintf(out, cap, "%s", buf);
}

void et_fmt_ipv4(uint32_t addr, char *out, size_t cap)
{
	snprintf(out, cap, "%u.%u.%u.%u", (addr >> 24) & 0xff,
		 (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff);
}

void et_fmt_ipv4inet(const uint8_t *msg, size_t n, char *out, size_t cap)
{
	const uint8_t *a = NULL;
	size_t alen = 0;
	uint64_t addr = 0, len = 0;

	out[0] = 0;
	if (!msg || !n)
		return;
	/* 上游 Ipv4Inet 是 Option：field 1 缺失时输出空串 */
	if (!et_pb_find(msg, n, 1, 2, NULL, &a, &alen))
		return;
	(void)et_pb_get_u64(a, alen, 1, &addr);
	(void)et_pb_get_u64(msg, n, 2, &len);
	{
		char ip[24];

		et_fmt_ipv4((uint32_t)addr, ip, sizeof(ip));
		snprintf(out, cap, "%s/%llu", ip, (unsigned long long)len);
	}
}

void et_fmt_socket_addr(const uint8_t *msg, size_t n, char *out, size_t cap)
{
	const uint8_t *v4 = NULL, *v6 = NULL;
	size_t l4 = 0, l6 = 0;
	uint64_t port = 0;

	out[0] = 0;
	if (!msg || !n)
		return;
	(void)et_pb_get_u64(msg, n, 3, &port);
	if (et_pb_find(msg, n, 1, 2, NULL, &v4, &l4)) {
		uint64_t addr = 0;
		char ip[24];

		(void)et_pb_get_u64(v4, l4, 1, &addr);
		et_fmt_ipv4((uint32_t)addr, ip, sizeof(ip));
		snprintf(out, cap, "%s:%llu", ip, (unsigned long long)port);
		return;
	}
	if (et_pb_find(msg, n, 2, 2, NULL, &v6, &l6)) {
		uint64_t p[4] = { 0, 0, 0, 0 };

		for (unsigned i = 0; i < 4; i++)
			(void)et_pb_get_u64(v6, l6, i + 1, &p[i]);
		snprintf(out, cap, "[%x:%x:%x:%x:%x:%x:%x:%x]:%llu",
			 (unsigned)(p[0] >> 16), (unsigned)(p[0] & 0xffff),
			 (unsigned)(p[1] >> 16), (unsigned)(p[1] & 0xffff),
			 (unsigned)(p[2] >> 16), (unsigned)(p[2] & 0xffff),
			 (unsigned)(p[3] >> 16), (unsigned)(p[3] & 0xffff),
			 (unsigned long long)port);
	}
}

void et_fmt_url(const uint8_t *msg, size_t n, char *out, size_t cap)
{
	out[0] = 0;
	if (!msg || !n)
		return;
	(void)et_pb_get_str(msg, n, 1, out, cap);
}

/* ================= 分发 ================= */

/* 子命令级 --help：上游由 clap 生成，这里给出等价的用法/子命令/选项三段 */
static void sub_help(const char *cmd, et_buf *out)
{
	struct {
		const char *cmd;
		const char *subs;
	} tab[] = {
		{ "peer", "list             List connected peers\n  ipv6             Show public IPv6 address information\n  list-foreign     List foreign networks discovered by this instance\n  list-global-foreign  List global foreign networks from the peer center" },
		{ "route", "list             List routes\n  dump             Dump routing table info" },
		{ "node", "info             Show node info\n  config           Show node config" },
		{ "connector", "list             List connectors\n  add <URL>        Add a connector\n  remove <URL>     Remove a connector\n  clear            Clear all connectors" },
		{ "mapped-listener", "list             List mapped listeners\n  add <URL>        Add a mapped listener\n  remove <URL>     Remove a mapped listener\n  clear            Clear all mapped listeners" },
		{ "vpn-portal", "show             Show vpn portal info\n  add-client NAME --virtual-ip <CIDR> [--groups <G>]\n  remove-client NAME\n  clear-clients" },
		{ "proxy", "list             List tcp proxy entries\n  add <SRC> <DST>  Add a tcp proxy entry\n  remove <SRC> <DST>  Remove a tcp proxy entry\n  clear            Clear all tcp proxy entries" },
		{ "acl", "stats            Show ACL statistics\n  set <ACL>        Patch ACL config (TOML)" },
		{ "port-forward", "list             List port forward rules\n  add <BIND> <DST> Add a port forward rule\n  remove <BIND> <DST>  Remove a port forward rule\n  clear            Clear all port forward rules" },
		{ "whitelist", "show             Show port whitelist\n  set <PORTS> --tcp Add TCP ports to whitelist\n  set <PORTS> --udp Add UDP ports to whitelist\n  clear            Clear whitelist" },
		{ "stats", "show             Show stats\n  prometheus       Show prometheus metrics" },
		{ "logger", "get              Get current log level\n  set <LEVEL>      Set log level (disabled/error/warning/info/debug/trace)" },
		{ "credential", "list             List credentials\n  generate [OPTIONS]  Generate a credential\n  revoke <ID>      Revoke a credential" },
		{ "gen-autocomplete", "bash | zsh | fish | powershell | elvish | nu" },
		{ NULL, NULL }
	};
	const char *extra = NULL;

	if (!strcmp(cmd, "stun") || !strcmp(cmd, "peer-center"))
		extra = "  (no subcommands)";
	et_buf_addf(out, "Usage: easytier-cli %s [OPTIONS]", cmd);
	if (extra)
		et_buf_add(out, "\n");
	else
		et_buf_add(out, " [COMMAND]\n");
	for (int i = 0; tab[i].cmd; i++)
		if (!strcmp(tab[i].cmd, cmd)) {
			et_buf_add(out, "\nCommands:\n  ");
			et_buf_add(out, tab[i].subs);
			et_buf_add(out, "\n");
			break;
		}
	if (extra)
		et_buf_addf(out, "\n%s\n", extra);
	et_buf_add(out,
		   "\nOptions:\n  -h, --help  Print help\n\nGlobal options (before the command):\n"
		   "  -p, --rpc-portal <ADDR>  -o, --output <table|json>  --no-trunc\n"
		   "  -i, --instance-id <UUID>  -n, --instance-name <NAME>  -v, --verbose\n");
}

int et_cli_dispatch(struct et_cli_ctx *ctx, const char *cmd, int argc,
		    char **argv)
{
	if (!cmd)
		return ET_ENOENT;
	if (argc > 0 && (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help"))) {
		et_buf b;

		et_buf_init(&b);
		sub_help(cmd, &b);
		et_buf_add(&ctx->out, b.p ? b.p : "");
		et_buf_free(&b);
		return ET_OK;
	}
	if (!strcmp(cmd, "peer"))
		return et_cli_peer(ctx, argc, argv);
	if (!strcmp(cmd, "route"))
		return et_cli_route(ctx, argc, argv);
	if (!strcmp(cmd, "node"))
		return et_cli_node(ctx, argc, argv);
	if (!strcmp(cmd, "connector"))
		return et_cli_connector(ctx, argc, argv);
	if (!strcmp(cmd, "mapped-listener"))
		return et_cli_mapped_listener(ctx, argc, argv);
	if (!strcmp(cmd, "stun"))
		return et_cli_stun(ctx, argc, argv);
	if (!strcmp(cmd, "peer-center"))
		return et_cli_peer_center(ctx, argc, argv);
	if (!strcmp(cmd, "vpn-portal"))
		return et_cli_vpn_portal(ctx, argc, argv);
	if (!strcmp(cmd, "proxy"))
		return et_cli_proxy(ctx, argc, argv);
	if (!strcmp(cmd, "acl"))
		return et_cli_acl(ctx, argc, argv);
	if (!strcmp(cmd, "port-forward"))
		return et_cli_port_forward(ctx, argc, argv);
	if (!strcmp(cmd, "whitelist"))
		return et_cli_whitelist(ctx, argc, argv);
	if (!strcmp(cmd, "stats"))
		return et_cli_stats(ctx, argc, argv);
	if (!strcmp(cmd, "logger"))
		return et_cli_logger(ctx, argc, argv);
	if (!strcmp(cmd, "credential"))
		return et_cli_credential(ctx, argc, argv);
	if (!strcmp(cmd, "gen-autocomplete") || !strcmp(cmd, "generate-completions"))
		return et_cli_gen_autocomplete(ctx, argc, argv);
	return ET_ENOENT;
}

/* ================= 多实例扇出 ================= */

int et_cli_fanout_begin(struct et_cli_ctx *ctx, struct et_cli_fanout *f)
{
	memset(f, 0, sizeof(*f));
	f->nt = et_cli_resolve_targets(ctx, &f->tg);
	if (f->nt < 0)
		return -1;
	f->json = et_cli_want_json(&ctx->opts);
	if (f->json) {
		et_json_init(&f->j, &ctx->out);
		if (f->nt > 1)
			et_json_arr_begin(&f->j, NULL);
	}
	return 0;
}

void et_cli_fanout_header(struct et_cli_ctx *ctx, struct et_cli_fanout *f)
{
	char label[192];
	const struct et_cli_target *t;

	if (f->json || f->nt <= 1)
		return;
	if (f->index)
		et_buf_addc(&ctx->out, '\n');
	t = &f->tg[f->index];
	if (t->name[0] && t->id_str[0])
		snprintf(label, sizeof(label), "%s (%s)", t->name, t->id_str);
	else if (t->name[0])
		snprintf(label, sizeof(label), "%s", t->name);
	else if (t->id_str[0])
		snprintf(label, sizeof(label), "%s", t->id_str);
	else
		snprintf(label, sizeof(label), "selected instance");
	et_cli_printf(ctx, "== %s ==\n", label);
}

void et_cli_fanout_json_open(struct et_cli_fanout *f)
{
	if (!f->json || f->nt <= 1)
		return;
	et_json_obj_begin(&f->j, NULL);
	et_json_str(&f->j, "instance_id", f->tg[f->index].id_str);
	et_json_str(&f->j, "instance_name", f->tg[f->index].name);
}

void et_cli_fanout_json_close(struct et_cli_fanout *f)
{
	if (!f->json || f->nt <= 1)
		return;
	et_json_obj_end(&f->j);
}

void et_cli_fanout_end(struct et_cli_ctx *ctx, struct et_cli_fanout *f)
{
	if (f->json && f->nt > 1)
		et_json_arr_end(&f->j);
	if (ctx->out.len && ctx->out.p && ctx->out.p[ctx->out.len - 1] != '\n')
		et_buf_addc(&ctx->out, '\n');
	et_cli_free_targets(f->tg, f->nt);
	f->tg = NULL;
	ctx->target = NULL;
}

/* ================= 状态页分区渲染 =================
 * etd_api_conninfo.c 在 easytierd 进程内直接调用，不再 fork easytier-cli。 */

int et_cli_render_part(const char *part, et_buf *out)
{
	return et_cli_render_part_ex(part, NULL, out);
}

int et_cli_render_part_ex(const char *part, const char *rpc_addr, et_buf *out)
{
	struct et_cli_ctx ctx;
	struct et_cli_opts o;
	const char *cmd = NULL;
	const char *sub = NULL;
	char *argv[2];
	int argc = 0;
	int rc;

	if (!part || !out)
		return ET_EINVAL;
	if (!strcmp(part, "node"))
		cmd = "node";
	else if (!strcmp(part, "peer"))
		cmd = "peer";
	else if (!strcmp(part, "route"))
		cmd = "route";
	else if (!strcmp(part, "connector"))
		cmd = "connector";
	else if (!strcmp(part, "stun"))
		cmd = "stun";
	else if (!strcmp(part, "peer_center"))
		cmd = "peer-center";
	else if (!strcmp(part, "vpn_portal"))
		cmd = "vpn-portal";
	else if (!strcmp(part, "proxy"))
		cmd = "proxy";
	else if (!strcmp(part, "mapped_listener"))
		cmd = "mapped-listener";
	else if (!strcmp(part, "stats"))
		cmd = "stats";
	else if (!strcmp(part, "acl")) {
		cmd = "acl";
		sub = "stats";
	} else {
		return ET_ENOENT;
	}

	memset(&ctx, 0, sizeof(ctx));
	et_cli_opts_init(&o);
	o.no_trunc = 1; /* 服务端渲染：不做终端宽度裁剪 */
	if (rpc_addr && *rpc_addr)
		o.rpc = rpc_addr; /* 跟随 UCI 的 rpc_portal 端口 */
	ctx.opts = o;
	et_buf_init(&ctx.out);
	if (sub)
		argv[argc++] = (char *)sub;
	rc = et_cli_dispatch(&ctx, cmd, argc, argv);
	et_cli_ctx_close(&ctx);
	if (rc) {
		et_buf_free(&ctx.out);
		return rc;
	}
	*out = ctx.out;
	return 0;
}