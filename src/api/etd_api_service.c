/*
 * etd_api_service.c - 服务控制 API（restart_service / toggle_core / toggle_web）
 *
 * 原 Lua 端点的行为是"改配置 → 后台触发 init.d 脚本 → 立刻返回 success"：
 *   restart_service() → /etc/init.d/easytier restart >/dev/null 2>&1 &
 *   toggle_core()     → uci set enabled=… ; commit ; enabled==1 ? start : restart
 *   toggle_web()      → uci set web_enabled=… ; commit ; 同上
 * 这里保持同样的语义：配置同步写完，动作丢给脱离会话的子进程，接口立即返回。
 */
#include "etd_api.h"
#include "etd_service.h"
#include "etd_uci.h"
#include "etd_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool has_val(const char *s)
{
	return s && *s;
}

/* 把耗时动作丢到后台执行，立即返回（对齐旧实现的 `... &`） */
static int et_api_async(void (*fn)(void))
{
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return ET_ERR;
	if (pid == 0) {
		/* 双 fork：中间进程立刻退出，动作进程不再挂在调用方进程组里 */
		pid_t pid2 = fork();
		if (pid2 != 0)
			_exit(0);
		setsid();
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		fn();
		_exit(0);
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
	return ET_OK;
}

static void et_api_do_restart(void)
{
	et_service_restart();
}

static void et_api_do_start(void)
{
	et_service_start(false);
}

int et_api_service_restart(const et_req *req, et_res *res)
{
	(void)req;
	et_api_async(et_api_do_restart);
	et_json_result(res, true, NULL);
	return ET_OK;
}

/* toggle 公共实现：enabled 可来自查询串（k=v）或 JSON body */
static int et_api_toggle(const et_req *req, et_res *res, const char *opt)
{
	const char *val = et_req_get(req, "enabled");
	char *body_val = NULL;
	if (!val) {
		body_val = et_body_json_str(req, "enabled");
		val = body_val;
	}
	if (!has_val(val)) {
		free(body_val);
		et_json_result(res, false, "缺少 enabled 参数");
		return ET_EINVAL;
	}
	bool on = !strcmp(val, "1");

	et_uci *u = et_uci_open();
	if (!u) {
		free(body_val);
		et_json_result(res, false, "uci 配置不可用");
		return ET_ERR;
	}
	int rc = et_uci_set_et(u, opt, on ? "1" : "0");
	if (rc == 0)
		rc = et_uci_commit_et(u);
	et_uci_close(u);
	free(body_val);

	if (rc != 0) {
		et_json_result(res, false, "配置写入失败");
		return ET_ERR;
	}

	/* 旧实现：开启 → start；关闭 → restart（stop 时按开关清理规则） */
	et_api_async(on ? et_api_do_start : et_api_do_restart);
	et_json_result(res, true, NULL);
	return ET_OK;
}

int et_api_toggle_core(const et_req *req, et_res *res)
{
	return et_api_toggle(req, res, "enabled");
}

int et_api_toggle_web(const et_req *req, et_res *res)
{
	return et_api_toggle(req, res, "web_enabled");
}
