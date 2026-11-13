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
		{ id = "ppt",      name = i18n.translate("PPT specialist") },
		{ id = "shortvideo", name = i18n.translate("Short-video scriptwriter") },
		{ id = "jsrev",    name = i18n.translate("JS reverse-engineering expert") },
		{ id = "selenium", name = i18n.translate("Selenium expert") },
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
	entry({"admin", "services", "dss", "kimicred"}, call("act_kimicred")).leaf = true
	entry({"admin", "services", "dss", "apicred"}, call("act_apicred")).leaf = true
	entry({"admin", "services", "dss", "doubaocred"}, call("act_doubaocred")).leaf = true
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
	c:set("dss", "cli", "pause",   http.formvalue("pause")   or "2")
	c:set("dss", "cli", "backend_a", (http.formvalue("backend_a") or "deepseek"):gsub("%s", ""))
	c:set("dss", "cli", "backend_b", (http.formvalue("backend_b") or "deepseek"):gsub("%s", ""))
	-- 模型：必须属于所选后端，否则清掉（避免 deepseek 侧留着 k28-instant）
	local MODELS = {
		deepseek = { ["deepseek-chat"] = true, ["deepseek-reasoner"] = true },
		api      = { ["glm-4-flash"] = true },
	}
	local function set_model(model_key, backend_key)
		local be = (http.formvalue(backend_key) or "deepseek"):gsub("%s", "")
		local val = (http.formvalue(model_key) or ""):gsub("%s", "")
		local allowed = MODELS[be] or MODELS.deepseek
		local free = (be == "api")
		if val ~= "" and not free and not allowed[val] then val = "" end
		if val == "" then c:delete("dss", "cli", model_key) else c:set("dss", "cli", model_key, val) end
	end
	set_model("model_a", "backend_a")
	set_model("model_b", "backend_b")
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

-- Kimi 凭据：可粘 ChatService/Chat 的 Copy as cURL，也可直接粘 localStorage.refresh_token
function act_kimicred()
	local op = http.formvalue("op") or "save"
	local KC = "/root/.dss/kimi.cred"

	if op == "clear" then
		fs.remove(KC)
		back("kimcleared")
		return
	end

	local txt = http.formvalue("kimicurl") or ""

	-- 读旧值，保留 cookie/device/session/traffic（粘单个 token 时不至于丢）
	local old, order = {}, {}
	local of = io.open(KC, "r")
	if of then
		for l in of:lines() do
			local k, v = l:match("^%s*([%w_]+)%s*:%s*(.+)$")
			if k then
				if not old[k] then table.insert(order, k) end
				old[k] = v
			end
		end
		of:close()
	end

	if not txt:find("curl", 1, true) then
		-- 直接粘 token
		local tok = txt:gsub("%s", ""):gsub('^"', ""):gsub('"$', "")
		if #tok < 40 then
			back("badkimi")
			return
		end
		old.refresh_token = tok
		old.access_token = nil          -- 强制用新 token 换一次
		if not old.model then old.model = "k28-instant" end
		local lines = {}
		for _, k in ipairs({ "refresh_token", "access_token", "cookie", "dev", "sess", "traffic", "model" }) do
			if old[k] and old[k] ~= "" then table.insert(lines, k .. ": " .. old[k]) end
		end
		txt = table.concat(lines, "\n")
	else
		-- 粘 cURL：截掉 --data-raw 的二进制正文
		local keep = {}
		for line in txt:gmatch("[^\r\n]+") do
			if line:find("^%s*%-%-data") or line:find("^%s*%-d%s") then break end
			table.insert(keep, line)
		end
		txt = table.concat(keep, "\n")
		if not txt:find("Bearer", 1, true) then
			back("badkimi")
			return
		end
	end

	fs.mkdirr("/root/.dss", "0700")
	local f = io.open(KC, "w")
	if not f then back("badkimi") return end
	f:write(txt .. "\n")
	f:close()
	fs.chmod(KC, "0600")
	back("kimsaved")
end

-- 豆包凭据：粘 /chat/completion 的 Copy as cURL（含 cookie、x-flow-trace、a_bogus）
function act_doubaocred()
	local op = http.formvalue("op") or "save"
	local DC = "/root/.dss/doubao.cred"

	if op == "clear" then
		fs.remove(DC)
		back("dbsaved")
		return
	end

	local txt = http.formvalue("dbcURL") or ""
	if not txt:find("doubao.com", 1, true) or not txt:find("/chat/completion", 1, true) then
		back("baddb")
		return
	end
	-- 只要第一条 curl 块（后面常跟着图片等无关请求）
	local first = txt:match("(curl[^\n]*chat/completion[\s\S]-%-%-data%-raw%s+'[\s\S]-')")
	if first then txt = first end
	if not txt:find("%-%-data%-raw", 1) then
		back("baddb")
		return
	end

	fs.mkdirr("/root/.dss", "0700")
	local f = io.open(DC, "w")
	if not f then back("baddb") return end
	f:write(txt .. "\n")
	f:close()
	fs.chmod(DC, "0600")
	back("dbsaved")
end

-- API 后端（OpenAI 兼容）：base / key / model 三行写到 /root/.dss/openai.cred
function act_apicred()
	local op = http.formvalue("op") or "save"
	local AC = "/root/.dss/openai.cred"

	if op == "clear" then
		fs.remove(AC)
		back("apicleared")
		return
	end

	local base = (http.formvalue("api_base") or ""):gsub("%s", "")
	local key  = (http.formvalue("api_key") or ""):gsub("^%s+", ""):gsub("%s+$", "")
	local model = (http.formvalue("api_model") or ""):gsub("%s", "")

	if base == "" then base = "https://open.bigmodel.cn/api/paas/v4" end
	if not base:find("^https?://") then back("badapi") return end

	-- key 留空 = 保留已存的（方便只改模型）
	if key == "" then
		local old = io.open(AC, "r")
		if old then
			local body = old:read("*a") or ""
			old:close()
			key = body:match("key%s*:%s*([^\n]+)") or ""
		end
	end
	if #key < 8 then back("badapi") return end

	fs.mkdirr("/root/.dss", "0700")
	local f = io.open(AC, "w")
	if not f then back("badapi") return end
	f:write("base: " .. base .. "\nkey: " .. key .. "\nmodel: " .. (model ~= "" and model or "glm-4-flash") .. "\n")
	f:close()
	fs.chmod(AC, "0600")
	back("apisaved")
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
local CMDD  = "/tmp/dss/console.cmd"

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

	-- 安全策略：只执行 dss 命令；任何其它内容一律当"提问"整句加引号传给 dss，
	-- 绝不直接执行外部命令（cd/ls/sh/&&/| 全都不放行）
	if cmd ~= "dss" and not cmd:match("^dss%s") then
		cmd = "dss " .. shq(cmd)
	end

luci.sys.exec("mkdir -p /tmp/dss")
	-- 先清掉上一次没退出的进程：stale pid 会让旧进程继续往同一个 console.out 里刷
	luci.sys.exec("/usr/lib/dss/console-reset.sh >/dev/null 2>&1")
	local cf = io.open(CMDD, "w")
	if cf then cf:write(cmd .. "\n"); cf:close() end

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
		local g = io.open(PIDF, "w"); if g then g:close() end
	end
	http.redirect(luci.dispatcher.build_url("admin/services/dss/console"))
end

--- 文件删除（两个目录都清）
function act_file()
	local op   = http.formvalue("op") or "del"
	local name = http.formvalue("name") or ""

	-- op=all（全部删除）不看 name，必须先处理，否则会被下面的空名字校验挡掉
	if op ~= "all" then
		if name:match("/") or name:match("%.%.") or name == "" then
			http.redirect(luci.dispatcher.build_url("admin/services/dss/files"))
			return
		end
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
