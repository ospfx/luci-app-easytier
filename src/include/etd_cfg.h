/*
 * etd_cfg.h - 配置模型与命令行拼装
 *
 * 设计要点：用一张 schema 表（et_schema[]）作为唯一事实来源，同时驱动
 *   1) 从 UCI 读取配置与默认值
 *   2) easytier-core / easytier-web 的 argv 拼装
 *   3) 前端配置表单渲染
 *   4) 文档生成
 * 避免 95 个选项在四处重复维护（原 Lua/Shell 实现的历史问题）。
 */
#ifndef ETD_CFG_H
#define ETD_CFG_H

#include "etd.h"
#include "etd_util.h"
#include "etd_uci.h"

/* 字段值类型（决定 UCI 读写方式与表单控件） */
enum et_kind {
	ET_K_STR,   /* 普通字符串 */
	ET_K_INT,   /* 整数（UCI 仍存字符串） */
	ET_K_BOOL,  /* UCI 中为 "0"/"1" */
	ET_K_ENUM,  /* 枚举，见 .enum_vals */
	ET_K_LIST,  /* UCI list，可多条 */
	ET_K_PASS,  /* 密码/密钥，表单用 password 控件 */
};

/* 命令行发射规则（严格对应原 shell 中的各种判空/判 0 写法） */
enum et_emit {
	ET_E_NONE = 0,
	ET_E_VAL,      /* [ -z "$x" ] || flag $x          —— 有值就带参数 */
	ET_E_YES,      /* [ "$x" = "0" ] || flag          —— 为 "1" 时输出裸旗标 */
	ET_E_NO,       /* [ "$x" = "1" ] || flag          —— 为 "0" 时输出裸旗标 */
	ET_E_BOOLVAL,  /* 始终输出 "flag true|false" */
	ET_E_REPEAT,   /* list：每个元素输出一次 flag <元素> */
	ET_E_REPEAT_JOIN, /* list：每个元素输出一次 "flag 'a b'"（带引号整体） */
	ET_E_CUSTOM,   /* 需要特殊逻辑，由 et_argv_core() 手写 */
};

typedef struct {
	const char *name;      /* UCI option 名 */
	enum et_kind kind;
	const char *def;       /* 默认值，NULL 表示无默认 */
	const char *label;     /* i18n 英文源串（.po 中需存在） */
	const char *help;      /* i18n 帮助串，可为 NULL */
	const char *tab;       /* 表单分组 id，如 "basic" */
	const char *enum_vals; /* ET_K_ENUM: 逗号分隔，如 "off,error,warn,info" */
	const char *core_flag; /* easytier-core 旗标，NULL 表示不参与 */
	enum et_emit core_emit;
	const char *web_flag;  /* easytier-web 旗标 */
	enum et_emit web_emit;
} et_field;

extern const et_field et_schema[];
extern const size_t et_schema_len;
const et_field *et_schema_find(const char *name);

/* 运行时配置 */
typedef struct {
	const et_field *f;
	char *val;        /* 单值（list 为 NULL） */
	et_list list;     /* list 值 */
} et_cfg_item;

typedef struct {
	et_cfg_item *items;
	size_t n;
	et_list extra_args; /* 额外参数，逐行原样附加 */
	char *easytierbin;  /* 已解析的 core 路径 */
	char *webbin;       /* 已解析的 web 路径 */
} et_cfg;

/* 按 schema 从 UCI 读取；缺失项填入 schema 默认值。失败返回 NULL */
et_cfg *et_cfg_load(et_uci *u);
void et_cfg_free(et_cfg *c);

const char *et_cfg_str(et_cfg *c, const char *name);
int et_cfg_int(et_cfg *c, const char *name, int def);
bool et_cfg_is01(et_cfg *c, const char *name); /* 值为 "1" */
const et_list *et_cfg_list(et_cfg *c, const char *name);
/* 写回 UCI（只写与 UCI 现有值不同的项）。changed 输出是否有改动 */
int et_cfg_save(et_cfg *c, et_uci *u, bool *changed);

/* ---------- argv 拼装 ---------- */
/* 返回以 NULL 结尾的 argv（argv[0]=bin），调用者用 et_argv_free 释放。
 * notes 收集与旧版行为差异的告警（如 vpn_portal 旧格式被跳过）。*/
char **et_argv_core(et_cfg *c, const char *bin, et_buf *notes);
char **et_argv_web(et_cfg *c, const char *bin, et_buf *notes);
void et_argv_free(char **argv);
/* 用单引号安全拼接（日志与状态页展示） */
char *et_argv_join(char **argv);

/* 状态页展示用：过滤掉敏感参数后的命令行 */
char *et_argv_visible(char **argv);

/* ---------- config.toml ---------- */
/* 生成 config.toml 内容（默认模板 + 按配置填充），返回 malloc 字符串 */
char *et_toml_render(et_cfg *c);
/* 从 toml 中解析指定键（nested=false 时取顶层） */
char *et_toml_get(const char *content, const char *key);
/* 端口类快捷读取：tcp/udp/ws/wss/wg/quic */
char *et_toml_port(const char *content, const char *scheme);

/* ---------- 杂项 ---------- */
/* 解析 core/cli/web 路径（含默认值兜底） */
int et_resolve_bins(et_uci *u, char **core, char **web, char **cli);
/* CPU 架构名（x86_64/aarch64/arm/armv7/mips/mipsel），未知返回 NULL */
const char *et_detect_arch(void);
/* 二进制是否为有效 EasyTier 程序（对应旧 test_binary） */
bool et_test_binary(const char *path);

#endif /* ETD_CFG_H */
