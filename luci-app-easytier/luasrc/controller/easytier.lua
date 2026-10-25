-- luci-app-easytier/luasrc/controller/easytier.lua
--
-- 极薄转发壳（前端桥接层）：只做菜单注册 + 请求转发，零业务逻辑。端点一律把请求
-- 交给 /usr/sbin/easytierctl（C 实现）并把 stdout 原样写回；不读 UCI、不读写业务
-- 文件、不拼业务命令（除转发必须的 shell quote）。路由表见 src/api/etd_routes.c。
module("luci.controller.easytier", package.seeall)

local http = require "luci.http"
local util = require "luci.util"
local sys  = require "luci.sys"

local CTL = "/usr/sbin/easytierctl"

-- 24 个原有端点（URL == 方法名，顺序与旧 index() 一致）+ 配置页新增 4 个
local EP = {
	"upload_binary", "get_upload_config", "save_upload_config", "get_disk_space",
	"get_tun_info", "get_log", "get_log_size", "clear_log", "get_wlog",
	"get_wlog_size", "clear_wlog", "clear_version_cache", "get_web_config",
	"save_web_config", "reset_database", "check_web_status", "api_status",
	"api_conninfo", "restart_service", "toggle_core", "toggle_web",
	"download_easytier", "download_progress", "cancel_download",
	"config_get", "config_save", "config_render", "schema",
}

-- 响应 MIME：默认 JSON，个别端点例外；raw 表里的参数不加引号（都是本地旗标）
local MIME = { get_log = "text/plain", get_wlog = "text/plain",
	config_render = "text/html", clear_version_cache = "text/plain" }
local RAW = { ["--body-file"] = true }

-- luci.util 没有 shellquote 时退化为最小实现（兼容很老的 luci）
local quote = util.shellquote or function(s)
	return "'" .. tostring(s or ""):gsub("'", "'\\''") .. "'"
end

-- 执行 easytierctl，参数逐个 quote（不经过 shell 展开）
local function ctl(method, extra)
	local args = { quote(method) }
	for i = 1, #(extra or {}) do
		local a = extra[i]
		args[#args + 1] = RAW[a] and a or quote(a)
	end
	return sys.exec(CTL .. " api " .. table.concat(args, " ") .. " 2>/dev/null") or ""
end

-- 把当前请求翻译成 easytierctl 的参数：query/表单 → key=value；JSON → --body-file
local function forward(method)
	local getenv = http.getenv or os.getenv
	local ct = (getenv("CONTENT_TYPE") or ""):lower()
	local extra, body_file = {}, nil
	if ct:find("json", 1, true) then
		local body = http.content() or ""
		body_file = string.format("/tmp/et_body_%d_%d", os.time(), math.random(1000000))
		local fp = io.open(body_file, "w")
		if fp then
			fp:write(body)
			fp:close()
		end
		extra[1], extra[2] = "--body-file", body_file
	else
		-- formvalue() 无参返回整张 params 表；formvaluetable() 按前缀过滤，无参恒为空表
		for k, v in pairs(http.formvalue() or {}) do
			if type(v) == "table" then v = table.concat(v, "\n") end
			if type(v) == "string" then
				extra[#extra + 1] = tostring(k) .. "=" .. v
			end
		end
	end

	local out = ctl(method, extra)
	if body_file then os.remove(body_file) end
	return out
end

local function reply(method)
	http.prepare_content(MIME[method] or "application/json")
	http.write(forward(method))
end

-- 模板内渲染配置表单（HTML 由 C 生成，这里只取 stdout）
function easytierctl_render()
	return forward("config_render")
end

function index()
	if not require("nixio.fs").access("/etc/config/easytier") then
		return
	end

	entry({"admin", "vpn"}, firstchild(), "VPN", 45).dependent = false
	entry({"admin", "vpn", "easytier"}, firstchild(), _("EasyTier"), 46).dependent = true
	entry({"admin", "vpn", "easytier", "status"}, template("easytier/easytier_status"), _("Status"), 1).leaf = true
	entry({"admin", "vpn", "easytier", "config"}, template("easytier/easytier_config"), _("EasyTier Core"), 2).leaf = true
	entry({"admin", "vpn", "easytier", "webconsole"}, template("easytier/easytier_web"), _("EasyTier Web"), 3).leaf = true
	entry({"admin", "vpn", "easytier", "log"}, template("easytier/easytier_log"), _("Logs"), 4).leaf = true
	entry({"admin", "vpn", "easytier", "upload"}, template("easytier/easytier_upload"), _("Upload Program"), 5).leaf = true
	for _, name in ipairs(EP) do
		entry({"admin", "vpn", "easytier", name}, call(name)).leaf = true
	end
end

-- 所有端点共用同一个转发实现；upload_binary 在下方单独覆盖（需要 setfilehandler）
for _, name in ipairs(EP) do
	_M[name] = function() reply(name) end
end

-- 上传：LuCI 的 filehandler 落盘到 /tmp/upload_<name>，再由 C 负责校验与安装。
-- 未收到文件时同样转发（交给 C 返回与旧版一致的错误文案）。
function upload_binary()
	local fp, path, name = nil, "", ""
	http.setfilehandler(function(meta, chunk, eof)
		if meta and meta.file then
			name = tostring(meta.file):match("[^/\\]+$") or tostring(meta.file)
			path = "/tmp/upload_" .. name
		end
		if not fp and path ~= "" then fp = io.open(path, "wb") end
		if chunk and fp then fp:write(chunk) end
		if eof and fp then
			fp:close()
			fp = nil
		end
	end)

	http.formvalue() -- 触发 multipart 解析，文件由上面的 handler 落盘

	-- 未收到文件时也转发，错误文案由 C 统一给出（与旧版一致）
	local extra = (path ~= "" and name ~= "") and { "file=" .. path, "name=" .. name } or {}
	http.prepare_content("application/json")
	http.write(ctl("upload_binary", extra))
end
