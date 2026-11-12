-- luci-app-xlsx2sqlite: LuCI 1.x (luasrc) controller, works on 15.05 and 21.02(luci-compat)
module("luci.controller.xlsx2sqlite", package.seeall)

function index()
	entry({ "admin", "services", "xlsx2sqlite" },
	      template("xlsx2sqlite/main"),
	      _("Excel 汇总"), 68)
end
