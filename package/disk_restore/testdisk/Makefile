#
# testdisk / photorec for OpenWrt
#
# Both binaries come from the same source tarball:
#   testdisk  - partition table and boot sector repair, file undelete
#   photorec  - signature based file carving (the "last resort" recovery)
#   fidentify - identify a file type from its content
#

include $(TOPDIR)/rules.mk

PKG_NAME:=testdisk
PKG_VERSION:=7.2
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.bz2
PKG_SOURCE_URL:=https://www.cgsecurity.org/
PKG_HASH:=f8343be20cb4001c5d91a2e3bcd918398f00ae6d8310894a5a9f2feb813c283f

PKG_MAINTAINER:=syb999
PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=COPYING

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

# no Qt (no X11 on the target), no libewf, no old libntfs, no progsreiserfs.
# libntfs-3g IS enabled (used by testdisk's NTFS undelete) and so is ncursesw,
# ext2fs, libuuid, zlib and libjpeg - all of them provide a better scan.
CONFIGURE_ARGS += \
	--disable-qt \
	--without-ewf \
	--without-ntfs \
	--without-reiserfs

define Package/testdisk/Default
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=Disk restore
  URL:=https://www.cgsecurity.org/
endef

define Package/testdisk
  $(call Package/testdisk/Default)
  TITLE:=Partition table / boot sector recovery and repair
  DEPENDS:=+libncurses +ntfs-3g +libext2fs +zlib +libjpeg-turbo
endef

define Package/testdisk/description
  TestDisk checks and repairs a partition table (MBR, GPT, ...) and recovers
  deleted partitions / boot sectors. It can also list and undelete files on
  FAT, exFAT, NTFS, ext2/3/4, HFS+ and other filesystems.

  Includes fidentify, which reports the filesystem type of an image file.
endef

define Package/photorec
  $(call Package/testdisk/Default)
  TITLE:=Signature based file carving (photo/file recovery)
  DEPENDS:=+libncurses +ntfs-3g +libext2fs +zlib +libjpeg-turbo
endef

define Package/photorec/description
  PhotoRec is a file data recovery tool. It recovers lost files (pictures,
  video, documents, archives, ...) from a partition or a disk image by
  looking for file signatures, ignoring the filesystem. Because it ignores
  the filesystem it also works on a formatted or badly damaged volume.

  It never writes to the source device, recovered files are written to a
  directory the user picks at start up (recup_dir.NNN).
endef

define Package/testdisk/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/testdisk $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/fidentify $(1)/usr/sbin/
endef

define Package/photorec/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/photorec $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,testdisk))
$(eval $(call BuildPackage,photorec))
