/*
 * etd_detect.c - CPU 架构探测、二进制有效性校验、程序路径解析
 *
 * 与旧实现的对应关系：
 *   et_detect_arch()  <- start_service() 里 uname -ms 的 cpucore 推导（init.d:914-923）
 *   et_test_binary()  <- controller/easytier.lua 的 test_binary(path)
 *                        （`path -h 2>&1` 行数 >= 3 且输出含 easytier）
 *   et_resolve_bins() <- easytierbin / webbin 的默认值兜底 + easytier-cli 同目录推导
 */
#include "etd_cfg.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>

static bool contains_ci(const char *hay, const char *needle)
{
	size_t n;

	if (!hay || !needle || !*needle)
		return false;
	n = strlen(needle);
	for (const char *p = hay; *p; p++) {
		if (strncasecmp(p, needle, n) == 0)
			return true;
	}
	return false;
}

static bool file_contains(const char *path, const char *needle)
{
	char *data = et_read_file(path, NULL);
	bool hit;

	if (!data)
		return false;
	hit = contains_ci(data, needle);
	free(data);
	return hit;
}

/* 小端：mipsel；大端：mips（旧实现用 hexdump 判断字节序，语义等价） */
static bool host_is_le(void)
{
	uint16_t x = 1;

	return *(const uint8_t *)&x == 1;
}

const char *et_detect_arch(void)
{
	static char arch[16];
	struct utsname u;
	char buf[256];
	const char *cpucore = "";

	if (uname(&u) != 0)
		return NULL;
	snprintf(buf, sizeof(buf), "%s_%s", u.sysname, u.machine);
	for (char *p = buf; *p; p++)
		*p = (char)tolower((unsigned char)*p);

	if (strstr(buf, "linux")) {
		/* 顺序与旧实现一致：后面的判断会覆盖前面的 */
		if (strstr(buf, "armv"))
			cpucore = "arm";
		if (strstr(buf, "armv7") && file_contains("/proc/cpuinfo", "vfp"))
			cpucore = "armv7";
		if (strstr(buf, "aarch64") || strstr(buf, "armv8"))
			cpucore = "aarch64";
		if (strstr(buf, "86"))
			cpucore = "i386";
		if (strstr(buf, "86_64"))
			cpucore = "x86_64";
		if (strstr(buf, "mips"))
			cpucore = host_is_le() ? "mipsel" : "mips";
	}

	if (!*cpucore)
		return NULL;
	snprintf(arch, sizeof(arch), "%s", cpucore);
	return arch;
}

/* 统计非空行数，与 Lua 的 output:gmatch("[^\r\n]+") 计数一致 */
static size_t count_lines(const char *s)
{
	size_t n = 0;
	bool in_line = false;

	if (!s)
		return 0;
	for (const char *p = s; *p; p++) {
		if (*p == '\n' || *p == '\r') {
			in_line = false;
		} else if (!in_line) {
			in_line = true;
			n++;
		}
	}
	return n;
}

bool et_test_binary(const char *path)
{
	char *argv[3];
	et_buf out, err;
	size_t lines;
	bool ok;

	if (!path || !*path)
		return false;
	if (!et_file_executable(path))
		return false;

	argv[0] = (char *)path;
	argv[1] = (char *)"-h";
	argv[2] = NULL;

	et_buf_init(&out);
	et_buf_init(&err);
	/* 对应 `path -h 2>&1`：两路输出都要算进行数与关键字匹配 */
	et_run(argv, &out, &err, 10);

	lines = count_lines(out.p) + count_lines(err.p);
	ok = lines >= 3 && (contains_ci(out.p, "easytier") || contains_ci(err.p, "easytier"));

	et_buf_free(&out);
	et_buf_free(&err);
	return ok;
}

int et_resolve_bins(et_uci *u, char **core, char **web, char **cli)
{
	char *c = u ? et_uci_get_et(u, "easytierbin") : NULL;
	char *w = u ? et_uci_get_et(u, "webbin") : NULL;
	char *cl = NULL;

	if (!c || !*c) {
		free(c);
		c = strdup(ET_PATH_CORE_DEF);
		if (!c)
			return ET_ENOMEM;
	}
	if (!w || !*w) {
		free(w);
		w = strdup(ET_PATH_WEB_DEF);
		if (!w) {
			free(c);
			return ET_ENOMEM;
		}
	}

	/* easytier-cli 与 easytier-core 同目录；文件名恰为 easytier-core 时直接替换后缀
	 * （与 controller 的 gsub("easytier%-core$", "easytier-cli") 一致） */
	if (et_ends_with(c, "easytier-core")) {
		size_t prefix = strlen(c) - strlen("easytier-core");

		if (asprintf(&cl, "%.*seasytier-cli", (int)prefix, c) < 0)
			cl = NULL;
	} else {
		char *dir = et_dirname_keep_slash(c);

		if (asprintf(&cl, "%seasytier-cli", dir ? dir : "/usr/bin/") < 0)
			cl = NULL;
		free(dir);
	}
	if (!cl) {
		free(c);
		free(w);
		return ET_ENOMEM;
	}

	if (core)
		*core = c;
	else
		free(c);
	if (web)
		*web = w;
	else
		free(w);
	if (cli)
		*cli = cl;
	else
		free(cl);
	return ET_OK;
}
