/*
 * cli_probe.c - 开发用探针：只跑 CLI 子命令（不链接 api/service/daemon）
 *
 * 用途：在路由器上直接对拍上游 easytier-cli 的输出，
 * 不依赖 libuci/json-c，因此可以静态交叉编译。
 */
#include "etd_cli.h"

#include <stdio.h>
#include <string.h>

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
			i++;
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
			i++;
		} else if ((!strcmp(a, "-i") || !strcmp(a, "--instance-id")) &&
			   i + 1 < argc) {
			o.instance_id = argv[++i];
			i++;
		} else if ((!strcmp(a, "-n") || !strcmp(a, "--instance-name")) &&
			   i + 1 < argc) {
			o.instance_name = argv[++i];
			i++;
		} else {
			fprintf(stderr, "probe: unknown option %s\n", a);
			return 2;
		}
	}
	if (i >= argc) {
		fprintf(stderr,
			"usage: cli_probe [OPTIONS] <peer|route|node|connector|...>\n");
		return 2;
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.opts = o;
	et_buf_init(&ctx.out);
	rc = et_cli_dispatch(&ctx, argv[i], argc - i - 1, argv + i + 1);
	(void)et_cli_flush(&ctx);
	et_buf_free(&ctx.out);
	et_cli_ctx_close(&ctx);
	return rc == 0 ? 0 : 1;
}
