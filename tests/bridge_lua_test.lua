-- tests/bridge_lua_test.lua
--
-- 极薄 controller 的行为测试（纯 Lua 5.1，不需要路由器）：
-- 用假的 luci.http / luci.util / luci.sys / nixio.fs 驱动真实 controller，
-- 检查
--   1) 菜单结构（5 个页面，顺序/标题/模板与原实现一致）
--   2) 28 个 call 端点全部注册，且 24 个旧端点一个不少
--   3) 每个端点转发给 easytierctl 的命令行（key=value 的拼装与 shell quote）
--   4) JSON body → --body-file（内容正确、事后删除）
--   5) 表单 → key=value；upload_binary → file=/tmp/upload_<name> name=<name>
--   6) 每个端点的 Content-Type
--
-- 用法：lua5.1 tests/bridge_lua_test.lua
-- 退出码 0 = 全部通过。

local fails = 0

local function check(cond, msg)
	if not cond then
		fails = fails + 1
		print("FAIL: " .. tostring(msg))
	end
end

local captured = { cmds = {}, mimes = {}, writes = {}, bodyfile = nil }

-- ---------------- 假 luci.http ----------------
local http = {
	_content_type = "",
	_form = {},
	_body = "",
	_upload = nil,
	_filehandler = nil,
}

function http.getenv(name)
	if name == "CONTENT_TYPE" then
		return http._content_type
	end
	return nil
end

function http.content()
	return http._body
end

function http.formvalue(name)
	-- 真实 LuCI 里 formvalue() 会触发 multipart 解析并回调 filehandler，
	-- 这里用 _upload 模拟一次"上传一个文件"
	if name == nil and http._upload and http._filehandler then
		local fh = http._filehandler
		http._filehandler = nil
		fh({ name = "file", file = http._upload }, "bin", false)
		fh({ name = "file", file = http._upload }, nil, true)
	end
	if name then
		return http._form[name]
	end
	return http._form
end

function http.prepare_content(mime)
	captured.mimes[#captured.mimes + 1] = mime
end

function http.write(s)
	captured.writes[#captured.writes + 1] = s
end

function http.setfilehandler(cb)
	http._filehandler = cb
end

package.loaded["luci.http"] = http
package.loaded["luci.util"] = {
	shellquote = function(s)
		return "'" .. tostring(s or ""):gsub("'", "'\\''") .. "'"
	end,
}
package.loaded["luci.sys"] = {
	exec = function(cmd)
		captured.cmds[#captured.cmds + 1] = cmd
		local p = cmd:match("%-%-body%-file '([^']+)'")
		if p then
			local f = io.open(p, "r")
			if f then
				captured.bodyfile = f:read("*all")
				f:close()
			end
		end
		return "STUB-OUT"
	end,
}
package.loaded["nixio.fs"] = { access = function() return true end }

-- ---------------- 假 dispatcher 全局 ----------------
local entries = {}

function _G.entry(path, target, title, order)
	local e = { path = path, target = target, title = title, order = order }
	entries[#entries + 1] = e
	return e
end

_G.firstchild = function() return { type = "firstchild" } end
_G.template = function(t) return { type = "template", target = t } end
_G.call = function(t) return { type = "call", target = t } end
_G._ = function(s) return s end
_G.translate = _G._

-- ---------------- 加载真实 controller ----------------
local CONTROLLER = "luci-app-easytier/luasrc/controller/easytier.lua"
local chunk, err = loadfile(CONTROLLER)
if not chunk then
	print("FAIL: 无法加载 " .. CONTROLLER .. ": " .. tostring(err))
	os.exit(1)
end
chunk()
local C = package.loaded["luci.controller.easytier"]
check(C ~= nil, "controller 未注册模块表")
C.index()

-- ---------------- 1) 菜单结构 ----------------
local PAGES = {
	{ "status", "Status", "easytier/easytier_status", 1 },
	{ "config", "EasyTier Core", "easytier/easytier_config", 2 },
	{ "webconsole", "EasyTier Web", "easytier/easytier_web", 3 },
	{ "log", "Logs", "easytier/easytier_log", 4 },
	{ "upload", "Upload Program", "easytier/easytier_upload", 5 },
}

check(#entries == 2 + 5 + 28, "entry 总数 = " .. #entries .. "（期望 35）")
check(entries[1].path[#entries[1].path] == "vpn" and entries[1].title == "VPN",
      "第一个 entry 应是 admin/vpn 分组")
check(entries[2].path[#entries[2].path] == "easytier" and entries[2].title == "EasyTier",
      "第二个 entry 应是 admin/vpn/easytier")

for i, p in ipairs(PAGES) do
	local e = entries[2 + i]
	check(e ~= nil, "缺页面 " .. p[1])
	if e then
		check(e.path[#e.path] == p[1], "第 " .. i .. " 个页面路径 = " .. tostring(e.path[#e.path]))
		check(e.title == p[2], p[1] .. " 标题 = " .. tostring(e.title))
		check(e.order == p[4], p[1] .. " 排序 = " .. tostring(e.order))
		check(e.target.type == "template", p[1] .. " 应按模板渲染")
		check(e.target.target == p[3], p[1] .. " 模板 = " .. tostring(e.target.target))
	end
end

-- ---------------- 2) 端点清单 ----------------
-- 24 个旧端点（与原 controller 逐字一致）+ 4 个配置页新端点
local OLD_EP = {
	"upload_binary", "get_upload_config", "save_upload_config", "get_disk_space",
	"get_tun_info", "get_log", "get_log_size", "clear_log", "get_wlog",
	"get_wlog_size", "clear_wlog", "clear_version_cache", "get_web_config",
	"save_web_config", "reset_database", "check_web_status", "api_status",
	"api_conninfo", "restart_service", "toggle_core", "toggle_web",
	"download_easytier", "download_progress", "cancel_download",
}
local NEW_EP = { "config_get", "config_save", "config_render", "schema" }

local registered = {}
for _, e in ipairs(entries) do
	if e.target.type == "call" then
		registered[e.target.target] = e
	end
end

for _, name in ipairs(OLD_EP) do
	check(registered[name] ~= nil, "旧端点缺失: " .. name)
	check(C[name] ~= nil, "端点函数缺失: " .. name)
end
for _, name in ipairs(NEW_EP) do
	check(registered[name] ~= nil, "新端点缺失: " .. name)
end
local n_call = 0
for _ in pairs(registered) do n_call = n_call + 1 end
check(n_call == 28, "call 端点数量 = " .. n_call .. "（期望 28）")

-- ---------------- 3~6) 转发行为 ----------------
local function invoke(name, ct, form, body)
	http._content_type = ct or ""
	http._form = form or {}
	http._body = body or ""
	captured = { cmds = {}, mimes = {}, writes = {}, bodyfile = nil }
	C[name]()
	return captured.cmds[1], captured.mimes[1], captured.writes[1]
end

local function has(s, sub)
	return s ~= nil and s:find(sub, 1, true) ~= nil
end

-- 纯 GET：无参数，JSON 类型
local cmd, mime, out = invoke("api_status")
check(cmd == "/usr/sbin/easytierctl api 'api_status' 2>/dev/null", "api_status 命令 = " .. tostring(cmd))
check(mime == "application/json", "api_status Content-Type = " .. tostring(mime))
check(out == "STUB-OUT", "响应未原样输出")

-- GET + query 参数
cmd, mime = invoke("api_conninfo", "", { parts = "node,peer" })
check(has(cmd, "'api_conninfo'"), "api_conninfo 方法名缺失")
check(has(cmd, "'parts=node,peer'"), "api_conninfo 未带 parts: " .. tostring(cmd))
check(mime == "application/json", "api_conninfo 应为 JSON")

cmd = invoke("get_tun_info", "", { ifname = "tun0" })
check(has(cmd, "'ifname=tun0'"), "get_tun_info 未带 ifname: " .. tostring(cmd))

cmd = invoke("toggle_core", "", { enabled = "1" })
check(has(cmd, "'enabled=1'"), "toggle_core 未带 enabled: " .. tostring(cmd))

-- text/plain 与 text/html
cmd, mime = invoke("get_log")
check(mime == "text/plain", "get_log 应为 text/plain（实际 " .. tostring(mime) .. "）")
cmd, mime = invoke("get_wlog")
check(mime == "text/plain", "get_wlog 应为 text/plain")
cmd, mime = invoke("config_render")
check(mime == "text/html", "config_render 应为 text/html")
cmd = invoke("clear_version_cache", "", { type = "core" })
check(has(cmd, "'type=core'"), "clear_version_cache 未带 type")

-- 表单 POST：多个字段
cmd, mime = invoke("save_web_config", "", {
	web_enabled = "1",
	web_port = "22020",
	web_oidc_scopes = "openid profile",
})
check(mime == "application/json", "save_web_config 应为 JSON")
check(has(cmd, "'web_enabled=1'"), "save_web_config 缺 web_enabled: " .. tostring(cmd))
check(has(cmd, "'web_port=22020'"), "save_web_config 缺 web_port")
check(has(cmd, "'web_oidc_scopes=openid profile'"), "带空格的值未正确传递/引用")

-- 特殊字符与换行（list 用 \n 分隔）
cmd = invoke("config_save", "", {
	network_secret = "a'b c",
	peeradd = "tcp://a:1\ntcp://b:2",
})
check(has(cmd, "'network_secret=a'\\''b c'"), "单引号未正确转义: " .. tostring(cmd))
check(has(cmd, "'peeradd=tcp://a:1\ntcp://b:2'"), "换行值未正确传递")

-- JSON POST → --body-file
cmd, mime = invoke("get_disk_space", "application/json", {}, '{"path":"/tmp","type":"easytierbin"}')
check(mime == "application/json", "get_disk_space 应为 JSON")
check(has(cmd, "--body-file '"), "JSON 请求未走 --body-file: " .. tostring(cmd))
check(captured.bodyfile == '{"path":"/tmp","type":"easytierbin"}', "body 文件内容不对")
local body_path = cmd:match("%-%-body%-file '([^']+)'")
local f = body_path and io.open(body_path, "r")
check(f == nil, "body 临时文件未清理: " .. tostring(body_path))
if f then f:close() end

-- upload_binary：文件落盘 → file=/tmp/upload_<name> name=<name>
captured = { cmds = {}, mimes = {}, writes = {}, bodyfile = nil }
http._content_type = "multipart/form-data; boundary=x"
http._form = {}
http._upload = "easytier-core"
C.upload_binary()
cmd = captured.cmds[1]
check(has(cmd, "'file=/tmp/upload_easytier-core'"), "upload_binary file 参数缺失: " .. tostring(cmd))
check(has(cmd, "'name=easytier-core'"), "upload_binary name 参数缺失")
check(captured.mimes[1] == "application/json", "upload_binary 应为 JSON")
local up = io.open("/tmp/upload_easytier-core", "r")
check(up ~= nil, "上传文件未落盘")
if up then
	check(up:read("*all") == "bin", "上传文件内容不完整")
	up:close()
	os.remove("/tmp/upload_easytier-core")
end

-- 未收到文件时也要转发（交给 C 返回错误文案）
captured = { cmds = {}, mimes = {}, writes = {}, bodyfile = nil }
http._content_type = "multipart/form-data; boundary=x"
http._form = {}
http._upload = nil
C.upload_binary()
check(has(captured.cmds[1], "'upload_binary'"), "无文件时仍应转发给 C")
check(not has(captured.cmds[1], "file="), "无文件时不应带 file 参数")

-- 渲染辅助：模板里调用的 easytierctl_render()
captured = { cmds = {}, mimes = {}, writes = {}, bodyfile = nil }
http._content_type = ""
http._form = {}
local html = C.easytierctl_render()
check(html == "STUB-OUT", "easytierctl_render 未返回 C 的 stdout")
check(has(captured.cmds[1], "'config_render'"), "easytierctl_render 未调用 config_render")

if fails > 0 then
	print("失败 " .. fails .. " 项")
	os.exit(1)
end
print("全部通过")
