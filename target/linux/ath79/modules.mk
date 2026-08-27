LEDS_MENU:=LED modules
SOUND_MENU:=Sound Support

define KernelPackage/leds-reset
  SUBMENU:=$(LEDS_MENU)
  TITLE:=reset controller LED support
  DEPENDS:= @TARGET_ath79
  KCONFIG:=CONFIG_LEDS_RESET=m
  FILES:=$(LINUX_DIR)/drivers/leds/leds-reset.ko
  AUTOLOAD:=$(call AutoLoad,60,leds-reset,1)
endef

define KernelPackage/leds-reset/description
 Kernel module for LEDs on reset lines
endef

$(eval $(call KernelPackage,leds-reset))


define KernelPackage/sound-wm8904
  SUBMENU:=$(SOUND_MENU)
  TITLE:=WM8904 I2S Audio Driver
  DEPENDS:=@TARGET_ath79 \
	+kmod-sound-soc-core \
	+kmod-i2c-core \
	+LINUX_5_4:kmod-i2c-algo-bit
  KCONFIG:= \
	CONFIG_SND=y \
	CONFIG_SND_SOC=y \
	CONFIG_SND_SOC_I2C_AND_SPI=y \
	CONFIG_SND_SOC_WM8904=y
  FILES:= \
	$(LINUX_DIR)/sound/soc/codecs/snd-soc-wm8904.ko
  AUTOLOAD:=$(call AutoLoad,50,snd-soc-wm8904)
endef

define KernelPackage/sound-wm8904/description
  WM8904 audio codec support. Also supports WM8918 which is a subset.
endef

$(eval $(call KernelPackage,sound-wm8904))


define KernelPackage/sound-ath79-soc
  SUBMENU:=$(SOUND_MENU)
  TITLE:=AR934x I2S CPU DAI + PISEN WMB001N machine driver
  DEPENDS:=@TARGET_ath79 \
	+kmod-sound-soc-core \
	+kmod-sound-wm8904
  KCONFIG:= \
	CONFIG_SND=y \
	CONFIG_SND_SOC=y \
	CONFIG_SND_ATH79_SOC=y
  FILES:= \
	$(LINUX_DIR)/sound/soc/ath79/snd-soc-ath79-i2s.ko \
	$(LINUX_DIR)/sound/soc/ath79/snd-soc-pisen-wm8918.ko
  AUTOLOAD:=$(call AutoLoad,55,snd-soc-ath79-i2s snd-soc-pisen-wm8918)
endef

define KernelPackage/sound-ath79-soc/description
  AR934x I2S CPU DAI + PISEN WMB001N (WM8918) machine driver.
endef

$(eval $(call KernelPackage,sound-ath79-soc))


define KernelPackage/sound-simple-card
  SUBMENU:=$(SOUND_MENU)
  TITLE:=Simple Audio Card
  DEPENDS:=@TARGET_ath79 \
	+kmod-sound-soc-core
  KCONFIG:= \
	CONFIG_SND=y \
	CONFIG_SND_SOC=y \
	CONFIG_SND_SIMPLE_CARD=y \
	CONFIG_SND_SIMPLE_CARD_UTILS=y
  FILES:= \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card-utils.ko \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card.ko
  AUTOLOAD:=$(call AutoLoad,88,snd-soc-simple-card-utils snd-soc-simple-card)
endef

define KernelPackage/sound-simple-card/description
  Simple Audio Card for ALSA SoC.
endef

$(eval $(call KernelPackage,sound-simple-card))


#
# Audio Graph Card
#
define KernelPackage/sound-audio-graph-card
  SUBMENU:=$(SOUND_MENU)
  TITLE:=Audio Graph Card
  DEPENDS:=@TARGET_ath79 \
	+kmod-sound-soc-core \
	+kmod-sound-simple-card
  KCONFIG:= \
	CONFIG_SND=y \
	CONFIG_SND_SOC=y \
	CONFIG_SND_AUDIO_GRAPH_CARD=y \
	CONFIG_SND_SIMPLE_CARD_UTILS=y
  FILES:= \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-audio-graph-card.ko
  AUTOLOAD:=$(call AutoLoad,88,snd-soc-audio-graph-card)
endef

define KernelPackage/sound-audio-graph-card/description
  Audio Graph Card for ALSA SoC.
endef

$(eval $(call KernelPackage,sound-audio-graph-card))