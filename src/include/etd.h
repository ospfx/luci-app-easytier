/*
 * etd.h - luci-app-easytier C 重写版公共定义
 *
 * 移植原则：所有路径、文件名、日志格式与原 Lua/Shell 实现保持逐字一致，
 * 以便新旧版本可互换、回滚，并让现有前端页面无需改动即可工作。
 */
#ifndef ETD_H
#define ETD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ETD_PROG "easytierd"
#define ETD_VERSION "1.0.0"

/* ---- 可外部覆盖的路径（测试时可 -D 覆盖） ---- */
#ifndef ET_PATH_CORE_DEF
#define ET_PATH_CORE_DEF "/usr/bin/easytier-core"
#endif
#ifndef ET_PATH_WEB_DEF
#define ET_PATH_WEB_DEF "/usr/bin/easytier-web"
#endif
#ifndef ET_FILE_UCI
#define ET_FILE_UCI "/etc/config/easytier"
#endif
#ifndef ET_FILE_TOML
#define ET_FILE_TOML "/etc/easytier/config.toml"
#endif
#ifndef ET_FILE_MACHINE_ID
#define ET_FILE_MACHINE_ID "/etc/easytier/et_machine_id"
#endif
#ifndef ET_PATH_DAEMON
#define ET_PATH_DAEMON "/usr/sbin/easytierd"
#endif
#ifndef ET_DIR_ET
#define ET_DIR_ET "/etc/easytier"
#endif

#define ET_CLI_NAME "easytier-cli"
#define ET_WEB_EMBED_NAME "easytier-web-embed"

#define ET_LOG_CORE "/tmp/easytier.log"
#define ET_LOG_WEB "/tmp/easytierweb.log"
#define ET_LOG_CORE_TMP "/tmp/easytier.log.tmp"
#define ET_LOG_WEB_TMP "/tmp/easytierweb.log.tmp"
#define ET_TIME_CORE "/tmp/easytier_time"
#define ET_TIME_WEB "/tmp/easytierweb_time"
#define ET_TAG_CORE "/tmp/easytier.tag"
#define ET_TAG_WEB "/tmp/easytierweb.tag"
#define ET_TAG_LATEST "/tmp/easytiernew.tag"
#define ET_CONNINFO_DIR "/tmp/easytier_conninfo"
#define ET_DL_PROGRESS "/tmp/easytier_download_progress"
#define ET_DL_CANCEL "/tmp/easytier_download_cancel"
#define ET_DL_DIR "/tmp/easytier_download"
#define ET_UP_DIR "/tmp/easytier_extract"
#define ET_LOCK_RELOAD "/var/lock/easytier_reload.lock"
#define ET_PIDFILE "/var/run/easytierd.pid"

/* ---- 与原实现一致的常量 ---- */
#define ET_LOG_MAX_KB 5120
#define ET_LOG_KEEP_LINES 500
#define ET_LOG_ROTATE_SEC 60
#define ET_CONNINFO_TTL 3
#define ET_RESPAWN_THRESHOLD 3600
#define ET_RESPAWN_TIMEOUT 5
#define ET_RESPAWN_RETRY 20
#define ET_DEFAULT_TCP_PORT "11010"
#define ET_DEFAULT_WS_PORT "11011"
#define ET_DEFAULT_WSS_PORT "11012"
#define ET_DEFAULT_WG_PORT "11011"

/* 状态码 */
enum {
	ET_OK = 0,
	ET_ERR = -1,
	ET_ENOENT = -2,
	ET_EINVAL = -3,
	ET_ENOMEM = -4,
	ET_EPERM = -5,
	ET_EBUSY = -6,
};

/* 全局调试开关（--verbose / -v） */
extern int et_debug;

#define et_dbg(fmt, ...) \
	do { if (et_debug) fprintf(stderr, "[dbg] " fmt "\n", ##__VA_ARGS__); } while (0)

#endif /* ETD_H */
