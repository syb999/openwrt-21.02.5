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
const DSS_VERSION = '1.0-43';
const OUTDIR = process.env.DSS_OUTDIR || '/root/.dss/out';
// 只保留一个目录：/root/.dss/out（重启后还在，LuCI 的"文件下载"从这里取）
// 对话日志：给 LuCI 的"监听对话"页面 tail
const CONV_LOG = process.env.DSS_LOG || '/tmp/dss/conversation.log';
const RUN_TIMEOUT = parseInt(process.env.DSS_RUN_TIMEOUT || '60000', 10);
const MAX_SHOW = 4000;

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
  const re = /```([A-Za-z0-9_+-]*)\n([\s\S]*?)```/g;
  let m;
  while ((m = re.exec(String(text || ''))) !== null) {
    const lang = (m[1] || '').toLowerCase();
    const code = m[2];
    const isC = ['c', 'h', 'cpp', 'cc'].includes(lang);
    const isShell = ['sh', 'bash', 'shell', 'zsh', 'ash', 'console', 'shell-session'].includes(lang) ||
      /^\s*#!\s*\/.*\b(ba|a|z)?sh\b/.test(code);
    const isPy = ['python', 'python3', 'py'].includes(lang) ||
      /^\s*#!\s*\/.*python/.test(code);
    if (isC) out.push({ lang: 'c', code });
    else if (isShell) out.push({ lang: 'sh', code });
    else if (isPy) out.push({ lang: 'py', code });
  }
  return out;
}

function saveScripts(answerText) {
  let blocks = parseBlocks(answerText);
  // 分段输出（PART k/N）时把相邻同语言块拼起来，避免存成一堆半截文件
  const merged = [];
  for (const b of blocks) {
    const prev = merged[merged.length - 1];
    if (prev && prev.lang === b.lang) prev.code += '\n' + b.code;
    else merged.push({ lang: b.lang, code: b.code });
  }
  blocks = merged;
  if (!blocks.length) return [];
  try {
    fs.mkdirSync(OUTDIR, { recursive: true, mode: 0o700 });
    fs.chmodSync(OUTDIR, 0o700);
  } catch (e) {}
  const saved = [];
  const ts = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 14);
  blocks.forEach((b, i) => {
    const ext = (b.lang === 'py') ? '.py' : (b.lang === 'c' ? '.c' : '.sh');
    // 模型给的文件名优先（meminfo_available.c / slic_si3217x_ar9341.h ...）
    let base = guessFileName(b.code, b.lang) || (ts + '-' + (i + 1) + ext);
    base = require('path').basename(base).replace(/[^A-Za-z0-9._-]/g, '_');
    if (!/\.[A-Za-z0-9]+$/.test(base)) base += ext;
    let path = OUTDIR + '/' + base;
    // 同名不同内容 -> 加 -2 / -3，避免互相覆盖
    if (fs.existsSync(path)) {
      let old2 = '';
      try { old2 = fs.readFileSync(path, 'utf8'); } catch (e) {}
      if (old2 !== b.code) {
        const dot = base.lastIndexOf('.');
        const stem = base.slice(0, dot), tail = base.slice(dot);
        for (let k = 2; k < 50; k++) {
          const cand = OUTDIR + '/' + stem + '-' + k + tail;
          if (!fs.existsSync(cand)) { base = stem + '-' + k + tail; path = cand; break; }
        }
      } else {
        return;   // 内容一样就不重复存（在 forEach 回调里）
      }
    }
    try {
      fs.writeFileSync(path, b.code, { mode: 0o600 });
      fs.chmodSync(path, 0o600);
      saved.push({ path, lang: b.lang, lines: b.code.split('\n').length });
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
  if (!chal) throw new Error('challenge failed: ' + chalRes.code + ' ' + String(chalRes.text).slice(0, 200));

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
            const waf = /<html|Request Blocked|Rate Limit/i.test(t);
            end(false, new Error(path + ' HTTP ' + res.statusCode +
              (waf ? ' —— 被 WAF 限流/拦截了，等几分钟再试（或重新从浏览器复制 cookie 到 /etc/dss/cred 第2行）'
                  : ': ' + t.slice(0, 200))));
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
            if (!line.startsWith('data:')) continue;
            const p = line.slice(5).trim();
            if (process.env.DSS_DEBUG) console.error('[sse] ' + p.slice(0, 200));
            if (!p || p === '[DONE]') continue;
            let j;
            try { j = JSON.parse(p); } catch (e) { continue; }
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
    const waitMs = paceMs - (Date.now() - lastReqAt);
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

function session(c) {
  return httpJson(c, '/api/v0/chat_session/create', { character_id: null }).then(r => {
    const d = r.j && r.j.data && r.j.data.biz_data;
    return (d && d.chat_session && d.chat_session.id) || null;
  });
}

async function ask(c, pow, prompt, state, printer) {
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
  // 每条回答都扫一遍代码块：duet / 一次性模式也要存进 /root/.dss/out（否则该目录永远是空的）
  try {
    const savedNow = saveScripts(cur.acc);
    if (savedNow.length) {
      lastSaved = savedNow;
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




const RATE_RE = /(发送过于频繁|请求过于频繁|过于频繁|操作频繁|稍后重试|请稍候|rate limit|too many requests|try again later)/i;

function looksRateLimited(text) {
  if (!text) return false;
  const t = String(text).trim();
  if (t.length > 400) return false;              // 长回答里出现这些词不算
  return RATE_RE.test(t);
}

async function backoff(why) {
  const wait = Math.min(paceMs, PACE_MAX);
  process.stderr.write('\n[rate-limited] ' + (why || '') + ' -> wait ' + Math.round(wait / 1000) +
    's, then retry (pace now ' + Math.round(paceMs / 1000) + 's)\n');
  await new Promise(r => setTimeout(r, wait));
  paceMs = Math.min(Math.round(paceMs * 2), PACE_MAX);
}

// 空回答重试：服务端偶发（限流/风控）会给 200 但内容为空
let emptyStreak = 0;
let lastSaved = [];

async function askText(c, pow, prompt, state, printer, tries) {
  if (process.env.DSS_DRY === '1') {
    process.stderr.write('\n----- PROMPT (dry-run) -----\n' + prompt + '\n----- END -----\n');
    return '（dry-run）';
  }
  tries = tries || 3;
  let text = '';
  for (let i = 0; i < tries; i++) {
    text = (await ask(c, pow, prompt, state, printer)) || '';
    if (looksRateLimited(text)) {
      await backoff('server said: ' + String(text).trim().slice(0, 60));
      i--;                       // 限流不算一次失败重试
      continue;
    }
    // 只要不是完全空就算有效（"2"、"ok" 这种短答案很常见）
    if (text.trim().length >= 1) {
      paceMs = Math.max(3000, Math.round(paceMs * 0.8));
      emptyStreak = 0;
      return text;
    }
    // 连续空回答通常就是被风控了（服务端只回 200+空，不给文字提示）
    emptyStreak++;
    if (emptyStreak >= 3) {
      paceMs = Math.min(Math.round(paceMs * 2), PACE_MAX);
      process.stderr.write('\n[rate-limited?] ' + emptyStreak + ' empty answers in a row -> waiting ' +
        Math.round(paceMs / 1000) + 's and slowing down to ' + Math.round(paceMs / 1000) + 's/request\n');
      await new Promise(r => setTimeout(r, paceMs));
      emptyStreak = 0;
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

async function askCode(c, pow, prompt, state, printer) {
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
  if (hasCode(acc) && !codeTruncated(acc)) return acc;

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
      if (hasCode(more) && !codeTruncated(more)) return acc;
      continue;
    }
    // 完全没有代码块
    process.stderr.write('\n[dss] no code block in that answer, asking again (' + round + '/3)\n');
    acc = (await askText(c, pow,
      '你上一条回答里没有任何代码块，属于无效回答。现在只输出一个代码块' +
      '（语言按题目：C 用 ```c、shell 用 ```sh、python 用 ```python、Java 用 ```java 等），完整可直接运行，不要任何解释文字。',
      state, printer, 2)) || acc;
    if (hasCode(acc) && !codeTruncated(acc)) return acc;
  }
  return acc;
}

// ---------- 自我对话：两个独立会话，一个只问、一个只答 ----------
async function duet(c, pow, topic, rounds, printer, pauseMs, shared) {
  pauseMs = (pauseMs === undefined) ? 1000 : pauseMs;
  shared = shared || {};
  const codeMode = !!shared.code;
  const roleA = shared.roleA || '';
  const roleB = shared.roleB || '';
  let aPromptRole = '';
  const maxMs = (shared.maxMinutes > 0) ? shared.maxMinutes * 60000 : 0;
  const fileIds = (shared.fileIds || []).slice();
  const useSearch = !!shared.search;
  const total = (rounds > 0) ? (rounds + ' rounds') : 'unlimited, Ctrl-C to stop';
  // 横幅用 ASCII：中文在很多终端里是乱码，看不出来模式有没有生效
  const extra = (fileIds.length ? ', ' + fileIds.length + ' attachment(s)' : '') +
    (useSearch ? ', web search' : '') + (codeMode ? ', CODE MODE' : '') +
    (roleA ? ', roleA=' + roleA : '') + (roleB ? ', roleB=' + roleB : '');
  printer.write('\n=== dss ' + DSS_VERSION + ' SELF-DIALOGUE: ' + topic + ' (' + total + extra + ')\n');
  printer.flush();

  const A = { sessionId: null, parentId: null, fileIds: fileIds, search: useSearch, filesSent: false };   // 提问方
  const B = { sessionId: null, parentId: null, fileIds: fileIds, search: useSearch, filesSent: false };   // 回答方
  try { A.sessionId = await session(c); } catch (e) {}
  try { B.sessionId = await session(c); } catch (e) {}

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
          '要求他**分批交付**——第一批只给最核心的 1~2 个文件（约 ' + BATCH_LINES + ' 行以内），' +
          '并在末尾声明总共要分几批（PART 1/N）以及后续每批包含什么；' +
          '明确说明"不要一次性输出全部代码"。语言与技术栈按这个项目/题目的实际情况来定，' +
          '不要套用与项目无关的硬件模板（除非题目确实是驱动/寄存器类）。' +
          '不要讨论文档有没有定义。只输出问题本身。'
        : '实现者刚才交付了这一批内容（共 ' + lastB.length + ' 字符）：\n"""\n' +
          (lastB.length > 8000 ? lastB.slice(0, 4000) + '\n…（中间省略）…\n' + lastB.slice(-4000) : lastB) +
          '\n"""\n' +
          '请审查它：指出具体缺哪段代码、哪处逻辑/接口不对，并要求他补出完整可运行的文件，' +
          '不要泛泛而谈；不要因为"只看到片段"而重复要求补开头（上面已是完整内容）。' +
          '你的问题里必须明确写"请给出完整可编译的代码块"。只输出问题本身。')
      : (i === 1
        ? '你在和另一个人对话。请围绕「' + topic + '」向对方提出一个具体、有深度的问题。' +
          '只输出问题本身，不要寒暄、不要解释你在做什么。'
        : '对方刚才这样回答你：\n"""\n' + lastB + '\n"""\n' +
          '请基于这段回答追问一个更深、更具体的问题。只输出问题本身。');

    if (roleA) qPrompt = roleText(roleA, '提问方') + qPrompt;
    if (roleB) aPromptRole = roleText(roleB, '回答方');

    if (i > 1 && pauseMs > 0) await new Promise(r => setTimeout(r, pauseMs));
    printer.write('\n--- 第 ' + i + ' 轮 ---\n[A·提问] ');
    printer.flush();
    let q = '';
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

    printer.write('\n[B·回答] ');
    printer.flush();
    let a = '';
    try {
      let aPrompt = codeMode
        ? '你是【实现者/回答方】。对方问：\n"""\n' + q + '\n"""\n' +
          '你必须给出完整、可运行的代码，语言/技术栈按题目和项目实际情况来（' +
          'C 用 ```c 给 .c+必要头文件；shell 用 ```sh；python 用 ```python；' +
          'Java/Kotlin/Android 用 ```java/```kotlin；补丁用 ```diff），' +
          '不要只写片段或伪代码；项目未写清的地方自行取合理值并用注释 /* TODO: 假设 xxx */ 标注，' +
          '绝不允许只说文档没定义或只做元讨论，也不要塞与题目无关的硬件驱动代码。' +
          '**分批输出**：如果总量超过约 ' + BATCH_LINES + ' 行，这次只输出第 1 批（1~2 个文件），' +
          '末尾用 /* PART 1/N: 未完，下一批: xxx */ 说明剩余部分；' +
          '不要把所有代码挤进一条回答，客户端会自动要后续批次。'
        : '对方问你：\n"""\n' + q + '\n"""\n请直接回答这个问题。';
      if (aPromptRole) aPrompt = aPromptRole + aPrompt;
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
  const why = (rounds > 0)
    ? 'round limit reached (--rounds ' + rounds + '); rerun with a bigger --rounds or none for unlimited'
    : 'stopped';
  printer.write('\n=== END === ' + done + ' round(s) done, ' + why + '\n');
  printer.flush();
}

// ---------- main ----------
(async () => {
  const args = process.argv.slice(2);
  // 没令牌时给一句人话（不要 fatal 堆栈）
  if (!process.env.DSS_CRED && !require('fs').existsSync('/root/.dss/cred')) {
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
  if (uciD.search === '1' && !args.includes('--search')) args.push('--search');
  if (uciD.code === '1' && !args.includes('--code')) args.push('--code');
  if (uciD.role_a && !args.includes('--role-a')) { args.push('--role-a'); args.push(uciD.role_a); }
  if (uciD.role_b && !args.includes('--role-b')) { args.push('--role-b'); args.push(uciD.role_b); }
  if (uciD.rounds && !args.includes('--rounds')) { args.push('--rounds'); args.push(uciD.rounds); }
  if (uciD.minutes && !args.includes('--minutes')) { args.push('--minutes'); args.push(uciD.minutes); }
  const c = cred();
  const pow = makePoller();
  const printer = makePrinter();
  const state = { sessionId: null, parentId: null, fileIds: [], search: false };

  async function oneShot(prompt) {
    if (!state.sessionId) {
      try { state.sessionId = await session(c); } catch (e) {}
    }
    process.stdout.write('\n[DeepSeek] ');
    try {
      const txt = await askText(c, pow, prompt, state, printer, 3);
      return txt || '';
    } catch (e) {
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

  await waitReady(pow, 10000).catch(e => { console.error('pow: ' + e.message); });

  // dss --duet [--rounds N] [话题] —— 两个会话自问自答
  let isDuet = false;
  const di = args.indexOf('--duet');
  if (di >= 0) { isDuet = true; args.splice(di, 1); }
  let rounds = 0;            // 0 = 不限轮数，一直问下去，Ctrl-C 停
  const ri = args.indexOf('--rounds');
  if (ri >= 0) {
    rounds = parseInt(args[ri + 1], 10) || 0;
    args.splice(ri, 2);
  }
  // 文件附件：支持 `dss !upload /path "问题"`（可多个）、`dss --file /path ... "问题"`
  const files = [];
  for (;;) {
    const ui = args.indexOf('!upload');
    if (ui < 0) break;
    const p = args[ui + 1];
    if (p && p[0] !== '-') { files.push(p); args.splice(ui, 2); } else { args.splice(ui, 1); }
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
    for (const fp of files) {
      const targets = expandUploadTargets(fp) || [];
      if (!targets.length) { console.error('[dss] nothing to upload: ' + fp); continue; }
      if (targets.length > 1) console.error('[dss] ' + fp + ' -> ' + targets.length + ' file(s)');
      for (const t of targets) {
        process.stdout.write('[dss] uploading ' + t.name + ' ... ');
        try {
          const f = await uploadFile(c, pow, t.path, t.name);
          state.fileIds.push(f.id);
          console.log('ok (' + f.size + ' bytes, ' + f.status + ')');
        } catch (e) { console.log('failed: ' + e.message); }
      }
    }
    await duet(c, pow, args.join(' ').trim() || '人工智能与人类未来', rounds, printer, 1000,
      { fileIds: state.fileIds, search: state.search, code: codeMode, maxMinutes, roleA, roleB });
    process.exit(0);
  }

  if (args.length && args[0] !== '-i') {
    for (const fp of files) {
      const targets = expandUploadTargets(fp) || [];
      if (!targets.length) { console.error('[dss] nothing to upload: ' + fp); continue; }
      if (targets.length > 1) console.error('[dss] ' + fp + ' -> ' + targets.length + ' file(s)');
      for (const t of targets) {
        process.stdout.write('[dss] uploading ' + t.name + ' ... ');
        try {
          const f = await uploadFile(c, pow, t.path, t.name);
          state.fileIds.push(f.id);
          console.log('ok (' + f.size + ' bytes, ' + f.status + ')');
        } catch (e) { console.log('failed: ' + e.message); }
      }
    }
    let q0 = args.join(' ');
    if (roleS) q0 = roleText(roleS, '回答方') + q0;
    if (codeMode) {
      q0 = '（硬要求：直接给出完整、可编译运行的代码，用 ```语言 代码块输出；' +
        '不要只讲原理或只讨论文档；文档没写清的地方自行取保守值并用 TODO 标注）\n' + q0;
    }
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
        process.stdout.write('[dss] uploading ' + t.name + ' ... ');
        try {
          const f = await uploadFile(c, pow, t.path, t.name);
          if (!state.fileIds.includes(f.id)) state.fileIds.push(f.id);
          state.filesSent = false;
          console.log('ok  id=' + f.id + '  (' + f.size + ' bytes)');
        } catch (e) {
          console.log('failed: ' + e.message);
        }
      }
      console.error('[dss] attached: ' + state.fileIds.length + ' file(s); 下一个问题会带上');
      return;
    }

    if (cmd === 'refile' || cmd === 'reattach') {
      state.filesSent = false;
      console.error('[dss] 下一次提问会重新附上 ' + state.fileIds.length + ' 个文件');
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
