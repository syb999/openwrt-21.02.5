#!/usr/bin/env node
'use strict';
/*
 * dss - 用 chat.deepseek.com 的网页版账号在纯终端里聊天（不装浏览器、不走付费 API）。
 *
 * 要点(都是踩过坑换来的):
 *   1) 复用站点自己的 PoW Worker（pow_worker.js + 8138 chunk + sha3 wasm），跑在
 *      **worker_thread** 里：主线程不被 wasm 暴力搜索堵住，pow 还预取。
 *   2) 长答案会被服务端/网关中途断流（实测 42KB 左右连接直接 aborted）⇒ 自动用
 *      站点的 /api/v0/chat/resume_stream 续传，续传帧里带完整快照，靠"快照比已打印
 *      的长"来只打印缺的尾巴，不会重复。
 *   3) 输出按"阵尾冲水"合批写 stdout（60ms 空闲 / 1KB / 400ms 上限）：每个 token
 *      一次 write 会在 ttyd/xterm 下掉帧。
 *   4) 一次只允许一个请求在飞（readline 的 pause() 挡不住缓冲区里的 'line'，
 *      并发起两个 SSE 会互相插队、越来越慢、最后假死）。
 *
 * 凭据: /etc/dss/cred  第1行 = bearer token, 第2行 = cookie (0600)
 * 资产: /opt/dss/{pow_worker.js,pow_chunk8138.js,pow_thread.js,sha3_wasm_bg.wasm}
 * 调试: DSS_DEBUG=1 打印 SSE 帧 / 耗时 / CPU
 */

const fs = require('fs');
const vm = require('vm');
const https = require('https');
const { Worker } = require('worker_threads');

// 令牌不放 /etc：/etc 会被 sysupgrade 备份带走，等于把凭据打进备份/固件里。
// 现在放 /root/.dss/cred（0600，不进备份）；老的 /etc/dss/cred 自动搬过来并删掉。
function credPath() {
  if (process.env.DSS_CRED) return process.env.DSS_CRED;
  const NEW = '/root/.dss/cred';
  const OLD = '/etc/dss/cred';
  try {
    if (!fs.existsSync(NEW) && fs.existsSync(OLD)) {
      fs.mkdirSync('/root/.dss', { recursive: true, mode: 0o700 });
      fs.chmodSync('/root/.dss', 0o700);
      fs.copyFileSync(OLD, NEW);
      fs.chmodSync(NEW, 0o600);
      fs.unlinkSync(OLD);
      try { fs.unlinkSync(OLD + '.prev'); } catch (e) {}
      console.error('[dss] token moved to ' + NEW + ' (out of /etc, so it stays out of sysupgrade backups)');
    }
  } catch (e) { /* ignore */ }
  return NEW;
}

const CRED = credPath();
const ASSETS = process.env.DSS_ASSETS || '/opt/dss';
const HOST = 'chat.deepseek.com';
const UA = process.env.DSS_UA ||
  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36';

const IDLE_MS = parseInt(process.env.DSS_IDLE_MS || '60', 10);
const MAX_HOLD_MS = parseInt(process.env.DSS_MAX_HOLD_MS || '400', 10);
const STALL_MS = parseInt(process.env.DSS_STALL_MS || '90000', 10);
// 两次 API 请求的最小间隔：太快会触发"消息发送过于频繁"风控
let paceMs = parseInt(process.env.DSS_PACE_MS || '3000', 10);
const PACE_MAX = parseInt(process.env.DSS_PACE_MAX || '60000', 10);
let lastReqAt = 0;
let lastPayloadChars = 0;   // 上次请求体大小
let lastAnswerChars = 0;    // 上次回答大小
let rlHits = 0;             // 连续被风控次数

// 长内容很容易触发 DeepSeek 风控：体量越大，下一次请求前等得越久
function dynamicWaitMs() {
  const size = lastPayloadChars + lastAnswerChars;
  const extra = Math.max(0, Math.min(40000, Math.round((size - 8000) / 3)));
  return paceMs + extra;
}
const MAX_RESUME = parseInt(process.env.DSS_MAX_RESUME || '4', 10);

function cred() {
  const lines = fs.readFileSync(CRED, 'utf8').split('\n');
  const token = (lines[0] || '').trim();
  const cookie = (lines[1] || '').trim();
  if (!token) throw new Error('no token in ' + CRED);
  return { token, cookie };
}

function baseHeaders(c) {
  const h = {
    'User-Agent': UA,
    'Accept': '*/*',
    'Accept-Language': 'zh-CN,zh;q=0.9,en;q=0.8',
    'Referer': 'https://chat.deepseek.com/',
    'Origin': 'https://chat.deepseek.com',
    'Authorization': 'Bearer ' + c.token,
  };
  if (c.cookie) h['Cookie'] = c.cookie;
  return h;
}

function httpJson(c, path, body, extra) {
  return new Promise(resolve => {
    const payload = body === undefined ? null : Buffer.from(JSON.stringify(body));
    const headers = Object.assign(baseHeaders(c), extra || {});
    if (payload) {
      headers['Content-Type'] = 'application/json';
      headers['Content-Length'] = payload.length;
    }
    const r = https.request({ host: HOST, path, method: 'POST', headers }, res => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', d => { buf += d; });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(buf); } catch (e) {}
        resolve({ code: res.statusCode, text: buf, j });
      });
    });
    r.setTimeout(60000, () => r.destroy(new Error('timeout')));
    r.on('error', e => resolve({ code: 0, text: 'ERR ' + e.message, j: null }));
    if (payload) r.write(payload);
    r.end();
  });
}

// ---------- output batching ----------
function makePrinter() {
  let buf = '';
  let timer = null;
  let firstAt = 0;
  function flush() {
    if (timer) { clearTimeout(timer); timer = null; }
    firstAt = 0;
    if (!buf) return;
    const out = buf;
    buf = '';
    process.stdout.write(out);
  }
  return {
    write(s) {
      buf += s;
      if (buf.length >= 1024) return flush();
      if (!firstAt) firstAt = Date.now();
      if (Date.now() - firstAt >= MAX_HOLD_MS) return flush();
      if (timer) clearTimeout(timer);
      timer = setTimeout(flush, IDLE_MS);
    },
    flush,
  };
}




function httpGetJson(c, path) {
  return new Promise(resolve => {
    const r = https.request({ host: HOST, path, method: 'GET', headers: baseHeaders(c) }, res => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', d => { buf += d; });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(buf); } catch (e) {}
        resolve({ code: res.statusCode, text: buf, j });
      });
    });
    r.setTimeout(30000, () => r.destroy(new Error('timeout')));
    r.on('error', e => resolve({ code: 0, text: 'ERR ' + e.message, j: null }));
    r.end();
  });
}


// 用户手打路径常错（大小写、漏目录）——按 basename 在常见目录里找一次
function resolvePath(input) {
  const fs2 = require('fs');
  const path2 = require('path');
  if (fs2.existsSync(input)) return { path: input, note: null };
  const base = path2.basename(input);
  const lower = base.toLowerCase();
  const dirs = [process.cwd(), '/root', '/root/zmtel_voip', '/tmp', '/tmp/dss_output', '/home', '/mnt', '/overlay'];
  for (const d of dirs) {
    try {
      for (const f of fs2.readdirSync(d)) {
        if (f.toLowerCase() === lower) {
          return { path: path2.join(d, f), note: 'fuzzy-matched ' + path2.join(d, f) };
        }
      }
    } catch (e) { /* dir not there */ }
  }
  return { path: null, note: null };
}

// ---------- 上传文件到 DeepSeek（multipart + pow） ----------

// 目录递归展开：返回 [{path, name}]，name 保留相对路径便于模型理解结构
function expandUploadTargets(target, base) {
  const fs2 = require('fs');
  const pp = require('path');
  const out = [];
  const root = base || target;
  let st;
  try { st = fs2.statSync(target); } catch (e) { return null; }

  if (!st.isDirectory()) {
    return [{ path: target, name: pp.basename(target) }];
  }

  const MAX_FILES = 80, MAX_EACH = 32 * 1024 * 1024, MAX_TOTAL = 200 * 1024 * 1024;
  let total = 0;

  (function walk(dir) {
    let ents = [];
    try { ents = fs2.readdirSync(dir, { withFileTypes: true }); } catch (e) { return; }
    ents.sort((a, b) => (a.name < b.name ? -1 : 1));
    for (const ent of ents) {
      const full = pp.join(dir, ent.name);
      if (ent.name === '.git' || ent.name === 'node_modules' || ent.name.startsWith('.DS')) continue;
      if (ent.isDirectory()) { walk(full); continue; }
      if (!ent.isFile()) continue;
      if (out.length >= MAX_FILES) return;
      let sz = 0;
      try { sz = fs2.statSync(full).size; } catch (e) { continue; }
      if (sz > MAX_EACH) continue;
      if (total + sz > MAX_TOTAL) return;
      total += sz;
      out.push({ path: full, name: pp.relative(root, full).split(pp.sep).join('/') });
    }
  })(target);

  return out;
}

async function uploadFile(c, pow, filePath, displayName) {
  const fs2 = require('fs');
  const r0 = resolvePath(filePath);
  if (!r0.path) throw new Error('找不到文件 ' + filePath + '（大小写/目录都不对；用绝对路径最稳）');
  if (r0.note) console.error('[dss] ' + r0.note);
  filePath = r0.path;
  let buf;
  try { buf = fs2.readFileSync(filePath); } catch (e) {
    throw new Error('读不到文件 ' + filePath + ': ' + e.message);
  }
  if (buf.length > 30 * 1024 * 1024) throw new Error('文件太大（>30MB）');

  const target = '/api/v0/file/upload_file';
  const powHeader = await getPow(c, pow, target);
  const name = require('path').basename(filePath);

  const fd = new FormData();
  fd.append('file', new Blob([buf]), name);

  const res = await fetch('https://chat.deepseek.com' + target, {
    method: 'POST',
    headers: Object.assign(baseHeaders(c), { 'x-ds-pow-response': powHeader }),
    body: fd,
  });
  const text = await res.text();
  let j = null;
  try { j = JSON.parse(text); } catch (e) {}
  const d = j && j.data && j.data.biz_data;
  if (res.status !== 200 || !d || !d.id) {
    throw new Error('upload failed: HTTP ' + res.status + ' ' + String(text).slice(0, 200));
  }
  // 服务端要解析文件（PENDING -> SUCCESS）；解析完才能被 completion 引用，
  // 否则会得到一个"空回答"。这里轮询 fetch_files，最多 40s。
  const t0 = Date.now();
  let status = d.status;
  while (Date.now() - t0 < 40000) {
    await new Promise(r => setTimeout(r, 1000));
    const fr = await httpGetJson(c, '/api/v0/file/fetch_files?file_ids=' + encodeURIComponent(d.id));
    const f = fr.j && fr.j.data && fr.j.data.biz_data && fr.j.data.biz_data.files && fr.j.data.biz_data.files[0];
    if (!f) continue;
    status = f.status;
    if (status === 'SUCCESS') break;
    if (status === 'FAILED' || f.error_code) {
      throw new Error('file parse failed: ' + (f.error_code || status));
    }
  }
  return { id: d.id, name: d.file_name || name, size: d.file_size || buf.length, status };
}

function logConv(who, text) {
  try {
    const fs2 = require('fs');
    fs2.mkdirSync(require('path').dirname(CONV_LOG), { recursive: true, mode: 0o700 });
    const ts = new Date().toISOString().replace('T', ' ').slice(0, 19);
    fs2.appendFileSync(CONV_LOG, '[' + ts + '] ' + who + '\n' + (text || '') + '\n\n', { mode: 0o600 });
    // 简单轮转：超过 2MB 就挪走
    try {
      if (fs2.statSync(CONV_LOG).size > 2 * 1024 * 1024) {
        fs2.renameSync(CONV_LOG, CONV_LOG + '.1');
      }
    } catch (e) {}
  } catch (e) { /* logging must never break the chat */ }
}


// 读 /etc/config/dss 里的 option（LuCI 的设置页写在这里），作为 CLI 默认值
function uciDefaults() {
  const fs2 = require('fs');
  const out = {};
  try {
    const txt = fs2.readFileSync(process.env.DSS_UCI || '/etc/config/dss', 'utf8');
    for (const line of txt.split('\n')) {
      const m = line.match(/^\s*option\s+(\w+)\s+'?"?([^'"]*)'?"?\s*$/);
      if (m) out[m[1]] = m[2];
    }
  } catch (e) { /* no uci config */ }
  return out;
}


// ================= 对话角色（LuCI 里勾选，id 必须与 luci-app-dss 的列表一致） =================
const ROLE_CATALOG = {
  embedded: { name: '嵌入式硬件开发工程师', desc: '精通 MCU/SoC 寄存器、时钟树、DMA、中断与时序，结论必须能上板验证；不确定的要标注假设。' },
  android:  { name: '安卓软件开发工程师',   desc: '熟悉 Java/Kotlin、Android SDK、Gradle、四大组件、NDK/JNI 与真机调试。' },
  cdev:     { name: 'C 语言开发工程师',     desc: '熟悉 C11、指针与内存模型、UB、ABI、链接与交叉编译；代码要能直接编译。' },
  novelist: { name: '小说作者',             desc: '擅长叙事、人物塑造、对白与节奏，文字要有画面感，避免空话套话。' },
  arch:     { name: '系统架构师',           desc: '关注模块划分、接口契约、可测试性、演进成本与失败模式。' },
  reviewer: { name: '严格代码审查者',       desc: '只挑真实缺陷：竞态、越界、泄漏、错误处理缺失、接口不一致；每条都要指出位置和修法。' },
  tester:   { name: '测试工程师',           desc: '设计可复现的用例，覆盖边界与异常路径，追求确定性验证而不是碰运气。' },
  doc:      { name: '技术文档作者',         desc: '结构清晰、示例可运行、术语一致，先给结论再给步骤。' },
  voip:     { name: 'VoIP/语音通信专家',    desc: '熟悉 SIP/RTP、PCM 时序、SLIC/Codec、抖动缓冲与回声消除。' },
  net:      { name: '网络协议专家',         desc: '熟悉 TCP/IP、抓包分析、Linux netfilter 与转发面性能。' },
  security: { name: '安全研究员',           desc: '威胁建模、攻击面分析、CVE 与最小权限；给可用 PoC 思路。' },
  ppt:      { name: 'PPT 制作专家',   desc: '熟练用 python-pptx 出成品：版式栅格、配色、字号层级、图表与图示；一页一个观点，不堆文字；脚本必须能直接跑出 .pptx。' },
  shortvideo: { name: '短视频脚本专家', desc: '3 秒钩子、分镜表、口播文案、字幕与节奏、时长控制；按平台（抖音/B站/视频号）给可直接拍摄或剪辑的方案。' },
  jsrev:    { name: 'JS 逆向专家',     desc: '浏览器调试、AST 与混淆还原（obfuscator/控制流平坦化）、webpack 拆包、运行时 hook 与补环境；只在合法授权与自有系统范围内工作。' },
  selenium: { name: 'Selenium 专家',   desc: 'WebDriver 实战：显式等待、稳定选择器、无头模式、CDP 与反检测边界、失败重试与并发控制；优先给能长期稳定运行的写法。' },
  teacher:  { name: '学科老师',             desc: '把复杂概念讲成学生能懂的话：先给直观类比，再给严格定义，最后给例题与易错点。' },
  gamedev:  { name: '游戏开发者',           desc: '熟悉玩法循环、数值平衡、状态机、存档与资源管理；给的设计要能真正落地成代码。' },
  webdev:   { name: '网站开发者',           desc: '熟悉 HTML/CSS/JS、前端框架、HTTP/API 设计、鉴权与部署；给可运行的完整文件。' },
  routerdev:{ name: '路由器/OpenWrt 开发者', desc: '熟悉 OpenWrt/LuCI、netifd、防火墙、交叉编译与嵌入式固件；改动要能在设备上复现。' },
  devops:   { name: '运维工程师',           desc: '关心部署、监控、备份、回滚与故障演练；给可执行的命令与检查项。' },
  dataeng:  { name: '数据工程师',           desc: '熟悉 SQL、ETL、数据建模与性能；给可跑的查询与表结构。' },
  ai:       { name: 'AI/算法工程师',        desc: '熟悉模型原理、训练/推理管线、评测与数据质量；区分已验证结论与猜测。' },
  translator:{ name: '技术翻译',            desc: '中英互译技术文本，术语准确、语气自然，保留原文结构与代码块。' },
  pm:       { name: '产品经理',             desc: '需求拆解、优先级排序、验收标准；拒绝含糊的需求。' },
};

// "embedded,novelist" / "嵌入式硬件开发者" / 混合 -> 提示词前缀
function roleText(spec, who) {
  if (!spec) return '';
  const parts = [];
  for (const raw of String(spec).split(/[,，;；|]/)) {
    const t = raw.trim();
    if (!t) continue;
    const r = ROLE_CATALOG[t];
    if (r) parts.push(r.name + '：' + r.desc);
    else if (t.length > 1) parts.push(t);       // 自定义角色原样使用
  }
  if (!parts.length) return '';
  const joined = parts.join(' / ').replace(/。\s*$/, '');
  return '你的角色设定（' + who + '）：' + joined +
    '。请始终以该角色的身份、知识边界和判断标准来' + (who === '提问方' ? '提问与审查' : '回答') +
    '，不要跳出角色。\n\n';
}

// ================= 脚本：检测 / 落盘 / 安全检查 / 执行 =================

let BATCH_LINES = parseInt(process.env.DSS_BATCH || '200', 10);
let DSS_VERSION = '1.0-95';
// 优先用安装包写入的版本文件，避免"HSS 常量忘了同步"这类显示不一致
try {
  const _v = require('fs').readFileSync('/usr/share/dss/version', 'utf8').trim();
  if (_v) DSS_VERSION = _v;
} catch (e) {}
const OUTDIR = process.env.DSS_OUTDIR || '/root/.dss/out';
// 任何退出都要留痕：进程被杀/崩溃时，console.out 里能看到 exit / signal
let _exitPrinted = false;
process.on('exit', (code) => {
  if (_exitPrinted) return;
  _exitPrinted = true;
  try { process.stderr.write('\n[dss] exit=' + code + '\n'); } catch (e) {}
});
['SIGTERM', 'SIGINT', 'SIGHUP'].forEach((sig) => {
  process.on(sig, () => {
    try { process.stderr.write('\n[dss] 收到 ' + sig + '，退出\n'); } catch (e) {}
    _exitPrinted = true;
    process.exit(0);
  });
});
// 只保留一个目录：/root/.dss/out（重启后还在，LuCI 的"文件下载"从这里取）
// 对话日志：给 LuCI 的"监听对话"页面 tail
const CONV_LOG = process.env.DSS_LOG || '/tmp/dss/conversation.log';
const RUN_TIMEOUT = parseInt(process.env.DSS_RUN_TIMEOUT || '60000', 10);
const MAX_SHOW = 4000;

// ================= 运行环境探测：有就执行，没有就跳过 =================
function haveCmd(name) {
  if (!name) return false;
  const fsv = require('fs');
  if (name.indexOf('/') >= 0) {
    try { fsv.accessSync(name, fsv.constants.X_OK); return true; } catch (e) { return false; }
  }
  const dirs = String(process.env.PATH || '/usr/sbin:/usr/bin:/sbin:/bin').split(':');
  for (const d of dirs) {
    if (!d) continue;
    try { fsv.accessSync(d + '/' + name, fsv.constants.X_OK); return true; } catch (e) {}
  }
  return false;
}

// 缺命令时给一句"怎么补"，别只报跳过
function cmdHint(what) {
  if (what === 'curl') return '安装：opkg update && opkg install curl（或 DSS_CURL=/path/to/curl 指定）';
  if (what === 'chrome') return '安装：opkg install chrome-glibc-runtime chrome-headless-shell（或 DSS_CHROME=/path/to/run-chrome.sh 指定）';
  if (what === 'python3') return '安装：opkg update && opkg install python3';
  if (what === 'gcc') return '安装：opkg update && opkg install gcc';
  if (what === 'sh') return '系统缺少 /bin/sh（busybox），无法执行 shell 脚本';
  return '请先安装 ' + what;
}

// ================= URL 抓取（--url / --url-dom） =================
// --url      : curl 抓原始内容（静态 HTML / .js 文件，最省事）
// --url-dom  : 用 chrome-headless-shell 渲染后再导 DOM（SPA、JS 生成的元素必须走这个）
const URL_DIR = OUTDIR + '/_url';
const CHROME_LAUNCHER = process.env.DSS_CHROME || '/opt/chrome-root/run-chrome.sh';
const UA_CHROME = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36';

function urlToName(u, rendered) {
  let host = 'page', path = '', ext = '.html';
  try {
    const o = new URL(u);
    host = o.hostname.replace(/[^A-Za-z0-9.-]/g, '_');
    path = o.pathname.replace(/^\/+/, '').replace(/[^A-Za-z0-9._-]/g, '_');
    if (/\.js(\?|$)/i.test(o.pathname)) ext = '.js';
    else if (/\.css(\?|$)/i.test(o.pathname)) ext = '.css';
    else if (/\.json(\?|$)/i.test(o.pathname)) ext = '.json';
  } catch (e) {}
  const base = (path || 'index').replace(/\.[A-Za-z0-9]+$/, '');
  const h = require('crypto').createHash('md5').update(u).digest('hex').slice(0, 6);
  return host + '_' + base.slice(0, 40) + '_' + h + (rendered ? '.dom' : '') + ext;
}

function fetchUrl(u, rendered) {
  const cp = require('child_process');
  const fsx = require('fs');
  try { fsx.mkdirSync(URL_DIR, { recursive: true, mode: 0o700 }); } catch (e) {}
  // 环境里没有对应工具就跳过这一条，不影响整轮
  if (rendered) {
    const launcher = CHROME_LAUNCHER;
    const realBin = '/opt/chrome-root/opt/chrome/chrome-headless-shell';
    if (!haveCmd(launcher) || !haveCmd(realBin)) {
      process.stderr.write('[url] 跳过 ' + u + '：环境里没有 chrome-headless-shell（' + realBin + '）\n' +
        '        ' + cmdHint('chrome') + '\n');
      return null;
    }
  } else {
    const curlBin = process.env.DSS_CURL || 'curl';
    if (!haveCmd(curlBin)) {
      process.stderr.write('[url] 跳过 ' + u + '：环境里没有 curl\n' +
        '        ' + cmdHint('curl') + '\n');
      return null;
    }
  }

  const name = urlToName(u, rendered);
  const out = URL_DIR + '/' + name;
  process.stderr.write('[url] ' + (rendered ? '渲染 ' : '抓取 ') + u + ' ... ');

  if (rendered) {
    const r = cp.spawnSync(CHROME_LAUNCHER, ['--dump-dom', u], { timeout: 180000, encoding: 'buffer' });
    const html = (r.stdout || Buffer.alloc(0)).toString('utf8').trim();
    if (r.status !== 0 || !html) {
      process.stderr.write('失败\n');
      throw new Error('Chrome 渲染失败（' + (r.status === null ? 'timeout' : 'exit=' + r.status) + '）：' +
        String(r.stderr || '').slice(-300));
    }
    fsx.writeFileSync(out, Buffer.from(html, 'utf8'), { mode: 0o600 });
    process.stderr.write('ok (' + html.length + ' 字节, 渲染后 DOM)\n');
  } else {
    // 用 buffer 存原始字节：按 utf8 字符串落盘会改变长度、破坏二进制/编码
    const r = cp.spawnSync('curl', ['-sSL', '--max-time', '40', '-A', UA_CHROME, u],
      { timeout: 50000, encoding: 'buffer' });
    const body = r.stdout || Buffer.alloc(0);
    if (r.status !== 0 || !body.length) {
      process.stderr.write('失败\n');
      throw new Error('抓取失败（' + (r.status === null ? 'timeout' : 'exit=' + r.status) + '）：' +
        String(r.stderr || '').slice(-300));
    }
    fsx.writeFileSync(out, body, { mode: 0o600 });
    process.stderr.write('ok (' + body.length + ' 字节)\n');
  }
  return out;
}

// 危险命令黑名单（宁可误杀，不可放过）
const BLOCK = [
  [/(^|[\s;&|(`])rm(\s|$)/i, 'rm'],
  [/\brmdir\b/i, 'rmdir'],
  [/\bmkfs|\bmke2fs|\bmkswap\b|\bwipefs\b/i, 'mkfs/wipefs'],
  [/\bdd\b/i, 'dd'],
  [/\bfdisk\b|\bparted\b|\bsfdisk\b|\bblockdev\b/i, 'fdisk/parted'],
  [/\bshutdown\b|\breboot\b|\bhalt\b|\bpoweroff\b|\binit\s+[06]\b/i, 'shutdown/reboot'],
  [/\bsysupgrade\b|\bfirstboot\b/i, 'sysupgrade/firstboot'],
  [/opkg\s+(remove|install|upgrade)/i, 'opkg remove/install'],
  [/\binsmod\b|\brmmod\b|\bmodprobe\b/i, 'kernel modules'],
  [/\biptables\s+(-F|-X|-P)|\bnft\s+(flush|delete)/i, 'firewall flush'],
  [/\buseradd\b|\buserdel\b|\bpasswd\b|\bchpasswd\b|\bcrontab\b/i, 'accounts/cron'],
  [/\bkill(all)?\s+(-9\s+)?1\b/i, 'kill init'],
  [/\/dev\/(sd|mmcblk|mtd|nvram)/i, 'raw device'],
  [/\bchmod\s+(-R\s+)?777\s+\//i, 'chmod 777 /'],
  [/\bmv\s+\/(\s|$)/i, 'mv /'],
  [/\bcp\s+[^|;]*\s\/etc\//i, 'write /etc'],
  [/\b(echo|printf|tee|sed\s+-i|cat\s*>)\b[^\n]*>\s*\/etc\//i, 'write /etc'],
  [/\buci\s+(set|commit|delete|add|rename)\b/i, 'uci change'],
  [/\/etc\/init\.d\//i, 'init.d control'],
  [/\b(curl|wget)\b[^|;]*\|\s*(ba|a|z)?sh\b/i, 'curl|sh'],
  [/:\(\)\s*\{/, 'fork bomb'],
  [/\btruncate\b[^\n]*\/dev/i, 'truncate /dev'],
  [/\bchroot\b|\bmount\b|\bumount\b/i, 'mount/chroot'],
  // python 侧：能绕过 shell 黑名单的调用
  [/\bos\.(system|popen|remove|unlink|rmdir|rename|execv|spawn\w*)\s*\(/i, 'python os.system/remove'],
  [/\bshutil\.(rmtree|move|copy\w*)\s*\(/i, 'python shutil.rmtree'],
  [/\bsubprocess\b/i, 'python subprocess'],
  [/\b(chmod|chown)\s*\(/i, 'python chmod/chown'],
  [/open\s*\(\s*['"]\/etc\//i, 'python write /etc'],
];

function checkScript(src) {
  if (!src || !src.trim()) return 'empty script';
  if (src.length > 20000) return 'script too long';
  for (const [re, why] of BLOCK) {
    if (re.test(src)) return 'blocked dangerous command: ' + why;
  }
  return null;
}

// 把令牌从任何要展示/回传的文本里抹掉
function redact(text) {
  let out = String(text || '');
  try {
    const tok = fs.readFileSync(CRED, 'utf8').split('\n')[0].trim();
    if (tok && tok.length > 20) out = out.split(tok).join('[REDACTED]');
  } catch (e) {}
  return out;
}

// 从回答里挑出可执行脚本

// 从代码块头部猜文件名：优先用模型自己写的（/* foo.c */、build: gcc foo.c、#ifndef FOO_H）
function guessFileName(code, lang) {
  const head = String(code).split('\n').slice(0, 10).join('\n');
  const ext = (lang === 'c') ? 'c' : (lang === 'py' ? 'py' : 'sh');
  // 1) 注释里出现 xxx.c / xxx.h / xxx.sh / xxx.py
  let m = head.match(/([A-Za-z0-9][A-Za-z0-9_.-]*\.(?:c|h|cpp|cc|sh|bash|py))\b/);
  if (m) return m[1];
  // 2) #ifndef FOO_BAR_H -> foo_bar.h
  m = code.match(/^#ifndef\s+([A-Z0-9_]+)_H\b/m);
  if (m) return m[1].toLowerCase().replace(/_/g, '_') + '.h';
  // 3) sh 里的 shebang 注释
  m = head.match(/^#\s*([A-Za-z0-9_.-]+\.sh)\b/m);
  if (m) return m[1];
  return null;
}

function parseBlocks(text) {
  const out = [];
  const src = String(text || '');
  // 围栏信息串里也可能带文件名（```c my_strlen.c）
  const re = /```([A-Za-z0-9_+.-]*)[ \t]*([^\n]*)\n([\s\S]*?)```/g;
  let m;
  while ((m = re.exec(src)) !== null) {
    const lang = (m[1] || '').toLowerCase();
    const info = (m[2] || '').trim();
    const code = m[3];
    // Kimi/DeepSeek 常在围栏上一行写 **my_strlen.c** 或 ### main.c —— 直接采用这个名字
    let name = '';
    const fm = info.match(/([A-Za-z0-9][A-Za-z0-9_.-]*\.[A-Za-z0-9]{1,5})$/);
    if (fm) name = fm[1];
    if (!name) {
      const prev = src.slice(Math.max(0, m.index - 200), m.index).split('\n')
        .map(x => x.trim()).filter(Boolean);
      for (let i = prev.length - 1; i >= 0 && i >= prev.length - 3; i--) {
        const ln = prev[i].replace(/^#+\s*/, '').replace(/\*\*/g, '').replace(/`/g, '')
          .replace(/[:：]\s*$/, '').trim();
        const nm = ln.match(/^([A-Za-z0-9][A-Za-z0-9_.-]*\.[A-Za-z0-9]{1,5})$/);
        if (nm) { name = nm[1]; break; }
      }
    }
    const isC = ['c', 'h', 'cpp', 'cc'].includes(lang);
    const isShell = ['sh', 'bash', 'shell', 'zsh', 'ash', 'console', 'shell-session'].includes(lang) ||
      /^\s*#!\s*\/.*\b(ba|a|z)?sh\b/.test(code);
    const isPy = ['python', 'python3', 'py'].includes(lang) ||
      /^\s*#!\s*\/.*python/.test(code);
    if (isC) out.push({ lang: 'c', code, name });
    else if (isShell) out.push({ lang: 'sh', code, name });
    else if (isPy) out.push({ lang: 'py', code, name });
  }
  return out;
}

// 轮次标记用的注释
function commentFor(name, label) {
  const ext = (String(name).match(/\.([A-Za-z0-9]+)$/) || [])[1];
  const e = (ext || '').toLowerCase();
  if (['sh', 'bash', 'py', 'yaml', 'yml', 'conf', 'ini'].indexOf(e) >= 0) {
    return '# ===== ' + label + ' =====';
  }
  if (['c', 'h', 'cpp', 'cc', 'hpp', 'java', 'kt', 'js', 'ts', 'go', 'rs', 'php'].indexOf(e) >= 0) {
    return '/* ===== ' + label + ' ===== */';
  }
  return '// ===== ' + label + ' =====';
}

// 判断这段代码是不是"完整文件"（有文件头注释 / include / 头文件保护 / shebang）
function looksComplete(code) {
  const head = String(code).slice(0, 600);
  if (/^#!\s*\//.test(String(code).trim())) return true;
  if (/#ifndef\s+[A-Z0-9_]+_H\b/.test(head)) return true;
  if (/^\s*\/\*[\s\S]{0,300}?\*\//.test(head) && /#include\s*</.test(head)) return true;
  if (/^\s*(package|import)\s+[\w.]+/m.test(head)) return true;
  if (/^\s*<\?php/.test(head)) return true;
  return false;
}

let LAST_FILE = null;   // 上一次落盘的文件名（跨轮次片段续写用）

function saveScripts(answerText, label) {
  label = label || '新的回答';
  const blocks = parseBlocks(answerText);
  // 按"解析出来的文件名"归并：同一份文件的多段拼起来；不同文件即使同语言也分开
  const byName = new Map();
  const order = [];
  let seq = 0;
  let lastName = null;
  for (const b of blocks) {
    seq++;
    let nm = b.name || guessFileName(b.code, b.lang);
    if (!nm) {
      // 没有文件名：如果上一块是同一语言，就当作它的续写（分批时模型常常不再重复文件名）
      if (lastName && byName.get(lastName) && byName.get(lastName).lang === b.lang) {
        byName.get(lastName).chunks.push(b.code);
        continue;
      }
      // 跨轮次：上一轮写过这个语言的文件，就当作它的续写
      const langOfLast = LAST_FILE
        ? (LAST_FILE.match(/\.py$/) ? 'py' : (LAST_FILE.match(/\.c$|\.h$/) ? 'c' : 'sh'))
        : null;
      if (LAST_FILE && LAST_FILE.match(/\.[A-Za-z0-9]+$/) && b.lang === langOfLast) {
        lastName = LAST_FILE;
        if (!byName.has(LAST_FILE)) { byName.set(LAST_FILE, { lang: b.lang, chunks: [], name: LAST_FILE }); order.push(LAST_FILE); }
        byName.get(LAST_FILE).chunks.push(b.code);
        continue;
      }
      nm = 'code_' + seq + ((b.lang === 'py') ? '.py' : (b.lang === 'c' ? '.c' : '.sh'));
    }
    nm = require('path').basename(nm).replace(/[^A-Za-z0-9._-]/g, '_');
    if (!byName.has(nm)) { byName.set(nm, { lang: b.lang, chunks: [], name: nm }); order.push(nm); }
    byName.get(nm).chunks.push(b.code);
    lastName = nm;
  }
  if (!order.length) return [];
  try {
    fs.mkdirSync(OUTDIR, { recursive: true, mode: 0o700 });
    fs.chmodSync(OUTDIR, 0o700);
  } catch (e) {}

  const saved = [];
  const ts = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 14);

  // duet 时给文件加 a_/b_ 前缀；同时要把脚本里对这些文件名的引用一起改掉，
  // 否则 b_run.sh 里写的是 run.sh / write_p.py，实际文件叫 b_run.sh / b_write_p.py，必然找不到
  const renameMap = new Map();
  if (SAVE_PREFIX) {
    order.forEach((name) => {
      const info = byName.get(name);
      const ext = (info.lang === 'py') ? '.py' : (info.lang === 'c' ? '.c' : '.sh');
      let base = info.name;
      if (!/\.[A-Za-z0-9]+$/.test(base)) base += ext;
      if (base.indexOf(SAVE_PREFIX) !== 0) renameMap.set(base, SAVE_PREFIX + base);
    });
  }

  order.forEach((name, i) => {
    const info = byName.get(name);
    const ext = (info.lang === 'py') ? '.py' : (info.lang === 'c' ? '.c' : '.sh');
    let base = info.name;
    if (!/\.[A-Za-z0-9]+$/.test(base)) base += ext;
    if (renameMap.has(base)) base = renameMap.get(base);
    const path = OUTDIR + '/' + base;

    // 同一条回答里的多段：第二段起加"（续）"标记，合并成一个文件
    let body = info.chunks.map((c, k) => (k === 0) ? c
      : commentFor(base, label + '（续）') + '\n' + c).join('\n\n');
    if (renameMap.size) {
      // 单遍正则替换：长名优先 + 不回头再匹配已替换的结果
      // （逐个 split/join 会把 test_write_hello.py 先变 b_test_write_hello.py，再被 write_hello.py 规则二次加前缀）
      const keys = [...renameMap.keys()]
        .filter(k => renameMap.get(k) !== k)
        .sort((a2, b2) => b2.length - a2.length);
      const esc = (t) => t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
      if (keys.length) {
        body = body.replace(new RegExp(keys.map(esc).join('|'), 'g'), (m2) => renameMap.get(m2));
      }
      // 模块名（不带扩展名）：import write_hello / from write_hello import x
      const modMap = new Map();
      for (const [o, n] of renameMap) {
        if (o === n) continue;
        const os = o.replace(/\.[^.]+$/, ''), ns = n.replace(/\.[^.]+$/, '');
        if (os !== ns) modMap.set(os, ns);
      }
      if (modMap.size) {
        body = body.replace(/\bimport\s+([A-Za-z_][\w.]*(?:\s*,\s*[A-Za-z_][\w.]*)*)/g, (all, list) => {
          const parts = list.split(',').map(x => x.trim()).map(mod => {
            const first = mod.split('.')[0];
            return modMap.has(first) ? modMap.get(first) + mod.slice(first.length) : mod;
          });
          return 'import ' + parts.join(', ');
        });
        body = body.replace(/\b(from\s+)([A-Za-z_][\w.]*)(\s+import\b)/g, (all, kw, mod, tail) => {
          const first = mod.split('.')[0];
          return modMap.has(first) ? kw + modMap.get(first) + mod.slice(first.length) + tail : all;
        });
        // 模块属性访问：import 改过了，write_hello.hello() 也得跟着改
        for (const [o, n] of modMap) {
          body = body.replace(new RegExp('\\b' + esc(o) + '\\.', 'g'), n + '.');
        }
      }
    }

    let existing = '';
    try { existing = fs.readFileSync(path, 'utf8'); } catch (e) {}

    let mode = 'new';
    let outText = body;
    if (existing) {
      if (existing.indexOf(body.trim()) >= 0) {
        mode = 'dup';
        return;                                   // 内容已在文件里，不动
      }
      if (looksComplete(body)) {
        mode = 'replaced';                        // 完整文件：替换（迭代修代码）
      } else {
        // 片段：追加，并标出这是"第几轮回答"
        mode = 'appended';
        outText = existing.replace(/\s+$/, '') + '\n\n' + commentFor(base, label) + '\n' + body + '\n';
      }
    }
    try {
      fs.writeFileSync(path, outText, { mode: 0o600 });
      fs.chmodSync(path, 0o600);
      LAST_FILE = base;
      saved.push({ path, lang: info.lang, lines: outText.split('\n').length, mode });
    } catch (e) { /* ignore */ }
  });
  return saved;
}

const CC = process.env.DSS_CC || '/usr/bin/gcc';
const CFLAGS = ['-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-Wno-unused-variable', '-Wno-unused-but-set-variable',
  '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare'];

function interpreterFor(path) {
  if (path.endsWith('.c')) return 'cc';
  if (path.endsWith('.py')) {
    for (const py of ['/usr/bin/python3', '/usr/bin/python']) {
      try { fs.accessSync(py, fs.constants.X_OK); return py; } catch (e) {}
    }
    return null;
  }
  return '/bin/sh';
}

function runScript(path) {
  return new Promise(resolve => {
    const cwd = OUTDIR;
    const env = {
      PATH: '/usr/sbin:/usr/bin:/sbin:/bin',
      HOME: cwd,
      TMPDIR: '/tmp',
      TERM: 'dumb',
      LANG: 'C',
    };
    let done = false;
    let out = '';
    const interp = interpreterFor(path);
    if (!interp) {
      return resolve({ code: -1, out: 'no python3 interpreter on this box (/usr/bin/python3 missing)' });
    }
    if (interp === 'cc') {
      // 先 gcc 编译，编译错误原样返回（可以 !run N send 回灌给模型修）
      const cp = require('child_process');
      const bin = path.replace(/\.c$/, '');
      const cbuild = cp.spawnSync(CC, CFLAGS.concat(['-o', bin, path]),
        { cwd, env, timeout: 120000, encoding: 'utf8' });
      if (cbuild.status !== 0) {
        const msg = 'compile failed (' + (cbuild.status === null ? 'timeout/error' : 'exit ' + cbuild.status) + '):\n' +
          ((cbuild.stderr || '') + (cbuild.stdout || '')).slice(0, 8000);
        return resolve({ code: cbuild.status === null ? -1 : cbuild.status, out: msg });
      }
      const child = cp.spawn(bin, [], { cwd, env });
      let fin = false;
      const cap2 = d => { if (out.length < 200000) out += d.toString(); };
      child.stdout.on('data', cap2);
      child.stderr.on('data', cap2);
      const t2 = setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) {} }, RUN_TIMEOUT);
      const fini = (code, extra) => {
        if (fin) return;
        fin = true;
        clearTimeout(t2);
        resolve({ code: code, out: redact(out + (extra || '')) });
      };
      child.on('close', code => fini(code));
      child.on('error', e => fini(-1, 'run error: ' + e.message));
      return;
    }
    const child = require('child_process').spawn(interp, [path], { cwd, env });
    const timer = setTimeout(() => {
      if (!done) { try { child.kill('SIGKILL'); } catch (e) {} }
    }, RUN_TIMEOUT);
    const cap = d => { if (out.length < 200000) out += d.toString(); };
    child.stdout.on('data', cap);
    child.stderr.on('data', cap);
    child.on('close', code => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolve({ code, out: redact(out) });
    });
    child.on('error', e => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolve({ code: -1, out: 'spawn error: ' + e.message });
    });
  });
}

function showOutput(res) {
  const t = res.out.length > MAX_SHOW
    ? res.out.slice(0, MAX_SHOW) + '\n...(truncated, full output saved next to the script)'
    : res.out;
  process.stdout.write(t.endsWith('\n') ? t : t + '\n');
  process.stdout.write('[exit ' + res.code + ']\n');
}

// ---------- pow ----------
function makePoller() {
  let worker = null;
  let readyP = null;
  let seq = 0;
  const waiting = new Map();

  function onMsg(m) {
    if (m.type === 'ready') return;
    const w = waiting.get(m.id);
    if (!w) return;
    waiting.delete(m.id);
    if (m.error) w.reject(new Error(m.error)); else w.resolve(m.answer);
  }

  function start() {
    worker = new Worker(ASSETS + '/pow_thread.js', {
      env: Object.assign({}, process.env, { DSS_ASSETS: ASSETS }),
    });
    readyP = new Promise((resolve, reject) => {
      worker.on('message', m => { if (m.type === 'ready') resolve(); onMsg(m); });
      worker.on('error', reject);
    });
    worker.on('error', e => {
      for (const w of waiting.values()) w.reject(e);
      waiting.clear();
    });
    return readyP;
  }

  return {
    start,
    solve(challenge) {
      return new Promise((resolve, reject) => {
        if (!worker) start();
        const id = ++seq;
        waiting.set(id, { resolve, reject });
        readyP.then(() => worker.postMessage({ type: 'solve', id, challenge }), e => reject(e));
      });
    },
  };
}

// 按后端选上传通道：deepseek -> /api/v0/file/upload_file；kimi -> /apiv2-files/file/upload
async function uploadAttachments(state, c, pow, backend, files) {
  for (const fp of files) {
    const targets = expandUploadTargets(fp) || [];
    if (!targets.length) { console.error('[dss] nothing to upload: ' + fp); continue; }
    if (targets.length > 1) console.error('[dss] ' + fp + ' -> ' + targets.length + ' file(s)');
    for (const t of targets) {
      process.stdout.write('[dss] uploading ' + t.name + ' (' + backend + ') ... ');
      try {
        if (['api', 'glm', 'openai', 'doubao', 'wenxin'].indexOf(backend) >= 0) {
          const err = new Error('后端 ' + backend + ' 不支持附件上传（GLM-4-Flash 是纯文本接口）。' +
            '要么去掉 --file/-a/-b，要么把这一侧换成 deepseek');
          err.fatalUpload = true;
          throw err;
        }
        if (backend === 'kimi') {
          const f = await kimiUploadFile(t.path, t.name);
          state.kimiFiles = state.kimiFiles || [];
          state.kimiFiles.push(f);
          console.log('ok (kimi ' + f.size + ' bytes)');
        } else {
          const f = await uploadFile(c, pow, t.path, t.name);
          state.fileIds.push(f.id);
          console.log('ok (' + f.size + ' bytes, ' + f.status + ')');
        }
      } catch (e) {
        console.log('failed: ' + e.message);
        if (e.fatalUpload) throw e;
      }
    }
  }
}

async function waitReady(p, ms) {
  await Promise.race([
    p.start(),
    new Promise((_, rej) => setTimeout(() => rej(new Error('pow thread init timeout')), ms || 10000)),
  ]);
}

const powCache = new Map();   // target_path -> {header, expireAt}
let powInflight = new Map();  // target_path -> Promise

async function fetchPow(c, pow, targetPath) {
  const chalRes = await httpJson(c, '/api/v0/chat/create_pow_challenge', { target_path: targetPath });
  const chal = chalRes.j && chalRes.j.data && chalRes.j.data.biz_data && chalRes.j.data.biz_data.challenge;
  if (!chal) {
    const t = String(chalRes.text || '');
    // 40003 / Authorization Failed = 网页令牌过期，不是网络问题，重试没用
    if (/40003|Authorization Failed|invalid token/i.test(t)) {
      const e = new Error('DeepSeek 网页令牌失效（40003）—— 打开 http://<盒子IP>:88/token ' +
        '登录 chat.deepseek.com 点一下书签刷新，然后重跑这条命令');
      e.noRetry = true;
      throw e;
    }
    throw new Error('challenge failed: ' + chalRes.code + ' ' + t.slice(0, 200));
  }

  const forWorker = Object.assign({}, chal, {
    expireAt: chal.expire_at !== undefined ? chal.expire_at : chal.expireAt,
    expireAfter: chal.expire_after !== undefined ? chal.expire_after : chal.expireAfter,
  });
  const ans = await pow.solve(forWorker);

  const header = Buffer.from(JSON.stringify({
    algorithm: ans.algorithm || chal.algorithm,
    challenge: ans.challenge || chal.challenge,
    salt: ans.salt || chal.salt,
    answer: ans.answer,
    signature: ans.signature || chal.signature,
    target_path: targetPath,
  })).toString('base64');

  const exp = chal.expire_at !== undefined ? chal.expire_at : chal.expireAt;
  return { header, expireAt: exp || (Date.now() + 120000) };
}

function fetchPowOnce(c, pow, targetPath) {
  if (!powInflight.has(targetPath)) {
    const p = fetchPow(c, pow, targetPath).finally(() => powInflight.delete(targetPath));
    powInflight.set(targetPath, p);
  }
  return powInflight.get(targetPath);
}

async function getPow(c, pow, targetPath) {
  const cached = powCache.get(targetPath);
  if (cached && cached.expireAt - Date.now() > 20000) {
    powCache.delete(targetPath);       // 一份 pow 只能用一次
    return cached.header;
  }
  const r = await fetchPowOnce(c, pow, targetPath);
  return r.header;
}

// 答完之后在后台把下一份(给 completion 用的) pow 算好
function prefetchPow(c, pow) {
  const key = '/api/v0/chat/completion';
  if (powCache.has(key)) return;
  setTimeout(async () => {
    if (powCache.has(key)) return;
    try {
      const r = await fetchPowOnce(c, pow, key);
      if (!powCache.has(key)) powCache.set(key, r);
    } catch (e) { /* ignore */ }
  }, 500);
}

// ---------- SSE ----------
// 返回 {finished, msgId}。连接被中途断掉时 finished=false（好走续传）。
function stream(c, pow, path, body, targetPath, cur, printer) {
  return new Promise((resolve, reject) => {
    let buf = '';
    let firstToken = 0;
    let lastEvt = Date.now();
    let done = false;
    const t0 = Date.now();
    const cpu0 = process.cpuUsage();

    const wd = setInterval(() => {
      if (Date.now() - lastEvt > STALL_MS) {
        if (!done) { done = true; clearInterval(wd); resolve({ finished: false, msgId: cur.msgId }); }
      }
    }, 5000);
    const end = (finished, err) => {
      if (done) return;
      done = true;
      clearInterval(wd);
      if (err) reject(err); else resolve({ finished, msgId: cur.msgId });
    };

    // 只有 completion 需要 pow；resume_stream 的 target_path 不在白名单里
    // (create_pow_challenge 会回 INVALID_TARGET_PATH)，续传直接发就行
    const needPow = targetPath === '/api/v0/chat/completion';
    const doReq = () => {
      lastReqAt = Date.now();
      (needPow ? getPow(c, pow, targetPath) : Promise.resolve(null)).then(powHeader => {
      const payload = Buffer.from(JSON.stringify(body));
      lastPayloadChars = payload.length;
      const headers = Object.assign(baseHeaders(c), {
        'Content-Type': 'application/json',
        'Content-Length': payload.length,
      });
      if (powHeader) headers['x-ds-pow-response'] = powHeader;
      const r = https.request({ host: HOST, path, method: 'POST', headers }, res => {
        if (res.statusCode !== 200) {
          let t = '';
          res.setEncoding('utf8');
          res.on('data', d => { t += d; });
          res.on('end', () => {
            const waf = /<html|Request Blocked|Rate Limit|too many/i.test(t);
            const e = new Error(path + ' HTTP ' + res.statusCode +
              (waf ? ' —— 被 WAF 限流/拦截了' : ': ' + t.slice(0, 200)));
            if (res.statusCode === 429 || res.statusCode === 403 || waf || looksRateLimited(t)) e.rateLimited = true;
            end(false, e);
          });
          return;
        }
        res.setEncoding('utf8');
        res.on('data', chunk => {
          buf += chunk;
          let i;
          while ((i = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, i).trim();
            buf = buf.slice(i + 1);
            if (process.env.DSS_RAW) process.stderr.write('[raw] ' + line.slice(0, 300) + '\n');
            if (!line.startsWith('data:')) continue;
            const p = line.slice(5).trim();
            if (process.env.DSS_DEBUG) console.error('[sse] ' + p.slice(0, 200));
            if (!p || p === '[DONE]') continue;
            let j;
            try { j = JSON.parse(p); } catch (e) { continue; }

            // 服务端 hint 帧：{"type":"error","content":"Messages too frequent...","finish_reason":"rate_limit_reached"}
            if (j.type === 'error') {
              const txt = String(j.content || '');
              const fr = String(j.finish_reason || '');
              const e = new Error('server: ' + txt.slice(0, 120) + (fr ? ' (' + fr + ')' : ''));
              if (/rate_limit/i.test(fr) || looksRateLimited(txt) || /too frequent/i.test(txt)) e.rateLimited = true;
              if (process.env.DSS_DEBUG || process.env.DSS_RAW) console.error('[dss] server hint: ' + txt + ' / ' + fr);
              end(false, e);
              return;
            }

            // 其它错误帧：{"code":x,"msg":"..."} 或 {"data":{"biz_code":7,"biz_msg":"rate limit reached"}}
            const errMsg = (typeof j.msg === 'string' && j.msg)
              || (j.data && typeof j.data.biz_msg === 'string' && j.data.biz_msg)
              || (j.error && (j.error.message || j.error)) || '';
            const bizCode = (j.data && typeof j.data.biz_code === 'number') ? j.data.biz_code
                          : (typeof j.code === 'number' ? j.code : 0);
            if (!j.v && errMsg && (bizCode !== 0 || looksRateLimited(String(errMsg)))) {
              const e = new Error('server: ' + String(errMsg).slice(0, 120) + ' (code=' + bizCode + ')');
              if (looksRateLimited(String(errMsg)) || bizCode === 7 || bizCode === 40003 || bizCode === 429) {
                e.rateLimited = true;
              }
              end(false, e);
              return;
            }

            lastEvt = Date.now();
            if (!firstToken && typeof j.v === 'string') firstToken = Date.now();

            // (a) 完整快照：只打印比已输出更长的尾巴（续传时不会重复）
            if (j.v && typeof j.v === 'object' && j.v.response) {
              const resp = j.v.response;
              if (resp.message_id) cur.msgId = resp.message_id;
              let snap = '';
              if (Array.isArray(resp.fragments)) {
                snap = resp.fragments
                  .filter(f => !f.type || f.type === 'RESPONSE')
                  .map(f => f.content || '').join('');
              } else if (typeof resp.content === 'string') {
                snap = resp.content;
              }
              if (snap && snap.length > cur.acc.length && snap.slice(0, cur.acc.length) === cur.acc) {
                printer.write(snap.slice(cur.acc.length));
                cur.acc = snap;
              }
            }
            // (b) 增量
            if (typeof j.v === 'string' && (!j.p || /\/content$/.test(j.p))) {
              cur.acc += j.v;
              printer.write(j.v);
            }
            // 测试用：DSS_DROP_AFTER=<字符数> 时主动掐断连接，验证断流恢复路径
            if (process.env.DSS_DROP_AFTER && !stream._dropped &&
                cur.acc.length >= parseInt(process.env.DSS_DROP_AFTER, 10)) {
              stream._dropped = true;
              delete process.env.DSS_DROP_AFTER;   // 只掐一次，别把续写也掐了
              console.error('[test] dropping connection at ' + cur.acc.length + ' chars');
              res.destroy();
              return;
            }
            if (j.p === 'response/status' && j.v === 'FINISHED') {
              printer.flush();
              if (process.env.DSS_DEBUG) {
                const cpu = process.cpuUsage(cpu0);
                console.error('[t] ' + path + ' first-token=' + (firstToken ? firstToken - t0 : 0) +
                  ' stream-cpu=' + Math.round((cpu.user + cpu.system) / 1000) + 'ms chars=' + cur.acc.length);
              }
              end(true);
              return;
            }
          }
        });
        res.on('end', () => {
          printer.flush();
          if (process.env.DSS_DEBUG) console.error('[stream] end status=' + res.statusCode + ' complete=' + res.complete);
          end(false);
        });
        res.on('aborted', () => {
          if (process.env.DSS_DEBUG) console.error('[stream] aborted after=' + (Date.now() - t0) + 'ms chars=' + cur.acc.length);
          printer.flush();
          end(false);
        });
        res.on('error', e => {
          if (process.env.DSS_DEBUG) console.error('[stream] error ' + e.message);
          end(false);
        });
      });
      r.setTimeout(0);
      // 连接被 RST/FIN 掉是常态（长答案必被断），当成"没说完"去续传，不要往上抛
      r.on('error', e => {
        if (process.env.DSS_DEBUG) console.error('[stream] req error ' + e.message);
        printer.flush();
        end(false);
      });
      r.write(payload);
      r.end();
      }).catch(e => end(false, e));
    };
    const target = dynamicWaitMs();
    const waitMs = target - (Date.now() - lastReqAt);
    if (waitMs > 1500) {
      process.stderr.write('[throttle] wait ' + Math.round(waitMs / 1000) + 's (last req ' +
        Math.round(lastPayloadChars / 1024) + 'KB, last answer ' + Math.round(lastAnswerChars / 1024) + 'KB)\n');
    }
    if (waitMs > 0) setTimeout(doReq, waitMs); else doReq();
  });
}


// 断流后从服务端历史里把这条消息的完整内容捞回来（GET，比再发一次 completion 轻）
function historyMessage(c, sessionId, msgId) {
  return new Promise(resolve => {
    const path = '/api/v0/chat/history_messages?chat_session_id=' + encodeURIComponent(sessionId);
    const r = https.request({ host: HOST, path, method: 'GET', headers: baseHeaders(c) }, res => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', d => { buf += d; });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(buf); } catch (e) {}
        const d = j && j.data && j.data.biz_data;
        const list = (d && d.chat_messages) || [];
        const m = list.find(x => x && x.message_id === msgId);
        resolve(m ? { content: m.content || '', status: m.status || '' } : null);
      });
    });
    r.setTimeout(30000, () => r.destroy(new Error('timeout')));
    r.on('error', () => resolve(null));
    r.end();
  });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));


// token 过期时等着"点书签"把新令牌写回来：轮询凭据文件的 mtime，变了就重试
function waitForCredChange(timeoutMs) {
  return new Promise(resolve => {
    let base = 0;
    try { base = fs.statSync(CRED).mtimeMs; } catch (e) {}
    const t0 = Date.now();
    const iv = setInterval(() => {
      let now = 0;
      try { now = fs.statSync(CRED).mtimeMs; } catch (e) {}
      if (now && now !== base) { clearInterval(iv); resolve(true); }
      else if (Date.now() - t0 > timeoutMs) { clearInterval(iv); resolve(false); }
    }, 2000);
  });
}

async function session(c) {
  await requestGate();
  return httpJson(c, '/api/v0/chat_session/create', { character_id: null }).then(r => {
    const d = r.j && r.j.data && r.j.data.biz_data;
    return (d && d.chat_session && d.chat_session.id) || null;
  });
}

async function ask(c, pow, prompt, state, printer) {
  await requestGate();
  logConv('USER', prompt);
  const cur = { acc: '', msgId: state.parentId };
  const tStart = Date.now();

  const body = {
    chat_session_id: state.sessionId || null,
    parent_message_id: state.parentId || null,
    prompt,
    // 附件只在会话的第一次提问时送；之后会话自己记着，不必每轮重传
    ref_file_ids: (state.filesSent ? [] : (state.fileIds || [])).slice(),
    thinking_enabled: false,
    search_enabled: !!state.search,
  };

  let r = await stream(c, pow, '/api/v0/chat/completion', body,
    '/api/v0/chat/completion', cur, printer);

  // 断流实测：服务端一旦发现客户端断开就停止生成，历史里这条消息是空的（len=0 /
  // FINISHED）⇒ 捞不回来。唯一可行的是带着已输出内容的尾巴让它"接着写"（只接一次）。
  let cont = 0;
  while (!r.finished && cur.acc.length > 60 && cont < 3) {
    cont++;
    const tail = cur.acc.slice(-1500);
    printer.flush();
    process.stderr.write('\n…(连接在 ' + Math.round((Date.now() - tStart) / 1000) + 's 被中断，自动接着写 ' + cont + '/3)\n');
    r = await stream(c, pow, '/api/v0/chat/completion', {
      chat_session_id: state.sessionId,
      parent_message_id: null,
      prompt: '（系统提示：你上一条回答因为网络中断被截断了。下面是你已经写出的最后一段，' +
        '请直接从截断处继续写完，不要重复已输出的内容，也不要加任何开场白；' +
        '如果是代码就接着写代码，保持可编译）\n<<<TAIL\n' + tail + '\nTAIL>>>',
      ref_file_ids: [],   // 续写不再重复带附件
      thinking_enabled: false,
      search_enabled: !!state.search,
    }, '/api/v0/chat/completion', cur, printer);
  }

  printer.flush();
  logConv('DEEPSEEK', cur.acc);
  // 非代码模式：每条回答都扫一遍代码块；代码模式留给 askCode 在整轮（含续批）结束后统一落盘
  try {
    const savedNow = CODE_MODE ? [] : saveScripts(cur.acc, CUR_LABEL);
    if (savedNow.length) {
      lastSaved = savedNow;
      maybeAutorun(savedNow);
      process.stderr.write('[dss] ' + savedNow.length + ' code file(s) saved: ' +
        savedNow.map(x => require('path').basename(x.path)).join(', ') + '\n');
    }
  } catch (e) { /* never break the chat */ }
  if (r.msgId) state.parentId = r.msgId;
  if ((state.fileIds || []).length) state.filesSent = true;
  if (!r.finished) {
    printer.flush();
    process.stderr.write('\n…(响应被中断，以上是已收到的部分)\n');
    state.parentId = null;
    return cur.acc;
  }
  return cur.acc;
}




const RATE_RE = /(发送过于频繁|请求过于频繁|过于频繁|操作频繁|稍后重试|请稍候|系统繁忙|服务繁忙|请求受限|rate limit|too many requests|try again later|slow down)/i;

function looksRateLimited(text) {
  if (!text) return false;
  const t = String(text).trim();
  if (t.length > 400) return false;              // 长回答里出现这些词不算
  return RATE_RE.test(t);
}

async function backoff(why) {
  rlHits++;
  const SCHEDULE = process.env.DSS_RL_SCHEDULE
    ? process.env.DSS_RL_SCHEDULE.split(',').map(x => Math.max(1, parseInt(x, 10) || 1))
    : [300, 600, 1800];   // 5 分钟 -> 10 分钟 -> 30 分钟（之后一直是 30 分钟）
  const mins = SCHEDULE[Math.min(rlHits - 1, SCHEDULE.length - 1)];
  const wait = mins * 1000;
  process.stderr.write('\n[rate-limited #' + rlHits + '] ' + (why || '') +
    ' -> 被限流，第 ' + rlHits + ' 档退避，等 ' + Math.round(wait / 60000) +
    ' 分钟（总计时长按档位递增 5→10→30 分钟；Ctrl-C 可中断）\n');
  const t0 = Date.now();
  while (Date.now() - t0 < wait) {
    const left = Math.ceil((wait - (Date.now() - t0)) / 1000);
    await new Promise(r => setTimeout(r, Math.min(30000, left * 1000)));
    const rest = Math.ceil((wait - (Date.now() - t0)) / 60000);
    if (rest > 0) process.stderr.write('[wait] 还剩 ' + rest + ' 分钟…\n');
  }
  // 这一轮会话里把基础间隔也抬高（成功后不会马上降回来，避免反复触发）
  paceMs = Math.min(60000, Math.max(paceMs, wait / 4));
  lastPayloadChars = 0; lastAnswerChars = 0;
}

// 空回答重试：服务端偶发（限流/风控）会给 200 但内容为空
let emptyStreak = 0;
let CODE_MODE = false;
let AUTORUN = false;
let AUTORUN_FAILS = [];     // autorun 失败清单，逐个回炉
let SAVE_PREFIX = '';      // duet 时给两侧文件加前缀（a_ / b_），避免同名追加成拼盘   // 上一次 autorun 的失败（traceback），用于自动修            // --autorun：代码存盘后在设备上直接执行，产物留在同一目录

// 跑本轮保存下来的 .py / .sh，并报告新增产物（pptx/pdf/png/csv…）
function maybeAutorun(saved) {
  if (!AUTORUN || !saved || !saved.length) return;
  const cp = require('child_process');
  const path = require('path');
  const dir = path.dirname(saved[0].path);
  let before = [];
  try { before = fs.readdirSync(dir); } catch (e) { return; }

  // 只跑“入口”脚本：多文件工程里 __init__.py / 被别的文件 import 的模块都不要单独执行，
  // 否则每个都会报错，还会触发回炉，白白多打一轮请求
  const srcOf = (f) => { try { return fs.readFileSync(f.path, 'utf8'); } catch (e) { return ''; } };
  const stem = (f) => require('path').basename(f.path).replace(/\.[^.]+$/, '');
  const isInit = (f) => /(^|_)__init__$|^__init__$/i.test(stem(f));
  const importedByOther = (f) => saved.some(o => {
    if (o.path === f.path) return false;
    const src = srcOf(o);
    const st = stem(f);
    return new RegExp('(^|[\\s;]) *(import|from) +' + st.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\b').test(src);
  });
  const entries = saved.filter(f => {
    const ext = path.extname(f.path).toLowerCase();
    if (ext !== '.py' && ext !== '.sh') return false;
    if (isInit(f)) return false;
    if (/if\s+__name__\s*==\s*['"]__main__['"]/.test(srcOf(f))) return true;
    if (importedByOther(f)) return false;
    return true;
  });
  const runList = entries.length ? entries : saved.slice(0, 1);

  for (const it of runList) {
    const ext = path.extname(it.path).toLowerCase();
    let argv = null;
    if (ext === '.py') argv = ['python3', it.path];
    else if (ext === '.sh') argv = ['sh', it.path];
    if (!argv) continue;
    if (!haveCmd(argv[0])) {
      process.stderr.write('[dss] 跳过 ' + path.basename(it.path) + '：环境里没有 ' + argv[0] + '\n' +
        '      ' + cmdHint(argv[0]) + '\n');
      continue;
    }
    process.stderr.write('[dss] run ' + path.basename(it.path) + ' ...\n');
    const r = cp.spawnSync(argv[0], [argv[1]], { cwd: dir, timeout: 240000, encoding: 'utf8' });
    const log = ((r.stdout || '') + (r.stderr || '')).trim();
    if (log) process.stderr.write(log + '\n');
    if (r.status !== 0) {
      const why = (r.status === null) ? 'timeout' : ('exit=' + r.status);
      process.stderr.write('[dss] run ' + why + '\n');
      AUTORUN_FAILS.push({ path: it.path, log: log || ('run ' + why) });
    }
  }

  let after = [];
  try { after = fs.readdirSync(dir); } catch (e) {}
  const fresh = after.filter(f => before.indexOf(f) < 0)
    .filter(f => !/^__pycache__$|^\..*cache$/.test(f))
    .filter(f => !/\.pyc$/.test(f));
  if (fresh.length) {
    process.stderr.write('[dss] 产物: ' + fresh.map(f => path.join(dir, f)).join(', ') + '\n');
    lastSaved = lastSaved.concat(fresh.map(f => ({ path: path.join(dir, f), name: f })));
  }
}
let CUR_LABEL = '对话';
let BACKEND_A = 'deepseek', BACKEND_B = 'deepseek';
let MODEL_A = '', MODEL_B = '';
let API_THINK = false;          // glm-4.5* 默认关思考；--api-think 打开
let lastSaved = [];
// 当前这次 askText/askCode 走哪个后端 / 哪个模型（duet 每轮切换）
let CUR_BACKEND = 'deepseek';
let CUR_MODEL = '';

// ================= Kimi (www.kimi.com) =================
// 协议（2026-11 实测，一次成功，返回 9413 字节 66 帧）：
//   POST /apiv2/kimi.gateway.chat.v1.ChatService/Chat
//   content-type: application/connect+json  （Connect 协议：00 | 4字节大端长度 | JSON）
//   body: {scenario:"SCENARIO_CHAT", message:{role:"user",blocks:[{message_id:"",text:{content}}]},
//          options:{thinking,enable_plugin,reasoning_effort,model:"k28-instant"}, project_id, chat_id?}
//   响应帧（同一个 Connect 封包）：
//     {"op":"set","chat":{"id":"<chatId>"}}                      -> 会话 id
//     {"op":"set","mask":"message","message":{id,role,status}}   -> assistant 消息
//     {"op":"append","mask":"block.text.content","block":{"text":{"content":"…"}}}   -> 正文增量
//     {"op":"append","mask":"block.think.content","block":{"think":{"content":"…"}}} -> 思考增量
//     message.status = MESSAGE_STATUS_COMPLETED                  -> 结束
// 鉴权：authorization: Bearer <15 分钟有效的 access token> + cookie kimi-auth（长期）。
//   ★ 只带 cookie 当 Bearer 会被 401；access token 过期只能重新从浏览器拿一份 cURL。
const KIMI_HOST = 'www.kimi.com';
const KIMI_PATH = '/apiv2/kimi.gateway.chat.v1.ChatService/Chat';
const KIMI_DEF_MODEL = 'k28-instant';
let KIMI_THINK = process.env.DSS_KIMI_THINK === '1';
let KIMI_TOOLS = process.env.DSS_KIMI_TOOLS === '1';
let KIMI_PLUGIN = process.env.DSS_KIMI_PLUGIN === '1';
const KIMI_AUTH_HOST='auth.kimi.com';
const KIMI_REFRESH_PATH = '/api/account.gateway.v1.AuthService/RefreshToken';

function kimiCredPath() { return process.env.DSS_KIMI_CRED || '/root/.dss/kimi.cred'; }

function kimiParseCred(txt) {
  const out = { cookie: '', bearer: '', refreshToken: '', dev: '', sess: '', traffic: '', model: '', chatId: '' };
  const t = String(txt || '');
  const h = (name) => {
    const re = new RegExp("-H\\s+['\"]" + name + ":\\s*([^'\"\\r\\n]*)['\"]", 'i');
    const m = t.match(re);
    return m ? m[1].trim() : '';
  };
  const mc = t.match(/--cookie\s+'([^']*)'/) || t.match(/-b\s+'([^']*)'/) ||
             t.match(/-H\s+['"]cookie:\s*([^'"]*)['"]/i);
  if (mc) out.cookie = mc[1].trim();
  const auth = h('authorization');
  out.bearer = auth.replace(/^Bearer\s+/i, '').trim();
  out.dev = h('x-msh-device-id');
  out.sess = h('x-msh-session-id');
  out.traffic = h('x-traffic-id');
  const mm = t.match(/"model"\s*:\s*"([^"]+)"/);
  if (mm) out.model = mm[1];
  const mc2 = t.match(/"chat_id"\s*:\s*"([^"]+)"/);
  if (mc2) out.chatId = mc2[1];
  if (!out.model) {
    const line = t.split('\n').map(x => x.trim()).find(x => /^model\s*[:=]/i.test(x));
    if (line) out.model = line.replace(/^model\s*[:=]\s*/i, '').trim();
  }
  // 纯文本形式：refresh_token: xxx / access_token: xxx / dev: / sess: / traffic: / cookie:
  for (const raw of t.split('\n')) {
    const ln = raw.trim();
    if (!ln || ln[0] === '#') continue;
    let m;
    if ((m = ln.match(/^refresh_token\s*[:=]\s*(\S+)$/i))) out.refreshToken = m[1];
    else if ((m = ln.match(/^access_token\s*[:=]\s*(\S+)$/i))) out.bearer = out.bearer || m[1];
    else if ((m = ln.match(/^dev(?:ice)?\s*[:=]\s*(\S+)$/i))) out.dev = out.dev || m[1];
    else if ((m = ln.match(/^sess(?:ion)?\s*[:=]\s*(\S+)$/i))) out.sess = out.sess || m[1];
    else if ((m = ln.match(/^traffic\s*[:=]\s*(\S+)$/i))) out.traffic = out.traffic || m[1];
    else if ((m = ln.match(/^cookie\s*[:=]\s*(.+)$/i))) out.cookie = out.cookie || m[1].trim();
  }
  return out;
}

let KIMI = null;
function kimiLoad() {
  if (KIMI) return KIMI;
  const p = kimiCredPath();
  let txt = '';
  try { txt = fs.readFileSync(p, 'utf8'); } catch (e) {
    throw new Error('没有 Kimi 凭据 ' + p + '（把 /api/chat 的 Copy as cURL 粘进去）');
  }
  const d = kimiParseCred(txt);
  if (!d.bearer && !d.refreshToken) {
    throw new Error(p + ' 里既没有 authorization: Bearer，也没有 refresh_token（二者必给一个）');
  }
  KIMI = d;
  return KIMI;
}

// refresh_token 会轮换，必须写回（0600），否则下一次就用旧的
function kimiPersist() {
  const k = KIMI;
  if (!k) return;
  const p = kimiCredPath();
  const lines = [];
  if (k.refreshToken) lines.push('refresh_token: ' + k.refreshToken);
  if (k.bearer) lines.push('access_token: ' + k.bearer);
  if (k.cookie) lines.push('cookie: ' + k.cookie);
  if (k.dev) lines.push('dev: ' + k.dev);
  if (k.sess) lines.push('sess: ' + k.sess);
  if (k.traffic) lines.push('traffic: ' + k.traffic);
  if (k.model) lines.push('model: ' + k.model);
  try {
    const tmp = p + '.tmp';
    fs.writeFileSync(tmp, lines.join('\n') + '\n', { mode: 0o600 });
    fs.chmodSync(tmp, 0o600);
    fs.renameSync(tmp, p);
  } catch (e) { /* 只读根文件系统忽略 */ }
}

// 把 Kimi 的错误体压成一行人话（外层结构不固定：code / error.code / details[].debug）
function kimiErrText(s) {
  const t = String(s || '').slice(0, 800);
  if (t.indexOf('"') < 0) return t.slice(0, 160);
  const pick = (re) => { const m = t.match(re); return m ? m[1] : ''; };
  const code = pick(/"code"\s*:\s*"([^"]+)/);
  const reason = pick(/"reason"\s*:\s*"([^"]+)/);
  const lm = pick(/"localizedMessage"\s*:\s*\{[^}]*"message"\s*:\s*"([^"]+)/);
  const msg = pick(/"message"\s*:\s*"([^"]+)/);
  const head = [code, reason].filter(Boolean).join('/');
  const tail = lm || msg;
  if (head || tail) return (head + (tail ? ' — ' + tail : '')).trim();
  return t.slice(0, 160);
}

// 用 refresh_token 换 access_token（auth.kimi.com，字段名是 snake_case）
function kimiRefresh() {
  return new Promise(resolve => {
    let k;
    try { k = kimiLoad(); } catch (e) { return resolve({ ok: false, err: e.message }); }
    if (!k.refreshToken) return resolve({ ok: false, err: '没有 refresh_token' });
    const rt = k.refreshToken;
    const body = Buffer.from(JSON.stringify({ refresh_token: rt }), 'utf8');
    const headers = {
      'accept': '*/*',
      'accept-language': 'zh-CN,zh;q=0.9',
      'content-type': 'application/json',
      'connect-protocol-version': '1',
      'origin': 'https://www.kimi.com',
      'referer': 'https://www.kimi.com/',
      'user-agent': UA,
      'content-length': body.length,
    };
    const r = https.request({ host: KIMI_AUTH_HOST, path: KIMI_REFRESH_PATH, method: 'POST', headers }, res => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', d => { buf += d; });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(buf); } catch (e) {}
        const at = j && (j.access_token || j.accessToken);
        const nrt = j && (j.refresh_token || j.refreshToken);
        if (res.statusCode === 200 && at) {
          k.bearer = at;
          if (nrt && nrt !== rt) { k.refreshToken = nrt; kimiPersist(); }
          return resolve({ ok: true });
        }
        let msg = buf || ('HTTP ' + res.statusCode);
        try {
          const d = j && j.details && j.details[0] && j.details[0].debug;
          if (d && d.reason) msg = d.reason + ' ' + ((d.localizedMessage && d.localizedMessage.message) || '');
        } catch (e) {}
        resolve({ ok: false, err: String(msg).slice(0, 200) });
      });
    });
    r.setTimeout(30000, () => r.destroy(new Error('timeout')));
    r.on('error', e => resolve({ ok: false, err: 'net ' + e.message }));
    r.write(body);
    r.end();
  });
}

const KIMI_MIME = {
  txt: 'text/plain', md: 'text/markdown', json: 'application/json', csv: 'text/csv',
  c: 'text/x-c', h: 'text/x-c', py: 'text/x-python', sh: 'text/x-sh', js: 'text/javascript',
  html: 'text/html', css: 'text/css', xml: 'text/xml', yaml: 'text/yaml', yml: 'text/yaml',
  pdf: 'application/pdf', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
  zip: 'application/zip', gz: 'application/gzip', tar: 'application/x-tar',
};
function kimiMime(name) {
  const ext = (String(name).match(/\.([A-Za-z0-9]+)$/) || [])[1];
  return KIMI_MIME[String(ext || '').toLowerCase()] || 'application/octet-stream';
}

// 上传到 Kimi：POST /apiv2-files/file/upload（multipart，字段名固定 file）
// 返回 { raw: <File 对象，原样放进 message.blocks[].file>, id, name, size }
async function kimiUploadFile(filePath, displayName) {
  const fs2 = require('fs'), path2 = require('path');
  const r0 = resolvePath(filePath);
  if (!r0.path) throw new Error('找不到文件 ' + filePath);
  if (r0.note) console.error('[dss] ' + r0.note);
  const buf = fs2.readFileSync(r0.path);
  const name = displayName || path2.basename(r0.path);
  const mime = kimiMime(name);
  const bnd = '----dss' + Date.now() + Math.random().toString(16).slice(2, 8);
  const head = Buffer.from('--' + bnd + '\r\nContent-Disposition: form-data; name="file"; filename="' + name +
    '"\r\nContent-Type: ' + mime + '\r\n\r\n', 'utf8');
  const tail = Buffer.from('\r\n--' + bnd + '--\r\n', 'utf8');
  const body = Buffer.concat([head, buf, tail]);

  const post = () => new Promise((resolve, reject) => {
    let k;
    try { k = kimiLoad(); } catch (e) { return reject(e); }
    const h = kimiHeaders(body.length);
    h['content-type'] = 'multipart/form-data; boundary=' + bnd;
    delete h['connect-protocol-version'];
    const r = https.request({ host: KIMI_HOST, path: '/apiv2-files/file/upload', method: 'POST', headers: h }, res => {
      let t = '';
      res.setEncoding('utf8');
      res.on('data', d => { t += d; });
      res.on('end', () => resolve({ status: res.statusCode, text: t }));
    });
    r.setTimeout(180000, () => r.destroy(new Error('upload timeout')));
    r.on('error', reject);
    r.write(body);
    r.end();
  });

  let r = await post();
  if (r.status === 401) {
    const rr = await kimiRefresh();
    if (!rr.ok) throw new Error('续期失败: ' + rr.err);
    r = await post();
  }
  if (r.status !== 200) throw new Error('HTTP ' + r.status + ' ' + kimiErrText(r.text));
  let j = null;
  try { j = JSON.parse(r.text); } catch (e) {}
  const f = j && (j.file || j.data || j);
  if (!f || !f.id) throw new Error('返回异常: ' + r.text.slice(0, 200));
  return { raw: f, id: f.id, name: (f.meta && f.meta.name) || name, size: (f.meta && f.meta.sizeBytes) || buf.length };
}

function kimiEnvelope(obj) {
  const payload = Buffer.from(JSON.stringify(obj), 'utf8');
  const head = Buffer.alloc(5);
  head[0] = 0;
  head.writeUInt32BE(payload.length, 1);
  return Buffer.concat([head, payload]);
}

function kimiHeaders(len) {
  const k = kimiLoad();
  const h = {
    'accept': '*/*',
    'accept-language': 'zh-CN,zh;q=0.9',
    'authorization': 'Bearer ' + k.bearer,
    'content-type': 'application/connect+json',
    'connect-protocol-version': '1',
    'origin': 'https://www.kimi.com',
    'referer': 'https://www.kimi.com/',
    'user-agent': UA,
    'x-language': 'zh-CN',
    'x-msh-platform': 'web',
    'x-msh-version': '2.3.0',
    'r-timezone': 'Asia/Shanghai',
    'content-length': len,
  };
  if (k.cookie) h['cookie'] = k.cookie;
  if (k.dev) h['x-msh-device-id'] = k.dev;
  if (k.sess) h['x-msh-session-id'] = k.sess;
  if (k.traffic) h['x-traffic-id'] = k.traffic;
  return h;
}

// 解析 Connect 帧，返回已消费的字节数（未完整的帧留在 buffer 里）
function kimiParseFrames(buf, acc, printer) {
  let off = 0;
  while (off + 5 <= buf.length) {
    const len = buf.readUInt32BE(off + 1);
    if (len > buf.length - off - 5) break;
    const s = buf.slice(off + 5, off + 5 + len).toString('utf8');
    off += 5 + len;
    let j;
    try { j = JSON.parse(s); } catch (e) { continue; }
    if (j.chat && j.chat.id) acc.chatId = j.chat.id;
    if (j.message) {
      if (j.message.id && j.message.role === 'assistant') acc.msgId = j.message.id;
      if (j.message.status) acc.status = j.message.status;
    }
    if (j.mask === 'message.status' && j.message && j.message.status) acc.status = j.message.status;
    // 错误/限流帧：Kimi 用 code/message 表达，不放在 block.text 里
    if (typeof j.code === 'string' && j.code !== 'OK' && j.code !== 'ok') {
      acc.err = acc.err || (j.code + (j.message ? ': ' + String(j.message) : ''));
      acc.errCode = acc.errCode || j.code;
    }
    if (j.error) {
      const eo = (typeof j.error === 'object' && j.error) ? j.error : null;
      acc.err = acc.err || (eo ? JSON.stringify(eo).slice(0, 240) : String(j.error).slice(0, 240));
      if (eo && typeof eo.code === 'string') {
        acc.errCode = acc.errCode || eo.code;
        if (typeof eo.message === 'string') acc.err = eo.code + ': ' + eo.message;
      }
      // Connect 会把 details/debug 一起塞进 error 里
      const det = eo && (eo.details || (eo.error && eo.error.details));
      if (det && det[0]) {
        const d = det[0].debug || {};
        if (d.reason) acc.errCode = acc.errCode || String(d.reason);
        if (d.localizedMessage && d.localizedMessage.message) acc.err = acc.err + ' ' + d.localizedMessage.message;
      }
    }
    if (j.block && j.block.statusMessage) acc.err = acc.err || String(j.block.statusMessage).slice(0, 240);
    if (j.block) {
      const b = j.block;
      if (b.think && typeof b.think.content === 'string' && b.think.content) acc.think += b.think.content;
      if (b.text && typeof b.text.content === 'string' && b.text.content) {
        acc.text += b.text.content;
        if (printer) printer.write(b.text.content);
      }
    }
  }
  return off;
}

async function kimiChatOnce(prompt, state, printer, model) {
  let k;
  try { k = kimiLoad(); } catch (e) { return { text: '', err: e.message, code: 0 }; }
  if (!k.bearer && k.refreshToken) {
    const rr = await kimiRefresh();
    if (!rr.ok) return { text: '', err: 'refresh 失败: ' + rr.err, code: 401 };
  }
  return new Promise(resolve => {
    const mid = String(model || k.model || KIMI_DEF_MODEL).trim();
    // 附件：只在会话首条消息带一次（和 DeepSeek 的 filesSent 语义一致）
    const blocks = [];
    if (state && state.kimiFiles && state.kimiFiles.length && !state.kimiFilesSent) {
      for (const f of state.kimiFiles) blocks.push({ file: f.raw });
    }
    blocks.push({ message_id: '', text: { content: prompt } });
    const req = {
      scenario: 'SCENARIO_CHAT',
      tools: KIMI_TOOLS ? [{ type: 'TOOL_TYPE_SEARCH', search: {} }, { type: 'TOOL_TYPE_CRON_JOB' }] : [],
      message: {
        role: 'user',
        blocks: blocks,
        scenario: 'SCENARIO_CHAT',
        is_goal: false,
      },
      options: KIMI_THINK
        ? { thinking: true, enable_plugin: KIMI_PLUGIN, reasoning_effort: 'REASONING_EFFORT_LOW', model: mid }
        : { thinking: false, enable_plugin: KIMI_PLUGIN, model: mid },
      project_id: '',
    };
    const cid = state.sessionId || state.kimiChatId;
    if (cid) req.chat_id = cid;
    const body = kimiEnvelope(req);
    const acc = { text: '', think: '', chatId: cid || null, msgId: null, status: null, err: null, errCode: null };
    const r = https.request({ host: KIMI_HOST, path: KIMI_PATH, method: 'POST', headers: kimiHeaders(body.length) }, res => {
      if (res.statusCode !== 200) {
        let buf = '';
        res.setEncoding('utf8');
        res.on('data', d => { buf += d; });
        res.on('end', () => {
          let ecode = null, msg = buf || ('HTTP ' + res.statusCode);
          try {
            const j = JSON.parse(buf);
            if (typeof j.code === 'string') ecode = j.code;
            const d = j.details && j.details[0] && j.details[0].debug;
            if (d) msg = (d.reason || '') + ' ' + ((d.localizedMessage && d.localizedMessage.message) || '');
          } catch (e) {}
          resolve({ text: '', err: String(msg).slice(0, 800), errCode: ecode, code: res.statusCode });
        });
        return;
      }
      let buf = Buffer.alloc(0);
      const rawDump = [];
      res.on('data', d => {
        rawDump.push(d);
        buf = Buffer.concat([buf, d]);
        const used = kimiParseFrames(buf, acc, printer);
        if (used) buf = buf.slice(used);
      });
      res.on('end', () => {
        if (buf.length >= 5) kimiParseFrames(buf, acc, printer);
        if (printer) printer.flush();
        if (state && state.kimiFiles && state.kimiFiles.length && (acc.text.trim() || acc.chatId)) {
          state.kimiFilesSent = true;
        }
        if (!acc.text.trim()) {
          try {
            const dbg = process.env.DSS_KIMI_DUMP || '/tmp/dss/kimi_last.bin';
            require('fs').mkdirSync(require('path').dirname(dbg), { recursive: true, mode: 0o700 });
            require('fs').writeFileSync(dbg, rawDump.slice(-65536), { mode: 0o600 });
          } catch (e) {}
        }
        resolve({ text: acc.text, think: acc.think, chatId: acc.chatId, err: acc.err, errCode: acc.errCode, code: 200, status: acc.status });
      });
      res.on('error', e => resolve({ text: acc.text, think: acc.think, chatId: acc.chatId, err: 'stream ' + e.message, code: 200 }));
    });
    r.setTimeout(STALL_MS, () => r.destroy(new Error('no data for ' + Math.round(STALL_MS / 1000) + 's')));
    r.on('error', e => resolve({ text: acc.text, think: acc.think, chatId: acc.chatId, err: 'net ' + e.message, code: 0 }));
    r.write(body);
    r.end();
  });
}


// ================= 豆包 (www.doubao.com) =================
// 协议（2026-11 抓包 + 实测）：
//   POST https://www.doubao.com/chat/completion?<aid/device_id/.../msToken/a_bogus>
//   headers: content-type: application/json, agw-js-conv: str, str, cookie, x-flow-trace,
//            user-agent 必须与抓包时一致（a_bogus 按它算的）
//   body: {client_meta:{local_conversation_id,conversation_id,bot_id,last_section_id,last_message_index},
//          messages:[{local_message_id,content_block:[{block_type:10000,content:{text_block:{text}}}]}],
//          option:{unique_key,create_time_ms,need_create_conversation,...}, user_context:[], ext:{}}
//   SSE: event: SSE_ACK(conversation_id/section_id) / STREAM_MSG_NOTIFY / STREAM_CHUNK(正文增量)
//        / STREAM_ERROR / DOWNLINK_CMD(cmd 50080 = 已答过，去拉历史) / SSE_REPLY_END
//   匿名态只能续已有会话（need_create_conversation=false），且频控极严（710022002 当前服务访问频繁）。
const DOUBAO_HOST = 'www.doubao.com';

function doubaoCredPath() { return process.env.DSS_DOUBAO_CRED || '/root/.dss/doubao.cred'; }

function doubaoParseCred(txt) {
  const t = String(txt || '');
  const out = { query: '', headers: {}, cookie: '', bodyTpl: null, convId: '', localConv: '', sectionId: '', lastIndex: null, botId: '' };

  const mu = t.match(/curl\s+'([^']+)'/);
  if (!mu) throw new Error('凭据里没有 "curl \'https://www.doubao.com/chat/completion?...\'"');
  const u = new URL(mu[1]);
  out.query = u.search || '';

  const re = /-H\s+'([^']+)'/g;
  let m;
  while ((m = re.exec(t)) !== null) {
    const kv = m[1];
    const i = kv.indexOf(':');
    if (i > 0) out.headers[kv.slice(0, i).trim().toLowerCase()] = kv.slice(i + 1).trim();
  }
  m = t.match(/-b\s+'([^']*)'/);
  if (m) out.cookie = m[1];

  m = t.match(/--data-raw\s+'([\s\S]*?)'(?=\s*(?:;|$|\n))/);
  if (!m) throw new Error('凭据里没有 --data-raw 里的 JSON body');
  try { out.bodyTpl = JSON.parse(m[1]); } catch (e) { throw new Error('body 不是合法 JSON: ' + e.message); }

  const cm = out.bodyTpl.client_meta || {};
  out.localConv = cm.local_conversation_id || '';
  out.convId = cm.conversation_id || '';
  out.botId = cm.bot_id || '';
  out.sectionId = cm.last_section_id || '';

  // 可选覆盖行：conversation_id: 3845... / local_conversation_id: local_...
  for (const raw of t.split('\n')) {
    const ln = raw.trim();
    let k;
    if ((k = ln.match(/^conversation_id\s*[:=]\s*(\S+)$/i))) out.convId = k[1];
    else if ((k = ln.match(/^local_conversation_id\s*[:=]\s*(\S+)$/i))) out.localConv = k[1];
    else if ((k = ln.match(/^section_id\s*[:=]\s*(\S+)$/i))) out.sectionId = k[1];
  }
  return out;
}

let DOUBAO = null;
function doubaoLoad() {
  if (DOUBAO) return DOUBAO;
  const p2 = doubaoCredPath();
  let txt = '';
  try { txt = fs.readFileSync(p2, 'utf8'); } catch (e) {
    throw new Error('没有豆包凭据 ' + p2 + '（把 /chat/completion 的 Copy as cURL 粘进去）');
  }
  DOUBAO = doubaoParseCred(txt);
  return DOUBAO;
}

function doubaoUuid() { return require('crypto').randomUUID(); }

function doubaoBuildBody(prompt, d, state) {
  const tpl = JSON.parse(JSON.stringify(d.bodyTpl));
  const now = Date.now();
  const cm = tpl.client_meta || (tpl.client_meta = {});
  const conv = state.doubaoConv || d.convId || '';
  cm.local_conversation_id = state.doubaoLocal || d.localConv || ('local_' + now);
  cm.conversation_id = conv;
  if (state.doubaoSection || d.sectionId) cm.last_section_id = state.doubaoSection || d.sectionId;
  cm.last_message_index = (state.doubaoIndex != null) ? state.doubaoIndex
    : (d.lastIndex == null ? null : d.lastIndex);
  if (!cm.bot_id && d.botId) cm.bot_id = d.botId;

  const msg = (tpl.messages && tpl.messages[0]) || { message_status: 0 };
  msg.local_message_id = doubaoUuid();
  const blk = (msg.content_block && msg.content_block[0]) || { block_type: 10000, parent_id: '', meta_info: [], extra_info: {}, append_fields: [] };
  blk.block_type = blk.block_type || 10000;
  blk.block_id = doubaoUuid();
  blk.parent_id = '';
  blk.content = { text_block: { text: prompt, icon_url: '', icon_url_dark: '', summary: '' }, pc_event_block: '' };
  msg.content_block = [blk];
  tpl.messages = [msg];

  const opt = tpl.option || (tpl.option = {});
  opt.unique_key = doubaoUuid();
  opt.create_time_ms = now;
  opt.need_create_conversation = !conv;
  if (opt.recovery_option) opt.recovery_option.req_create_time_sec = Math.floor(now / 1000);
  return tpl;
}

// 解析豆包 SSE：event: 行给类型，data: 行给 JSON
function doubaoParseSSE(block, acc, printer) {
  let ev = null;
  const data = [];
  for (const l of String(block).split('\n')) {
    if (l.startsWith('event:')) ev = l.slice(6).trim();
    else if (l.startsWith('data:')) data.push(l.slice(5).trim());
  }
  if (!data.length) return;
  let j = null;
  try { j = JSON.parse(data.join('\n')); } catch (e) { return; }

  if (ev === 'SSE_ACK') {
    const cm = j.ack_client_meta || {};
    if (cm.conversation_id) acc.convId = cm.conversation_id;
    if (cm.section_id) acc.sectionId = cm.section_id;
    const ql = j.query_list && j.query_list[0];
    if (ql && ql.message_index) acc.lastIndex = ql.message_index;
    return;
  }
  if (ev === 'STREAM_ERROR') {
    acc.errCode = j.error_code || acc.errCode;
    acc.err = j.error_msg || JSON.stringify(j).slice(0, 200);
    // shark 风控：extra.decision 里带 type/subtype，verify+slide 就是要求滑块验证
    try {
      const dec = j.extra && j.extra.decision;
      const d = typeof dec === 'string' ? JSON.parse(dec) : dec;
      if (d && d.type === 'verify') acc.verify = (d.subtype || 'verify') + (d.from ? '@' + d.from : '');
    } catch (e) {}
    return;
  }
  if (ev === 'DOWNLINK_CMD') {
    if (j.cmd === 50080 && !acc.text) acc.err = '服务端判定该请求已答过（重复 unique_key），请换会话或稍后再试';
    return;
  }
  if (ev !== 'STREAM_MSG_NOTIFY' && ev !== 'STREAM_CHUNK') return;

  (function walk(o) {
    if (!o || typeof o !== 'object') return;
    if (Array.isArray(o.content_block)) {
      for (const b of o.content_block) {
        const tb = b && b.content && b.content.text_block;
        if (tb && typeof tb.text === 'string' && tb.text !== '$' && tb.text !== '') {
          acc.text += tb.text;
          if (printer) printer.write(tb.text);
        }
      }
    }
    for (const k of Object.keys(o)) {
      if (k === 'content_block') continue;
      if (o[k] && typeof o[k] === 'object') walk(o[k]);
    }
  })(j);
}

function doubaoAskOnce(prompt, state, printer) {
  return new Promise(resolve => {
    let d;
    try { d = doubaoLoad(); } catch (e) { return resolve({ text: '', err: e.message, code: 0 }); }
    let body;
    try { body = Buffer.from(JSON.stringify(doubaoBuildBody(prompt, d, state)), 'utf8'); }
    catch (e) { return resolve({ text: '', err: 'body 构造失败: ' + e.message, code: 0 }); }

    const h = Object.assign({}, d.headers);
    h['content-type'] = 'application/json';
    h['origin'] = 'https://www.doubao.com';
    h['referer'] = 'https://www.doubao.com/chat/' + (state.doubaoLocal || d.localConv || '');
    h['content-length'] = body.length;
    if (d.cookie && !h['cookie']) h['cookie'] = d.cookie;
    delete h['host']; delete h['content-length'];
    h['content-length'] = body.length;

    const acc = { text: '', convId: state.doubaoConv || d.convId || null, sectionId: null, lastIndex: null, err: null, errCode: null, verify: null };
    const r = https.request({ host: DOUBAO_HOST, path: '/chat/completion' + (d.query || ''), method: 'POST', headers: h }, res => {
      if (res.statusCode !== 200) {
        let t = '';
        res.setEncoding('utf8');
        res.on('data', x => { t += x; });
        res.on('end', () => resolve({ text: '', err: 'HTTP ' + res.statusCode + ' ' + t.slice(0, 200), code: res.statusCode }));
        return;
      }
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', chunk => {
        buf += chunk;
        let i;
        while ((i = buf.indexOf('\n\n')) >= 0) {
          const blk = buf.slice(0, i);
          buf = buf.slice(i + 2);
          doubaoParseSSE(blk, acc, printer);
        }
      });
      res.on('end', () => {
        if (buf.trim()) doubaoParseSSE(buf, acc, printer);
        if (printer) printer.flush();
        resolve({ text: acc.text, convId: acc.convId, sectionId: acc.sectionId, lastIndex: acc.lastIndex, err: acc.err, errCode: acc.errCode, verify: acc.verify || null, code: 200 });
      });
      res.on('error', e => resolve({ text: acc.text, err: 'stream ' + e.message, code: 200 }));
    });
    r.setTimeout(STALL_MS, () => r.destroy(new Error('no data for ' + Math.round(STALL_MS / 1000) + 's')));
    r.on('error', e => resolve({ text: acc.text, err: 'net ' + e.message, code: 0 }));
    r.write(body);
    r.end();
  });
}

// 单次豆包请求；重试/退避统一由 askText 的循环处理
async function askDoubaoOnce(prompt, state, printer) {
  const r = await doubaoAskOnce(prompt, state, printer);
  if (r.convId) state.doubaoConv = r.convId;
  if (r.sectionId) state.doubaoSection = r.sectionId;
  if (r.lastIndex != null) state.doubaoIndex = r.lastIndex;
  if (!state.doubaoLocal) {
    try { state.doubaoLocal = doubaoLoad().localConv; } catch (e) {}
  }

  const text = (r.text || '').trim();
  if (text.length >= 1) {
    lastAnswerChars = text.length;
    logConv('DOUBAO', text);
    try {
      const savedNow = CODE_MODE ? [] : saveScripts(text, CUR_LABEL);
      if (savedNow.length) {
        lastSaved = savedNow;
        maybeAutorun(savedNow);
        process.stderr.write('[dss] ' + savedNow.length + ' code file(s) saved: ' +
          savedNow.map(x => require('path').basename(x.path)).join(', ') + '\n');
      }
    } catch (e) {}
    return text;
  }

  const why = r.err || ('HTTP/SSE 无正文' + (r.code ? ' (' + r.code + ')' : ''));
  if (r.verify) {
    const e = new Error('豆包要求人机验证（shark ' + r.verify + '）—— 纯 HTTP 重放过不去，' +
      '请在浏览器里打开 www.doubao.com 过一次滑块，再重粘一份 cURL；冷却期内也会被拦');
    e.noRetry = true;
    throw e;
  }
  if (String(r.errCode) === '710022002' || /频繁|too many|rate/i.test(why)) {
    const e = new Error('豆包: ' + why);
    e.rateLimited = true;
    throw e;
  }
  if (r.err) throw new Error('豆包: ' + why);
  return '';
}

// 单次 Kimi 请求；重试/限流退避由 askText 的循环统一处理（和 DeepSeek 同一条路）
async function askKimiOnce(prompt, state, printer) {
  await requestGate();
  const r = await kimiChatOnce(prompt, state, printer, CUR_MODEL);
  if (r.chatId) { state.sessionId = r.chatId; state.kimiChatId = r.chatId; }

  // 401：refresh_token 续期一次
  if (r.code === 401 || /unauthor|token.*expir|expired/i.test(String(r.err || ''))) {
    if (!state.kimiRefreshed) {
      state.kimiRefreshed = true;
      printer.flush();
      process.stderr.write('\n[Kimi] access token 过期，用 refresh_token 自动续期…\n');
      const rr = await kimiRefresh();
      if (rr.ok) return askKimiOnce(prompt, state, printer);
      const e = new Error('Kimi 自动续期失败: ' + rr.err);
      e.kimiExpired = true;
      throw e;
    }
    const e = new Error('Kimi 登录态失效（refresh 之后仍然 401）');
    e.kimiExpired = true;
    throw e;
  }

  const text = (r.text || '').trim();
  if (text.length >= 1) {
    lastAnswerChars = text.length;
    logConv('KIMI', text);
    try {
      const savedNow = CODE_MODE ? [] : saveScripts(text, CUR_LABEL);
      if (savedNow.length) {
        lastSaved = savedNow;
        maybeAutorun(savedNow);
        process.stderr.write('[dss] ' + savedNow.length + ' code file(s) saved: ' +
          savedNow.map(x => require('path').basename(x.path)).join(', ') + '\n');
      }
    } catch (e) {}
    return text;
  }

  // 没拿到正文：把服务端给的原因亮出来；像限流的交给 backoff
  const why = kimiErrText(r.err || (r.status ? 'status=' + r.status : '(empty)'));
  const RETRYABLE = ['resource_exhausted', 'unavailable', 'deadline_exceeded', 'internal', 'aborted', 'rate_limited'];
  const RETRY_RE = /resource_exhausted|unavailable|deadline_exceeded|server_overload|rate|too many|overload/i;
  if ((r.errCode && RETRYABLE.indexOf(String(r.errCode)) >= 0) || RETRY_RE.test(String(r.err || '')) || looksRateLimited(why)) {
    const e = new Error('Kimi: ' + why);
    e.rateLimited = true;
    throw e;
  }
  if (r.err) throw new Error('Kimi: ' + why);
  return '';
}


// ================= 浏览器传输（wenxin / doubao） =================
// 路由器上没有签名环境：用 chrome-headless-shell 的 --remote-debugging-pipe（fd3 写/fd4 读，\0 分隔），
// 让页面自己去发请求（chat_token / a_bogus / 风控 / 滑块都交给浏览器），dss 只负责输入和读回正文。
const BROWSER_SITES = {
  wenxin: { url: 'https://wenxin.baidu.com/', idle: /(正在思考|正在生成|停止生成|思考中)/ },
  doubao: { url: 'https://www.doubao.com/chat/', idle: /(正在输入|思考中|停止生成|生成中)/ },
};

let CH = null;   // { proc, buf, pending, seq, targets: {} }

function chStart() {
  if (CH) return CH;
  const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
    '--remote-debugging-pipe', '--user-data-dir=' + (process.env.DSS_CHROME_PROFILE || '/root/.dss/chrome-profile'), 'about:blank'];
  const proc = require('child_process').spawn(CHROME_LAUNCHER, args,
    { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });
  const ch = { proc, buf: '', pending: new Map(), seq: 1, targets: {}, ready: false };
  proc.stdio[4].setEncoding('utf8');
  proc.stdio[4].on('data', d => {
    ch.buf += d;
    let i;
    while ((i = ch.buf.indexOf('\0')) >= 0) {
      const msg = ch.buf.slice(0, i);
      ch.buf = ch.buf.slice(i + 1);
      let j;
      try { j = JSON.parse(msg); } catch (e) { continue; }
      if (j.id && ch.pending.has(j.id)) { ch.pending.get(j.id)(j); ch.pending.delete(j.id); }
    }
  });
  proc.on('exit', () => { if (CH === ch) CH = null; });
  CH = ch;
  return ch;
}

function chSend(method, params, sessionId, timeout) {
  const ch = chStart();
  const id = ch.seq++;
  const m = { id, method, params: params || {} };
  if (sessionId) m.sessionId = sessionId;
  try { ch.proc.stdio[3].write(JSON.stringify(m) + '\0'); } catch (e) { return Promise.reject(e); }
  return new Promise((res, rej) => {
    ch.pending.set(id, res);
    setTimeout(() => {
      if (ch.pending.has(id)) { ch.pending.delete(id); rej(new Error('cdp timeout: ' + method)); }
    }, timeout || 30000);
  });
}

async function chAttach(key) {
  const site = BROWSER_SITES[key];
  const ch = chStart();
  if (ch.targets[key]) {
    try {
      const ts = await chSend('Target.getTargets', {}, null, 10000);
      if (ts.result.targetInfos.some(x => x.targetId === ch.targets[key])) {
        const a = await chSend('Target.attachToTarget', { targetId: ch.targets[key], flatten: true });
        await chSend('Page.enable', {}, a.result.sessionId, 10000).catch(() => {});
        return a.result.sessionId;
      }
    } catch (e) {}
  }
  const t = await chSend('Target.createTarget', { url: site.url }, null, 30000);
  ch.targets[key] = t.result.targetId;
  const a = await chSend('Target.attachToTarget', { targetId: t.result.targetId, flatten: true });
  await chSend('Page.enable', {}, a.result.sessionId, 10000).catch(() => {});
  return a.result.sessionId;
}

// 页面跳转会把上下文冲掉：遇到就重新 attach
async function chEval(key, sid, expr, awaitPromise) {
  let s = sid;
  for (let i = 0; i < 5; i++) {
    try {
      const r = await chSend('Runtime.evaluate',
        { expression: expr, returnByValue: true, awaitPromise: !!awaitPromise }, s, 60000);
      if (r.error && /navigated|closed|Cannot find context|Execution context/i.test(r.error.message)) {
        await sleep(700);
        s = await chAttach(key);
        continue;
      }
      return r;
    } catch (e) {
      await sleep(700);
      try { s = await chAttach(key); } catch (x) {}
    }
  }
  return null;
}

function chVal(r) {
  return (r && r.result && r.result.result && r.result.result.value !== undefined) ? r.result.result.value : null;
}

async function chSettle(key, sid) {
  let prev = '';
  for (let i = 0; i < 25; i++) {
    const v = chVal(await chEval(key, sid, 'location.href + "|" + document.readyState'));
    if (v && v === prev && /complete$/.test(v)) return v;
    prev = v || prev;
    await sleep(600);
  }
  return prev;
}

const CH_SUBMIT_JS = (prompt) => `(function(){
  const q = ${JSON.stringify('__Q__')};
  const el = document.querySelector('textarea') || document.querySelector('[contenteditable]');
  if (!el) return 'no-input';
  if (el.tagName === 'TEXTAREA') {
    const set = Object.getOwnPropertyDescriptor(window.HTMLTextAreaElement.prototype, 'value').set;
    set.call(el, q);
  } else { el.textContent = q; }
  el.dispatchEvent(new Event('input', { bubbles: true }));
  el.focus();
  el.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, bubbles: true }));
  el.dispatchEvent(new KeyboardEvent('keyup', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, bubbles: true }));
  return 'sent';
})()`;

const CH_OBSERVE_JS = `(function(){
  window.__dssBuf = '';
  try { if (window.__dssObs) window.__dssObs.disconnect(); } catch (e) {}
  window.__dssObs = new MutationObserver(function (ms) {
    for (const m of ms) for (const n of m.addedNodes) {
      if (n && n.nodeType === 1) {
        const t = n.innerText || n.textContent || '';
        if (t) window.__dssBuf += t + '\\n';
      }
    }
  });
  window.__dssObs.observe(document.body, { childList: true, subtree: true });
  return 'observed';
})()`;

async function askBrowser(prompt, state, printer) {
  const key = CUR_BACKEND === 'doubao' ? 'doubao' : 'wenxin';
  const site = BROWSER_SITES[key];
  let sid = await chAttach(key);
  await chSettle(key, sid);
  sid = await chAttach(key);                 // 跳转后重新拿 sessionId

  // SPA（豆包）挂载很慢：先等输入框真的出现，别急着判定失败
  let ready = false;
  for (let i = 0; i < 45; i++) {
    const v = chVal(await chEval(key, sid,
      "!!(document.querySelector('textarea')||document.querySelector('[contenteditable]'))"));
    if (v === true) { ready = true; break; }
    await sleep(700);
  }
  if (!ready) throw new Error('页面一直没有输入框（未加载完或被要求登录）');

  const obs = await chEval(key, sid, CH_OBSERVE_JS);
  if (chVal(obs) !== 'observed') throw new Error('浏览器注入失败（chrome 未就绪？）');

  const sub = await chEval(key, sid, CH_SUBMIT_JS(prompt).replace('__Q__', prompt));
  const sv = chVal(sub);
  if (sv !== 'sent') throw new Error('页面上找不到输入框（页面结构变了或没加载完）');

  let printed = 0, stable = 0, lastLen = 0;
  const t0 = Date.now();
  while (Date.now() - t0 < 180000) {
    await sleep(900);
    const buf = chVal(await chEval(key, sid, 'window.__dssBuf || ""'));
    if (typeof buf === 'string' && buf.length > printed) {
      printer.write(buf.slice(printed));
      printed = buf.length;
      stable = 0;
    } else {
      stable++;
    }
    // 正文稳定 && 页面不在“思考中” → 结束
    const body = chVal(await chEval(key, sid, '(function(){var t=document.body.innerText||"";return t.slice(-200);})()'));
    const busy = typeof body === 'string' && site.idle.test(body);
    if (!busy && stable >= 3 && printed > 0) break;
    lastLen = printed;
  }
  printer.flush();

  const buf = chVal(await chEval(key, sid, 'window.__dssBuf || ""')) || '';
  // 去掉我们自己的提问回显和常见尾部噪声
  let text = String(buf)
    .replace(prompt, '')
    .replace(/内容由AI生成[^\n]*/g, '')
    .replace(/对话支持收藏啦?/g, '')
    .replace(/^\s+|\s+$/g, '');
  if (!text) throw new Error('浏览器里没抓到正文（页面可能要求登录或一直在思考）');
  return text;
}


// ================= 通用 OpenAI 兼容后端（智谱 / 硅基流动 / 千帆 / 混元 / Ark / OpenRouter） =================
// 凭据 /root/.dss/openai.cred（0600），三行：
//   base: https://open.bigmodel.cn/api/paas/v4
//   key:  xxxxxxxx
//   model: glm-4-flash
// 请求 POST {base}/chat/completions，stream:true，SSE: data:{"choices":[{"delta":{"content":"…"}}]}
function apiCredPath() { return process.env.DSS_API_CRED || '/root/.dss/openai.cred'; }

let API = null;
function apiLoad() {
  if (API) return API;
  const p2 = apiCredPath();
  let txt = '';
  try { txt = fs.readFileSync(p2, 'utf8'); } catch (e) {
    throw new Error('没有 API 凭据 ' + p2 + '（base/key/model 三行）');
  }
  const out = { base: '', key: '', model: '' };
  for (const raw of txt.split('\n')) {
    const m = raw.trim().match(/^(base|key|model)\s*:\s*(\S.*)$/i);
    if (m) out[m[1].toLowerCase()] = m[2].trim();
  }
  if (!out.base || !out.key) throw new Error(p2 + ' 缺少 base 或 key');
  out.base = out.base.replace(/\/+$/, '');
  API = out;
  return API;
}

function apiHistory(state) {
  if (!state.apiMessages) state.apiMessages = [];
  return state.apiMessages;
}

function apiAskOnce(prompt, state, printer) {
  return new Promise(resolve => {
    let cfg;
    try { cfg = apiLoad(); } catch (e) { return resolve({ text: '', err: e.message, code: 0 }); }
    const model = String(CUR_MODEL || cfg.model || '').trim();
    if (!model) return resolve({ text: '', err: 'API 后端没有模型名（LuCI 或 --model-a）', code: 0 });

    const hist = apiHistory(state);
    const msgs = hist.concat([{ role: 'user', content: prompt }]);
    const reqBody = { model, messages: msgs, stream: true };
    // 实测：glm-4.5-flash 开思考首字 8.0s / 关思考 5.0s / glm-4-flash 0.67s
    if (/^glm-4\.[5-9]/.test(model) && !API_THINK) reqBody.thinking = { type: 'disabled' };
    const payload = Buffer.from(JSON.stringify(reqBody), 'utf8');
    let u;
    try { u = new URL(cfg.base + '/chat/completions'); } catch (e) { return resolve({ text: '', err: 'base 不是合法 URL', code: 0 }); }
    const headers = {
      'content-type': 'application/json',
      'authorization': 'Bearer ' + cfg.key,
      'accept': 'text/event-stream',
      'content-length': payload.length,
      'user-agent': UA,
    };
    const acc = { text: '', err: null };
    const lib = (u.protocol === 'http:') ? require('http') : https;
    const r = lib.request({ host: u.hostname, port: u.port || (u.protocol === 'http:' ? 80 : 443), path: u.pathname + u.search, method: 'POST', headers }, res => {
      if (res.statusCode !== 200) {
        let t = '';
        res.setEncoding('utf8');
        res.on('data', d => { t += d; });
        res.on('end', () => resolve({ text: '', err: 'HTTP ' + res.statusCode + ' ' + kimiErrText(t).slice(0, 200), code: res.statusCode }));
        return;
      }
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', d => {
        buf += d;
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, i).trim();
          buf = buf.slice(i + 1);
          if (!line.startsWith('data:')) continue;
          const data = line.slice(5).trim();
          if (data === '[DONE]') continue;
          let j;
          try { j = JSON.parse(data); } catch (e) { continue; }
          if (j.error) { acc.err = j.error.message || JSON.stringify(j.error).slice(0, 200); continue; }
          const ch2 = j.choices && j.choices[0];
          const d2 = ch2 && ch2.delta && ch2.delta.content;
          const rc = ch2 && ch2.delta && ch2.delta.reasoning_content;
          if (typeof rc === 'string' && rc) {
            acc.think = (acc.think || '') + rc;          // 思考内容单独攒着，不打印
          }
          if (typeof d2 === 'string' && d2) { acc.text += d2; if (printer) printer.write(d2); }
        }
      });
      res.on('end', () => {
        if (printer) printer.flush();
        // GLM-4.5 有时只回思考不回正文：别当空回答，落到 think 上
        if (!acc.text && acc.think) acc.text = acc.think;
        resolve({ text: acc.text, err: acc.err, code: 200 });
      });
      res.on('error', e => resolve({ text: acc.text, err: 'stream ' + e.message, code: 200 }));
    });
    r.setTimeout(STALL_MS, () => r.destroy(new Error('no data for ' + Math.round(STALL_MS / 1000) + 's')));
    r.on('error', e => resolve({ text: acc.text, err: 'net ' + e.message, code: 0 }));
    r.write(payload);
    r.end();
  });
}

async function askApiOnce(prompt, state, printer) {
  await requestGate();
  const r = await apiAskOnce(prompt, state, printer);
  const text = (r.text || '').trim();
  if (text.length >= 1) {
    const hist = apiHistory(state);
    hist.push({ role: 'user', content: prompt });
    hist.push({ role: 'assistant', content: text });
    if (hist.length > 40) hist.splice(0, hist.length - 40);
    lastAnswerChars = text.length;
    logConv('API', text);
    try {
      const savedNow = CODE_MODE ? [] : saveScripts(text, CUR_LABEL);
      if (savedNow.length) {
        lastSaved = savedNow;
        maybeAutorun(savedNow);
        process.stderr.write('[dss] ' + savedNow.length + ' code file(s) saved: ' +
          savedNow.map(x => require('path').basename(x.path)).join(', ') + '\n');
      }
    } catch (e) {}
    return text;
  }
  const why = r.err || ('HTTP/SSE 无正文' + (r.code ? ' (' + r.code + ')' : ''));
  if (r.code === 401 || r.code === 403 || /invalid.*(api.?key|token)|unauthor/i.test(why)) {
    const e = new Error('API Key 无效或过期：' + why);
    e.noRetry = true;
    throw e;
  }
  if (r.code === 429 || /rate|too many|频繁|quota/i.test(why)) {
    const e = new Error('API 限流/额度用尽：' + why);
    e.rateLimited = true;
    throw e;
  }
  if (r.err) throw new Error('API 错误：' + why);
  return '';
}


// ================= 全局请求节流器 =================
// 目的：防止 --duet + --code + --autorun 叠加时打出 4~6 秒一条的突发（这是网页版风控最敏感的形态）
// 规则：任意两次请求之间至少 DSS_MIN_GAP 秒（默认 8），且每 60 秒最多 DSS_MAX_PER_MIN 条（默认 6）
const REQ_MIN_GAP = Math.max(0, parseInt(process.env.DSS_MIN_GAP || '8', 10) || 0) * 1000;
const REQ_MAX_PER_MIN = Math.max(1, parseInt(process.env.DSS_MAX_PER_MIN || '6', 10) || 6);
let _lastReqAt = 0;
const _reqStamps = [];

function _gatePlan() {
  const now = Date.now();
  while (_reqStamps.length && now - _reqStamps[0] > 60000) _reqStamps.shift();
  let wait = Math.max(0, REQ_MIN_GAP - (now - _lastReqAt));
  if (_reqStamps.length >= REQ_MAX_PER_MIN) {
    wait = Math.max(wait, 60000 - (now - _reqStamps[0]) + 100);
  }
  return wait;
}

async function requestGate() {
  if (REQ_MIN_GAP === 0 && REQ_MAX_PER_MIN >= 9999) return;
  let wait = _gatePlan();
  if (wait <= 0) { _lastReqAt = Date.now(); _reqStamps.push(_lastReqAt); return; }
  process.stderr.write('[throttle] 请求节流：等 ' + Math.ceil(wait / 1000) + 's' +
    '（最近一分钟 ' + _reqStamps.length + '/' + REQ_MAX_PER_MIN + ' 条，最短间隔 ' +
    Math.round(REQ_MIN_GAP / 1000) + 's）\n');
  const t0 = Date.now();
  while (Date.now() - t0 < wait) {
    await new Promise(r => setTimeout(r, Math.min(2000, wait - (Date.now() - t0))));
  }
  _lastReqAt = Date.now();
  _reqStamps.push(_lastReqAt);
}

async function askText(c, pow, prompt, state, printer, tries) {
  if (process.env.DSS_DRY === '1') {
    process.stderr.write('\n----- PROMPT (dry-run) -----\n' + prompt + '\n----- END -----\n');
    return '（dry-run）';
  }
  tries = tries || 3;
  if (CUR_BACKEND === 'deepseek' && (!c || !c.token)) {
    throw new Error('当前后端是 deepseek，但没有 DeepSeek 令牌（改用 --backend-a kimi / doubao / wenxin）');
  }
  let text = '';
  for (let i = 0; i < tries; i++) {
    try {
      text = (CUR_BACKEND === 'kimi')
        ? await askKimiOnce(prompt, state, printer)
        : (CUR_BACKEND === 'doubao' || CUR_BACKEND === 'wenxin')
          ? await askBrowser(prompt, state, printer)
          : (CUR_BACKEND === 'api' || CUR_BACKEND === 'glm' || CUR_BACKEND === 'openai')
            ? await askApiOnce(prompt, state, printer)
            : ((await ask(c, pow, prompt, state, printer)) || '');
    } catch (e) {
      if (e && e.noRetry) throw e;
      if (e && e.rateLimited) {
        await backoff(String(e.message || ''));
        i--;
        continue;
      }
      throw e;
    }
    if (looksRateLimited(text)) {
      await backoff('server said: ' + String(text).trim().slice(0, 60));
      i--;                       // 限流不算一次失败重试
      continue;
    }
    // 只要不是完全空就算有效（"2"、"ok" 这种短答案很常见）
    if (text.trim().length >= 1) {
      lastAnswerChars = text.length;
      if (rlHits === 0) paceMs = Math.max(3000, Math.round(paceMs * 0.85));
      emptyStreak = 0;
      return text;
    }
    // 连续空回答通常就是被风控了（服务端只回 200+空，不给文字提示）
    emptyStreak++;
    if (emptyStreak >= 2) {
      await backoff('连续 ' + emptyStreak + ' 次空回答（服务端多半在限流）');
      emptyStreak = 0;
      i--;                       // 退避后不算一次失败重试，继续等
      continue;
    }
    printer.flush();
    process.stderr.write('\n(空回答，重试 ' + (i + 1) + '/' + tries + ')\n');
    await new Promise(r => setTimeout(r, 1500));
  }
  return text;
}


// 代码模式硬校验：
//  - 没有代码块        -> 要求重写（只输出代码块）
//  - 代码块没闭合(被截断) -> 从断点续写（绝不要求"重写完整代码"，那会越写越慢/触发风控）
function hasCode(t) {
  return /```\s*(c|cpp|h|cc|hpp|sh|bash|shell|python3?|py|java|kt|kotlin|js|ts|go|rs|diff|patch|xml|json|yaml|yml|makefile|cmake|sql)\b/i.test(t) || /#include\s*</.test(t) || /^#!\/(bin|usr)/m.test(t);
}
function codeTruncated(t) {
  const n = (String(t).match(/```/g) || []).length;
  return (n % 2) === 1;
}
function lastLang(t) {
  const m = String(t).match(/```\s*(c|cpp|h|cc|sh|bash|shell|python3?|py)/i);
  return m ? m[1].toLowerCase() : 'c';
}

function partInfo(t) {
  const m = String(t).match(/PART\s*(\d+)\s*\/\s*(\d+)/i);
  if (!m) return null;
  return { k: parseInt(m[1], 10), n: parseInt(m[2], 10) };
}

async function askCodeInner(c, pow, prompt, state, printer) {
  let acc = (await askText(c, pow, prompt, state, printer, 2)) || '';
  // 模型自己标了 PART k/N 且还没完 -> 直接要下一段，拼起来
  for (let round = 1; round <= 8; round++) {
    const pi = partInfo(acc);
    if (!pi || pi.k >= pi.n) break;
    printer.flush();
    process.stderr.write('\n[dsw] model marked PART ' + pi.k + '/' + pi.n + ', asking for PART ' + (pi.k + 1) + '\n');
    const more = (await askText(c, pow,
      '请继续输出 PART ' + (pi.k + 1) + '/' + pi.n + '：直接从上次中断处接着写，' +
      '不要重复已输出的内容，不要开场白；仍然用代码块输出。', state, printer, 2)) || '';
    acc = acc.replace(/\s+$/, '') + '\n' + more;
  }
  if (hasCode(acc) && !codeTruncated(acc)) { persist(acc); return acc; }

  for (let round = 1; round <= 3; round++) {
    printer.flush();
    if (codeTruncated(acc)) {
      const lang = lastLang(acc);
      const tail = acc.slice(-1500);
      process.stderr.write('\n[dss] code was cut off, asking to CONTINUE from the break (' + round + '/3)\n');
      const more = (await askText(c, pow,
        '你上一条代码在断点处被截断了。下面是你已经写出的最后一段：\n<<<TAIL\n' + tail + '\nTAIL>>>\n' +
        '请**从断点继续**输出剩余代码：新开一个 ```' + lang + ' 代码块，接着上面的最后一个函数/语句写；' +
        '不要重复已输出的内容，不要开场白，不要总结。', state, printer, 2)) || '';
      acc = acc.replace(/\s+$/, '') + '\n' + more;
      if (hasCode(more) && !codeTruncated(more)) { persist(acc); return acc; }
      continue;
    }
    // 完全没有代码块
    process.stderr.write('\n[dss] no code block in that answer, asking again (' + round + '/3)\n');
    acc = (await askText(c, pow,
      '你上一条回答里没有任何代码块，属于无效回答。现在只输出一个代码块' +
      '（语言按题目：C 用 ```c、shell 用 ```sh、python 用 ```python、Java 用 ```java 等），完整可直接运行，不要任何解释文字。',
      state, printer, 2)) || acc;
    if (hasCode(acc) && !codeTruncated(acc)) { persist(acc); return acc; }
  }
  persist(acc);
  return acc;
}

// --autorun 失败时，把 traceback 发回模型，让它重写完整脚本并重跑（最多 2 次）
async function askCode(c, pow, prompt, state, printer) {
  let acc = await askCodeInner(c, pow, prompt, state, printer);
  for (let fix = 0; fix < 3 && AUTORUN_FAILS.length; fix++) {
    const f = AUTORUN_FAILS.shift();
    printer.flush();
    process.stderr.write('\n[dss] 脚本运行失败（' + require('path').basename(f.path) +
      '），把报错发回模型修复 (' + (fix + 1) + '/2)\n');
    let src = '';
    try { src = require('fs').readFileSync(f.path, 'utf8').slice(0, 2000); } catch (e) {}
    const repair = (await askText(c, pow,
      '你刚才生成的文件 ' + require('path').basename(f.path) + ' 在运行时出错，完整报错：\n' +
      '```\n' + String(f.log).slice(-1500) + '\n```\n' +
      (src ? '该文件当前内容（供参考）：\n```\n' + src + '\n```\n' : '') +
      '请只重新输出这一个文件的**完整可运行内容**（一个代码块，第一行就是代码，' +
      '代码块内不要出现用法示例、命令行或解释文字；不要只给补丁、不要省略未改动的部分）；' +
      '环境是 OpenWrt + python3.9，产物写到当前目录。', state, printer, 2)) || '';
    if (!hasCode(repair)) {
      process.stderr.write('[dss] 修复回答里没有完整代码块，保留原文件不动\n');
      continue;
    }
    acc = repair;
    // 不再"先删再写"：上次因为写之前删了旧文件、修复又中断，结果文件彻底丢失
    const got = persist(repair);
    if (got && got.length) {
      if (got[0].mode === 'appended') {
        process.stderr.write('[dss] 警告：修复内容是片段，已追加到原文件，可能仍然不可运行\n');
      }
      if (got[0].path !== f.path) {
        try { fs.unlinkSync(f.path); } catch (e) {}   // 模型换了文件名，旧的才删
      }
    }
  }
  return acc;
}

function persist(text, label) {
  try {
    const saved = saveScripts(text, label || CUR_LABEL);
    if (saved.length) {
      process.stderr.write('[dss] ' + saved.length + ' code file(s) [' + (label || CUR_LABEL) + ']: ' +
        saved.map(x => require('path').basename(x.path) + '(' + x.mode + ')').join(', ') + '\n');
      lastSaved = saved;
      maybeAutorun(saved);
    }
    return saved;
  } catch (e) { return []; }
}

// ---------- 自我对话：两个独立会话，一个只问、一个只答 ----------
async function duet(c, pow, topic, rounds, printer, pauseMs, shared) {
  pauseMs = (shared && shared.pause !== undefined) ? shared.pause
          : ((pauseMs === undefined) ? 2000 : pauseMs);
  shared = shared || {};
  const codeMode = !!shared.code;
  const roleA = shared.roleA || '';
  const roleB = shared.roleB || '';
  let aPromptRole = '';
  const maxMs = (shared.maxMinutes > 0) ? shared.maxMinutes * 60000 : 0;
  const fileIds = (shared.fileIds || []).slice();
  const kimiFiles = (shared.kimiFiles || []).slice();
  const useSearch = !!shared.search;
  const total = (rounds > 0) ? (rounds + ' rounds') : 'unlimited, Ctrl-C to stop';
  // 横幅用 ASCII：中文在很多终端里是乱码，看不出来模式有没有生效
  const extra = (fileIds.length ? ', ' + fileIds.length + ' attachment(s)' : '') +
    (useSearch ? ', web search' : '') + (codeMode ? ', CODE MODE' : '') +
    (roleA ? ', roleA=' + roleA : '') + (roleB ? ', roleB=' + roleB : '') +
    ', A=' + (BACKEND_A || 'deepseek') + (MODEL_A ? '/' + MODEL_A : '') +
    ', B=' + (BACKEND_B || 'deepseek') + (MODEL_B ? '/' + MODEL_B : '');
  printer.write('\n=== dss ' + DSS_VERSION + ' SELF-DIALOGUE: ' + topic + ' (' + total + extra + ')\n');
  printer.flush();

  const A = { sessionId: null, parentId: null, fileIds: (shared.fileIds || []).slice(), kimiFiles: (shared.kimiFiles || []).slice(), search: useSearch, filesSent: false, kimiFilesSent: false };   // 提问方
  const B = { sessionId: null, parentId: null, fileIds: (shared.fileIdsB || shared.fileIds || []).slice(), kimiFiles: (shared.kimiFilesB || shared.kimiFiles || []).slice(), search: useSearch, filesSent: false, kimiFilesSent: false };   // 回答方
  try { if ((BACKEND_A || 'deepseek') === 'deepseek') A.sessionId = await session(c); } catch (e) {}
  try { if ((BACKEND_B || 'deepseek') === 'deepseek') B.sessionId = await session(c); } catch (e) {}

  let lastB = '';
  let done = 0;
  const limit = (rounds > 0) ? rounds : 1000;
  const tStartDuet = Date.now();
  for (let i = 1; i <= limit; i++) {
    if (maxMs && (Date.now() - tStartDuet) > maxMs) {
      printer.write('\n=== END === time limit reached (--minutes ' + shared.maxMinutes + '), ' + done + ' round(s) done\n');
      printer.flush();
      return;
    }
    process.stderr.write('[round ' + i + (limit < 1000 ? '/' + limit : '') + '] ' +
      Math.round((Date.now() - tStartDuet) / 1000) + 's elapsed\n');
    let qPrompt = codeMode
      ? (i === 1
        ? '你是【架构师/提问方】。围绕「' + topic + '」向实现者提出第一个能推进落地的问题：' +
          '要求他**以文件为单位分批交付**——第一批给 1~3 个**完整文件**（不要把同一个文件切成两半），' +
          '并在末尾声明总共要分几批（PART 1/N）以及后续每批包含哪些文件；' +
          '只有单个文件确实超过约 400 行时，才允许把它拆成多段（每段注释里保留同一文件名，客户端会合并）。' +
          '明确说明"不要一次性输出全部代码"。语言与技术栈按这个项目/题目的实际情况来定，' +
          '不要套用与项目无关的硬件模板（除非题目确实是驱动/寄存器类）。' +
          '不要讨论文档有没有定义。只输出问题本身。'
        : '实现者刚才交付了这一批内容（共 ' + lastB.length + ' 字符）：\n"""\n' +
          (lastB.length > 150000
            ? lastB.slice(0, 75000) + '\n…（为控制长度省略中段 ' + (lastB.length - 150000) + ' 字符，这是刻意的，不要因此说对方只给了片段）…\n' + lastB.slice(-75000)
            : lastB) +
          '\n"""\n' +
          '请审查这一批：**若上面标了 PART k/N 或明显只交付了部分文件，那是刻意的分批，' +
          '不要因为“文件没写完/只是片段”而要求他重发全部代码**；只指出这一批的具体错误，' +
          '然后要求交付下一批（PART k+1），同样控制在约 ' + BATCH_LINES + ' 行以内。' +
          '你的问题里必须明确写"请给出这一批的完整代码块（PART k+1/N）"。只输出问题本身。')
      : (i === 1
        ? '你在和另一个人对话。请围绕「' + topic + '」向对方提出一个具体、有深度的问题。' +
          '只输出问题本身，不要寒暄、不要解释你在做什么。'
        : '对方刚才这样回答你：\n"""\n' + lastB + '\n"""\n' +
          '请基于这段回答追问一个更深、更具体的问题。只输出问题本身。');

    if (roleA) qPrompt = roleText(roleA, '提问方') + qPrompt;
    if (roleB) aPromptRole = roleText(roleB, '回答方');

    CUR_LABEL = '第 ' + i + ' 轮回答';
    if (pauseMs > 0 && i > 1) await new Promise(r => setTimeout(r, pauseMs));
    printer.write('\n--- 第 ' + i + ' 轮 ---\n[A·提问] ');
    printer.flush();
    let q = '';
    CUR_BACKEND = BACKEND_A || 'deepseek'; CUR_MODEL = MODEL_A || '';
    SAVE_PREFIX = 'a_';        // A 侧的文件统一 a_ 前缀，避免和 B 侧同名合并
    try { q = (await askText(c, pow, qPrompt, A, printer)) || ''; }
    catch (e) {
      printer.flush();
      process.stderr.write('\n=== END === aborted at round ' + i + ': ' + e.message + '\n');
      return;
    }
    q = q.trim();
    if (q.length < 1) {
      printer.flush();
      process.stderr.write('\n(提问方空回答，跳过这一轮)\n');
      continue;
    }

    if (pauseMs > 0) await new Promise(r => setTimeout(r, pauseMs));
    printer.write('\n[B·回答] ');
    printer.flush();
    let a = '';
    SAVE_PREFIX = 'b_';        // B 侧的文件统一 b_ 前缀
    try {
      let aPrompt = codeMode
        ? '你是【实现者/回答方】。对方问：\n"""\n' + q + '\n"""\n' +
          '你必须给出完整、可运行的代码，语言/技术栈按题目和项目实际情况来（' +
          'C 用 ```c 给 .c+必要头文件；shell 用 ```sh；python 用 ```python；' +
          'Java/Kotlin/Android 用 ```java/```kotlin；补丁用 ```diff），' +
          '不要只写片段或伪代码；项目未写清的地方自行取合理值并用注释 /* TODO: 假设 xxx */ 标注，' +
          '绝不允许只说文档没定义或只做元讨论，也不要塞与题目无关的硬件驱动代码。' +
          '**分批输出，但以文件为单位**：这次只输出第 1 批——1~3 个**完整文件**，' +
          '每个文件开头注释里写清文件名；不要把同一个文件按行数切成几块。' +
          '只有单个文件超过约 400 行时才拆段，且每段注释里都写同一个文件名（客户端会拼成一个文件）。' +
          '末尾用 /* PART 1/N: 未完，下一批: xxx */ 说明剩余部分。'
        : '对方问你：\n"""\n' + q + '\n"""\n请直接回答这个问题。';
      if (aPromptRole) aPrompt = aPromptRole + aPrompt;
      CUR_BACKEND = BACKEND_B || 'deepseek'; CUR_MODEL = MODEL_B || '';
      a = ((codeMode ? await askCode(c, pow, aPrompt, B, printer)
                     : await askText(c, pow, aPrompt, B, printer))) || '';
    } catch (e) {
      printer.flush();
      process.stderr.write('\n=== END === aborted at round ' + i + ': ' + e.message + '\n');
      return;
    }
    a = a.trim();
    if (a.length < 1) {
      printer.flush();
      process.stderr.write('\n(回答方空回答，换下一轮)\n');
      continue;
    }
    lastB = a;
    done = i;
  }
  SAVE_PREFIX = '';
  const why = (rounds > 0)
    ? 'round limit reached (--rounds ' + rounds + '); rerun with a bigger --rounds or none for unlimited'
    : 'stopped';
  printer.write('\n=== END === ' + done + ' round(s) done, ' + why + '\n');
  printer.flush();
}

// ---------- main ----------
(async () => {
  const args = process.argv.slice(2);
  // 自检：DSS_DOUBAO_CREDTEST=<file> 验证豆包凭据解析（不打印明文）
  if (process.env.DSS_DOUBAO_CREDTEST) {
    const d = doubaoParseCred(fs.readFileSync(process.env.DSS_DOUBAO_CREDTEST, 'utf8'));
    console.log(JSON.stringify({
      queryLen: d.query.length,
      headers: Object.keys(d.headers).sort(),
      cookie: d.cookie ? (d.cookie.split(';').length + ' pair(s)') : '(none)',
      bot: d.botId, conv: d.convId || '(empty)', local: d.localConv,
      bodyKeys: Object.keys(d.bodyTpl || {}).sort(),
      promptPath: d.bodyTpl && d.bodyTpl.messages && d.bodyTpl.messages[0] &&
        d.bodyTpl.messages[0].content_block && d.bodyTpl.messages[0].content_block[0] &&
        d.bodyTpl.messages[0].content_block[0].content &&
        d.bodyTpl.messages[0].content_block[0].content.text_block &&
        d.bodyTpl.messages[0].content_block[0].content.text_block.text,
    }, null, 1));
    process.exit(0);
  }
  // 自检：DSS_KIMI_CREDTEST=<file> 验证 Kimi 凭据解析（不打印明文）
  if (process.env.DSS_KIMI_CREDTEST) {
    const d = kimiParseCred(fs.readFileSync(process.env.DSS_KIMI_CREDTEST, 'utf8'));
    console.log(JSON.stringify({
      bearer: d.bearer ? (d.bearer.length + 'B ' + d.bearer.slice(0, 8) + '…') : '(none)',
      cookie: d.cookie ? (d.cookie.split(';').length + ' pair(s), kimi-auth=' + (/kimi-auth=/.test(d.cookie) ? 'yes' : 'no')) : '(none)',
      dev: d.dev, sess: d.sess, traffic: d.traffic, model: d.model,
    }));
    process.exit(0);
  }
  // 自检：DSS_KIMI_SELFTEST=<file> 用真机抓到的 Connect 流回放验证解析（不联网）
  if (process.env.DSS_KIMI_SELFTEST) {
    const raw = fs.readFileSync(process.env.DSS_KIMI_SELFTEST);
    const acc = { text: '', think: '', chatId: null, msgId: null, status: null, err: null };
    let used = 0;
    const step = parseInt(process.env.DSS_KIMI_CHUNK || '0', 10);
    if (step > 0) {
      let off = 0, carry = Buffer.alloc(0);
      while (off < raw.length) {
        carry = Buffer.concat([carry, raw.slice(off, off + step)]);
        off += Math.min(step, raw.length - off);
        const u = kimiParseFrames(carry, acc, null);   // 模拟网络分片，残留字节留到下一片
        carry = carry.slice(u);
        used += u;
      }
    } else {
      used = kimiParseFrames(raw, acc, null);
    }
    console.log(JSON.stringify({
      len: acc.text.length, text: acc.text.slice(0, 120), err: acc.err || null, errCode: acc.errCode || null,
      thinkLen: acc.think.length, chatId: acc.chatId, status: acc.status,
      consumed: used, total: raw.length, complete: used === raw.length,
    }));
    process.exit(0);
  }
  const hasDsCred = !!(process.env.DSS_CRED || require('fs').existsSync('/root/.dss/cred'));
  const hasKimiCred = !!(process.env.DSS_KIMI_CRED || require('fs').existsSync(kimiCredPath()));
  const hasApiCred = !!(process.env.DSS_API_CRED || require('fs').existsSync(apiCredPath()));
  const hasDoubaoCred = !!(process.env.DSS_DOUBAO_CRED || require('fs').existsSync(doubaoCredPath()));
  const wantAlt = args.some((a, i) => (a === '--backend-a' || a === '--backend-b') &&
    ['kimi', 'doubao', 'wenxin', 'api', 'glm', 'openai'].indexOf(String(args[i + 1] || '')) >= 0);
  if (!hasDsCred && !hasKimiCred && !hasDoubaoCred && !hasApiCred && !wantAlt) {
    const port = (uciDefaults().port || '88');
    console.error('[dss] 还没有令牌。打开 http://<盒子IP>:' + port + '/token 粘贴一个，');
    console.error('      或者在 LuCI: 服务 -> dss -> Settings 里粘贴 copy(localStorage.userToken)');
    process.exit(2);
  }
  const uciD = uciDefaults();
  // LuCI 里设的默认值（命令行参数会覆盖它）
  if (uciD.pace && !args.includes('--pace')) {
    const v = parseInt(uciD.pace, 10);
    if (v > 0) paceMs = Math.max(1000, v * 1000);
  }
  if (uciD.kimi_think === '1') KIMI_THINK = true;
  if (uciD.kimi_tools === '1') KIMI_TOOLS = true;
  if (uciD.kimi_plugin === '1') KIMI_PLUGIN = true;
  if (uciD.search === '1' && !args.includes('--search')) args.push('--search');
  if (uciD.code === '1' && !args.includes('--code')) args.push('--code');
  for (const [k, flag] of [['backend_a','--backend-a'], ['backend_b','--backend-b'],
                           ['model_a','--model-a'], ['model_b','--model-b']]) {
    if (uciD[k] && !args.includes(flag)) { args.push(flag); args.push(uciD[k]); }
  }
  if (uciD.pause && !args.includes('--pause')) { args.push('--pause'); args.push(uciD.pause); }
  // 命令行没写任何 --role* 时才用 LuCI 里勾的角色；写了就完全听命令行的
  const _hasRoleCli = args.includes('--role') || args.includes('--role-a') || args.includes('--role-b');
  if (!_hasRoleCli) {
    if (uciD.role_a) { args.push('--role-a'); args.push(uciD.role_a); }
    if (uciD.role_b) { args.push('--role-b'); args.push(uciD.role_b); }
    if (uciD.role_a || uciD.role_b) {
      process.stderr.write('[role] 使用 LuCI 设置的角色：A=' + (uciD.role_a || '-') +
        ' B=' + (uciD.role_b || '-') + '（命令行写 --role/--role-a/--role-b 可覆盖）\n');
    }
  }
  if (uciD.rounds && !args.includes('--rounds')) { args.push('--rounds'); args.push(uciD.rounds); }
  if (uciD.minutes && !args.includes('--minutes')) { args.push('--minutes'); args.push(uciD.minutes); }
  let c;
  try { c = cred(); } catch (e) { c = { token: '', cookie: '' }; }
  // 只用 Kimi 时不必初始化 DeepSeek 的 PoW worker（真机实测省掉 ~1.4s 启动）
  const _argv = (f) => { const i = process.argv.indexOf(f); return i >= 0 ? String(process.argv[i + 1] || '') : ''; };
  const _ba0 = _argv('--backend-a') || _argv('--backend-b') || 'deepseek';
  const _bb0 = _argv('--backend-b') || _ba0;
  const NO_DEEPSEEK = (_ba0 !== 'deepseek' && _bb0 !== 'deepseek');
  const pow = (c.token && !NO_DEEPSEEK) ? makePoller() : null;
  const printer = makePrinter();
  const state = { sessionId: null, parentId: null, fileIds: [], search: false };

  async function oneShot(prompt) {
    CUR_BACKEND = BACKEND_A || 'deepseek';
    CUR_MODEL = MODEL_A || '';
    if (CUR_BACKEND === 'deepseek' && !state.sessionId) {
      try { state.sessionId = await session(c); } catch (e) {}
    }
    process.stdout.write('\n[' + (CUR_BACKEND === 'kimi' ? 'Kimi' : CUR_BACKEND === 'doubao' ? 'Doubao' : CUR_BACKEND === 'wenxin' ? 'Wenxin' : (CUR_BACKEND === 'api' || CUR_BACKEND === 'glm' || CUR_BACKEND === 'openai') ? 'API' : 'DeepSeek') +
      (CUR_MODEL ? ' ' + CUR_MODEL : '') + (CUR_BACKEND === 'kimi' && KIMI_THINK ? ' think' : '') +
      (CUR_BACKEND === 'kimi' && KIMI_TOOLS ? ' tools' : '') + (CUR_BACKEND === 'kimi' && KIMI_PLUGIN ? ' plugin' : '') + '] ');
    try {
      const txt = CODE_MODE ? await askCode(c, pow, prompt, state, printer)
                            : await askText(c, pow, prompt, state, printer, 3);
      if (CODE_MODE && (!lastSaved || !lastSaved.length)) {
        process.stderr.write('\n[dss] 本轮没有产出任何文件（回答被截断/被中断，或模型没给完整代码块）。' +
          '可以重跑，或把要求写得更具体（文件名、页数、保存位置）。\n');
      }
      return txt || '';
    } catch (e) {
      // 只有 DeepSeek 才会走“换令牌”流程；API/Kimi/豆包/文心的 401 直接报自己的错
      if (CUR_BACKEND !== 'deepseek') throw e;
      if (!/401|403|Missing Token|token/i.test(e.message)) throw e;
      printer.flush();
      console.log('\n[令牌失效] 在电脑浏览器打开 http://<盒子IP>:88/token');
      console.log('           登录 chat.deepseek.com 后点一下那个书签，写好会自动重试…');
      const changed = await waitForCredChange(300000);
      if (!changed) throw e;
      Object.assign(c, cred());
      state.sessionId = null;
      try { state.sessionId = await session(c); } catch (e2) {}
      Object.assign(c, cred());
      const txt = await ask(c, pow, prompt, state, printer);
      process.stdout.write('\n');
      return txt || '';
    }
    process.stdout.write('\n');
    return '';
  }

  if (pow) await waitReady(pow, 10000).catch(e => { console.error('pow: ' + e.message); });

  // dss --duet [--rounds N] [话题] —— 两个会话自问自答
  let isDuet = false;
  const di = args.indexOf('--duet');
  if (di >= 0) { isDuet = true; args.splice(di, 1); }
  let rounds = 0;            // 0 = 不限轮数，一直问下去，Ctrl-C 停
  let ri = args.indexOf('--rounds');
  if (ri < 0) ri = args.indexOf('--round');   // 兼容手写的 --round N
  if (ri >= 0) {
    rounds = parseInt(args[ri + 1], 10) || 0;
    args.splice(ri, 2);
  }
  // 文件附件：支持 `dss !upload /path "问题"`（可多个）、`dss --file /path ... "问题"`
  const files = [];
  const filesA = [];
  const filesB = [];
  for (;;) {
    const ui = args.indexOf('!upload');
    if (ui < 0) break;
    const p = args[ui + 1];
    if (p && p[0] !== '-') { files.push(p); args.splice(ui, 2); } else { args.splice(ui, 1); }
  }
  for (;;) {
    const fi = args.indexOf('--file-a');
    if (fi < 0) break;
    const p = args[fi + 1];
    if (p && p[0] !== '-') { filesA.push(p); args.splice(fi, 2); } else { args.splice(fi, 1); }
  }
  for (;;) {
    const fi = args.indexOf('--file-b');
    if (fi < 0) break;
    const p = args[fi + 1];
    if (p && p[0] !== '-') { filesB.push(p); args.splice(fi, 2); } else { args.splice(fi, 1); }
  }
  for (;;) {
    const di2 = args.indexOf('--url-dom');
    if (di2 < 0) break;
    const u = args[di2 + 1];
    if (u && u[0] !== '-') { const got = fetchUrl(u, true); if (got) files.push(got); args.splice(di2, 2); }
    else args.splice(di2, 1);
  }
  for (;;) {
    const ui2 = args.indexOf('--url');
    if (ui2 < 0) break;
    const u = args[ui2 + 1];
    if (u && u[0] !== '-') { const got = fetchUrl(u, false); if (got) files.push(got); args.splice(ui2, 2); }
    else args.splice(ui2, 1);
  }
  for (;;) {
    const fi = args.indexOf('--file');
    if (fi < 0) break;
    const p = args[fi + 1];
    if (p && p[0] !== '-') { files.push(p); args.splice(fi, 2); } else { args.splice(fi, 1); }
  }
  if (args.includes('--search')) { state.search = true; args.splice(args.indexOf('--search'), 1); }
  let codeMode = false;
  if (args.includes('--code')) { codeMode = true; args.splice(args.indexOf('--code'), 1); }
  if (args.includes('--autorun')) { AUTORUN = true; args.splice(args.indexOf('--autorun'), 1); }
  if (args.includes('--run')) { AUTORUN = true; args.splice(args.indexOf('--run'), 1); }
  CODE_MODE = codeMode;
  let backA = '', backB = '', modelA = '', modelB = '';
  for (const [key, set] of [['--api-think', () => API_THINK = true],
    ['--backend-a', v => backA = v], ['--backend-b', v => backB = v],
                            ['--model-a', v => modelA = v], ['--model-b', v => modelB = v]]) {
    const idx = args.indexOf(key);
    if (idx >= 0 && args[idx + 1]) { set(args[idx + 1]); args.splice(idx, 2); }
  }
  // --backend-a/--model-a 对所有模式生效（单发、duet、管道批处理、交互）；duet/单发分支后面会再覆盖
  if (backA || backB) BACKEND_A = backA || backB || 'deepseek';
  if (backB) BACKEND_B = backB;
  if (modelA) MODEL_A = modelA;
  if (modelB) MODEL_B = modelB;
  let roleA = '', roleB = '', roleS = '';
  for (const key of ['--role-a', '--role-b', '--role']) {
    const idx = args.indexOf(key);
    if (idx >= 0 && args[idx + 1]) {
      const val = args[idx + 1];
      if (key === '--role-a') roleA = val;
      else if (key === '--role-b') roleB = val;
      else { roleS = val; roleA = val; roleB = val; }
      args.splice(idx, 2);
    }
  }
  // 单发（--role 没写）时沿用 A 侧角色，否则 LuCI 勾的角色对单发命令无效
  if (!roleS && roleA) roleS = roleA;
  if (args.includes('--kimi-think')) { KIMI_THINK = true; args.splice(args.indexOf('--kimi-think'), 1); }
  if (args.includes('--kimi-tools')) { KIMI_TOOLS = true; args.splice(args.indexOf('--kimi-tools'), 1); }
  if (args.includes('--kimi-plugin')) { KIMI_PLUGIN = true; args.splice(args.indexOf('--kimi-plugin'), 1); }
  let pauseArg = 0;
  const pa = args.indexOf('--pause');
  if (pa >= 0 && args[pa + 1]) { pauseArg = Math.max(0, parseInt(args[pa + 1], 10) || 0); args.splice(pa, 2); }
  const bi = args.indexOf('--batch');
  if (bi >= 0 && args[bi + 1]) { BATCH_LINES = Math.max(40, parseInt(args[bi + 1], 10) || 200); args.splice(bi, 2); }
  let paceArg = 0;
  const pi2 = args.indexOf('--pace');
  if (pi2 >= 0) { paceArg = parseInt(args[pi2 + 1], 10) || 0; args.splice(pi2, 2); }
  if (paceArg > 0) paceMs = Math.max(1000, paceArg * 1000);
  let maxMinutes = 0;
  const mi = args.indexOf('--minutes');
  if (mi >= 0) { maxMinutes = parseInt(args[mi + 1], 10) || 0; args.splice(mi, 2); }

  if (isDuet) {
    if (rounds > 0) rounds = Math.max(1, Math.min(rounds, 1000));
    // 组合：--file / !upload 先传上去，--search 透传给双方
    {
      const eA = backA || 'deepseek';
      const eB = backB || backA || 'deepseek';
      for (const be of (eA === eB ? [eA] : [eA, eB])) {
        await uploadAttachments(state, c, pow, be, files);
      }
    }
    BACKEND_A = (backA || 'deepseek');
    BACKEND_B = (backB || backA || 'deepseek');
    // 混跑会让网页版被判异常（实测：deepseek+glm 混跑 duet，deepseek 立刻被踢下线要求重登）
    // 所以强制两侧同一后端：deepseek+deepseek 或 api+api
    if ((BACKEND_A || 'deepseek') !== (BACKEND_B || 'deepseek')) {
      console.error('\n[dss] duet 两侧必须使用同一个后端：' + BACKEND_A + ' + ' + BACKEND_B + ' 不行。');
      console.error('      实测混跑会让网页版被判异常（DeepSeek 会被踢下线要求重登）。');
      console.error('      用 --backend-a deepseek --backend-b deepseek，或 --backend-a api --backend-b api。\n');
      process.exit(2);
    }
    MODEL_A = modelA; MODEL_B = modelB;
    const badBackend = [BACKEND_A, BACKEND_B].find(b => b && ['deepseek','kimi','doubao','wenxin','api','glm','openai'].indexOf(b) < 0);
    if (badBackend) {
      console.error('\n[dss] 后端 "' + badBackend + '" 还不支持，只认 deepseek / kimi / doubao / wenxin / api（或别名 glm、openai）。\n');
      process.exit(2);
    }
    // 附件按侧上传：--file 两边都给，--file-a / --file-b 只给指定侧
    const listA = files.concat(filesA), listB = files.concat(filesB);
    const stateA = { fileIds: [], kimiFiles: [], search: state.search };
    const stateB2 = { fileIds: [], kimiFiles: [], search: state.search };
    await uploadAttachments(stateA, c, pow, BACKEND_A || 'deepseek', listA);
    if (BACKEND_B === BACKEND_A && listB.length === listA.length &&
        listB.every((x, i) => x === listA[i])) {
      stateB2.fileIds = stateA.fileIds.slice();
      stateB2.kimiFiles = stateA.kimiFiles.slice();
    } else {
      await uploadAttachments(stateB2, c, pow, BACKEND_B || 'deepseek', listB);
    }

    await duet(c, pow, args.join(' ').trim() || '人工智能与人类未来', rounds, printer, 1000,
      { fileIds: stateA.fileIds, kimiFiles: stateA.kimiFiles,
        fileIdsB: stateB2.fileIds, kimiFilesB: stateB2.kimiFiles,
        search: state.search, code: codeMode, maxMinutes, roleA, roleB,
        pause: pauseArg > 0 ? pauseArg * 1000 : undefined });
    process.exit(0);
  }

  if (args.length && args[0] !== '-i') {
    await uploadAttachments(state, c, pow, backA || 'deepseek', files.concat(filesA));
    let q0 = args.join(' ');
    if (roleS) q0 = roleText(roleS, '回答方') + q0;
    if (codeMode) {
      q0 = (AUTORUN
        ? '（硬要求：直接给出完整、可编译运行的代码，用 ```语言 代码块输出；' +
          '脚本会立刻在路由器上执行（OpenWrt + python3.9，已装 python-pptx 1.0.2 / lxml / Pillow），' +
          '所以：所有产物写到当前工作目录、不要交互式输入、不要 pip install、用相对路径；' +
          '注意 python-pptx 的占位符文本框默认只有 1 个段落——追加要点必须用 ' +
          'text_frame.add_paragraph()，禁止直接索引 paragraphs[1]；'
        : '（硬要求：直接给出完整、可编译运行的代码，用 ```语言 代码块输出；') +
        '不要只讲原理或只讨论文档；文档没写清的地方自行取保守值并用 TODO 标注）\n' + q0;
    }
    CUR_LABEL = '一次性提问';
    BACKEND_A = BACKEND_B = backA || backB || 'deepseek';
    MODEL_A = modelA; MODEL_B = modelB;
    await oneShot(q0);
    process.exit(0);
  }

  if (!process.stdin.isTTY) {
    const all = fs.readFileSync(0, 'utf8').split('\n');
    for (const raw of all) {
      const q = raw.trim();
      if (!q || q === 'exit' || q === 'quit') continue;
      try { await oneShot(q); }
      catch (e) { printer.flush(); console.log('\n[错误] ' + e.message); }
    }
    process.exit(0);
  }

  console.log('dss ' + DSS_VERSION + ' - chat.deepseek.com (web account) / plain text, type exit to quit');
  const rl = require('readline').createInterface({ input: process.stdin, output: process.stdout, prompt: '[shell]> ' });

  let busy = false;
  const queued = [];
  let lastScripts = [];                 // 上一条回答里保存的脚本
  const runResults = new Map();         // script path -> {code, out}

  function printHelp() {
    console.log([
      '',
      '  !list           list scripts saved from the last answer',
      '  !cat N          show script N',
      '  !run N          run script N (cwd /root/.dss/out, dangerous cmds refused)',
      '  !run N send     run it and feed the output back to DeepSeek',
      '  !out N          feed script N\'s last output back to DeepSeek',
      '  !upload PATH    upload a file and attach it to the next questions',
      '  !files          list attached files',
      '  !refile         re-send the attachments with the next question',
      '  !detach         clear attachments',
      '  !search         toggle web search (联网搜索) on/off',
      '  !help           this help',
      '  exit            quit',
      '',
      '  Scripts are only ever stored/executed under ' + OUTDIR,
      '  (dangerous commands are refused; the token is never passed to scripts)',
      ''].join('\n'));
  }

  async function sendOutput(path, res, label) {
    const body = res.out.length > 6000 ? res.out.slice(0, 6000) + '\n...(truncated)' : res.out;
    const prompt = '这是脚本 ' + (label || path) + ' 的执行输出（exit ' + res.code + '）：\n```\n' + body +
      '\n```\n请根据输出继续：如果报错就给出修正后的完整脚本；如果正常就简要说明结果。';
    process.stdout.write('\n[DeepSeek] ');
    let txt = '';
    try {
      txt = (await ask(c, pow, prompt, state, printer)) || '';
    } catch (e) {
      printer.flush();
      console.log('\n[错误] ' + e.message);
      return '';
    }
    process.stdout.write('\n');
    return txt;
  }

  async function handleCommand(line) {
    const parts = line.replace(/^!/, '').trim().split(/\s+/);
    const cmd = (parts[0] || '').toLowerCase();
    const n = parseInt(parts[1], 10);

    if (cmd === '' || cmd === 'help') { printHelp(); return; }

    if (cmd === 'list' || cmd === 'ls') {
      if (!lastScripts.length) { console.error('[dss] 上一条回答里没有脚本（有的会显示在这里）'); return; }
      lastScripts.forEach((x, i) => {
        let size = 0;
        try { size = fs.statSync(x.path).size; } catch (e) {}
        console.log('  ' + (i + 1) + ') [' + (x.lang || 'sh') + '] ' + x.path + '  ' + x.lines + ' lines, ' + size + ' bytes' +
          (runResults.has(x.path) ? '  [ran, exit ' + runResults.get(x.path).code + ']' : ''));
      });
      return;
    }

    if (cmd === 'upload') {
      const fp = line.replace(/^!\S+\s*/, '').trim();
      if (!fp) { console.error('[dss] 用法: !upload /path/to/file'); return; }
      const targets = expandUploadTargets(fp) || [];
      if (!targets.length) { console.error('[dss] nothing to upload: ' + fp); return; }
      if (targets.length > 1) console.error('[dss] ' + fp + ' -> ' + targets.length + ' file(s)');
      for (const t of targets) {
        process.stdout.write('[dss] uploading ' + t.name + ' (' + CUR_BACKEND + ') ... ');
        try {
          if (CUR_BACKEND === 'kimi') {
            const f = await kimiUploadFile(t.path, t.name);
            state.kimiFiles = state.kimiFiles || [];
            state.kimiFiles.push(f);
            state.kimiFilesSent = false;
            console.log('ok  kimi id=' + f.id + '  (' + f.size + ' bytes)');
          } else {
            const f = await uploadFile(c, pow, t.path, t.name);
            if (!state.fileIds.includes(f.id)) state.fileIds.push(f.id);
            state.filesSent = false;
            console.log('ok  id=' + f.id + '  (' + f.size + ' bytes)');
          }
        } catch (e) {
          console.log('failed: ' + e.message);
        }
      }
      console.error('[dss] attached: ' + ((state.fileIds || []).length + (state.kimiFiles || []).length) + ' file(s); 下一个问题会带上');
      return;
    }

    if (cmd === 'refile' || cmd === 'reattach') {
      state.filesSent = false;
      state.kimiFilesSent = false;
      console.error('[dss] 下一次提问会重新附上 ' +
        ((state.fileIds || []).length + (state.kimiFiles || []).length) + ' 个文件');
      return;
    }

    if (cmd === 'files') {
      if (!state.fileIds.length) { console.error('[dss] 没有附件'); return; }
      state.fileIds.forEach((id, i) => console.log('  ' + (i + 1) + ') ' + id));
      return;
    }

    if (cmd === 'detach') {
      state.fileIds = [];
      state.filesSent = false;
      state.kimiFiles = [];
      state.kimiFilesSent = false;
      console.error('[dss] attachments cleared');
      return;
    }

    if (cmd === 'search') {
      const v = (parts[1] || '').toLowerCase();
      if (v === 'on') state.search = true;
      else if (v === 'off') state.search = false;
      else state.search = !state.search;
      console.error('[dss] web search: ' + (state.search ? 'ON' : 'OFF'));
      return;
    }

    // 只有 cat / run / out 需要脚本编号
    if (!lastScripts.length) { console.error('[dss] 没有可用的脚本，先让 DeepSeek 给一段脚本'); return; }
    if (!(n >= 1 && n <= lastScripts.length)) { console.error('[dss] 编号 1..' + lastScripts.length); return; }
    const item = lastScripts[n - 1];

    if (cmd === 'cat') {
      process.stdout.write(fs.readFileSync(item.path, 'utf8'));
      return;
    }

    if (cmd === 'run') {
      let src = '';
      try { src = fs.readFileSync(item.path, 'utf8'); } catch (e) { console.error('[dss] 读不到 ' + item.path); return; }
      const bad = checkScript(src);
      if (bad) {
        console.error('[dss] 拒绝执行：' + bad);
        console.log('      （危险命令一律不执行；要跑就自己改脚本再手动执行）');
        return;
      }
      console.error('[dss] run ' + item.path + '  cwd=' + OUTDIR + '  timeout=' + (RUN_TIMEOUT / 1000) + 's');
      const res = await runScript(item.path);
      showOutput(res);
      try { fs.writeFileSync(item.path + '.out', res.out, { mode: 0o600 }); } catch (e) {}
      runResults.set(item.path, res);
      if (parts.includes('send')) {
        const txt = await sendOutput(item.path, res, '#' + n);
        const saved = saveScripts(txt);
        if (saved.length) { lastScripts = saved; console.error('[dss] ' + saved.length + ' new script(s) saved (!list)'); }
      }
      return;
    }

    if (cmd === 'out') {
      const res = runResults.get(item.path);
      if (!res) { console.error('[dss] 这个脚本还没运行过，先用 !run ' + n); return; }
      const txt = await sendOutput(item.path, res, '#' + n);
      const saved = saveScripts(txt);
      if (saved.length) { lastScripts = saved; console.error('[dss] ' + saved.length + ' new script(s) saved (!list)'); }
      return;
    }

    console.error('[dss] 不认识的命令：!' + cmd + '（!help 看帮助）');
  }

  async function run(q) {
    busy = true;
    rl.pause();
    let txt = '';
    try {
      txt = await oneShot(q);
    } catch (e) {
      printer.flush();
      console.log('\n[错误] ' + e.message);
      if (/401|403/.test(e.message)) {
        console.log('（token 过期了：在电脑浏览器打开 http://<盒子IP>:88/token ，');
        console.log('  登录 chat.deepseek.com 把 copy(localStorage.userToken) 粘进页面保存）');
      }
    }
    const saved = saveScripts(txt);
    if (saved.length) {
      lastScripts = saved;
      console.error('[dss] ' + saved.length + ' script(s) saved to ' + OUTDIR +
        '   (!list / !run 1 / !run 1 send)');
    }
    const next = queued.shift();
    if (next) { await run(next); return; }
    busy = false;
    rl.resume();
    rl.prompt();
  }

  rl.on('line', line => {
    const q = line.trim();
    if (q === 'exit' || q === 'quit') { rl.close(); return; }
    if (!q) { if (!busy) rl.prompt(); return; }
    if (q[0] === '!') {
      if (busy) { console.error('[dss] 等这次回答结束再执行命令'); return; }
      busy = true;
      rl.pause();
      handleCommand(q).catch(e => console.error('[dss] ' + e.message)).then(() => {
        busy = false;
        rl.resume();
        rl.prompt();
      });
      return;
    }
    if (busy) {
      if (queued.length < 1) { queued.push(q); console.log('\n(这次回答还没结束，你这条已排队)'); }
      return;
    }
    run(q);
  });

  rl.prompt();
  rl.on('close', () => process.exit(0));
})().catch(e => { console.error('fatal: ' + e.message); process.exit(1); });
