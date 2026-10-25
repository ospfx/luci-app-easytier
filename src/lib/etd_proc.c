/*
 * etd_proc.c - 进程发现/创建/回收
 *
 * 原实现用 `pgrep -f xxx`、`ps | grep | grep -v grep | awk '{print $1}' | xargs kill`
 * 这类管道完成同样的工作：依赖 busybox ps 的输出格式、每次都 fork 子进程、
 * 管道之间还可能丢 PID。这里直接扫 /proc 并用 kill(2)，语义与 pgrep -f 等价，
 * 但不依赖任何外部工具，也不会有管道丢数据的窗口。
 */
#include "etd_proc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* pid 是否还活着（EPERM 说明存在但无权限，同样算活着） */
static bool et_pid_alive(pid_t pid)
{
	if (pid <= 1)
		return false;
	if (kill(pid, 0) == 0)
		return true;
	return errno == EPERM;
}

static int et_pid_push(pid_t **v, size_t *n, size_t *cap, pid_t pid)
{
	if (*n + 1 >= *cap) {
		size_t ncap = *cap ? *cap * 2 : 8;
		pid_t *nv = realloc(*v, ncap * sizeof(pid_t));
		if (!nv)
			return ET_ENOMEM;
		*v = nv;
		*cap = ncap;
	}
	(*v)[(*n)++] = pid;
	return ET_OK;
}

/* /proc/<pid>/cmdline 中 argv 之间是 NUL，统一换成空格便于子串匹配 */
char *et_proc_cmdline(pid_t pid)
{
	if (pid <= 0)
		return NULL;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	size_t len = 0;
	char *s = et_read_file(path, &len);
	if (!s)
		return NULL;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '\0')
			s[i] = ' ';
	}
	/* 尾部空格（末尾 NUL 被替换后留下的）去掉 */
	while (len > 0 && s[len - 1] == ' ')
		s[--len] = '\0';
	return s;
}

pid_t *et_proc_find_all(const char *name, size_t *n)
{
	if (n)
		*n = 0;
	if (!name || !*name)
		return NULL;

	DIR *d = opendir("/proc");
	if (!d)
		return NULL;

	pid_t *v = NULL;
	size_t cnt = 0, cap = 0;
	pid_t self = getpid();
	struct dirent *e;

	while ((e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		pid_t pid = (pid_t)strtol(e->d_name, NULL, 10);
		if (pid <= 1 || pid == self)
			continue;
		char *cl = et_proc_cmdline(pid);
		if (!cl)
			continue;
		bool hit = strstr(cl, name) != NULL;
		free(cl);
		if (!hit)
			continue;
		if (et_pid_push(&v, &cnt, &cap, pid) < 0)
			break;
	}
	closedir(d);

	if (cnt == 0) {
		free(v);
		return NULL;
	}
	if (n)
		*n = cnt;
	return v;
}

bool et_proc_running(const char *name)
{
	size_t n = 0;
	pid_t *v = et_proc_find_all(name, &n);
	free(v);
	return n > 0;
}

pid_t et_proc_pid(const char *name)
{
	size_t n = 0;
	pid_t *v = et_proc_find_all(name, &n);
	pid_t pid = -1;
	if (v && n > 0) {
		pid = v[0];
		free(v);
	} else {
		free(v);
	}
	return pid;
}

pid_t et_proc_pid_comm(const char *comm)
{
	if (!comm || !*comm)
		return -1;
	DIR *d = opendir("/proc");
	if (!d)
		return -1;

	pid_t self = getpid();
	pid_t found = -1;
	struct dirent *e;

	while ((e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		pid_t pid = (pid_t)strtol(e->d_name, NULL, 10);
		if (pid <= 1 || pid == self)
			continue;
		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
		char *s = et_read_file(path, NULL);
		if (!s)
			continue;
		et_trim_newline(s);
		if (!strcmp(s, comm))
			found = pid;
		free(s);
		if (found > 0)
			break;
	}
	closedir(d);
	return found;
}

int et_proc_terminate(const char *name, int grace_ms)
{
	size_t n = 0;
	pid_t *v = et_proc_find_all(name, &n);
	if (!v || n == 0) {
		free(v);
		return ET_ENOENT;
	}

	for (size_t i = 0; i < n; i++)
		kill(v[i], SIGTERM);

	/* 分批等待，避免 1ms 粒度的忙等 */
	if (grace_ms < 0)
		grace_ms = 0;
	for (int waited = 0; waited < grace_ms; waited += 20) {
		bool any = false;
		for (size_t i = 0; i < n; i++) {
			if (et_pid_alive(v[i])) {
				any = true;
				break;
			}
		}
		if (!any)
			break;
		usleep(20 * 1000);
	}

	for (size_t i = 0; i < n; i++) {
		if (et_pid_alive(v[i]))
			kill(v[i], SIGKILL);
	}
	free(v);
	return ET_OK;
}

int et_proc_rss_kb(pid_t pid, long *kb)
{
	if (!kb)
		return ET_EINVAL;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	char *s = et_read_file(path, NULL);
	if (!s)
		return ET_ENOENT;

	long v = -1;
	char *save = NULL;
	for (char *line = strtok_r(s, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "VmRSS:", 6)) {
			v = strtol(line + 6, NULL, 10);
			break;
		}
	}
	free(s);
	if (v < 0)
		return ET_ENOENT;
	*kb = v;
	return ET_OK;
}

long et_proc_hz(void)
{
	long hz = sysconf(_SC_CLK_TCK);
	return hz > 0 ? hz : 100;
}

int et_proc_cpu_jiffies(pid_t pid, unsigned long long *j)
{
	if (!j)
		return ET_EINVAL;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	char *s = et_read_file(path, NULL);
	if (!s)
		return ET_ENOENT;

	/* comm 里可能有空格和括号，从最后一个 ')' 之后再切分字段 */
	char *p = strrchr(s, ')');
	if (!p) {
		free(s);
		return ET_ERR;
	}
	p++;

	unsigned long long ut = 0, st = 0;
	int field = 3; /* ')' 之后第一个字段是 state，即第 3 个字段 */
	bool got = false;
	char *save = NULL;
	for (char *tok = strtok_r(p, " \t\n", &save); tok;
	     tok = strtok_r(NULL, " \t\n", &save), field++) {
		if (field == 14)
			ut = strtoull(tok, NULL, 10);
		else if (field == 15) {
			st = strtoull(tok, NULL, 10);
			got = true;
			break;
		}
	}
	free(s);
	if (!got)
		return ET_ERR;
	*j = ut + st;
	return ET_OK;
}

int et_proc_cpu_percent(pid_t pid, int sample_ms, double *pct)
{
	if (!pct)
		return ET_EINVAL;
	if (sample_ms <= 0)
		sample_ms = 200;

	unsigned long long j1 = 0, j2 = 0;
	if (et_proc_cpu_jiffies(pid, &j1) < 0)
		return ET_ENOENT;
	usleep((useconds_t)sample_ms * 1000);
	if (et_proc_cpu_jiffies(pid, &j2) < 0)
		return ET_ENOENT;

	long hz = et_proc_hz();
	double span = (double)hz * (double)sample_ms / 1000.0;
	if (span <= 0.0)
		return ET_ERR;
	double delta = j2 >= j1 ? (double)(j2 - j1) : 0.0;
	*pct = delta * 100.0 / span;
	return ET_OK;
}

pid_t et_proc_spawn(char *const argv[], const char *cwd, const char *out_path)
{
	if (!argv || !argv[0])
		return ET_EINVAL;

	pid_t pid = fork();
	if (pid < 0)
		return ET_ERR;
	if (pid > 0)
		return pid;

	/* 子进程：脱离会话，清空信号掩码，重定向到日志 */
	setsid();
	sigset_t empty;
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);
	signal(SIGPIPE, SIG_DFL);

	if (cwd && *cwd && chdir(cwd) < 0)
		_exit(126);

	int devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		if (devnull > STDERR_FILENO)
			close(devnull);
	}

	const char *out = (out_path && *out_path) ? out_path : "/dev/null";
	int fd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}

	execvp(argv[0], argv);
	_exit(127);
}

int et_pidfile_write(const char *path, pid_t pid)
{
	if (!path)
		return ET_EINVAL;
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
	return et_write_file(path, buf, (size_t)n);
}

pid_t et_pidfile_read(const char *path)
{
	if (!path)
		return -1;
	char *s = et_read_file(path, NULL);
	if (!s)
		return -1;
	long v = 0;
	bool ok = et_parse_int(s, &v);
	free(s);
	if (!ok || v <= 1)
		return -1;
	return (pid_t)v;
}

bool et_pidfile_alive(const char *path)
{
	return et_pid_alive(et_pidfile_read(path));
}
