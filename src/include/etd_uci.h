/*
 * etd_uci.h - libuci 封装
 *
 * 只暴露 easytier 包所需的操作集合，屏蔽 libuci 细节。
 * 所有 get/set 均作用于 @easytier[0]（匿名 section），与原实现的
 * `uci -q get easytier.@easytier[0].xxx` 语义一致。
 */
#ifndef ETD_UCI_H
#define ETD_UCI_H

#include "etd.h"
#include "etd_util.h"

typedef struct et_uci et_uci;

/* 打开游标；配置文件不存在时返回 NULL */
et_uci *et_uci_open(void);
void et_uci_close(et_uci *u);

/* 读单值。不存在返回 NULL（需 free）。
 * list 类型多值以空格连接，与 `uci -q get` 保持一致。 */
char *et_uci_get(et_uci *u, const char *pkg, const char *sec, const char *opt);
/* 读 list（保序、保留元素内部的空格）。*n 可为 NULL */
char **et_uci_get_list(et_uci *u, const char *pkg, const char *sec,
		       const char *opt, size_t *n);
/* 便捷包装：easytier 包 / @easytier[0] 节 */
char *et_uci_get_et(et_uci *u, const char *opt);
char **et_uci_get_et_list(et_uci *u, const char *opt, size_t *n);

/* 写单值；失败返回 <0 */
int et_uci_set(et_uci *u, const char *pkg, const char *sec, const char *opt,
	       const char *val);
/* 写 list（先清空再逐项写） */
int et_uci_set_list(et_uci *u, const char *pkg, const char *sec,
		    const char *opt, const char *const *vals, size_t n);
int et_uci_delete(et_uci *u, const char *pkg, const char *sec, const char *opt);
/* 删除整个 section */
int et_uci_delete_sec(et_uci *u, const char *pkg, const char *sec);
/* 批量提交（committed=true 才落盘） */
int et_uci_commit(et_uci *u, const char *pkg);
/* 是否有未提交改动 */
bool et_uci_changed(et_uci *u, const char *pkg);
/* 便捷：easytier 包 */
int et_uci_set_et(et_uci *u, const char *opt, const char *val);
int et_uci_del_et(et_uci *u, const char *opt);
int et_uci_commit_et(et_uci *u);

/* 通用：设定 section 类型（不存在则创建），如 firewall 的 rule/zone/forwarding */
int et_uci_ensure_sec(et_uci *u, const char *pkg, const char *sec,
		      const char *type);

/* 读取原始配置文件行（兼容遗留写法，如 grep "list xxx"） */
char *et_uci_read_raw(void);

#endif /* ETD_UCI_H */
