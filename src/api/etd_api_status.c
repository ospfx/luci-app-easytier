/*
 * etd_api_status.c - 状态采集 / TUN 接口信息 / 磁盘空间
 *
 * 对应原 controller 的 act_status / get_tun_info / get_disk_space。
 *
 * 与旧实现的差别只在"怎么取数"：旧版为了一屏状态要 fork pgrep×2、top×2、
 * pidof×4、awk×4、df×2、ls -lh×2、ip×4、ifconfig×1，路由器上单次请求数秒；
 * 这里全部改为读 /proc 与 /sys，只有版本探测和 ifconfig 回退才起子进程。
 * 输出字段名、格式（"%.2f MB"、"3天 04小时05分06秒"）与旧实现逐字一致。
 */
#include "etd_api_priv.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define ET_UA_GITHUB                                                       \
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "    \
	"(KHTML, like Gecko) Chrome/117.0.0.0 Safari/537.36"
#define ET_URL_LATEST                                                      \
	"https://api.github.com/repos/EasyTier/EasyTier/releases/latest"

pid_t et_api_pid_web(void)
{
	pid_t pid = et_proc_pid_comm("easytier-web");

	if (pid > 0)
		return pid;
	return et_proc_pid_comm("easytier-web-em");
}

/* ---------------- /proc 采样 ---------------- */

/* 一次短窗口差分，同时算出 core / web 的 CPU 占用（%）。
 *
 * 不能用 (utime+stime)/HZ ÷ (/proc/uptime - starttime/HZ)：容器启用 time
 * namespace 后 /proc/uptime 与 /proc/<pid>/stat 里的 starttime 不同源
 * （实测 uptime=525s 而 starttime 折合 42 天），会算出负数。
 * 差分法只依赖两次自身采样，与 uptime 无关，也不受命名空间影响。
 * 两个进程共用同一次 120ms 窗口，总共只睡一次。 */
static void cpu_pair(pid_t a, pid_t b, double *pct_a, double *pct_b)
{
	unsigned long long a1 = 0, a2 = 0, b1 = 0, b2 = 0;
	bool ok_a, ok_b;
	const int window_ms = 120;
	double span;

	*pct_a = -1.0;
	*pct_b = -1.0;
	ok_a = a > 0 && et_proc_cpu_jiffies(a, &a1) == ET_OK;
	ok_b = b > 0 && et_proc_cpu_jiffies(b, &b1) == ET_OK;
	if (!ok_a && !ok_b)
		return;

	usleep((useconds_t)window_ms * 1000);

	span = (double)et_proc_hz() * (double)window_ms / 1000.0;
	if (span <= 0.0)
		return;
	if (ok_a && et_proc_cpu_jiffies(a, &a2) == ET_OK)
		*pct_a = (a2 >= a1 ? (double)(a2 - a1) : 0.0) * 100.0 / span;
	if (ok_b && et_proc_cpu_jiffies(b, &b2) == ET_OK)
		*pct_b = (b2 >= b1 ? (double)(b2 - b1) : 0.0) * 100.0 / span;
}

/* 未运行时旧实现输出空串（top/awk 无输出），保持一致 */
static char *cpu_text(double v)
{
	char buf[32];

	if (v < 0.0)
		return strdup("");
	snprintf(buf, sizeof(buf), "%.1f%%", v);
	return strdup(buf);
}

static char *proc_mem_text(pid_t pid)
{
	char buf[32];
	long kb = 0;

	if (pid <= 0 || et_proc_rss_kb(pid, &kb) < 0)
		return strdup("");
	snprintf(buf, sizeof(buf), "%.2f MB", (double)kb / 1024.0);
	return strdup(buf);
}

/* ---------------- 版本 ---------------- */

/* `<bin> -V` 输出中首个数字开始的版本号（等价于旧实现用 sed 去掉开头的非数字字符） */
static char *bin_version(const char *bin)
{
	char *argv[3];
	et_buf out;
	char *ver = NULL;

	if (!bin || !*bin)
		return strdup("unknown");
	argv[0] = (char *)bin;
	argv[1] = (char *)"-V";
	argv[2] = NULL;
	et_buf_init(&out);
	et_run(argv, &out, NULL, 10);
	if (out.p) {
		char *save = NULL;

		for (char *line = strtok_r(out.p, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char *p = line;

			while (*p && (*p < '0' || *p > '9'))
				p++;
			if (!*p)
				continue;
			et_trim_newline(p);
			ver = strdup(p);
			break;
		}
	}
	et_buf_free(&out);
	if (!ver || !*ver) {
		free(ver);
		return strdup("unknown");
	}
	return ver;
}

/* GitHub 最新 tag：只保留数字与点（等价于旧实现 grep tag_name 后再用 sed 过滤） */
static char *github_latest_tag(void)
{
	char *argv[] = {
		(char *)"curl", (char *)"-L", (char *)"-k", (char *)"-s",
		(char *)"--connect-timeout", (char *)"3",
		(char *)"--max-time", (char *)"4",
		(char *)"--user-agent", (char *)ET_UA_GITHUB,
		(char *)ET_URL_LATEST, NULL
	};
	et_buf out;
	char *tag = NULL;
	int rc;

	et_buf_init(&out);
	rc = et_run(argv, &out, NULL, 6);
	if (rc == 0 && out.p) {
		char *save = NULL;

		for (char *line = strtok_r(out.p, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			et_buf d;

			if (!strstr(line, "tag_name"))
				continue;
			et_buf_init(&d);
			for (const char *q = line; *q; q++) {
				if ((*q >= '0' && *q <= '9') || *q == '.')
					et_buf_addc(&d, *q);
			}
			tag = d.len ? et_buf_steal(&d) : (et_buf_free(&d), NULL);
			break;
		}
	}
	et_buf_free(&out);
	return tag;
}

/* 最新版本：命中 /tmp/easytiernew.tag 直接返回，否则联网；失败保留空串 */
static char *latest_tag(void)
{
	char *cached = et_api_tag_read(ET_TAG_LATEST);
	char *tag;
	int rc;

	if (cached)
		return cached;
	tag = github_latest_tag();
	if (!tag)
		return strdup("");
	rc = et_api_tag_write(ET_TAG_LATEST, tag);
	(void)rc;
	return tag;
}

/* 本机版本：命中缓存直接返回，否则跑 -V 后写回（结果恒非空，故恒写） */
static char *bin_tag(const char *cache_path, const char *bin)
{
	char *cached = et_api_tag_read(cache_path);
	char *tag;
	int rc;

	if (cached)
		return cached;
	tag = bin_version(bin);
	rc = et_api_tag_write(cache_path, tag);
	(void)rc;
	return tag;
}

/* ---------------- api_status ---------------- */

int et_api_status(const et_req *req, et_res *res)
{
	et_uci *u;
	char *core = NULL, *web = NULL, *cli = NULL;
	char *port_s, *s_cen, *s_wen, *s_notun, *s_tun;
	char *etsta, *websta, *etcpu, *etram, *webcpu, *webram;
	char *newtag, *ctag, *wtag;
	pid_t cpid, wpid;
	long port = 0;

	(void)req;
	u = et_uci_open();
	et_resolve_bins(u, &core, &web, &cli);
	free(cli);

	cpid = et_proc_pid_comm("easytier-core");
	wpid = et_api_pid_web();

	port_s = u ? et_uci_get_et(u, "web_html_port") : NULL;
	if (port_s && !et_parse_int(port_s, &port))
		port = 0;
	free(port_s);

	s_cen = u ? et_uci_get_et(u, "enabled") : NULL;
	s_wen = u ? et_uci_get_et(u, "web_enabled") : NULL;
	s_notun = u ? et_uci_get_et(u, "no_tun") : NULL;
	s_tun = u ? et_uci_get_et(u, "tunname") : NULL;

	etsta = et_api_uptime_text(ET_TIME_CORE);
	websta = et_api_uptime_text(ET_TIME_WEB);

	{
		double pc = -1.0, pw = -1.0;

		cpu_pair(cpid, wpid, &pc, &pw);
		etcpu = cpu_text(pc);
		webcpu = cpu_text(pw);
	}
	etram = proc_mem_text(cpid);
	webram = proc_mem_text(wpid);

	newtag = latest_tag();
	ctag = bin_tag(ET_TAG_CORE, core);
	wtag = bin_tag(ET_TAG_WEB, web);

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "crunning", cpid > 0);
	et_json_kv_bool(res, "wrunning", wpid > 0);
	et_json_kv_int(res, "port", port);
	et_json_kv_bool(res, "cenabled", s_cen && !strcmp(s_cen, "1"));
	et_json_kv_bool(res, "wenabled", s_wen && !strcmp(s_wen, "1"));
	et_json_kv_str(res, "etsta", etsta);
	et_json_kv_str(res, "etwebsta", websta);
	et_json_kv_str(res, "etcpu", etcpu);
	et_json_kv_str(res, "etram", etram);
	et_json_kv_str(res, "etwebcpu", webcpu);
	et_json_kv_str(res, "etwebram", webram);
	et_json_kv_str(res, "etnewtag", newtag);
	et_json_kv_str(res, "ettag", ctag);
	et_json_kv_str(res, "etwebtag", wtag);
	et_json_kv_bool(res, "no_tun", s_notun && !strcmp(s_notun, "1"));
	et_json_kv_str(res, "dev_name",
		       (s_tun && *s_tun) ? s_tun : "tun0");
	et_json_end(res);

	free(etsta);
	free(websta);
	free(etcpu);
	free(etram);
	free(webcpu);
	free(webram);
	free(newtag);
	free(ctag);
	free(wtag);
	free(s_cen);
	free(s_wen);
	free(s_notun);
	free(s_tun);
	free(core);
	free(web);
	et_uci_close(u);
	return ET_OK;
}

/* ---------------- get_tun_info ---------------- */

struct tun_info {
	char *ip;
	char *netmask;
	char *mtu;
	char *state;
	char *ipv6;
	long long rx;
	long long tx;
};

static bool ifname_ok(const char *s)
{
	size_t n = 0;

	if (!s || !*s || strlen(s) >= 16) /* IFNAMSIZ = 16，含结尾 NUL */
		return false;
	for (const char *p = s; *p; p++, n++) {
		if (isalnum((unsigned char)*p) || *p == '_' || *p == '-' ||
		    *p == '.' || *p == ':')
			continue;
		return false;
	}
	return n > 0;
}

static char *sysfs_read(const char *ifname, const char *leaf)
{
	char path[160];
	char *s;

	snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname, leaf);
	s = et_read_file(path, NULL);
	if (!s)
		return NULL;
	et_trim_newline(s);
	return s;
}

static bool looks_ipv4(const char *s, size_t n)
{
	int dots = 0;

	if (n < 7 || n > 15)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '.')
			dots++;
		else if (!isdigit((unsigned char)s[i]))
			return false;
	}
	return dots == 3;
}

/* 在 ifconfig / ip addr 输出里找第一个 IPv4；顺带带回 CIDR 前缀长度 */
static char *find_inet4(const char *text, int *prefix_out)
{
	const char *p = text;

	if (prefix_out)
		*prefix_out = -1;
	while (p && (p = strstr(p, "inet")) != NULL) {
		const char *q, *st;
		int len = -1;

		p += 4;
		if (*p == '6' || (*p != ' ' && *p != '\t'))
			continue;
		q = p;
		if (!strncmp(q, " addr:", 6))
			q += 6;
		while (*q == ' ' || *q == '\t')
			q++;
		st = q;
		while (isdigit((unsigned char)*q) || *q == '.')
			q++;
		if (!looks_ipv4(st, (size_t)(q - st)))
			continue;
		if (*q == '/')
			len = (int)strtol(q + 1, NULL, 10);
		if (prefix_out)
			*prefix_out = len;
		return strndup(st, (size_t)(q - st));
	}
	return NULL;
}

static char *find_netmask(const char *text)
{
	const char *p = strstr(text, "Mask:");

	if (!p)
		p = strstr(text, "netmask ");
	while (p) {
		const char *q, *st;

		q = p;
		if (!strncmp(q, "Mask:", 5))
			q += 5;
		else
			q += 8;
		while (*q == ' ' || *q == '\t')
			q++;
		st = q;
		while (isdigit((unsigned char)*q) || *q == '.')
			q++;
		if (looks_ipv4(st, (size_t)(q - st)))
			return strndup(st, (size_t)(q - st));
		p = strstr(q, "Mask:");
		if (!p)
			p = strstr(q, "netmask ");
	}
	return NULL;
}

/* 旧实现是 ifconfig 输出里 match("UP") / match("DOWN")，这里保持同一判据 */
static char *find_state(const char *text)
{
	if (strstr(text, "UP"))
		return strdup("UP");
	if (strstr(text, "DOWN"))
		return strdup("DOWN");
	return strdup("UNKNOWN");
}

static void prefix_to_mask(int len, char *out, size_t n)
{
	uint32_t m;

	if (len < 0)
		len = 0;
	if (len > 32)
		len = 32;
	m = len == 0 ? 0u : (uint32_t)(0xFFFFFFFFu << (32 - len));
	snprintf(out, n, "%u.%u.%u.%u", (unsigned)(m >> 24),
		 (unsigned)((m >> 16) & 0xFF), (unsigned)((m >> 8) & 0xFF),
		 (unsigned)(m & 0xFF));
}

/* 32 位十六进制 → 压缩形式 "fe80::1/64"（与 ip -6 addr 的显示风格一致） */
static char *ipv6_format(const char *hex, int plen)
{
	unsigned v[8];
	int best = -1, bestlen = 0;
	bool prev_colon = false;

	for (int i = 0; i < 8; i++) {
		char t[5];

		memcpy(t, hex + i * 4, 4);
		t[4] = '\0';
		v[i] = (unsigned)strtoul(t, NULL, 16);
	}
	for (int i = 0; i < 8;) {
		int j = i;

		if (v[i]) {
			i++;
			continue;
		}
		while (j < 8 && !v[j])
			j++;
		if (j - i > bestlen) {
			best = i;
			bestlen = j - i;
		}
		i = j;
	}
	if (bestlen < 2)
		best = -1;

	et_buf b;
	et_buf_init(&b);
	for (int i = 0; i < 8; i++) {
		if (best >= 0 && i == best) {
			et_buf_add(&b, "::");
			i += bestlen - 1;
			prev_colon = true;
			continue;
		}
		if (i > 0 && !prev_colon)
			et_buf_addc(&b, ':');
		et_buf_addf(&b, "%x", v[i]);
		prev_colon = false;
	}
	et_buf_addf(&b, "/%d", plen);
	return et_buf_steal(&b);
}

/* /proc/net/if_inet6 一文件取全部 IPv6，不再 fork ip -6 addr（旧实现 3 个进程） */
static char *find_ipv6(const char *ifname)
{
	char *s = et_read_file("/proc/net/if_inet6", NULL);
	char *best = NULL, *save = NULL;

	if (!s)
		return NULL;
	for (char *line = strtok_r(s, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char hex[40], name[64];
		unsigned idx, plen, scope, flags;
		char *txt;

		if (sscanf(line, "%39s %x %x %x %x %63s", hex, &idx, &plen,
			   &scope, &flags, name) != 6)
			continue;
		if (strcmp(name, ifname))
			continue;
		if (strlen(hex) != 32)
			continue;
		txt = ipv6_format(hex, (int)plen);
		if (!txt)
			continue;
		if (scope == 0) { /* 全局地址优先，链路本地兜底 */
			free(best);
			best = txt;
			break;
		}
		if (!best)
			best = txt;
		else
			free(txt);
	}
	free(s);
	return best;
}

static void tun_reply_missing(et_res *res)
{
	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", false);
	et_json_kv_bool(res, "exists", false);
	et_json_end(res);
}

int et_api_tun_info(const et_req *req, et_res *res)
{
	char *ifname_arg = et_api_arg(req, "ifname");
	const char *ifname = (ifname_arg && *ifname_arg) ? ifname_arg : "tun0";
	struct tun_info ti = { .rx = 0, .tx = 0 };
	char dir[160];
	char ebuf[64];
	int prefix = -1;

	if (!ifname_ok(ifname)) {
		free(ifname_arg);
		tun_reply_missing(res);
		return ET_EINVAL;
	}
	snprintf(dir, sizeof(dir), "/sys/class/net/%s", ifname);
	if (!et_is_dir(dir)) {
		free(ifname_arg);
		tun_reply_missing(res);
		return ET_OK;
	}

	ti.rx = 0;
	ti.tx = 0;
	{
		char *v = sysfs_read(ifname, "statistics/rx_bytes");

		if (v)
			ti.rx = strtoll(v, NULL, 10);
		free(v);
		v = sysfs_read(ifname, "statistics/tx_bytes");
		if (v)
			ti.tx = strtoll(v, NULL, 10);
		free(v);
	}
	/* MTU 内核直接给出，省掉 ifconfig 与 ip link 两次解析 */
	ti.mtu = sysfs_read(ifname, "mtu");

	if (et_have_cmd("ifconfig")) {
		char *argv[] = { (char *)"ifconfig", (char *)ifname, NULL };
		et_buf out, err;

		et_buf_init(&out);
		et_buf_init(&err);
		et_run(argv, &out, &err, 5);
		if (out.p) {
			ti.ip = find_inet4(out.p, &prefix);
			ti.netmask = find_netmask(out.p);
			ti.state = find_state(out.p);
		}
		et_buf_free(&out);
		et_buf_free(&err);
	}

	if (!ti.ip) {
		/* 没有 ifconfig（或解析不到地址）：一次 ip addr 拿到地址与状态 */
		char *argv[] = { (char *)"ip", (char *)"addr", (char *)"show",
				 (char *)"dev", (char *)ifname, NULL };
		et_buf out, err;

		et_buf_init(&out);
		et_buf_init(&err);
		et_run(argv, &out, &err, 5);
		if (out.p) {
			ti.ip = find_inet4(out.p, &prefix);
			if (!ti.netmask)
				ti.netmask = find_netmask(out.p);
			if (!ti.state)
				ti.state = find_state(out.p);
		}
		et_buf_free(&out);
		et_buf_free(&err);
	}

	if (!ti.netmask && prefix >= 0) {
		prefix_to_mask(prefix, ebuf, sizeof(ebuf));
		ti.netmask = strdup(ebuf);
	}
	ti.ipv6 = find_ipv6(ifname);

	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_bool(res, "exists", true);
	et_json_kv_str(res, "ip", ti.ip ? ti.ip : "");
	et_json_kv_str(res, "netmask", ti.netmask ? ti.netmask : "");
	et_json_kv_str(res, "mtu", ti.mtu ? ti.mtu : "");
	et_json_kv_str(res, "state", ti.state ? ti.state : "UNKNOWN");
	et_json_kv_int(res, "rx_bytes", ti.rx);
	et_json_kv_int(res, "tx_bytes", ti.tx);
	if (ti.ipv6 && *ti.ipv6)
		et_json_kv_str(res, "ipv6", ti.ipv6);
	et_json_end(res);

	free(ti.ip);
	free(ti.netmask);
	free(ti.mtu);
	free(ti.state);
	free(ti.ipv6);
	free(ifname_arg);
	return ET_OK;
}

/* ---------------- get_disk_space ---------------- */

static void disk_size_field(et_res *res, const char *key, const char *path)
{
	long long sz = et_file_size(path);
	char human[32];

	if (sz < 0)
		return;
	et_human_size(sz, human, sizeof(human));
	et_json_kv_str(res, key, human);
}

int et_api_disk_space(const et_req *req, et_res *res)
{
	char *path = et_api_arg(req, "path");
	char *full = et_api_arg(req, "full_path");
	char *type = et_api_arg(req, "type");
	long long kb;
	char human[32];

	if (!path || !*path) {
		et_json_result(res, false, "Invalid path");
		free(path);
		free(full);
		free(type);
		return ET_EINVAL;
	}

	kb = et_df_available_kb(path);
	if (kb < 0) {
		et_json_result(res, false, "Unable to query disk space");
		free(path);
		free(full);
		free(type);
		return ET_OK;
	}

	et_human_size(kb * 1024, human, sizeof(human));
	res->type = ET_CT_JSON;
	et_json_begin(res);
	et_json_kv_bool(res, "success", true);
	et_json_kv_str(res, "available", human);
	et_json_kv_int(res, "available_mb", kb / 1024);

	if (type && !strcmp(type, "easytierbin") && full && *full) {
		disk_size_field(res, "core_size", full);
		char *dir = et_dirname_keep_slash(full);
		char *cli = NULL;

		if (asprintf(&cli, "%seasytier-cli", dir ? dir : "/usr/bin/") < 0)
			cli = NULL;
		if (cli)
			disk_size_field(res, "cli_size", cli);
		free(cli);
		free(dir);
	} else if (type && !strcmp(type, "webbin") && full && *full) {
		disk_size_field(res, "web_size", full);
	}
	et_json_end(res);

	free(path);
	free(full);
	free(type);
	return ET_OK;
}
