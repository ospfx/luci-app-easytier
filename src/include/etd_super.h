/*
 * etd_super.h - 前台监督循环（私有头）
 *
 * 替代原实现里 procd 承担的职责：拉起 core/web、按 respawn 策略重启、
 * 60 秒日志轮转、checkip 看门狗、信号处理。procd 参数 threshold=3600、
 * timeout=5、retry=20 在 etd.h 里以 ET_RESPAWN_* 常量给出。
 */
#ifndef ETD_SUPER_H
#define ETD_SUPER_H

#include "etd.h"
#include "etd_cfg.h"

enum {
	ET_SUP_STOPPED = 0, /* SIGTERM/SIGINT：正常停止 */
	ET_SUP_RESTART = 1, /* SIGUSR1/SIGHUP 或看门狗请求：需要整体重启 */
};

/* 进入监督循环；core_pid/web_pid 为 0 表示该实例不参与监督。
 * 返回 ET_SUP_STOPPED 或 ET_SUP_RESTART（由调用方决定是否重新走一遍启动流程）。 */
int et_super_run(et_cfg *c, pid_t core_pid, pid_t web_pid);

#endif /* ETD_SUPER_H */
