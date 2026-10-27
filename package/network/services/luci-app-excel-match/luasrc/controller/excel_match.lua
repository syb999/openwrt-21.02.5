module("luci.controller.excel_match", package.seeall)

function index()
	entry({"admin", "services", "excel-match"}, cbi("excel_match/settings"), _("Excel 匹配"), 62)
end
