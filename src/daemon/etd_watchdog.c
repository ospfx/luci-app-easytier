/*
 * etd_watchdog.c - checkip 通断检测看门狗
 *
 * 对齐旧 /tmp/easytier_check 脚本的行为：
 *   1) 间隔 checktime*60 秒；checktime 缺失/非法时用 120 秒并把 checktime 写回 2；
 *   2) 先用 `ping -c 3 223.5.5.5 -w 5` 判断外网可用，不可用则跳过本轮；
 *   3) 再逐个 ping checkip 列表，延迟/中断都写进 /tmp/easytier.log；
 *   4) 全部不可达 → 重启服务（这里向监督进程发 SIGUSR1）；
 *   5) enabled != 1 → 记一条日志后退出（旧实现此时 restart 等于停服）。
 * 与旧实现的差异：
 *   - 不再生成 /tmp/easytier_check 脚本文件，也不会出现 `du -k ... = 1000` 这种
 *     只在恰好 1000KB 时才清空的边角逻辑（日志轮转由监督进程统一负责）。
 */
#include "etd_watchdog.h"
#include "etd_util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#define ET_WD_DEFAULT_SEC 120
#define ET_WD_MAX_SEC (24L * 3600L)
#define ET_WD_PROBE_HOST "223.5.5.5"

bool et_watchdog_wanted(et_cfg *c)
{
	if (!c)
		return false;
	/* checkip 在 UCI 里是 list（可与 `uci get` 一样用空格连接看待），
	 * 任一元素非空就要跑看门狗 —— 对应旧实现 `[ ! -z "$checkip" ]`。 */
	const et_list *l = et_cfg_list(c, "checkip");
	if (l && l->n > 0) {
		for (size_t i = 0; i < l->n; i++) {
			if (!et_is_blank(l->v[i]))
				return true;
		}
		return false;
	}
	return !et_is_blank(et_cfg_str(c, "checkip"));
}

/* 读 checktime（分钟）→ 秒；缺失/非法时写回 checktime=2 并返回 120 秒 */
static long wd_interval_sec(void)
{
	long sec = ET_WD_DEFAULT_SEC;
	et_uci *u = et_uci_open();
	if (!u)
		return sec;

	char *t = et_uci_get_et(u, "checktime");
	long v = 0;
	if (t && et_parse_int(t, &v) && v > 0) {
		sec = v * 60;
		if (sec > ET_WD_MAX_SEC)
			sec = ET_WD_MAX_SEC;
	} else {
		/* 旧实现：time 算不出来（checktime 为空）时 time=120 且写回 checktime=2 */
		et_uci_set_et(u, "checktime", "2");
		et_uci_commit_et(u);
		sec = ET_WD_DEFAULT_SEC;
	}
	free(t);
	et_uci_close(u);
	return sec;
}

/* 外网是否可用：ping -c 3 223.5.5.5 -w 5 */
static bool wd_wan_ok(void)
{
	char *argv[] = { (char *)"/bin/ping", (char *)"-c", (char *)"3",
			 (char *)ET_WD_PROBE_HOST, (char *)"-w", (char *)"5",
			 NULL };
	return et_run(argv, NULL, NULL, 20) == 0;
}

/* 从 ping -q 的统计行里取平均延迟。
 * busybox:  "round-trip min/avg/max = 1.1/2.2/3.3 ms"   → 第 2 个数
 * iputils:  "rtt min/avg/max/mdev = 1.1/2.2/3.3/0.1 ms" → 第 2 个数
 * 旧实现用 awk -F '/' '{print $4}' 在 busybox 下正好命中平均值，这里显式取 avg。 */
static char *wd_parse_avg(const char *out)
{
	if (!out)
		return NULL;
	const char *p = strstr(out, "min/avg/max");
	if (!p)
		return NULL;
	p = strchr(p, '=');
	if (!p)
		return NULL;
	p++;
	while (*p == ' ')
		p++;

	/* 跳过最小值，取第二个数 */
	const char *slash = strchr(p, '/');
	if (!slash)
		return NULL;
	slash++;
	const char *end = slash;
	while (*end && *end != '/' && *end != ' ')
		end++;
	if (end == slash)
		return NULL;
	return strndup(slash, (size_t)(end - slash));
}

static char *wd_ping_avg(const char *ip)
{
	char *argv[] = { (char *)"/bin/ping", (char *)"-4", (char *)ip,
			 (char *)"-c",	       (char *)"2",  (char *)"-w",
			 (char *)"4",	       (char *)"-q", NULL };
	et_buf o, e;
	et_buf_init(&o);
	et_buf_init(&e);

	char *avg = NULL;
	if (et_run(argv, &o, &e, 15) == 0)
		avg = wd_parse_avg(o.p ? o.p : (e.p ? e.p : NULL));

	et_buf_free(&o);
	et_buf_free(&e);
	return avg;
}

int et_watchdog_main(pid_t supervisor)
{
#ifdef __linux__
	/* 监督进程消失时自己也退出，避免留下孤儿看门狗 */
	prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
	long interval = wd_interval_sec();

	for (;;) {
		et_uci *u = et_uci_open();
		if (!u)
			return ET_ERR;
		char *enabled = et_uci_get_et(u, "enabled");
		char *checkip = et_uci_get_et(u, "checkip");
		et_uci_close(u);

		if (!enabled || strcmp(enabled, "1") != 0) {
			et_log_to(ET_LOG_CORE, "通断检测：程序已关闭，退出检测");
			free(enabled);
			free(checkip);
			/* 旧实现此处 restart：enabled=0 时重启等价于停服 */
			if (supervisor > 1)
				kill(supervisor, SIGTERM);
			return ET_OK;
		}

		if (et_is_blank(checkip)) {
			free(enabled);
			free(checkip);
			sleep((unsigned)interval);
			continue;
		}

		if (!wd_wan_ok()) {
			et_log_to(ET_LOG_CORE,
				  "通断检测：检测到互联网未能成功访问，跳过检测");
		} else {
			et_list ips;
			et_list_init(&ips);
			et_list_split_ws(&ips, checkip);

			bool online = false;
			for (size_t i = 0; i < ips.n; i++) {
				char *ms = wd_ping_avg(ips.v[i]);
				if (ms && *ms) {
					et_log_to(ET_LOG_CORE,
						  "通断检测：%s 延迟：%sms",
						  ips.v[i], ms);
					online = true;
				} else {
					et_log_to(ET_LOG_CORE,
						  "通断检测：%s 网络中断",
						  ips.v[i]);
				}
				free(ms);
			}

			if (!online && ips.n > 0) {
				et_log_to(ET_LOG_CORE,
					  "通断检测：%s 所有指定IP皆无法ping通,重新启动程序！",
					  checkip);
				if (supervisor > 1)
					kill(supervisor, SIGUSR1);
			}
			et_list_free(&ips);
		}

		free(enabled);
		free(checkip);
		sleep((unsigned)interval);
	}
}
