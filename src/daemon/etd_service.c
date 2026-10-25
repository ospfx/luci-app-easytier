/*
 * etd_service.c - 服务生命周期：start / run / stop / restart / status / enable
 *
 * 与原实现 init.d/easytier 的对应关系：
 *   start_service()  → et_service_start(false)：后台拉起监督进程（等价 procd 收编）
 *   start_service()  → et_service_start(true) ：前台直接进监督循环（等价 procd 本体）
 *   start_et()       → et_start_et()：二进制自愈/下载、清理残留、config 校验、防火墙
 *   start_web()      → et_start_web()
 *   stop_service()   → et_service_stop() + et_teardown()
 *   60 秒日志轮转、respawn 3600/5/20、信号级联  → etd_super.c
 *
 * 关键差异（都是"不再需要 shell"带来的）：原实现用 /bin/sh -c 包一层做日志重定向
 * 与 trap 级联 kill，这里直接 fork/exec + dup2，进程层级从 sh→core 变成 core 本身，
 * 少一层进程、信号与退出码也不再被 sh 吃掉。
 */
#include "etd_service.h"
#include "etd_api.h"
#include "etd_net.h"
#include "etd_proc.h"
#include "etd_super.h"
#include "etd_util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define ET_SPACE_MIN_KB 5000   /* 低于此可用空间改用 /tmp/easytier-core */
#define ET_DL_WAIT_SEC 300     /* 触发下载后最多等待的秒数 */
#define ET_STOP_WAIT_MS 8000   /* stop 时等监督进程退出的上限 */

static const char *cfg_or(et_cfg *c, const char *name, const char *def)
{
	const char *v = et_cfg_str(c, name);
	return (v && *v) ? v : def;
}

static bool has_val(const char *s)
{
	return s && *s;
}

/* ---------------- 小工具 ---------------- */

static void et_truncate_log(const char *path)
{
	et_write_file(path, "", 0);
}

/* echo `date +%s` > /tmp/easytier_time */
static void et_write_ts(const char *path)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%lld\n", (long long)time(NULL));
	et_write_file(path, buf, (size_t)n);
}

/* get_tz：/etc/localtime 存在就不用设置；否则以 /etc/TZ、/var/etc/TZ 中最后
 * 存在的那个文件内容为准（与旧 for 循环"后者覆盖前者"一致）。 */
static char *et_get_tz(void)
{
	if (et_file_exists("/etc/localtime"))
		return NULL;

	char *tz = NULL;
	const char *files[] = { "/etc/TZ", "/var/etc/TZ" };
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char *s = et_read_file(files[i], NULL);
		if (!s)
			continue;
		free(tz);
		tz = s;
	}
	if (tz) {
		et_trim_newline(tz);
		if (!*tz) {
			free(tz);
			tz = NULL;
		}
	}
	return tz;
}

/* 统计 `<bin> -h 2>&1 | wc -l` 的行数 */
static int et_bin_help_lines(const char *bin)
{
	char *argv[] = { (char *)bin, (char *)"-h", NULL };
	et_buf o, e;
	et_buf_init(&o);
	et_buf_init(&e);

	int lines = 0;
	if (et_run(argv, &o, &e, 15) >= 0) {
		if (o.p) {
			for (const char *p = o.p; *p; p++)
				if (*p == '\n')
					lines++;
		}
		if (e.p) {
			for (const char *p = e.p; *p; p++)
				if (*p == '\n')
					lines++;
		}
	}
	et_buf_free(&o);
	et_buf_free(&e);
	return lines;
}

/* `[ -f bin ] && [ $(bin -h | wc -l) -ge 3 ]` 的组合判定 */
static bool et_bin_ok(const char *bin)
{
	if (!has_val(bin) || !et_file_exists(bin) || !et_file_executable(bin))
		return false;
	return et_bin_help_lines(bin) >= 3;
}

/* bin -V | awk '{print $2}' | tr -d '~' */
static char *et_bin_version(const char *bin)
{
	char *argv[] = { (char *)bin, (char *)"-V", NULL };
	int st = 0;
	char *out = et_run_capture(argv, &st);
	if (!out)
		return NULL;

	char *save = NULL;
	strtok_r(out, " \t\n", &save);
	char *second = strtok_r(NULL, " \t\n", &save);
	char *ver = NULL;
	if (second) {
		ver = strdup(second);
		if (ver) {
			char *w = ver;
			for (char *r = ver; *r; r++) {
				if (*r != '~')
					*w++ = *r;
			}
			*w = '\0';
			if (!*ver) {
				free(ver);
				ver = NULL;
			}
		}
	}
	free(out);
	return ver;
}

static int et_move_file(const char *src, const char *dst)
{
	if (rename(src, dst) == 0)
		return ET_OK;
	/* 跨文件系统时退化为复制 + 删除 */
	int rc = et_copy_file(src, dst, true);
	if (rc == ET_OK)
		unlink(src);
	return rc;
}

/* fork/exec 一个常驻实例：cwd=/tmp，stdout/stderr 追加到日志，带上环境变量。
 * 子进程里顺手把 procd 那两项 limits 也设上（core=unlimited、nofile=1000000）。 */
static pid_t et_spawn_argv(char *const argv[], const char *cwd,
			   const char *out_path, char *const envp[])
{
	if (!argv || !argv[0])
		return ET_EINVAL;

	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return ET_ERR;
	if (pid > 0)
		return pid;

	/* 子进程：独立会话，清空继承的信号掩码 */
	setsid();
	sigset_t empty;
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);

	struct rlimit rl;
	rl.rlim_cur = rl.rlim_max = 1000000;
	if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
		/* 部分内核 /proc/sys/fs/nr_open 更小，取不到就算了 */
	}
	rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_CORE, &rl) < 0) {
		/* 同上 */
	}

	if (cwd && *cwd && chdir(cwd) < 0)
		_exit(126);

	int devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		if (devnull > STDERR_FILENO)
			close(devnull);
	}

	int fd = open(has_val(out_path) ? out_path : "/dev/null",
		      O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}

	execve(argv[0], argv, envp ? envp : environ);
	_exit(127);
}

/* ---------------- 二进制准备 ---------------- */

/* 通过 api-surface 的 download_easytier 触发在线下载（不重复实现下载逻辑），
 * 然后最多等 ET_DL_WAIT_SEC 秒看目标二进制是否就位。 */
static void et_bin_download(et_cfg *c, const char *bin)
{
	(void)c;
	char *ver = NULL;
	et_uci *u = et_uci_open();
	if (u) {
		ver = et_uci_get_et(u, "fallback_version");
		et_uci_close(u);
	}
	if (!has_val(ver)) {
		free(ver);
		ver = strdup("latest");
	}
	if (!ver)
		return;

	if (!et_have_cmd("unzip")) {
		et_log_to(ET_LOG_CORE,
			  "系统缺少 unzip，无法解压下载的 EasyTier 压缩包，请安装 unzip 后重试");
		et_log_to(ET_LOG_WEB,
			  "系统缺少 unzip，无法解压下载的 EasyTier 压缩包，请安装 unzip 后重试");
		free(ver);
		return;
	}

	et_log_to(ET_LOG_CORE,
		  "开始在线下载%s版本，通过 download_easytier 接口触发，下载较慢耐心等候",
		  ver);
	et_log_to(ET_LOG_WEB,
		  "开始在线下载%s版本，通过 download_easytier 接口触发，下载较慢耐心等候",
		  ver);

	et_list params;
	et_list_init(&params);
	char kv[256];
	snprintf(kv, sizeof(kv), "version=%s", ver);
	et_list_add(&params, kv);

	et_req req = {
		.method = "download_easytier",
		.params = &params,
		.body = NULL,
		.body_len = 0,
	};
	et_res res;
	et_res_init(&res, ET_CT_JSON);
	int rc = et_api_dispatch(&req, &res);
	et_res_free(&res);
	et_list_free(&params);

	if (rc != ET_OK) {
		et_log_to(ET_LOG_CORE,
			  "在线下载接口不可用（download_easytier 未注册或调用失败），请手动下载并上传程序");
		et_log_to(ET_LOG_WEB,
			  "在线下载接口不可用（download_easytier 未注册或调用失败），请手动下载并上传程序");
		free(ver);
		return;
	}

	for (int i = 0; i < ET_DL_WAIT_SEC; i++) {
		if (et_bin_ok(bin))
			break;
		sleep(1);
	}

	if (et_bin_ok(bin)) {
		et_log_to(ET_LOG_CORE, "%s 下载成功", bin);
		et_log_to(ET_LOG_WEB, "%s 下载成功", bin);
	} else {
		et_log_to(ET_LOG_CORE, "%s 下载不完整，请手动下载上传程序", bin);
		et_log_to(ET_LOG_WEB, "%s 下载不完整，请手动下载上传程序", bin);
	}
	free(ver);
}

/* 解析 core 路径：UCI 值 → 默认值写回 → 可用空间不足改用 /tmp → 补执行位 → 下载。
 * 返回值一定非空（内部 strdup），调用方负责 free。 */
static char *et_resolve_core_bin(et_cfg *c, et_uci *u)
{
	char *bin = NULL;
	if (c->easytierbin && *c->easytierbin) {
		bin = strdup(c->easytierbin);
	} else {
		const char *opt = et_cfg_str(c, "easytierbin");
		bin = strdup(has_val(opt) ? opt : ET_PATH_CORE_DEF);
	}
	if (!bin)
		return NULL;

	/* 旧实现：easytierbin 为空时用默认值并 uci set 回配置（未 commit） */
	char *optv = et_uci_get_et(u, "easytierbin");
	if (!has_val(optv)) {
		et_uci_set_et(u, "easytierbin", ET_PATH_CORE_DEF);
	}
	free(optv);

	/* 可用空间不足自愈：只在目标文件不存在时触发，与旧实现条件一致 */
	if (!et_file_exists(bin)) {
		long long size = et_df_available_kb("/overlay");
		if (size < 0)
			size = et_df_available_kb("/usr/bin");
		if (size >= 0 && size < ET_SPACE_MIN_KB) {
			et_log_to(ET_LOG_CORE,
				  "自定义程序路径的可用空间不足，当前可用空间剩余%lldkb,自动切换为内存/tmp/easytier-core",
				  size);
			/* 旧实现用 sed -i 直接改配置文件，这里用 libuci 正确写回并落盘 */
			if (et_uci_set_et(u, "easytierbin", "/tmp/easytier-core") ==
			    0)
				et_uci_commit_et(u);
			free(bin);
			bin = strdup("/tmp/easytier-core");
			if (!bin)
				return NULL;
			free(c->easytierbin);
			c->easytierbin = strdup("/tmp/easytier-core");
		}
	}

	/* 缺执行位时补上，同时补上同目录的 easytier-cli */
	if (et_file_exists(bin) && !et_file_executable(bin)) {
		chmod(bin, 0755);
		char *dir = et_dirname_keep_slash(bin);
		if (dir) {
			char cli[512];
			snprintf(cli, sizeof(cli), "%s%s", dir, ET_CLI_NAME);
			chmod(cli, 0755);
			free(dir);
		}
	}

	if (!et_bin_ok(bin)) {
		et_log_to(ET_LOG_CORE, "%s 不存在或程序不完整，开始在线下载...",
			  bin);
		et_bin_download(c, bin);
	}
	return bin;
}

/* ---------------- config 模式 ---------------- */

static void et_check_config(const char *bin)
{
	et_log_to(ET_LOG_CORE,
		  "检测配置文件可用性 %s --check-config --config-file %s", bin,
		  ET_FILE_TOML);

	char *argv[] = { (char *)bin, (char *)"--check-config",
			 (char *)"--config-file", (char *)ET_FILE_TOML, NULL };
	et_buf o, e;
	et_buf_init(&o);
	et_buf_init(&e);
	int rc = et_run(argv, &o, &e, 30);
	if (o.p && o.len)
		et_append_file(ET_LOG_CORE, o.p, o.len);
	if (e.p && e.len)
		et_append_file(ET_LOG_CORE, e.p, e.len);
	et_buf_free(&o);
	et_buf_free(&e);

	if (rc != 0) {
		et_log_to(ET_LOG_CORE,
			  "配置文件 %s 校验失败，请查看上方 easytier-core 的报错。EasyTier 2.7.0 起配置校验更严格：旧的 [vpn_portal_config] client_cidr 写法与 [flags] 中的未知键都会导致解析失败",
			  ET_FILE_TOML);
	}
}

/* config 模式下端口/tunname 来自 config.toml（旧实现 check_config 是 grep toml 的） */
static void et_ports_from_toml(et_fw_ports *p, const char *toml)
{
	if (!toml)
		return;
	p->tcp = et_toml_port(toml, "tcp");
	p->udp = et_toml_port(toml, "udp");
	p->ws = et_toml_port(toml, "ws");
	p->wss = et_toml_port(toml, "wss");
	p->wg = et_toml_port(toml, "wg");
	p->quic = et_toml_port(toml, "quic");
	p->tunname = et_toml_get(toml, "dev_name");
}

static void et_ports_free(et_fw_ports *p)
{
	free((char *)p->tcp);
	free((char *)p->udp);
	free((char *)p->ws);
	free((char *)p->wss);
	free((char *)p->wg);
	free((char *)p->quic);
	free((char *)p->tunname);
	memset(p, 0, sizeof(*p));
}

/* ---------------- 单实例启动 ---------------- */

/* et_argv_core() 按约定不在末尾拼 --console-log-level（见 etd_argv.c 的注记），
 * 由调用方补上；旧实现三种模式（etcmd/config/web）末尾都带这一项。 */
static char **et_argv_core_with_log(et_cfg *c, const char *bin, et_buf *notes)
{
	char **base = et_argv_core(c, bin, notes);
	if (!base)
		return NULL;

	size_t n = 0;
	while (base[n])
		n++;
	/* 配置里已经显式带了这个开关就不再重复 */
	for (size_t i = 0; i < n; i++) {
		if (!strcmp(base[i], "--console-log-level"))
			return base;
	}

	const char *log = cfg_or(c, "log", "off");
	char **out = calloc(n + 3, sizeof(char *));
	if (!out) {
		et_argv_free(base);
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		out[i] = strdup(base[i]);
		if (!out[i]) {
			et_argv_free(out);
			et_argv_free(base);
			return NULL;
		}
	}
	out[n] = strdup("--console-log-level");
	out[n + 1] = strdup(log);
	et_argv_free(base);
	if (!out[n] || !out[n + 1]) {
		et_argv_free(out);
		return NULL;
	}
	return out;
}

pid_t et_instance_start_core(et_cfg *c, et_buf *notes)
{
	if (!c)
		return ET_EINVAL;

	const char *bin = has_val(c->easytierbin) ? c->easytierbin
						 : ET_PATH_CORE_DEF;
	et_buf local;
	et_buf *np = notes;
	if (!np) {
		et_buf_init(&local);
		np = &local;
	}

	char **argv = et_argv_core_with_log(c, bin, np);
	if (!argv) {
		if (np == &local)
			et_buf_free(&local);
		return ET_ERR;
	}

	/* 旧实现把拼好的命令行写进日志；这里走 et_argv_visible，
	 * 顺带把 network-secret 之类的敏感参数从日志里隐去。 */
	char *vis = et_argv_visible(argv);
	et_log_to(ET_LOG_CORE, "运行 %s", has_val(vis) ? vis : bin);
	free(vis);
	if (np->p && np->len)
		et_log_to(ET_LOG_CORE, "%s", np->p);

	/* 环境变量：TZ 与门户私钥（私钥不进命令行，避免状态页/ps 泄漏） */
	char *env[3];
	int en = 0;
	char *tzs = NULL, *keys = NULL;
	char *tz = et_get_tz();
	if (tz) {
		if (asprintf(&tzs, "TZ=%s", tz) >= 0)
			env[en++] = tzs;
		free(tz);
	}
	const char *key = et_cfg_str(c, "vpn_portal_private_key");
	if (has_val(key)) {
		if (asprintf(&keys, "ET_VPN_PORTAL_PRIVATE_KEY=%s", key) >= 0)
			env[en++] = keys;
	}
	env[en] = NULL;

	pid_t pid = et_spawn_argv(argv, "/tmp", ET_LOG_CORE, env);

	free(tzs);
	free(keys);
	et_argv_free(argv);
	if (np == &local)
		et_buf_free(&local);
	return pid;
}

pid_t et_instance_start_web(et_cfg *c, et_buf *notes)
{
	if (!c)
		return ET_EINVAL;

	const char *bin = has_val(c->webbin) ? c->webbin : ET_PATH_WEB_DEF;
	et_buf local;
	et_buf *np = notes;
	if (!np) {
		et_buf_init(&local);
		np = &local;
	}

	char **argv = et_argv_web(c, bin, np);
	if (!argv) {
		if (np == &local)
			et_buf_free(&local);
		return ET_ERR;
	}

	char *vis = et_argv_visible(argv);
	et_log_to(ET_LOG_WEB, "运行 %s", has_val(vis) ? vis : bin);
	free(vis);
	if (np->p && np->len)
		et_log_to(ET_LOG_WEB, "%s", np->p);

	char *env[2];
	int en = 0;
	char *tzs = NULL;
	char *tz = et_get_tz();
	if (tz) {
		if (asprintf(&tzs, "TZ=%s", tz) >= 0)
			env[en++] = tzs;
		free(tz);
	}
	env[en] = NULL;

	pid_t pid = et_spawn_argv(argv, "/tmp", ET_LOG_WEB, env);

	free(tzs);
	et_argv_free(argv);
	if (np == &local)
		et_buf_free(&local);
	return pid;
}

/* start_et()：准备二进制 → 清理残留 → config 校验 → 启动实例 → 防火墙/转发/时间戳 */
static pid_t et_start_et(et_cfg *c, et_uci *u)
{
	const char *arch = et_detect_arch();
	et_log_to(ET_LOG_CORE, "easytier-core_%s开始启动", arch ? arch : "");

	char *bin = et_resolve_core_bin(c, u);
	if (!bin)
		return ET_ERR;

	if (!et_bin_ok(bin)) {
		et_log_to(ET_LOG_CORE, "%s 不存在或程序不完整，已放弃本次启动",
			  bin);
		free(bin);
		return ET_ENOENT;
	}

	char *ver = et_bin_version(bin);
	if (has_val(ver))
		et_log_to(ET_LOG_CORE, "%s当前版本号-%s", bin, ver);
	free(ver);

	/* 启动前杀掉残留：旧看门狗脚本与旧 core（升级/重启场景） */
	et_proc_terminate("easytier_check", 2000);
	et_proc_terminate("easytier-core", 3000);

	const char *etcmd = cfg_or(c, "etcmd", "etcmd");
	et_fw_ports ports;
	memset(&ports, 0, sizeof(ports));
	char *toml = NULL;
	bool config_mode = !strcmp(etcmd, "config");

	if (config_mode) {
		toml = et_read_file(ET_FILE_TOML, NULL);
		et_check_config(bin);
		et_log_to(ET_LOG_CORE, "开始运行 %s -c %s", bin, ET_FILE_TOML);
		et_ports_from_toml(&ports, toml);
	}

	pid_t pid = et_instance_start_core(c, NULL);
	if (pid <= 0) {
		et_log_to(ET_LOG_CORE, "%s 启动失败", bin);
		et_ports_free(&ports);
		free(toml);
		free(bin);
		return ET_ERR;
	}

	/* set_firewall()：config 模式用 toml 里的端口，其余模式用 UCI */
	et_net_apply_firewall(c, config_mode ? &ports : NULL);
	et_net_ip_forward_enable();
	et_write_ts(ET_TIME_CORE);
	et_log_to(ET_LOG_CORE, "启动完成!");

	et_ports_free(&ports);
	free(toml);
	free(bin);
	return pid;
}

/* start_web() */
static pid_t et_start_web(et_cfg *c, et_uci *u)
{
	char *bin = has_val(c->webbin) ? strdup(c->webbin)
				      : strdup(ET_PATH_WEB_DEF);
	if (!bin)
		return ET_ENOMEM;

	char *optv = et_uci_get_et(u, "webbin");
	if (!has_val(optv))
		et_uci_set_et(u, "webbin", ET_PATH_WEB_DEF);
	free(optv);

	/* 上传的 web 程序优先覆盖安装路径 */
	if (et_file_exists("/tmp/easytier-web-embed")) {
		et_log_to(ET_LOG_WEB, "找到上传的easytier-web程序，替换%s", bin);
		et_move_file("/tmp/easytier-web-embed", bin);
	}

	if (!et_bin_ok(bin)) {
		et_log_to(ET_LOG_WEB, "%s 不存在或程序不完整，开始在线下载...",
			  bin);
		et_bin_download(c, bin);
	}
	if (et_file_exists(bin) && !et_file_executable(bin))
		chmod(bin, 0755);

	if (!et_bin_ok(bin)) {
		et_log_to(ET_LOG_WEB,
			  "未找到%s程序或程序不适配当前架构，请上传安装后再启动！",
			  bin);
		free(bin);
		return ET_ENOENT;
	}

	et_log_to(ET_LOG_WEB, "开始启动%s", bin);

	pid_t pid = et_instance_start_web(c, NULL);
	if (pid <= 0) {
		et_log_to(ET_LOG_WEB, "%s 启动失败", bin);
		free(bin);
		return ET_ERR;
	}

	et_net_apply_web_firewall(c);
	et_write_ts(ET_TIME_WEB);
	et_log_to(ET_LOG_WEB, "启动完成！");
	free(bin);
	return pid;
}

/* ---------------- 停止清理 ---------------- */

/* stop_service() 里的 rm -rf 清单 */
static void et_remove_temp_files(void)
{
	static const char *files[] = {
		"/tmp/easytier-cli_peer",
		"/tmp/easytier-cli_connector",
		"/tmp/easytier-cli_stun",
		"/tmp/easytier-cli_route",
		"/tmp/easytier-cli_acl",
		"/tmp/easytier-cli_peer-center",
		"/tmp/easytier-cli_vpn-portal",
		"/tmp/easytier_cmd",
		"/tmp/easytier-cli_node",
		"/tmp/easytier-cli_mapped_listener",
	};
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		unlink(files[i]);

	/* /tmp/easytier-*.* */
	DIR *d = opendir("/tmp");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (strncmp(e->d_name, "easytier-", 9) != 0)
				continue;
			if (!strchr(e->d_name, '.'))
				continue;
			char path[512];
			snprintf(path, sizeof(path), "/tmp/%s", e->d_name);
			et_rm_rf(path);
		}
		closedir(d);
	}

	unlink(ET_LOG_CORE);
}

/* 按 enabled / web_enabled 做删除与清理（旧 stop_service 只在对应开关为 0 时动手） */
static void et_teardown(et_uci *u)
{
	if (!u)
		return;

	char *en = et_uci_get_et(u, "enabled");
	char *web = et_uci_get_et(u, "web_enabled");
	bool core_off = !en || strcmp(en, "1") != 0;
	bool web_off = !web || strcmp(web, "1") != 0;

	if (core_off) {
		et_proc_terminate("easytier_check", 2000);
		et_proc_terminate("easytier-core", 3000);
		et_net_cleanup_all();
		et_remove_temp_files();
		et_log_to(ET_LOG_CORE, "easytier-core 停止运行");
	}
	if (web_off) {
		et_proc_terminate("easytier-web", 3000);
		et_net_cleanup_web();
	}

	/* 旧实现把延迟重载放在 core 分支里（此时 web 规则还没删）；
	 * 这里统一放到最后，保证 web 规则的删除也会被 commit + reload。 */
	if (core_off || web_off)
		et_net_schedule_reload("停止服务，将在5秒后后台重载配置...");

	free(en);
	free(web);
}

/* ---------------- 启动流程 ---------------- */

static int et_startup(et_cfg *c, et_uci *u, pid_t *core_pid, pid_t *web_pid)
{
	*core_pid = 0;
	*web_pid = 0;

	/* 旧实现每次 start 先清空两个日志 */
	et_truncate_log(ET_LOG_CORE);
	et_truncate_log(ET_LOG_WEB);

	/* web_enabled 缺失时写回 0 */
	char *web_en = et_uci_get_et(u, "web_enabled");
	if (!web_en) {
		et_uci_set_et(u, "web_enabled", "0");
		et_uci_commit_et(u);
	}
	bool web_on = web_en && !strcmp(web_en, "1");
	free(web_en);

	char *en = et_uci_get_et(u, "enabled");
	bool core_on = en && !strcmp(en, "1");
	free(en);

	if (web_on)
		*web_pid = et_start_web(c, u);
	if (core_on)
		*core_pid = et_start_et(c, u);

	et_net_schedule_reload("检测到配置变更，将在5秒后后台重载网络、防火墙...");
	return ET_OK;
}

/* 前台监督入口：先起实例，再进监督循环；重启请求时回到循环头部重新读配置 */
static int et_service_run(void)
{
	pid_t sup = et_pidfile_read(ET_PIDFILE);
	if (sup > 0 && sup != getpid() && et_pidfile_alive(ET_PIDFILE)) {
		et_log_to(ET_LOG_CORE, "已有服务实例在运行（pid %d），本次启动忽略",
			  (int)sup);
		return ET_OK;
	}

	for (;;) {
		et_uci *u = et_uci_open();
		if (!u) {
			fprintf(stderr, "%s: 无法打开 %s\n", ETD_PROG, ET_FILE_UCI);
			return ET_ERR;
		}
		et_cfg *c = et_cfg_load(u);
		if (!c) {
			fprintf(stderr, "%s: 配置加载失败（%s）\n", ETD_PROG,
				ET_FILE_UCI);
			et_uci_close(u);
			return ET_ERR;
		}

		et_pidfile_write(ET_PIDFILE, getpid());

		pid_t core = 0, web = 0;
		et_startup(c, u, &core, &web);

		/* core 与 web 都没起来（例如 enabled/web_enabled 全是 0，或二进制缺失），
		 * 没有可监督的实例，直接走清理并退出——与旧实现"注册不到实例"等价。 */
		if (core <= 0 && web <= 0) {
			et_teardown(u);
			et_cfg_free(c);
			et_uci_close(u);
			break;
		}

		int r = et_super_run(c, core, web);

		/* 监督循环退出后再做一次旧 stop_service 的清理（enabled=0 时才动手） */
		et_teardown(u);

		et_cfg_free(c);
		et_uci_close(u);

		if (r == ET_SUP_RESTART)
			continue;
		break;
	}

	et_unlink(ET_PIDFILE);
	return ET_OK;
}

/* start（非前台）：fork 出脱离会话的 `easytierd service run` */
static int et_service_daemonize(void)
{
	/* 只看 pid 文件：真有监督进程在跑才算"已启动"。
	 * 只有零散 core 进程（上次监督进程被 KILL）时仍要拉起，由 et_start_et
	 * 里的"杀残留"把它们清掉，避免出现两个 core 抢同一个 tun。 */
	if (et_pidfile_alive(ET_PIDFILE))
		return ET_OK;

	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return ET_ERR;
	if (pid == 0) {
		setsid();
		pid_t pid2 = fork();
		if (pid2 != 0)
			_exit(0);

		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}

		char self[128];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n > 0)
			self[n] = '\0';
		else
			snprintf(self, sizeof(self), "/usr/sbin/%s", ETD_PROG);

		execl(self, ETD_PROG, "service", "run", (char *)NULL);
		_exit(127);
	}

	/* 回收中间进程 */
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;

	/* 等监督进程写下 pid 文件，最多 3 秒 */
	for (int i = 0; i < 60; i++) {
		if (et_pidfile_alive(ET_PIDFILE))
			return ET_OK;
		usleep(50 * 1000);
	}
	return et_service_running() ? ET_OK : ET_ERR;
}

int et_service_start(bool foreground)
{
	if (foreground)
		return et_service_run();
	return et_service_daemonize();
}

int et_service_stop(void)
{
	pid_t sup = et_pidfile_read(ET_PIDFILE);
	if (sup > 0) {
		kill(sup, SIGTERM);
		for (int waited = 0; waited < ET_STOP_WAIT_MS; waited += 100) {
			if (kill(sup, 0) < 0 && errno == ESRCH)
				break;
			usleep(100 * 1000);
		}
		if (kill(sup, 0) == 0)
			kill(sup, SIGKILL);
	}
	et_unlink(ET_PIDFILE);

	/* 监督进程已经把子进程带走；这里再兜一次，处理旧版本残留/监督进程被 KILL 的情况 */
	et_proc_terminate("easytier_check", 2000);
	et_proc_terminate("easytier-core", 3000);
	et_proc_terminate("easytier-web", 3000);

	et_uci *u = et_uci_open();
	if (u) {
		et_teardown(u);
		et_uci_close(u);
	}
	return ET_OK;
}

int et_service_restart(void)
{
	if (et_service_stop() != ET_OK)
		return ET_ERR;
	return et_service_start(false);
}

bool et_service_running(void)
{
	if (et_pidfile_alive(ET_PIDFILE))
		return true;
	if (et_proc_running("easytier-core"))
		return true;
	if (et_proc_running("easytier-web"))
		return true;
	return false;
}

int et_service_enable(bool on)
{
	const char *link = "/etc/rc.d/S99easytier";

	if (on) {
		if (et_mkdir_p("/etc/rc.d", 0755) < 0)
			return ET_ERR;
		if (symlink("/etc/init.d/easytier", link) < 0 && errno != EEXIST)
			return ET_ERR;
		return ET_OK;
	}

	if (unlink(link) < 0 && errno != ENOENT)
		return ET_ERR;
	return ET_OK;
}
