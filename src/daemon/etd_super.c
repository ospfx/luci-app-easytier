/*
 * etd_super.c - 前台监督循环
 *
 * 对应原实现里 procd 的那一半职责（init.d 脚本只管把进程交给 procd）：
 *   - 拉起 easytier-core / easytier-web 并记录 pid；
 *   - respawn threshold=3600s / timeout=5s / retry=20：同一个 1 小时窗口内
 *     崩溃超过 20 次（第 21 次）就放弃拉起，存活满 1 小时则计数归零；
 *   - 每 60 秒检查日志，超过 5120KB 保留最后 500 行；
 *   - checkip 看门狗作为子进程运行；
 *   - SIGTERM/SIGINT 优雅停止（先 TERM，5 秒后 KILL），SIGUSR1/SIGHUP 整体重启。
 */
#include "etd_super.h"
#include "etd_proc.h"
#include "etd_service.h"
#include "etd_util.h"
#include "etd_watchdog.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct et_slot {
	const char *name;
	const char *log;
	bool web;
	pid_t pid;
	time_t start_at;
	time_t next_at;
	int fail;
	bool giveup;
	bool wanted;
};

struct et_sup {
	et_cfg *cfg;
	pid_t self;
	struct et_slot slots[2];
	pid_t wd;
	time_t wd_next;
	bool wd_wanted;
	time_t next_rotate;
};

/* 原地截断：保留最后 keep_lines 行。
 *
 * 不能用 et_log_rotate()：它是 tmp + rename，正在写日志的 core/web 进程持有的
 * fd 会指向被删掉的旧 inode，rename 之后日志就再也不增长了。原实现
 * （commit 87efe0f "rotate EasyTier logs in place"）正是把 rotate 改成原地截断，
 * 这里保持同样的语义。 */
static void et_rotate_inplace(const char *file, int max_kb, int keep_lines)
{
	long long size = et_file_size(file);
	if (size < 0 || size <= (long long)max_kb * 1024)
		return;

	size_t len = 0;
	char *data = et_read_file(file, &len);
	if (!data)
		return;

	size_t pos = len;
	int lines = 0;
	while (pos > 0 && lines <= keep_lines) {
		pos--;
		if (data[pos] == '\n')
			lines++;
	}
	if (lines <= keep_lines)
		pos = 0;
	else
		pos++;

	size_t keep = len - pos;
	int fd = open(file, O_WRONLY | O_TRUNC);
	if (fd >= 0) {
		size_t off = 0;
		while (off < keep) {
			ssize_t w = write(fd, data + pos + off, keep - off);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			off += (size_t)w;
		}
		close(fd);
	}
	free(data);
}

static void et_slot_reason(int status, char *out, size_t n)
{
	if (WIFSIGNALED(status))
		snprintf(out, n, "被信号 %d 终止", WTERMSIG(status));
	else if (WIFEXITED(status))
		snprintf(out, n, "退出码 %d", WEXITSTATUS(status));
	else
		snprintf(out, n, "状态未知");
}

/* 子进程退出后的 respawn 判定（procd threshold/timeout/retry 语义） */
static void et_slot_died(struct et_slot *sl, int status)
{
	time_t now = time(NULL);
	long up = (long)(now - sl->start_at);
	char reason[64];

	sl->pid = 0;
	et_slot_reason(status, reason, sizeof(reason));

	/* 存活满 threshold（1 小时）→ 崩溃计数归零 */
	if (up >= ET_RESPAWN_THRESHOLD) {
		if (sl->fail > 0)
			et_log_to(sl->log, "%s 已稳定运行 %ld 秒，崩溃计数归零",
				  sl->name, up);
		sl->fail = 0;
	}

	sl->fail++;
	if (sl->fail > ET_RESPAWN_RETRY) {
		sl->giveup = true;
		et_log_to(sl->log,
			  "%s 连续异常退出 %d 次（%s），已放弃拉起；请检查配置或日志后手动重启服务",
			  sl->name, sl->fail, reason);
		return;
	}

	sl->next_at = now + ET_RESPAWN_TIMEOUT;
	et_log_to(sl->log, "%s 异常退出（%s，已运行 %ld 秒），%d 秒后第 %d 次重启",
		  sl->name, reason, up, ET_RESPAWN_TIMEOUT, sl->fail);
}

static void et_sup_reap(struct et_sup *s, bool shutdown)
{
	for (;;) {
		int status = 0;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if (pid <= 0)
			return;

		if (s->wd == pid) {
			s->wd = 0;
			s->wd_next = time(NULL) + ET_RESPAWN_TIMEOUT;
			if (!shutdown && s->wd_wanted)
				et_log_to(ET_LOG_CORE,
					  "通断检测进程已退出，%d 秒后重新拉起",
					  ET_RESPAWN_TIMEOUT);
			continue;
		}

		for (size_t i = 0; i < sizeof(s->slots) / sizeof(s->slots[0]); i++) {
			struct et_slot *sl = &s->slots[i];
			if (sl->pid != pid)
				continue;
			if (shutdown) {
				sl->pid = 0;
				break;
			}
			et_slot_died(sl, status);
			break;
		}
	}
}

static void et_sup_spawn(struct et_sup *s, struct et_slot *sl)
{
	time_t now = time(NULL);
	pid_t pid = sl->web ? et_instance_start_web(s->cfg, NULL)
			    : et_instance_start_core(s->cfg, NULL);
	if (pid > 0) {
		sl->pid = pid;
		sl->start_at = now;
		return;
	}

	/* fork 失败等：按失败计数，避免无限快速重试 */
	sl->fail++;
	if (sl->fail > ET_RESPAWN_RETRY) {
		sl->giveup = true;
		et_log_to(sl->log, "%s 启动失败 %d 次，已放弃拉起", sl->name,
			  sl->fail);
		return;
	}
	sl->next_at = now + ET_RESPAWN_TIMEOUT;
}

static void et_sup_watchdog(struct et_sup *s)
{
	time_t now = time(NULL);

	if (!s->wd_wanted) {
		if (s->wd > 0)
			kill(s->wd, SIGTERM);
		return;
	}
	if (s->wd > 0 || now < s->wd_next)
		return;

	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		/* 子进程不能继承被阻塞的信号集 */
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, NULL);
		et_watchdog_main(s->self);
		_exit(0);
	}
	s->wd = pid;
}

static void et_sup_maintain(struct et_sup *s)
{
	time_t now = time(NULL);

	for (size_t i = 0; i < sizeof(s->slots) / sizeof(s->slots[0]); i++) {
		struct et_slot *sl = &s->slots[i];
		if (!sl->wanted || sl->pid > 0 || sl->giveup)
			continue;
		if (now < sl->next_at)
			continue;
		et_sup_spawn(s, sl);
	}

	et_sup_watchdog(s);

	if (now >= s->next_rotate) {
		et_rotate_inplace(ET_LOG_CORE, ET_LOG_MAX_KB, ET_LOG_KEEP_LINES);
		et_rotate_inplace(ET_LOG_WEB, ET_LOG_MAX_KB, ET_LOG_KEEP_LINES);
		s->next_rotate = now + ET_LOG_ROTATE_SEC;
	}
}

static bool et_sup_alive(const struct et_sup *s)
{
	if (s->wd > 0)
		return true;
	for (size_t i = 0; i < sizeof(s->slots) / sizeof(s->slots[0]); i++) {
		if (s->slots[i].pid > 0)
			return true;
	}
	return false;
}

/* 优雅停止：先 SIGTERM，等 timeout（5 秒，与 procd 一致），再 SIGKILL */
static void et_sup_shutdown(struct et_sup *s)
{
	for (size_t i = 0; i < sizeof(s->slots) / sizeof(s->slots[0]); i++) {
		struct et_slot *sl = &s->slots[i];
		sl->wanted = false;
		sl->giveup = true;
		if (sl->pid > 0)
			kill(sl->pid, SIGTERM);
	}
	s->wd_wanted = false;
	if (s->wd > 0)
		kill(s->wd, SIGTERM);

	time_t deadline = time(NULL) + ET_RESPAWN_TIMEOUT;
	while (et_sup_alive(s) && time(NULL) < deadline) {
		et_sup_reap(s, true);
		if (!et_sup_alive(s))
			break;
		usleep(100 * 1000);
	}

	for (size_t i = 0; i < sizeof(s->slots) / sizeof(s->slots[0]); i++) {
		struct et_slot *sl = &s->slots[i];
		if (sl->pid > 0)
			kill(sl->pid, SIGKILL);
	}
	if (s->wd > 0)
		kill(s->wd, SIGKILL);

	/* 收尸，别把僵尸留给 init */
	for (int i = 0; i < 20 && et_sup_alive(s); i++) {
		et_sup_reap(s, true);
		usleep(50 * 1000);
	}
}

int et_super_run(et_cfg *c, pid_t core_pid, pid_t web_pid)
{
	if (!c)
		return ET_SUP_STOPPED;

	struct et_sup s;
	memset(&s, 0, sizeof(s));
	s.cfg = c;
	s.self = getpid();
	s.next_rotate = time(NULL) + ET_LOG_ROTATE_SEC;
	s.wd_wanted = et_watchdog_wanted(c);

	s.slots[0].name = "easytier-core";
	s.slots[0].log = ET_LOG_CORE;
	s.slots[0].web = false;
	s.slots[0].pid = core_pid;
	s.slots[0].start_at = time(NULL);
	s.slots[0].wanted = core_pid > 0;

	s.slots[1].name = "easytier-web";
	s.slots[1].log = ET_LOG_WEB;
	s.slots[1].web = true;
	s.slots[1].pid = web_pid;
	s.slots[1].start_at = time(NULL);
	s.slots[1].wanted = web_pid > 0;

	/* 阻塞相关信号，用 sigtimedwait 同步取信号：
	 * 这样 waitpid 不会被信号处理函数打断，也不存在 handler 与主循环的竞态。 */
	sigset_t set, old;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, &old);

	int result = ET_SUP_STOPPED;
	for (;;) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
		int sig = sigtimedwait(&set, NULL, &ts);

		if (sig == SIGTERM || sig == SIGINT) {
			result = ET_SUP_STOPPED;
			break;
		}
		if (sig == SIGUSR1 || sig == SIGHUP) {
			result = ET_SUP_RESTART;
			break;
		}

		et_sup_reap(&s, false);
		et_sup_maintain(&s);
	}

	et_sup_shutdown(&s);
	sigprocmask(SIG_SETMASK, &old, NULL);
	return result;
}
