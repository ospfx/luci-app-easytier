/*
 * etd_main.c - easytierd 入口（服务生命周期）
 *
 * 用法（由极薄 init.d 委托脚本调用）：
 *   easytierd service start    后台启动 core/web
 *   easytierd service run      前台运行并监督（供 procd 使用）
 *   easytierd service stop
 *   easytierd service restart
 *   easytierd service reload
 *   easytierd service status   0=运行中
 *   easytierd service enable|disable
 *   easytierd api <method> [k=v...]   —— 单次调用，调试用
 *   easytierd version
 */
#include "etd.h"
#include "etd_api.h"
#include "etd_service.h"
#include "etd_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *fp)
{
	fprintf(fp,
		"%s %s - EasyTier 服务守护进程\n"
		"用法:\n"
		"  %s service start|run|stop|restart|reload|status|enable|disable\n"
		"  %s api <method> [key=value ...]\n"
		"  %s version\n",
		ETD_PROG, ETD_VERSION, ETD_PROG, ETD_PROG, ETD_PROG);
}

static int cmd_service(int argc, char **argv)
{
	if (argc < 1) {
		usage(stderr);
		return 2;
	}
	const char *sub = argv[0];
	if (!strcmp(sub, "start"))
		return et_service_start(false) == ET_OK ? 0 : 1;
	if (!strcmp(sub, "run"))
		return et_service_start(true) == ET_OK ? 0 : 1;
	if (!strcmp(sub, "stop"))
		return et_service_stop() == ET_OK ? 0 : 1;
	if (!strcmp(sub, "restart") || !strcmp(sub, "reload"))
		return et_service_restart() == ET_OK ? 0 : 1;
	if (!strcmp(sub, "status"))
		return et_service_running() ? 0 : 3;
	if (!strcmp(sub, "enable"))
		return et_service_enable(true) == ET_OK ? 0 : 1;
	if (!strcmp(sub, "disable"))
		return et_service_enable(false) == ET_OK ? 0 : 1;
	usage(stderr);
	return 2;
}

static int cmd_api(int argc, char **argv)
{
	if (argc < 1) {
		usage(stderr);
		return 2;
	}
	et_list params;
	et_list_init(&params);
	for (int i = 1; i < argc; i++)
		et_list_add(&params, argv[i]);

	et_req req = { .method = argv[0], .params = &params, .body = NULL, .body_len = 0 };
	et_res res;
	et_res_init(&res, ET_CT_JSON);
	int rc = et_api_dispatch(&req, &res);
	if (rc == ET_ENOENT)
		fprintf(stderr, "unknown method: %s\n", argv[0]);
	if (res.body.p)
		fputs(res.body.p, stdout);
	if (res.body.p && res.body.len && res.body.p[res.body.len - 1] != '\n')
		fputc('\n', stdout);
	et_res_free(&res);
	et_list_free(&params);
	return rc == ET_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
	int i = 1;
	while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			et_debug = 1;
			i++;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
			printf("%s %s\n", ETD_PROG, ETD_VERSION);
			return 0;
		} else {
			fprintf(stderr, "未知选项: %s\n", argv[i]);
			return 2;
		}
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}

	const char *cmd = argv[i++];
	if (!strcmp(cmd, "service"))
		return cmd_service(argc - i, argv + i);
	if (!strcmp(cmd, "api"))
		return cmd_api(argc - i, argv + i);
	if (!strcmp(cmd, "version")) {
		printf("%s %s\n", ETD_PROG, ETD_VERSION);
		return 0;
	}
	usage(stderr);
	return 2;
}
