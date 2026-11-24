# tts-rk3566 · 离线中文 TTS（OpenWrt 包）

```
opkg install tts-rk3566_*.ipk
tts "这是中文语音合成的测试" /tmp/out.wav
```

## 装了些什么

```
/opt/tts-rk3566/
  tts_sherpa                    推理程序（aarch64 glibc，14 KB）
  lib/libonnxruntime.so         onnxruntime 1.28
  lib/libsherpa-onnx-c-api.so   sherpa-onnx C API
  lib/libsherpa-onnx-cxx-api.so
  matcha/model_qdq.onnx         声学模型，int8 QDQ 量化，22 MB
  matcha/hifigan_v2.onnx        声码器 fp32，3.6 MB（不要量化，量化后会有电流声）
  matcha/tokens.txt lexicon.txt date.fst number.fst dict/   拼音/分词资源
/usr/bin/tts                    包装脚本（走 /opt/glibc 的 loader）
```

依赖 `rk356x-npu-ocr`，原因是它提供 `/opt/glibc`（musl 系统里跑 glibc 二进制必需）。
TTS 本身不用 NPU。

## 参数与行为

```
tts "文本" [输出] [语速] [码率]
  输出默认 /tmp/tts_out.mp3 —— 后缀 .wav 才出 wav，否则走 ffmpeg 转 mp3
  默认 64 kbps，可传 32k/48k/96k
  采样率 22050 Hz；语速 1.0 正常，>1 变慢，<1 变快（length_scale = 1/speed）
```

板上 10.80.1.125（RK3566，llama-server 常驻 2.5 GB 也在跑）实测 **98 个汉字**：

```
音频长度      19.4 s
合成耗时      18.3 s   （RTF 0.94；llama 忙的时候会到 22 s，RTF 1.13）
模型加载      ~3 s     （每次调用重新加载，进程用完即退）
mp3 编码      ~2 s     （ffmpeg libmp3lame 64k）
合计          ~23 s    正常负载 / 40 s 以上（llama 满载时）

体积对比（同一段 19.4 s）：
  wav           862 KB
  mp3 96k       236 KB
  mp3 64k       157 KB   ← 默认
  mp3 48k       118 KB
  mp3 32k        79 KB   （ASR 回读仍然全对，但开始有金属感）
```

换算：1000 字 ≈ 195 s 音频 ≈ 3.5–4 分钟；批量时长基本线性，只有模型加载那 3 s 是固定成本。


## 换音色 / 换模型

`matcha` 目录里换 `.onnx` 即可，前提是同一套 sherpa-onnx VITS/Matcha 结构：

```
声学模型 -> matcha/model_qdq.onnx
声码器   -> matcha/hifigan_v2.onnx
```

试过的其它中文音色（都没进包，体积或音质不合适）：kokoro-multi-lang-v1_0
（好听、有男声，但 311 MB，int8 141 MB 且卷积量化出高频沙沙）、
vits-zh-hf-eula/theresa（39 MB int8，RTF 7.6 太慢）、
vits-piper-zh_CN-chaowen/xiao_ya（20 MB，机械）、
vits-zh-aishell3（174 音色但只有 8 kHz）。

## 已知限制

- 只认中文（拼音 lexicon + jieba 分词）；英文单词会被跳过。
- 数字靠 number.fst 转写，中文数字读法正常，长串数字建议先转成中文。
- 一次调用进程即退，批量合成时每次要 3 s 加载；需要连续合成就自己写循环，
  或者改成常驻服务（当前没做）。
