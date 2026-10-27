local sys       = require "luci.sys"
local datatypes = require "luci.cbi.datatypes"
local i18n   = require "luci.i18n"
local translate = i18n.translate

local m, s, o

m = Map("cups_helper", translate("CUPS Helper - optimization"),
	translate("These values are written into a managed block inside /etc/cups/cupsd.conf " ..
		"and applied by restarting cupsd. Queues are published over mDNS so clients " ..
		"find the printer without typing an address."))

s = m:section(TypedSection, "cups_helper", translate("Settings"))
s.anonymous = true
s.addremove = false

o = s:option(Flag, "avahi", translate("Publish queues over mDNS (avahi)"),
	translate("Requires avahi-daemon. Then phones and desktops list the queue automatically."))
o.default = "1"
o.rmempty = false

o = s:option(Flag, "listen_any", translate("Listen on all interfaces"),
	translate("Off = localhost only (printers unreachable from the LAN)."))
o.default = "1"
o.rmempty = false

o = s:option(Value, "access_subnet", translate("Allowed subnets"),
	translate("Comma separated CIDRs, e.g. 192.168.1.0/24, 10.80.1.0/24. Every Allow list " ..
		"in cupsd.conf is rewritten to 127.0.0.1 plus these subnets. " ..
		"Leave empty to keep the CUPS defaults (10/8, 172.16/12, 192.168/16)."))
o.placeholder = "192.168.1.0/24, 10.80.1.0/24"
-- luci's "list(cidr4)" only splits on whitespace, so commas would be rejected;
-- accept both separators here
o.validate = function(self, value)
	if not value or value == "" then
		return value
	end
	for item in value:gmatch("[^,%s]+") do
		if not datatypes.cidr4(item) then
			return nil, translate("Invalid CIDR: ") .. item
		end
	end
	return value
end
o.default = "192.168.1.0/24"
o.rmempty = true

o = s:option(ListValue, "log_level", translate("CUPS log level"),
	translate("error keeps the flash writes down on a small device."))
o:value("error", translate("error"))
o:value("warn", translate("warn"))
o:value("info", translate("info"))
o:value("debug", translate("debug"))
o.default = "warn"

o = s:option(Value, "max_jobs", translate("Max queued jobs"), translate("Default 50."))
o.datatype = "uinteger"
o.default = "50"

o = s:option(Value, "max_jobs_per_printer", translate("Max jobs per printer"), translate("Default 10."))
o.datatype = "uinteger"
o.default = "10"

o = s:option(Value, "keep_jobs_days", translate("Keep job history (days, 0 = never)"),
	translate("On tmpfs every reboot clears it anyway; 1 is enough for troubleshooting."))
o.datatype = "uinteger"
o.default = "1"

m.on_after_commit = function()
	sys.call("/usr/lib/cups-helper/apply.sh >/dev/null 2>&1")
end

return m
