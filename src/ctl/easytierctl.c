/*
 * easytierctl.c - 管理 CLI（安装名 easytier-cli，与上游同名）
 *
 * 上游同名子命令（peer/route/node/connector/…）全部由 lib/etd_cli_*.c 实现，
 * 输出与上游 easytier-cli 2.7.0 对齐；本文件只负责全局选项解析、分发，
 * 以及本项目的额外子命令：
 *   easytier-cli api <method> [k=v ...] [--body|--body-file]
 *   easytier-cli service <start|stop|restart|status|enable|disable>
 *   easytier-cli doctor
 *   easytier-cli version
 */
#include "etd.h"
#include "etd_api.h"
#include "etd_cli.h"
#include "etd_proc.h"
#include "etd_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void usage(FILE *fp)
{
	fprintf(fp,
		"A full meshed p2p VPN, connecting all your devices in one network with one command.\n"
		"\n"
		"Usage: easytier-cli [OPTIONS] <COMMAND>\n"
		"\n"
		"Commands:\n"
		"  peer              show peers info\n"
		"  connector         manage connectors\n"
		"  mapped-listener   manage mapped listeners\n"
		"  stun              do stun test\n"
		"  route             show route info\n"
		"  peer-center       show global peers info\n"
		"  vpn-portal        show vpn portal (wireguard) info\n"
		"  node              inspect self easytier-core status\n"
		"  service           manage easytier-core as a system service\n"
		"  proxy             show tcp/kcp proxy status\n"
		"  acl               show ACL rules statistics\n"
		"  port-forward      manage port forwarding\n"
		"  whitelist         manage TCP/UDP whitelist\n"
		"  stats             show statistics information\n"
		"  logger            manage logger configuration\n"
		"  credential        manage temporary credentials\n"
		"  gen-autocomplete  generate shell completions\n"
		"  api               call an internal LuCI handler (this build only)\n"
		"  doctor            environment self-check (this build only)\n"
		"  version           print version (this build only)\n"
		"  help              Print this message or the help of the given subcommand(s)\n"
		"\n"
		"Options:\n"
		"  -p, --rpc-portal <addr>    easytier-core rpc portal address [default: 127.0.0.1:15888]\n"
		"  -v, --verbose              verbose output\n"
		"  -o, --output <format>      output format [default: table] [possible values: table, json]\n"
		"      --no-trunc             disable column truncation\n"
		"  -i, --instance-id <id>     the instance id\n"
		"  -n, --instance-name <name> the instance name\n"
		"  -h, --help                 Print help\n"
		"  -V, --version              Print version\n");
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
		fprintf(stderr, "easytier-cli: 未知方法 %s\n", argv[0]);

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
	/* 服务生命周期实现在 easytierd 里，这里只做转发，避免逻辑双份。 */
	et_buf cmd;
	et_buf_init(&cmd);
	et_buf_add(&cmd, ET_PATH_DAEMON);
	/* easytierd 的入口是 "easytierd service <sub>"，这里必须补上 service，
	 * 否则会以 "easytierd <sub>" 调用而只打印用法（真机验证过的 bug） */
	et_buf_add(&cmd, " service");
	for (int i = 0; i < argc; i++) {
		char *q = et_shell_quote(argv[i]);

		if (!q) {
			et_buf_free(&cmd);
			return 1;
		}
		et_buf_add(&cmd, " ");
		et_buf_add(&cmd, q);
		free(q);
	}
	int rc = system(cmd.p);

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
	printf("easytier-cli %s\n", ETD_VERSION);
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

/* 全局选项必须出现在子命令之前；子命令自己的参数原样传下去 */
int main(int argc, char **argv)
{
	struct et_cli_opts o;
	struct et_cli_ctx ctx;
	int i = 1;
	int rc;

	et_cli_opts_init(&o);
	while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
		const char *a = argv[i];

		if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
			o.verbose = 1;
			et_debug = 1;
			i++;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			printf("easytier-cli %s\n", ETD_VERSION);
			return 0;
		} else if (!strcmp(a, "--no-trunc")) {
			o.no_trunc = 1;
			i++;
		} else if ((!strcmp(a, "-p") || !strcmp(a, "--rpc-portal")) &&
			   i + 1 < argc) {
			o.rpc = argv[++i];
			i++;
		} else if ((!strcmp(a, "-o") || !strcmp(a, "--output")) &&
			   i + 1 < argc) {
			o.format = argv[++i];
			if (strcmp(o.format, "table") && strcmp(o.format, "json")) {
				fprintf(stderr,
					"easytier-cli: invalid output format %s\n",
					o.format);
				return 2;
			}
			i++;
		} else if ((!strcmp(a, "-i") || !strcmp(a, "--instance-id")) &&
			   i + 1 < argc) {
			o.instance_id = argv[++i];
			i++;
		} else if ((!strcmp(a, "-n") || !strcmp(a, "--instance-name")) &&
			   i + 1 < argc) {
			o.instance_name = argv[++i];
			i++;
		} else if (!strcmp(a, "-o") || !strcmp(a, "--output") ||
			   !strcmp(a, "-p") || !strcmp(a, "--rpc-portal") ||
			   !strcmp(a, "-i") || !strcmp(a, "--instance-id") ||
			   !strcmp(a, "-n") || !strcmp(a, "--instance-name")) {
			fprintf(stderr,
				"easytier-cli: missing value for option %s\n", a);
			return 2;
		} else {
			fprintf(stderr, "未知选项: %s\n", a);
			return 2;
		}
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}

	const char *cmd = argv[i++];
	int sub_argc = argc - i;
	char **sub_argv = argv + i;

	if (!strcmp(cmd, "help")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(cmd, "version")) {
		printf("easytier-cli %s\n", ETD_VERSION);
		return 0;
	}
	if (!strcmp(cmd, "doctor"))
		return do_doctor();
	if (!strcmp(cmd, "api"))
		return do_api(sub_argc, sub_argv);
	if (!strcmp(cmd, "service"))
		return do_service(sub_argc, sub_argv);

	/* 上游同名子命令 */
	memset(&ctx, 0, sizeof(ctx));
	ctx.opts = o;
	et_buf_init(&ctx.out);
	rc = et_cli_dispatch(&ctx, cmd, sub_argc, sub_argv);
	if (rc == ET_ENOENT) {
		et_buf_free(&ctx.out);
		et_cli_ctx_close(&ctx);
		usage(stderr);
		return 2;
	}
	(void)et_cli_flush(&ctx);
	et_buf_free(&ctx.out);
	et_cli_ctx_close(&ctx);
	return rc == 0 ? 0 : 1;
}
