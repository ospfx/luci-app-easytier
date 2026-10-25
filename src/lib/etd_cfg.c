/*
 * etd_cfg.c - 配置模型：按 schema 从 UCI 装载/写回
 *
 * 装载规则：
 *   - 单值项：UCI 有值用 UCI 的值，没有则用 schema 默认值，两者都没有则为 NULL
 *   - list 项：逐元素读取（保留元素内部空格），不做任何再切分
 *   - extra_args 单独存于 et_cfg.extra_args（同时可通过 et_cfg_list() 取到）
 *   - easytierbin / webbin 额外解析到 et_cfg.easytierbin / webbin（带默认值兜底）
 */
#include "etd_cfg.h"

#include <stdlib.h>
#include <string.h>

#define ET_PKG "easytier"
#define ET_SEC "@easytier[0]"

/* extra_args 的 schema 项（数据实体在 et_cfg.extra_args） */
static bool is_extra_args(const char *name)
{
	return name && strcmp(name, "extra_args") == 0;
}

static et_cfg_item *find_item(et_cfg *c, const char *name)
{
	if (!c || !name)
		return NULL;
	for (size_t i = 0; i < c->n; i++) {
		if (c->items[i].f && c->items[i].f->name &&
		    strcmp(c->items[i].f->name, name) == 0)
			return &c->items[i];
	}
	return NULL;
}

/* 取 UCI list 并搬到 et_list（v 里是 malloc 串，需逐个搬走后释放外层数组） */
static void load_uci_list(et_uci *u, const char *name, et_list *dst)
{
	size_t n = 0;
	char **v;

	if (!u)
		return;
	v = et_uci_get_et_list(u, name, &n);
	if (!v)
		return;
	for (size_t i = 0; i < n; i++)
		et_list_add_owned(dst, v[i]);
	free(v);
}

et_cfg *et_cfg_load(et_uci *u)
{
	et_cfg *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	et_list_init(&c->extra_args);
	c->n = et_schema_len;
	c->items = calloc(c->n ? c->n : 1, sizeof(et_cfg_item));
	if (!c->items) {
		et_list_free(&c->extra_args);
		free(c);
		return NULL;
	}

	for (size_t i = 0; i < c->n; i++) {
		const et_field *f = &et_schema[i];
		et_cfg_item *it = &c->items[i];

		it->f = f;
		et_list_init(&it->list);

		if (is_extra_args(f->name)) {
			load_uci_list(u, f->name, &c->extra_args);
			continue;
		}
		if (f->kind == ET_K_LIST) {
			load_uci_list(u, f->name, &it->list);
			continue;
		}

		char *v = u ? et_uci_get_et(u, f->name) : NULL;
		if (v)
			it->val = v;
		else if (f->def)
			it->val = strdup(f->def);
	}

	/* 二进制路径（easytierbin/webbin 已在 items 里，这里再解析成最终路径） */
	if (et_resolve_bins(u, &c->easytierbin, &c->webbin, NULL) != ET_OK) {
		et_cfg_free(c);
		return NULL;
	}
	return c;
}

void et_cfg_free(et_cfg *c)
{
	if (!c)
		return;
	if (c->items) {
		for (size_t i = 0; i < c->n; i++) {
			free(c->items[i].val);
			et_list_free(&c->items[i].list);
		}
		free(c->items);
	}
	et_list_free(&c->extra_args);
	free(c->easytierbin);
	free(c->webbin);
	free(c);
}

const char *et_cfg_str(et_cfg *c, const char *name)
{
	et_cfg_item *it = find_item(c, name);

	if (!it || is_extra_args(name))
		return NULL;
	return it->val;
}

int et_cfg_int(et_cfg *c, const char *name, int def)
{
	const char *s = et_cfg_str(c, name);
	long v;

	if (!s || !et_parse_int(s, &v))
		return def;
	return (int)v;
}

bool et_cfg_is01(et_cfg *c, const char *name)
{
	const char *s = et_cfg_str(c, name);

	return s && strcmp(s, "1") == 0;
}

const et_list *et_cfg_list(et_cfg *c, const char *name)
{
	et_cfg_item *it;

	if (!c || !name)
		return NULL;
	if (is_extra_args(name))
		return &c->extra_args;
	it = find_item(c, name);
	if (!it || it->f->kind != ET_K_LIST)
		return NULL;
	return &it->list;
}

static bool uci_list_equal(const et_list *mem, char **cur, size_t n)
{
	if (mem->n != n)
		return false;
	if (n == 0)
		return true;
	if (!cur)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (!mem->v[i] || !cur[i] || strcmp(mem->v[i], cur[i]) != 0)
			return false;
	}
	return true;
}

static void free_uci_list(char **v, size_t n)
{
	if (!v)
		return;
	for (size_t i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

int et_cfg_save(et_cfg *c, et_uci *u, bool *changed)
{
	bool ch = false;

	if (changed)
		*changed = false;
	if (!c || !u)
		return ET_EINVAL;

	for (size_t i = 0; i < c->n; i++) {
		et_cfg_item *it = &c->items[i];
		const et_field *f = it->f;

		if (!f || is_extra_args(f->name))
			continue;

		if (f->kind == ET_K_LIST) {
			size_t n = 0;
			char **cur = et_uci_get_et_list(u, f->name, &n);
			bool same = uci_list_equal(&it->list, cur, n);

			free_uci_list(cur, n);
			if (same)
				continue;
			if (et_uci_set_list(u, ET_PKG, ET_SEC, f->name,
					    (const char *const *)it->list.v,
					    it->list.n) != ET_OK)
				return ET_ERR;
			ch = true;
			continue;
		}

		char *cur = et_uci_get_et(u, f->name);
		bool mem_set = it->val && *it->val;
		bool same;

		if (!cur) {
			/* UCI 里没有这一项：
			 *   - 内存值也为空          → 无差异
			 *   - 内存值恰好等于 schema 默认值 → 视为无差异，**不把默认值物化进 UCI**
			 *     （否则第一次保存就会把 60 多个默认值写进 /etc/config/easytier）
			 *   - 其它                  → 需要写入 */
			if (!mem_set)
				same = true;
			else if (f->def && strcmp(it->val, f->def) == 0)
				same = true;
			else
				same = false;
		} else {
			same = mem_set && strcmp(cur, it->val) == 0;
		}
		free(cur);
		if (same)
			continue;

		if (mem_set) {
			if (et_uci_set_et(u, f->name, it->val) != ET_OK)
				return ET_ERR;
		} else if (et_uci_del_et(u, f->name) != ET_OK) {
			return ET_ERR;
		}
		ch = true;
	}

	/* extra_args 独立保存 */
	{
		size_t n = 0;
		char **cur = et_uci_get_et_list(u, "extra_args", &n);
		bool same = uci_list_equal(&c->extra_args, cur, n);

		free_uci_list(cur, n);
		if (!same) {
			if (et_uci_set_list(u, ET_PKG, ET_SEC, "extra_args",
					    (const char *const *)c->extra_args.v,
					    c->extra_args.n) != ET_OK)
				return ET_ERR;
			ch = true;
		}
	}

	if (ch && et_uci_commit_et(u) != ET_OK)
		return ET_ERR;
	if (changed)
		*changed = ch;
	return ET_OK;
}
