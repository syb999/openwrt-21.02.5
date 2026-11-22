local m, s, o
local sys = require "luci.sys"

m = Map("llama", translate("Llama AI"),
	translate("Local LLM inference served by llama-server. The chat page and " ..
		"the login-free page both talk to this service over HTTP."))

s = m:section(NamedSection, "main", "llama", translate("Service"))
s.anonymous = false
s.addremove = false

o = s:option(Flag, "enabled", translate("Enable"))
o.rmempty = false
o.default = "1"
o.description = translate("Start llama-server on boot and keep it running.")

o = s:option(Value, "binary", translate("Server binary"))
o.datatype = "file"
o.placeholder = "/opt/llama/llama-server"
o.rmempty = false
o.description = translate("Full path of the llama-server executable on this device.")

o = s:option(Value, "model", translate("Model file"))
o.datatype = "file"
o.placeholder = "/opt/llama/qwen2.5-0.5b-q4_k_m.gguf"
o.rmempty = false
o.description = translate("Absolute path to a .gguf file on the device.")

o = s:option(Value, "port", translate("Port"))
o.datatype = "port"
o.default = "8090"
o.rmempty = false

o = s:option(Value, "threads", translate("Threads"))
o.datatype = "range(1,16)"
o.default = "4"
o.description = translate("CPU threads used for inference.")

o = s:option(Value, "ctx", translate("Context size"))
o.datatype = "uinteger"
o.default = "2048"
o.description = translate("Larger values use more RAM for the KV cache.")

o = s:option(Value, "parallel", translate("Parallel slots"))
o.datatype = "range(1,8)"
o.default = "1"
o.description = translate("Each slot gets its own KV cache and compute buffers, " ..
	"so 4 slots on a 4 GB board wastes over 1 GB. Keep 1 unless you really " ..
	"serve several clients at once.")

o = s:option(Value, "api_key", translate("Access key"),
	translate("Optional. When set, clients must send this key. Leave empty for an open service. " ..
		"The key is stored in a 0600 file, never in UCI or in any web page."))
o.password = true
o.rmempty = true

o = s:option(Value, "key_file", translate("Access key file"))
-- deliberately "string", not "file": the file does not exist while no access
-- key is configured, and datatype "file" requires the path to exist, which
-- made every save fail validation.
o.datatype = "string"
o.placeholder = "/etc/llama.key"
o.rmempty = true
o.description = translate("Passed to llama-server as --api-key-file so the key never appears in ps.")

function m.on_after_commit(self)
	local key = (sys.exec("uci -q get llama.main.api_key") or ""):gsub("%s*$", "")
	local kf  = (sys.exec("uci -q get llama.main.key_file") or ""):gsub("%s*$", "")
	if kf == "" then kf = "/etc/llama.key" end

	local f = io.open(kf, "w")
	if f then
		if key ~= "" then f:write(key, "\n") end
		f:close()
		sys.call("chmod 600 '" .. kf:gsub("'", "") .. "' >/dev/null 2>&1")
	end
	if key == "" then
		sys.call("rm -f '" .. kf:gsub("'", "") .. "'")
	end

	-- do not keep the plaintext key in UCI
	sys.call("uci -q delete llama.main.api_key; uci commit llama")
	sys.call("/etc/init.d/llama restart >/dev/null 2>&1")
end

-- ===== 模型：下载 / 状态 =====
-- 为什么不做"上传"：GGUF 动辄 2GB，浏览器上传要过 LuCI 的 multipart + 内存，
-- 板子 4GB 根本吃不下；直接在线拉（ModelScope）比上传快也稳。
local DL = {
    ["qwen2.5-3b-q4_k_m"]   = "/opt/llama/qwen2.5-3b-q4_k_m.gguf",
    ["qwen2.5-1.5b-q4_k_m"] = "/opt/llama/qwen2.5-1.5b-q4_k_m.gguf",
    ["qwen2.5-0.5b-q4_k_m"] = "/opt/llama/qwen2.5-0.5b-q4_k_m.gguf",
}
local dl_sel

local function cgi(q)
    return sys.exec("wget -q -O- 'http://127.0.0.1/cgi-bin/agent?" .. q .. "' 2>/dev/null") or ""
end

s = m:section(NamedSection, "main", "llama", translate("Model download"))
s.anonymous = false
s.addremove = false

o = s:option(DummyValue, "_mst", translate("Current model"))
o.rawhtml = true
function o.cfgvalue()
    local t = cgi("model_status=1&fmt=text")
    local path = t:match("model=(.-)\n") or "?"
    local ex = t:match("exists=(%d)") or "0"
    local sz = tonumber(t:match("size=(%d+)") or "0") or 0
    local h
    if ex == "1" then
        h = string.format('<span style="color:#2a2">已就绪</span> %s（%.2f GB）',
            path, sz / 1073741824)
    else
        h = string.format('<span style="color:#c22">文件不存在</span> %s', path)
    end
    for key, pct, run, done in t:gmatch("dl=(%S+) total=%d+ bytes=%d+ pct=(%d+) running=(%d) done=(%d)") do
        if run == "1" then
            h = h .. string.format('<br><span style="color:#26c">正在下载 %s：%s%%</span> ' ..
                '（刷新页面看最新进度）', key, pct)
        elseif done == "1" then
            h = h .. string.format('<br><span style="color:#2a2">%s 已下载完成</span>', key)
        end
    end
    return h
end

o = s:option(ListValue, "_mdl", translate("Model to download"))
o:value("qwen2.5-3b-q4_k_m",   translate("Qwen2.5-3B Q4_K_M  (2.0 GB, recommended)"))
o:value("qwen2.5-1.5b-q4_k_m", translate("Qwen2.5-1.5B Q4_K_M  (1.0 GB)"))
o:value("qwen2.5-0.5b-q4_k_m", translate("Qwen2.5-0.5B Q4_K_M  (0.4 GB)"))
o.default = "qwen2.5-3b-q4_k_m"
o.cfgvalue = function() return dl_sel or "qwen2.5-3b-q4_k_m" end
o.write = function(self, section, value) dl_sel = value end   -- 只记在内存，不写 UCI

b = s:option(Button, "_mdlgo", translate("Start download"))
b.inputstyle = "apply"
function b.write()
    local key = dl_sel or "qwen2.5-3b-q4_k_m"
    local r = cgi("model_dl=" .. key)
    if r:match('"ok":1') then
        m.message = translate("已开始下载（后台），刷新本页看进度；下完点下面的按钮设为当前模型")
    else
        m.message = translate("启动下载失败") .. "：" .. (r:match('"error":"(.-)"') or r)
    end
end

b = s:option(Button, "_mdluse", translate("Use downloaded model"))
b.inputstyle = "apply"
function b.write()
    local key = dl_sel or "qwen2.5-3b-q4_k_m"
    local p = DL[key]
    local f = io.open(p, "r")
    if not f then
        m.message = translate("这个模型还没下载完") .. "：" .. p
        return
    end
    f:close()
    sys.call("uci set llama.main.model='" .. p .. "'; uci commit llama")
    sys.call("/etc/init.d/llama restart >/dev/null 2>&1")
    m.message = translate("已设为当前模型并重启服务") .. "：" .. p
end

o = s:option(DummyValue, "_mnote", translate("Note"))
o.rawhtml = true
o.cfgvalue = function()
    return translate("下载走 ModelScope（modelscope.cn），约 2 GB，视网速 5~30 分钟。" ..
        "文件先写 .part，下完才改名，中途中断不会污染已有模型。")
end

return m
