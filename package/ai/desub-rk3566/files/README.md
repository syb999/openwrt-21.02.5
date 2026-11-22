# desub-rk3566 · JP TV Box 3566 NPU 视频去字幕

RK3566（0.8 TOPS NPU）上跑 MI-GAN 图像修复，把视频里的烧录字幕去掉。
全部在板子上离线处理，不依赖云端。

## 目录

```
code/
  desub-rk3566.py   主流水线：解码 → 掩码 → NPU 修补 → 贴回 → 编码
  migan_rknn.c      推理运行器 C 源码（三种模式：单次 / loop / perf）
  graph_opt.py      ONNX 图优化：删 clip(±256)、把 √2 折进卷积
  convert_migan.py  ONNX → RKNN 转换（fp16，含校准集生成）
  one.py            单帧调试，10 秒一次，改参数试画质用
  flicker.py        量化"边框抖动"：字幕带内连续帧的均值跳变
  scan.py           全片扫描，列出跳变最大的时刻
  bench.py          分步耗时（定位 Python 侧瓶颈）
  cmp_models.py     两个 RKNN 模型逐像素对比（验证图优化没改画质）
bin/
  migan_rknn        aarch64 运行器（glibc 动态，需 /opt/glibc 的 loader）
models/
  migan_noclip.rknn 生产模型（删掉 65 个空操作 clip，快 26%）
  migan_fp16_orig.rknn  原始 fp16 模型（对比用）
onnx/
  migan_orig.onnx / migan_noclip.onnx   转换源（26MB 各一个）
firmware/
  rk3566-jp-tvbox.dts     设备树（NPU 不开 IOMMU）
  mmc.bootscript          rk3566/rk3568 cma=512M
  config-5.4              内核配置
  thp_service/            /etc/config/thp + /etc/init.d/thp
  rockchip-target.diff    target/linux/rockchip 的全部改动
```

## 板子上的部署

文件放在 `/opt/desub-rk3566/`（overlay，刷机不保留配置会丢）。

```sh
python3 /opt/desub-rk3566/desub-rk3566.py <输入.mp4> <输出.mp4> \
    --model /opt/desub-rk3566/migan_noclip.rknn \
    --bin /opt/desub-rk3566/migan_rknn
```

**音轨**：视频是从处理后的裸帧重新编码的（h264_rkmpp），音频用
`-map 1:a? -c:a copy` 从原片**直接流复制**，不重新编码 —— 实测处理前后的
采样数、峰值、RMS 完全一致。原片没有音轨时也不报错。

**只处理前 N 帧时**（`--frames`）会自动加 `-t`，把音频截到与视频等长。

可选参数（默认值是验证过的画质配置）：

| 参数 | 默认 | 含义 |
|---|---|---|
| `--white` | 45 | 局部对比度阈值（top-hat），不是绝对亮度 |
| `--band` | 0.78 | 只处理底部 22% |
| `--dilate` | 10 | 文字笔划膨胀（512 空间） |
| `--margin` | 8 | 再外扩一圈，吞掉编码振铃（mosquito noise） |
| `--flat` | 13 | 纯色底判定阈值（鲁棒过滤后的环带 std） |
| `--feather` | 6 | 边界羽化宽度 |
| `--noise` | 关 | 纯色填充时加匹配噪声（默认不加更干净） |
| `--absorb` | 关 | 额外的残余吸收（慢，通常不需要） |
| `--hwdec` | 关 | rkmpp 硬解（慢消费会丢帧，慎用） |
| `--frames N` | 全部 | 只处理前 N 帧（调试用） |

## 依赖（板子上）

```
/opt/rknpu/librknnrt.so 2.3.2     RKNN 运行时
/opt/glibc/                       glibc loader + 库（运行器是 glibc 动态链接，系统是 musl）
/usr/bin/ffmpeg-rkrga             硬件加速 ffmpeg（h264_rkmpp 编码）
python3 + numpy                   musl 版
```

## 固件前提

1. **不用 IOMMU**。本内核树没有厂商 `rknpu_iommu.c`，硬开 `rknpu_mmu` 会导致
   NPU 作业提交后无人应答（`job timeout` + `soft reset`）。DTS 里删掉 `iommus` 即可。
2. **`cma=512M`**（`mmc.bootscript` 的 rk3566/rk3568 分支）。
3. **THP = never**（`/etc/config/thp` + `/etc/init.d/thp`）。5.4 arm64 不支持大页迁移，
   llama 这类进程的 2MB 大页会把 CMA 池钉住，NPU 要连续内存时 `cma_alloc -EBUSY`。
   关掉 THP 后实测占满 4GB 匿名内存，`CmaFree` 不掉、NPU 照常 1 秒完成。

## 性能（250 帧 576×432）

```
总耗时        216 s（最初 738 s，优化 3.4 倍）
  NPU 推理   139 帧（其余帧靠"逐句复用"省掉推理）
  逐句复用   41 帧
```

| 优化 | 手段 | 效果 |
|---|---|---|
| 删 65 个 `clip(±256)` | 逐层耗时显示它占 274ms，实测是空操作 | 1.55s → 1.15s/帧 |
| 常驻推理进程 | 模型只加载一次，逐帧走管道 | 省 0.3s/帧 |
| 流水线重叠 | 后台线程等 NPU，主线程准备下一帧 | 再省 ~0.4s/帧 |
| 逐句复用 | 文字/背景没变就贴上一帧的补丁 | 再省 41 帧推理 |
| 掩码半分辨率 | box_blur 是 O(N)，256² 快 4 倍 | 216ms → 90ms |
| 背景估计 1/4 分辨率 | 背景是低频的 | 快 16 倍 |
| 羽化只算包围盒 | 全图算 422ms | → 120ms |
| resize 索引缓存 + 可分离 | 4 次全图 gather | 253ms → 70ms |

逐层耗时（perf 模式）：Conv+ConvLeaky 613ms(55%) / Add 249ms / Mul 91ms /
LeakyRelu 72ms / ConvTranspose 56ms。**模型计算量 14.8 GFLOP/帧**，
1.15s 对应约 13 GOPS —— 对 0.8 TOPS 的 RKNPU1 属于合理水平（所有算子都在 NPU 上，
`RKNN_QUERY_PERF_DETAIL` 显示 CPUTime=0）。

## 两个提速开关

```sh
--no-thread     关掉流水线重叠（同步跑，调试用）
--no-reuse      关掉逐句复用
--reuse-bg 6    复用背景差阈值（调大=复用更狠、快但可能与新场景不符）
--reuse-mask 0.03  复用掩码变化阈值
```

纯色/纹理模式的切换带 3 个色阶迟滞（`tstate['mode']`），
否则 ring_std 在阈值附近抖动时会在两条填充路径间来回切，成片里表现为"突然亮一下"。

**提速历史**：2.95 s/帧 → 0.86 s/帧
- 删 65 个 `clip(±256)`（实测是空操作）：1.55s → 1.15s
- 常驻推理进程（模型只加载一次）：省 0.3s/帧
- 掩码检测降到半分辨率、背景估计降到 1/4、羽化只算包围盒、resize 缓存索引

## 关键坑（都踩过）

1. **RKNN fp16 不套 mean/std** → 归一化必须自己做（运行器里按 IOPaint 约定）
2. **RKNN 转换把 mask 极性反了** → ch0 在洞里要给 +0.5（和 torch 相反）
3. **h264_rkmpp 慢喂会丢帧** → 先写裸 rgb 文件再一次性编码
4. **rkmpp 硬解在慢消费下约 110 帧就断** → 先整段快解成裸文件再逐帧处理
5. **运行器是 glibc** → 必须 `/opt/glibc/ld-linux-aarch64.so.1` 启动
6. **numpy uint16*299 溢出** → 亮度计算用 uint32
7. **绝对亮度阈值选不对** → 改局部对比度（top-hat），否则把亮场景元素也补掉
8. **纯色底用全局平面拟合会有 5~6 色阶偏差** → 改局部背景估计（归一化卷积）
9. **边框外一圈颗粒 = 编码振铃** → 掩码外扩 margin，不是填充问题
10. **掩码逐帧伸缩会造成"边框抖动"** → 迟滞 + 相邻帧掩码并集

## 验证方法（不靠眼睛）

```sh
# 单帧快速迭代
python3 one.py /opt/mp4/03.mp4 5.4 /tmp/out.png

# 抖动量化（处理后应 ≤ 原始素材自身抖动）
python3 flicker.py /opt/desub-rk3566/03_desub.mp4 5.4 10

# 全片跳变扫描
python3 scan.py /opt/desub-rk3566/03_desub.mp4

# 逐层耗时
LD_LIBRARY_PATH=/opt/glibc:/opt/rknpu /opt/glibc/ld-linux-aarch64.so.1 \
  /opt/desub-rk3566/migan_rknn model.rknn img.rgb mask.gray out.rgb inv perf

# 模型对比
python3 cmp_models.py old.rknn new.rknn 2.2 4.0 5.4 7.0
```

## 重新转换模型

```sh
# 1. 图优化（可选）
python3 code/graph_opt.py noclip migan_orig.onnx migan_noclip.onnx

# 2. 转换（本机 CPU 无 AVX，必须走 qemu，否则 rknn-toolkit2 SIGILL）
MODEL=/tmp/migan_noclip.onnx OUT_RKNN=/tmp/migan_noclip.rknn \
  /tmp/qemu-x86_64-static -L / /usr/bin/python3.10 code/convert_migan.py --fp16
```
