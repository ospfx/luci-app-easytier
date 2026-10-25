/*
 * tests/argv_test.c - golden 测试：UCI 样例 -> argv/toml/notes 与期望文件逐行比对
 *
 * 用法： argv_test <golden 目录>
 *
 * 约定：对 golden/ 下每个 <name>.uci
 *   <name>.core.args  期望的 easytier-core 完整 argv（每行一个参数，含 argv[0]）
 *   <name>.web.args   期望的 easytier-web 完整 argv
 *   <name>.notes      argv 生成过程中必须出现的告警关键字（每行一个，子串匹配）
 *   <name>.toml       期望的 config.toml 渲染结果（逐行比对）
 * 期望文件均由 init.d 的 shell 逻辑手工推导，推导依据写在文件头注释里。
 *
 * 期望文件里的 `<uuid>` 是占位符：匹配任意 36 字符 UUID。--machine-id 与 toml 的
 * instance_id 都是运行时生成的随机值，写死会让测试不稳定（见 match_expected）。
 *
 * 机器 ID 路径可用 -DET_FILE_MACHINE_ID="..." 指到临时文件（tests/run_host.sh 就这么做）；
 * 不指定时走生产路径 /etc/easytier/et_machine_id，落盘失败也会用生成的 UUID（见 etd_argv.c）。
 */
#include "etd_cfg.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ET_FILE_MACHINE_ID
#define ET_FILE_MACHINE_ID "/etc/easytier/et_machine_id"
#endif

static int g_fail;
static int g_pass;

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("FAIL: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
	g_fail++;
}

/* ---------------- 文件工具 ---------------- */

static char *read_all(const char *path)
{
	return et_read_file(path, NULL);
}

/* 按行切分（去掉行尾 \r，首尾空行不产生元素） */
static char **split_lines(const char *s, size_t *n_out)
{
	char **lines = NULL;
	size_t n = 0, cap = 0;
	const char *p = s;

	*n_out = 0;
	if (!s)
		return NULL;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);

		if (len > 0 && p[len - 1] == '\r')
			len--;
		if (n + 1 >= cap) {
			cap = cap ? cap * 2 : 16;
			char **nl = realloc(lines, cap * sizeof(char *));
			if (!nl) {
				free(lines);
				return NULL;
			}
			lines = nl;
		}
		lines[n++] = strndup(p, len);
		if (!eol)
			break;
		p = eol + 1;
	}
	if (lines)
		lines[n] = NULL;
	*n_out = n;
	return lines;
}

static char **read_lines(const char *path, size_t *n_out)
{
	char *data = read_all(path);
	char **lines;

	*n_out = 0;
	if (!data)
		return NULL;
	lines = split_lines(data, n_out);
	free(data);
	return lines;
}

static void free_lines(char **l)
{
	if (!l)
		return;
	for (size_t i = 0; l[i]; i++)
		free(l[i]);
	free(l);
}

static size_t argv_len(char **argv)
{
	size_t n = 0;

	if (!argv)
		return 0;
	while (argv[n])
		n++;
	return n;
}

static void dump_argv(const char *what, char **argv)
{
	printf("  %s 实际内容（%zu 个参数）：\n", what, argv_len(argv));
	for (size_t i = 0; argv && argv[i]; i++)
		printf("    [%2zu] %s\n", i, argv[i]);
}

/* ---------------- 期望值匹配（支持 <uuid> 占位） ---------------- */

/* 期望文件里写 <uuid> 表示"任选一个 UUID 都算对"，避免把随机生成的
 * instance_id / --machine-id 写死导致测试不稳定。 */
static bool is_uuid_at(const char *s)
{
	static const int dash[] = { 8, 13, 18, 23 };

	for (int i = 0; i < 36; i++)
		if (!s[i])
			return false;
	for (int i = 0; i < 36; i++) {
		bool want_dash = false;

		for (size_t k = 0; k < sizeof(dash) / sizeof(dash[0]); k++)
			if (i == dash[k])
				want_dash = true;
		if (want_dash) {
			if (s[i] != '-')
				return false;
		} else if (!isxdigit((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

static bool match_expected(const char *exp, const char *act)
{
	while (*exp) {
		if (strncmp(exp, "<uuid>", 6) == 0) {
			if (!is_uuid_at(act))
				return false;
			exp += 6;
			act += 36;
			continue;
		}
		if (*exp != *act)
			return false;
		exp++;
		act++;
	}
	return *act == '\0';
}

/* ---------------- 比对 ---------------- */

static void check_argv(const char *base, const char *what, char **got,
		       const char *exp_path)
{
	size_t nexp = 0;
	char **exp = read_lines(exp_path, &nexp);
	size_t ngot = 0;

	if (!exp) {
		fail("%s: 读取期望文件 %s 失败 (%s)", base, exp_path, strerror(errno));
		return;
	}
	while (got && got[ngot])
		ngot++;

	if (ngot != nexp) {
		fail("%s/%s: 参数个数 %zu != 期望 %zu", base, what, ngot, nexp);
		dump_argv(what, got);
		for (size_t i = 0; i < nexp; i++)
			printf("    期望[%2zu] %s\n", i, exp[i]);
		free_lines(exp);
		return;
	}

	int bad = 0;
	for (size_t i = 0; i < nexp; i++) {
		if (!match_expected(exp[i], got[i])) {
			fail("%s/%s: 第 %zu 个参数不一致\n    期望: %s\n    实际: %s",
			     base, what, i, exp[i], got[i]);
			bad = 1;
			break;
		}
	}
	if (bad) {
		dump_argv(what, got);
		free_lines(exp);
		return;
	}

	free_lines(exp);
	g_pass++;
}

static void check_text(const char *base, const char *what, const char *got,
		       const char *exp_path)
{
	size_t nexp = 0;
	char **exp = read_lines(exp_path, &nexp);
	size_t nact = 0;
	char **act = split_lines(got, &nact);

	if (!exp) {
		fail("%s: 读取期望文件 %s 失败 (%s)", base, exp_path, strerror(errno));
		return;
	}

	if (nact != nexp) {
		fail("%s/%s: 行数 %zu != 期望 %zu", base, what, nact, nexp);
		goto dump;
	}
	for (size_t i = 0; i < nexp; i++) {
		if (!match_expected(exp[i], act[i])) {
			fail("%s/%s: 第 %zu 行不一致\n    期望: %s\n    实际: %s",
			     base, what, i, exp[i], act[i]);
			goto dump;
		}
	}
	free_lines(exp);
	free_lines(act);
	g_pass++;
	return;

dump:
	for (size_t i = 0; i < nact; i++)
		printf("    实际[%2zu] %s\n", i, act[i]);
	free_lines(exp);
	free_lines(act);
}

static void check_notes(const char *base, const char *notes, const char *exp_path)
{
	size_t n = 0;
	char **keys = read_lines(exp_path, &n);

	if (!keys) {
		fail("%s: 读取期望文件 %s 失败", base, exp_path);
		return;
	}
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		if (!notes || !strstr(notes, keys[i])) {
			fail("%s/notes: 缺少告警 %s", base, keys[i]);
			bad = 1;
		}
	}
	if (bad && notes)
		printf("  实际 notes:\n%s\n", notes);
	free_lines(keys);
	if (!bad)
		g_pass++;
}

/* ---------------- 单元测试 ---------------- */

static void unit_tests(void)
{
	/* 1. et_toml_port：六种 scheme */
	static const char *const toml =
		"listeners = [\n"
		"\t\"tcp://0.0.0.0:11010\",\n"
		"\t\"udp://0.0.0.0:11010\",\n"
		"\t\"ws://0.0.0.0:11011\",\n"
		"\t\"wss://0.0.0.0:11012\",\n"
		"\t\"wg://0.0.0.0:11013\",\n"
		"\t\"quic://[::]:11014\",\n"
		"]\n";
	static const char *const schemes[] = { "tcp", "udp", "ws", "wss", "wg", "quic" };
	static const char *const ports[] = { "11010", "11010", "11011", "11012", "11013", "11014" };

	for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
		char *p = et_toml_port(toml, schemes[i]);

		if (!p || strcmp(p, ports[i]) != 0) {
			fail("et_toml_port(%s) = %s，期望 %s", schemes[i], p ? p : "(null)", ports[i]);
		} else {
			g_pass++;
		}
		free(p);
	}
	if (et_toml_port(toml, "http") != NULL)
		fail("et_toml_port(http) 应返回 NULL");

	/* 2. et_toml_get：顶层优先，退回嵌套 */
	static const char *const doc =
		"instance_name = \"n1\"\n"
		"[flags]\n"
		"dev_name = \"tun0\"\n"
		"mtu = 1380\n";
	char *v = et_toml_get(doc, "instance_name");

	if (!v || strcmp(v, "n1") != 0)
		fail("et_toml_get(instance_name) 错误: %s", v ? v : "(null)");
	else
		g_pass++;
	free(v);

	v = et_toml_get(doc, "mtu");
	if (!v || strcmp(v, "1380") != 0)
		fail("et_toml_get(mtu) 错误: %s", v ? v : "(null)");
	else
		g_pass++;
	free(v);

	/* 3. et_argv_visible：敏感值脱敏，其余保持可读 */
	char *argv[] = { (char *)"easytier-core", (char *)"--network-secret",
			 (char *)"s3cret", (char *)"--vpn-portal-private-key",
			 (char *)"a2V5", (char *)"-r", (char *)"15888", NULL };
	char *vis = et_argv_visible(argv);

	if (!vis || strstr(vis, "s3cret") || strstr(vis, "a2V5") || !strstr(vis, "***"))
		fail("et_argv_visible 未正确脱敏: %s", vis ? vis : "(null)");
	else
		g_pass++;
	free(vis);

	char *join = et_argv_join(argv);
	if (!join || !strstr(join, "s3cret"))
		fail("et_argv_join 不应脱敏: %s", join ? join : "(null)");
	else
		g_pass++;
	free(join);

	/* 4. schema 自检：名称唯一、默认值符合 shell 语义 */
	size_t n_dup = 0;

	for (size_t i = 0; i < et_schema_len; i++) {
		if (!et_schema[i].name) {
			fail("schema[%zu] 缺少 name", i);
			continue;
		}
		for (size_t k = i + 1; k < et_schema_len; k++) {
			if (et_schema[k].name && strcmp(et_schema[i].name, et_schema[k].name) == 0) {
				fail("schema 名称重复: %s", et_schema[i].name);
				n_dup++;
			}
		}
		if (!et_schema_find(et_schema[i].name))
			fail("et_schema_find(%s) 失败", et_schema[i].name);
	}
	if (n_dup == 0)
		g_pass++;

	const et_field *f = et_schema_find("rpc_portal");
	if (!f || !f->def || strcmp(f->def, "15888") != 0)
		fail("rpc_portal 默认值应为 15888（init.d:210）");
	else
		g_pass++;
	f = et_schema_find("multi_thread");
	if (!f || !f->def || strcmp(f->def, "0") != 0)
		fail("multi_thread 默认值应为 0（init.d:230）");
	else
		g_pass++;
	f = et_schema_find("log");
	if (!f || !f->def || strcmp(f->def, "off") != 0)
		fail("log 默认值应为 off（init.d:239）");
	else
		g_pass++;
	f = et_schema_find("comp");
	if (!f || !f->def || strcmp(f->def, "none") != 0)
		fail("comp 默认值应为 none（init.d:246）");
	else
		g_pass++;
}

/* ---------------- UCI 语义测试 ---------------- */

static void uci_semantics(const char *dir)
{
	char path[512];
	const char *conf =
		"config easytier 'x'\n"
		"\toption scalar 'v1'\n"
		"\tlist items 'has space inside'\n"
		"\tlist items 'second'\n"
		"\toption empty ''\n";

	snprintf(path, sizeof(path), "%s/easytier", dir);
	if (et_write_file(path, conf, strlen(conf)) != ET_OK) {
		fail("写入 UCI 样例失败: %s", path);
		return;
	}

	et_uci *u = et_uci_open();
	if (!u) {
		fail("et_uci_open 失败（ET_UCI_CONFDIR=%s）", dir);
		return;
	}

	char *s = et_uci_get_et(u, "scalar");
	if (!s || strcmp(s, "v1") != 0)
		fail("et_uci_get(scalar) = %s", s ? s : "(null)");
	else
		g_pass++;
	free(s);

	/* list 单值接口：按空格连接（与 uci -q get 一致） */
	s = et_uci_get_et(u, "items");
	if (!s || strcmp(s, "has space inside second") != 0)
		fail("et_uci_get(list) = %s，期望 'has space inside second'", s ? s : "(null)");
	else
		g_pass++;
	free(s);

	/* list 接口：逐元素，且保留元素内部空格 */
	size_t n = 0;
	char **v = et_uci_get_et_list(u, "items", &n);
	if (n != 2 || !v || strcmp(v[0], "has space inside") != 0 || strcmp(v[1], "second") != 0)
		fail("et_uci_get_list 元素不符（n=%zu，v0=%s）", n, v && v[0] ? v[0] : "(null)");
	else
		g_pass++;
	if (v) {
		for (size_t i = 0; i < n; i++)
			free(v[i]);
		free(v);
	}

	/* 缺失项返回 NULL；空值选项 libuci 直接丢弃，因此同样返回 NULL
	 * （与 `uci -q get` 返回 rc=1、无输出完全一致） */
	if (et_uci_get_et(u, "no_such_option") != NULL)
		fail("缺失项应返回 NULL");
	else
		g_pass++;
	if (et_uci_get_et(u, "empty") != NULL)
		fail("空值选项应返回 NULL（libuci 丢弃空值）");
	else
		g_pass++;

	et_uci_close(u);
}

/* ---------------- 写回 UCI 的往返测试 ---------------- */

static void save_roundtrip(const char *dir)
{
	char path[512];
	const char *conf =
		"config easytier 'cfg'\n"
		"\toption enabled '1'\n"
		"\toption network_name 'old'\n"
		"\tlist peeradd 'tcp://a.example.com:11010'\n";
	bool changed = false;

	snprintf(path, sizeof(path), "%s/easytier", dir);
	if (et_write_file(path, conf, strlen(conf)) != ET_OK) {
		fail("写入 UCI 样例失败: %s", path);
		return;
	}

	et_uci *u = et_uci_open();
	if (!u) {
		fail("et_uci_open 失败（写回测试）");
		return;
	}
	et_cfg *c = et_cfg_load(u);
	if (!c) {
		fail("et_cfg_load 失败（写回测试）");
		et_uci_close(u);
		return;
	}

	/* 首次保存：内存值全部来自 UCI 或等于 schema 默认值 → 不应产生任何写入
	 * （默认值不物化，避免第一次保存就把几十个默认项写进 /etc/config/easytier） */
	int rc = et_cfg_save(c, u, &changed);

	if (rc != ET_OK || changed)
		fail("et_cfg_save 首次不应物化默认值（rc=%d changed=%d）", rc, (int)changed);
	else
		g_pass++;

	/* 再次保存：仍然幂等 */
	changed = true;
	if (et_cfg_save(c, u, &changed) != ET_OK || changed)
		fail("et_cfg_save 第二次应幂等（实际 changed=%d）", (int)changed);
	else
		g_pass++;

	/* 改内存值 + 加一条 extra_args，再保存 */
	for (size_t i = 0; i < c->n; i++) {
		if (c->items[i].f && strcmp(c->items[i].f->name, "network_name") == 0) {
			free(c->items[i].val);
			c->items[i].val = strdup("new name");
		}
	}
	et_list_add(&c->extra_args, "--tcp-whitelist 80");
	changed = false;
	if (et_cfg_save(c, u, &changed) != ET_OK || !changed)
		fail("et_cfg_save 修改后应置 changed（实际 changed=%d）", (int)changed);
	else
		g_pass++;

	/* 只写有变化的项：默认值不得被物化 */
	char *saved = et_read_file(path, NULL);
	if (!saved) {
		fail("写回后无法读取 %s", path);
	} else {
		bool ok = strstr(saved, "option network_name 'new name'") != NULL &&
			  strstr(saved, "list extra_args") != NULL &&
			  strstr(saved, "rpc_portal") == NULL &&
			  strstr(saved, "multi_thread") == NULL;

		if (!ok)
			fail("写回内容不符（应只写改动项，不物化默认值）:\n%s", saved);
		else
			g_pass++;
		free(saved);
	}

	et_cfg_free(c);
	et_uci_close(u);

	/* libuci 的 delta 目录默认是 /tmp/.uci，清掉避免污染后续用例 */
	unlink("/tmp/.uci/easytier");

	u = et_uci_open();
	c = et_cfg_load(u);
	const char *v = et_cfg_str(c, "network_name");
	if (!v || strcmp(v, "new name") != 0)
		fail("写回后 network_name = %s，期望 'new name'", v ? v : "(null)");
	else
		g_pass++;

	const et_list *ea = et_cfg_list(c, "extra_args");
	if (!ea || ea->n != 1 || strcmp(ea->v[0], "--tcp-whitelist 80") != 0)
		fail("写回后 extra_args 不符（n=%zu）", ea ? ea->n : 0);
	else
		g_pass++;

	if (c)
		et_cfg_free(c);
	et_uci_close(u);

	/* 写回后文件仍必须可读（uci commit 没把文件写坏） */
	u = et_uci_open();
	if (u) {
		g_pass++;
		et_uci_close(u);
	} else {
		fail("写回后 UCI 文件不可读");
	}
}

/* ---------------- section 创建 / changed 语义 ---------------- */

static void uci_section_tests(const char *dir)
{
	char path[512];
	const char *conf = "config rule 'other'\n\toption name 'x'\n";

	snprintf(path, sizeof(path), "%s/firewall", dir);
	if (et_write_file(path, conf, strlen(conf)) != ET_OK) {
		fail("写入 firewall 样例失败: %s", path);
		return;
	}
	unlink("/tmp/.uci/firewall");

	et_uci *u = et_uci_open();
	if (!u) {
		fail("et_uci_open 失败（section 测试）");
		return;
	}

	/* 1. 未改动 → changed=false */
	if (et_uci_changed(u, "firewall"))
		fail("未改动时 et_uci_changed 应为 false");
	else
		g_pass++;

	/* 2. 新建 section（等价于 `uci set firewall.et_test=rule`） */
	if (et_uci_ensure_sec(u, "firewall", "et_test", "rule") != ET_OK)
		fail("et_uci_ensure_sec 创建 section 失败");
	else
		g_pass++;

	if (!et_uci_changed(u, "firewall"))
		fail("ensure_sec 之后 et_uci_changed 应为 true");
	else
		g_pass++;

	/* 3. 往新建 section 里写 option，再 ensure 一次（同类型，不应报错） */
	if (et_uci_set(u, "firewall", "et_test", "name", "v1") != ET_OK)
		fail("向新 section 写 option 失败");
	else
		g_pass++;
	if (et_uci_ensure_sec(u, "firewall", "et_test", "rule") != ET_OK)
		fail("同类型 ensure_sec 应幂等成功");
	else
		g_pass++;

	/* 4. commit 后 changed 必须回到 false（libuci 的 has_delta 恒真，不能拿它判断） */
	if (et_uci_commit(u, "firewall") != ET_OK)
		fail("commit firewall 失败");
	else
		g_pass++;
	if (et_uci_changed(u, "firewall"))
		fail("commit 之后 et_uci_changed 应为 false");
	else
		g_pass++;

	/* 5. 落盘结果：section 存在且 option 在 */
	char *data = et_read_file(path, NULL);
	if (!data || !strstr(data, "config rule 'et_test'") || !strstr(data, "option name 'v1'"))
		fail("commit 后 firewall 文件内容不符:\n%s", data ? data : "(null)");
	else
		g_pass++;
	free(data);

	/* 6. 改类型：rule → zone，option 必须保留 */
	if (et_uci_ensure_sec(u, "firewall", "et_test", "zone") != ET_OK)
		fail("ensure_sec 改类型失败");
	else
		g_pass++;
	if (et_uci_commit(u, "firewall") != ET_OK)
		fail("改类型后 commit 失败");
	else
		g_pass++;
	data = et_read_file(path, NULL);
	if (!data || !strstr(data, "config zone 'et_test'") || strstr(data, "config rule 'et_test'")
	    || !strstr(data, "option name 'v1'"))
		fail("改类型后文件内容不符（应变成 zone 且保留 option）:\n%s", data ? data : "(null)");
	else
		g_pass++;
	free(data);

	/* 7. 匿名 section：@easytier[0] 已存在且类型一致 → 成功且不产生改动 */
	if (et_uci_ensure_sec(u, "easytier", "@easytier[0]", "easytier") != ET_OK)
		fail("ensure_sec 匿名 section 失败");
	else
		g_pass++;

	et_uci_close(u);
	unlink("/tmp/.uci/firewall");
	unlink("/tmp/.uci/easytier");
}

/* ---------------- 主流程 ---------------- */

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
	const char *golden = argc > 1 ? argv[1] : "tests/golden";
	char tmpdir[] = "/tmp/et_uci_case.XXXXXX";
	char path[1024];

	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "mkdtemp 失败: %s\n", strerror(errno));
		return 2;
	}
	setenv("ET_UCI_CONFDIR", tmpdir, 1);

	/* 机器 UUID 不再预置：web 模式的 --machine-id 与 toml 的 instance_id 都用
	 * 期望文件里的 <uuid> 占位做格式断言，这样 /etc/easytier 是否存在都不影响结果 */

	printf("== 单元测试 ==\n");
	unit_tests();
	printf("== UCI 语义 ==\n");
	uci_semantics(tmpdir);
	printf("== 写回往返 ==\n");
	save_roundtrip(tmpdir);
	printf("== section / changed ==\n");
	uci_section_tests(tmpdir);

	printf("== golden: %s ==\n", golden);

	DIR *d = opendir(golden);
	if (!d) {
		fprintf(stderr, "无法打开 %s: %s\n", golden, strerror(errno));
		return 2;
	}

	et_list cases;
	et_list_init(&cases);
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t len = strlen(e->d_name);

		if (len > 4 && strcmp(e->d_name + len - 4, ".uci") == 0)
			et_list_add(&cases, e->d_name);
	}
	closedir(d);
	qsort(cases.v, cases.n, sizeof(char *), cmp_str);

	for (size_t i = 0; i < cases.n; i++) {
		const char *file = cases.v[i];
		char base_name[256];
		char base[1024];
		char *content;

		snprintf(base_name, sizeof(base_name), "%s", file);
		base_name[strlen(base_name) - 4] = '\0';
		snprintf(base, sizeof(base), "%s/%s", golden, base_name);
		snprintf(path, sizeof(path), "%s/%s", golden, file);

		content = read_all(path);
		if (!content) {
			fail("%s: 无法读取样例", file);
			continue;
		}
		char uci_path[600];
		snprintf(uci_path, sizeof(uci_path), "%s/easytier", tmpdir);
		if (et_write_file(uci_path, content, strlen(content)) != ET_OK) {
			fail("%s: 写入临时 UCI 失败", file);
			free(content);
			continue;
		}
		free(content);

		et_uci *u = et_uci_open();
		if (!u) {
			fail("%s: et_uci_open 失败", base_name);
			continue;
		}
		et_cfg *c = et_cfg_load(u);
		if (!c) {
			fail("%s: et_cfg_load 失败", base_name);
			et_uci_close(u);
			continue;
		}

		et_buf notes;
		et_buf_init(&notes);

		char exp[1200];
		snprintf(exp, sizeof(exp), "%s.core.args", base);
		if (et_file_exists(exp)) {
			char **av = et_argv_core(c, "easytier-core", &notes);

			check_argv(base_name, "core", av, exp);
			et_argv_free(av);
		}

		snprintf(exp, sizeof(exp), "%s.web.args", base);
		if (et_file_exists(exp)) {
			char **av = et_argv_web(c, "easytier-web", &notes);

			check_argv(base_name, "web", av, exp);
			et_argv_free(av);
		}

		snprintf(exp, sizeof(exp), "%s.notes", base);
		if (et_file_exists(exp))
			check_notes(base_name, notes.p, exp);

		snprintf(exp, sizeof(exp), "%s.toml", base);
		if (et_file_exists(exp)) {
			char *toml = et_toml_render(c);

			check_text(base_name, "toml", toml, exp);
			free(toml);
		}

		et_buf_free(&notes);
		et_cfg_free(c);
		et_uci_close(u);
	}

	et_list_free(&cases);
	rmdir(tmpdir);

	printf("\n== 结果: %d 项通过, %d 项失败 ==\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
