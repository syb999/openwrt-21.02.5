m = Map("finddev", translate("Find Device"))

m:section(SimpleSection).template  = "finddev_status"

s = m:section(TypedSection, "finddev", "")

s:tab("finddevtab", translate("Find Device Menu"))

dev_mac = s:taboption("finddevtab", Value, "dev_mac", translate("Target MAC address")) 
dev_mac.rmempty = true
dev_mac.datatype = "string"
dev_mac.placeholder = "aa:bb:cc:dd:ee:ff"

search_if = s:taboption("finddevtab", ListValue, "search_if", translate("Search for the network segment"))
search_if.placeholder = "wan"
search_if:value("wan", translate("wan"))
search_if:value("lan", translate("lan"))
search_if.default = "wan"
search_if.rempty = false

dev_search = s:taboption("finddevtab", Button, "dev_search", translate("Start searching")) 
dev_search.rmempty = false
dev_search.inputstyle = "apply"
function dev_search.write(self, section)
	luci.util.exec("sh /usr/bin/finddev.sh >/dev/null 2>&1 &")
end

return m
