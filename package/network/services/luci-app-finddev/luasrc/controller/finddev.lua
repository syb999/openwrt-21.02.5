module("luci.controller.finddev", package.seeall)

function index()
	local page = entry({"admin", "network", "finddev"}, cbi("finddev"), _("Find Device"))
	page.dependent = true

	entry({"admin", "network", "finddev", "status"}, call("finddev_status")).leaf = true
end

function finddev_status()
	local e = {
		running = (luci.sys.call("ps | grep -v grep | grep finddev.sh >/dev/null") == 0),
		device = luci.sys.exec("cat /tmp/finddev_.tmp")
	}

	luci.http.prepare_content("application/json")
	luci.http.write_json(e)
end

