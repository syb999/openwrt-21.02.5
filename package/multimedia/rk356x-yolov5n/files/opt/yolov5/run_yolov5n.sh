#!/bin/sh
# Run the YOLOv5n RKNN demo on RK3566 (Panther X2).
# The binary is a glibc aarch64 build, so it is launched through the bundled
# glibc loader with /opt/glibc and the RKNPU runtime on the library path.
#
# Usage: /opt/yolov5/run_yolov5n.sh [image_path] [loop_count]
#   image_path  default /opt/yolov5/model/bus.jpg
#   loop_count  default 1 (use e.g. 100 for an FPS benchmark)
#
# The demo reads ./model/coco_80_labels_list.txt, so we cd into /opt/yolov5.

IMG="${1:-/opt/yolov5/model/bus.jpg}"
N="${2:-1}"

case "$IMG" in
	/*) ;;
	*) IMG="$(pwd)/$IMG" ;;
esac

cd /opt/yolov5 || exit 1
exec /opt/glibc/ld-linux-aarch64.so.1 --library-path /opt/glibc:/opt/rknpu \
    ./rknn_yolov5_demo model/yolov5n_rk3566_i8.rknn "$IMG" "$N"
