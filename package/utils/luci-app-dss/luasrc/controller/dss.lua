-- LuCI controller for dss (DeepSeek in your shell)

module("luci.controller.dss", package.seeall)

local http = require "luci.http"
local uci  = require "luci.model.uci"
local fs   = require "nixio.fs"

local CRED = "/root/.dss/cred"

-- 角色目录（id 必须与 dss.js 的 ROLE_CATALOG 一致）
local i18n = require "luci.i18n"

function role_list()
	return {
		{ id = "embedded", name = i18n.translate("Embedded hardware engineer") },
		{ id = "android",  name = i18n.translate("Android developer") },
		{ id = "cdev",     name = i18n.translate("C developer") },
		{ id = "novelist", name = i18n.translate("Novelist") },
		{ id = "arch",     name = i18n.translate("System architect") },
		{ id = "reviewer", name = i18n.translate("Strict code reviewer") },
		{ id = "tester",   name = i18n.translate("Test engineer") },
		{ id = "doc",      name = i18n.translate("Technical writer") },
		{ id = "voip",     name = i18n.translate("VoIP / voice expert") },
		{ id = "net",      name = i18n.translate("Network protocol expert") },
		{ id = "security", name = i18n.translate("Security researcher") },
		{ id = "teacher",  name = i18n.translate("Subject teacher") },
		{ id = "gamedev",  name = i18n.translate("Game developer") },
		{ id = "webdev",   name = i18n.translate("Web developer") },
		{ id = "routerdev",name = i18n.translate("Router / OpenWrt developer") },
		{ id = "devops",   name = i18n.translate("DevOps engineer") },
		{ id = "dataeng",  name = i18n.translate("Data engineer") },
		{ id = "ai",       name = i18n.translate("AI / algorithm engineer") },
		{ id = "translator",name = i18n.translate("Technical translator") },
		{ id = "pm",       name = i18n.translate("Product manager") },
	}
end

function index()
	entry({"admin", "services", "dss"}, firstchild(), _("DeepSeek"), 60).dependent = false

	entry({"admin", "services", "dss", "settings"}, template("dss/settings"), _("Settings"), 10).leaf = true
	entry({"admin", "services", "dss", "console"},  template("dss/console"),  _("Console"), 15).leaf = true
	entry({"admin", "services", "dss", "commands"}, template("dss/commands"), _("Command examples"), 20).leaf = true
	entry({"admin", "services", "dss", "files"},    template("dss/files"),    _("Code files"), 40).leaf = true

	entry({"admin", "services", "dss", "save"},  call("act_save")).leaf  = true
	entry({"admin", "services", "dss", "token"}, call("act_token")).leaf = true
	entry({"admin", "services", "dss", "svc"},   call("act_svc")).leaf   = true
	entry({"admin", "services", "dss", "run"},   call("act_run")).leaf   = true
	entry({"admin", "services", "dss", "file"},  call("act_file")).leaf  = true
	entry({"admin", "services", "dss", "stop"},  call("act_stop")).leaf  = true
end

local function back(msg)
	local u = luci.dispatcher.build_url("admin/services/dss/settings")
	if msg then u = u .. "?m=" .. msg end
	http.redirect(u)
end

function act_save()
	local c = uci.cursor()

	c:set("dss", "main", "port",  http.formvalue("port")  or "88")
	c:set("dss", "main", "bind",  http.formvalue("bind")  or "0.0.0.0")
	c:set("dss", "main", "model", http.formvalue("model") or "deepseek-chat")

	c:set("dss", "cli", "pace",    http.formvalue("pace")    or "5")
	c:set("dss", "cli", "rounds",  http.formvalue("rounds")  or "0")
	c:set("dss", "cli", "minutes", http.formvalue("minutes") or "0")
	c:set("dss", "cli", "search",  http.formvalue("search")  == "1" and "1" or "0")
	c:set("dss", "cli", "code",    http.formvalue("code")    == "1" and "1" or "0")

	-- 角色：勾选的 id（每个 checkbox 独立字段名 role_a_<id>）+ 自定义文本，逗号拼接
	local function join_roles(prefix, custom_key)
		local out = {}
		for _, r in ipairs(role_list()) do
			if http.formvalue(prefix .. "_" .. r.id) == "1" then
				table.insert(out, r.id)
			end
		end
		local custom = (http.formvalue(custom_key) or ""):gsub("^%s+", ""):gsub("%s+$", "")
		if custom ~= "" then
			for piece in custom:gmatch("[^,，;；]+") do
				local t = piece:gsub("^%s+", ""):gsub("%s+$", "")
				if t ~= "" then table.insert(out, t) end
			end
		end
		return table.concat(out, ",")
	end
	c:set("dss", "cli", "role_a", join_roles("role_a", "role_a_custom"))
	c:set("dss", "cli", "role_b", join_roles("role_b", "role_b_custom"))

	c:commit("dss")

	luci.sys.exec("/etc/init.d/dss restart >/dev/null 2>&1")
	back("saved")
end

function act_token()
	local op = http.formvalue("op") or "save"

	if op == "clear" then
		fs.remove(CRED)
		luci.sys.exec("/etc/init.d/dss restart >/dev/null 2>&1")
		back("cleared")
		return
	end

	local tok = http.formvalue("token") or ""
	tok = tok:gsub("^%s+", ""):gsub("%s+$", "")

	local inner = tok:match('"value"%s*:%s*"([^"]+)"')
	if inner then tok = inner end
	tok = tok:gsub('^"(.*)"$', "%1")

	if #tok < 20 then
		back("badtoken")
		return
	end

	fs.mkdirr("/root/.dss", "0700")
	local f = io.open(CRED, "w")
	if f then
		f:write(tok .. "\n")
		f:close()
		fs.chmod(CRED, "0600")
	end
	luci.sys.exec("/etc/init.d/dss restart >/dev/null 2>&1")
	back("tokensaved")
end

function act_svc()
	local op = http.formvalue("op") or "restart"
	if op ~= "start" and op ~= "stop" and op ~= "restart" then
		op = "restart"
	end
	luci.sys.exec("/etc/init.d/dss " .. op .. " >/dev/null 2>&1")
	back("svc" .. op)
end

local OUT   = "/tmp/dss/console.out"
local PIDF  = "/tmp/dss/console.pid"
local RUND  = "/tmp/dss/console.run"

local function shq(s)
	return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

function act_run()
	local cmd = http.formvalue("cmd") or ""
	cmd = cmd:gsub("[\r\n]+", " "):gsub("^%s+", ""):gsub("%s+$", "")

	if cmd == "" then
		http.redirect(luci.dispatcher.build_url("admin/services/dss/console") .. "?m=empty")
		return
	end

	-- 没写 dss 就自动补上，这样直接敲问题也行
	if cmd ~= "dss" and not cmd:match("^dss%s") then
		cmd = "dss " .. cmd
	end

	luci.sys.exec("mkdir -p /tmp/dss")
	luci.sys.exec("sh -c " .. shq("cd /root && date +%s > " .. RUND ..
		" && nohup " .. cmd .. " > " .. OUT .. " 2>&1 & echo $! > " .. PIDF))

	http.redirect(luci.dispatcher.build_url("admin/services/dss/console"))
end

function act_stop()
	local f = io.open(PIDF, "r")
	if f then
		local pid = f:read("*l")
		f:close()
		if pid and tonumber(pid) then
			luci.sys.exec("kill " .. tonumber(pid) .. " >/dev/null 2>&1")
		end
	end
	http.redirect(luci.dispatcher.build_url("admin/services/dss/console"))
end

--- 文件删除（两个目录都清）
function act_file()
	local op   = http.formvalue("op") or "del"
	local name = http.formvalue("name") or ""

	if name:match("/") or name:match("%.%.") or name == "" then
		http.redirect(luci.dispatcher.build_url("admin/services/dss/files"))
		return
	end

	if op == "del" then
		fs.remove("/root/.dss/out/" .. name)
		fs.remove("/tmp/dss_output/" .. name)
	elseif op == "all" then
		for _, d in ipairs({"/root/.dss/out", "/tmp/dss_output"}) do
			local p = io.popen("ls -1p " .. d .. " 2>/dev/null | grep -v /")
			if p then
				for f in p:lines() do
					if f:match("^[A-Za-z0-9._-]+$") then
						fs.remove(d .. "/" .. f)
					end
				end
				p:close()
			end
		end
	end

	http.redirect(luci.dispatcher.build_url("admin/services/dss/files"))
end
