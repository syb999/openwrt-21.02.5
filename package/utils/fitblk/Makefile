include $(TOPDIR)/rules.mk

PKG_NAME:=fitblk
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Daniel Golle <daniel@makrotopia.org>

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/fitblk
  HIDDEN:=1
  SECTION:=base
  CATEGORY:=Base system
  TITLE:=fitblk firmware release tool
  # 注: 上游此处是 DEPENDS:=+fit-check-sign，但 21.02.5 没有该包。
  # 我们的 lib/upgrade/fit.sh 已对 fit_check_sign 做了 "有才用" 的兼容处理。
  DEPENDS:=
endef

define Package/fitblk/description
Release uImage.FIT block devices using ioctl.
endef

define Build/Configure
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Werror" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/fitblk/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fitblk $(1)/usr/sbin/
	# 注: 上游还会安装 ./files/fit.sh 到 /lib/upgrade，但本树
	# target/linux/airoha/base-files/lib/upgrade/fit.sh 已经提供了同一文件
	# （且对全部 airoha 设备生效），重复安装会触发 opkg 文件冲突，故此处不装。
endef

$(eval $(call BuildPackage,fitblk))
