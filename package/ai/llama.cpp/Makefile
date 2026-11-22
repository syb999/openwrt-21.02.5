include $(TOPDIR)/rules.mk

PKG_NAME:=llama.cpp
PKG_VERSION:=1.0.0
PKG_RELEASE:=3
PKG_SOURCE:=llama.cpp-v$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/ggml-org/llama.cpp/archive/refs/tags/v$(PKG_VERSION).tar.gz
PKG_HASH:=3f5f750e36326aea4d65c7623e7d597a33e395d65f4d76e5e0532171f252532b
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1

RSTRIP:=:

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

TARGET_CFLAGS   += -O3 -mcpu=cortex-a55+dotprod
TARGET_CXXFLAGS += -O3 -mcpu=cortex-a55+dotprod

define Package/llama.cpp
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=llama.cpp tools (server / cli / bench)
  DEPENDS:=+libstdcpp +libpthread
endef

define Package/llama.cpp/description
  llama.cpp built with the OpenWrt toolchain (musl/aarch64), installed to
  /opt/llama/. Flash-safe: the binaries ship inside the firmware image.
endef

CMAKE_OPTIONS += \
	-DCMAKE_CXX_STANDARD_LIBRARIES="-lstdc++fs" \
	-DLLAMA_BUILD_SERVER=ON \
	-DLLAMA_BUILD_TOOLS=ON \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=OFF \
	-DLLAMA_BUILD_HTML=OFF \
	-DLLAMA_CURL=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_OPENMP=OFF \
	-DGGML_BACKEND_DL=OFF \
	-DBUILD_SHARED_LIBS=OFF

define Package/llama.cpp/install
	$(INSTALL_DIR) $(1)/opt/llama
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/llama-server $(1)/opt/llama/llama-server
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/llama-cli    $(1)/opt/llama/llama-cli
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/llama-bench  $(1)/opt/llama/llama-bench
endef

$(eval $(call BuildPackage,llama.cpp))
