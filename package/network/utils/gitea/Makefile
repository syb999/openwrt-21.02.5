include $(TOPDIR)/rules.mk

PKG_NAME:=gitea
PKG_VERSION:=1.23.5
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
	PKG_HASH:=60773eabdacaf2d787200649b6756a66d96cb581ae45df9d492f952f97926915
endif

ifeq ($(ARCH),arm)
	gitea_ARCH:=linux-arm-5
	PKG_HASH:=a1d19b84b8062d1c56dbc99835efae4b6c3970e95bc8cabda7541e7afe5904ed
endif

ifeq ($(ARCH),aarch64)
	gitea_ARCH:=linux-arm64
	PKG_HASH:=9872f2fdf7e1187304bd69d0d0771350b4f1be98179f917fb60273e5b070a570
endif

PKG_SOURCE_URL:=https://dl.gitea.com/gitea/1.23.5/
PKG_SOURCE:=gitea-1.23.5-$(gitea_ARCH)

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
