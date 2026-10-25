/*
 * easytierctl.c - 管理 CLI
 *
 * 极薄 Lua 转发层只调用本程序，不包含任何业务逻辑：
 *   easytierctl api <method> [key=value ...]     输出 handler 的原始响应
 *   easytierctl api <method> --body-file <path>  附带 POST 体
 *   easytierctl service <start|stop|restart|status|enable|disable>
 *   easytierctl version
 *   easytierctl doctor                          环境自检
 */
#include "etd.h"
#include "etd_api.h"
#include "etd_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *fp)
{
	fprintf(fp,
		"easytierctl %s - luci-app-easytier 管理 CLI\n"
		"用法:\n"
		"  easytierctl api <method> [key=value ...] [--body-file <path>]\n"
		"  easytierctl service <start|stop|restart|status|enable|disable>\n"
		"  easytierctl version | doctor\n",
		ETD_VERSION);
}

static int do_api(int argc, char **argv)
{
	if (argc < 1) {
		usage(stderr);
		return 2;
	}
	et_list params;
	et_list_init(&params);
	char *body = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--body-file") && i + 1 < argc) {
			size_t len = 0;
			body = et_read_file(argv[++i], &len);
		} else if (!strcmp(argv[i], "--body") && i + 1 < argc) {
			body = strdup(argv[++i]);
		} else {
			et_list_add(&params, argv[i]);
		}
	}

	et_req req = {
		.method = argv[0],
		.params = &params,
		.body = body,
		.body_len = body ? strlen(body) : 0,
	};
	et_res res;
	et_res_init(&res, ET_CT_JSON);
	int rc = et_api_dispatch(&req, &res);

	if (rc == ET_ENOENT)
		fprintf(stderr, "easytierctl: 未知方法 %s\n", argv[0]);

	if (res.body.p && res.body.len) {
		fwrite(res.body.p, 1, res.body.len, stdout);
		if (res.body.p[res.body.len - 1] != '\n')
			fputc('\n', stdout);
	}
	et_res_free(&res);
	et_list_free(&params);
	free(body);
	return rc == ET_OK ? 0 : 1;
}

static int do_service(int argc, char **argv)
{
	if (argc < 1) {
		usage(stderr);
		return 2;
	}
	/* 服务生命周期实现在 easytierd 里，这里只做转发，避免逻辑双份 */
	et_buf cmd;
	et_buf_init(&cmd);
	et_buf_add(&cmd, "/usr/sbin/easytierd service ");
	et_buf_add(&cmd, argv[0]);
	char *q = et_shell_quote(cmd.p);
	int rc = system(q);
	free(q);
	et_buf_free(&cmd);
	if (rc < 0)
		return 1;
	if (WIFEXITED(rc))
		return WEXITSTATUS(rc);
	return 1;
}

static int do_doctor(void)
{
	char ts[32];
	et_now_str(ts, sizeof(ts));
	printf("easytierctl %s\n", ETD_VERSION);
	printf("时间            : %s\n", ts);
	printf("uci 配置        : %s (%s)\n", ET_FILE_UCI,
	       et_file_exists(ET_FILE_UCI) ? "存在" : "缺失");
	printf("core 二进制     : %s (%s)\n", ET_PATH_CORE_DEF,
	       et_file_executable(ET_PATH_CORE_DEF) ? "可执行" : "不可执行");
	printf("web 二进制      : %s (%s)\n", ET_PATH_WEB_DEF,
	       et_file_executable(ET_PATH_WEB_DEF) ? "可执行" : "不可执行");

	const char *arch = et_detect_arch();
	printf("CPU 架构        : %s\n", arch ? arch : "未知");

	const char *tools[] = { "curl", "wget", "unzip", "tar", "ping" };
	printf("外部工具        : ");
	for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
		printf("%s=%s ", tools[i], et_have_cmd(tools[i]) ? "有" : "无");
	printf("\n");

	long long kb = et_df_available_kb("/tmp");
	char human[32] = "";
	if (kb >= 0)
		et_human_size(kb * 1024, human, sizeof(human));
	printf("/tmp 可用空间   : %s\n", human);

	long long lk = et_file_size(ET_LOG_CORE);
	printf("core 日志大小   : %lld 字节\n", lk);

	pid_t pid = et_proc_pid_comm("easytier-core");
	printf("easytier-core   : %s\n", pid > 0 ? "运行中" : "未运行");
	pid = et_proc_pid_comm("easytier-web");
	printf("easytier-web    : %s\n", pid > 0 ? "运行中" : "未运行");
	return 0;
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
			printf("easytierctl %s\n", ETD_VERSION);
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
	if (!strcmp(cmd, "api"))
		return do_api(argc - i, argv + i);
	if (!strcmp(cmd, "service"))
		return do_service(argc - i, argv + i);
	if (!strcmp(cmd, "version")) {
		printf("easytierctl %s\n", ETD_VERSION);
		return 0;
	}
	if (!strcmp(cmd, "doctor"))
		return do_doctor();
	usage(stderr);
	return 2;
}
