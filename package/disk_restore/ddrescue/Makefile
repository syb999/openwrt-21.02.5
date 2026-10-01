#
# GNU ddrescue for OpenWrt
#
# GNU ddrescue is the reference tool for imaging a failing disk: it reads the
# good areas first, keeps a mapfile of what has been recovered and only goes
# back for the damaged areas on later passes.  Use it as the first step before
# any filesystem level recovery (testdisk / photorec).
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ddrescue
PKG_VERSION:=1.30
PKG_RELEASE:=1

# Debian ships the pristine upstream release as .orig.tar.gz (upstream itself
# only distributes .tar.lz, which the old buildroot in this tree cannot unpack)
PKG_SOURCE:=gddrescue_$(PKG_VERSION).orig.tar.gz
PKG_SOURCE_URL:=https://deb.debian.org/debian/pool/main/g/gddrescue \
	https://ftp.gnu.org/gnu/ddrescue
PKG_HASH:=d01c9ff0599a13d10261c9b435333cc4aaff016be226788cfe515329c221789a
PKG_BUILD_DIR:=$(BUILD_DIR)/gddrescue-$(PKG_VERSION)

PKG_MAINTAINER:=syb999
PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=COPYING

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/uclibc++.mk
include $(INCLUDE_DIR)/package.mk

define Package/ddrescue
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=Disk restore
  TITLE:=GNU ddrescue - data recovery / disk imaging tool
  URL:=https://www.gnu.org/software/ddrescue/
  DEPENDS:=$(CXX_DEPENDS)
  CONFLICTS:=gddrescue
endef

define Package/ddrescue/description
  GNU ddrescue is a data recovery tool. It copies data from one file or block
  device (hard disc, cdrom, etc) to another, trying to rescue the good parts
  first in case of read errors.

  It keeps a mapfile so an interrupted rescue can be resumed, and it never
  writes to the source device. Recommended first step before testdisk/photorec.
endef

# ddrescue uses a hand written configure script (no autoconf), it only takes
# VAR=VALUE assignments.
#
# -include cctype: ddrescue 1.30 calls std::isspace()/std::isdigit() without
# including <cctype>; glibc's libstdc++ headers pull it in implicitly, musl
# does not, so the declaration has to be forced in.
CONFIGURE_ARGS += \
	CXX="$(TARGET_CXX)" \
	CPPFLAGS="$(TARGET_CPPFLAGS)" \
	CXXFLAGS="$(TARGET_CXXFLAGS) -include cctype" \
	LDFLAGS="$(TARGET_LDFLAGS)"

define Package/ddrescue/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/ddrescue $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/ddrescuelog $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,ddrescue))
