include $(TOPDIR)/rules.mk

PKG_NAME:=gitea
PKG_VERSION:=1.21.7
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  TITLE:=Web administration of git repositories
  URL:=https://gitea.com/
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Version Control Systems
  USERID:=gitea=6801:gitea=6801
  DEPENDS:=@(TARGET_sunxi_cortexa7||TARGET_brcm2708_bcm2710||TARGET_bcm27xx||TARGET_armvirt||TARGET_x86_64) +git-http +sqlite3-cli +shadow-useradd +shadow-su
endef

define Package/$(PKG_NAME)/description
  Gitea: Git with a cup of tea
  git management with a web interface
endef

ifeq ($(ARCH),x86_64)
	gitea_ARCH:=linux-amd64
	PKG_HASH:=fa88e6404d3d34136bdd50c990a8c390d5e05f4cb2e31641559d14234e022bd6
endif

ifeq ($(ARCH),arm)
	gitea_ARCH:=linux-arm-5
	PKG_HASH:=8df9d51e611375d4e97e46b57a4a9868790ebd4cd247ffcaba9c3d9aea68b741
endif

ifeq ($(ARCH),aarch64)
	gitea_ARCH:=linux-arm64
	PKG_HASH:=508f21e9146409c54b77dfa266bfb29a9f7710e6468f4a7bcd9b9d5e41e4c309
endif

PKG_SOURCE_URL:=https://dl.gitea.com/gitea/1.21.7/
PKG_SOURCE:=gitea-1.21.7-$(gitea_ARCH)

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
