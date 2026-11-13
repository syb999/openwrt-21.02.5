include $(TOPDIR)/rules.mk

PKG_NAME:=dss
PKG_VERSION:=1.0
PKG_RELEASE:=110
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/dss
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=DeepSeek in your shell (web account, no browser, no paid API)
  DEPENDS:=+node
endef

define Package/dss/description
 DeepSeek inside a plain shell, using your own chat.deepseek.com web session:

  * /usr/bin/dss      CLI. Streams answers in the terminal. The site's own
                      proof-of-work worker (pow_worker.js + chunk + sha3_wasm_bg.wasm)
                      runs inside node, so no browser is needed and no API
                      credits are spent. Supports --file upload, --search,
                      --duet (self-dialogue), --code, script detection and
                      !run / !run N send execution loop.
  * /usr/bin/dss-web  small HTTP helper (init: /etc/init.d/dss) serving
                      http://<box>:88/token so a fresh token can be pasted
                      from Chrome (F12 -> copy(localStorage.userToken)).

 Credentials live in /root/.dss/cred (0600) and are never written to the image.
 Conversation log: /tmp/dss/conversation.log. Generated code: /root/.dss/out.
endef

define Build/Compile
endef

define Package/dss/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/dss.js $(1)/usr/bin/dss
	$(INSTALL_BIN) ./files/dss-web.js $(1)/usr/bin/dss-web

	$(INSTALL_DIR) $(1)/opt/dss
	$(INSTALL_DATA) ./files/pow_worker.js $(1)/opt/dss/pow_worker.js
	$(INSTALL_DATA) ./files/pow_chunk8138.js $(1)/opt/dss/pow_chunk8138.js
	$(INSTALL_DATA) ./files/sha3_wasm_bg.wasm $(1)/opt/dss/sha3_wasm_bg.wasm
	$(INSTALL_DATA) ./files/pow_thread.js $(1)/opt/dss/pow_thread.js

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/dss.init $(1)/etc/init.d/dss

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/dss.config $(1)/etc/config/dss
endef

define Package/dss/conffiles
/etc/config/dss
endef

$(eval $(call BuildPackage,dss))
