/*
 * etd_cli.h - 与上游 easytier-cli 对齐的子命令实现
 */
#ifndef ETD_CLI_H
#define ETD_CLI_H

/* 下面三个都通过 core 的 RPC（默认 127.0.0.1:15888）取数，
 * 输出与上游 easytier-cli 同名的 Markdown 表格。 */
int et_cli_peer(const char *rpc_addr);
int et_cli_route(const char *rpc_addr);
int et_cli_node(const char *rpc_addr);

#endif /* ETD_CLI_H */
