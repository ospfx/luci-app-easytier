/*
 * etd_cli_acl_toml.c - easytier-cli acl set：ACL TOML 子集解析 + acl.Acl 编码
 *
 * 上游（easytier-cli 2.7.0）的 acl set 接受：
 *   1) 整段 ACL TOML 字符串；
 *   2) @path/to/file.toml 从文件读；
 *   3) JSON（完整 PatchConfigRequest / {acl:...} / Acl 本体）——当前 C 版只支持前两种。
 *
 * TOML 形状与 config.toml 的 [acl] 段一致（serde 直接反序列化成 acl.Acl）：
 *
 *   [acl.acl_v1]
 *   [[acl.acl_v1.chains]]
 *   name = "Inbound"
 *   chain_type = 1
 *   enabled = true
 *   default_action = 2
 *   [[acl.acl_v1.chains.rules]]
 *   protocol = 1
 *   ports = ["80", "443"]
 *   action = 1
 *   [acl.acl_v1.group]
 *   members = ["a", "b"]
 *   [[acl.acl_v1.group.declares]]
 *   group_name = "a"
 *   group_secret = "xxx"
 *
 * 枚举在上游是 prost 的 i32 字段（serde 收整数），这里额外允许枚举名，属超集。
 *
 * 语义要点（与上游逐条对齐）：
 *   - 整份 ACL 是替换不是合并；
 *   - 空 ACL（没有 chain 也没有 group）直接报错；
 *   - 多实例逐个下发，全失败/部分失败都要报错并以非 0 退出；
 *   - JSON 输出里 group_secret 会打码成 "<redacted>"。
 */
#include "etd_cli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 动态字符串数组 ---------------- */

struct sv {
	char **v;
	int n;
	int cap;
};

static void sv_free(struct sv *s)
{
	for (int i = 0; i < s->n; i++)
		free(s->v[i]);
	free(s->v);
	s->v = NULL;
	s->n = 0;
	s->cap = 0;
}

static void sv_push(struct sv *s, const char *x)
{
	char **p;

	if (!x)
		return;
	if (s->n >= s->cap) {
		int c = s->cap ? s->cap * 2 : 4;

		p = realloc(s->v, (size_t)c * sizeof(*p));
		if (!p)
			return;
		s->v = p;
		s->cap = c;
	}
	s->v[s->n++] = strdup(x);
}

/* ---------------- ACL 中间结构 ---------------- */

struct acl_rule {
	char name[128];
	char desc[256];
	int have_priority, priority;
	int have_enabled, enabled;
	int have_protocol, protocol;
	struct sv ports, src_ips, dst_ips, src_ports, src_groups, dst_groups;
	int have_action, action;
	int have_rate, rate;
	int have_burst, burst;
	int have_stateful, stateful;
};

struct acl_chain {
	char name[128];
	char desc[256];
	int have_type, type;
	int have_enabled, enabled;
	int have_default, default_action;
	struct acl_rule *rules;
	int nrules;
	int caprules;
};

struct acl_decl {
	char name[128];
	char secret[256];
};

struct acl_doc {
	struct acl_chain *chains;
	int nchains;
	int capchains;
	struct acl_decl *decls;
	int ndecls;
	int capdecls;
	struct sv members;
	int have_group;
};

static void doc_free(struct acl_doc *d)
{
	for (int i = 0; i < d->nchains; i++) {
		struct acl_chain *c = &d->chains[i];

		for (int j = 0; j < c->nrules; j++) {
			sv_free(&c->rules[j].ports);
			sv_free(&c->rules[j].src_ips);
			sv_free(&c->rules[j].dst_ips);
			sv_free(&c->rules[j].src_ports);
			sv_free(&c->rules[j].src_groups);
			sv_free(&c->rules[j].dst_groups);
		}
		free(c->rules);
	}
	free(d->chains);
	free(d->decls);
	sv_free(&d->members);
	memset(d, 0, sizeof(*d));
}

static struct acl_chain *doc_new_chain(struct acl_doc *d)
{
	struct acl_chain *c;

	if (d->nchains >= d->capchains) {
		int cap = d->capchains ? d->capchains * 2 : 4;
		struct acl_chain *p =
			realloc(d->chains, (size_t)cap * sizeof(*p));

		if (!p)
			return NULL;
		d->chains = p;
		d->capchains = cap;
	}
	c = &d->chains[d->nchains++];
	memset(c, 0, sizeof(*c));
	return c;
}

static struct acl_rule *chain_new_rule(struct acl_chain *c)
{
	if (c->nrules >= c->caprules) {
		int cap = c->caprules ? c->caprules * 2 : 4;
		struct acl_rule *p = realloc(c->rules, (size_t)cap * sizeof(*p));

		if (!p)
			return NULL;
		c->rules = p;
		c->caprules = cap;
	}
	memset(&c->rules[c->nrules], 0, sizeof(*c->rules));
	return &c->rules[c->nrules++];
}

static struct acl_decl *doc_new_decl(struct acl_doc *d)
{
	if (d->ndecls >= d->capdecls) {
		int cap = d->capdecls ? d->capdecls * 2 : 4;
		struct acl_decl *p = realloc(d->decls, (size_t)cap * sizeof(*p));

		if (!p)
			return NULL;
		d->decls = p;
		d->capdecls = cap;
	}
	memset(&d->decls[d->ndecls], 0, sizeof(*d->decls));
	return &d->decls[d->ndecls++];
}

/* ---------------- 词法小工具 ---------------- */

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
			 e[-1] == '\n'))
		*--e = 0;
	return s;
}

/* 去掉引号外的注释（#） */
static void strip_comment(char *s)
{
	int in_str = 0;
	char q = 0;

	for (char *p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && q == '"') {
				p++;
				continue;
			}
			if (*p == q)
				in_str = 0;
			continue;
		}
		if (*p == '"' || *p == '\'') {
			in_str = 1;
			q = *p;
			continue;
		}
		if (*p == '#') {
			*p = 0;
			return;
		}
	}
}

/* 括号（[]）在引号外是否配平，用于多行数组续行 */
static int brackets_balanced(const char *s)
{
	int depth = 0, in_str = 0;
	char q = 0;

	for (const char *p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && q == '"') {
				p++;
				continue;
			}
			if (*p == q)
				in_str = 0;
			continue;
		}
		if (*p == '"' || *p == '\'') {
			in_str = 1;
			q = *p;
			continue;
		}
		if (*p == '[')
			depth++;
		else if (*p == ']')
			depth--;
	}
	return depth == 0 && !in_str;
}

/* 解析 TOML 字符串（"..." 或 '...'），返回消耗字节数；失败 -1 */
static int parse_toml_string(const char *p, char *out, size_t cap)
{
	const char *start = p;
	char q;
	size_t o = 0;

	p = skip_ws(p);
	q = *p;
	if (q != '"' && q != '\'')
		return -1;
	p++;
	while (*p && *p != q) {
		char c = *p++;

		if (q == '"' && c == '\\' && *p) {
			switch (*p) {
			case 'n':
				c = '\n';
				p++;
				break;
			case 't':
				c = '\t';
				p++;
				break;
			case 'r':
				c = '\r';
				p++;
				break;
			case '"':
				c = '"';
				p++;
				break;
			case '\\':
				c = '\\';
				p++;
				break;
			default:
				break;
			}
		}
		if (o + 1 < cap)
			out[o++] = c;
	}
	if (*p != q)
		return -1;
	if (out)
		out[o] = 0;
	p++;
	return (int)(p - start);
}

/* 读取一个字符串值；*used 为消耗长度 */
static int read_string_value(const char *p, char *out, size_t cap, int *used)
{
	int n = parse_toml_string(p, out, cap);

	if (n < 0)
		return -1;
	if (used)
		*used = n;
	return 0;
}

/* 枚举：数字优先，其次枚举名 */
static int enum_value(const char *p, const char *const *names, int n, int *out)
{
	const char *s = skip_ws(p);

	if (*s == '"' || *s == '\'') {
		char tmp[64];

		if (parse_toml_string(s, tmp, sizeof(tmp)) < 0)
			return -1;
		for (int i = 0; i < n; i++)
			if (!strcmp(tmp, names[i])) {
				*out = i;
				return 0;
			}
		return -1;
	}
	{
		int have_num = 0;
		long long v = 0;

		while (*s == '-' || (*s >= '0' && *s <= '9')) {
			have_num = 1;
			v = v * 10 + (*s - '0');
			s++;
		}
		if (!have_num)
			return -1;
		*out = (int)v;
	}
	return 0;
}

/* 数组元素：字符串或裸值（数字/布尔） */
static int parse_array_into(const char *p, struct sv *out, char *err,
			    size_t errcap)
{
	const char *s = skip_ws(p);

	if (*s != '[')
		return -1;
	s = skip_ws(s + 1);
	while (*s && *s != ']') {
		char buf[512];

		if (*s == '"' || *s == '\'') {
			int used = 0;

			if (read_string_value(s, buf, sizeof(buf), &used))
				goto bad;
			sv_push(out, buf);
			s = skip_ws(s + used);
		} else {
			const char *start = s;

			while (*s && *s != ',' && *s != ']')
				s++;
			{
				char tmp[256];
				size_t n = (size_t)(s - start);
				char *t;

				if (n >= sizeof(tmp))
					n = sizeof(tmp) - 1;
				memcpy(tmp, start, n);
				tmp[n] = 0;
				t = trim(tmp);
				if (*t)
					sv_push(out, t);
			}
			s = skip_ws(s);
		}
		if (*s == ',') {
			s = skip_ws(s + 1);
			continue;
		}
		if (*s != ']' && *s)
			goto bad;
	}
	if (*s != ']')
		goto bad;
	return 0;
bad:
	snprintf(err, errcap, "无法解析数组：%.60s", p);
	return -1;
}

/* ---------------- TOML 解析主流程 ---------------- */

enum ctx_kind {
	CTX_NONE = 0,
	CTX_CHAIN,
	CTX_RULE,
	CTX_GROUP,
	CTX_DECL
};

static void set_num(int *have, int *dst, int v)
{
	*have = 1;
	*dst = v;
}

static int parse_acl_toml(const char *text, struct acl_doc *d, char *err,
			  size_t errcap)
{
	char *copy = strdup(text ? text : "");
	char *save = NULL, *raw;
	int ctx = CTX_NONE;
	struct acl_chain *cc = NULL;
	struct acl_rule *cr = NULL;
	struct acl_decl *cd = NULL;
	char stmt[16384];

	if (!copy) {
		snprintf(err, errcap, "内存不足");
		return -1;
	}
	for (raw = strtok_r(copy, "\n", &save); raw;
	     raw = strtok_r(NULL, "\n", &save)) {
		char *s;

		snprintf(stmt, sizeof(stmt), "%s", raw);
		strip_comment(stmt);
		s = trim(stmt);
		if (!*s)
			continue;
		/* 多行数组：把后续行接上直到括号配平 */
		while (s[0] != '[' && !brackets_balanced(s)) {
			char *more = strtok_r(NULL, "\n", &save);
			size_t l;

			if (!more)
				break;
			strip_comment(more);
			l = strlen(stmt);
			if (l + strlen(more) + 2 >= sizeof(stmt))
				break;
			stmt[l] = ' ';
			stmt[l + 1] = 0;
			strncat(stmt, more, sizeof(stmt) - l - 2);
			s = trim(stmt);
		}
		if (s[0] == '[') {
			char path[256];
			size_t n;
			const char *p = s + 1;
			int is_array = 0;
			char *pp;

			if (*p == '[') {
				is_array = 1;
				p++;
			}
			n = strcspn(p, "]");
			if (n >= sizeof(path))
				n = sizeof(path) - 1;
			memcpy(path, p, n);
			path[n] = 0;
			pp = trim(path);
			if (!strcmp(pp, "acl.acl_v1.chains") && is_array) {
				cc = doc_new_chain(d);
				cr = NULL;
				ctx = CTX_CHAIN;
			} else if (!strcmp(pp, "acl.acl_v1.chains.rules") &&
				   is_array && cc) {
				cr = chain_new_rule(cc);
				ctx = CTX_RULE;
			} else if (!strcmp(pp, "acl.acl_v1.group") &&
				   !is_array) {
				d->have_group = 1;
				ctx = CTX_GROUP;
			} else if (!strcmp(pp,
					   "acl.acl_v1.group.declares") &&
				   is_array) {
				cd = doc_new_decl(d);
				d->have_group = 1;
				ctx = CTX_DECL;
			} else {
				ctx = CTX_NONE;
			}
			continue;
		}
		{
			char *eq = strchr(s, '=');
			char *key, *val;

			if (!eq)
				continue;
			*eq = 0;
			key = trim(s);
			val = trim(eq + 1);
			if (ctx == CTX_CHAIN && cc) {
				if (!strcmp(key, "name"))
					(void)read_string_value(
						val, cc->name, sizeof(cc->name),
						NULL);
				else if (!strcmp(key, "description"))
					(void)read_string_value(
						val, cc->desc, sizeof(cc->desc),
						NULL);
				else if (!strcmp(key, "chain_type")) {
					static const char *names[] = {
						"UnspecifiedChain", "Inbound",
						"Outbound", "Forward"
					};
					int v = 0;

					if (!enum_value(val, names, 4, &v))
						set_num(&cc->have_type,
							&cc->type, v);
					else
						snprintf(err, errcap,
							 "chain_type 无法识别：%.40s",
							 val);
				} else if (!strcmp(key, "enabled")) {
					cc->have_enabled = 1;
					cc->enabled = !strncmp(val, "true", 4);
				} else if (!strcmp(key, "default_action")) {
					static const char *names[] = {
						"Noop", "Allow", "Drop"
					};
					int v = 0;

					if (!enum_value(val, names, 3, &v))
						set_num(&cc->have_default,
							&cc->default_action,
							v);
					else
						snprintf(err, errcap,
							 "default_action 无法识别：%.40s",
							 val);
				}
			} else if (ctx == CTX_RULE && cr) {
				if (!strcmp(key, "name"))
					(void)read_string_value(
						val, cr->name, sizeof(cr->name),
						NULL);
				else if (!strcmp(key, "description"))
					(void)read_string_value(
						val, cr->desc, sizeof(cr->desc),
						NULL);
				else if (!strcmp(key, "priority")) {
					cr->have_priority = 1;
					cr->priority =
						(int)strtol(val, NULL, 10);
				} else if (!strcmp(key, "enabled")) {
					cr->have_enabled = 1;
					cr->enabled = !strncmp(val, "true", 4);
				} else if (!strcmp(key, "protocol")) {
					static const char *names[] = {
						"Unspecified", "TCP", "UDP",
						"ICMP", "ICMPv6", "Any"
					};
					int v = 0;

					if (!enum_value(val, names, 6, &v))
						set_num(&cr->have_protocol,
							&cr->protocol, v);
				} else if (!strcmp(key, "action")) {
					static const char *names[] = {
						"Noop", "Allow", "Drop"
					};
					int v = 0;

					if (!enum_value(val, names, 3, &v))
						set_num(&cr->have_action,
							&cr->action, v);
				} else if (!strcmp(key, "ports")) {
					if (parse_array_into(val, &cr->ports,
							     err, errcap))
						goto out;
				} else if (!strcmp(key, "source_ips")) {
					if (parse_array_into(val, &cr->src_ips,
							     err, errcap))
						goto out;
				} else if (!strcmp(key,
						   "destination_ips")) {
					if (parse_array_into(val, &cr->dst_ips,
							     err, errcap))
						goto out;
				} else if (!strcmp(key, "source_ports")) {
					if (parse_array_into(
						    val, &cr->src_ports, err,
						    errcap))
						goto out;
				} else if (!strcmp(key, "source_groups")) {
					if (parse_array_into(
						    val, &cr->src_groups, err,
						    errcap))
						goto out;
				} else if (!strcmp(key,
						   "destination_groups")) {
					if (parse_array_into(
						    val, &cr->dst_groups, err,
						    errcap))
						goto out;
				} else if (!strcmp(key, "rate_limit")) {
					cr->have_rate = 1;
					cr->rate =
						(int)strtol(val, NULL, 10);
				} else if (!strcmp(key, "burst_limit")) {
					cr->have_burst = 1;
					cr->burst =
						(int)strtol(val, NULL, 10);
				} else if (!strcmp(key, "stateful")) {
					cr->have_stateful = 1;
					cr->stateful =
						!strncmp(val, "true", 4);
				}
			} else if (ctx == CTX_GROUP) {
				if (!strcmp(key, "members") &&
				    parse_array_into(val, &d->members, err,
						     errcap))
					goto out;
			} else if (ctx == CTX_DECL && cd) {
				if (!strcmp(key, "group_name"))
					(void)read_string_value(
						val, cd->name,
						sizeof(cd->name), NULL);
				else if (!strcmp(key, "group_secret"))
					(void)read_string_value(
						val, cd->secret,
						sizeof(cd->secret), NULL);
			}
		}
	}
	free(copy);
	if (err[0])
		return -1;
	return 0;
out:
	free(copy);
	return -1;
}

/* ---------------- 编码成 acl.Acl ---------------- */

static void emit_rule(et_pb *chain, const struct acl_rule *r)
{
	et_pb pb;

	et_pb_init(&pb);
	if (r->name[0])
		(void)et_pb_str(&pb, 1, r->name);
	if (r->desc[0])
		(void)et_pb_str(&pb, 2, r->desc);
	if (r->have_priority)
		(void)et_pb_uint(&pb, 3, (uint64_t)r->priority);
	if (r->have_enabled)
		(void)et_pb_uint(&pb, 4, (uint64_t)r->enabled);
	if (r->have_protocol)
		(void)et_pb_uint(&pb, 5, (uint64_t)r->protocol);
	for (int i = 0; i < r->ports.n; i++)
		(void)et_pb_str(&pb, 6, r->ports.v[i]);
	for (int i = 0; i < r->src_ips.n; i++)
		(void)et_pb_str(&pb, 7, r->src_ips.v[i]);
	for (int i = 0; i < r->dst_ips.n; i++)
		(void)et_pb_str(&pb, 8, r->dst_ips.v[i]);
	for (int i = 0; i < r->src_ports.n; i++)
		(void)et_pb_str(&pb, 9, r->src_ports.v[i]);
	if (r->have_action)
		(void)et_pb_uint(&pb, 10, (uint64_t)r->action);
	if (r->have_rate)
		(void)et_pb_uint(&pb, 11, (uint64_t)r->rate);
	if (r->have_burst)
		(void)et_pb_uint(&pb, 12, (uint64_t)r->burst);
	if (r->have_stateful)
		(void)et_pb_uint(&pb, 13, (uint64_t)r->stateful);
	for (int i = 0; i < r->src_groups.n; i++)
		(void)et_pb_str(&pb, 14, r->src_groups.v[i]);
	for (int i = 0; i < r->dst_groups.n; i++)
		(void)et_pb_str(&pb, 15, r->dst_groups.v[i]);
	(void)et_pb_msg(chain, 5, &pb);
	et_pb_free(&pb);
}

static void build_acl(const struct acl_doc *d, et_pb *acl)
{
	et_pb v1;

	et_pb_init(&v1);
	for (int i = 0; i < d->nchains; i++) {
		const struct acl_chain *c = &d->chains[i];
		et_pb ch;

		et_pb_init(&ch);
		if (c->name[0])
			(void)et_pb_str(&ch, 1, c->name);
		if (c->have_type)
			(void)et_pb_uint(&ch, 2, (uint64_t)c->type);
		if (c->desc[0])
			(void)et_pb_str(&ch, 3, c->desc);
		if (c->have_enabled)
			(void)et_pb_uint(&ch, 4, (uint64_t)c->enabled);
		for (int j = 0; j < c->nrules; j++)
			emit_rule(&ch, &c->rules[j]);
		if (c->have_default)
			(void)et_pb_uint(&ch, 6, (uint64_t)c->default_action);
		(void)et_pb_msg(&v1, 1, &ch);
		et_pb_free(&ch);
	}
	if (d->have_group) {
		et_pb g;

		et_pb_init(&g);
		for (int i = 0; i < d->ndecls; i++) {
			et_pb gi;

			et_pb_init(&gi);
			if (d->decls[i].name[0])
				(void)et_pb_str(&gi, 1, d->decls[i].name);
			if (d->decls[i].secret[0])
				(void)et_pb_str(&gi, 2,
						d->decls[i].secret);
			(void)et_pb_msg(&g, 1, &gi);
			et_pb_free(&gi);
		}
		for (int i = 0; i < d->members.n; i++)
			(void)et_pb_str(&g, 2, d->members.v[i]);
		(void)et_pb_msg(&v1, 2, &g);
		et_pb_free(&g);
	}
	(void)et_pb_msg(acl, 2, &v1);
	et_pb_free(&v1);
}

/* ---------------- JSON 回显 ---------------- */

static void json_sv(et_json *j, const char *key, const struct sv *s)
{
	et_json_arr_begin(j, key);
	for (int i = 0; i < s->n; i++)
		et_json_str(j, NULL, s->v[i]);
	et_json_arr_end(j);
}

/* Acl 对象体：{ "acl_v1": {...} }，调用方负责先 obj_begin */
static void json_acl_emit(et_json *j, const struct acl_doc *d)
{
	et_json_obj_begin(j, "acl_v1");
	et_json_arr_begin(j, "chains");
	for (int i = 0; i < d->nchains; i++) {
		const struct acl_chain *c = &d->chains[i];

		et_json_obj_begin(j, NULL);
		et_json_str(j, "name", c->name);
		et_json_uint(j, "chain_type",
			     c->have_type ? (uint64_t)c->type : 0);
		et_json_str(j, "description", c->desc);
		et_json_bool(j, "enabled",
			     c->have_enabled ? c->enabled : 0);
		et_json_arr_begin(j, "rules");
		for (int k = 0; k < c->nrules; k++) {
			const struct acl_rule *r = &c->rules[k];

			et_json_obj_begin(j, NULL);
			et_json_str(j, "name", r->name);
			et_json_str(j, "description", r->desc);
			et_json_uint(j, "priority",
				     r->have_priority ? (uint64_t)r->priority
						      : 0);
			et_json_bool(j, "enabled",
				     r->have_enabled ? r->enabled : 0);
			et_json_uint(j, "protocol",
				     r->have_protocol ? (uint64_t)r->protocol
						      : 0);
			json_sv(j, "ports", &r->ports);
			json_sv(j, "source_ips", &r->src_ips);
			json_sv(j, "destination_ips", &r->dst_ips);
			json_sv(j, "source_ports", &r->src_ports);
			et_json_uint(j, "action",
				     r->have_action ? (uint64_t)r->action
						    : 0);
			et_json_uint(j, "rate_limit",
				     r->have_rate ? (uint64_t)r->rate : 0);
			et_json_uint(j, "burst_limit",
				     r->have_burst ? (uint64_t)r->burst : 0);
			et_json_bool(j, "stateful",
				     r->have_stateful ? r->stateful : 0);
			json_sv(j, "source_groups", &r->src_groups);
			json_sv(j, "destination_groups", &r->dst_groups);
			et_json_obj_end(j);
		}
		et_json_arr_end(j);
		et_json_uint(j, "default_action",
			     c->have_default ? (uint64_t)c->default_action
					     : 0);
		et_json_obj_end(j);
	}
	et_json_arr_end(j);
	if (!d->have_group) {
		et_json_null(j, "group");
	} else {
		et_json_obj_begin(j, "group");
		et_json_arr_begin(j, "declares");
		for (int i = 0; i < d->ndecls; i++) {
			et_json_obj_begin(j, NULL);
			et_json_str(j, "group_name", d->decls[i].name);
			et_json_str(j, "group_secret",
				    d->decls[i].secret[0] ? "<redacted>" : "");
			et_json_obj_end(j);
		}
		et_json_arr_end(j);
		json_sv(j, "members", &d->members);
		et_json_obj_end(j);
	}
	et_json_obj_end(j);
}

/* ---------------- 入口 ---------------- */

static void target_label(const struct et_cli_target *t, char *out, size_t cap)
{
	int have_name = t && t->name[0];
	int have_id = t && t->id_str[0];

	if (have_name && have_id)
		snprintf(out, cap, "%s (%s)", t->name, t->id_str);
	else if (have_name)
		snprintf(out, cap, "%s", t->name);
	else if (have_id)
		snprintf(out, cap, "%s", t->id_str);
	else
		snprintf(out, cap, "selected instance");
}

static char *read_whole_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *buf;
	long n;

	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	n = ftell(fp);
	if (n < 0 || n > 4 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	buf = malloc((size_t)n + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
		free(buf);
		fclose(fp);
		return NULL;
	}
	buf[n] = 0;
	fclose(fp);
	return buf;
}

int et_cli_acl_set_apply(struct et_cli_ctx *ctx, const char *raw)
{
	struct acl_doc doc;
	char *text = NULL;
	char err[256] = "";
	et_pb acl, aclpatch, ipatch;
	struct et_cli_fanout f;
	int ok = 0, failed = 0;
	int rc = 1;

	memset(&doc, 0, sizeof(doc));
	et_pb_init(&acl);
	et_pb_init(&aclpatch);
	et_pb_init(&ipatch);

	if (!raw || !*raw) {
		fprintf(stderr,
			"error: the following required arguments were not provided:\n  <ACL>\n");
		return 2;
	}
	if (raw[0] == '@') {
		text = read_whole_file(raw + 1);
		if (!text) {
			fprintf(stderr,
				"easytier-cli: acl set: failed to read ACL file: %s\n",
				raw + 1);
			return 1;
		}
	} else {
		text = strdup(raw);
		if (!text)
			return 1;
	}
	if (parse_acl_toml(text, &doc, err, sizeof(err))) {
		fprintf(stderr, "easytier-cli: acl set: %s\n",
			err[0] ? err : "ACL TOML 解析失败");
		goto out;
	}
	if (!doc.nchains && !doc.have_group) {
		fprintf(stderr,
			"easytier-cli: parsed ACL is empty; provide at least one chain or a non-empty group under [acl.acl_v1]\n");
		goto out;
	}
	build_acl(&doc, &acl);
	(void)et_pb_msg(&aclpatch, 1, &acl);
	(void)et_pb_msg(&ipatch, 5, &aclpatch);

	if (et_cli_fanout_begin(ctx, &f))
		goto out;
	for (f.index = 0; f.index < f.nt; f.index++) {
		char label[256];

		ctx->target = &f.tg[f.index];
		target_label(ctx->target, label, sizeof(label));
		if (et_cli_patch_config(ctx, &ipatch)) {
			failed++;
			if (f.json) {
				et_json j;

				et_json_init(&j, &ctx->out);
				et_json_obj_begin(&j, NULL);
				et_json_str(&j, "instance", label);
				et_json_bool(&j, "success", 0);
				et_json_str(&j, "error",
					    et_cli_last_error()[0]
						    ? et_cli_last_error()
						    : "patch config failed");
				et_json_obj_end(&j);
				et_cli_puts(ctx, "\n");
			} else {
				const char *detail = et_cli_last_error();

				et_cli_printf(ctx, "== %s ==\n", label);
				et_cli_printf(ctx,
					      "ACL update failed: Rust error: %s\n",
					      detail[0] ? detail
							: "rpc error");
			}
			continue;
		}
		ok++;
		if (f.json) {
			et_json j;

			et_json_init(&j, &ctx->out);
			et_json_obj_begin(&j, NULL);
			et_json_str(&j, "instance", label);
			et_json_bool(&j, "success", 1);
			et_json_uint(&j, "chains", (uint64_t)doc.nchains);
			et_json_obj_begin(&j, "acl");
			json_acl_emit(&j, &doc);
			et_json_obj_end(&j);
			et_json_obj_end(&j);
			et_cli_puts(ctx, "\n");
		} else {
			et_cli_printf(ctx, "== %s ==\n", label);
			et_cli_printf(ctx,
				      "ACL updated successfully (%d chain(s))\n",
				      doc.nchains);
		}
	}
	et_cli_fanout_end(ctx, &f);
	if (ok == 0) {
		fprintf(stderr,
			"easytier-cli: ACL update failed on all %d selected instance(s)\n",
			failed);
		goto out;
	}
	if (failed) {
		fprintf(stderr,
			"easytier-cli: ACL update failed on %d of %d selected instance(s)\n",
			failed, ok + failed);
		goto out;
	}
	rc = 0;
out:
	et_pb_free(&acl);
	et_pb_free(&aclpatch);
	et_pb_free(&ipatch);
	doc_free(&doc);
	free(text);
	return rc;
}
