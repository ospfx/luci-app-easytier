/*
 * etd_proc.h - 进程发现/创建/回收（替代原 shell 里的 pgrep/ps/kill 管道）
 */
#ifndef ETD_PROC_H
#define ETD_PROC_H

#include "etd.h"
#include "etd_util.h"

/* 扫描 /proc 找到命令行中包含 name 的进程（等价 pgrep -f）。
 * 返回 PID 数组（malloc），*n 为个数；无匹配返回 NULL。 */
pid_t *et_proc_find_all(const char *name, size_t *n);
/* 是否有进程在跑 */
bool et_proc_running(const char *name);
/* 第一个匹配的 PID，无则 -1 */
pid_t et_proc_pid(const char *name);
/* 精确匹配 /proc/<pid>/comm（用于 easytier-core 等） */
pid_t et_proc_pid_comm(const char *comm);
/* 向所有匹配进程发信号；先 TERM，超时后 KILL */
int et_proc_terminate(const char *name, int grace_ms);
/* 读 /proc/<pid>/status 的 VmRSS，单位 KB */
int et_proc_rss_kb(pid_t pid, long *kb);
/* 读 /proc/<pid>/cmdline，空格替换 NUL */
char *et_proc_cmdline(pid_t pid);
/* 读取并解析 /proc/<pid>/stat 的 utime+stime（jiffies） */
int et_proc_cpu_jiffies(pid_t pid, unsigned long long *j);
/* 系统 HZ */
long et_proc_hz(void);
/* 进程 CPU 占用百分比（两次采样差，permille 精度） */
int et_proc_cpu_percent(pid_t pid, int sample_ms, double *pct);

/* 启动子进程：cwd 可 NULL；stdout/stderr 重定向到 out_path（append）。
 * 返回 pid（>0）或 <0。 */
pid_t et_proc_spawn(char *const argv[], const char *cwd, const char *out_path);

/* 写/读 pid 文件 */
int et_pidfile_write(const char *path, pid_t pid);
pid_t et_pidfile_read(const char *path);
bool et_pidfile_alive(const char *path);

#endif /* ETD_PROC_H */
