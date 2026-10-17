include $(TOPDIR)/rules.mk

PKG_NAME:=cups-printer-choose
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/cups-printer-choose/Default
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Printing
  URL:=https://www.cups.org/
endef

# 安装后在路由器上执行 cups-printer-choose 即可选择打印机型号并建队列。
# 驱动包按 menuconfig 里的 CUPS_PRINTER_CHOOSE_* 开关自动带进来。
define Package/cups-printer-choose
$(call Package/cups-printer-choose/Default)
  TITLE:=CUPS printer queue chooser (选型号 / raw 打印服务器)
  DEPENDS:=+cups +cups-client +kmod-usb-printer \
	+CUPS_PRINTER_CHOOSE_HP_ZJS:foo2zjs \
	+CUPS_PRINTER_CHOOSE_BROTHER:brlaser \
	+CUPS_PRINTER_CHOOSE_CANON_BJNP:cups-bjnp
endef

define Package/cups-printer-choose/description
  One command on the router to pick a printer and create the CUPS queue.

   raw 模式: 路由器只转发, 客户端自带驱动 (Windows/手机/Linux 客户端装厂商驱动),
             不需要 ghostscript/cups-filters, P1006/P1106/针打 都能这样用。
   ppd 模式: 路由器侧出驱动, 客户端发 PDF 直接打; 需要 cups-filters + ghostscript,
             以及对应的驱动包 (menuconfig 里勾 CUPS_PRINTER_CHOOSE_* 子项)。

  命令: cups-printer-choose [-l|-c|-d|-a URI -n 名字 [-p PPD|-r]|-D -n 名字]
endef

define Package/cups-printer-choose/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/cups-printer-choose $(1)/usr/sbin/

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/50-cups-enable $(1)/etc/uci-defaults/

	# HP 9x0/10xx/P100x 上电后必须先灌固件, OpenWrt 没有 udev ⇒ hotplug.d
	$(INSTALL_DIR) $(1)/etc/hotplug.d/usb
	$(INSTALL_BIN) ./files/30-cups-printer-firmware $(1)/etc/hotplug.d/usb/

	# 随包带一个 24 针 Epson (ESC/P2, 走 gs 的 lq850 设备) 的 PPD
	$(INSTALL_DIR) $(1)/usr/share/ppd/epson
	$(INSTALL_DATA) ./files/ppd/Epson-LQ-850-24pin.ppd $(1)/usr/share/ppd/epson/
endef

# 纯脚本包, 没有源码要编 (默认的 Build/Compile 会在空目录里跑 make)
define Build/Compile
endef

define Package/cups-printer-choose/config
	source "$(SOURCE)/Config.in"
endef

$(eval $(call BuildPackage,cups-printer-choose))
