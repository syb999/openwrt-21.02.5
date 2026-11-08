#!/usr/bin/env node
'use strict';

/*
 * dss - a no-JavaScript chat page for DeepSeek, meant for text browsers
 * (elinks / w3m / lynx) and for any device on the LAN.
 *
 * There is no JS in the page at all: everything is plain HTML forms and
 * links, the node process talks to https://api.deepseek.com/chat/completions
 * on the server side and prints the answer as <pre> text.
 */

const http = require('http');
const https = require('https');
const fs = require('fs');

const KEYFILE = process.env.DSS_KEYFILE || '/etc/dss.key';
// 同 dss：令牌放 /root/.dss/cred，不放 /etc（/etc 会进 sysupgrade 备份）
function credPath() {
  if (process.env.DSS_CRED_PATH) return process.env.DSS_CRED_PATH;
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
    }
  } catch (e) { /* ignore */ }
  return NEW;
}
const DSS_CRED = credPath();
const PORT = parseInt(process.env.DSS_PORT || '88', 10);
const BIND = process.env.DSS_BIND || '0.0.0.0';
const MODEL = process.env.DSS_MODEL || 'deepseek-chat';
const ENDPOINT = 'https://api.deepseek.com/chat/completions';
const MAX_MSGS = 20;

const sessions = new Map();

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function apiKey() {
  if (process.env.DEEPSEEK_API_KEY) return process.env.DEEPSEEK_API_KEY.trim();
  try { return fs.readFileSync(KEYFILE, 'utf8').trim(); } catch (e) { return ''; }
}

function cookieSid(req) {
  const c = req.headers.cookie || '';
  const m = /(?:^|;\s*)sid=([A-Za-z0-9]+)/.exec(c);
  return m ? m[1] : null;
}

function history(sid) {
  if (!sessions.has(sid)) sessions.set(sid, []);
  return sessions.get(sid);
}




function saveCred(tok) {
  if (!tok || tok.length < 20) {
    throw new Error('token too short (' + (tok ? tok.length : 0) + ' chars), refused');
  }
  let prev = '';
  try { prev = fs.readFileSync(DSS_CRED, 'utf8'); } catch (e) {}
  if (prev.trim()) {
    try {
      fs.writeFileSync(DSS_CRED + '.prev', prev, { mode: 0o600 });
      fs.chmodSync(DSS_CRED + '.prev', 0o600);
    } catch (e) { /* backup is best effort */ }
  }
  let prevCookie = '';
  try { prevCookie = (prev.split('\n')[1] || '').trim(); } catch (e) {}
  fs.writeFileSync(DSS_CRED, tok + '\n' + prevCookie + '\n', { mode: 0o600 });
  fs.chmodSync(DSS_CRED, 0o600);
  console.log('[token] saved ' + tok.length + ' chars, tail ' + tok.slice(-4) +
    ' (previous kept as ' + DSS_CRED + '.prev)');
}

function extractToken(raw) {
  let s = String(raw === undefined || raw === null ? '' : raw).trim();
  for (let i = 0; i < 4; i++) {
    if (!s) break;
    if (s.length > 2 && s[0] === '"' && s[s.length - 1] === '"') {
      s = s.slice(1, -1).replace(/\\"/g, '"').trim();
    }
    if (/^[A-Za-z0-9+/=_.:-]{20,}$/.test(s)) return s;
    try {
      const j = JSON.parse(s);
      if (typeof j === 'string') { s = j; continue; }
      if (j && typeof j.value === 'string') { s = j.value; continue; }
    } catch (e) { /* not json */ }
    const m = s.match(/[A-Za-z0-9+/=_.:-]{40,}/);
    if (m) return m[0];
    break;
  }
  return s;
}

function maskTok(t) {
  if (!t) return '(空)';
  return t.length + ' chars, head ' + t.slice(0, 4) + '... tail ' + t.slice(-4);
}

// 存之前先验一下令牌，别把废票写进去
function verifyToken(tok) {
  return new Promise(resolve => {
    const req = https.request({
      host: 'chat.deepseek.com', path: '/api/v0/users/settings', method: 'GET',
      headers: {
        'Authorization': 'Bearer ' + tok,
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36',
        'Referer': 'https://chat.deepseek.com/',
        'Accept': '*/*',
      },
    }, res => {
      let b = '';
      res.setEncoding('utf8');
      res.on('data', d => { b += d; });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(b); } catch (e) {}
        resolve({ ok: res.statusCode === 200 && j && j.code === 0, msg: (j && j.msg) || ('HTTP ' + res.statusCode) });
      });
    });
    req.setTimeout(15000, () => req.destroy(new Error('timeout')));
    req.on('error', e => resolve({ ok: false, msg: '网络: ' + e.message }));
    req.end();
  });
}

function dssTokenState() {
  try {
    const lines = fs.readFileSync(DSS_CRED, 'utf8').split('\n');
    const t = (lines[0] || '').trim();
    if (!t) return 'not set';
    return 'set (' + t.length + ' chars, ends with ' + t.slice(-4) + ')';
  } catch (e) { return 'not set'; }
}

function layout(title, inner) {
  return '<!DOCTYPE html>\n<html><head><meta charset="utf-8">' +
    '<title>' + esc(title) + '</title></head><body>\n' + inner +
    '\n<hr><p><a href="/">[chat]</a> <a href="/new">[new dialog]</a> <a href="/token">[dss token]</a> ' +
    '<a href="/key">[api key]</a> | model: ' + esc(MODEL) + '</p>\n' +
    '</body></html>\n';
}

function renderToken(host, note) {
  let out = '<h3>DeepSeek 登录令牌</h3>\n';
  if (note) out += '<p><b>' + esc(note) + '</b></p>\n';
  out += '<p>current: ' + esc(dssTokenState()) + '</p>\n';
  out += '<ol>' +
    '<li>In Chrome open chat.deepseek.com and log in</li>' +
    '<li>Press F12 -&gt; Console, run: <code>copy(localStorage.userToken)</code></li>' +
    '<li>Paste into the box below and click save (quotes / JSON are fine)</li>' +
    '</ol>\n';
  out += '<form method="post" action="/token">' +
    '<textarea name="token" rows="4" cols="76"></textarea><br>' +
    '<input type="submit" value="save token">' +
    '</form>\n';
  out += '<p>The token is written to ' + esc(DSS_CRED) + ' (0600) and verified against ' +
    'api.deepseek.com before saving. Nothing is kept in the URL or in this page.</p>\n';
  return layout('dss token', out);
}

function renderChat(sid, note) {
  const msgs = history(sid);
  let out = '<h3>DeepSeek - text mode</h3>\n';

  if (note) out += '<p><b>' + esc(note) + '</b></p>\n';

  if (!msgs.length) {
    out += '<p><i>no messages yet - type below and press send</i></p>\n';
  } else {
    for (const m of msgs) {
      const who = m.role === 'user' ? '\u3010\u4f60\u3011' : '\u3010DeepSeek\u3011';
      out += '<p><b>' + who + '</b></p>\n<pre>' + esc(m.content) + '</pre>\n';
    }
  }

  out += '<form method="post" action="/send">' +
    '<textarea name="msg" rows="4" cols="72"></textarea><br>' +
    '<input type="submit" value="send">' +
    '</form>\n';

  if (!apiKey()) {
    out += '<p><b>api key not set</b> - open <a href="/key">api key</a> ' +
      'and paste your key from platform.deepseek.com</p>\n';
  }
  return layout('dss', out);
}

function renderKey(note) {
  const k = apiKey();
  let out = '<h3>DeepSeek api key</h3>\n';
  if (note) out += '<p><b>' + esc(note) + '</b></p>\n';
  out += '<p>current: ' + (k ? 'set (ends with ' + esc(k.slice(-4)) + ')' : 'not set') + '</p>\n';
  out += '<form method="post" action="/key">' +
    '<input type="text" name="key" size="60" value="">' +
    '<input type="submit" value="save">' +
    '</form>\n';
  return layout('dss key', out);
}

function send(res, body, code) {
  res.writeHead(code || 200, {
    'Content-Type': 'text/html; charset=utf-8',
    'Content-Length': Buffer.byteLength(body)
  });
  res.end(body);
}

function redirect(res, to, sid) {
  const h = { Location: to, 'Content-Length': 0 };
  if (sid) h['Set-Cookie'] = 'sid=' + sid + '; Path=/; HttpOnly';
  res.writeHead(302, h);
  res.end();
}

function postBody(req, cb) {
  let buf = '';
  req.on('data', d => {
    buf += d;
    if (buf.length > 65536) req.destroy();
  });
  req.on('end', () => cb(buf));
}

function decodeForm(s) {
  const out = {};
  for (const part of String(s).split('&')) {
    if (!part) continue;
    const i = part.indexOf('=');
    const k = i < 0 ? part : part.slice(0, i);
    const v = i < 0 ? '' : part.slice(i + 1);
    try {
      out[decodeURIComponent(k.replace(/\+/g, ' '))] =
        decodeURIComponent(v.replace(/\+/g, ' '));
    } catch (e) { /* ignore bad escapes */ }
  }
  return out;
}

function askDeepSeek(msgs, cb) {
  const body = JSON.stringify({
    model: MODEL,
    messages: msgs.slice(-MAX_MSGS),
    stream: false
  });
  const req = https.request({
    hostname: 'api.deepseek.com',
    path: '/chat/completions',
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(body),
      'Accept': 'application/json'
    }
  }, r => {
    let data = '';
    r.setEncoding('utf8');
    r.on('data', c => { data += c; });
    r.on('end', () => {
      let j = null;
      try { j = JSON.parse(data); } catch (e) { /* keep raw */ }
      if (r.statusCode === 200 && j && j.choices && j.choices[0]) {
        cb(null, j.choices[0].message.content);
      } else if (j && j.error) {
        cb('API ' + r.statusCode + ': ' + (j.error.message || JSON.stringify(j.error)));
      } else {
        cb('API ' + r.statusCode + ': ' + data.slice(0, 300));
      }
    });
  });
  req.setTimeout(180000, () => req.destroy(new Error('timeout after 180s')));
  req.on('error', e => cb('network: ' + e.message));
  req.write(body);
  req.end();
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1');
  const sid = cookieSid(req);

  if (req.method === 'GET' && u.pathname === '/token') {
    // 只渲染页面。令牌不进 URL（不然会留在浏览器历史/代理日志里）
    send(res, renderToken(req.headers.host || '127.0.0.1:88', u.searchParams.get('m')));
    return;
  }

  if (req.method === 'POST' && u.pathname === '/token') {
    postBody(req, async buf => {
      const f = decodeForm(buf);
      const tok = extractToken(f.token);
      if (!tok) return send(res, renderToken(req.headers.host || '127.0.0.1:88', 'token 为空，没保存'), 400);
      const v = await verifyToken(tok);
      if (!v.ok) {
        return send(res, renderToken(req.headers.host || '127.0.0.1:88',
          '收到 ' + maskTok(tok) + '，但验证失败（' + v.msg + '），没有保存'), 400);
      }
      try {
        saveCred(tok);
      } catch (e) {
        return send(res, renderToken(req.headers.host || '', '写 ' + DSS_CRED + ' 失败: ' + e.message), 500);
      }
      send(res, layout('token ok', '<h3>✅ 已收到</h3><p>令牌已写入 ' + esc(DSS_CRED) +
        '（0600）。回终端继续用 <code>dss</code> 就行。</p>'));
    });
    return;
  }

  if (req.method === 'GET' && u.pathname === '/') {
    const note = u.searchParams.get('m');
    const s = sid || require('crypto').randomBytes(8).toString('hex');
    if (!sid) return redirect(res, '/', s);
    send(res, renderChat(s, note));
    return;
  }

  if (req.method === 'GET' && u.pathname === '/key') {
    const s = sid || require('crypto').randomBytes(8).toString('hex');
    if (!sid) return redirect(res, '/key', s);
    send(res, renderKey(null));
    return;
  }

  if (req.method === 'GET' && u.pathname === '/new') {
    const s = sid || require('crypto').randomBytes(8).toString('hex');
    sessions.set(s, []);
    redirect(res, '/', s);
    return;
  }

  if (req.method === 'POST' && u.pathname === '/key') {
    const s = sid || require('crypto').randomBytes(8).toString('hex');
    postBody(req, buf => {
      const f = decodeForm(buf);
      const k = (f.key || '').trim();
      if (!k) return redirect(res, '/key', s);
      try {
        fs.writeFileSync(KEYFILE, k + '\n', { mode: 0o600 });
        fs.chmodSync(KEYFILE, 0o600);
      } catch (e) {
        return send(res, layout('error', '<p><b>cannot write ' +
          esc(KEYFILE) + ': ' + esc(e.message) + '</b></p>'), 500);
      }
      redirect(res, '/', s);
    });
    return;
  }

  if (req.method === 'POST' && u.pathname === '/send') {
    const s = sid || require('crypto').randomBytes(8).toString('hex');
    postBody(req, buf => {
      const f = decodeForm(buf);
      const msg = (f.msg || '').trim();
      if (!msg) return redirect(res, '/', s);
      if (!apiKey()) return redirect(res, '/?m=api+key+not+set', s);
      const msgs = history(s);
      msgs.push({ role: 'user', content: msg });
      askDeepSeek(msgs, (err, answer) => {
        if (err) {
          msgs.push({ role: 'assistant', content: '(' + err + ')' });
          send(res, renderChat(s, 'request failed'));
          return;
        }
        msgs.push({ role: 'assistant', content: answer });
        send(res, renderChat(s, null));
      });
    });
    return;
  }

  send(res, layout('404', '<p>no such page</p>'), 404);
});

server.listen(PORT, BIND, () => {
  console.log('dss listening on ' + BIND + ':' + PORT + ' model=' + MODEL +
    ' key=' + (apiKey() ? 'set' : 'missing'));
});
