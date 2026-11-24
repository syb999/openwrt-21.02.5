local m, s, o
local sys = require "luci.sys"

m = Map("llama", translate("基本设置"),
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

return m
