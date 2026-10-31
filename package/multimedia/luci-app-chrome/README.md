# luci-app-chrome

LuCI 前端 + `/usr/bin/chromeui` 后端：在设备上用 **headless Chrome** 抓网页，并把页面里的
**多媒体资源**（视频/音频/HLS(m3u8)/DASH/图片/字幕）提取出来。

依赖同目录下的三个包：`chrome-glibc-runtime`、`chrome-headless-shell`、`chromedriver`
（chrome 154.0.8037.57 住在 glibc chroot `/opt/chrome-root`），以及 `python3-selenium`。

## 界面

菜单：**Web Tools → Headless Chrome**

1. **组件状态** — 三个 chrome 包是否装好/版本、chroot 绑定挂载（dev/proc/sys/tmp）、
   磁盘占用、chromedriver 9515 端口、selenium 版本；「组件自检」会真的执行
   `run-chrome.sh --version` 和 `cdrv.sh --version`，打印真实版本号（不是读元数据）。
2. **抓取网页** — 填 URL（http/https，不写协议按 http 处理），可选：等待 CSS 选择器、
   超时、窗口宽高、整页截图、滚动加载懒内容、不等子资源、抓 HTML 源码。
3. **截图** — 页内直接显示（也把 PNG 存在设备上，路径会打印出来）。
4. **可见文本** — 渲染后的可见文本，可一键复制。
5. **多媒体资源** — 表格 + 筛选（默认只显示音视频/流，可切图片/全部/字幕）：
   - **解析此资源**：`mediaprobe` 取响应头；若目标是 m3u8 就把播放列表拉下来解析出
     多码率变体(带宽/分辨率)、分片数、总时长、是否直播、是否加密、前几个分片 URL。
   - **复制 URL**：复制该资源地址。
   - 「复制全部 URL」／「导出 .m3u」：把当前抓取到的流写成一个可播放的 m3u 播放列表文件。
   - 🔴 **本 app 只解析 URL，不会下载任何资源**（图片/视频都不落盘）。命令行里有
     `chromeui fetch` 可显式下载，但网页界面上没有这个入口。
6. **HTML 源码** — 抓取时勾了「HTML 源码」才有，可一键复制。

## 资源是怎么找出来的（三路合并，缺一路就会有漏）

| 来源 | 手段 | 能抓到 |
|------|------|--------|
| network | DevTools performance log（`goog:loggingPrefs`） | **每一个** Chrome 发出的响应，包括 XHR 拉的 m3u8/mp4、**跨域 iframe 里的媒体**（DOM 读不到的） |
| dom | `querySelectorAll('video,audio,source,img,iframe,...')`（含同源 frame） | 元素标签、`currentSrc`（lazy-load 后的真实地址） |
| html | 渲染后 HTML 里的裸 URL | 只写在 script 文本里、从未被请求的资源 |

三路按 **百分号解码后的 URL** 去重（DOM 给的 `afx 2.mp4` 和网络层的 `afx%202.mp4` 是同一个），
合并后按 流→图片 排序。

## 🔴 抓到的 URL 打不开？先看这几条（实测）

### 1. 视频站给的是"分片轨"，而且编码可能是你播放器不认的

bilibili 这种站用的是 DASH：**画面和声音是两个文件**。实测抓到的一条视频：

    42209837464-1-30216.m4s   2.3 MB   fourCC: mp4a   ← 这是**音频**（AAC），不是视频！
    42209837464-1-100022.m4s  7.0 MB   fourCC: av01   ← AV1 视频，多数播放器解不了
    42209837464-1-100023.m4s  9.8 MB   fourCC: av01   ← 同上

所以"只有那个音频能用、视频都打不开"是**两个原因叠在一起**：
网络层把音频轨也标成 `video/mp4`（名字还叫 `.m4s`），而且视频轨是 **AV1**。

**修法一：嗅探真实编码。** 抓取时对每条流做一次 128KB 的 Range GET（**只读进内存，不落盘**），
按 fMP4 里的 fourCC 判定真实类型和编码，于是列表会正确显示
`视频文件 … H.264 (avc1)` / `音频文件 … AAC (mp4a)`，遇到 AV1/HEVC/Opus 还会给出
"这条轨道的编码很多播放器解不了"的警告。

**修法二：让站点改用 H.264 —— 这是关键的一招。**
播放器是**按浏览器声称支持什么编码**来选流的。无头 Chrome 支持 AV1，所以 bilibili 喂它 AV1；
在页面脚本执行前注入下面这段（CDP `Page.addScriptToEvaluateOnNewDocument`），
让 `MediaSource.isTypeSupported` / `canPlayType` 对 AV1/HEVC/Dolby 报 false：

    var bad = /av01|hev1|hvc1|dvh1|dvhe|vp09/i;
    // 包装 MediaSource.isTypeSupported 和 HTMLMediaElement.prototype.canPlayType

效果（同一个视频，同一次抓取）：

    改之前: 100022.m4s (AV1 1080P) / 100023.m4s (AV1)  → 播放器打不开
    改之后: 30032.m4s (H.264 480P) / 30016.m4s (H.264 360P) → 到处都能播

（未登录只能拿到 360P/480P；另外换编码会牺牲一点分辨率，值得——不能播的分辨率没有意义。
想要 AV1 就加 `--all-codecs`。）

同一个视频轨经不同 CDN 节点会来好几条，嗅探后用 (文件名, 大小) 去重。

### 2. 206 不是错误，需要 Referer，而且地址会过期

| 请求方式 | 结果 |
|----------|------|
| 不带 Referer | **HTTP 403** |
| `Referer: <抓取的那个页面>` | **HTTP 206 video/mp4, 2.2 MB** |

- **206**：Chrome 用 Range 分段取回的标记，地址本身完整。
- **Referer**：CDN 校验来源页，浏览器直接把地址粘进去必然 403。「解析此资源」会两种请求
  都实测并写明"需要以页面地址作为 Referer"，同时给出两边的真实状态码。
- **签名有效期**：URL 里有 `deadline=<unix 时间戳>`（bilibili 实测约 1 小时），行内标
  **带鉴权/临时**，解析里显示**有效期至 <本地时间>**；过期只能重新抓页面。

### 3. 怎么真正播放

- **VLC/ffmpeg 能带 Referer**，导出的 `.m3u` 已内置：

      #EXTVLCOPT:http-referrer=https://www.bilibili.com/video/BV...
      #EXTVLCOPT:http-user-agent=Mozilla/5.0 ... HeadlessChrome/154.0.0.0 ...

  （一条 `#EXTVLCOPT` 只作用于它后面那一条 entry。）现在导出的是
  `…-30032.m4s (H.264)` / `…-30216.m4s (AAC)`，都能拉下来。
- **画面和声音是两条轨**：单开视频轨只有画面、单开音频轨只有声音。要合成一个文件得
  `ffmpeg -i video.m4s -i audio.m4s -c copy out.mp4` —— 那属于下载+合并，
  本 app 刻意不做（只解析 URL）。
- `.m4s` 是 fMP4 分片（`moof`+`sidx`），播放器认它，但**不能**改成 `.mp4` 当完整文件用。

## bilibili：为什么"解析出来的 URL 一个都下不下来"

实测（2026-10-31，同一页面的 4 个不同 CDN 主机逐个试）：

    裸请求（不带任何请求头）        → 403 Forbidden
    只带 User-Agent                 → 403 Forbidden
    带 User-Agent + Referer         → 200 / 206  ✓

即 **bilibili 的所有 m4s/mp4 地址都必须带 Referer（+UA）**，粘到浏览器/下载器里裸下必然失败。
这跟 v.qq.com 那条路无关：`QQ_URL_RE` 只匹配 `v.qq.com`，bilibili 根本不进那段代码；
`cmd_fetch` 最近只加过"m3u8 相对路径补全"，它只看 m3u8，不碰 m4s。

另外两个必须知道的事实：

- **画面和声音是两条轨**：`…-30016.m4s`(视频) + `…-30280.m4s`(音频，AAC)，单下一条只有画面或只有声音。
- **音轨会被误判成视频**：CDN 对 30280 也回 `video/mp4` ⇒ 原来 27 条"视频"里混着 9 条纯音频。
  现在按流 id 纠正（`30216/30232/30280` → audio，`-(30[23]\d{2})\.m4s`）。

所以界面里给了 **[生成下载脚本]**（后端 `chromeui dl`）：自动挑一条视频（H.264 优先）+ 一条音频
（码率最高），生成一个可直接跑的 .sh —— 带 Referer/UA/Origin 取流（**curl 优先，没有 curl 才退 wget**），
再用 `ffmpeg -c copy` 合成一个 mp4。

    chromeui dl [--out out.mp4]      # 输出 JSON：cmd / script / path / video / audio / headers
    sh /tmp/chromeui/scripts/dl-*.sh [out.mp4]

实测（63 秒的竖屏视频）：2.2 秒下完并合成，`4,270,574 B`、`h264 480x852 + aac`、`duration 62.718` ✓。
用的就是 `curl -fL --retry 3 --progress-bar -H "Referer: …" -H "User-Agent: …" -H "Origin: …"`；
**不需要 aria2**（之前那版用过 aria2，它没有 `-H`、`-o` 还是相对 `-d` 的，坑多，已去掉）。

## 「带这些请求头下载」到电脑：两条老路都下不动大文件（2026-10-31 实测修好）

用户视角：**电脑开 LuCI → 资源行 → [带这些请求头下载] → 存到电脑**。这条路上原来有两个坑：

1. 🔴 **重放了浏览器自己的 `Range` 头**。bilibili 播放器是按片取的，抓到的请求头里就带着
   `Range: bytes=0-14838`；原样重放 ⇒ CDN 只回那一片 ⇒ 文件"只有几百 KB"。
   修：`clean_headers()` 的 `DROP_ALWAYS` 里加 `range`（`if-range` 本来就在）。
2. 🔴 **字节在 RPC 里搬**。`fetch` 在**一个 RPC** 里下完（rpcd 有超时/输出上限），回来再用
   `filedata` 一帧 48 KB 往浏览器挪（`readChunks` 400 帧 ≈ 19 MB 到顶）⇒ 465 MB 的片子永远过不去。

现在的链路（`chromeui fetchbg` + `/cgi-bin/chromeui-dl`）：

    [带这些请求头下载] → chromeui fetchbg <url> --headers …   # 设备后台 curl 落 /tmp/chromeui/dl/
                       → chromeui dlstat --name X             # 网页每 1.2 s 问进度
                       → /cgi-bin/chromeui-dl?name=X          # 浏览器普通 GET，路由器直传

实测（96 分钟那期的音轨，45.2 MB）：

    fetchbg 带浏览器那套头（含 range）→ total = 47,362,442（完整大小，不再是 14839）
    dlstat 下完 → 47,362,442 / 47,362,442  done
    CGI 直取  → http=200 size=47362442 speed=45 MB/s，cmp 逐字节一致，头 ftypiso5 ✓
    网页实测  → 正在设备上下载… 4.5 MB / 45.2 MB 10% … → 正在把文件交给浏览器… 45.2 MB ✓

⚠️ CGI 只服务 `/tmp/chromeui/dl/<纯文件名>`（`^[A-Za-z0-9][A-Za-z0-9._-]*$`），
不做目录遍历；`/tmp` 是内存盘，机器重启就没了。

## 时间上限与"必定结束"（v.qq.com 这类页面）

**默认总时限 110 秒**（`--max-total N` 可调），到点由看门狗线程**杀掉浏览器进程树**并返回：
一个把渲染器卡死的页面不能让整个服务跟着挂。相关修复（都实测过）：

- 页面 JS 超时 `set_script_timeout(8)`：JS 超过 8 秒即视为渲染器卡死；
- 页面加载超时的页面只给 **35 秒**余量（重页面尽早放弃）；
- WAKE/DOM 扫描：原来每个选择器都要全量 `querySelectorAll('*')` 并递归 shadow（16 遍），
  现在**一次遍历复用** + 节点预算 12000；编解码嗅探 4 个/5 秒超时、接口体 15 个；
- 收工 `finally` 里兜底 `kill_browser_tree()`：**杀掉 python 不会杀 Chrome**，这正是之前残留
  越积越多、负载跑到 9 的原因。

实测（Panther X2）：bilibili 单页 **65 秒**成功（79 资源）；v.qq.com 到点停止、**0 残留进程**。

## 腾讯视频 v.qq.com：整集怎么拿（2026-10 实测结论）

**公开接口拿不到整集，但拿到"清单地址"之后，设备能直接下整集。**

实测（vid `u4102ixc2j0`，短剧 48.6 分钟）：

- **匿名就能播整集**：真 Chrome 里 `video.duration=2947s`、`src=blob:`（MSE），不用登录；
- **PC 公开接口只给一段**：`vv.video.qq.com/getinfo?...platform=101001...` 返回 16.3MB / 205 秒 的 mp4，
  而同一响应自报该轨是 **253MB / 2931 秒** ✗ —— 它不是整集（`tail/td` 也不是本集时长，别据此下结论）；
- 真正的流是 **HLS + TS**：`<dir>/0287_gzc_1000102_<keyid>.f321002.10.ts?index=287&start=…&end=…&brs=…&bre=…&ver=4&token=…`
  主机 `omexts/moviets/defaultts.tc.qq.com`，镜像 `<32hex>.v.smtcdns.com`；
- 播放器取流走**签名接口** `POST https://vd6.l.qq.com/proxyhttp`
  （`sphls=6&spwm=1&defnpayver=3&encryptVer=9.2&cKey=<签名>`），签名在 superplayer 里生成，脚本伪造不了；
- **可用的路（无需签名/登录）**：让真 Chrome 播一下，抓到分片 URL，把**分片名换成清单名**：

      分片: <dir>/0287_gzc_1000102_<keyid>.f321002.10.ts?…
      清单: <dir>/gzc_1000102_<keyid>.f321002.ts.m3u8?ver=4      ← HTTP 200，292 段，48.6 分钟

  清单只带 `?ver=4`（token 在分片里），所以**任何机器**都能用这条 URL 下整集。
  本设备实测：清单 `200 / 54577 B`，分片 `200 / video/MP2T / 311892 B`。

**怎么用**：在 PC 上跑 `scripts/qq_playlist.py`（技能库里有，参数化、自带校验）
→ 它打印清单 URL → 把这条 URL 粘到本工具的「**访问这个地址**」→ 拉回清单 → 按 m3u8 流程下载整集。
（也可以直接给 ffmpeg/VLC，记得带 `Referer: https://v.qq.com/`。）

**为什么设备上做不了这一步**：设备只有 `chrome-headless-shell`（old headless），
腾讯播放器会把它的渲染器占死（manifest 成"卡死"）；本机完整 Chrome 无此问题。
要一步到位就得在 chroot 里装完整 Chromium（arm64）——目前不做。

## 独立 API：`/usr/bin/qqurl`（腾讯整集资源地址）

LuCI 只通过这一个入口拿腾讯的整集地址（**只给 URL，不下载**）：

    qqurl <page-url>                        # 派生模式：自己开浏览器听网络（约 90 秒）
    qqurl --put <playlist-url> [page-url]   # 收下外部拿到的清单：校验 + 缓存
    qqurl --serve [--port 8799]             # HTTP API: GET /?url=<page-url> , GET /health

    {"ok":true,"kind":"hls","playlist":"https://…/xxx.f321002.ts.m3u8?ver=4",
     "segments":292,"duration_s":2918.5,"vid":"u4102ixc2j0","took":93.2,"cached":false}
    {"ok":false,"error":"…","stage":"launch|navigate|listen|derive|put"}

- 结果按 `vid` 缓存 15 分钟（分片 token 会过期）；命中的查询 **1 秒**返回（实测）。
- 界面：「多媒体资源」卡片里的 **【腾讯：取整集地址】** → 显示 URL + [复制] + [下载到电脑]
  （下载走已有的 m3u8 流程，文件落在 `/tmp/chromeui/qq_ep.m3u8`）。

### 🔴 设备侧的真实限制（实测，别指望它自己搞定）

`qqurl` 的派生模式**在这台设备上只能拿到广告那条清单**（93 秒，`dirs_seen:1`，`segments:1`）——
因为设备上只有 `chrome-headless-shell`(old headless)，腾讯播放器播不动正片，
正片的分片请求根本不会发出。设备能听网络（408 请求 ✓）但**播不动**这一点无法绕过。

所以实际用法是**两步**：

1. 在有**完整 Chrome** 的 PC 上跑 `scripts/qq_playlist.py <page-url>`（技能库里那份）→ 输出整集清单 URL；
2. 把它交给设备上的 API：

       qqurl --put "<playlist url>" "<page url>"     # 校验 + 缓存（实测：292 段 / 2918.5 秒 ✓）
       # 或直接粘进工具的「访问这个地址」框里取回

（想让设备一步到位 → 在 chroot 里装完整 Chromium（arm64）；当前未做。）

## JSON -> CSV 表格（`chromeui csv`）

「访问这个地址」抓到**纯 JSON** 的返回时，界面上会多一个 **【把这个 JSON 生成 CSV 表格】** 按钮 ✓
（命令行等价：`chromeui csv <url> [--out X.csv] [--headers T|--headers-file F]`）。

表格怎么推断（`_json_table` / `_json_flatten`）：

    list[obj]              -> 列 = 各对象键的并集（嵌套压成点路径 info.city ✓，数组拼成 "a, b"）
    {..,"data":{"list":[..]}} -> 自动找到里面最大的那组对象（国内接口常见的包装形态 ✓）
    纯对象                 -> 两列 key,value（键也走点路径 user.name ✓）
    其它（数字/字符串/嵌套过深）-> 明确报错，并给出响应的前几个字节

- 容忍 `QZOutputJson={...};`、JSONP 这类包装（自动从第一个 `{`/`[` 开始解析 ✓）
- 输出 **utf-8-sig（带 BOM）+ CRLF** ✓ —— Excel 直接打开中文不乱码
- 返回 `{"ok":true,"file":"data.csv","rows":N,"cols":M,"columns":[...],"shape":"records|object|list"}`

## 改前端后必做的一步：`node check_view.js`

`main.js` 里是"先建元素、再绑事件"的写法，**监听器插在变量声明之前会让整页打不开**：

    TypeError: Cannot read properties of undefined (reading 'addEventListener')

改完 main.js 一定要跑（本机）：

    node /home/oem/check_view.js package/luci-app-chrome/htdocs/luci-static/resources/view/chrome/main.js
    # OK render() 执行完毕，返回 DIV   ← 通过；FAIL 会直接给出异常和行号

它用桩把 `render()` 真跑一遍（含全部元素构建），能在打包前抓住这类"页面加载即报错"的问题。

## 🔴 登录站的关键：cookie 必须"装进浏览器"，不只是当请求头发

粘贴请求头后，Cookie 以前只通过 `Network.setExtraHTTPHeaders` 当**请求头**发出去：
服务器认（`/revision/main/getCurrentUser` 返回已登录），但**页面 JS 读 `document.cookie` 是空的** ——
于是播放器/横幅以为你没登录（ximalaya 会显示锁图标 `xuicon-web_album_ic_lock`），点播放什么都不发生。

现在解析出 Cookie 后用 CDP `Network.setCookie` **逐条真正安装**（20/20 成功），页面 JS 也能看到登录态。
实测单集页 `https://www.ximalaya.com/sound/1010704375`（登录头）：

    login: 20/20 cookie(s) installed INTO the browser (document.cookie now carries them as well)
    wake: pressed i.icon _fh                     ← 首次按压即触发播放
    资源 57（audio 1 + image 56）
    音频 https://audiopay.cos.tx.xmcdn.com/download/1.0.0/storages/c19d-audiopay/11/8B/GKwRIaIObSofAEgnrQTQs0VK.m4a?sign=…
    该地址下载：HTTP 200 · audio/x-m4a · 1507328 字节 · 文件头 ftyp ✓

其他相关修复（同一轮）：唤醒按"候选标记 `data-cg-click`"精确按压（此前按另一套类名列表的下标，点错了元素）；
`--click` 支持 **XPath**（DevTools 右键 Copy full XPath）；候选上限 4 → 12，拿到资源即停。

## 专辑页 vs 单集页（喜马拉雅）

- **单集页 `/sound/<id>` 抓不到音频**：站点对"自动化浏览器 + 播放"故意拦截（>137 + DevTools 逻辑），
  同一集在**专辑页**里点就能拿到。工具现在会自动把这集所属的**专辑页地址**写进结果提示：
  「This episode does not play on its own page … It belongs to the album https://www.ximalaya.com/album/xxxxx」。
- **专辑页勾选「逐集点击」**（`--each`）：唤醒前把列表里**每一集**的播放控件按顺序按一遍
  （每次重新查选择器，不用坐标；见 `EACH_PRESS_JS`），这样能一次收集多集的 `audiopay…m4a` 地址。
  实测 6 集 → 5 条不同音频地址。
- 🔴 **每次最多 8 集**（`--each-max N` 可调）：连续加载 20+ 个播放器会把这块板子的无头 Chrome 压崩
  （实测崩在第 14 个左右）。要抓整张专辑就按页分批来。

## 资源请求头展示（已恢复）

每行「解析此资源」里会显示**该资源实际发出的请求头**（token/cookie 是页面 JS 生成的，很有用），
并给出：[复制请求头] [复制为 curl] [复制为 ffmpeg] [带这些请求头下载]。
资源行上带 `带请求头` 标记。抓取时只重放身份类头（Cookie/UA/sec-ch-ua*）。

## 喜马拉雅 VIP 音频：实测可用的完整做法（2026-10-01 验证）

**步骤（页面操作，无需填任何选择器）**

1. 【抓取网页】填**专辑页**地址（`https://www.ximalaya.com/album/<id>`，不是 /sound 单集页）；
2. 勾选 ☐ 粘贴我自己的请求头（登录后才能访问的网页用）→ 粘贴你登录后浏览器里复制的请求头
   （DevTools → Network → 该请求 → Copy request headers / Copy as cURL）；
3. [开始抓取] → 资源列表里会出现 **VIP 音频**：`https://audiopay…xmcdn.com/…/xxxx.m4a?sign=…`；
4. 用 [导出 .M3U（存到设备）] 或 [访问这个地址] 取走；实测该地址直接可下（HTTP 200 audio/x-m4a，ftypM4A）。

**为什么之前点不动（三个坑，都已修）**

- 站点 >137 + DevTools 会故意让单集页播放失败 → 用**专辑页**的"每集播放图标"这条路径（免费/付费都走它）；
- 专辑页的播放图标 **尺寸是 0×0**（在折叠下方/列表里），坐标点击必然落到屏幕角落
  → 必须用 **JS 点击 `element.click()`**（唤醒现在对"无尺寸的播放控件"自动改用 JS 点击，并优先尝试）；
- 重放请求头时**只发身份类头**（Cookie/User-Agent/sec-ch-ua*），
  若把 `Cache-Control: no-cache`/`Pragma`/`Accept: text/html`/`sec-fetch-*` 一起重放，页面会白屏（0 个 XHR）。

**判定登录态是否生效**：标题变成该专辑/该集自己的标题、正文上千字；否则是站点通用标题 + 300 字左右空白页。

## 借鉴：喜马拉雅 VIP 音频的抓法（来自用户提供的可用脚本）

用户提供的可用脚本（xmdl_openwrt.py）用的是：

    audio_pattern = r'https://[^\s"\']*?audiopay[^\s"\']*?\.m4a[^\s"\']*'   # VIP 走 audiopay 路径
    选择器       = i.xuicon-web_album_btn_play_s.playIcon                     # 专辑页"每集"的播放图标
    点击         = driver.execute_script("arguments[0].click();", btn)        # 普通 JS 点击
    节奏         = 点击后 sleep(5) 再从 performance 日志里捞

已按此移植进本 app：

- `collect_logged_media()`：**正则扫整条日志文本**找媒体地址（不再只信 response 的 mime/type 记账）——
  VIP 音频的记账常常不像媒体，只有 URL 本身认得出来；
- 唤醒时对"play/btn"形状的候选**追加一次普通 JS 点击**（脚本验证过的方式），并把等待从 2.2s 提到 **5s**；
- 复现要点：**是在专辑页点"某一集"的播放图标**，不是 /sound 单集页的 `a.play`。

## 弹登录框的站点（抖音这类）：先自动关掉遮罩

抖音精选/课程页每次都会盖一个"登录后免费畅享高清视频"的弹窗，**不关掉什么都不会播、媒体请求一个都不发**。
所以抓取流程最前面加了一步（在唤醒之前，否则所有点击都落在遮罩上）：

1. **识别弹窗**：不看 class 名（抖音的类名是 `oi8cIVOq` 这种乱码），而是看**形状 + 内容**——
   可见、面积 ≥ 屏幕 5%、大致居中（中心偏差 <25% 宽 / 32% 高）、
   **且是它自己中心位置上的最上层元素**（说明它盖在别人上面），并且内部文本含
   `登录 / 扫码 / 密码 / 手机号 / 验证码 / login / sign in / passport`。取其中**最小**的那个（真正弹出的框）。
2. **点关闭**：在弹窗内部找**小的**可见控件（≤64×64），优先 class/aria/title 含
   `close / clos / 关闭 / icon-close / btn-close / cancel`，否则取**离弹窗右上角最近**的那个
   （X 就在右上角）；离角超过 160px 的排除掉，免得点到别的东西。
   点完复查弹窗还在不在，还在就换下一个候选。
3. **兜底**：都点不掉就发一次 **Escape**（模态框的通用退出键），最多重复 3 轮。
4. `--no-close` 关掉这步；「抓取前先点击这个选择器」现在也是**在唤醒之前**执行
   （正好用来填自定义的关闭按钮）。

实测抖音那条 URL：

    close: clicked div.oi8cIVOq
    close: overlay dismissed
    wake: clicked svg.douyin-player-icon-play
    → 真实视频流：https://v26-weba.douyinvod.com/.../tos-cn-ve-15/ow2BFRU0PHBNVJBDCeLIA…  (H.264)
      音频轨：    https://v26-weba.douyinvod.com/.../ogaTAGFiQALoCkfeDArfi…              (AAC)
      另抓到 8 组请求头（含 cookie）

⚠️ 抖音这种页面很重（300+ 图片、还要嗅探编码），一次抓取约 **4 分钟**，进度条会一直走到"完成"。

## 把请求头也带出来（token / xtoken / cookie 这类）

很多站点光有地址不够：请求头里带着 JS 生成的 `token`/`xtoken`/`auth`/`sign`，
还有浏览器最后补上的 `Cookie`。所以抓取时会**顺带记录每条媒体请求实际发出的请求头**：

- 数据来源是 CDP 的两条事件：`Network.requestWillBeSent`（渲染进程看到的头）
  和 `Network.requestWillBeSentExtraInfo`（**最终**发出的头，能拿到网络栈后加的 Cookie）。
- 过滤掉纯浏览器噪声（`accept*`/`connection`/`cache-control`/`sec-ch-ua*`/`sec-fetch-*` …），
  保留 `referer`/`user-agent`/`cookie`/`authorization`/`x-*`/`range` 等有意义的东西。
- **按指纹去重**：一页里几十条媒体请求通常共用同一套头，所以只存 1~3 套、条目里存索引
  （ubus 应答有上限，不能每条都塞一份）。上限：每套 18 个头、上万个字符封顶。

界面上：资源行会多一个标记 **带请求头**；点「解析此资源」后详情里直接列出这套请求头，
并给三个按钮：**复制请求头** / **复制为 curl** / **复制为 ffmpeg**（后两个是拼好的完整命令，
拿来就能在别的机器上跑）。导出的 `.m3u` 里也会以注释列出这套头。

**实测证明**（喜马拉雅那条音频，用抓出来的 `地址 + 头部` 在设备上跑 curl）：

    curl -H 'range: bytes=0-' -H 'referer: https://www.ximalaya.com/' -H 'user-agent: Mozilla/5.0 …'
    → 206 audio/mp4  11507325 字节
    file: ISO Media, Apple iTunes ALAC/AAC-LC (.M4A) Audio

也就是说导出的东西在别处**可以直接用**。
（注意：这个站把鉴权放在 URL 签名里 —— `sign=`/`timestamp=`/`token=`，所以它的头部只有
referer/UA/range；换到把 token 放头部的站点，那些 token 就会出现在这里。）

## 抓不到的站点：媒体要点播放才加载（喜马拉雅这类）

很多站点（喜马拉雅、各种自定义 `<xm-player>` / web-component 播放器）**页面加载完不会请求媒体**，
必须点一次播放，地址才由 JS 写进播放器。抓取流程里加了三层处理：

1. **唤醒（默认开）**：先把页面上所有 `audio/video` 都 `muted` 起来 `play()`（**含 shadow DOM 里的**，
   自定义元素常把真正的 `<audio>` 藏在 shadow root 里），再找"播放按钮"来点。
2. **点哪个按钮**：不能只看大小。实测喜马拉雅页面上最大的是专辑封面上的
   `div.album-play-layer`（点了会跳走），真正属于播放器的是**离媒体元素最近**的那个
   （`a.play`）。所以候选按"到媒体元素的距离"排序，其次按面积。
3. **闭环**：点一个候选 → 检查网络日志里有没有新的媒体响应 / 媒体元素是否拿到了 src →
   没出现就换下一个候选（最多 4 个、最多 3 轮），并且**等播放器渲染出来**
   （喜马拉雅的按钮是加载后 6~9 秒才出现的，等不到就重试，最多 14 秒）。

🔴 关键细节（否则点了等于没点）：

- **必须用可信事件**。JS 里 `el.click()` 的 `isTrusted=false`，多数播放器直接忽略；
  我们用 CDP `Input.dispatchMouseEvent` 在元素坐标上发真实鼠标事件。改之前实测
  "点了但一个媒体请求都没有"，改成可信点击后模拟页立刻出结果。
- **DevTools 复制的选择器会穿透 shadow root**，`document.querySelector` 永远找不到它。
  所以 `--click`（界面上的「抓取前先点击这个选择器」）除了直接查，还会：递归所有 shadow root 查、
  用路径**最后一段**再查一遍。
- 唤醒的每一步都会写进 `/tmp/chromeui/worker.log`（`wake: clicked a.play` /
  `wake: media element is playing [...]`），点没点到一看就知道。

**已知边界**：喜马拉雅对匿名无头会话下发了极验人机校验（页面里有 `gt4.js`/`fe-captcha`），
这种情况下点了播放也不会出媒体——这是站点的反爬拦人，不是点击的问题（同一条音频用登录后的
真实浏览器是能播的）。这类站点请把要点的元素填进「抓取前先点击这个选择器」再试，
worker.log 里的 `wake:` 行会告诉你它点到了什么。

## 带请求头访问 URL（登录站点的通用解法）

做通用登录太麻烦，所以改用**重放请求头**：把浏览器发出的那套请求头（cookie / token / xtoken / auth）
交给设备，由设备带着它们去取资源。这样"需要登录才返回"的站点不需要实现登录流程。

- **后端**：`chromeui fetch <url> --headers "K: V\n…"` 或 `--headers-file headers.txt`
  （直接吃本 app 导出的 `headers.txt`，`#` 注释行自动跳过；`host`/`content-length`/`connection`
  这类由 urllib 自己管的头会被忽略）。回复里带 `status` / `content_type` / `bytes` / `file` /
  `headers_sent`（实际发了哪些头，便于排查）。
- **前端**：「请求头」卡片里新增一节 —— URL 输入框 + 请求头文本框（会自动填入抓到的第一组头），
  按钮 **[带请求头访问]**，成功后排出现 **[下载到电脑]**（二进制安全，`isBin` 已覆盖音视频/压缩包等）。
  每行资源「解析此资源」里也有一个 **[带请求头访问]**，一键把**这条资源自己的** URL + 请求头填进去。

**典型用法（站点要登录才给内容）**：在你自己的浏览器里打开该页面 → F12 → Network →
找到那个请求 → 右键 "Copy request headers"（或 Copy as cURL）→ 把内容粘进请求头文本框，
URL 填上要取的那个地址 → [带请求头访问]。设备就用你的登录态把这东西取下来。

**注意**：如果登录态只存在于你自己的浏览器里，无头浏览器抓页面时并没有登录，
所以抓到的头是匿名的 —— 这种情况请**粘贴你自己的请求头**，而不是用抓页面的那套。

## 翻译不走 LuCI 词典（否则新词条永远显示英文）

🔴 `/cgi-bin/luci/admin/translations/<lang>`（也就是 JS 里 `_()`/`T()` 查的 `window.TR`）
带 **`Cache-Control: max-age=31536000`**：浏览器第一次拿到哪份词典，就**存一年不动**。
于是每次新增的中文词条在用户那边永远是英文，而且删缓存才能好——"很多中文没翻译"就是这么来的。

所以本 app 的界面**不用** LuCI 的词典：

- 构建时 `tools/po2json.py` 把 `po/zh_Hans/chrome.po` 转成
  `/usr/share/chromeui/i18n/zh-cn.json`（和 .po 同源，不会不同步）；
- 后端 `chromeui i18n <lang>` 通过 RPC 把这份 JSON 交给页面（**RPC 没有任何缓存**）；
- 前端在 `load()` 里先取词典（用 `navigator.language`），并在模块内**重定义 `_()` 和 `T()`**
  指向这份词典，所以本视图的每个字串都走它。

验收方式：用 CDP `Network.setBlockedURLs(['*translations*'])` **屏蔽掉 LuCI 的词典**再打开页面，
界面依然全中文即说明生效（实测通过）。`luci-i18n-chrome-zh-cn` 仍然照常打包，不影响别人。

## 请求头：页面级导出（不只是逐条资源）

抓取时记录的请求头现在有两处出口：

1. 每行资源「解析此资源」里的请求头块 + 复制按钮（流媒体、图片、画布页都有）；
2. **独立的「请求头」卡片**（页面上方）：列出抓到的每组请求头（含 **JS 生成的 cookie**）
   以及该组被多少个请求使用，并给两个按钮：

       [复制全部请求头]   [下载 headers.txt]

   `headers.txt` 形如：

       # request headers seen by the headless browser
       # page: https://www.doc88.com/p-...
       # https://face.doc88.com/snapshot/face/...
       cookie: cdb_sys_sid=...
       referer: https://www.doc88.com/
       user-agent: Mozilla/5.0 ...

   这样即使页面**没有流媒体**（doc88 全是画布/图片）也能拿到请求头。

## 把 .m3u 下载到电脑

「导出 .M3U（存到设备）」只是写到设备上的 `/tmp/chromeui/playlist.m3u`；旁边还有一个
**「下载 .M3U 到电脑」**：它先重新导出一次（保证和屏幕上看到的一致），再用 `filedata`
把文件分块读回来，在浏览器里拼成 Blob 触发真正的下载（文件名用页面标题，如
`晚风撞破谎言-第61集.m3u`）。不需要服务端任何新接口，也不受 ubus 应答上限影响。

## 为什么资源列表不用表格（长 URL 会把整页撑爆）

CDN 地址有 700+ 字符，放进 `<table>` 会把表格连同整个 LuCI 页面撑得比屏幕还宽
（侧栏被挤走、手机上一片混乱）。所以资源列表是 **DIV 布局**：
`.cg-mrow` 是 flex，`.cg-mmain{flex:1 1 260px;min-width:0}` —— **`min-width:0` 是让
末行省略号生效的关键**，而表格单元格给不了这个；行内 URL 用 `.cg-clip` 单行省略，
解析详情里则用 `overflow-wrap:anywhere` 换行显示完整地址。
（验收方式：`document.documentElement.scrollWidth > window.innerWidth` 必须为 false。）

## 实现要点（踩过的坑）

- **抓取是后台任务**：`chromeui start` 派一个脱离会话的 worker（`start_new_session`），
  进度和结果写 `/tmp/chromeui/job.json`，前端每 1.5s 轮询 `chromeui job`。
  这样浏览器不会卡在一个几十秒的同步调用上，也有实时阶段提示（排队/启动浏览器/加载页面/
  收集资源/截图）。「中止」= 对 worker 的进程组 `killpg`，chrome 和 chromedriver 一起收掉。
- **flock 用 `pass_fds` 传给 worker**：`start` 加锁后把 fd 继承给 worker，父进程退出锁仍持有，
  并发第二次抓取会被明确拒绝（4G 板子扛不住两个 Chrome）。
- **🔴 ubus 应答有上限（实测 131KB 过 / 262KB 报 `result:[8]`）**：前端所有调用都走
  rpcd 的 `file exec`，超过上限的应答被**静默丢弃**（页面上表现为"永远停在某个阶段"）。
  所以没有任何数据可以整块传：
  - `job` 只回状态（被 1.5s 轮询），**从不带结果**；
  - `result` 回精简结果：可见文本截到 20KB、媒体列表截到 300 条；
  - 完整文本/HTML 落成设备上的文件（`/tmp/chromeui/last.txt` / `last.html`）；
  - 截图用 `filedata` **按 48KB 分块**读回，前端拼成 data URL（拼出来与原件逐字节一致）。
- **🔴 绝不能给 chromedriver 传 `--user-data-dir`**：在这个 glibc-chroot 环境里，chrome 其实
  正常启动了、也把 `DevToolsActivePort` 写进了指定目录，但 chromedriver 只在自己管的临时目录里
  找它，结果 **60 秒后** 报 `session not created: unable to discover open pages`。
  让 chromedriver 自己管 profile（默认）就 1.4 秒会话就绪。
- **🔴 JS 注入的返回值必须用顶层 `return`**：selenium 会把脚本包进 `function(){...}`，
  写成 `(function(){...})()` 的 IIFE 会**静默丢掉返回值**（innerText 就是这样变成空的），
  `SCROLL_JS`/`TEXT_JS` 都是顶层 return。
- **🔴 用 CSS 类的 `display:none` 藏 `<img>` 会踩坑**：之后 `el.style.display = ''` 只是**移除
  内联样式**，类里的 `display:none` 依然生效 —— 图永远不显示。要么内联隐藏，要么显式
  `display:block`。
- **翻译会撞名**：LuCI 把所有 app 的 lmo 合并成一个词表，泛用词会被别的 app 抢走 —— 实测
  `Image`→"镜像"(docker)、`Probe`→"探测"(vputrans)、`Width`→"宽带"、`Copy text`→"复制文字"。
  本 app 一律用不易撞的串（`Image file`/`Analyze URL`/`Window width`/`Copy all text`）。
- 中文/空格 URL 在 DOM 侧是未编码形式，直接拿去下载会失败；合并时以网络层的编码形式为准。
- 抓取结束会保留最近 12 张截图，更早的自动删掉（`/tmp` 是 tmpfs，别囤）。

## 编译 / 安装

    make package/luci-app-chrome/clean            # 🔴 改了包内文件必须 clean
    make package/luci-app-chrome/compile V=s
    # ipk: bin/packages/aarch64_cortex-a53/base/luci-app-chrome_*.ipk
    #      bin/packages/aarch64_cortex-a53/base/luci-i18n-chrome-zh-cn_*.ipk
    scp <ipk> root@<dev>:/tmp/ && ssh root@<dev> 'opkg install /tmp/<ipk>'

⚠️ 就地升级（opkg install）时，版本号没变 opkg 会直接说 "installed in root is up to date" 而不装：
升级前改 `PKG_VERSION`（luci.mk 的 `Version` 字段只取 PKG_VERSION，不含 PKG_RELEASE），
或者先 `opkg install --force-reinstall`。i18n 子包版本恒为 `unknown`，只能 `--force-reinstall`。

中文界面要装 `luci-i18n-chrome-zh-cn`（`.config` 里
`CONFIG_PACKAGE_luci-i18n-chrome-zh-cn=y`），否则显示英文源串。

## 导出调用脚本（网页「导出调用脚本」/ `chromeui script`）

不做模板库：**表单里当下是什么，就导出什么**。点「导出调用脚本」后后端把同一套参数
（`GRAB_SPEC`，和「开始抓取」完全一致）原样拼成：

    命令行（可复制）   /usr/bin/chromeui grab 'https://…' --timeout '30' --width '1280' …
    脚本               /tmp/chromeui/scripts/grab-<host>-<时间戳>.sh   (0755，可直接跑)

```sh
sh /tmp/chromeui/scripts/grab-example.com-20261031-085052.sh            # 用它自己写死的地址
sh /tmp/chromeui/scripts/grab-example.com-20261031-085052.sh <另一个url> # 换页面不用改文件
sh ... > out.json                                                       # 结果是单个 JSON
```

后一句话很重要：脚本里地址被提成 `DEF_URL` + `URL="${1:-$DEF_URL}"`，所以同一个脚本
可以反复用于任何页面。粘贴的请求头会作为 `HDRS='…'`（单引号安全转义，含引号/换行都行）
写进脚本，命令行里显示成 `--headers "$HDRS"`。

后端 JSON：`{"ok":true,"url":…,"cmd":"<命令行>","script":"<脚本全文>","path":…,"file":…,"bytes":N}`
—— 网页直接显示 `cmd`、把 `script` 放进文本框，「复制命令行 / 复制脚本 / 下载脚本」都不再
多发一次 RPC。`/tmp` 是 tmpfs，重启后脚本就没了（要留就下载走）。

🔴 这里用的是 `grab`（一次性、打完 JSON 返回），不是网页轮询用的 `start`/`job` ——
脚本是给 shell/别人/AI 复现用的。

## 命令行用法（不通过网页也能用）

    chromeui version
    chromeui status
    chromeui selftest
    chromeui grab  <url> [--wait CSS] [--timeout N] [--width W] [--height H]
                         [--full] [--scroll] [--eager] [--html] [--no-shot]
                         [--shot PATH] [--max N]
    chromeui start <url> [同上]      # 后台跑，立刻返回
    chromeui job                     # 进度（不含结果，供轮询）
    chromeui result                  # 精简结果（网页用）
    chromeui filedata --file <name> [--offset N] [--len L] [--b64]
                                     # 分块读 /tmp/chromeui 下的大文件
    chromeui stop                    # 中止
    chromeui mediaprobe <url>        # 头部信息 / m3u8 解析
    chromeui fetch <url> [--out P] [--max-bytes N]
    chromeui script <url> [同上]     # 不启动浏览器：把等价命令行 + 可直接跑的 .sh 导出来
    chromeui export [--out P] [--all]

`chromeui grab`（命令行）仍然一次性打印完整 JSON（含 base64 截图与 HTML），方便脚本用；
`start`/`job`/`result` 那条路是给网页的，受上面的 ubus 上限约束。

所有命令输出**单个 JSON**；错误是 `{"ok":false,"error":"<英文模板>","error_args":[...]}`，
中文在前端 `/usr/lib/lua/luci/i18n/chrome.zh-cn.lmo` 里翻译（后端不含任何中文字面量）。

调试：`/tmp/chromeui/worker.log`（worker 的 stdout/stderr）、`/tmp/chromeui/chromedriver.log`、
`/tmp/chromeui/job.json`（状态+精简结果）、`last.html` / `last.txt`（完整文本与 HTML）。
