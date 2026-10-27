#
# luci-app-cups-helper - CUPS helper: status, diagnostics, one-click queue setup
#
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-cups-helper
PKG_VERSION:=1.0.0
PKG_RELEASE:=8
PKG_PO_VERSION:=1.0.0

PKG_LICENSE:=MIT
PKG_MAINTAINER:=local

LUCI_TITLE:=CUPS helper: status, diagnostics, queue setup, mDNS discovery
LUCI_DESCRIPTION:=Show cupsd state and queues, start/stop/autostart it, create USB or network queues \
(raw or PPD), send a test page, publish queues over mDNS (avahi), tune cupsd for a small device, \
and point out the usual failures (User root, missing lp account, no /dev/usb/lp0).

define Package/luci-app-cups-helper/config
	menu "Printer support"

	config CUPS_HELPER_RENDER
		bool "Router-side rendering (cups-filters + ghostscript)"
		default y
		help
		  Needed unless every client renders jobs itself (raw queues only).
		  Pulls in cups-filters and ghostscript; ghostscript is ~30 MB.

	config CUPS_HELPER_AVAHI
		bool "mDNS zero-config discovery (avahi + dbus)"
		default y
		help
		  Publishes every print queue as _ipp._tcp so phones and desktops
		  find the printer automatically instead of typing ipp:// manually.

	config CUPS_HELPER_DRV_FOO2ZJS
		bool "foo2zjs driver (HP P1005/P1006/P1102/P1106/P1505, Samsung CLP)"
		default y

	config CUPS_HELPER_DRV_BRLASER
		bool "brlaser driver (Brother laser printers)"
		default n

	config CUPS_HELPER_DRV_CUPS_BJNP
		bool "cups-bjnp backend (Canon BJNP network printers)"
		default n

	config CUPS_HELPER_DRV_P910ND
		bool "p910nd raw print server (share the printer on port 9100)"
		default n

	config CUPS_HELPER_DRV_HPLIP
		bool "hplip support (HP multi-function, x86 targets only)"
		depends on TARGET_x86 || TARGET_x86_64

	endmenu
endef

LUCI_DEPENDS:=+cups +cups-client \
	+CUPS_HELPER_RENDER:cups-filters +CUPS_HELPER_RENDER:ghostscript \
	+CUPS_HELPER_AVAHI:avahi-daemon +CUPS_HELPER_AVAHI:dbus \
	+CUPS_HELPER_DRV_FOO2ZJS:foo2zjs \
	+CUPS_HELPER_DRV_BRLASER:brlaser \
	+CUPS_HELPER_DRV_CUPS_BJNP:cups-bjnp \
	+CUPS_HELPER_DRV_P910ND:p910nd \
	+CUPS_HELPER_DRV_HPLIP:hplip-common

LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
