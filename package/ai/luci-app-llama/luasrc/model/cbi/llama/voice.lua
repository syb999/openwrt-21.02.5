local m, s, o, b
local sys = require "luci.sys"

m = Map("llama", translate("语音设置"),
    translate("离线语音合成与语音转文字。模型按需下载，不进固件包。"))

local up_tgt

local function cgi(q)
    return sys.exec("wget -q -O- 'http://127.0.0.1/cgi-bin/agent?" .. q .. "' 2>/dev/null") or ""
end

-- 查询串要自己编码：自定义下载地址里常有 & 和 %，原样拼会把参数截断
local function enc(s)
    return (tostring(s):gsub("[^%w%-%._~]", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

local function hesc(s)
    return (tostring(s):gsub("[&<>]", { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;" }))
end

-- ============ 语音（TTS / ASR） ============
-- 模型不进 ipk：包里只有 matcha（26MB 中文女声），
-- 中英双语和 kokoro 男声按需下载，靠 ?plug=voice 直接调插件（不绕模型）。
-- 下载/解包/收尾全在 C 插件里（voice.c op=model_install），页面只发一次请求。
local PKGS = {
    { name = "zh-en",  label = translate("中英双语 melo（英文真发音）  下载约 160MB") },
    { name = "kokoro", label = translate("kokoro 男声 / 上百音色（RTF≈8，较慢）  下载约 140MB") },
    { name = "custom", label = translate("自定义：从 URL 下载任意压缩包 → /opt/tts-rk3566/custom") },
}
local sel, cust_url

s = m:section(NamedSection, "main", "llama", translate("语音模型"))
s.tab = translate("语音")
s.anonymous = false
s.addremove = false

o = s:option(DummyValue, "_vst", translate("已安装"))
o.rawhtml = true
function o.cfgvalue()
    local t = cgi("plug=voice&op=models")
    local rows = {}
    for nm, desc, ins, mb in t:gmatch('"name":"([^"]+)","desc":"([^"]*)","installed":(%d),"mb":(%d+)') do
        rows[#rows + 1] = string.format(
            '<tr><td style="padding:3px 8px"><code>%s</code></td>'
            .. '<td style="padding:3px 8px">%s</td><td style="padding:3px 8px;white-space:nowrap">%s</td></tr>',
            hesc(nm), hesc(desc), (ins == "1") and (mb ~= "0"
                and string.format('<span style="color:#2a2">已装 %s MB</span>', mb)
                or '<span style="color:#2a2">已装</span>')
                                or '<span style="color:#c22">未安装</span>')
    end
    if #rows == 0 then
        return translate("读取失败（voice 插件没装？）")
    end
    return '<table style="width:100%">' .. table.concat(rows) .. '</table>'
end

o = s:option(ListValue, "_pkg", translate("要下载的模型"))
for _, it in ipairs(PKGS) do o:value(it.name, it.label) end
o.default = "zh-en"
o.cfgvalue = function() return sel or "zh-en" end
o.write = function(self, section, v) sel = v end       -- 只记在内存，不写 UCI

o = s:option(Value, "_url", translate("自定义压缩包地址"),
    translate("只在上面选「自定义」时用。直链即可，例如 GitHub release 里的 "
        .. "model.tar.bz2 / .tar.gz / .tar.xz（tar 包，里面一个目录或直接是文件都行）。"))
o.placeholder = "https://github.com/<owner>/<repo>/releases/download/<tag>/<name>.tar.bz2"
o.datatype = "string"
o.rmempty = true
o.cfgvalue = function() return cust_url end
o.write = function(self, section, v) cust_url = v end

b = s:option(Button, "_go", translate("开始下载（后台自动解包安装）"))
b.inputstyle = "apply"
function b.write()
    local name = sel or "zh-en"
    local q = "plug=voice&op=model_install&name=" .. enc(name)
    if name == "custom" then
        local u = (cust_url or ""):gsub("^%s+", ""):gsub("%s+$", "")
        if u == "" then
            m.message = translate("选了「自定义」就要填压缩包地址")
            return
        end
        if not u:match("^https?://") then
            m.message = translate("地址要以 http:// 或 https:// 开头")
            return
        end
        q = q .. "&url=" .. enc(u)
    end
    local r = cgi(q)
    if r:match('"skipped":1') then
        m.message = translate("已经装过了，跳过下载") .. "：" .. name
            .. translate("（要重装先点下面的「卸载」）")
    elseif r:match('"ok":1') then
        m.message = translate("已开始后台下载") .. " " .. name
            .. translate("，看下面「下载进度」那一行；下载完还要解包 1~2 分钟，装完即生效")
    else
        m.message = translate("启动失败") .. "：" .. (r:match('"error":"(.-)"') or r)
    end
end

-- 进度：三个包全列出来，不看下拉框选了谁 —— 否则下 kokoro 时页面显示的是 zh-en 的状态，
-- 看起来就像"卡着不动"
o = s:option(DummyValue, "_prog", translate("下载进度"))
o.rawhtml = true
function o.cfgvalue()
    local rows = {}
    for _, nm in ipairs({ "zh-en", "kokoro", "custom" }) do
        local t = cgi("plug=voice&op=model_status&name=" .. enc(nm))
        local state
        if not t:match('"ok":1') then
            state = '<span style="color:#c22">读不到状态</span>'
        else
            local ins = t:match('"installed":(%d)') or "0"
            local mb  = t:match('"mb":(%d+)') or "0"
            local run = t:match('"running":(%d)') or "0"
            local dl  = t:match('"dl_mb":(%d+)') or "0"
            local exp = t:match('"exp_mb":(%d+)') or "0"
            local log = t:match('"log":"(.-)"}') or ""
            local stage = "—"
            for n, txt in log:gmatch("%[([1-4])/4%] ([^\\]+)") do stage = n .. "/4 " .. txt end
            if ins == "1" then
                state = (mb ~= "0") and string.format('<span style="color:#2a2">已装 %s MB</span>', mb)
                                     or '<span style="color:#2a2">已装</span>'
            elseif run == "1" then
                if stage:match("^1/4") then
                    state = string.format('<span style="color:#26c">下载中 %s / %s MB</span>',
                        dl, (exp ~= "0") and exp or "?")
                else
                    state = string.format(
                        '<span style="color:#26c">%s —— 解包阶段，弱 CPU 上要 1~2 分钟，没有进度条</span>',
                        hesc(stage))
                end
            elseif log ~= "" then
                state = string.format('<span style="color:#c22">上次没跑完（停在 %s），日志 %s</span>',
                    hesc(stage), hesc("/tmp/llama_agent/gen/model_" .. nm .. ".log"))
            else
                state = '<span style="color:#888">未开始</span>'
            end
        end
        rows[#rows + 1] = string.format(
            '<tr><td style="padding:3px 8px;white-space:nowrap"><code>%s</code></td>'
            .. '<td style="padding:3px 8px">%s</td></tr>', nm, state)
    end
    return '<table style="width:100%">' .. table.concat(rows) .. '</table>'
end

-- 卸载按模型分开，不受下拉框影响（下拉框每次提交会回到默认值）
for _, it in ipairs(PKGS) do
    local key = (it.name:gsub("%-", "_"))
    local rb = s:option(Button, "_rm_" .. key, translate("卸载") .. " " .. it.name)
    rb.inputstyle = "reset"
    rb._mname = it.name
    function rb.write(self, section)
        local r = cgi("plug=voice&op=model_remove&name=" .. enc(self._mname))
        if r:match('"ok":1') then
            local freed = r:match('"freed_mb":(%d+)') or "0"
            m.message = translate("已卸载") .. " " .. self._mname .. "，" .. freed
                .. translate(" MB 已回收；用到它会自动回退到 matcha 女声")
        else
            m.message = translate("卸载失败") .. "：" .. (r:match('"error":"(.-)"') or r)
        end
    end
end

o = s:option(DummyValue, "_vnote", translate("说明"))
o.rawhtml = true
o.cfgvalue = function()
    return translate("包里只带 matcha。中英双语（melo）和 kokoro 男声从下拉选好点一次「开始下载」"
        .. "就会后台下载 + 解包 + 装好，装完自动删掉 fp32 权重和 espeak 数据；卸载可回收 180MB 左右。")
        .. translate("选「自定义」再填地址，就能装 GitHub 上其它 tar 包（例如别的 sherpa-onnx "
        .. "tts-models），装到 /opt/tts-rk3566/custom。")
        .. translate("整个过程：下载约 30 秒（视网速），解包（bunzip2）在弱 CPU 上要 1~2 分钟，"
        .. "这一段没有百分比，只显示阶段；装完「下载进度」会变成「已装 xxx MB」。"
        .. "下载走 GitHub；国内慢的话可以先手动放到 /opt/tts-rk3566/<名字>/ 再刷新本页。"
        .. "语音转文字（sense-voice，229MB）不在这里管理，位于 /opt/asr。")
end


o = s:option(DummyValue, "_asrnote", translate("语音转文字"))
o.rawhtml = true
o.cfgvalue = function()
    local f = io.open("/opt/asr/model/model.int8.onnx", "r")
    local ok = f ~= nil
    if f then f:close() end
    return ok and '<span style="color:#2a2">已安装 sense-voice（中英日韩粤，229MB）</span>'
              or '<span style="color:#c22">未安装：把 /opt/asr 部署好即可用</span>'
end

-- ===== 本地上传模型包（离线/网络差时用） =====
-- 用 luci 自带的 cgi-io（/cgi-bin/cgi-upload）收文件，避免在 C 里写 multipart 解析。
local token = (luci.dispatcher and luci.dispatcher.context and luci.dispatcher.context.authsession) or ""
if type(token) ~= "string" then token = "" end

s = m:section(NamedSection, "main", "llama", translate("本地上传模型包"))
s.anonymous = false
s.addremove = false

o = s:option(DummyValue, "_upform", translate("上传"))
o.rawhtml = true
o.cfgvalue = function()
    return string.format([[
<form method="post" enctype="multipart/form-data" style="margin:0"
      action="/cgi-bin/cgi-upload?sessionid=%s&filename=/tmp/tts_up.tbz">
  <input type="file" name="file" accept=".bz2,.gz,.tar,.xz" style="max-width:320px">
  <button type="submit" class="cbi-button cbi-button-apply">上传到设备</button>
</form>
<div style="color:#888;font-size:12px;margin-top:6px">
上传后文件在 <code>/tmp/tts_up.tbz</code>，再点下面的「解包安装」。
</div>]], token)
end

o = s:option(ListValue, "_uptgt", translate("解包到哪个模型"))
o:value("zh-en",  "zh-en（中英双语）  → /opt/tts-rk3566/zh-en-model")
o:value("kokoro", "kokoro（男声/多音色）→ /opt/tts-rk3566/kokoro")
o:value("custom", "custom（其它）      → /opt/tts-rk3566/custom")
o.default = "zh-en"
o.cfgvalue = function() return up_tgt or "zh-en" end
o.write = function(self, section, v) up_tgt = v end

b = s:option(Button, "_upgo", translate("解包安装"))
b.inputstyle = "apply"
function b.write()
    local tgt = up_tgt or "zh-en"
    local dir = "/opt/tts-rk3566/" .. tgt
    local f = io.open("/tmp/tts_up.tbz", "r")
    if not f then
        m.message = translate("没有找到上传的包：/tmp/tts_up.tbz")
        return
    end
    f:close()
    os.execute("mkdir -p '" .. dir .. "' && rm -rf /tmp/tts_up_x && mkdir -p /tmp/tts_up_x && " ..
        "(bunzip2 -c /tmp/tts_up.tbz | tar xf - -C /tmp/tts_up_x " ..
        "|| gunzip -c /tmp/tts_up.tbz | tar xf - -C /tmp/tts_up_x " ..
        "|| xz -dc /tmp/tts_up.tbz | tar xf - -C /tmp/tts_up_x " ..
        "|| tar xf /tmp/tts_up.tbz -C /tmp/tts_up_x) && " ..
        "(cp -r /tmp/tts_up_x/*/* '" .. dir .. "/' 2>/dev/null || cp -r /tmp/tts_up_x/* '" .. dir .. "/')")
    m.message = translate("已解包到") .. " " .. dir .. translate("。刷新本页看「已安装」")
end

o = s:option(DummyValue, "_fnames", translate("支持的模型文件名"))
o.rawhtml = true
o.cfgvalue = function()
    return [[
<div style="font-size:12px;line-height:1.7">
当前代码按<b>固定文件名</b>找权重，包里的文件必须包含：<br>
<b>zh-en（melo vits）</b>：<code>model.onnx</code>、<code>tokens.txt</code>、<code>lexicon.txt</code>、
<code>date.fst</code>、<code>number.fst</code>（缺 fst 会报 config 错，可从 matcha 目录拷）<br>
<b>kokoro</b>：<code>model.int8.onnx</code>、<code>voices.bin</code>、<code>tokens.txt</code>、
<code>lexicon-us-en.txt</code>、<code>lexicon-zh.txt</code>、
<code>date-zh.fst</code>、<code>number-zh.fst</code>
（程序还会去找 <code>model_int8.onnx</code>／<code>date.fst</code>／<code>number.fst</code>，安装脚本已自动建软链）<br>
<b>matcha（内置）</b>：<code>model_qdq.onnx</code>、<code>hifigan_v2.onnx</code>、<code>tokens.txt</code>、<code>lexicon.txt</code>、<code>dict/</code><br>
<b>ASR（sense-voice）</b>：<code>/opt/asr/model/model.int8.onnx</code> + <code>tokens.txt</code>
</div>
]]
end


return m
