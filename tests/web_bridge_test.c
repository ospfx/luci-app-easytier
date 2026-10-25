/*
 * web_bridge_test.c - 配置页后端（config_get/save/render/schema + i18n）自测
 *
 * 不依赖路由器：用 ET_UCI_CONFDIR 指向临时目录里的 easytier 配置，
 * 用 ET_I18N_DIR 指向构建出来的 i18n JSON，然后在宿主机上直接跑。
 *
 * 用法：web_bridge_test [i18n_json_dir]
 * 退出码 0 = 全部通过。
 */
#include "etd_api.h"
#include "etd_i18n.h"
#include "etd_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

/* etd_api.c 的 et_api_dispatch() 引用路由表；本测试只调 4 个 handler，
 * 不链接 etd_routes.c（它会把全部 api 实现拖进来），故给一个空表桩。 */
const et_route et_routes[] = { { NULL, NULL, NULL } };
const size_t et_routes_len = 0;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			fails++; \
		} \
	} while (0)

static const char *INIT_UCI =
	"config easytier 'easytier'\n"
	"\toption enabled '0'\n"
	"\toption etcmd 'etcmd'\n"
	"\toption network_name 'oldnet'\n"
	"\toption network_secret 'oldsecret'\n"
	"\toption ipaddr '10.0.0.9'\n"
	"\toption log 'info'\n"
	"\tlist peeradd 'tcp://old.example:11010'\n"
	"\tlist et_forward 'etfwlan'\n"
	"\toption web_enabled '0'\n"
	"\toption web_port '22020'\n";

static const char *CONF_DIR = "/tmp/et_web_test/etc/config";
static const char *CONF_FILE = "/tmp/et_web_test/etc/config/easytier";

static void run(const char *cmd)
{
	if (system(cmd) != 0)
		printf("WARN: 命令失败: %s\n", cmd);
}

static char *slurp(const char *path)
{
	return et_read_file(path, NULL);
}

typedef int (*handler)(const et_req *req, et_res *res);

/* 只测本模块的 4 个 handler：直接调用，避免拉起 etd_routes.c（会拖进全部 api 实现） */
static handler lookup(const char *method)
{
	if (!strcmp(method, "config_get"))
		return et_api_config_get;
	if (!strcmp(method, "config_save"))
		return et_api_config_save;
	if (!strcmp(method, "config_render"))
		return et_api_config_render;
	if (!strcmp(method, "schema"))
		return et_api_schema;
	return NULL;
}

static void call(const char *method, et_list *params, et_ctype want, et_res *res)
{
	et_req req = { .method = method, .params = params, .body = NULL, .body_len = 0 };
	handler fn = lookup(method);

	et_res_init(res, want);
	if (!fn) {
		printf("FAIL: 未知方法 %s\n", method);
		fails++;
		return;
	}
	int rc = fn(&req, res);

	CHECK(rc == ET_OK, "%s 返回 %d", method, rc);
	CHECK(res->body.p != NULL && res->body.len > 0, "%s 响应为空", method);
}

int main(int argc, char **argv)
{
	const char *i18n = argc > 1 ? argv[1] : "luci-app-easytier/root/usr/share/easytier/i18n";
	char path[512];

	run("rm -rf /tmp/et_web_test && mkdir -p /tmp/et_web_test/etc/config");
	if (et_write_file(CONF_FILE, INIT_UCI, strlen(INIT_UCI)) != ET_OK) {
		printf("FAIL: 无法写入 %s\n", CONF_FILE);
		return 1;
	}
	setenv("ET_UCI_CONFDIR", CONF_DIR, 1);
	setenv("ET_I18N_DIR", i18n, 1);
	setenv("LANG", "zh_CN.UTF-8", 1);
	setenv("LC_ALL", "zh_CN.UTF-8", 1);

	et_i18n_init();
	CHECK(!strcmp(et_i18n_lang(), "zh_Hans"), "语言识别 = %s（期望 zh_Hans）", et_i18n_lang());
	CHECK(!strcmp(et_i18n("Network Name"), "网络名称"), "i18n 未命中: %s",
	      et_i18n("Network Name"));
	CHECK(!strcmp(et_i18n("不存在的键"), "不存在的键"), "未命中时应原样返回 msgid");

	/* ---------- config_render ---------- */
	{
		et_res res;
		et_list no;

		et_list_init(&no);
		call("config_render", &no, ET_CT_HTML, &res);
		CHECK(res.type == ET_CT_HTML, "config_render 应返回 HTML 类型");
		CHECK(strstr(res.body.p, "class=\"et-tabs\"") != NULL, "缺少 tab 导航");
		CHECK(strstr(res.body.p, "基本设置") != NULL, "tab 标题未翻译");
		CHECK(strstr(res.body.p, "高级设置") != NULL, "缺少高级设置 tab");
		CHECK(strstr(res.body.p, "网络名称") != NULL, "字段 label 未翻译");
		CHECK(strstr(res.body.p, "name=\"network_name\"") != NULL, "缺 network_name");
		CHECK(strstr(res.body.p, "value=\"oldnet\"") != NULL, "UCI 值未填入表单");
		CHECK(strstr(res.body.p, "data-name=\"peeradd\"") != NULL, "list 字段未渲染");
		CHECK(strstr(res.body.p, "tcp://old.example:11010") != NULL, "list 值未填入");
		CHECK(strstr(res.body.p, "class=\"et-checks\" data-name=\"et_forward\"") != NULL,
		      "et_forward 未渲染成勾选组");
		CHECK(strstr(res.body.p, "value=\"etfwlan\" checked=\"checked\"") != NULL,
		      "et_forward 已有值未勾选");
		CHECK(strstr(res.body.p, "<select") != NULL, "enum 字段未用 select");
		CHECK(strstr(res.body.p, "type=\"checkbox\"") != NULL, "bool 字段未用 checkbox");
		CHECK(strstr(res.body.p, "value=\"11010\"") != NULL, "tcp_port 表单默认值缺失");
		CHECK(strstr(res.body.p, "placeholder=\"10.0.0.1\"") != NULL, "placeholder 缺失");
		CHECK(strstr(res.body.p, "id=\"et_et_config\"") != NULL, "config.toml 文本框缺失");
		CHECK(strstr(res.body.p, "id=\"et_uuid\"") != NULL, "uuid 字段缺失");
		CHECK(strstr(res.body.p, "data-dep=\"etcmd:config\"") != NULL, "缺少显隐依赖");
		CHECK(strstr(res.body.p, "name=\"web_db_path\"") != NULL, "web 组未渲染");
		CHECK(strstr(res.body.p, "name=\"easytierbin\"") == NULL, "system 组不应在配置页渲染");
		CHECK(strstr(res.body.p, "name=\"network_secret\"") != NULL &&
		      strstr(res.body.p, "type=\"password\"") != NULL, "密码字段未用 password");
		printf("render: %zu 字节\n", res.body.len);
		if (argc > 2) /* web_bridge_test <i18n_dir> <dump.html>：人工核对 HTML 用 */
			et_write_file(argv[2], res.body.p, res.body.len);
		et_res_free(&res);
	}

	/* ---------- config_save（正常保存） ---------- */
	{
		et_res res;
		et_list p;

		et_list_init(&p);
		et_list_add(&p, "network_name=newnet");
		et_list_add(&p, "network_secret=新密码");
		et_list_add(&p, "ipaddr=10.1.2.3");
		et_list_add(&p, "enabled=1");
		et_list_add(&p, "log=debug");
		et_list_add(&p, "listenermode=OFF");
		et_list_add(&p, "peeradd=tcp://a.example:11010\ntcp://b.example:11011");
		et_list_add(&p, "et_forward=lanfwet\nwanfwet");
		et_list_add(&p, "unknown_field=ignored");
		call("config_save", &p, ET_CT_JSON, &res);
		CHECK(strstr(res.body.p, "\"success\":true") != NULL, "保存失败: %s", res.body.p);
		CHECK(strstr(res.body.p, "配置保存成功") != NULL, "成功提示未翻译: %s", res.body.p);
		et_res_free(&res);
		et_list_free(&p);

		char *cfg = slurp(CONF_FILE);

		CHECK(cfg != NULL, "读不到保存后的配置");
		if (cfg) {
			CHECK(strstr(cfg, "newnet") != NULL, "network_name 未写入");
			CHECK(strstr(cfg, "新密码") != NULL, "network_secret 未写入");
			CHECK(strstr(cfg, "10.1.2.3") != NULL, "ipaddr 未写入");
			CHECK(strstr(cfg, "option enabled '1'") != NULL, "enabled 未写入");
			CHECK(strstr(cfg, "tcp://a.example:11010") != NULL, "list 第一项未写入");
			CHECK(strstr(cfg, "tcp://b.example:11011") != NULL, "list 第二项未写入");
			CHECK(strstr(cfg, "old.example") == NULL, "旧 list 值未被替换");
			CHECK(strstr(cfg, "lanfwet") != NULL && strstr(cfg, "wanfwet") != NULL,
			      "et_forward 多选值未写入");
			CHECK(strstr(cfg, "etfwlan") == NULL, "et_forward 旧值未被替换");
			CHECK(strstr(cfg, "unknown_field") == NULL, "未知字段不应写入");
			free(cfg);
		}
	}

	/* ---------- config_save（非法值） ---------- */
	{
		et_res res;
		et_list p;

		et_list_init(&p);
		et_list_add(&p, "tcp_port=abc");
		call("config_save", &p, ET_CT_JSON, &res);
		CHECK(strstr(res.body.p, "\"success\":false") != NULL, "非法端口应被拒绝: %s",
		      res.body.p);
		et_res_free(&res);
		et_list_free(&p);

		et_list_init(&p);
		et_list_add(&p, "log=not-a-level");
		call("config_save", &p, ET_CT_JSON, &res);
		CHECK(strstr(res.body.p, "\"success\":false") != NULL, "非法枚举应被拒绝: %s",
		      res.body.p);
		et_res_free(&res);
		et_list_free(&p);
	}

	/* ---------- config_save（config.toml 落盘） ---------- */
	{
		et_res res;
		et_list p;
		char *toml;

		run("mkdir -p /tmp/et_web_test/etc/easytier");
		snprintf(path, sizeof(path), "/tmp/et_web_test/etc/easytier/config.toml");
		et_list_init(&p);
		et_list_add(&p, "et_config=[[instances]]\r\nname = \"x\"\r\n");
		call("config_save", &p, ET_CT_JSON, &res);
		et_res_free(&res);
		et_list_free(&p);

		toml = slurp(path);
		CHECK(toml != NULL, "config.toml 未写入");
		if (toml) {
			CHECK(strstr(toml, "\r") == NULL, "config.toml 未做 \\r\\n → \\n 归一化");
			CHECK(strstr(toml, "name = \"x\"") != NULL, "config.toml 内容不正确");
			free(toml);
		}
	}

	/* ---------- schema / config_get ---------- */
	{
		et_res res;
		et_list no;
		char needle[64];

		et_list_init(&no);
		call("schema", &no, ET_CT_JSON, &res);
		snprintf(needle, sizeof(needle), "\"count\":%zu", et_schema_len);
		CHECK(strstr(res.body.p, needle) != NULL, "schema count 不对: %.120s", res.body.p);
		CHECK(strstr(res.body.p, "\"tabs\"") != NULL, "schema 缺 tabs");
		CHECK(strstr(res.body.p, "\"name\":\"network_name\"") != NULL, "schema 缺字段");
		et_res_free(&res);
		et_list_free(&no);

		et_list_init(&no);
		call("config_get", &no, ET_CT_JSON, &res);
		CHECK(strstr(res.body.p, "\"success\":true") != NULL, "config_get 失败");
		CHECK(strstr(res.body.p, "\"network_name\":\"newnet\"") != NULL,
		      "config_get 值不对: %.200s", res.body.p);
		CHECK(strstr(res.body.p, "\"peeradd\":[\"tcp://a.example:11010\",\"tcp://b.example:11011\"]")
		      != NULL, "config_get list 序列化不对: %.200s", res.body.p);
		et_res_free(&res);
		et_list_free(&no);
	}

	et_i18n_free();

	if (fails) {
		printf("失败 %d 项\n", fails);
		return 1;
	}
	printf("全部通过\n");
	return 0;
}
