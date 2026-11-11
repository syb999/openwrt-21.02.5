#
# ONMP: Nginx + MariaDB + PHP8 (+Redis) web stack for OpenWrt 21.02
#
include $(TOPDIR)/rules.mk

PKG_NAME:=ONMP
PKG_VERSION:=1.0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=syb999

include $(INCLUDE_DIR)/package.mk

# script-only package: nothing to compile
define Build/Prepare
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/ONMP
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Web Servers/Proxies
  TITLE:=ONMP - Nginx(81) + MariaDB + PHP8 + Redis one-key stack
  URL:=https://github.com/xzhih/ONMP
  DEPENDS:=+nginx-ssl +mariadb-server +mariadb-server-base +mariadb-client +redis-server \
	+php8 +php8-cli +php8-fpm \
	+php8-mod-mysqli +php8-mod-mysqlnd +php8-mod-pdo +php8-mod-pdo-mysql \
	+php8-mod-pdo-sqlite +php8-mod-sqlite3 \
	+php8-mod-session +php8-mod-opcache +php8-mod-mbstring +php8-mod-curl \
	+php8-mod-gd +php8-mod-zip +php8-mod-xml +php8-mod-xmlreader \
	+php8-mod-xmlwriter +php8-mod-simplexml +php8-mod-dom +php8-mod-ctype \
	+php8-mod-fileinfo +php8-mod-filter +php8-mod-iconv +php8-mod-openssl \
	+php8-mod-tokenizer +php8-mod-phar +php8-mod-bcmath +php8-mod-calendar \
	+php8-mod-exif
endef

define Package/ONMP/description
  ONMP is a LNMP-like web stack for OpenWrt: nginx (default port 81),
  MariaDB, PHP8-FPM and Redis, all managed by a single init script.
  Uses the firmware php8 (patched against CVE-2026-109618/109619).
  It runs its own nginx instance with a private pid/log/config and does
  not touch the nginx-rtmp-server package.
  uci-defaults also makes the stack bootable as-is: creates the mariadb
  runtime user, writes datadir/sql_mode into /etc/mysql/conf.d/onmp.cnf,
  clears php.ini doc_root (php-fpm refuses scripts outside it) and gives
  php-fpm (nobody) a writable session dir.
endef

define Package/ONMP/conffiles
/etc/config/onmp
/etc/onmp/nginx.conf
endef

define Package/ONMP/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/onmp.init $(1)/etc/init.d/onmp
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DATA) ./files/onmp.config $(1)/etc/config/onmp
	$(INSTALL_DIR) $(1)/etc/onmp/vhosts
	$(INSTALL_DATA) ./files/nginx.conf $(1)/etc/onmp/nginx.conf
	$(INSTALL_DATA) ./files/fastcgi_params $(1)/etc/onmp/fastcgi_params
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/95-onmp $(1)/etc/uci-defaults/95-onmp
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/onmp.cli $(1)/usr/bin/onmp
endef

$(eval $(call BuildPackage,ONMP))
