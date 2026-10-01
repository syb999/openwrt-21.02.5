-- Copyright (C) 2026 syb999
-- luci-app-diskrestore: controller
--
-- 页面只做展示 + XHR 转发, 所有实际工作由 /usr/bin/diskrestore 完成。
--
-- 🔴 本树 LuCI (ccache 字节码 + dispatcher setfenv) 的两条铁律:
--   1) index() 里绝不抛异常: dispatcher 在 createtree 阶段调用它, 一抛就是
--      整个站点的所有页面 500。全部逻辑包在 pcall 里。
--   2) index() 里不要用模块级 local 上值 (实测为 nil), 路径一律写字面量。

module("luci.controller.diskrestore", package.seeall)

local http = luci.http
local sys  = luci.sys

local CLI = "/usr/bin/diskrestore"

function index()
	pcall(function()
		if not require("nixio.fs").access("/usr/bin/diskrestore") then
			return
		end

		entry({"admin", "services", "diskrestore"},
			alias("admin", "services", "diskrestore", "main"),
			_("磁盘数据恢复"), 86).dependent = false

		entry({"admin", "services", "diskrestore", "main"},
			template("diskrestore/main"), _("文件恢复"), 1).leaf = true

		entry({"admin", "services", "diskrestore", "list"},   call("act_list"),   nil).leaf = true
		entry({"admin", "services", "diskrestore", "dests"},  call("act_dests"),  nil).leaf = true
		entry({"admin", "services", "diskrestore", "destcheck"}, call("act_destcheck"), nil).leaf = true
		entry({"admin", "services", "diskrestore", "images"}, call("act_images"), nil).leaf = true
		entry({"admin", "services", "diskrestore", "devs"},   call("act_devs"),   nil).leaf = true
		entry({"admin", "services", "diskrestore", "status"}, call("act_status"), nil).leaf = true
		entry({"admin", "services", "diskrestore", "start"},  call("act_start"),  nil).leaf = true
		entry({"admin", "services", "diskrestore", "stop"},   call("act_stop"),   nil).leaf = true
		entry({"admin", "services", "diskrestore", "log"},    call("act_log"),    nil).leaf = true
		entry({"admin", "services", "diskrestore", "result"}, call("act_result"), nil).leaf = true
		entry({"admin", "services", "diskrestore", "filelist"}, call("act_filelist"), nil).leaf = true
		entry({"admin", "services", "diskrestore", "organize"}, call("act_organize"), nil).leaf = true
		entry({"admin", "services", "diskrestore", "orgstatus"}, call("act_orgstatus"), nil).leaf = true
	end)
end

-- 调后端并原样转发它的 JSON (后端保证任何情况下都输出合法 JSON)。
-- 参数走严格白名单, 杜绝 shell 注入。
-- 白名单只放行: 字母数字 _ - . : = + /  (没有空格/引号/;|&$`, 构不成 shell 语句)
-- 手输的目标路径用 base64 传, 所以这里的字符够用。
local function relay(...)
	local args = { ... }
	local out = nil
	local ok = pcall(function()
		local t = {}
		for _, a in ipairs(args) do
			a = tostring(a)
			if not a:match("^[%w_%-%.%:%=%+%/]+$") then
				error("bad arg")
			end
			t[#t + 1] = a
		end
		out = sys.exec(CLI .. " " .. table.concat(t, " "))
	end)

	http.prepare_content("application/json")
	if not ok or type(out) ~= "string" or out == "" then
		http.write('{"ok":false,"error":"后端调用失败 (/usr/bin/diskrestore)"}')
		return
	end
	http.write(out)
end

local function getdev()
	local d = http.formvalue("dev")
	if d and d:match("^[%w_%-]+$") then
		return d
	end
	return nil
end

function act_list()   relay("list") end
function act_status() relay("status") end
function act_log()    relay("log", 20) end
function act_stop()   relay("stop") end

function act_dests()
	local d = getdev()
	if d then
		relay("destinations", d)
	else
		relay("destinations")
	end
end

-- 扫描已做好的镜像文件 (供"用已有镜像恢复"选择)
function act_images()
	relay("images")
end

-- 校验/预览手输的目标目录 (base64 传路径, 见 relay 的白名单说明)
function act_destcheck()
	local p = http.formvalue("p")
	if not p or not p:match("^[0-9a-fA-F]+$") then
		http.prepare_content("application/json")
		http.write('{"ok":false,"error":"路径参数非法"}')
		return
	end
	local d = getdev()
	if d then
		relay("destcheck", "hex:" .. p, d)
	else
		relay("destcheck", "hex:" .. p)
	end
end

function act_start()
	-- 恢复源二选一: dev=<设备名>  或  img=<base64url 镜像路径>
	local src = getdev()
	if not src then
		local im = http.formvalue("img")
		if im and im:match("^[0-9a-fA-F]+$") then
			src = "hex:" .. im
		end
	end
	local i = http.formvalue("dest")
	local ok_dest = i and (i:match("^%d+$") or i:match("^hex:[0-9a-fA-F]+$"))
	if not src or not ok_dest then
		http.prepare_content("application/json")
		http.write('{"ok":false,"error":"参数非法: 需要 dev 或 img，加上 dest"}')
		return
	end
	relay("start", src, i)
end

function act_result()
	local n = http.formvalue("n")
	if not n or not n:match("^%d+$") then
		n = "100"
	end
	relay("result", n)
end

-- 整理雕出的文件: 按 文件日期 分到 年/月 目录 (后台任务, 状态用 orgstatus 轮询)
function act_organize()
	relay("organize")
end

function act_devs()
	relay("devs")
end

function act_orgstatus()
	relay("orgstatus")
end

-- 完整文件清单: 纯文本下载 (几万个文件时页面只渲染前 N 个, 想看全的走这里)
function act_filelist()
	local w = http.formvalue("w")
	if type(w) ~= "string" or not w:match("^%a+$") then
		w = "recup"
	end
	local ok, out = pcall(sys.exec, CLI .. " filelist " .. w)
	http.prepare_content("text/plain; charset=utf-8")
	http.write(ok and out or "读取失败")
end
