# luci-app-diskrestore — 损坏 U 盘 / 移动硬盘 一键文件恢复 (LuCI 页面)

LuCI 页面 + 后台 worker，把 `ddrescue` / `testdisk` / `photorec` 串成一条固定流水线，
点一下就把坏盘里的文件捞出来。**源盘全程只读**，所有写入都落在你自己选的目标目录。

## 四步流水线（固定，不需要选模式）

| 步骤 | 做什么 | 工具 |
|------|--------|------|
| 1/4 镜像 | 整盘只读镜像，坏区自动重试，中断可续 | `ddrescue -f -r3 -d` |
| 2/4 分析 | 读分区表，列出可恢复分区 | `testdisk /list` |
| 3/4 **按原文件名/目录恢复** | 把镜像里的分区**只读挂载**出来整树复制；挂不上先试**备份引导扇区修复** | `losetup -P`+`mount -ro`+`cp -a`+`dd` |
| 3b/4 **原名补漏** | 挂不上的分区 / 被删除的文件交给 testdisk 自己的解析器：`advanced,<N>,list,recursive,filecopy` 整树按原名复制，NTFS 再 `advanced,<N>,undelete,allundelete` | `testdisk /cmd` |
| 4/4 按签名雕取 | 连 testdisk 都解析不了的分区才走签名雕取（名字必丢） | `photorec /cmd ... search` |
| 4b **真名回填** | 雕出来的 `recup.N/f<扇区>.<ext>` 按起始扇区对回 testdisk 日志里的原名；对上的就改成真名（testdisk 簇链坏了、内容烂的，用雕出的那份覆盖，旧的改名 `*.testdisk` 留着） | 纯 shell + `testdisk` 日志 |

## 恢复源有两种：坏盘（设备）或 已经做好的镜像文件

同一份镜像可以**反复用来恢复**，不必每跑一次就重新 dd 一次：

| 源 | 怎么做 | 行为 |
|----|--------|------|
| 磁盘设备 | 点页面上的磁盘卡片 | 跑第 1 步：ddrescue 只读整盘镜像，再走 2/3/4 |
| **已有镜像文件** | 在"或者：用已经做好的镜像文件恢复"里填路径，或点"扫描已有镜像"扫出来的条目 | **跳过第 1 步**，直接 2/3/4（日志写"① 使用已有镜像"） |

细节：
- "扫描已有镜像"扫所有可写挂载点里的 `*.img`（深度≤4、>1MB、最多 40 条，带大小和时间），
  上一次恢复留下的 `<目标>/diskrestore/<设备>/image.img` 会直接出现在列表里，点一下就复用。
- 用镜像作源时**不做"同盘"检查**（镜像只是目标盘上的一个文件，恢复结果写同一个盘是正常用法），
  只要求目标目录还剩 ≥32MB；小于镜像 10% 时页面提示"可能只能恢复一部分"。
- 同一路径的镜像固定映射到同一个工作目录 `diskrestore/img-<文件名>-<路径校验>`，反复恢复的结果
  聚在一起（`by_name/` 覆盖更新，`recup.N` 逐次新增）。
- 镜像里**没有分区表**（单分区/裸文件系统镜像）时，第 3 步把镜像本身当文件系统挂，
  产物落 `by_name/whole/`。

第 3 步是保住文件名/目录的关键：**photorec 按签名雕取，天生丢名字**（输出
`recup.N/f<起始扇区>.<ext>`，它的 report.xml 里也没有原文件名）。

### FAT32 / exFAT 能不能保住名字？能，而且比 NTFS 更省事

| 情况 | 结果 |
|------|------|
| vfat(FAT12/16/32)、exFAT 文件系统完好 | 内核原生挂载 → **文件名 + 目录结构 + 内容全保住**（不需要 ntfs-3g 那层） |
| 只有**引导扇区**被打坏（典型报错 `bad superblock` / `unknown entry type`） | 先自动用**备份引导扇区**修回来再挂：exFAT 备份在**扇区 12**，FAT32/16/12 备份在**扇区 6** → 修好就能挂上，名字照样保住 |
| 引导扇区修了还是挂不上（FAT/目录项也坏了） | 先交给 **3b：testdisk 自己的解析器**（`advanced,<N>,list,recursive,filecopy`）整树按原名复制 —— 它不需要挂载，目录项残缺也能列；连它都认不出这个文件系统，才退回 photorec 雕取（名字会丢），并在结果表里写明原因 |
| 文件是**被删除**的（盘现在挂得上，但要救删掉的东西） | 挂载这条路永远看不见删除项；NTFS 走 **3b 的 `advanced,<N>,undelete,allundelete`** 按原文件名恢复被删除的文件。FAT/exFAT 的删除项首字母已被系统改写成 `0xE5`，只能得到 `_LD.TXT` 这种缺首字母的名字 |
| NTFS | 走 ntfs-3g 用户态挂载（只读），同样保名字；NTFS 的备份引导扇区在分区**最后一扇区**，不在本 app 自动修复的范围 |

修复只写在**镜像副本**上（loop 分区设备），源盘一个字节都不写；修完仍挂不上会自动
**回滚**引导扇区，镜像保持原样。引导扇区这类损坏很常见，实测：把 FAT32 的扇区 0 整段
清零（完全挂不上）→ app 用备份扇区 6 修回来 → `DCIM/photo.jpg`、`MyDocs/notes.txt`
连目录带文件名全恢复，内容与原文件 sha256 一致。

## 依赖（命令 → 包，一个都不能漏）

| 用到的命令 | 由哪个包提供 | 说明 |
|---|---|---|
| `ddrescue` / `ddrescuelog` | `ddrescue` | package/disk_restore/ |
| `testdisk` / `fidentify` | `testdisk` | package/disk_restore/ |
| `photorec` | `photorec` | package/disk_restore/ |
| `blkid`（`-p`/`-s TYPE`） | **`blkid`** | util-linux 拆出来的独立包，很容易漏 |
| `smartctl`（`-H -A -i`） | **`smartmontools`** | 盘健康，漏了页面只是少一栏且 opkg 会告警 |
| `mount.ntfs-3g` | `ntfs-3g` | 由 testdisk 依赖带入 |
| `mount/umount/losetup/dd/base64/cmp/awk/sed/find/du/df/date/tail` | busybox | 基础系统自带，无需声明 |

> `lsblk` / `fdisk` **没有用到**：设备/分区枚举走 `/sys/block` + `blkid`（比解析
> `fdisk -l` 更稳，不依赖输出格式），分区表详情由第 2 步的 `testdisk /list` 输出。
> 所以也没有把它们写进依赖 —— 不用的命令不该有依赖。

## 安装 / 编译

```sh
make package/luci-app-diskrestore/compile -j$(nproc)
# 产物 bin/packages/<arch>/base/luci-app-diskrestore_<ver>_all.ipk
opkg install luci-app-diskrestore_<ver>_all.ipk
# 装完清 LuCI 缓存 (用 mv, 别 rm)
cd /tmp && for f in luci-indexcache* luci-modulecache; do [ -e "$f" ] && mv "$f" "$f.stale.$(date +%s)"; done
/etc/init.d/uhttpd restart
```

## 用

LuCI → 服务 → **磁盘数据恢复**（页面每 3 秒自刷）

1. 插上坏盘，磁盘卡片显示三重信息：分区表/文件系统健康徽章 + **SMART 健康徽章**
   （正常 / 劣化（待映射·重映射扇区）/ 失败 / 不可用 + 温度·通电小时），
   以及每个分区的容量、类型、起始扇区 `@2048`。
2. 点一下盘卡片选中。
3. 目标目录**可以直接手输任意路径**（不存在会自动创建），也可从下拉的已挂载可写目录
   点一下快速填入；实时校验 可写 / 剩余空间 / 是否与源盘同盘（同盘直接拒绝）。
4. 点「开始恢复」。后台 worker 跑，关页面也继续；回来能看到进度、实时日志、
   各分区按原名恢复结果（含是否修复过引导扇区、有多少文件损坏没复制）和雕出的文件清单。

## 关键路径（设备上）

```
/tmp/diskrestore/                  worker 状态 + 日志(≤100 行) + SMART 缓存
/tmp/diskrestore/extra_devs        可选: 额外块设备名(每行一个), 让 loop/dm 也进列表; 默认不存在
<目标>/diskrestore/<dev>/image.img          整盘镜像 (+ image.map 断点续传)
<目标>/diskrestore/<dev>/by_name/partN/     ★ 保住原文件名/目录的文件
<目标>/diskrestore/<dev>/recup.1/           按签名雕出的文件 + report.xml
<目标>/diskrestore/<dev>/byname.json        各分区结果 (页面结果表数据源)
```

## 后端 CLI

```
diskrestore list                       # 磁盘 JSON (含 smart + 分区 start)
diskrestore destinations [dev]         # 可写目标目录 JSON
diskrestore destcheck b64:<b64url>     # 校验手输目录
diskrestore start <dev> <dest>         # dest = 序号 或 b64:<base64url 路径>
diskrestore status | log [n] | result [n] | stop
diskrestore smart_scan <dev>           # 内部: 后台 SMART 扫描
```

## 实现要点 / 坑

- controller `index()` 必须整块 `pcall` + 只用字面量（模块级 local 上值取不到）；
  controller 任何 Lua 语法错误会让**整个站点 500（连登录都 500）**。
- 手输路径：前端 base64url → controller 参数白名单 → 后端解码后校验，杜绝 shell 注入。
- busybox `ls -l` **没有 total 头行**，awk 别用 `NR>1`（会吃掉第一个文件）。
- SMART 必须后台跑（设备上没有 `timeout`，smartctl 在坏盘上会卡）+ 缓存 2 分钟，
  `list` 只读缓存保证秒回；读不到健康值时报"不可用"，**绝不显示"正常"**。
- 引导扇区修复的判据要格式无关（`offset 11 = 00 02` 且 `offset 510 = 55 AA`），
  别用 OEM 名白名单：mformat 写的是 `MTOO4032`，mkdosfs 写 `mkfs.fat`。

## 已实测（Panther X2 / OpenWrt 21.02.5 / aarch64，真 Chrome 走完整页面链路）

测试盘：80MB，分区1 = FAT32（含 `DCIM/photo.jpg` + `MyDocs/notes.txt`，**引导扇区被整段清零**），
分区2 = 无文件系统（里面躺一个 jpg）。

```
分区              文件系统  结果
part1 (loop1p1)   vfat      成功: 2 个文件 / 832 KB
                            （该分区引导扇区已用备份修复后挂载）
                            /tmp/drout4/diskrestore/loop3/by_name/part1
part2 (loop1p2)             未成功: 挂载失败 (文件系统 认不出 已损坏), 改用按签名雕取

按签名雕出的文件: f0002053.jpg 827 KB / report.xml 2 KB
```

- 引导扇区修复：日志 `分区1: 引导扇区损坏, 已用备份(扇区 6)修复镜像副本 (源盘未动), 重试挂载…`
  → `✔ 分区1 (loop1p1, vfat) [引导扇区已修复] 按原文件名恢复: 2 个文件`
- 产物：`by_name/part1/DCIM/photo.jpg`、`by_name/part1/MyDocs/notes.txt` 名字+目录+内容全对
  （sha256 与原图一致，`notes.txt` 内容 "hello recovery test"）
- 源盘未被写：对比源镜像与工作镜像的第 0 扇区 —— 源仍是全零，工作镜像已被修复
- 页面：0 个 JS 错误；SMART 对 loop 设备诚实显示"不可用（设备类型或 USB 桥不支持透传）"

### 用已有镜像作源（v1.4.0 实测）

```
选磁盘卡片 / 填镜像路径 → 目标目录（可手输） → 开始恢复
t= 0s  启动中 | 已派发，正在启动…      ← 点下去立刻清掉上一次的进度/结果
t= 2s  进行中 | 分析分区 (testdisk) | step 2/4   ← 第 1 步"制作镜像"被跳过
t= 8s  完成    | step 4/4 | 100%
按原文件名恢复: 2 个（839 KB） · 按签名雕出: 2 个（834 KB）
log: ▶ 开始恢复: 镜像 /root/images/t_fat32.img (80.0 MB) → …
     [00:00] ① 使用已有镜像，跳过「制作镜像」这一步
```

同一份镜像第二次恢复：10 秒完成、结果聚在同一工作目录、日志里没有 ddrescue（`recup.1`/`recup.2` 累加）。

### 3b 原名补漏：挂不上也能拿真名字（v1.6.8 实测）

造了一块 30MB 的盘镜像（MBR + 手写 FAT16，全程不碰 mkfs/mtools），里面有
`HELLO.TXT` / `PHOTO.JPG` / `DCIM/IMG_0001.JPG`，外加一个**被删除**的目录项
`OLD.TXT`（首字节已被系统改成 `0xE5`，簇链还在）。用同一份 7.2 源码在 x86 上编出
testdisk，把 worker 的整条路径跑了一遍（镜像当源，目标 `/tmp/fsdst2`）：

```
[00:02] ✖ 镜像挂不到 /dev/loop0, 跳过按文件名恢复        ← 3/4 这条路 0 产出
[00:02]    testdisk: part1 (FAT16, blocksize=2048) 整树按原名复制 → by_name/part1
[00:04] ✔ part1 (FAT16, blocksize=2048) testdisk 按原名恢复: 4 个文件 / 40 KB
✔ 恢复完成：按原文件名 4 个（40 KB）+ 按签名雕出 1 个（8 KB）

by_name/part1/
    HELLO.TXT          4800B   与原文件逐字节一致
    PHOTO.JPG         10240B   与原文件逐字节一致
    DCIM/IMG_0001.JPG  4200B   目录结构也保住了
    _LD.TXT            3000B   ★ 被删除的那个文件（原名 OLD.TXT，首字母被抹掉）
```

testdisk 自己的日志 `Copy done! 4 ok, 0 failed`；`byname.json` 里 `dev=testdisk`、`fs=FAT16`。
★ 最后那行是 photorec 永远做不到的，挂载那条路也看不见（删除项对 VFS 不存在）。

设备侧的同一份二进制里这些子命令都在（`strings` 检查过 `advanced` / `recursive` /
`filecopy` / `allundelete`）；上面这次是 x86 同源编译跑的，**设备上还没实测**
（机器当时关机），页面链路等设备起来再走一遍。

### 4b 真名回填：雕出来的编号也能变回真名（v1.6.9 实测）

同一块手造 FAT16 镜像（三个真 JPEG：`OK.JPG` 簇链指向越界簇、`BROKEN.JPG` 簇链被清空、
`DEL.JPG` 是删除项），跑完整 worker：

```
[00:02]    testdisk: part1 (FAT16) 整树按原名复制 → by_name/part1
[00:04] ✔ part1 (FAT16) testdisk 按原名恢复: 3 个文件 / 80 KB    ← 三份内容其实是烂的
[00:06] ✔ 真名回填: 补空 0 个, 覆盖损坏的 3 个（坏的改名为 *.testdisk 留着）, 完全相同 0 个

photorec 雕出:  recup.1/f0000151.jpg  f0000179.jpg  f0000263.jpg
testdisk 日志:  fat_copy OK.JPG first_cluster=2 (151)      ← 扇区号逐个对上
                fat_copy BROKEN.JPG first_cluster=9 (179)
                fat_copy _EL.JPG first_cluster=30 (263)
结果: by_name/part1/OK.JPG / BROKEN.JPG / _EL.JPG 全部与真图逐字节一致
      旧的坏文件: OK.JPG.testdisk / BROKEN.JPG.testdisk / _EL.JPG.testdisk（不删）
      recup.1 里只剩 report.xml
```

对照：把簇链修好（testdisk 能拷对）再跑同一条流水线 →

```
✔ 真名回填: 补空 0 个, 覆盖损坏的 0 个, 与已恢复文件完全相同的 3 个留在 recup 里
recup.1/: f0000151.jpg f0000179.jpg f0000263.jpg   ← 已验证一致, 不白搬一遍数据
```

这就是"testdisk 拿不到、只有 photorec 拿得到"的那类文件：**内容用雕出来的那份，
名字从目录项里对回来**。对不上的（目录项被新数据覆盖）仍然只能是编号——没有名字可取。

中文文件名：实测 FAT16 里 `照片.jpg` + 中文目录 `我的文档/说明.txt` 按原名逐个落盘、内容一致，
回填也用中文名对上（`f0000151.jpg` → `照片.jpg`）。不用装 locale：FAT 读的是 LFN 长名，
NTFS/exFAT 走 `iconv("UTF-8","UTF-16LE")`，ext2/3/4 名字本身就是 UTF-8 字节。

## 文件很多时（几千 / 几万 / 几十万）也不崩

恢复大容量盘会产出成千上万个文件，这一版的硬约束是"**任何被页面轮询的接口都必须是 O(1) 或 O(前 N 个)**"：

| 地方 | 以前（会崩） | 现在 |
|------|--------------|------|
| `result` 取结果 | 每次轮询都 `find`+`du` 数**全部**文件（3 万文件 ≈ 3.5 秒/次，每 3 秒一次；USB 盘上几万个文件就是几十秒 → CGI 超时、页面卡死） | 计数直接读 worker 写好的 state（0.10 秒，且与文件数无关）；文件清单用 `find … | head -n 200` 只取前 200 个 |
| 结果表格 | 一次渲染上万行 DOM | 只渲染前 200 行，末行提示"只列出前 200 个（共 N 个）" |
| 超出统计上限 | 无上限，几十万个文件时统计要几分钟 | 计数上限 2 万（`find | head -n 20001`），超出显示 `20000+`；上限机制自证：2.5 万文件 → 截到 20001 |
| 看全部文件 | 没有别的办法，只能让浏览器硬扛 | 结果区给两个**纯文本清单**下载链接（`filelist?w=byname` / `?w=recup`，上限 10 万行） |
| 按原名复制的进度 | 按"顶层项"算，一个大目录里几十万文件时进度看着不动 | 源侧先数文件数（封顶 2 万），进度 = 已复制/总数，日志每 15 秒报"已复制 N/M 个文件（P%）" |

实测（3 万个文件的工作目录）：

```
旧版每次轮询要做的三件事    新版 result
find|wc -l   0.61s          0.10s（恒定，不随文件数增长）
du -sk       0.54s
ls -l|awk    2.40s   ← 光这一步就把页面拖死
合计 ≈ 3.55s / 每 3 秒一次

页面表现: 结果表 202 行（表头+200 个文件+末行提示）、DOM 总行数 204、
          汇总"按原文件名 20000+ 个 · 按签名雕出 20000+ 个"、JS 异常 0
清单下载: 30002 行 / 2.7 秒
```

## 「整理文件」按钮：雕出来的文件按 年/月 归档

只在**文件系统挂不上、只能靠 photorec 雕取**时才需要：雕出来的文件名是 `f<起始扇区>.<ext>`，
全平铺在一个 `recup.N/` 里，没有目录结构。结果区会出现按钮：

    整理雕出的文件（按年/月归档）

点一下（后台任务，带进度）把雕出的文件按**文件日期**分到两级目录：

```
<目标>/diskrestore/<源>/organized/
    2023/05/
    2024/11/  2024/12/
    2025/03/  2025/07/
    2026/01/  2026/10/
```

细节：
- **用文件日期（mtime）分**，按设备本地时区（`date +%z`）。mv 会保留 mtime，
  所以归档后每个文件的修改时间还是原来的。
- **是移动不是复制**：同分区 `mv` 瞬时完成、不额外占空间，原 `recup.N/` 会空掉；
  photorec 自己的报表 `report.xml` 不参与整理，留在原处。
- 几十万个文件也不会卡：文件日期用 `xargs stat -c '%Y %n'` **一次批量**取（逐个 `date -r`
  会为每个文件起一个进程），移动用 `xargs` **每批 100 个一次 `mv`**；进度按"已归档/总数"报。
- 整理是**后台任务**：页面显示"整理中 N%（已归档 x/y 个）"，结束时显示
  "已整理 N 个文件 → <目录>"；已整理过再点会提示"已经整理过了"；
  **重新跑一次恢复会重置整理状态**，新雕出的文件可以再整理（并进同一棵年/月 树）。
- ⚠️ photorec **雕取**时通常保不住文件原来的日期（写的是雕刻那一刻的时间），所以按日期归档的
  效果取决于文件里带着什么时间戳。真按照片拍摄日期归档需要读 EXIF 的 `DateTimeOriginal`
  —— OpenWrt 上没有 exiftool，这一版没做（要做就只读 JPEG 头几 KB 找 `YYYY:MM:DD` 那个
  ASCII 字段，几十万个文件同样要走批量方案）。

## 依赖审计：别假设"基本系统都有"

OpenWrt 上**只有 busybox**。这几样我一开始以为是基本命令，其实都不是（实测审计出来的）：

| 命令 | 真实来源 | 替代做法（本项目已改） |
|------|----------|------------------------|
| `stat` | **coreutils 包的 coreutils-stat**（busybox 里根本没有 stat applet） | 声明依赖 `+coreutils-stat`；同时代码里有退路：没有 stat 时用 `ls -l` 的日期列（只精确到年/月） |
| `cmp` | **diffutils 包**（GNU cmp） | 纯 shell 比字节：跟 NUL 比用 `tr -d '\000' \| wc -c`，跟普通字节比用命令替换 + `printf '\125'` |
| `base64` | **coreutils-base64** | 前端改传**十六进制**，后端用 awk 一行解码（见下） |
| `cksum` | **根本没有**（busybox 未编该 applet） | 用 `sha256sum \| cut -c1-8` |

审计方法（照着跑一遍就知道自己漏了什么）：

```sh
for c in $(脚本里用到的所有命令); do
  p=$(command -v $c) || { echo "$c 缺失"; continue; }
  case "$(readlink -f $p)" in *busybox*) ;; *) echo "$c 非busybox: $p" ;; esac
done
```

### 为什么把 base64 换成了 hex

手输路径必须**绝不喂给 shell**，所以前端做一次编码、后端解码：

- base64 需要 `base64 -d`（coreutils-base64 包，不是自带的）⇒ 换成十六进制；
- 后端解码只用一个 awk（awk 一定在），而且天然支持 UTF-8 中文路径：

```sh
hexpath() {   # hex:<十六进制> → 路径
	printf '%s' "${1#hex:}" | awk '
		function hv(c) { return index("0123456789abcdef", tolower(c)) - 1 }
		{ n = length($0); for (i = 1; i + 1 <= n; i += 2) printf "%c", hv(substr($0, i, 1)) * 16 + hv(substr($0, i + 1, 1)) }'
}
```

前端用 `TextEncoder` 取 UTF-8 字节再转 hex（中文路径也对）。控制器侧的白名单也因此更简单更严：
`^[0-9a-fA-F]+$`。

## 路径里有空格 / 中文时踩的三个坑（都已修）

用户的目标目录完全可能叫 `/mnt/我的盘/恢复 输出` 这种。实测踩到：

1. **busybox `df` 处理不了含空格的路径** —— `/proc/mounts` 里空格是 `\040`，`df -k "<真实空格路径>"`
   会报 `No such file or directory` 且**只输出表头**，于是 `$(( ))` 拿到空值。
   修法：新增 `df_bytes()`，先按 `/proc/mounts`（还原 `\040`）找到挂载点，再 `df -k <设备>`。
2. **`$(( $(cmd) * 1024 ))` 会被空值炸掉脚本**：ash 里空值算数直接
   `arithmetic syntax error` 并**退出整个 worker**（实测就是这个把我坑了：worker 静默 exit 2）。
   修法：`num()` 兜底，所有"命令替换进算数"的地方一律过一遍；df/du/cat 取值也全走它。
3. **`xargs` 默认按空白切分参数** —— 带空格的路径会被切成两半（`ls: /overlay/恢复: No such file`）。
   修法：所有 xargs 管线改 NUL 分隔：`find -print0` + `xargs -0`、或者 `tr '\n' '\0' | xargs -0`；
   文件名也改成"去掉行首大小字段后的整行"，这样带空格的名字不会被切掉。
4. **`/tmp`（tmpfs）这类非块设备的目标，剩余空间被显示成 `0 KB`** —— 修第 1 个坑时我改成"按
   /proc/mounts 找设备再 df"，但 tmpfs / overlay / none 这些**第 1 列不是块设备**，
   `df -k tmpfs` 无效 ⇒ 空值 ⇒ `num()` 兜成 0 ⇒ 页面显示"剩余空间: 0 KB"，
   更糟的是 32MB 门槛会把 /tmp 直接判成"空间不足"、根本不让开始。
   修法：`df_bytes()` 三级兜底 —— ①路径本身不含空格就直接 `df -k <路径>`（最准）；
   ②按 /proc/mounts 找最长前缀挂载点，用它的**转义写法**（`\040`）问 df；
   ③最后才按 `/dev/*` 设备问。实测 `/tmp` → 1.6 GB（与 `df` 第 4 列一致）、
   `/overlay` → 448.1 MB、`/overlay/恢复 输出` 这类带空格路径也对。
   ✅ 教训：**"设备"和"挂载点"不是一回事，tmpfs/overlay 没有设备**；页面上出现的每个数字都要
   拿真盘、真路径、真文件系统类型验一遍（我这次只验了 overlay，tmpfs 就漏了）。

## 实现上踩过的坑（都是实测踩出来的）

- **busybox `find -size` 只认 `k` 后缀**：`-size +1M` 报 `invalid number '1M'`，叠加
  `2>/dev/null` 就静默变成"扫不到任何镜像"。用 `-size +1024k`。
- **busybox `sleep` 不接受小数**：`sleep 0.5` 报 `invalid number`，写在轮询里会让循环空转并误判。
- **overlay 上扫出来的是内部路径** `/overlay/upper/root/…`，要去掉 `<挂载点>/upper` 换成可见路径。
- **别对整份镜像算校验和当标签**：62G 镜像 cksum 要好几分钟；标签用**路径**算（瞬时），
  顺便让"同一路径 = 同一工作目录"。
- **改共享状态文件要"事后复核再落盘"**：判断"是 running"和 sed 落盘之间有窗口，worker 可能刚好
  写完 done，直接 mv 会把过期错误文本注进正常状态（实测出现"done 却带着进程已退出"）。
  落盘前 grep 复核确实翻成了 error 再 mv。
- **假 running 会挡住后续任务**：设备重启/进程被杀留下 `state=running` 时 `start` 会拒绝；
  `status` 每次轮询都自愈（进程不在 → 落 error 并说明原因），`stop` 强杀后也会自己落 `stopped`。
- **`start` 返回前不能读旧状态**：worker 还没写状态时 `$STATE` 是上一次的 done，会把人家的成功
  报成失败。先删旧状态再等新状态（最多 3 秒，等不到照实回"已派发"）。
- **点下按钮立刻清面板**：`start` 请求要 1~3 秒，不能等它回来才清，否则那几秒页面还在拿旧值当新值。
