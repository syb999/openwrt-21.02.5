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

-- 注意: 字符类里必须有 %% —— CUPS 的 usb URI 是百分号编码的
-- (usb://HP/LaserJet%20Professional%20P1106?serial=...)，漏了 %% 会被判成非法 URI
local function safe_uri(v)
	if not v or #v > 200 then return nil end
	if v:match("^[%w_%.%-:/%%?#%[%]@=+,]+$") then return v end
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

	elseif a == "add" or a == "edit" then
		local is_edit = (a == "edit")
		if not name then
			msg = translate("Queue name may only contain letters, digits, _ . -")
		elseif not is_edit and not uri then
			msg = translate("Device URI is empty or invalid")
		else
			-- 设备地址里可能有 % ? = (usb://...?serial=...)，一律单引号包起来
			local args = "lpadmin -p " .. name .. " -E"
			if uri then args = args .. " -v '" .. uri .. "'" end
			-- model=keep 是编辑模式下的"只改地址，不动驱动"
			if model ~= "keep" then
				-- driverless (IPP everywhere): 网络打印机不用 PPD, CUPS 自己问设备要能力
				if model == "everywhere" then
					args = args .. " -m everywhere"
				-- 选了 PPD 就以 PPD 为准: 否则"model 停在 raw、下面又挑了 PPD"会静默建出
				-- raw 队列, 客户端发 PDF 时打出一堆乱码/空白, 表现就是"添加了但没用"
				elseif ppd then
					args = args .. " -P '" .. ppd .. "'"
				elseif model == "raw" then
					args = args .. " -m raw"
				else
					args = nil
				end
			end
			if not args then
				msg = translate("Choose a driver (raw or a PPD)")
			else
				local ok, out = sh(args)
				if ok == 0 then
					sh("cupsenable " .. name .. "; cupsaccept " .. name)
					if make_default then sh("lpadmin -d " .. name) end
					-- 队列变了就重建 mDNS 发布, 否则客户端要等下次"应用优化设置"才能搜到
					sh("/usr/lib/cups-helper/apply.sh mdns")
					-- 同时让 samba 重建打印共享: 共享名 = 队列名, 不重建的话
					-- \\盒子\<旧队列名> 会一直指向一个已经不存在的队列, Windows 就"无法添加打印机"
					sh("[ -x /etc/init.d/samba ] && /etc/init.d/samba reload")
					msg = is_edit and (translate("Queue updated: ") .. name)
						or (translate("Queue added: ") .. name)
				else
					msg = is_edit and (translate("Failed to update queue: ") .. out)
						or (translate("Failed to add queue: ") .. out)
				end
			end
		end

	elseif a == "del" then
		if not name then
			msg = translate("Invalid queue name")
		else
			local ok, out = sh("lpadmin -x " .. name)
			if ok == 0 then
				sh("/usr/lib/cups-helper/apply.sh mdns")
				sh("[ -x /etc/init.d/samba ] && /etc/init.d/samba reload")
			end
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
			-- 用 CUPS 自带的 PDF 测试页: 文本(text/plain)要过 texttopdf, 盒子上没装字体
			-- 就会报 "No usable font available"; PDF 走 pdftopdf/pdftops, 不需要字体
			local ok, out = sh(
				"p=/usr/share/cups/data/default-testpage.pdf; " ..
				"if [ -f $p ]; then lp -d " .. name .. " -t cups-helper-test $p; " ..
				"else printf '%%!PS-Adobe-3.0\n/Helvetica findfont 14 scalefont setfont\n" ..
				"72 720 moveto (cups-helper test page) show\nshowpage\n' >/tmp/ch-test.ps; " ..
				"lp -d " .. name .. " -t cups-helper-test /tmp/ch-test.ps; rm /tmp/ch-test.ps; fi")
			msg = ok == 0 and (translate("Test page sent to ") .. name .. ": " .. out)
				or (translate("Failed to send test page: ") .. out)
		end

	else
		msg = translate("Unknown action")
	end

	http.redirect(disp.build_url("admin/services/cups-helper") ..
		"?msg=" .. http.urlencode(msg or ""))
end
