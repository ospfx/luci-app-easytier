/*
 * etd_api_ops.c - 二进制上传/安装、在线下载、上传页配置
 *
 * 对应原 controller 的 upload_binary / download_easytier / download_progress /
 * cancel_download / get_upload_config / save_upload_config。
 *
 * 两条与旧实现的有意差异（其余逐字对齐，包括所有面向用户的错误文案）：
 *   1. 下载改为后台执行：handler 只做参数与依赖校验，然后 fork 子进程干活并
 *      立刻返回 progress=1，进度写 /tmp/easytier_download_progress 供前端轮询。
 *      旧实现同步阻塞 HTTP 响应最长 5×300 秒，用户既看不到进度也无法取消。
 *   2. 安装用 "临时文件 + rename" 落盘，避免 cp 中途失败留下半个二进制，
 *      被 rc 重新拉起时 exec 失败。
 */
#include "etd_api_priv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* 与 po/zh_Hans 中 msgstr 逐字一致：前端直接显示这些字符串 */
#define MSG_NO_FILE      "未上传文件"
#define MSG_NO_UNZIP     "当前系统缺少unzip包无法解压zip程序包"
#define MSG_BAD_ARCH     "上传的不是有效easytier程序或架构不匹配"
#define MSG_BAD_BIN      "不是有效的easytier程序"
#define MSG_WEB_NOT_USED                                              \
	"easytier-web不是当前插件自建控制台所使用的程序，请上传"      \
	"easytier-web-embed程序"
#define MSG_MOVE_FAIL    "移动文件失败。空间不足或权限不足。"
#define MSG_NO_ARCH      "无法检测CPU架构"
#define MSG_NO_X86       "没有当前x86-32位的程序"
#define MSG_NEED_UNZIP   "系统缺少 unzip 下载后无法解压"
#define MSG_NO_VERSION   "缺少版本号参数"
#define MSG_TASK_EXISTS  "已有下载任务正在进行"
#define MSG_ALL_FAIL                                                  \
	"所有下载地址均下载失败，请确认github加速镜像地址有效或使用"  \
	"手动上传"
#define MSG_CANCELLED    "已取消下载"
#define MSG_DOWNLOADING  "正在下载..."
#define MSG_EXTRACTING   "正在解压..."
#define MSG_VERIFYING    "正在校验程序..."
#define MSG_INSTALLING   "正在安装..."
#define MSG_DL_DONE      "下载安装完成"
#define MSG_INSTALL_FAIL "程序安装失败"
#define MSG_INCOMPLETE   "程序不完整或架构不匹配"
#define MSG_PREPARING    "准备中..."
#define MSG_SAVED        "配置保存成功"
#define MSG_UCI_DOWN     "uci 配置不可用"
#define MSG_BAD_JSON     "Invalid JSON data"

#define DL_MIN_BYTES (3LL * 1024 * 1024) /* ≥3MB 才算下载完整 */

/* ---------------- 通用小工具 ---------------- */

/* 执行外部命令并吞掉输出：子进程 stdout/stderr 不能被继承，
 * 否则 tar/unzip 的输出会混进 HTTP 响应体。 */
static int run_quiet(char *const argv[], int timeout_sec)
{
	et_buf out, err;
	int rc;

	et_buf_init(&out);
	et_buf_init(&err);
	rc = et_run(argv, &out, &err, timeout_sec);
	et_buf_free(&out);
	et_buf_free(&err);
	return rc;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
	size_t ls, lf;

	if (!s || !suffix)
		return false;
	ls = strlen(s);
	lf = strlen(suffix);
	if (ls < lf)
		return false;
	return strcasecmp(s + ls - lf, suffix) == 0;
}

/* 复制到目标目录下的临时名再 rename：目标要么是旧文件、要么是完整新文件，
 * 不会出现"装了一半"的二进制被 init 脚本 exec。 */
static int copy_stream(const char *src, const char *dst)
{
	char tmp[512];
	char buf[65536];
	int in = -1, out = -1;
	int rc = ET_ERR;

	if (snprintf(tmp, sizeof(tmp), "%s.etnew", dst) >= (int)sizeof(tmp))
		return ET_EINVAL;
	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return ET_ENOENT;
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	if (out < 0)
		goto done;
	for (;;) {
		ssize_t r = read(in, buf, sizeof(buf));

		if (r < 0) {
			if (errno == EINTR)
				continue;
			goto done;
		}
		if (r == 0)
			break;
		for (ssize_t off = 0; off < r;) {
			ssize_t w = write(out, buf + off, (size_t)(r - off));

			if (w < 0) {
				if (errno == EINTR)
					continue;
				goto done;
			}
			off += w;
		}
	}
	rc = ET_OK;

done:
	if (in >= 0)
		close(in);
	if (out >= 0) {
		fsync(out);
		close(out);
	}
	if (rc != ET_OK) {
		unlink(tmp);
		return rc;
	}
	if (rename(tmp, dst) < 0) {
		/* 目标目录不存在等：保留原错误，清理临时文件 */
		unlink(tmp);
		return ET_ERR;
	}
	return ET_OK;
}

static int move_file(const char *src, const char *dst)
{
	if (rename(src, dst) == 0)
		return ET_OK;
	if (copy_stream(src, dst) != ET_OK)
		return ET_ERR;
	unlink(src);
	return ET_OK;
}

/* ---------------- 安装单个二进制 ---------------- */

/* 目标路径规则与旧实现一致：
 *   easytier-web-embed → webbin
 *   easytier-core      → easytierbin
 *   easytier-cli       → easytierbin 同目录下的 easytier-cli
 * 装完再验一次（旧实现同样要求"安装后还能跑"才算成功）。 */
static int install_one(const char *src, const char *bin_name,
		       const char *core_path, const char *web_path,
		       char **final_out, const char **err)
{
	char *dest = NULL;

	if (!strcmp(bin_name, ET_WEB_EMBED_NAME)) {
		dest = strdup(web_path);
	} else if (!strcmp(bin_name, "easytier-core")) {
		dest = strdup(core_path);
	} else {
		char *dir = et_dirname_keep_slash(core_path);

		if (asprintf(&dest, "%seasytier-cli", dir ? dir : "/usr/bin/") < 0)
			dest = NULL;
		free(dir);
	}
	if (!dest) {
		*err = MSG_MOVE_FAIL;
		return ET_ENOMEM;
	}

	/* 不先删目标：move_file 对同文件系统是 rename、跨文件系统是"临时名 + rename"，
	 * 两种都是原子替换，安装失败时旧二进制仍在（旧实现先 rm 再 mv，中间有个空洞）。 */
	if (move_file(src, dest) != ET_OK) {
		*err = MSG_MOVE_FAIL;
		free(dest);
		return ET_ERR;
	}
	if (chmod(dest, 0755) < 0) {
		*err = MSG_MOVE_FAIL;
		unlink(dest);
		free(dest);
		return ET_ERR;
	}
	if (!et_test_binary(dest)) {
		unlink(dest);
		*err = MSG_BAD_ARCH;
		free(dest);
		return ET_ERR;
	}
	*final_out = dest;
	return ET_OK;
}

/* ---------------- 压缩包处理 ---------------- */

static int extract_zip(const char *file, const char *dir)
{
	char *argv[] = {
		(char *)"unzip", (char *)"-o", (char *)"-q", (char *)file,
		(char *)"-d", (char *)dir, NULL
	};

	return run_quiet(argv, 300) == 0 ? ET_OK : ET_ERR;
}

static int extract_tar(const char *file, const char *dir)
{
	char *a1[] = {
		(char *)"tar", (char *)"-xzf", (char *)file, (char *)"-C",
		(char *)dir, NULL
	};
	char *a2[] = {
		(char *)"tar", (char *)"-xf", (char *)file, (char *)"-C",
		(char *)dir, NULL
	};

	if (run_quiet(a1, 300) == 0)
		return ET_OK;
	return run_quiet(a2, 300) == 0 ? ET_OK : ET_ERR;
}

/* 官方 zip 里是一层 easytier-linux-<arch>/ 目录；旧实现用
 * `find .../easytier-linux-* -maxdepth 1 -type f -exec mv {} dir \;`
 * 把文件摊平到提取目录。这里先收集文件名再搬，避免边遍历边移动漏文件。 */
static void hoist_subdirs(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		char sub[512];
		DIR *sd;
		struct dirent *se;
		et_list names;

		if (e->d_name[0] == '.' || strncmp(e->d_name, "easytier-linux-", 15))
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		if (!et_is_dir(sub))
			continue;
		sd = opendir(sub);
		if (!sd)
			continue;
		et_list_init(&names);
		while ((se = readdir(sd))) {
			char path[1024];
			struct stat st;

			if (se->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", sub, se->d_name);
			if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
				et_list_add(&names, se->d_name);
		}
		closedir(sd);
		for (size_t i = 0; i < names.n; i++) {
			char src[1024], dst[1024];

			snprintf(src, sizeof(src), "%s/%s", sub, names.v[i]);
			snprintf(dst, sizeof(dst), "%s/%s", dir, names.v[i]);
			move_file(src, dst);
		}
		et_list_free(&names);
	}
	closedir(d);
}

/* 在解包目录中挑出有效二进制；无效/架构不符的直接删掉 */
static size_t collect_valid(const char *dir, const char *const *names,
			    size_t n, const char **valid)
{
	size_t cnt = 0;

	for (size_t i = 0; i < n; i++) {
		char path[1024];

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (!et_file_exists(path))
			continue;
		chmod(path, 0755);
		if (et_test_binary(path))
			valid[cnt++] = names[i];
		else
			unlink(path);
	}
	return cnt;
}

static const char *const ALL_BINS[] = {
	"easytier-core", "easytier-cli", ET_WEB_EMBED_NAME
};
#define ALL_BINS_N (sizeof(ALL_BINS) / sizeof(ALL_BINS[0]))

static void drop_tag_cache(void)
{
	et_unlink(ET_TAG_CORE);
	et_unlink(ET_TAG_WEB);
}

/* ---------------- upload_binary ---------------- */

static int upload_archive(const char *file, const char *name, et_res *res,
			  const char *core_path, const char *web_path)
{
	const char *valid[ALL_BINS_N];
	size_t nvalid;
	int rc = ET_ERR;

	if (ends_with_ci(name, ".zip") && !et_have_cmd("unzip")) {
		unlink(file);
		et_json_result(res, false, MSG_NO_UNZIP);
		return ET_OK;
	}

	et_rm_rf(ET_UP_DIR);
	if (et_mkdir_p(ET_UP_DIR, 0755) != ET_OK) {
		unlink(file);
		et_json_result(res, false, MSG_MOVE_FAIL);
		return ET_OK;
	}
	if (ends_with_ci(name, ".zip"))
		rc = extract_zip(file, ET_UP_DIR);
	else
		rc = extract_tar(file, ET_UP_DIR);

	hoist_subdirs(ET_UP_DIR);
	unlink(file);

	if (rc != ET_OK) {
		et_rm_rf(ET_UP_DIR);
		et_json_result(res, false, MSG_BAD_ARCH);
		return ET_OK;
	}

	/* 非 embed 版的 easytier-web 本插件不用，解出来也直接丢掉 */
	{
		char stale[1024];

		snprintf(stale, sizeof(stale), "%s/easytier-web", ET_UP_DIR);
		unlink(stale);
	}

	nvalid = collect_valid(ET_UP_DIR, ALL_BINS, ALL_BINS_N, valid);
	if (nvalid == 0) {
		et_rm_rf(ET_UP_DIR);
		et_json_result(res, false, MSG_BAD_ARCH);
		return ET_OK;
	}

	for (size_t i = 0; i < nvalid; i++) {
		char src[1024];
		char *final = NULL;
		const char *err = MSG_BAD_ARCH;

		snprintf(src, sizeof(src), "%s/%s", ET_UP_DIR, valid[i]);
		if (install_one(src, valid[i], core_path, web_path, &final,
				&err) != ET_OK) {
			et_rm_rf(ET_UP_DIR);
			et_json_result(res, false, err);
			return ET_OK;
		}
		free(final);
	}
	et_rm_rf(ET_UP_DIR);
	drop_tag_cache();

	{
		et_buf b;

		et_buf_init(&b);
		et_buf_addf(&b, "成功安装 %zu 个二进制程序", nvalid);
		et_json_result(res, true, b.p ? b.p : "");
		et_buf_free(&b);
	}
	return ET_OK;
}

static int upload_single(const char *file, const char *name, et_res *res,
			 const char *core_path, const char *web_path)
{
	static const char *const ok_names[] = {
		"easytier-core", "easytier-cli", "easytier-web",
		ET_WEB_EMBED_NAME
	};
	bool known = false;
	char *final = NULL;
	const char *err = MSG_BAD_ARCH;

	for (size_t i = 0; i < sizeof(ok_names) / sizeof(ok_names[0]); i++) {
		if (!strcmp(name, ok_names[i]))
			known = true;
	}
	if (!known) {
		unlink(file);
		et_json_result(res, false, MSG_BAD_BIN);
		return ET_OK;
	}
	if (!strcmp(name, "easytier-web")) {
		unlink(file);
		et_json_result(res, false, MSG_WEB_NOT_USED);
		return ET_OK;
	}

	chmod(file, 0755);
	if (!et_test_binary(file)) {
		unlink(file);
		et_json_result(res, false, MSG_BAD_ARCH);
		return ET_OK;
	}

	if (install_one(file, name, core_path, web_path, &final, &err) != ET_OK) {
		et_json_result(res, false, err);
		return ET_OK;
	}
	drop_tag_cache();

	{
		et_buf b;

		et_buf_init(&b);
		et_buf_addf(&b, "二进制文件已成功上传至 %s", final);
		et_json_result(res, true, b.p ? b.p : "");
		et_buf_free(&b);
	}
	free(final);
	return ET_OK;
}

/* 上传源文件必须是绝对路径、必须是普通文件，且不能落在系统目录里。
 * 这个 handler 会对参数做 unlink/chmod，而参数最终来自 multipart 的文件名
 * （极薄 Lua 层拼成 /tmp/upload_<name>），恶意文件名可能把它带出 /tmp；
 * 旧实现没有这层校验，直接删/改目标路径。 */
static bool upload_src_ok(const char *path)
{
	static const char *const bad[] = {
		"/bin", "/sbin", "/usr", "/lib", "/lib64", "/boot",
		"/etc", "/dev", "/proc", "/sys", "/var/lib", "/var/run"
	};
	struct stat st;

	if (!path || path[0] != '/')
		return false;
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (et_starts_with(path, bad[i]))
			return false;
	}
	/* 文件不存在时不在这里拦：交给下面的校验流程报"不是有效的easytier程序"，
	 * 与旧实现的文案保持一致；存在但不是普通文件则直接拒绝。 */
	return stat(path, &st) != 0 || S_ISREG(st.st_mode);
}

int et_api_upload_binary(const et_req *req, et_res *res)
{
	/* 极薄 Lua 层负责收 multipart 落盘，这里只认两个参数：
	 * file = 临时文件绝对路径，name = 原始文件名 */
	char *file = et_api_arg(req, "file");
	char *name = et_api_arg(req, "name");
	et_uci *u;
	char *core_path = NULL, *web_path = NULL, *cli = NULL;
	bool is_archive;

	if (!name || !*name) {
		free(name);
		name = et_api_arg(req, "filename");
	}
	if (!file || !*file || !name || !*name || !upload_src_ok(file)) {
		free(file);
		free(name);
		et_json_result(res, false, MSG_NO_FILE);
		return ET_EINVAL;
	}
	/* name 里带了目录时只取最后一段，避免提示文案/压缩包判断被路径干扰 */
	if (strrchr(name, '/')) {
		char *base = strrchr(name, '/') + 1;

		memmove(name, base, strlen(base) + 1);
	}

	is_archive = ends_with_ci(name, ".zip") || ends_with_ci(name, ".tar.gz") ||
		     ends_with_ci(name, ".tgz") || ends_with_ci(name, ".tar");

	u = et_uci_open();
	et_resolve_bins(u, &core_path, &web_path, &cli);
	free(cli);

	if (is_archive)
		upload_archive(file, name, res, core_path, web_path);
	else
		upload_single(file, name, res, core_path, web_path);

	free(core_path);
	free(web_path);
	free(file);
	free(name);
	et_uci_close(u);
	return ET_OK;
}

/* ---------------- 上传页配置 ---------------- */

static char *json_str(json_object *root, const char *key)
{
	json_object *v = NULL;

	if (!root || !json_object_object_get_ex(root, key, &v))
		return NULL;
	if (json_object_is_type(v, json_type_string))
		return strdup(json_object_get_string(v));
	if (json_object_is_type(v, json_type_int) ||
	    json_object_is_type(v, json_type_double))
		return strdup(json_object_to_json_string(v));
	if (json_object_is_type(v, json_type_boolean))
		return strdup(json_object_get_boolean(v) ? "1" : "0");
	return NULL;
}

/* 按键取字符串：JSON 对象优先，回退 k=v 参数 */
static char *cfg_arg(json_object *root, const et_req *req, const char *key)
{
	char *v = json_str(root, key);

	if (v)
		return v;
	return et_api_arg(req, key);
}

int et_api_upload_config_get(const et_req *req, et_res *res)
{
	et_uci *u = et_uci_open();
	char *core = NULL, *web = NULL, *cli = NULL;
	char *fb;
	char **proxies = NULL;
	size_t n = 0;

	(void)req;
	et_resolve_bins(u, &core, &web, &cli);
	free(cli);
	fb = u ? et_uci_get_et(u, "fallback_version") : NULL;
	if (u)
		proxies = et_uci_get_et_list(u, "github_proxys", &n);

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_str(res, "easytierbin", core ? core : ET_PATH_CORE_DEF);
	et_json_kv_str(res, "webbin", web ? web : ET_PATH_WEB_DEF);
	et_json_kv_strv(res, "github_proxys", proxies, n);
	et_json_kv_str(res, "fallback_version",
		       (fb && *fb) ? fb : "v2.7.0");
	et_json_end(res);

	free(core);
	free(web);
	free(fb);
	et_api_free_strv(proxies);
	et_uci_close(u);
	return ET_OK;
}

int et_api_upload_config_save(const et_req *req, et_res *res)
{
	json_object *root = NULL;
	et_uci *u;
	et_list list;
	char *core, *web, *fb;
	int rc = ET_OK;

	if (req->body && *req->body) {
		root = json_tokener_parse(req->body);
		if (!root) {
			et_json_result(res, false, MSG_BAD_JSON);
			return ET_EINVAL;
		}
	}

	core = cfg_arg(root, req, "easytierbin");
	web = cfg_arg(root, req, "webbin");
	fb = cfg_arg(root, req, "fallback_version");

	et_list_init(&list);
	{
		json_object *arr = NULL;
		bool have_arr = false;

		if (root && json_object_object_get_ex(root, "github_proxys", &arr) &&
		    json_object_is_type(arr, json_type_array)) {
			have_arr = true;
			for (size_t i = 0; i < json_object_array_length(arr); i++) {
				json_object *it = json_object_array_get_idx(arr, i);
				const char *s = it ? json_object_get_string(it) : NULL;

				if (s && *s)
					et_list_add(&list, s);
			}
		}
		if (!have_arr) {
			char *one = cfg_arg(root, req, "github_proxys");

			if (one && *one)
				et_list_add(&list, one);
			free(one);
		}
	}

	u = et_uci_open();
	if (!u) {
		et_json_result(res, false, MSG_UCI_DOWN);
		rc = ET_ERR;
		goto out;
	}
	if (et_uci_set_et(u, "easytierbin",
			  (core && *core) ? core : ET_PATH_CORE_DEF) != ET_OK)
		rc = ET_ERR;
	if (et_uci_set_et(u, "webbin",
			  (web && *web) ? web : ET_PATH_WEB_DEF) != ET_OK)
		rc = ET_ERR;
	if (et_uci_set_et(u, "fallback_version",
			  (fb && *fb) ? fb : "v2.7.0") != ET_OK)
		rc = ET_ERR;
	/* list 语义：先清空再写入（et_uci_set_list 内部就是 delete + add_list） */
	if (et_uci_set_list(u, "easytier", "@easytier[0]", "github_proxys",
			    (const char *const *)list.v, list.n) != ET_OK)
		rc = ET_ERR;
	if (rc == ET_OK)
		rc = et_uci_commit_et(u);
	et_uci_close(u);

	if (rc != ET_OK)
		et_json_result(res, false, "配置写入失败");
	else
		et_json_result(res, true, MSG_SAVED);

out:
	et_list_free(&list);
	free(core);
	free(web);
	free(fb);
	if (root)
		json_object_put(root);
	return ET_OK;
}

/* ---------------- 下载 ---------------- */

static void dl_write_progress(int pct, const char *msg, const char *url,
			      const char *err)
{
	et_buf b;

	et_buf_init(&b);
	et_buf_addf(&b, "{\"progress\":%d,\"message\":\"", pct);
	et_buf_add_json(&b, msg ? msg : "");
	et_buf_add(&b, "\",\"url\":\"");
	et_buf_add_json(&b, url ? url : "");
	et_buf_add(&b, "\",\"error\":");
	if (err) {
		et_buf_addc(&b, '"');
		et_buf_add_json(&b, err);
		et_buf_addc(&b, '"');
	} else {
		et_buf_add(&b, "false");
	}
	et_buf_addc(&b, '}');
	et_write_file(ET_DL_PROGRESS, b.p ? b.p : "", b.len);
	et_buf_free(&b);
}

static json_object *dl_read_progress(void)
{
	char *s = et_read_file(ET_DL_PROGRESS, NULL);
	json_object *o;

	if (!s)
		return NULL;
	o = json_tokener_parse(s);
	free(s);
	return o;
}

static bool dl_cancel_requested(void)
{
	return et_file_exists(ET_DL_CANCEL) != 0;
}

/* 执行下载命令，期间每 200ms 看一次取消标志。
 * 旧实现只在换镜像之间检查取消，用户点了取消还得等 curl 自己超时（最长 5 分钟），
 * 取消等于没生效；这里把取消延迟压到 200ms 量级。
 * 返回 0 表示成功，-2 表示被取消，其它为失败。 */
static int run_cancellable(char *const argv[], int timeout_sec)
{
	pid_t pid;
	time_t deadline;
	int status = 0;

	pid = fork();
	if (pid < 0)
		return ET_ERR;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		execvp(argv[0], argv);
		_exit(127);
	}

	deadline = timeout_sec > 0 ? time(NULL) + timeout_sec : 0;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);

		if (r == pid)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return ET_ERR;
		}
		if (dl_cancel_requested()) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			return -2;
		}
		if (deadline && time(NULL) >= deadline) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			return ET_ERR;
		}
		usleep(200 * 1000);
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* curl 优先、wget 兜底；≥3MB 才认为下载完整（旧实现门槛一致） */
static bool dl_fetch(const char *url, const char *out_path)
{
	if (et_have_cmd("curl")) {
		char *argv[] = {
			(char *)"curl", (char *)"-L", (char *)"-k",
			(char *)"--connect-timeout", (char *)"30",
			(char *)"--max-time", (char *)"300",
			(char *)"-o", (char *)out_path, (char *)url, NULL
		};

		dl_write_progress(10, "使用 curl...", url, NULL);
		if (run_cancellable(argv, 320) == 0 &&
		    et_file_size(out_path) >= DL_MIN_BYTES)
			return true;
		if (dl_cancel_requested())
			return false;
	}
	if (et_have_cmd("wget")) {
		char *argv[] = {
			(char *)"wget", (char *)"--no-check-certificate",
			(char *)"--timeout=30", (char *)"--tries=3",
			(char *)"-O", (char *)out_path, (char *)url, NULL
		};

		dl_write_progress(10, "使用 wget...", url, NULL);
		if (run_cancellable(argv, 320) == 0 &&
		    et_file_size(out_path) >= DL_MIN_BYTES)
			return true;
	}
	return false;
}

/* 解包目录里第一个 easytier-linux-<arch> 子目录（旧实现 find | head -1） */
static char *find_linux_subdir(const char *dir)
{
	DIR *d = opendir(dir);
	char *found = NULL;
	struct dirent *e;

	if (!d)
		return NULL;
	while ((e = readdir(d))) {
		char path[1024];

		if (e->d_name[0] == '.' ||
		    strncmp(e->d_name, "easytier-linux-", 15))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (!et_is_dir(path))
			continue;
		found = strdup(path);
		break;
	}
	closedir(d);
	return found;
}

static void dl_fail(const char *msg)
{
	et_rm_rf(ET_DL_DIR);
	et_unlink(ET_DL_CANCEL);
	dl_write_progress(0, msg, "", msg);
}

/* 校验三个二进制都能跑；mips/mipsel 没有 web-embed 包，跳过它 */
static bool dl_bins_ok(const char *core, const char *cli, const char *web,
		       bool need_web)
{
	chmod(core, 0755);
	chmod(cli, 0755);
	if (!et_test_binary(core) || !et_test_binary(cli))
		return false;
	if (need_web) {
		chmod(web, 0755);
		if (!et_test_binary(web))
			return false;
	}
	return true;
}

static void dl_run(const char *version, const char *arch, char *const *proxies,
		   size_t np)
{
	char zip[320];
	char exdir[320];
	char *base = NULL;
	bool need_web = strcmp(arch, "mips") != 0 && strcmp(arch, "mipsel") != 0;
	char *core_file = NULL, *cli_file = NULL, *web_file = NULL;
	bool ok = false;

	snprintf(zip, sizeof(zip), "%s/easytier-linux-%s-%s.zip", ET_DL_DIR,
		 arch, version);
	snprintf(exdir, sizeof(exdir), "%s/extracted", ET_DL_DIR);
	if (asprintf(&base,
		     "https://github.com/EasyTier/EasyTier/releases/download/%s/"
		     "easytier-linux-%s-%s.zip",
		     version, arch, version) < 0) {
		dl_fail(MSG_ALL_FAIL);
		return;
	}

	for (size_t i = 0; i < np && !ok; i++) {
		char *url = NULL;

		if (dl_cancel_requested()) {
			free(base);
			dl_fail(MSG_CANCELLED);
			return;
		}
		if (asprintf(&url, "%s%s", proxies[i] ? proxies[i] : "", base) < 0)
			continue;
		dl_write_progress(5, MSG_DOWNLOADING, url, NULL);
		et_unlink(zip);
		et_rm_rf(exdir);

		if (!dl_fetch(url, zip)) {
			et_unlink(zip);
			free(url);
			continue;
		}
		if (dl_cancel_requested()) {
			free(url);
			free(base);
			dl_fail(MSG_CANCELLED);
			return;
		}

		dl_write_progress(50, MSG_EXTRACTING, url, NULL);
		if (et_mkdir_p(exdir, 0755) != ET_OK ||
		    extract_zip(zip, exdir) != ET_OK) {
			et_unlink(zip);
			free(url);
			continue;
		}
		if (dl_cancel_requested()) {
			free(url);
			free(base);
			dl_fail(MSG_CANCELLED);
			return;
		}

		char *sub = find_linux_subdir(exdir);

		if (sub) {
			asprintf(&core_file, "%s/easytier-core", sub);
			asprintf(&cli_file, "%s/easytier-cli", sub);
			asprintf(&web_file, "%s/%s", sub, ET_WEB_EMBED_NAME);
			free(sub);

			if (core_file && cli_file && web_file &&
			    et_file_exists(core_file) &&
			    et_file_exists(cli_file) &&
			    (!need_web || et_file_exists(web_file))) {
				dl_write_progress(70, MSG_VERIFYING, url, NULL);
				ok = dl_bins_ok(core_file, cli_file, web_file,
						need_web);
			}
		}
		if (!ok) {
			free(core_file);
			free(cli_file);
			free(web_file);
			core_file = cli_file = web_file = NULL;
			et_unlink(zip);
		}
		free(url);
	}

	if (!ok) {
		free(base);
		dl_fail(MSG_ALL_FAIL);
		return;
	}
	if (dl_cancel_requested()) {
		free(core_file);
		free(cli_file);
		free(web_file);
		free(base);
		dl_fail(MSG_CANCELLED);
		return;
	}

	dl_write_progress(90, MSG_INSTALLING, "", NULL);

	{
		et_uci *u = et_uci_open();
		char *core_dst = NULL, *web_dst = NULL, *cli = NULL;
		char *cli_dst = NULL;
		const char *err = NULL;
		char *err_dst = NULL;

		et_resolve_bins(u, &core_dst, &web_dst, &cli);
		free(cli);
		{
			char *dir = et_dirname_keep_slash(core_dst);

			if (asprintf(&cli_dst, "%seasytier-cli",
				     dir ? dir : "/usr/bin/") < 0)
				cli_dst = NULL;
			free(dir);
		}

		struct {
			const char *src;
			const char *dst;
			bool use;
		} jobs[3] = {
			{ core_file, core_dst, true },
			{ cli_file, cli_dst, true },
			{ web_file, web_dst, need_web },
		};

		for (size_t i = 0; i < 3 && !err; i++) {
			if (!jobs[i].use || !jobs[i].dst)
				continue;
			if (copy_stream(jobs[i].src, jobs[i].dst) != ET_OK) {
				err = MSG_INSTALL_FAIL;
				err_dst = strdup(jobs[i].dst);
				break;
			}
			chmod(jobs[i].dst, 0755);
			if (!et_test_binary(jobs[i].dst))
				err = MSG_INCOMPLETE;
		}

		if (err) {
			if (err_dst) {
				et_buf b;

				et_buf_init(&b);
				et_buf_addf(&b, "%s: %s", err, err_dst);
				dl_fail(b.p ? b.p : err);
				et_buf_free(&b);
			} else {
				dl_fail(err);
			}
		} else {
			et_rm_rf(ET_DL_DIR);
			et_unlink(ET_DL_CANCEL);
			drop_tag_cache();
			dl_write_progress(100, MSG_DL_DONE, "", NULL);
		}

		free(err_dst);
		free(core_dst);
		free(web_dst);
		free(cli_dst);
		et_uci_close(u);
	}

	free(core_file);
	free(cli_file);
	free(web_file);
	free(base);
}

/* 双 fork + setsid：下载进程不挂在 HTTP 处理进程的进程组里，
 * 请求返回、ctl 退出都不会把它带走。 */
static int dl_spawn(const char *version, const char *arch,
		    char *const *proxies, size_t np)
{
	pid_t pid;

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return ET_ERR;
	if (pid == 0) {
		pid_t p2 = fork();

		if (p2 != 0)
			_exit(0);
		setsid();
		signal(SIGPIPE, SIG_IGN);
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		dl_run(version, arch, proxies, np);
		_exit(0);
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
	return ET_OK;
}

/* GitHub 镜像：UCI github_proxys 优先，缺省四个内置镜像 + 直连（空串） */
static void dl_proxies(et_list *out)
{
	et_uci *u = et_uci_open();
	char **cfgd = NULL;
	size_t n = 0;

	et_list_init(out);
	if (u)
		cfgd = et_uci_get_et_list(u, "github_proxys", &n);
	for (size_t i = 0; i < n; i++) {
		if (cfgd[i] && *cfgd[i])
			et_list_add(out, cfgd[i]);
	}
	et_api_free_strv(cfgd);
	et_uci_close(u);

	if (out->n == 0) {
		et_list_add(out, "https://ghproxy.net/");
		et_list_add(out, "https://gh-proxy.com/");
		et_list_add(out, "https://cdn.gh-proxy.com/");
		et_list_add(out, "https://ghfast.top/");
	}
	et_list_add(out, ""); /* 直连 */
}

int et_api_download_start(const et_req *req, et_res *res)
{
	json_object *cur = dl_read_progress();
	char *version;
	const char *arch;
	et_list proxies;

	/* 已有进行中的任务：直接回当前进度，避免开第二个下载把文件写乱 */
	if (cur) {
		json_object *pv = NULL;
		json_object *ev = NULL;
		int pct = 0;
		bool has_err = false;

		if (json_object_object_get_ex(cur, "progress", &pv))
			pct = json_object_get_int(pv);
		if (json_object_object_get_ex(cur, "error", &ev) && ev) {
			has_err = json_object_is_type(ev, json_type_boolean)
					  ? json_object_get_boolean(ev)
					  : true;
		}

		if (pct > 0 && pct < 100 && !has_err) {
			json_object *uv = NULL;
			const char *u = "";

			if (json_object_object_get_ex(cur, "url", &uv) && uv)
				u = json_object_get_string(uv);
			res->type = ET_CT_JSON;
			et_json_begin(res);
			et_json_kv_bool(res, "success", true);
			et_json_kv_int(res, "progress", pct);
			et_json_kv_str(res, "message", MSG_TASK_EXISTS);
			et_json_kv_str(res, "url", u);
			et_json_end(res);
			json_object_put(cur);
			return ET_OK;
		}
		json_object_put(cur);
	}

	version = et_api_arg(req, "version");
	if (!version || !*version) {
		free(version);
		et_json_result(res, false, MSG_NO_VERSION);
		return ET_EINVAL;
	}
	arch = et_detect_arch();
	if (!arch) {
		free(version);
		et_json_result(res, false, MSG_NO_ARCH);
		return ET_OK;
	}
	if (!strcmp(arch, "i386")) {
		free(version);
		et_json_result(res, false, MSG_NO_X86);
		return ET_OK;
	}
	if (!et_have_cmd("unzip")) {
		free(version);
		et_json_result(res, false, MSG_NEED_UNZIP);
		return ET_OK;
	}

	dl_proxies(&proxies);
	et_mkdir_p(ET_DL_DIR, 0755);
	et_unlink(ET_DL_CANCEL);
	dl_write_progress(1, MSG_DOWNLOADING, "", NULL);

	if (dl_spawn(version, arch, proxies.v, proxies.n) != ET_OK) {
		et_rm_rf(ET_DL_DIR);
		et_unlink(ET_DL_PROGRESS);
		et_json_result(res, false, MSG_ALL_FAIL);
		goto out;
	}

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_int(res, "progress", 1);
	et_json_kv_str(res, "message", MSG_DOWNLOADING);
	et_json_kv_str(res, "url", "");
	et_json_end(res);

out:
	et_list_free(&proxies);
	free(version);
	return ET_OK;
}

int et_api_download_progress(const et_req *req, et_res *res)
{
	json_object *o = dl_read_progress();
	json_object *v = NULL;
	json_object *ev = NULL;
	int pct = 0;
	const char *msg = MSG_PREPARING;
	const char *url = "";
	char *err = NULL;

	(void)req;
	if (o) {
		if (json_object_object_get_ex(o, "progress", &v) && v)
			pct = json_object_get_int(v);
		if (json_object_object_get_ex(o, "message", &v) && v)
			msg = json_object_get_string(v);
		if (json_object_object_get_ex(o, "url", &v) && v)
			url = json_object_get_string(v);
		if (json_object_object_get_ex(o, "error", &ev) && ev) {
			if (json_object_is_type(ev, json_type_boolean)) {
				if (json_object_get_boolean(ev))
					err = strdup(MSG_ALL_FAIL);
			} else if (json_object_is_type(ev, json_type_string)) {
				err = strdup(json_object_get_string(ev));
			}
		}
	}

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_int(res, "progress", pct);
	et_json_kv_str(res, "message", msg ? msg : "");
	et_json_kv_str(res, "url", url ? url : "");
	if (err && *err) {
		et_buf q;

		et_buf_init(&q);
		et_buf_addc(&q, '"');
		et_buf_add_json(&q, err);
		et_buf_addc(&q, '"');
		et_json_kv_raw(res, "error", q.p ? q.p : "false");
		et_buf_free(&q);
	} else {
		et_json_kv_raw(res, "error", "false");
	}
	et_json_end(res);

	free(err);
	if (o)
		json_object_put(o);
	return ET_OK;
}

int et_api_download_cancel(const et_req *req, et_res *res)
{
	(void)req;
	et_write_file(ET_DL_CANCEL, "", 0);
	et_json_result(res, true, NULL);
	return ET_OK;
}
