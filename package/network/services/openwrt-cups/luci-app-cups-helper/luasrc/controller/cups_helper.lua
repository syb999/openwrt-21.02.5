module("luci.controller.cups_helper", package.seeall)
local http  = require "luci.http"
local sys   = require "luci.sys"
local disp  = require "luci.dispatcher"
local i18n  = require "luci.i18n"

-- the dispatcher only exports "translate" as a global after it finishes loading,
-- which is too late for a call() target: bind it explicitly here
local translate = i18n.translate

function index()
	entry({"admin", "services", "cups-helper"},
	      template("cups_helper/status"), _("CUPS Helper"), 63)
	entry({"admin", "services", "cups-helper", "add"},
	      template("cups_helper/add"), nil).leaf = true
	entry({"admin", "services", "cups-helper", "settings"},
	      cbi("cups_helper/settings"), nil).leaf = true
	entry({"admin", "services", "cups-helper", "op"},
	      call("op"), nil).leaf = true
end

-- run a shell command and capture its combined output.
-- The whole thing is wrapped in { ... ; } so that
--   * inner redirections (a || echo x >> /etc/group) keep working, and
--   * nothing leaks to the CGI stdout before http.redirect() (which would
--     break the redirect because headers are already sent).
local function sh(cmd)
	local ok = sys.call("{ " .. cmd .. " ; } >/tmp/.cupsh 2>&1")
	local out = sys.exec("cat /tmp/.cupsh 2>/dev/null") or ""
	sys.call("rm -f /tmp/.cupsh")
	return ok, out:gsub("%s+$", "")
end

-- everything below ends up in a shell command line, so only allow safe chars
local function safe_name(v)
	return (v and v:match("^[%w_%.%-]+$")) or nil
end

local function safe_uri(v)
	if not v or #v > 200 then return nil end
	if v:match("^[%w_%.%-:/%?#%[%]@=+,]+$") then return v end
	return nil
end

local function safe_path(v)
	if v and #v < 200 and v:match("^/[%w_%.%-/]+$") then return v end
	return nil
end

function op()
	local a     = http.formvalue("action") or ""
	local name  = safe_name(http.formvalue("name"))
	local uri   = safe_uri(http.formvalue("uri_custom"))
	local model = http.formvalue("model") or ""
	local ppd   = safe_path(http.formvalue("ppd"))
	local make_default = http.formvalue("default") == "1"
	local msg

	if not uri then uri = safe_uri(http.formvalue("uri")) end

	if a == "start" or a == "stop" or a == "restart" or a == "enable" then
		local ok, out = sh("/etc/init.d/cupsd " .. a)
		msg = ok == 0 and translate("Done: ") .. a
			or translate("Failed: ") .. out

	elseif a == "apply" then
		local ok, out = sh("/usr/lib/cups-helper/apply.sh")
		msg = ok == 0 and translate("Settings applied and queues republished")
			or (translate("Apply failed: ") .. out)

	elseif a == "fixuser" then
		-- classic CUPS 2.x startup failure: "User root" in cups-files.conf
		-- and no "lp" account in the system
		sh("grep -q '^lp:' /etc/group  || echo 'lp:x:7:' >> /etc/group")
		sh("grep -q '^lp:' /etc/passwd || echo 'lp:x:7:7:lp:/var/spool/lpd:/bin/false' >> /etc/passwd")
		sh("sed -i 's/^User root$/User lp/; s/^Group root$/Group lp/' /etc/cups/cups-files.conf")
		sh("grep -q '^User lp'  /etc/cups/cups-files.conf || sed -i '1i User lp'  /etc/cups/cups-files.conf")
		sh("grep -q '^Group lp' /etc/cups/cups-files.conf || sed -i '1i Group lp' /etc/cups/cups-files.conf")
		sh("mkdir -p /var/spool/cups/tmp /var/cache/cups /var/run/cups; " ..
		   "chown -R lp:lp /var/spool/cups /var/cache/cups /var/run/cups 2>/dev/null")
		sh("/etc/init.d/cupsd enable")
		local _, out = sh("/etc/init.d/cupsd restart; sleep 1; pidof cupsd")
		msg = out ~= "" and (translate("Fixed and started, cupsd pid ") .. out)
			or translate("Still not running after the fix")

	elseif a == "add" then
		if not name then
			msg = translate("Queue name may only contain letters, digits, _ . -")
		elseif not uri then
			msg = translate("Device URI is empty or invalid")
		else
			local args = "lpadmin -p " .. name .. " -E -v " .. uri
			if model == "raw" then
				args = args .. " -m raw"
			elseif ppd then
				args = args .. " -P " .. ppd
			else
				args = nil
			end
			if not args then
				msg = translate("Choose a driver (raw or a PPD)")
			else
				local ok, out = sh(args)
				if ok == 0 then
					sh("cupsenable " .. name .. "; cupsaccept " .. name)
					if make_default then sh("lpadmin -d " .. name) end
					msg = translate("Queue added: ") .. name
				else
					msg = translate("Failed to add queue: ") .. out
				end
			end
		end

	elseif a == "del" then
		if not name then
			msg = translate("Invalid queue name")
		else
			local ok, out = sh("lpadmin -x " .. name)
			msg = ok == 0 and (translate("Queue deleted: ") .. name)
				or (translate("Failed to delete: ") .. out)
		end

	elseif a == "setdefault" then
		if not name then
			msg = translate("Invalid queue name")
		else
			sh("lpadmin -d " .. name)
			msg = translate("Default printer: ") .. name
		end

	elseif a == "test" then
		if not name then
			msg = translate("Invalid queue name")
		else
			local ok, out = sh("printf 'CUPS helper test page\\n" ..
				"from OpenWrt cups-helper\\n\\n' | lp -d " .. name ..
				" -t cups-helper-test")
			msg = ok == 0 and (translate("Test page sent to ") .. name .. ": " .. out)
				or (translate("Failed to send test page: ") .. out)
		end

	else
		msg = translate("Unknown action")
	end

	http.redirect(disp.build_url("admin/services/cups-helper",
		"msg=" .. http.urlencode(msg or "")))
end
