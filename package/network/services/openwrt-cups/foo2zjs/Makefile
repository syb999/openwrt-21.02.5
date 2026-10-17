include $(TOPDIR)/rules.mk

PKG_NAME:=foo2zjs
PKG_VERSION:=2021-04-26
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/koenkooi/foo2zjs
PKG_SOURCE_PROTO:=git
PKG_SOURCE_VERSION:=e04290de6b7a30d588f3411fd9834618e09f7b9b
PKG_M5SUM:=b2c1f602fa57a490e684c9de934fd60a

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

# 编 command2foo2lava-pjl.c 要 cups/*.h, 由 feeds/packages/libs/libcups 提供
PKG_BUILD_DEPENDS:=libcups

include $(INCLUDE_DIR)/package.mk

define Package/foo2zjs
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=A linux printer driver for ZjStream protocol.
  URL:=http://foo2zjs.rkkda.com/
  DEPENDS:= +libcups
endef

define Package/foo2zjs/description
	A linux printer driver for ZjStream protocol.
endef

define Package/foo2zjs/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(CP) $(PKG_BUILD_DIR)/getweb $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/bin/* $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/* $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/usr/share
	$(CP) $(PKG_INSTALL_DIR)/usr/share/* $(1)/usr/share/

	# PPD 必须放 CUPS 扫的目录 (/usr/share/ppd 或 /usr/share/cups/model),
	# 上游的 Makefile 把整份 PPD/ 装进 /usr/bin => CUPS 根本看不到。
	$(INSTALL_DIR) $(1)/usr/share/ppd/foo2zjs
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/PPD/*.ppd $(1)/usr/share/ppd/foo2zjs/

	# 固件: 上游 install-extra 用"目标架构"的 arm2hpdl 转换, 在构建机上无法执行
	# => 生成的 .dl 全是 0 字节; 而且它的 sihp1*.img 通配漏了 P1005/P1006/P1505
	# (文件名是 sihpP*.img)。这里用 host 编译器另编一份转换工具, 覆盖全部 sihp*.img。
	$(HOSTCC) $(HOST_CFLAGS) -o $(PKG_BUILD_DIR)/arm2hpdl-host $(PKG_BUILD_DIR)/arm2hpdl.c
	sh ./files/mkfirmware.sh $(PKG_BUILD_DIR) $(1)/usr/share/foo2zjs/firmware $(1)/usr/share/foo2xqx/firmware $(PKG_BUILD_DIR)/arm2hpdl-host
endef

$(eval $(call BuildPackage,foo2zjs))
