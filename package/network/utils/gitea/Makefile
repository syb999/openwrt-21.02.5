include $(TOPDIR)/rules.mk

PKG_NAME:=gitea
PKG_VERSION:=1.22.2
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  TITLE:=Web administration of git repositories
  URL:=https://gitea.com/
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Version Control Systems
  USERID:=gitea=6801:gitea=6801
  DEPENDS:=@(TARGET_rockchip||TARGET_sunxi_cortexa7||TARGET_brcm2708_bcm2710||TARGET_bcm27xx||TARGET_armvirt||TARGET_x86_64) +git-http +sqlite3-cli +shadow-useradd +shadow-su
endef

define Package/$(PKG_NAME)/description
  Gitea: Git with a cup of tea
  git management with a web interface
endef

ifeq ($(ARCH),x86_64)
	gitea_ARCH:=linux-amd64
	PKG_HASH:=310d7dfa48680f3eaa5fe2998ca7a8a1f707518c1772b64d85fb3b9e197188d3
endif

ifeq ($(ARCH),arm)
	gitea_ARCH:=linux-arm-5
	PKG_HASH:=daa74bbf9d6b59fcd65b85ec09bde0a1d5089cfe3cc68220ddaf56de19520aa7
endif

ifeq ($(ARCH),aarch64)
	gitea_ARCH:=linux-arm64
	PKG_HASH:=a4df27d2cb9339bdea96cd51f694cdf2d2bd7301d38f4906ad3611a033537bae
endif

PKG_SOURCE_URL:=https://dl.gitea.com/gitea/1.22.2/
PKG_SOURCE:=gitea-1.22.2-$(gitea_ARCH)

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
