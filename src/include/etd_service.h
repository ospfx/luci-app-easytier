/*
 * etd_service.h - 服务生命周期（对应原 init.d/easytier 的 start/stop/reload）
 */
#ifndef ETD_SERVICE_H
#define ETD_SERVICE_H

#include "etd.h"
#include "etd_cfg.h"

/* 启动 core 与 web（按 UCI enabled/web_enabled）
 * foreground=true 时前台运行并进入监督循环（供 procd / init.d 使用） */
int et_service_start(bool foreground);
/* 停止所有实例并清理防火墙/网络规则 */
int et_service_stop(void);
/* 重载：stop + start */
int et_service_restart(void);
/* 是否在运行（任一实例） */
bool et_service_running(void);
/* 开机自启开关（创建/删除 /etc/rc.d 链接） */
int et_service_enable(bool on);

/* ---- 内部：单实例 ---- */
/* 启动 easytier-core 实例，成功返回 pid */
pid_t et_instance_start_core(et_cfg *c, et_buf *notes);
/* 启动 easytier-web 实例 */
pid_t et_instance_start_web(et_cfg *c, et_buf *notes);

#endif /* ETD_SERVICE_H */
