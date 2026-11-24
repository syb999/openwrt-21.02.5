#!/bin/sh
# 中文 TTS（matcha-zh-baker 22.05 kHz，CPU / sherpa-onnx C API）
#   tts "要念的中文" [/tmp/out.mp3] [语速] [码率]
#   输出后缀 .wav 就出 wav，其它（默认 mp3）就走 ffmpeg 转 mp3，临时 wav 自动删
# 二进制是 glibc 的，系统是 musl，所以走 /opt/glibc 的 loader（rk356x-npu-ocr 提供）
set -e
DIR=/opt/tts-rk3566
TEXT=${1:?"tts \"要念的中文\" [out.mp3] [speed] [bitrate]"}
OUT=${2:-/tmp/tts_out.mp3}
SPD=${3:-1.0}
BR=${4:-64k}

case "$OUT" in
*.wav)
	exec env TTS_KIND=matcha TTS_QDQ=1 TTS_VOCFP32=1 \
		LD_LIBRARY_PATH=/opt/glibc:/opt/rknpu:$DIR/lib:$DIR/matcha \
		/opt/glibc/ld-linux-aarch64.so.1 $DIR/tts_sherpa $DIR/matcha "$TEXT" "$OUT" "$SPD" 0
	;;
esac

TMP=$(mktemp -p /tmp tts-XXXXXX).wav   # BusyBox mktemp 只认末尾 XXXX
trap 'rm -f "$TMP"' EXIT INT TERM
env TTS_KIND=matcha TTS_QDQ=1 TTS_VOCFP32=1 \
	LD_LIBRARY_PATH=/opt/glibc:/opt/rknpu:$DIR/lib:$DIR/matcha \
	/opt/glibc/ld-linux-aarch64.so.1 $DIR/tts_sherpa $DIR/matcha "$TEXT" "$TMP" "$SPD" 0
ffmpeg -hide_banner -loglevel error -y -i "$TMP" -codec:a libmp3lame -b:a "$BR" "$OUT"
echo "-> $OUT ($(wc -c <"$OUT") bytes, $BR)"
