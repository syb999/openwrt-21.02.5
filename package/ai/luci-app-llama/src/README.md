# C Agent — RK3566 本地 AI 助手

纯 C 写的 Agent 框架，跑在 RK3566 (Panther X2) / OpenWrt 21.02.5 上，
后端是 llama.cpp 的 `llama-server`（OpenAI 兼容接口，纯 CPU）。

Python 版已废弃删除，这里只有 C 实现。

## 目录

```
agent.c        主程序：HTTP 客户端 / JSON / 插件加载 / 任务分块 / 路由
cgi.c          Web 后端：?task ?job ?env ?file ?upload ?stop_job ?auto*
json.c json.h  轻量 JSON 解析
plugin.h       插件接口（plugin_entry / plugin_auto_entry）
memcheck.c     内存记账（--memcheck 打开）
memstat.sh     扫日志里的 [mem] 行做汇总
Makefile       编译规则

plugins/       11 个插件，见下表
docwrite.py    文档写入脚本的源（生成 py_write.h）
py_write.h     docwrite.py 转成的 C 字符串，doc.so 内嵌执行
doc_read.inc   文档读取：docx/xlsx/pptx 的 zip+xml 解析（纯 C）
```

## 编译

本机编出来是 x86_64，**必须到板上用交叉工具链重编**：

```sh
make CC=aarch64-openwrt-linux-musl-gcc LDFLAGS=
```

- `LDFLAGS=` 是必需的：默认的 `-static` 在 musl 上会让 `dlopen` 失效
- 编译选项里带 `-D_FORTIFY_SOURCE=0`（musl + 老 glibc 头文件的组合会报错）
- 改了 `.inc` 或 `py_write.h` 之后要 `rm -f plugins/xxx.so` 强制重编，
  板上时钟和主机有偏差时 `make` 会误判「不需要重建」

## 部署

```sh
cp agent          /opt/llama/agent/agent-c
cp cgi            /www/cgi-bin/agent
cp plugins/*.so   /opt/llama/agent/plugins/
chmod 755 /opt/llama/agent/agent-c /www/cgi-bin/agent /opt/llama/agent/plugins/*.so
```

板上 `/opt/llama/agent/` 需要：

```
agent-c       主程序

memstat.sh    可选，分析内存记账
plugins/      11 个 .so
```

## 插件

| 插件 | 干什么 |
|---|---|
| `math` | 数学：算式 / 方程 / 方程组 / 统计 / 数论 / 行程 / 鸡兔 / 工程 |
| `curl` | 解析 DevTools「Copy as cURL」直接发请求，响应按 JSON/HTML/二进制分类落盘 |
| `jq` | 本地 JSON：probe 探测结构 / get 分批取值 / table 导表格+CSV / query 自定义表达式 |
| `doc` | 文档读写：PDF/Word/Excel/PPT → 文本；生成 docx/xlsx/pptx/pdf |
| `sys` | 文件与系统：sysinfo / shell / list / du / grep / read / write / http_get |
| `net` | 网络诊断：ip / route / dns / arp / ping / port / trace / wifi |
| `owrt` | OpenWrt 配置：uci_* / iwinfo / iw / service / logread |
| `music` | MIDI 生成 / FC 音色合成 MP3 / 导入 MIDI 转 MP3 |
| `batch` | 大任务切小 JSON 分批生成再合并 |
| `table` | 按列排序取 TOP/BOTTOM |
| `sysmon` | 系统快照 + 自动巡检（`sysmon-auto`） |
| `browser` | 登录态网站：probe 探测登录框 / autologin 自动填表取 cookie（chrome+selenium） |

后两个是自动化插件：默认停用，支持前台跑和后台守护，网页可启停。

## 依赖

`jq` 和 `curl` 是可执行文件，不是 Python 库：

- 缺 `jq` → 不加载 `jq.so`，环境面板里 `libs.jq = 0`
- 缺 `curl` → 不加载 `curl.so`

依赖表在 `agent.c` 的 `PLUG_DEPS[]`，加新依赖加一行即可，插件本身不用改。

**读取**文档（板上不需要任何 `.py`）：

| 格式 | 实现 | 效果 |
|---|---|---|
| `.docx` | 解 zip → `word/document.xml` → `<w:t>` | 纯 C，段落换行正确 |
| `.xlsx` | 解 zip → sheet xml + sharedStrings/inlineStr | 纯 C，中文数字都对 |
| `.pptx` | 逐页解 `ppt/slides/slideN.xml` → `<a:t>` | 纯 C，按页分段 |
| `.pdf` | pypdf（脚本运行时生成到 `/tmp/.doc_pdf.py`） | 完整，21 页文档验证过 |
| 纯文本 | 直接读（限 8MB） | 完整 |

PDF 为什么没做纯 C：它要解析对象树 → 每页资源 → 字体 `/Subtype`
→ 各自的 `/ToUnicode` CMap → 文本流里 `/F1 Tf` 切换编码。子集嵌入字体
+ 混合编码的真实 PDF 上，几百行的 C 实现还原不出文字（pypdf 是几千行）。
Office 三种是纯 C，PDF 交给 pypdf —— 但脚本内嵌在插件里生成，
不依赖板上任何 `.py` 文件。

**写**文档走纯 C + python3 库：doc.so 里 fork 出 `python3 -c <脚本> <op>`，
数据从 stdin 进、结果从 stdout 出。那段脚本内嵌在 `py_write.h`（C 字符串），
板上不需要任何 `.py` 文件。

写操作依赖 `python-docx` / `openpyxl` / `python-pptx`（PDF 还要 `pypdf`）。

## browser 插件（登录态网站）

免登录的页面用 `curl` 就够；这个插件专治需要登录的站点。

```
probe      {"op":"probe","url":"https://x.com/login"}
           → inputs[{i,type,name,label,placeholder}] / buttons[{i,text}] / captcha
autologin  {"op":"autologin","url":"...","user":"账号","pass":"密码"}
           → 自动猜账号/密码框 → 填 → 点登录 → 返回 cookies
login      {"op":"login","url":"...","steps":[{act:fill,i:0,value:..},{act:click,i:0}]}
           → 需要先切 tab / 有 iframe 时用这个精确控制
cookie     {"op":"cookie","url":"..."}        直接导出 cookie
shot       {"op":"shot","url":"...","path":"/tmp/a.png"}
```

框架对"账号=X 密码=Y 登录 http://..."这种任务会直接走 autologin，不经过模型
（3B 组不出 steps，实测两轮都传空 url）。

坑（都在 `browse.py` 里）：

- **不能传 `--user-data-dir`**：chroot 里 chromedriver 找不到 DevToolsActivePort，
  60 秒后报 `unable to discover open pages`。所以会话无法跨进程复用，
  登录流程必须一次跑完。
- `page_load_strategy = "eager"`：视频/门户站的子资源永远加载不完。
- `driver.command_executor.set_timeout(15)`：浏览器挂了以后 selenium 默认会
  在死连接上重试 30 秒。
- 填表用 `Object.getOwnPropertyDescriptor(proto,'value').set` + 手工派发
  `input`/`change` 事件，否则 React/Vue 受控组件收不到。
- `execute_script` 的脚本是函数体，IIFE 要写 `return (...)`；带参数的
  箭头函数不能 `return (arg) => {...}`（那只返回了函数），要用 `arguments[0]`。
- 可用内存 < 350MB 直接拒绝；chrome 起来要 300MB 上下。
- 200 秒 alarm 兜底，页面卡死自动 kill。

autologin 的内部流程：

1. 打开页面 → 探测主文档；没表单就**遍历 iframe**（很多站点把登录框放 iframe 里）
2. 还没表单 → **循环点开登录入口**（最多 4 步）：先找 `账号登录 / 密码登录` 这类 tab，
   再找 `登录 / 注册 / Sign in` 这类入口。真实站点常见两步——先点 header 的"登录"
   打开弹窗，再点"账号登录"切 tab，每次点完重新探测
3. 猜账号框（type=password 定密码，其余按 name/label/placeholder 里的
   user/account/phone/email/账号/手机 等词定账号）
4. 填表（受控组件用 value setter + 派发 input 事件）→ 点登录按钮（跳过 tab）
5. 五个信号判断登录成没成：有没有 session 类 cookie、**localStorage 里有没有
   token**（现代 SPA 常不写 cookie 只存 localStorage）、URL 变没变、离开登录页没有、
   页面上有没有"退出/我的"
6. 返回 `cookies` + `cookie_header`（curl 的 Cookie 头）+ `storage`
   （localStorage / sessionStorage 全量，SPA 的 token 在这）

实测（7 个不同布局的模板，`tests/gen_login_pages.py` 可重建）：

| 页 | 特点 | 结果 | 耗时 |
|---|---|---|---|
| p1 | 标准中文表单 | OK | 8.1s |
| p2 | 英文 Sign in | OK | 8.0s |
| p3 | 只有 name/id | OK | 8.0s |
| p4 | 带验证码 | 明确报错（需要人工/OCR） | 4.0s |
| p5 | 需要先切 tab | OK（自动点了"账号登录"） | 9.5s |
| p6 | React 受控 | OK | 8.2s |
| p7 | 复杂布局干扰 | OK（跳过站内搜索框） | 8.2s |
| v1 | Vue 3 · v-model + localStorage token | OK | 8.5s |
| v2 | Vue 3 · 弹窗 + 两步点击 + 懒加载 800ms | OK | 12.7s |

**8/9 通过，平均 8.5 秒**（p4 验证码是能力边界，报错明确）。

Vue 场景的两个坎：

- **v-model 受控组件**：直接 `el.value = x` 无效，Vue 的响应式收不到。必须用
  `Object.getOwnPropertyDescriptor(proto,'value').set` + 手工派发 `input` 事件。
- **token 不在 cookie 里**：v1/v2 都往 `localStorage` 存 `access_token` / `token`。
  只导 cookie 会漏掉登录凭证，所以要 `JS_STORAGE` 把两个 storage 全 dump 出来。
  注意写成 IIFE（`return (() => {...})()`），写成 `var` + 尾行 `return` 在
  selenium 里会返回 None。

测试页在 `tests/vue/`（v1/v2）和 `tests/gen_login_pages.py`（p1-p7，运行时生成）。

### hunt：按流程走完真实网站，抄目标页的请求头

**这是插件真正的用途。** 很多站点的请求头里有 JS 现算的签名（`X-Sign`
= md5(时间戳+盐)、`X-Token`、`X-Nonce`…），逆向成本极高。换个思路：让浏览器
自己登录、自己点进目标模块、自己发请求，我们把浏览器算好的完整 header 抄走。

真实站点不是一个"点一串文字"能描述的。典型过程是这样：

```
第 1 页  登录页      输账号密码
第 2 页  工作台      菜单是异步渲染的，要等它出来
第 3 页  业务管理    有个搜索框，填订单号 + 按回车
第 4 页  搜索结果    点"订单详情"
第 5 页  订单详情    ← 目标页，这里的请求头才有用
```

`flow` 就是把这件事写成一步一步的脚本，**每步声明做什么、在哪个页面、干什么用的**：

```json
{"op":"hunt",
 "url":"https://x.com/login",
 "flow":[
   {"act":"login","user":"账号","pass":"密码",
    "page":"登录页","note":"输账号密码登录"},

   {"act":"wait_for","text":"业务管理","tries":15,
    "page":"工作台","note":"菜单异步渲染，等它出来再点"},

   {"act":"click","text":"业务管理",
    "page":"工作台","note":"点左侧菜单进入模块"},

   {"act":"fill","field":"订单号","value":"SO20261119001",
    "page":"业务管理","note":"搜索框填单号"},

   {"act":"press","key":"Enter","field":"订单号",
    "page":"业务管理","note":"回车触发查询"},

   {"act":"click","text":"订单详情",
    "page":"搜索结果","note":"点进详情页"},

   {"act":"capture",
    "page":"订单详情","note":"目标页：抓这里发出的请求头"}
 ],
 "wait":3,
 "match":"api"}
```

#### 每步的字段

| 字段 | 说明 |
|---|---|
| `act` | 必填，动作类型（见下表） |
| `page` | 这一步在哪个页面，**只用于回显**，方便人看懂走到哪了 |
| `note` | 人话说明，同样只回显 |
| `text` | `click` / `wait_for` 要匹配的文字 |
| `field` | `fill` / `press` 定位输入框（placeholder/name/id 的片段） |
| `value` | `fill` 要填的值 |
| `sec` | `wait` / `goto` 等多少秒 |
| `tries` | `wait_for` 等几轮（每轮 0.4 秒） |
| `key` | `press` 发什么键，默认 Enter |

#### 动作类型

| act | 干什么 |
|---|---|
| `goto` | 直接跳 URL |
| `login` | 用账号密码登录当前页（自动认验证码，走 ppocr） |
| `click` | 点文字匹配的元素（支持 div 假按钮、Shadow DOM、iframe） |
| `fill` | 填输入框（支持 contenteditable） |
| `wait` | 死等 N 秒 |
| `wait_for` | 等某段文字出现，等不到就报错（异步渲染必用） |
| `press` | 给指定输入框/当前焦点发按键 |
| `capture` | 标记"目标页在这里"，**这之后发的请求才算数** |

#### 返回

```json
{"ok":true,"stage":"done",
 "flow_done":7,"flow_total":7,
 "steps":[
   {"step":1,"act":"login","page":"登录页","note":"...","ok":true,"url":"..."},
   {"step":5,"act":"press","page":"业务管理","note":"...","ok":true,"target":"订单号"}
 ],
 "final_page_only":true,"noise_dropped":0,
 "request_count":1,
 "requests":[{"kind":"fetch","method":"GET","url":"/api/order/detail/SO...",
              "headers":{"X-Sign":"6a4f24cc","X-Nonce":"6mjfkw7l",
                         "X-Timestamp":"1795053863058","X-Trace-Id":"trace_6mjfkw7l"}}],
 "best":{...},
 "curl":"curl 'http://x/api/order/detail/SO...' -H 'X-Sign: 6a4f24cc' ..."}
```

失败时 `stage:"flow"`，`error` 会指明**第几步、哪个页面、什么动作、为什么**：

```
"error":"第 4 步没成（业务管理 / 搜索框填单号）：找不到输入框「订单号」"
```

**老写法还能用**：只给 `path:["订单管理","订单列表"]`（一串要点的文字）会
自动补一步 `login` 和收尾的 `capture`，适合结构简单的站点。

`requests` 已按"像不像业务接口"排过序（静态资源淘汰，带 `X-*`/`sign`/`token`
头的加分，POST 加分），`best` 是最高分那个，`curl` 可以直接粘。

**只保留最终目标页发出的请求。** 这一点很关键：中间每一层菜单页都会自己发请求
（菜单树、待办数、权限、统计…），那些对复现目标接口毫无用处，混在结果里只会
干扰判断。做法是记下**点最后一级菜单之前**的浏览器时刻（`Date.now()`），
只留那之后发出的请求：

- `final_page_only: true` —— 时间切分成功
- `noise_dropped: 2` —— 中间层被丢掉的请求条数
- `final_page_only: false` + `warning` —— 时间戳没生效，退回了全部请求
  （拦截器注入失败时会这样，此时结果里可能混着噪音）

#### 请求拦截器怎么工作的

1. `Page.addScriptToEvaluateOnNewDocument` 注入 hook → **每次导航都重新生效**，
   多级跳转不会漏
2. 包住 `XMLHttpRequest.open/setRequestHeader/send` 和 `window.fetch`，
   把 method / url / headers / body 记下来
3. 结果写 **`sessionStorage`** —— 换页后 `window` 变量全没了，只有它还在
   （限制是同源、5MB，所以只留最近 300 条）

#### 已知限制

- **reCAPTCHA / hCaptcha / 滑块**：原理上过不去，会明确报
  「这个站点要过 reCAPTCHA 人机验证，自动化过不去，得人工点一下」
- **closed 模式的 Shadow DOM**（`attachShadow({mode:'closed'})`）：规范上就拿不到，
  open 模式的能穿
- **菜单文字必须跟页面完全一致**：全角空格、繁体、图标字符都会导致点不上，
  报错里会说清是第几级没点上
- **跨域 iframe**：selenium 切不进去，只能操作同源的

### 账号安全：密码错绝不重试

**这是最要紧的一条。** 很多站点密码错 5 次就锁号、锁 IP。拿凭据反复试探
是最危险的做法 —— 宁可这次失败、把原因说清楚，也不能把账号试废。

所以 `tries` 的语义是「**验证码最多换几张**」，不是「凭据最多试几次」。
失败先分类，只有验证码类才允许再来：

| 失败类型 | 重试 | 理由 |
|---|---|---|
| `credentials` 账号或密码错误 | ❌ **立即停** | 会触发锁号/锁 IP |
| `captcha` 验证码填错 | ✅ 最多 3 次 | 换一张验证码，不涉及凭据 |
| `captcha_unreadable` 验证码认不出（还没提交） | ✅ 最多 3 次 | 同上 |
| `recaptcha` 人机验证 | ❌ | 重试也没用 |
| `npu` NPU/内存不足 | ❌ | 环境问题，重试同样失败 |
| `structure` 页面结构对不上 | ❌ | 下次还是一样 |
| `unknown` 判不出来 | ❌ **保守不试** | 宁可不登录，也不冒险 |

实测（故意传 `tries=3`，看它会不会真的提交三次）：

```
GitHub     tries=3 → tries_used=1  credentials  安全停止
恩山论坛    tries=3 → tries_used=1  credentials  安全停止
知乎       tries=3 → tries_used=1  unknown      不重试（保守）
B站        tries=3 → tries_used=1  unknown      不重试（保守）
```

返回里 `stopped_for_safety: true` 表示是主动停手，不是碰巧只试了一次。

#### 判定靠什么

1. **页面错误提示**（中英双语关键词表，`PWD_ERR_PAT` / `CAPTCHA_ERR_PAT`）
   —— 覆盖"密码错误""账号不存在""登录失败""账号已锁定""尝试次数过多"
   "Incorrect username or password""invalid credentials""too many attempts" 等
2. **页面文字必须在 `_login_once` 里当场采**：恩山的错误提示闪 2 秒就消失，
   等外层判失败原因时早读不到了
3. 判不出来就归 `unknown`，**保守不重试**

#### 顺带修的两个判据 bug

- **`logged_in=no` 被当成登录凭证**：GitHub 未登录也发 `logged_in` cookie，
  名字对上了但值是 `no`。现在连值一起看，`no/false/0/guest/anonymous` 一律不算。
- **探测时机太早**：GitHub 登录失败是一次 302 回登录页，抢在重定向中间 probe
  会拿到空表单，判据因此误以为"密码框消失了"，把**失败判成成功**。
  现在拿不到输入框会再等再探（最多 3 轮）。

### 真实站点验证（假凭据）

专门验"判据会不会说假话" —— 把登录失败误判成成功是最危险的，下游会拿一份
没权限的 cookie 去请求，然后到处报"接口返回 403"。

| 站点 | 特点 | 结果 |
|---|---|---|
| GitHub | 标准英文表单 | ✓ 填值成功；判据 `score 0`，正确判失败 |
| 知乎 | **默认短信验证码登录**，压根没有密码框 | ✓ 自动切"密码登录"tab → 填值成功 → `score 0` |
| B站 | 登录按钮不是 `<button>` | ✓ 填值成功；`score 0` |
| 京东 | 有图片验证码 | 报"验证码识别不出来"（京东那种码 ppocr 认不了，合理） |

#### 判据怎么改的

第一版是"cookie 名含 sid/sess/token + URL 变了 + 不在登录页"凑分。
**GitHub 拿错密码直接判成登录成功（score 3）** —— 它未登录也发 `_gh_sess`，
按名字判断必然误判。

改成按可靠性排序：

1. **页面上还有密码框，而且 URL/标题还是登录页** → 铁定没进去（最强信号，
   成功登录后登录表单必然消失）
2. **出现"退出/注销/登出"入口** → 基本成功
3. 没有密码框了 + 拿到 auth 类 cookie → 可能成功

三个容易踩的细节：

- **cookie 名单必须收紧**：`xxx_sess` / `SESSION` 是匿名会话，未登录就有
- **标题含"登录"不等于在登录页**："登录成功"、"欢迎登录" 都含这两个字，
  得把"成功/欢迎/首页/工作台"排除掉
- **不能把 `before_url` 拼进"当前在哪"的判断**：登录成功后它仍然含 `login`，
  一拼就永远判"还在登录页"

#### 填值：React 站点只认真实键盘事件

知乎填进去的值会**立刻变空**。原因：`dispatchEvent(new Event('input'))` 生成的
事件 `isTrusted=false`，React 的 onChange 会校验并把它清掉。

解法是 `ActionChains.send_keys_to_element()` —— 发的是真实键盘事件，
`isTrusted=true`，框架才认。现在的顺序是：**真实键盘 → 不行再退回 JS 设值**。

两个坑：
- **`el.clear()` 在 glibc chroot 的 chromedriver 上会段错误**，清空改用 JS 设 `value=''`
- `el.click()` 会报 `element click intercepted`（即使元素没被挡），
  但 `send_keys_to_element` 不检查遮挡，可以直接用

#### 默认手机号/短信登录

很多国内站点（知乎、淘宝、美团）默认是短信验证码登录，页面上**只有手机号框、
没有密码框**。现在检测到"有输入框但没有密码框"会先点"密码登录"切 tab，
再重新探测；切不过去才报"没找到密码框"。

### 验证码

```
captcha  {"op":"captcha","url":"...","retry":3}
         → {"text":"9mk6", "pngs":[...], "attempts":[...]}
```

autologin 默认 `ocr=true`，失败会**自己重试**：每轮重新加载页面（验证码会换一张）
→ 重新识别 → 重填 → 重提交，最多 `tries` 轮（默认 3），每轮结果都记在 `history` 里。
识别不出来 / 都失败才把证据交给调用方，`ocr=0` 可以关掉自动识别。

**只用板上的 ppocr**（`/usr/bin/ppocr`，rk356x-npu-ocr 包提供的 PP-OCRv4，
RKNN 模型走 NPU，0.7 秒一张，不占 CPU）。tesseract 已经不用了：它慢
（CPU 上 1-2 秒）而且对验证码只有 50%。ppocr 不在就直接报"识别不可用"，
不做静默降级。

**实测（30 张，三档难度，`/tmp/captcha`）**：

| 类型 | ppocr |
|---|---|
| 干净（无干扰无旋转） | 8/10 |
| 轻微旋转 ±6° | 5/10 |
| 旋转+干扰线+噪点 | 4/10 |
| **合计** | **17/30 = 57%** |

（作为对比，tesseract 是 15/30 = 50%，两个一起跑 19/30 = 63% ——
多的那点准确率不值得多花一倍时间。）

**放大图片没用**：ppocr 放大 x2/x3 后，干净档还是 8/10，轻微档 5→4，
强化档 4→3，越放越差，所以代码里不做图像增强。

**PP-OCR 也是通用 OCR，不是验证码专用模型。** 它的问题是会漏字符
（`43vh` → `43`）和把字符顺序读错（`zb97` → `b97z`）。真要 90%+ 得上
ddddocr，但它依赖 onnxruntime，这块板子装不上（musl 没有 aarch64 wheel，
`/opt/chrome-root` 里也没有 python3）。

其余坑：

- **别用 `element.screenshot_as_png`**：截图会被页面 CSS 缩放/加边框，
  原图 `9mk6` 截出来识别成 `9nk6`。优先从 `src` 下载原图（带 cookie），
  canvas 没 src 才退回截图。
- **别搞 psm 投票**：psm 8/13 对验证码会读出一堆碎字符（`onke`），
  投票反而把 psm 7 的正确结果压掉。按 7 → 8 → 13 优先级取第一个长度合理的。
- **except 别吞编程错误**：`_find_captcha_img` 里早先把整段包在
  `try/except: pass` 里，重构时漏了个常量定义，NameError 被吞掉，
  表现成"页面上没找到验证码图片"，查了半天。

### 拖拽 / 滑块验证码（op=drag）

```json
{"op":"drag","url":"https://x.com/login",
 "auto":true,              // 从背景图自动识别缺口
 "distance":224,           // 或者直接给要拖多少 CSS 像素（auto=false 时用）
 "thumb":".slider-btn",    // 可选：指定滑块选择器，不给就自动找
 "open_click":"开始验证",    // 可选：有的站点要先点一下才弹滑块
 "tries":3}
```

返回 `ok` + `gap`（识别到缺口多少像素）+ `history`（每次拖了多少、页面什么反应）。

#### 缺口识别为什么用竖直边缘

第一版按"暗色区域"找，**直接失败**：测试图的背景是暗色渐变，整片都被判成洞。
后来改按"亮色描边"也不通用。

现在用的是：**算每列与右移 4 列的逐像素差**，差异最大的两处就是缺口的左右竖边，
第一个峰取左边。这跟背景深浅、颜色完全无关，只要缺口有完整的竖边就能找到。
实测 380px 宽的图识别到 230（真值 232）。

#### 轨迹必须是"人手"的

匀速直线位移是最容易被判定成机器人的特征。这里是 ease-out（先快后慢）
+ 每步 ±0.8px 随机抖动 + 松手前 50~150ms 停顿。

#### 一个必须校正的坑：累积误差

浏览器会对鼠标坐标取整，多次 `move_by_offset` 累积下来**实际位移比参数少
10~12px**。表现就是"看着拖到位了，验证就是不过，差 -12px"。

所以松手前要读一次滑块的真实位置，把差值补上：

```python
actual = thumb.location["x"] - x0
if abs(distance - actual) >= 2:
    ActionChains(drv).move_by_offset(distance - actual, 0).perform()
```

加了这个之后 `distance=224` / `232` / `auto` 三种方式都能一次通过。

#### 真实站点的情况（如实说）

- **恩山论坛（www.right.com.cn）**：连打 5 次错密码都**没出现**拖拽验证，
  提示从 "you can try 3 times" 变成 "try 2 times" 后就静默了。这个站的拖拽
  应该是**凭据正确**或特定风控条件下才触发，没有账号复现不了。
- **京东**：登录页是**图片文字验证码**（扭曲字符），不是拖拽；拖拽只在
  特定风控下出现，同样没法稳定复现。
- **所以这个功能是用自建的拼图滑块页验证的**（`tests/gen_slider.py`），
  真实站点的兼容性还没实测——如果你有能稳定触发拖拽的站点，告诉我再调。

#### 局限

- **点选类验证码**（"请依次点击图中的 xxx"）不支持，那是另一套逻辑
- **旋转验证码**不支持
- 缺口识别依赖"有完整竖边"，如果缺口是斜的/圆形/只有模糊阴影，会识别不出，
  得退回 `distance` 手动试
- **风控更严的站点**会检测鼠标轨迹的物理特征（加速度曲线、抖动频谱），
  这里的模拟只是"看起来像人"，专业反爬系统仍可能识破

### 失败现场截图

登录失败时插件自动截一张当前页面，`shot` 字段带回路径，页面把图片直接显示在
回答气泡里（点开看大图）。人工 debug 就靠这个：卡在哪一步、页面上有没有报错、
验证码长什么样，一眼能看到。

```json
{"ok": false, "error": "登录没通过",
 "shot": "/tmp/llama_agent/shots/try3_084540.png",
 "history": [{"attempt":1, "shot":"...try1...png"}, ...]}
```

截图落在 `/tmp/llama_agent/shots/`（统一到 WORK_DIR 下，`?file=` 才找得到），
每轮一张。

前端 `pluginResultHtml()` 认这几个字段：`shot` / `image` / `pngs[]` /
`attempts[].png`，去重后最多显示 4 张。

`cgi.c` 的 `do_file()` 为此改了三处：

- 候选路径加了 `%s/shots/%s`
- MIME 认 png / jpg / jpeg / gif / webp
- 图片用 `Content-Disposition: inline` —— 用 `attachment` 的话浏览器只会弹下载框，
  `<img src="...?file=x.png">` 显示不出来

图片缩放：同时给 `max-width:100%` 和 `max-height:320px` + `width:auto;height:auto`，
浏览器按原比例缩到最严的约束。只给 `width:100%` 配 `object-fit:contain` 会留黑边
（683×318 的盒子里放 728 宽的图），只给 `max-height` 会把图压扁。

## 插件启停

页面上点标题的 ✎ → 设置弹窗 → 插件区勾选/取消 → 保存。

- 名单写到 `/etc/llama.plugoff`（一行一个插件名）
- agent 启动时读它，命中的插件**直接不 dlopen**（连内存都省）
- 环境面板 `disabled` 字段回报被停用的名字；前端把它们补进列表末尾并置灰，
  方便重新打开（agent 没加载它们，env 的 plugins 数组里本来没有）

为什么要这个：插件一多，`tools_prompt()` 里的工具表就长，3B 的选错率跟着上升，
每步也更慢。关掉不用的几个，prompt 短一截，任务跑得更快。

也可以直接 curl：

```sh
curl "http://<板子>/cgi-bin/agent?set_plugoff=music,batch"
cat /etc/llama.plugoff          # → music\nbatch
curl "http://<板子>/cgi-bin/agent?env=1"   # 看 disabled 字段
```

只接受字母数字下划线，其它字符直接丢掉（防止往配置里写脏东西）。

## 环境检测与自动加载

`PLUG_DEPS[]`（agent.c）声明每个插件依赖什么，`load_plugins()` 在注册前用
`access(need, X_OK)` 检查，缺了就不加载并记一行日志：

| 插件 | 依赖 | 缺了会怎样 |
|---|---|---|
| `curl` | `/usr/bin/curl` | 不加载 |
| `jq` | `/usr/bin/jq` | 不加载 |
| `browser` | `/opt/chrome-root/cdrv.sh` | 不加载（chrome 不在这个位置就是没装） |

环境面板把这些显示在"专用库"分组里（jq / curl / ppocr / sympy / midiutil /
pypdf / docx / openpyxl / pptx）。`ppocr` 是 `/usr/bin/ppocr`
（rk356x-npu-ocr 包），在不在都能正确显示。

## 改内嵌脚本

`py_write.h` / `py_browse.h` 都是 Python 脚本转义成 C 字符串的产物。
仓库里没有原始的 `.py`（保持"板上零 .py"），改的逻辑是这样：

1. 写一份新的 Python 脚本，接口照旧：
   - `sys.argv[1]` 是 op（`text` / `xlsx` / `docx` / `pptx` / `pdf` …）
   - 数据从 stdin 读（JSON）
   - 结果以 JSON 打到 stdout
2. 转成 `py_write.h`：

```sh
python3 - <<'EOF'
src = open('new_write.py', encoding='utf-8').read().split('
')
out = ['/* 自动生成：文档写入脚本内嵌在这里 */', '#ifndef PY_WRITE_SRC', '#define PY_WRITE_SRC \\']
for i, ln in enumerate(src):
    esc = ln.replace('\\', '\\\\').replace('"', '\\"')
    out.append('    "%s\\n"%s' % (esc, ' \\' if i < len(src) - 1 else ''))
out.append('#endif')
open('py_write.h', 'w', encoding='utf-8').write('\n'.join(out) + '\n')
EOF
```

3. `make clean && make`（`doc.so` 依赖 `py_write.h`，会重编）

要从 `py_browse.h` 反解回 `.py` 再改（脚本删了但内容都在头文件里）：

```sh
python3 - <<'EOF'
src = open('py_browse.h', encoding='utf-8').read().split('\n')
out = []
for ln in src:
    s = ln.strip()
    if not s.startswith('"'): continue
    s = s[1:]
    if s.endswith('\\'): s = s[:-1].rstrip()
    if s.endswith('"'): s = s[:-1]
    s = s.replace('\\\\', '\x00').replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"')
    out.append(s.replace('\x00', '\\'))
open('browse.py', 'w', encoding='utf-8').write(''.join(out))
EOF
```

注意 `''.join` 不能写成 `'\n'.join`——每行本身就带换行符，再 join 会翻倍。

加 PPT 版式、改 Excel 样式都在这一步做——`python-pptx` / `openpyxl`
的排版能力用 C 重写不值得。

## 几个关键设计

**任务分块**（`CHUNKS[]`）

一句话里有多个动作时，按关键词在任务里的位置切成有序步骤依次执行，
后一步自动吃前一步的产出：

```
"输出表格，再转成 xlsx"
  → 第 1 步 jq.table（主表 + 各子表 CSV）
  → 第 2 步 doc.xlsx（读上一步的 CSV，打包成多 sheet）
```

加新动作在表里加一行。

**插件联动**（`PIPES[]`）

一个插件的产出是另一个插件的输入时自动接力：

```c
{"curl", "type", "json", "jq", "{\"op\":\"probe\",\"file\":\"%f\"}"}
```

curl 抓到 JSON 就自动让 jq 报一下结构（多少条、有哪些字段），非 JSON 不触发。

**插件路由**（`route_plugins()`）

按关键词只把相关的 1~3 个插件写进 prompt。没命中时全列兜底。
`AGENT_DEBUG=1` 会打印 `[路由] ...`。

**纯对话预判**（`is_pure_chat()`）

路由一个都没命中、也没有强动作词 → 不给插件表，prompt 从 1.5KB 降到 150 字节。
先跑路由再判断，否则 `帮我算一下` 这种会被误判成闲聊。

**生成必须非流式**

`llama-server` 用 `-np 1` 单 slot。流式请求会让它卡在写阻塞
（4 核满载、不出结果、后续请求全排队）。所以 `"stream":false`，
前端靠 `?job=` 轮询日志 + "运行中 XX 秒"看进度。

**停止按钮要杀进程**

前端 abort 自己的 fetch 不够，还得调 `?stop_job=1`：
读 `.running` 的 pid → SIGTERM → 2 秒不退 SIGKILL → 清 `.running`。

**输出格式**

模型必须输出单个 JSON 对象，`tool` 字段决定调哪个插件，`final` 表示直接回答。
框架侧有三道容错：多余反斜杠、数字后多余引号、截断的 JSON 补齐。
