/*
 * etd_i18n.c - i18n 字典加载与查表
 *
 * 字典格式（构建期由 tools/i18n_build.py 生成）：
 *   {"Network Name": "网络名称", ...}
 * 即一个扁平的 msgid → msgstr 映射；en.json 为 identity 映射。
 *
 * 为什么不复用 LuCI 的 .lmo：.lmo 是 LuCI 私有的二进制格式，解析它需要把
 * LuCI 的 lmo 读取逻辑搬进 C；而配置表单的文案量很小（约 120 条），生成
 * 一份 JSON 更简单，也便于测试逐字比对。
 */
#include "etd_i18n.h"
#include "etd_util.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>

/* 可用 -DET_DIR_I18N=... 覆盖（测试用），运行时还可用 ET_I18N_DIR 环境变量 */
#ifndef ET_DIR_I18N
#define ET_DIR_I18N "/usr/share/easytier/i18n"
#endif

#define ET_LANG_MAX 32

static json_object *et_dict;
static bool et_inited;
static char et_cur[ET_LANG_MAX];

static const char *i18n_dir(void)
{
	const char *env = getenv("ET_I18N_DIR");

	return (env && *env) ? env : ET_DIR_I18N;
}

/* "zh_CN.UTF-8" / "zh-cn" / "en_US" → "zh_Hans" / "en" */
static void norm_lang(const char *in, char *out, size_t n)
{
	char base[64];
	size_t j = 0;

	if (!in || !*in) {
		snprintf(out, n, "en");
		return;
	}
	for (const char *p = in; *p && j < sizeof(base) - 1; p++) {
		if (*p == '.' || *p == '@')
			break;
		base[j++] = (char)tolower((unsigned char)*p);
	}
	base[j] = '\0';

	if (strncmp(base, "zh", 2) == 0) {
		/* 繁体没有字典，退化为 identity（与 LuCI 无 zh_Hant 时的行为一致） */
		if (strstr(base, "hant") || strstr(base, "tw") ||
		    strstr(base, "hk") || strstr(base, "mo"))
			snprintf(out, n, "zh_Hant");
		else
			snprintf(out, n, "zh_Hans");
		return;
	}
	snprintf(out, n, "en");
}

/* LuCI 会话语言：uci luci.main.lang（luci.i18n.setlanguage() 写在这里） */
static char *luci_lang(void)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_ptr ptr;
	char tuple[] = "luci.main.lang";
	char *out = NULL;

	if (!ctx)
		return NULL;
	memset(&ptr, 0, sizeof(ptr));
	if (uci_lookup_ptr(ctx, &ptr, tuple, true) == UCI_OK &&
	    (ptr.flags & UCI_LOOKUP_COMPLETE) && ptr.o && ptr.o->v.string &&
	    *ptr.o->v.string)
		out = strdup(ptr.o->v.string);
	uci_free_context(ctx);
	return out;
}

/* "zh-CN,zh;q=0.9,en;q=0.8" → "zh-CN" */
static char *accept_lang(void)
{
	const char *hdr = getenv("HTTP_ACCEPT_LANGUAGE");
	char buf[64];
	size_t j = 0;

	if (!hdr || !*hdr)
		return NULL;
	for (const char *p = hdr; *p && *p != ',' && *p != ';' && j < sizeof(buf) - 1; p++)
		buf[j++] = *p;
	buf[j] = '\0';
	while (j > 0 && (buf[j - 1] == ' ' || buf[j - 1] == '\t'))
		buf[--j] = '\0';
	return j ? strdup(buf) : NULL;
}

static char *detect_lang(void)
{
	static const char *vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
	char *cand = NULL;

	for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
		const char *v = getenv(vars[i]);

		/* C/POSIX locale 等于"没有指定语言"，继续往下找 */
		if (v && *v && strcmp(v, "C") != 0 && strcmp(v, "POSIX") != 0)
			return strdup(v);
	}
	cand = luci_lang();
	if (cand && *cand)
		return cand;
	free(cand);
	return accept_lang();
}

bool et_i18n_load(const char *lang)
{
	char name[ET_LANG_MAX];
	char path[512];
	char *data;
	json_object *obj;

	norm_lang(lang, name, sizeof(name));
	if (snprintf(path, sizeof(path), "%s/%s.json", i18n_dir(), name) >= (int)sizeof(path))
		return false;

	data = et_read_file(path, NULL);
	if (!data)
		return false;
	obj = json_tokener_parse(data);
	free(data);
	if (!obj)
		return false;
	if (!json_object_is_type(obj, json_type_object)) {
		json_object_put(obj);
		return false;
	}

	if (et_dict)
		json_object_put(et_dict);
	et_dict = obj;
	snprintf(et_cur, sizeof(et_cur), "%s", name);
	return true;
}

void et_i18n_init(void)
{
	char *lang;

	if (et_inited)
		return;
	et_inited = true;

	lang = detect_lang();
	if (lang) {
		/* 首选语言没有字典时退回英文（identity），保证 msgid 语义可用 */
		if (!et_i18n_load(lang))
			et_i18n_load("en");
	} else {
		et_i18n_load("en");
	}
	free(lang);
}

void et_i18n_free(void)
{
	if (et_dict) {
		json_object_put(et_dict);
		et_dict = NULL;
	}
	et_cur[0] = '\0';
}

const char *et_i18n(const char *msgid)
{
	json_object *v = NULL;

	if (!msgid)
		return "";
	if (!et_inited)
		et_i18n_init();
	if (!et_dict)
		return msgid;
	if (json_object_object_get_ex(et_dict, msgid, &v) &&
	    json_object_is_type(v, json_type_string)) {
		const char *s = json_object_get_string(v);

		if (s && *s)
			return s;
	}
	return msgid;
}

const char *et_i18n_lang(void)
{
	return et_cur;
}
