/*
 * etd_watchdog.h - checkip 连通性看门狗（私有头）
 *
 * 原实现把一段 shell 写到 /tmp/easytier_check 再交给 procd 跑，用来在
 * "所有 checkip 都 ping 不通" 时重启服务。这里改成 C 实现：由监督进程
 * fork 一个子进程跑循环，用信号通知监督进程，不再落任何脚本文件。
 */
#ifndef ETD_WATCHDOG_H
#define ETD_WATCHDOG_H

#include "etd.h"
#include "etd_cfg.h"

/* 是否需要看门狗：checkip 非空（与旧实现 `[ ! -z "$checkip" ]` 一致） */
bool et_watchdog_wanted(et_cfg *c);

/* 在看门狗子进程中运行；正常返回表示该退出（进程会随之结束）。
 * supervisor 为监督进程 pid，用于：全部不通 → SIGUSR1 请求重启；
 * enabled 变 0 → SIGTERM 请求停服（等价旧的 /etc/init.d/easytier restart）。 */
int et_watchdog_main(pid_t supervisor);

#endif /* ETD_WATCHDOG_H */
