#
# ffmpeg-rkrga - standalone FFmpeg for Rockchip SoCs
#
# Adds the RGA 2D acceleration filters (scale_rkrga / vpp_rkrga /
# overlay_rkrga) and the rkmpp hardware codecs on top of a regular FFmpeg
# tree.  Upstream FFmpeg does not carry these blocks, they live in the
# Rockchip FFmpeg fork; the original rockchip-linux/ffmpeg repository has
# been removed, so this package uses the actively maintained successor
# nyanmisaka/ffmpeg-rockchip (branch 6.0).
#
# This package is intentionally independent from feeds/packages/multimedia/
# ffmpeg:
#   * binary is installed as /usr/bin/ffmpeg-rkrga (no name clash)
#   * libraries are built statically, nothing is installed in /usr/lib
#   * the system ffmpeg / libffmpeg-* files are never touched
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ffmpeg-rkrga
PKG_VERSION:=6.0.2
PKG_RELEASE:=1

FF_GIT_HASH:=ad9403cea77c36e9a523bcda244307ef823379b0
PKG_SOURCE:=$(FF_GIT_HASH).tar.gz
PKG_SOURCE_URL:=https://github.com/nyanmisaka/ffmpeg-rockchip/archive
PKG_HASH:=b169fd108cad6aa97e7f8dcb6b188b29a2c005142a676438c155996d55fed92c
PKG_BUILD_DIR:=$(BUILD_DIR)/ffmpeg-rockchip-$(FF_GIT_HASH)

PKG_MAINTAINER:=local build
PKG_LICENSE:=LGPL-2.1-or-later GPL-2.0-or-later
PKG_LICENSE_FILES:=COPYING.LGPLv2.1 COPYING.GPLv2

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/ffmpeg-rkrga
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=FFmpeg with RGA scale_rkrga + MPP hardware codecs
  URL:=https://github.com/nyanmisaka/ffmpeg-rockchip
  DEPENDS:=@aarch64 +librga +rockchip-mpp +libdrm +libstdcpp +zlib
endef

define Package/ffmpeg-rkrga/description
 Standalone FFmpeg build for Rockchip SoCs providing:
  - hardware scaling / format conversion via the RGA2 block
    (scale_rkrga, vpp_rkrga, overlay_rkrga avfilters)
  - hardware H.264/H.265/VP8/VP9/MPEG video decode + encode via the
    Rockchip MPP (rkmpp) decoders/encoders (requires the kernel
    rk_vcodec / mpp-srv support).

 The command is installed as /usr/bin/ffmpeg-rkrga and the libraries are
 statically linked, so it coexists with the distribution ffmpeg package.
endef

FFMPEG_RKRGA_FILTERS:= \
	scale scale_rkrga vpp_rkrga overlay_rkrga \
	format null anull fps setpts crop transpose \
	hwupload hwdownload hwmap aformat split copy select trim vflip hflip pad

FFMPEG_RKRGA_DECODERS:= \
	png mjpeg rawvideo h264 hevc mpeg1video mpeg2video mpeg4 vp8 vp9 av1 \
	h263 wmv1 wmv2 gif bmp ppm tiff webp apng zlib \
	h264_rkmpp hevc_rkmpp mjpeg_rkmpp mpeg1_rkmpp mpeg2_rkmpp mpeg4_rkmpp \
	vp8_rkmpp vp9_rkmpp h263_rkmpp av1_rkmpp

FFMPEG_RKRGA_ENCODERS:= \
	png mjpeg rawvideo mpeg4 gif bmp ppm tiff \
	h264_rkmpp hevc_rkmpp mjpeg_rkmpp

FFMPEG_RKRGA_DEMUXERS:= \
	image2 image2pipe image_png_pipe image_jpeg_pipe image_bmp_pipe \
	image_tiff_pipe image_webp_pipe mjpeg rawvideo h264 hevc matroska mov \
	avi mpegts rtsp rtp sdp concat yuv4mpegpipe flv mpegvideo mpegps \
	mp3 wav ogg gif

FFMPEG_RKRGA_MUXERS:= \
	image2 image2pipe rawvideo mjpeg mp4 matroska avi mpegts h264 hevc \
	rtp null wav adts flv gif

FFMPEG_RKRGA_PARSERS:= \
	h264 hevc mjpeg png mpeg4video mpegvideo vp8 vp9 aac mpegaudio av1

FFMPEG_RKRGA_BSFS:= \
	h264_mp4toannexb hevc_mp4toannexb mpeg4_unpack_bframes dump_extradata \
	extract_extradata vp9_superframe null remove_extradata

FFMPEG_RKRGA_PROTOCOLS:=file pipe tcp udp rtp cache data http

FFMPEG_RKRGA_INDEVS:=v4l2 lavfi
FFMPEG_RKRGA_OUTDEVS:=v4l2

FFMPEG_RKRGA_COMPONENTS:= \
	$(foreach c,$(FFMPEG_RKRGA_FILTERS),--enable-filter=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_DECODERS),--enable-decoder=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_ENCODERS),--enable-encoder=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_DEMUXERS),--enable-demuxer=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_MUXERS),--enable-muxer=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_PARSERS),--enable-parser=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_BSFS),--enable-bsf=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_PROTOCOLS),--enable-protocol=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_INDEVS),--enable-indev=$(c)) \
	$(foreach c,$(FFMPEG_RKRGA_OUTDEVS),--enable-outdev=$(c))

FFMPEG_RKRGA_CONFIGURE:= \
	CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" \
	LDFLAGS="$(TARGET_LDFLAGS)" \
	./configure \
	--enable-cross-compile \
	--cross-prefix="$(TARGET_CROSS)" \
	--arch="$(ARCH)" \
	--cpu=cortex-a53 \
	--target-os=linux \
	--prefix="/usr" \
	--pkg-config="pkg-config" \
	--enable-static \
	--disable-shared \
	--enable-pthreads \
	--enable-zlib \
	--disable-doc \
	--disable-debug \
	--disable-autodetect \
	--disable-lzma \
	--disable-vaapi \
	--disable-vdpau \
	--disable-d3d11va \
	--disable-libx264 \
	--disable-libx265 \
	--disable-libvpx \
	--disable-libaom \
	--disable-libdav1d \
	--disable-libvorbis \
	--disable-libopus \
	--disable-libmp3lame \
	--disable-libfdk-aac \
	--disable-gnutls \
	--disable-openssl \
	--disable-libfreetype \
	--disable-alsa \
	--enable-libdrm \
	--enable-rkmpp \
	--enable-rkrga \
	--enable-gpl \
	--enable-version3 \
	--enable-small \
	--disable-everything \
	--enable-avcodec \
	--enable-avdevice \
	--enable-avfilter \
	--enable-avformat \
	--enable-avutil \
	--enable-swresample \
	--enable-swscale \
	--enable-ffmpeg \
	--disable-ffplay \
	--disable-ffprobe \
	$(FFMPEG_RKRGA_COMPONENTS)

define Build/Configure
	( cd $(PKG_BUILD_DIR); $(FFMPEG_RKRGA_CONFIGURE) )
endef

define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) \
		DESTDIR="$(PKG_INSTALL_DIR)" \
		all install
endef

# Only the CLI program is shipped, renamed to ffmpeg-rkrga.  Nothing else
# is installed into /usr so the system ffmpeg stays untouched.
define Package/ffmpeg-rkrga/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(CP) $(PKG_INSTALL_DIR)/usr/bin/ffmpeg $(1)/usr/bin/ffmpeg-rkrga
endef

$(eval $(call BuildPackage,ffmpeg-rkrga))
