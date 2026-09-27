#
# rk356x-npu-ocr - Rockchip RK3566/RK3568 NPU PP-OCR engine for musl OpenWrt.
#
# Supported SoCs: RK3566 / RK3568 (RK356x family, RKNPU1).
# NOT supported: RK3588 / RK3576 (different NPU generation, needs different
# models and a different librknnrt build).  See usr/share/doc/rk356x-npu-ocr/README.
#
# The board userland is musl, but the RKNN user-space runtime (librknnrt.so)
# and the PP-OCR inference binaries are glibc ELF objects.  They cannot run on
# musl directly, so this package bundles a small glibc runtime environment
# under /opt/glibc and always launches the engine through its loader:
#
#   /opt/glibc/ld-linux-aarch64.so.1 \
#       --library-path /opt/glibc:/opt/rknpu <engine> ...
#
# Layout installed by this package (kept identical to the hand-deployed paths
# so existing callers keep working):
#   /opt/ppocr/            engine binary, models, dictionary, test image
#   /opt/rknpu/            librknnrt.so
#   /opt/glibc/            glibc loader + its shared libraries
#   /usr/bin/ppocr         wrapper that applies the loader invocation above
#   /usr/bin/rk356x-npu-ocr  alias for the wrapper
#
# ---------------------------------------------------------------------------
# PREBUILT BINARIES CARRIED IN ./files (NOT built from source by this package)
# ---------------------------------------------------------------------------
#   files/opt/rknpu/librknnrt.so
#       Rockchip RKNN runtime 2.3.2 (429f97ae6b@2025-04-09), taken verbatim from
#       rknn-toolkit2 / rknn_model_zoo 3rdparty/rknpu2/Linux/aarch64/.
#       Rockchip proprietary user-space blob: freely redistributable, not open
#       source.  It is the only implementation of the RKNPU2 user ABI, so it
#       cannot be replaced by anything built here.  License: Rockchip RKNN
#       runtime license (see the rknn-toolkit2 repository).
#
#   files/opt/glibc/*
#       Minimal glibc runtime bundle: glibc 2.35 from Ubuntu 2.35-0ubuntu3
#       (ld-linux-aarch64.so.1, libc, libm, libpthread, libdl, librt, libresolv,
#       libgcc_s, libstdc++ 6.0.30).  Licenses: LGPL-2.1-or-later (libc and
#       friends), GPL-2.0-or-later (ld.so), GPL-3.0-with-GCC-exception
#       (libgcc_s / libstdc++).  Bundled so the glibc engine can start on a
#       musl system; nothing here is compiled by this package.
#
#   files/opt/ppocr/models/ppocrv4_det.rknn, ppocrv4_rec.rknn
#       PP-OCRv4 detection / recognition NPU models, converted from the
#       Apache-2.0 PaddleOCR PP-OCRv4 models with rknn-toolkit2.  Prebuilt
#       (.rknn) blobs, not compiled here.  These are RK3566/RK3568 (RKNPU1-
#       compatible) models and will NOT load on RK3588/RK3576.
#
#   files/opt/ppocr/ppocr_demo
#       ELF built from the Apache-2.0 rknn_model_zoo PPOCR-System example
#       (examples/PPOCR/PPOCR-System/cpp).  The OpenWrt musl toolchain cannot
#       produce a glibc binary and the zoo tree is ~900 MB (it pulls in a
#       prebuilt aarch64 OpenCV), so the already verified build is carried as a
#       file rather than recompiled inside the OpenWrt build system.  Rebuild
#       command (external glibc toolchain, run in the zoo root):
#         cmake -S examples/PPOCR/PPOCR-System/cpp -B build -DTARGET_SOC=rk356x \
#           -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
#           -DCMAKE_BUILD_TYPE=Release -DDISABLE_RGA=ON -DDISABLE_LIBJPEG=ON \
#           -DCMAKE_C_COMPILER=/usr/bin/aarch64-linux-gnu-gcc-11 \
#           -DCMAKE_CXX_COMPILER=<shim g++> \
#           -DOpenCV_DIR=<zoo>/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV
#         cmake --build build -- -j1      # produces rknn_ppocr_system_demo
#
#   files/opt/ppocr/keys/ppocr_keys_v1.txt and files/opt/ppocr/dict/ppocr_keys_v1.txt
#       PP-OCR recognition dictionary (Apache-2.0, from PaddleOCR).
#
#   files/opt/ppocr/test.jpg
#       Small sample image shipped for a smoke test.
#
# Nothing in this package is compiled by the OpenWrt build system: Build/Prepare
# and Build/Compile are intentionally empty.  DEPENDS is empty on purpose - the
# glibc runtime travels with the package, so a musl system needs no libc or
# libstdcpp dependency.
