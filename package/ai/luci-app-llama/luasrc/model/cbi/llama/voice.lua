local m, s, o
local sys = require "luci.sys"

m = Map("llama", translate("语音设置"),
    translate("离线语音合成与语音转文字。模型按需下载，不进固件包。"))

local up_tgt

local function cgi(q)
    return sys.exec("wget -q -O- 'http://127.0.0.1/cgi-bin/agent?" .. q .. "' 2>/dev/null") or ""
end

-- ============ 语音（TTS / ASR） ============
-- 模型不进 ipk：包里只有 matcha（26MB 中文女声），
-- 中英双语和 kokoro 男声按需下载，靠 ?plug=voice 直接调插件（不绕模型）
local TTS = {
    { name = "matcha", desc = translate("中文女声（内置，最快 RTF 0.95）"), builtin = true },
    { name = "zh-en",  desc = translate("中英双语，英文真发音（RTF≈5）") },
    { name = "kokoro", desc = translate("男声 / 上百种音色（RTF≈8 较慢）") },
}

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
            nm, desc, (ins == "1") and string.format('<span style="color:#2a2">已装 %s MB</span>', mb)
                                or '<span style="color:#c22">未安装</span>')
    end
    if #rows == 0 then
        return translate("读取失败（voice 插件没装？）")
    end
    return '<table style="width:100%">' .. table.concat(rows) .. '</table>'
end

for _, it in ipairs(TTS) do
    if not it.builtin then
        local key = (it.name:gsub("%-", "_"))
        b = s:option(Button, "_dl_" .. key, it.desc)
        b.inputstyle = "apply"
        b._mname = it.name
        function b.write(self, section)
            local r = cgi("plug=voice&op=model_install&name=" .. self._mname)
            if r:match('"skipped":1') then
                m.message = translate("已经装过了，跳过下载") .. "：" .. self._mname
            elseif r:match('"ok":1') then
                m.message = translate("已开始后台下载") .. " " .. self._mname
                    .. translate("（约 180~210MB），刷新本页看「已安装」；装完自动清理冗余文件")
            else
                m.message = translate("启动失败") .. "：" .. (r:match('"error":"(.-)"') or r)
            end
        end

        b = s:option(Button, "_rm_" .. key, it.desc)
        b.inputstyle = "reset"
        b._mname = it.name
        function b.write(self, section)
            local r = cgi("plug=voice&op=model_remove&name=" .. self._mname)
            if r:match('"ok":1') then
                m.message = translate("已卸载") .. " " .. self._mname
                    .. translate("，用到它会自动回退到 matcha 女声")
            else
                m.message = translate("卸载失败") .. "：" .. (r:match('"error":"(.-)"') or r)
            end
        end
    end
end

o = s:option(DummyValue, "_vnote", translate("说明"))
o.rawhtml = true
o.cfgvalue = function()
    return translate("包里只带 matcha。中英双语（melo）和 kokoro 男声点上面按钮现下，"
        .. "装完自动删掉 fp32 权重和 espeak 数据；卸载可回收 180~190MB。"
        .. "下载走 GitHub，国内如慢可先手动放到 /opt/tts-rk3566/<名字>/ 再刷新。"
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
  <input type="file" name="file" accept=".bz2,.gz,.tar" style="max-width:320px">
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
        "(bunzip2 -c /tmp/tts_up.tbz | tar xf - -C /tmp/tts_up_x || tar xf /tmp/tts_up.tbz -C /tmp/tts_up_x) && " ..
        "cp -r /tmp/tts_up_x/*/* '" .. dir .. "/' 2>/dev/null || cp -r /tmp/tts_up_x/* '" .. dir .. "/'")
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
