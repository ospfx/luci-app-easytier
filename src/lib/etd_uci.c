/*
 * etd_uci.c - libuci 封装实现
 *
 * 语义严格对齐 `uci -q get easytier.@easytier[0].<opt>`：
 *   - 选项缺失返回 NULL（无任何输出，也不报错）
 *   - list 类型：单值接口把各元素用单个空格连接（与 `uci -q get` 一致），
 *     list 接口逐元素返回并**保留元素内部的空格**（不按空格二次切分）
 *
 * 写操作返回码与 uci 命令行一致：成功 ET_OK，选项/节不存在 ET_ENOENT，
 * 其它失败 ET_ERR；是否落盘由 et_uci_commit() 决定。
 *
 * 仅测试用：环境变量 ET_UCI_CONFDIR 可覆盖 libuci 的配置目录，
 * 让 golden 测试指向样例文件。生产环境不设置该变量，行为与默认完全一致。
 */
#include "etd_uci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>

struct et_uci {
	struct uci_context *ctx;
};

/* 拼 tuple 串（uci_lookup_ptr 会就地修改并在 ptr 内持有指针，故必须保活到用完） */
static int uci_tuple(char **out, const char *pkg, const char *sec, const char *opt)
{
	int n;

	*out = NULL;
	if (opt && *opt)
		n = asprintf(out, "%s.%s.%s", pkg, sec, opt);
	else
		n = asprintf(out, "%s.%s", pkg, sec);
	if (n < 0) {
		*out = NULL;
		return ET_ENOMEM;
	}
	return ET_OK;
}

/* 读路径：要求 UCI_LOOKUP_COMPLETE，否则视为"没有这一项" */
static int uci_read_ptr(et_uci *u, struct uci_ptr *ptr, char **tuple,
			const char *pkg, const char *sec, const char *opt)
{
	int rc = uci_tuple(tuple, pkg, sec, opt);

	if (rc != ET_OK)
		return rc;
	memset(ptr, 0, sizeof(*ptr));
	if (uci_lookup_ptr(u->ctx, ptr, *tuple, true) != UCI_OK)
		return ET_ENOENT;
	if (!(ptr->flags & UCI_LOOKUP_COMPLETE))
		return ET_ENOENT;
	return ET_OK;
}

/* 写路径：只要 package 与 section 存在即可（option 允许尚不存在） */
/* 宽松查找：只要 package 存在就返回 OK，section/option 允许不存在。
 * uci_lookup_ptr 对"section 不存在"返回 UCI_OK 且 ptr.s == NULL（不报错），
 * 对"package 不存在"才返回错误 —— 这里据此区分。 */
static int uci_loose_ptr(et_uci *u, struct uci_ptr *ptr, char **tuple,
			 const char *pkg, const char *sec, const char *opt)
{
	int rc = uci_tuple(tuple, pkg, sec, opt);

	if (rc != ET_OK)
		return rc;
	memset(ptr, 0, sizeof(*ptr));
	if (uci_lookup_ptr(u->ctx, ptr, *tuple, true) != UCI_OK || !ptr->p)
		return ET_ENOENT;
	return ET_OK;
}

static int uci_write_ptr(et_uci *u, struct uci_ptr *ptr, char **tuple,
			 const char *pkg, const char *sec, const char *opt)
{
	int rc = uci_loose_ptr(u, ptr, tuple, pkg, sec, opt);

	if (rc != ET_OK)
		return rc;
	/* 写 option 必须先有 section（旧实现的 uci set pkg.@type[0].x=y 也是这个前提） */
	if (!ptr->s)
		return ET_ENOENT;
	return ET_OK;
}

et_uci *et_uci_open(void)
{
	const char *dir = getenv("ET_UCI_CONFDIR");
	struct uci_context *ctx;
	char path[512];
	et_uci *u;

	if (!dir || !*dir)
		dir = "/etc/config";

	ctx = uci_alloc_context();
	if (!ctx)
		return NULL;
	if (getenv("ET_UCI_CONFDIR") && uci_set_confdir(ctx, dir) != UCI_OK) {
		uci_free_context(ctx);
		return NULL;
	}

	/* 配置文件不存在时返回 NULL，与原实现的 access() 前置判断一致 */
	snprintf(path, sizeof(path), "%s/easytier", dir);
	if (!et_file_exists(path)) {
		uci_free_context(ctx);
		return NULL;
	}

	u = calloc(1, sizeof(*u));
	if (!u) {
		uci_free_context(ctx);
		return NULL;
	}
	u->ctx = ctx;
	return u;
}

void et_uci_close(et_uci *u)
{
	if (!u)
		return;
	if (u->ctx)
		uci_free_context(u->ctx);
	free(u);
}

char *et_uci_get(et_uci *u, const char *pkg, const char *sec, const char *opt)
{
	struct uci_ptr ptr;
	char *tuple = NULL;
	char *out = NULL;

	if (!u || !pkg || !sec || !opt)
		return NULL;
	if (uci_read_ptr(u, &ptr, &tuple, pkg, sec, opt) != ET_OK || !ptr.o)
		goto done;

	if (ptr.o->type == UCI_TYPE_STRING) {
		out = strdup(ptr.o->v.string ? ptr.o->v.string : "");
	} else {
		struct uci_element *e;
		et_buf b;

		et_buf_init(&b);
		uci_foreach_element(&ptr.o->v.list, e) {
			if (b.len)
				et_buf_addc(&b, ' ');
			et_buf_add(&b, e->name);
		}
		if (!b.p)
			et_buf_add(&b, "");
		out = et_buf_steal(&b);
	}

done:
	free(tuple);
	return out;
}

char **et_uci_get_list(et_uci *u, const char *pkg, const char *sec,
		       const char *opt, size_t *n)
{
	struct uci_ptr ptr;
	char *tuple = NULL;
	et_list l;

	et_list_init(&l);
	if (n)
		*n = 0;
	if (!u || !pkg || !sec || !opt)
		return NULL;
	if (uci_read_ptr(u, &ptr, &tuple, pkg, sec, opt) != ET_OK || !ptr.o)
		goto done;

	if (ptr.o->type == UCI_TYPE_STRING) {
		if (ptr.o->v.string)
			et_list_add(&l, ptr.o->v.string);
	} else {
		struct uci_element *e;

		uci_foreach_element(&ptr.o->v.list, e)
			et_list_add(&l, e->name);
	}

done:
	free(tuple);
	if (n)
		*n = l.n;
	return l.v;
}

char *et_uci_get_et(et_uci *u, const char *opt)
{
	return et_uci_get(u, "easytier", "@easytier[0]", opt);
}

char **et_uci_get_et_list(et_uci *u, const char *opt, size_t *n)
{
	return et_uci_get_list(u, "easytier", "@easytier[0]", opt, n);
}

int et_uci_set(et_uci *u, const char *pkg, const char *sec, const char *opt,
	       const char *val)
{
	struct uci_ptr ptr;
	char *tuple = NULL;
	int rc;

	if (!u || !pkg || !sec || !opt)
		return ET_EINVAL;
	rc = uci_write_ptr(u, &ptr, &tuple, pkg, sec, opt);
	if (rc == ET_OK) {
		ptr.value = val ? val : "";
		rc = uci_set(u->ctx, &ptr) == UCI_OK ? ET_OK : ET_ERR;
	}
	free(tuple);
	return rc;
}

int et_uci_set_list(et_uci *u, const char *pkg, const char *sec,
		    const char *opt, const char *const *vals, size_t n)
{
	int rc;

	/* 先清空（不存在也不算错），再逐项追加，语义同
	 * `uci -q delete p.s.o` + 多次 `uci add_list p.s.o=...` */
	et_uci_delete(u, pkg, sec, opt);
	for (size_t i = 0; i < n; i++) {
		struct uci_ptr ptr;
		char *tuple = NULL;

		if (!vals[i])
			continue;
		rc = uci_write_ptr(u, &ptr, &tuple, pkg, sec, opt);
		if (rc == ET_OK) {
			ptr.value = vals[i];
			rc = uci_add_list(u->ctx, &ptr) == UCI_OK ? ET_OK : ET_ERR;
		}
		free(tuple);
		if (rc != ET_OK)
			return rc;
	}
	return ET_OK;
}

int et_uci_delete(et_uci *u, const char *pkg, const char *sec, const char *opt)
{
	struct uci_ptr ptr;
	char *tuple = NULL;
	int rc;

	if (!u || !pkg || !sec || !opt)
		return ET_EINVAL;
	rc = uci_read_ptr(u, &ptr, &tuple, pkg, sec, opt);
	if (rc == ET_OK)
		rc = uci_delete(u->ctx, &ptr) == UCI_OK ? ET_OK : ET_ERR;
	free(tuple);
	return rc;
}

int et_uci_delete_sec(et_uci *u, const char *pkg, const char *sec)
{
	struct uci_ptr ptr;
	char *tuple = NULL;
	int rc;

	if (!u || !pkg || !sec)
		return ET_EINVAL;
	rc = uci_read_ptr(u, &ptr, &tuple, pkg, sec, NULL);
	if (rc == ET_OK)
		rc = uci_delete(u->ctx, &ptr) == UCI_OK ? ET_OK : ET_ERR;
	free(tuple);
	return rc;
}

/* 取已加载（或按需加载）的 package 对象。
 * 注意不能改用 uci_load()：它对已经被 uci_lookup_ptr 加载过的 package 会返回
 * 不一致的结果（实测 uci_load 返回错误码、且拿到的 package 上看不到本次改动），
 * 这也是 uci 命令行用 uci_lookup_ptr 定位 package 的原因。 */
static struct uci_package *et_uci_pkg(et_uci *u, const char *pkg)
{
	struct uci_ptr ptr;
	struct uci_package *p = NULL;
	char *tuple = NULL;

	if (asprintf(&tuple, "%s", pkg) < 0)
		return NULL;
	memset(&ptr, 0, sizeof(ptr));
	if (uci_lookup_ptr(u->ctx, &ptr, tuple, true) == UCI_OK)
		p = ptr.p;
	free(tuple);
	return p;
}

/* 是否有待提交改动。
 * 不能用 package->has_delta：实测该标志在 load 时就为真、commit 后也不会清零；
 * 真正准确的信号是 delta 队列是否为空（set/delete 入队，commit 出队）。 */
static bool pkg_has_pending(struct uci_package *p)
{
	return p && p->delta.next != &p->delta;
}

int et_uci_commit(et_uci *u, const char *pkg)
{
	struct uci_package *p;

	if (!u || !pkg)
		return ET_EINVAL;
	p = et_uci_pkg(u, pkg);
	if (!p)
		return ET_ENOENT;
	if (!pkg_has_pending(p))
		return ET_OK;
	if (uci_commit(u->ctx, &p, false) != UCI_OK)
		return ET_ERR;
	return ET_OK;
}

bool et_uci_changed(et_uci *u, const char *pkg)
{
	if (!u || !pkg)
		return false;
	return pkg_has_pending(et_uci_pkg(u, pkg));
}

int et_uci_set_et(et_uci *u, const char *opt, const char *val)
{
	return et_uci_set(u, "easytier", "@easytier[0]", opt, val);
}

int et_uci_del_et(et_uci *u, const char *opt)
{
	return et_uci_delete(u, "easytier", "@easytier[0]", opt);
}

int et_uci_commit_et(et_uci *u)
{
	return et_uci_commit(u, "easytier");
}

int et_uci_ensure_sec(et_uci *u, const char *pkg, const char *sec,
		      const char *type)
{
	struct uci_ptr ptr;
	char *tuple = NULL;
	int rc;

	if (!u || !pkg || !sec || !type)
		return ET_EINVAL;

	/* 语义对齐 `uci set pkg.sec=type`：
	 *   - section 不存在：uci_set 用 value 作为类型创建（ucli 里就是这条路径）
	 *   - section 已存在且类型相同：什么都不做（不产生 delta）
	 *   - section 已存在但类型不同：uci_set 会改类型，并保留原有 option
	 * 注意 uci_lookup_ptr 对"section 不存在"返回 UCI_OK + ptr.s == NULL，
	 * 所以这里不能沿用 uci_read_ptr（它要求 COMPLETE）。 */
	rc = uci_loose_ptr(u, &ptr, &tuple, pkg, sec, NULL);
	if (rc != ET_OK) {
		free(tuple);
		return rc;
	}
	if (ptr.s && ptr.s->type && strcmp(ptr.s->type, type) == 0) {
		free(tuple);
		return ET_OK;
	}

	ptr.value = type;
	rc = uci_set(u->ctx, &ptr) == UCI_OK ? ET_OK : ET_ERR;
	free(tuple);
	return rc;
}

char *et_uci_read_raw(void)
{
	return et_read_file(ET_FILE_UCI, NULL);
}
