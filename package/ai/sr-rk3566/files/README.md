# 超分（当前交付状态）

## 支持的两档分辨率

| 片源 | 模型 | 模型输入 | 切块 | 输出 | 速度 |
|---|---|---|---|---|---|
| 576×432（03.mp4 那种 4:3） | `av3_res.rknn` | 288×216 | 2×2 | 1152×864 | 5.5 秒/帧 |
| 640×360（06.mp4 那种 16:9） | `av3_320x180.rknn` | 320×180 | 2×2 | 1280×720 | 5.1 秒/帧 |

两档都是：源片**原生裁剪**成 4 块（不缩放），每块 4 倍放大，拼起来再缩 2 倍。

## 用法（板子上，文件都在 /opt/a4k/）

### 576×432 源

```sh
cd /opt/a4k
ffmpeg-rkrga -y -v error -i in.mp4 -f rawvideo -pix_fmt rgb24 in.rgb
python3 sr_tile2.py in.rgb out.rgb 576 432
ffmpeg-rkrga -y -v error -f rawvideo -pix_fmt rgb24 -s 1152x864 -r 25 -i out.rgb \
    -i in.mp4 -map 0:v:0 -map 1:a? -aspect 4:3 \
    -c:v h264_rkmpp -pix_fmt nv12 -b:v 20M -c:a copy -shortest out_2x.mp4
```

### 640×360 源

```sh
cd /opt/a4k
ffmpeg-rkrga -y -v error -i in.mp4 -f rawvideo -pix_fmt rgb24 in.rgb
python3 sr_tile3.py in.rgb out.rgb 640 360
ffmpeg-rkrga -y -v error -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i out.rgb \
    -i in.mp4 -map 0:v:0 -map 1:a? -aspect 16:9 \
    -c:v h264_rkmpp -pix_fmt nv12 -b:v 20M -c:a copy -shortest out_2x.mp4
```

`-aspect` 必须写，否则 SAR=N/A，电视盒子会按 16:9 硬拉变形。
`-r` 用源片的帧率（03 是 25，06 是 30）。

## 文件

```
av3_res.rknn          1.6 MB   576×432 源用（输入 288×216）
av3_320x180.rknn      1.7 MB   640×360 源用（输入 320×180）
sr_tile2.py                    对应 288×216 的切块脚本
sr_tile3.py                    对应 320×180 的切块脚本
av3_res.onnx / av3_320x180.onnx  转换源
resr_animevideov3.pth            官方权重
export_av3_res.py                权重 → ONNX（含残差、1/255）
conv_anime4k.py                  通用 ONNX → RKNN
anime4k_rknn.c / 二进制          aarch64 运行器
anime4k_m.rknn / animevideov3.rknn  早期的对比模型（Anime4K / 半分辨率版）
```

## 实测数据

```
06.mp4 640×360 450 帧 15 秒：解码 2s + 超分 2303s（5.1 秒/帧）+ 编码 14s = 38.6 分钟
03.mp4 576×432 250 帧 10 秒：超分 1380s（5.5 秒/帧）+ 编码 8s = 23 分钟
```

## 已知限制

- **源片太糊时效果有限**：模型只会在已有结构上收紧边缘，不会凭空生成细节。
  实测 06.mp4（mpeg4 老片）能明显改善线条，但如果源片糊到结构都看不出，就没有改善空间。
- **大模型也不行**：anime6B（7 倍参数）在这块板上是 220 秒/帧，一集 87 天，不可用。
- **分辨率固定**：RKNN 的输入尺寸在转换时写死（`dynamic_input` 在 RK3566 不可用），
  换分辨率必须重新转换。
- **转换很慢**：这台机器没 AVX，rknn-toolkit2 要跑在 qemu 里，
  转换耗时 ≈ 算力 × 0.65 分钟/GFLOP（77 GFLOP 约 50 分钟）。有 AVX 的机器快 20 倍。

## 踩过的坑

1. **权重是残差式** —— 输出要加上输入的最近邻放大，否则一片黑（均值 0.009）
2. **fp16 RKNN 不套 mean/std** —— `1/255` 必须写进 ONNX 图
3. **写进图的 1/255 会被折进卷积** —— 残差分支要单独走一条 1×1 卷积 + DepthToSpace
4. **ONNX 的 Resize 会让 rknn-toolkit2 的 fold_constant 死循环** —— 用 DepthToSpace 代替
5. **输出必须写 `-aspect`** —— 否则 SAR=N/A，盒子按 16:9 拉伸
6. **不要缩小画面** —— 切块是原生裁剪，缩放会永久丢细节
