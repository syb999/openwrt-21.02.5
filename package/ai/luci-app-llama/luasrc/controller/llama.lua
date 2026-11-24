module("luci.controller.llama", package.seeall)

function index()
	if not nixio.fs.access("/opt/llama/llama-server") then
		return
	end

	local e = entry({"admin", "services", "llama"},
		alias("admin", "services", "llama", "chat"), _("Llama AI"), 62)
	e.dependent = false

	entry({"admin", "services", "llama", "chat"},
		template("llama/chat"), _("Chat"), 10).leaf = true

	entry({"admin", "services", "llama", "settings"},
		cbi("llama/settings"), _("基本设置"), 20).leaf = true

	entry({"admin", "services", "llama", "model"},
		cbi("llama/model"), _("llama 模型设置"), 30).leaf = true

	entry({"admin", "services", "llama", "voice"},
		cbi("llama/voice"), _("语音设置"), 40).leaf = true
end
