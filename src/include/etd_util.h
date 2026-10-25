/*
 * etd_util.h - 通用工具：动态字符串、文件、进程执行、字符串处理
 */
#ifndef ETD_UTIL_H
#define ETD_UTIL_H

#include "etd.h"
#include <stdarg.h>
#include <time.h>

/* ---------- 动态字符串缓冲 ---------- */
typedef struct {
	char *p;
	size_t len;
	size_t cap;
} et_buf;

void et_buf_init(et_buf *b);
void et_buf_free(et_buf *b);
int et_buf_add(et_buf *b, const char *s);
int et_buf_addn(et_buf *b, const char *s, size_t n);
int et_buf_addc(et_buf *b, char c);
int et_buf_addf(et_buf *b, const char *fmt, ...);
/* 交出所有权，返回 malloc 字符串（buf 被重置） */
char *et_buf_steal(et_buf *b);
/* JSON 字符串转义（含引号，不含外层引号） */
int et_buf_add_json(et_buf *b, const char *s);
/* HTML 转义 */
int et_buf_add_html(et_buf *b, const char *s);

/* ---------- 字符串列表 ---------- */
typedef struct {
	char **v;
	size_t n;
	size_t cap;
} et_list;

void et_list_init(et_list *l);
void et_list_free(et_list *l);
int et_list_add(et_list *l, const char *s);
int et_list_add_owned(et_list *l, char *s);
/* 用分隔符切分（不保留空段） */
int et_list_split(et_list *l, const char *s, const char *sep);
/* 用空白切分 */
int et_list_split_ws(et_list *l, const char *s);
const char *et_list_get(const et_list *l, size_t i);

/* ---------- 文件 ---------- */
/* 读整个文件；返回 malloc 字符串（NUL 结尾）。文件不存在返回 NULL */
char *et_read_file(const char *path, size_t *len_out);
/* 原子写入（tmp + rename）。append=1 时追加 */
int et_write_file(const char *path, const char *data, size_t len);
int et_append_file(const char *path, const char *data, size_t len);
int et_file_exists(const char *path);
int et_file_executable(const char *path);
/* 目录遍历工具：返回 p 是否为目录 */
int et_is_dir(const char *path);
int et_mkdir_p(const char *path, mode_t mode);
int et_rm_rf(const char *path);
int et_unlink(const char *path);
/* 文件大小，失败返回 -1 */
long long et_file_size(const char *path);
/* 目录可用空间（KB），失败返回 -1 */
long long et_df_available_kb(const char *path);
/* 人类可读大小，如 "1.2M"（写入 out） */
int et_human_size(long long bytes, char *out, size_t n);
/* 复制文件（保留权限位），src->dst */
int et_copy_file(const char *src, const char *dst, bool set_exec);

/* ---------- 字符串 ---------- */
int et_trim_newline(char *s);
int et_is_blank(const char *s);
/* 去 ANSI 转义序列，返回 malloc 字符串 */
char *et_strip_ansi(const char *s);
/* 单引号安全引用，返回 malloc 字符串；NULL 返回 "''" */
char *et_shell_quote(const char *s);
/* 严格解析十进制整数，成功返回 true */
bool et_parse_int(const char *s, long *out);
/* 字符串以 prefix 开头 */
bool et_starts_with(const char *s, const char *prefix);
bool et_ends_with(const char *s, const char *suffix);
/* 取目录部分（含末尾斜杠），如 "/usr/bin/x" -> "/usr/bin/"；无斜杠返回 NULL */
char *et_dirname_keep_slash(const char *path);
/* 秒数格式化为 "3天 04小时05分06秒"（与原 calc_uptime 一致） */
int et_format_uptime(long seconds, char *out, size_t n);
char *et_strdup_or_null(const char *s);

/* ---------- 执行外部命令 ---------- */
/* 用 fork/execvp 执行，收集 stdout/stderr；返回退出码，出错返回 -1。
 * timeout_sec > 0 时超时杀进程。out/err 可为 NULL。 */
int et_run(char *const argv[], et_buf *out, et_buf *err, int timeout_sec);
/* 便捷：只取 stdout，成功时返回 malloc 字符串（去尾换行） */
char *et_run_capture(char *const argv[], int *status);
/* 通过 /bin/sh -c 执行（仅用于必须管道的场景，调用方负责转义） */
int et_run_shell(const char *cmd, et_buf *out, int timeout_sec);
/* 命令是否存在 */
bool et_have_cmd(const char *name);

/* ---------- 时间 ---------- */
/* "YYYY-mm-dd HH:MM:SS" */
void et_now_str(char *out, size_t n);

/* ---------- 日志（追加到指定日志文件，格式与原 shell 一致） ---------- */
void et_log_to(const char *file, const char *fmt, ...);
int et_log_rotate(const char *file, int max_kb, int keep_lines);

#endif /* ETD_UTIL_H */
