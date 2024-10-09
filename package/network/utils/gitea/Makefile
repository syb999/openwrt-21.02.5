include $(TOPDIR)/rules.mk

PKG_NAME:=gitea
PKG_VERSION:=1.22.3
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  TITLE:=Web administration of git repositories
  URL:=https://gitea.com/
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Version Control Systems
  USERID:=gitea=6801:gitea=6801
  DEPENDS:=@(TARGET_amlogic||TARGET_rockchip||TARGET_sunxi_cortexa7||TARGET_brcm2708_bcm2710||TARGET_bcm27xx||TARGET_armvirt||TARGET_x86_64) +git-http +sqlite3-cli +shadow-useradd +shadow-su
endef

define Package/$(PKG_NAME)/description
  Gitea: Git with a cup of tea
  git management with a web interface
endef

ifeq ($(ARCH),x86_64)
	gitea_ARCH:=linux-amd64
	PKG_HASH:=a720ff937912a6eb6c0cacf6ebcdd774deed5197cd945ecc34f5744cb5c517e8
endif

ifeq ($(ARCH),arm)
	gitea_ARCH:=linux-arm-5
	PKG_HASH:=f4b1791d983555601e626be4eb1120a6b3e65e043148247bdd77290b4ebc2e6c
endif

ifeq ($(ARCH),aarch64)
	gitea_ARCH:=linux-arm64
	PKG_HASH:=0d957ca51317be75788a7d286193fb550463c432518fb4b2dd05c19df3910b22
endif

PKG_SOURCE_URL:=https://dl.gitea.com/gitea/1.22.3/
PKG_SOURCE:=gitea-1.22.3-$(gitea_ARCH)

define Build/Prepare
	mv $(DL_DIR)/$(PKG_SOURCE) $(PKG_BUILD_DIR)
endef

define Build/Compile
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/gitea.init $(1)/etc/init.d/gitea
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/$(PKG_SOURCE) $(1)/usr/bin/gitea
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
