local sys = require "luci.sys"

local m, s, o

m = Map("excel_match", translate("Excel 表格匹配"),
	translate("免登录页面：") .. [[<a href="/excel.htm" target="_blank">/excel.htm</a>]] ..
	translate("　（不经过 LuCI 登录，直接在手机/电脑浏览器打开）"))

s = m:section(TypedSection, "excel_match", translate("访问与限制"))
s.anonymous = true
s.addremove = false

o = s:option(Value, "token", translate("访问口令"),
	translate("留空 = 不启用（任何人可打开页面使用）。启用后打开页面要先输入口令；" ..
		"可以用空格和符号，区分大小写。"))
o.rmempty = true
o.password = false

o = s:option(Value, "max_file_mb", translate("单个文件上限 (MB)"),
	translate("默认 10。超过直接拒绝，避免把 1GB 内存的盒子打挂。"))
o.datatype = "uinteger"
o.default = "10"
o.rmempty = true

o = s:option(Value, "max_total_mb", translate("两份合计上限 (MB)"), translate("默认 20。"))
o.datatype = "uinteger"
o.default = "20"
o.rmempty = true

o = s:option(Value, "max_unpacked_mb", translate("解压后上限 (MB)"),
	translate("默认 200。xlsx 是压缩包，用来防\"压缩炸弹\"。"))
o.datatype = "uinteger"
o.default = "200"
o.rmempty = true

s = m:section(TypedSection, "excel_match", translate("状态"))
s.anonymous = true
function s.cfgsections() return { "_status" } end
o = s:option(DummyValue, "_v", translate("运行环境"))
o.rawhtml = true
function o.cfgvalue()
	return "python3 + pandas + openpyxl" ..
		"<br>uhttpd script_timeout = " ..
		tostring(sys.exec("uci -q get uhttpd.main.script_timeout"):gsub("%s+$", ""))
end

return m
