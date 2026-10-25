/*
 * etd_i18n.h - C 侧 i18n 字典
 *
 * Lua 侧继续用 LuCI 自己的 _()（.lmo），但 C 生成的 HTML 片段（配置表单）
 * 拿不到 LuCI 的翻译表，因此构建期由 tools/i18n_build.py 把 po 目录下的
 * easytier.po 转成 /usr/share/easytier/i18n/<lang>.json（键 = msgid，
 * 值 = msgstr），运行时由本模块加载查表。
 */
#ifndef ETD_I18N_H
#define ETD_I18N_H

#include "etd.h"

/* 载入字典（幂等）。语言识别顺序：
 *   LC_ALL → LC_MESSAGES → LANG → uci luci.main.lang → HTTP_ACCEPT_LANGUAGE → "en"
 * 任何一步失败都不算错误，et_i18n() 会退化为返回 msgid 本身。 */
void et_i18n_init(void);

/* 释放字典（进程退出 / 测试用例之间切换语言用） */
void et_i18n_free(void);

/* 显式加载语言，接受 "en" / "zh_Hans" / "zh_CN.UTF-8" 等写法。
 * 没有对应 JSON 时返回 false（不影响已加载字典）。 */
bool et_i18n_load(const char *lang);

/* 查表；未命中、未初始化或 msgid 为 NULL 时原样返回 msgid */
const char *et_i18n(const char *msgid);

/* 当前生效的语言名（未初始化返回空串） */
const char *et_i18n_lang(void);

#endif /* ETD_I18N_H */
