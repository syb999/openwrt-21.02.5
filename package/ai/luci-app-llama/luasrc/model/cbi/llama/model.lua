local m, s, o
local sys = require "luci.sys"

m = Map("llama", translate("llama 模型设置"),
    translate("在设备上直接下载 GGUF 模型并切换。下载走 ModelScope，先写 .part，下完才改名。"))

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
