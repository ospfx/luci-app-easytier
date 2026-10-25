/*
 * etd_util.c - 通用工具实现
 */
#include "etd_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int et_debug = 0;

/* ================= 动态缓冲 ================= */

void et_buf_init(et_buf *b)
{
	if (!b)
		return;
	b->p = NULL;
	b->len = 0;
	b->cap = 0;
}

void et_buf_free(et_buf *b)
{
	if (!b)
		return;
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

static int et_buf_reserve(et_buf *b, size_t need)
{
	if (b->len + need + 1 <= b->cap)
		return ET_OK;
	size_t cap = b->cap ? b->cap : 128;
	while (cap < b->len + need + 1)
		cap *= 2;
	char *np = realloc(b->p, cap);
	if (!np)
		return ET_ENOMEM;
	b->p = np;
	b->cap = cap;
	return ET_OK;
}

int et_buf_addn(et_buf *b, const char *s, size_t n)
{
	if (!b || !s)
		return ET_EINVAL;
	if (et_buf_reserve(b, n) < 0)
		return ET_ENOMEM;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
	return ET_OK;
}

int et_buf_add(et_buf *b, const char *s)
{
	if (!s)
		return ET_OK;
	return et_buf_addn(b, s, strlen(s));
}

int et_buf_addc(et_buf *b, char c)
{
	if (et_buf_reserve(b, 1) < 0)
		return ET_ENOMEM;
	b->p[b->len++] = c;
	b->p[b->len] = '\0';
	return ET_OK;
}

int et_buf_addf(et_buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		return ET_ERR;
	}
	if (et_buf_reserve(b, (size_t)n) < 0) {
		va_end(ap2);
		return ET_ENOMEM;
	}
	vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	b->len += (size_t)n;
	return ET_OK;
}

char *et_buf_steal(et_buf *b)
{
	char *p = b->p;
	b->p = NULL;
	b->len = b->cap = 0;
	return p;
}

int et_buf_add_json(et_buf *b, const char *s)
{
	if (!s)
		return et_buf_add(b, "\\u0000") == 0 ? ET_OK : ET_ERR;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': et_buf_add(b, "\\\""); break;
		case '\\': et_buf_add(b, "\\\\"); break;
		case '\n': et_buf_add(b, "\\n"); break;
		case '\r': et_buf_add(b, "\\r"); break;
		case '\t': et_buf_add(b, "\\t"); break;
		case '\b': et_buf_add(b, "\\b"); break;
		case '\f': et_buf_add(b, "\\f"); break;
		default:
			if (*p < 0x20)
				et_buf_addf(b, "\\u%04x", *p);
			else
				et_buf_addc(b, (char)*p);
		}
	}
	return ET_OK;
}

int et_buf_add_html(et_buf *b, const char *s)
{
	if (!s)
		return ET_OK;
	for (const char *p = s; *p; p++) {
		switch (*p) {
		case '&': et_buf_add(b, "&amp;"); break;
		case '<': et_buf_add(b, "&lt;"); break;
		case '>': et_buf_add(b, "&gt;"); break;
		case '"': et_buf_add(b, "&quot;"); break;
		case '\'': et_buf_add(b, "&#39;"); break;
		default: et_buf_addc(b, *p);
		}
	}
	return ET_OK;
}

/* ================= 字符串列表 ================= */

void et_list_init(et_list *l)
{
	l->v = NULL;
	l->n = l->cap = 0;
}

void et_list_free(et_list *l)
{
	if (!l)
		return;
	for (size_t i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = l->cap = 0;
}

int et_list_add_owned(et_list *l, char *s)
{
	if (l->n + 1 >= l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 8;
		char **nv = realloc(l->v, cap * sizeof(char *));
		if (!nv) {
			free(s);
			return ET_ENOMEM;
		}
		l->v = nv;
		l->cap = cap;
	}
	l->v[l->n++] = s;
	l->v[l->n] = NULL;
	return ET_OK;
}

int et_list_add(et_list *l, const char *s)
{
	char *d = strdup(s ? s : "");
	if (!d)
		return ET_ENOMEM;
	return et_list_add_owned(l, d);
}

int et_list_split(et_list *l, const char *s, const char *sep)
{
	if (!s || !sep || !*sep)
		return ET_OK;
	size_t sl = strlen(sep);
	const char *p = s;
	while (*p) {
		const char *hit = strstr(p, sep);
		size_t n = hit ? (size_t)(hit - p) : strlen(p);
		if (n > 0) {
			char *piece = strndup(p, n);
			if (!piece)
				return ET_ENOMEM;
			et_list_add_owned(l, piece);
		}
		if (!hit)
			break;
		p = hit + sl;
	}
	return ET_OK;
}

int et_list_split_ws(et_list *l, const char *s)
{
	if (!s)
		return ET_OK;
	const char *p = s;
	while (*p) {
		while (*p && isspace((unsigned char)*p))
			p++;
		const char *st = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (p > st) {
			char *piece = strndup(st, (size_t)(p - st));
			if (!piece)
				return ET_ENOMEM;
			et_list_add_owned(l, piece);
		}
	}
	return ET_OK;
}

const char *et_list_get(const et_list *l, size_t i)
{
	if (!l || i >= l->n)
		return NULL;
	return l->v[i];
}

/* ================= 文件 ================= */

char *et_read_file(const char *path, size_t *len_out)
{
	if (!path)
		return NULL;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	et_buf b;
	et_buf_init(&b);
	char tmp[4096];
	for (;;) {
		ssize_t r = read(fd, tmp, sizeof(tmp));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			et_buf_free(&b);
			return NULL;
		}
		if (r == 0)
			break;
		if (et_buf_addn(&b, tmp, (size_t)r) < 0) {
			close(fd);
			et_buf_free(&b);
			return NULL;
		}
	}
	close(fd);
	if (!b.p)
		et_buf_add(&b, "");
	if (len_out)
		*len_out = b.len;
	return et_buf_steal(&b);
}

static int et_write_fd_all(int fd, const char *data, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return ET_ERR;
		}
		off += (size_t)w;
	}
	return ET_OK;
}

int et_write_file(const char *path, const char *data, size_t len)
{
	if (!path)
		return ET_EINVAL;
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return ET_ERR;
	if (et_write_fd_all(fd, data, len) < 0) {
		close(fd);
		unlink(tmp);
		return ET_ERR;
	}
	fsync(fd);
	close(fd);
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return ET_ERR;
	}
	return ET_OK;
}

int et_append_file(const char *path, const char *data, size_t len)
{
	if (!path || !data)
		return ET_EINVAL;
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return ET_ERR;
	int rc = et_write_fd_all(fd, data, len);
	close(fd);
	return rc;
}

int et_file_exists(const char *path)
{
	struct stat st;
	if (!path)
		return 0;
	return stat(path, &st) == 0;
}

int et_file_executable(const char *path)
{
	return path && access(path, X_OK) == 0;
}

int et_is_dir(const char *path)
{
	struct stat st;
	if (!path)
		return 0;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int et_mkdir_p(const char *path, mode_t mode)
{
	if (!path || !*path)
		return ET_EINVAL;
	char tmp[512];
	size_t n = strlen(path);
	if (n >= sizeof(tmp))
		return ET_EINVAL;
	memcpy(tmp, path, n + 1);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
			*p = '/';
			return ET_ERR;
		}
		*p = '/';
	}
	if (mkdir(tmp, mode) < 0 && errno != EEXIST)
		return ET_ERR;
	return ET_OK;
}

int et_rm_rf(const char *path)
{
	if (!path || !*path)
		return ET_EINVAL;
	struct stat st;
	if (lstat(path, &st) < 0)
		return ET_ENOENT;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d))) {
				if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
					continue;
				char sub[1024];
				snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
				et_rm_rf(sub);
			}
			closedir(d);
		}
		return rmdir(path) == 0 ? ET_OK : ET_ERR;
	}
	return unlink(path) == 0 ? ET_OK : ET_ERR;
}

int et_unlink(const char *path)
{
	if (!path)
		return ET_EINVAL;
	if (unlink(path) == 0)
		return ET_OK;
	return errno == ENOENT ? ET_ENOENT : ET_ERR;
}

long long et_file_size(const char *path)
{
	struct stat st;
	if (!path || stat(path, &st) < 0)
		return -1;
	return (long long)st.st_size;
}

long long et_df_available_kb(const char *path)
{
	struct statvfs vfs;
	if (!path || statvfs(path, &vfs) < 0)
		return -1;
	return (long long)vfs.f_bavail * (long long)vfs.f_frsize / 1024;
}

int et_human_size(long long bytes, char *out, size_t n)
{
	static const char *unit[] = { "B", "K", "M", "G", "T" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		return snprintf(out, n, "%lld%s", bytes, unit[u]);
	return snprintf(out, n, "%.1f%s", v, unit[u]);
}

int et_copy_file(const char *src, const char *dst, bool set_exec)
{
	size_t len = 0;
	char *data = et_read_file(src, &len);
	if (!data)
		return ET_ENOENT;
	int rc = et_write_file(dst, data, len);
	free(data);
	if (rc < 0)
		return rc;
	return chmod(dst, set_exec ? 0755 : 0644) == 0 ? ET_OK : ET_ERR;
}

/* ================= 字符串 ================= */

int et_trim_newline(char *s)
{
	if (!s)
		return 0;
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
			 s[n - 1] == '\t'))
		s[--n] = '\0';
	return (int)n;
}

int et_is_blank(const char *s)
{
	if (!s)
		return 1;
	for (const char *p = s; *p; p++)
		if (!isspace((unsigned char)*p))
			return 0;
	return 1;
}

char *et_strip_ansi(const char *s)
{
	if (!s)
		return NULL;
	et_buf b;
	et_buf_init(&b);
	for (const char *p = s; *p; p++) {
		if (*p == 0x1b && p[1] == '[') {
			p += 2;
			while (*p && !(*p >= '@' && *p <= '~'))
				p++;
			if (!*p)
				break;
			continue;
		}
		et_buf_addc(&b, *p);
	}
	if (!b.p)
		et_buf_add(&b, "");
	return et_buf_steal(&b);
}

char *et_shell_quote(const char *s)
{
	et_buf b;
	et_buf_init(&b);
	et_buf_addc(&b, '\'');
	for (const char *p = s ? s : ""; *p; p++) {
		if (*p == '\'')
			et_buf_add(&b, "'\\''");
		else
			et_buf_addc(&b, *p);
	}
	et_buf_addc(&b, '\'');
	return et_buf_steal(&b);
}

bool et_parse_int(const char *s, long *out)
{
	if (!s || !*s)
		return false;
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno != 0 || !end)
		return false;
	while (*end && isspace((unsigned char)*end))
		end++;
	if (*end)
		return false;
	if (out)
		*out = v;
	return true;
}

bool et_starts_with(const char *s, const char *prefix)
{
	if (!s || !prefix)
		return false;
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool et_ends_with(const char *s, const char *suffix)
{
	if (!s || !suffix)
		return false;
	size_t ls = strlen(s), lf = strlen(suffix);
	return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

char *et_dirname_keep_slash(const char *path)
{
	if (!path)
		return NULL;
	const char *last = strrchr(path, '/');
	if (!last)
		return NULL;
	size_t n = (size_t)(last - path) + 1;
	return strndup(path, n);
}

int et_format_uptime(long seconds, char *out, size_t n)
{
	if (seconds < 0)
		seconds = 0;
	long days = seconds / 86400;
	long hours = (seconds % 86400) / 3600;
	long mins = (seconds % 3600) / 60;
	long secs = seconds % 60;
	if (days > 0)
		return snprintf(out, n, "%ld天 %02ld小时%02ld分%02ld秒", days, hours,
				mins, secs);
	return snprintf(out, n, "%02ld小时%02ld分%02ld秒", hours, mins, secs);
}

char *et_strdup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

/* ================= 执行外部命令 ================= */

static void et_close_from(int lowfd)
{
	long maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd < 0)
		maxfd = 1024;
	for (int fd = lowfd; fd < maxfd; fd++)
		close(fd);
}

int et_run(char *const argv[], et_buf *out, et_buf *err, int timeout_sec)
{
	if (!argv || !argv[0])
		return ET_EINVAL;

	int po[2] = { -1, -1 }, pe[2] = { -1, -1 };
	if (out && pipe(po) < 0)
		return ET_ERR;
	if (err && pipe(pe) < 0) {
		if (po[0] >= 0) {
			close(po[0]);
			close(po[1]);
		}
		return ET_ERR;
	}

	pid_t pid = fork();
	if (pid < 0) {
		if (po[0] >= 0) {
			close(po[0]);
			close(po[1]);
		}
		if (pe[0] >= 0) {
			close(pe[0]);
			close(pe[1]);
		}
		return ET_ERR;
	}
	if (pid == 0) {
		if (out) {
			dup2(po[1], STDOUT_FILENO);
			close(po[0]);
			close(po[1]);
		}
		if (err) {
			dup2(pe[1], STDERR_FILENO);
			close(pe[0]);
			close(pe[1]);
		}
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}
		et_close_from(3);
		execvp(argv[0], argv);
		_exit(127);
	}

	if (out) {
		close(po[1]);
		po[1] = -1;
	}
	if (err) {
		close(pe[1]);
		pe[1] = -1;
	}

	struct pollfd pfd[2];
	int nfd = 0;
	if (out)
		pfd[nfd++] = (struct pollfd){ .fd = po[0], .events = POLLIN };
	if (err)
		pfd[nfd++] = (struct pollfd){ .fd = pe[0], .events = POLLIN };

	time_t deadline = timeout_sec > 0 ? time(NULL) + timeout_sec : 0;
	int open_fds = nfd;
	char tmp[4096];
	while (open_fds > 0) {
		int wait_ms = -1;
		if (deadline) {
			long left = (long)(deadline - time(NULL));
			if (left <= 0) {
				kill(pid, SIGKILL);
				break;
			}
			wait_ms = (int)(left * 1000);
		}
		int rc = poll(pfd, (nfds_t)nfd, wait_ms);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc == 0) {
			kill(pid, SIGKILL);
			break;
		}
		for (int i = 0; i < nfd; i++) {
			if (pfd[i].fd < 0 || !(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			ssize_t r = read(pfd[i].fd, tmp, sizeof(tmp));
			if (r > 0) {
				et_buf *dst = (out && pfd[i].fd == po[0]) ? out : err;
				if (dst)
					et_buf_addn(dst, tmp, (size_t)r);
			} else if (r == 0) {
				close(pfd[i].fd);
				pfd[i].fd = -1;
				open_fds--;
			} else if (errno != EINTR && errno != EAGAIN) {
				close(pfd[i].fd);
				pfd[i].fd = -1;
				open_fds--;
			}
		}
	}
	if (out && po[0] >= 0)
		close(po[0]);
	if (err && pe[0] >= 0)
		close(pe[0]);
	if (out && !out->p)
		et_buf_add(out, "");
	if (err && !err->p)
		et_buf_add(err, "");

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return ET_ERR;
}

char *et_run_capture(char *const argv[], int *status)
{
	et_buf o;
	et_buf_init(&o);
	int rc = et_run(argv, &o, NULL, 30);
	if (status)
		*status = rc;
	if (!o.p)
		return NULL;
	et_trim_newline(o.p);
	return et_buf_steal(&o);
}

int et_run_shell(const char *cmd, et_buf *out, int timeout_sec)
{
	char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)cmd, NULL };
	return et_run(argv, out, NULL, timeout_sec);
}

bool et_have_cmd(const char *name)
{
	if (!name || !*name)
		return false;
	const char *path = getenv("PATH");
	if (!path)
		path = "/usr/sbin:/usr/bin:/sbin:/bin";
	et_list l;
	et_list_init(&l);
	et_list_split(&l, path, ":");
	bool found = false;
	for (size_t i = 0; i < l.n && !found; i++) {
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", l.v[i], name);
		if (access(full, X_OK) == 0)
			found = true;
	}
	et_list_free(&l);
	return found;
}

void et_now_str(char *out, size_t n)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

/* ================= 日志 ================= */

void et_log_to(const char *file, const char *fmt, ...)
{
	if (!file)
		return;
	char ts[32];
	et_now_str(ts, sizeof(ts));

	et_buf b;
	et_buf_init(&b);
	et_buf_addf(&b, "%s easytier : ", ts);

	va_list ap;
	va_start(ap, fmt);
	char msg[4096];
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	et_buf_add(&b, msg);
	et_buf_addc(&b, '\n');

	et_append_file(file, b.p ? b.p : "", b.len);
	et_buf_free(&b);
}

/* 统计并保留文件最后 keep 行，原地重写 */
int et_log_rotate(const char *file, int max_kb, int keep_lines)
{
	long long size = et_file_size(file);
	if (size < 0 || size <= (long long)max_kb * 1024)
		return ET_OK;

	size_t len = 0;
	char *data = et_read_file(file, &len);
	if (!data)
		return ET_ERR;

	/* 从尾部回溯找第 keep_lines 个换行 */
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

	char *tmp = NULL;
	if (asprintf(&tmp, "%s.tmp", file) < 0) {
		free(data);
		return ET_ENOMEM;
	}
	int rc = et_write_file(tmp, data + pos, keep);
	free(data);
	if (rc == 0)
		rc = rename(tmp, file) == 0 ? ET_OK : ET_ERR;
	if (rc != ET_OK)
		unlink(tmp);
	free(tmp);
	return rc;
}
