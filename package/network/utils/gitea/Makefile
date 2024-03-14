include $(TOPDIR)/rules.mk

PKG_NAME:=gitea
PKG_VERSION:=1.21.8
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
	PKG_HASH:=01817c5730d50f5cf2470bbe2a35fceb7e0375903e36b6ebcd496f799a35b0de
endif

ifeq ($(ARCH),arm)
	gitea_ARCH:=linux-arm-5
	PKG_HASH:=581fd7f7981d8f95c521adfb395a84bc0b32bf096dd94c4e64cb8d552cd914d1
endif

ifeq ($(ARCH),aarch64)
	gitea_ARCH:=linux-arm64
	PKG_HASH:=d4d2f68b6ac5cdc021b48240dc6aa523389f43fc9a7532c287515f27a62cdfb5
endif

PKG_SOURCE_URL:=https://dl.gitea.com/gitea/1.21.8/
PKG_SOURCE:=gitea-1.21.8-$(gitea_ARCH)

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
