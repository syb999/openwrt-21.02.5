'use strict';
/*
 * pow_thread.js - 把站点的 PoW worker（pow_worker.js + 8138 chunk + sha3 wasm）
 * 放进一个 worker_thread 里跑，主线程不再被 wasm 的暴力搜索堵住
 * （否则预取 pow 的那 0.5~1s 里，终端输入/输出会卡住）。
 */
const { parentPort } = require('worker_threads');
const fs = require('fs');
const vm = require('vm');

const ASSETS = process.env.DSS_ASSETS || '/opt/dss';

const self = {};
const ctx = {
  self, console,
  TextEncoder, TextDecoder, WebAssembly, URL, Response,
  setTimeout, clearTimeout, queueMicrotask,
  location: 'https://chat.deepseek.com/',
  fetch: async () => new Response(fs.readFileSync(ASSETS + '/sha3_wasm_bg.wasm'),
    { headers: { 'content-type': 'application/wasm' } }),
};
ctx.globalThis = ctx;
ctx.onmessage = null;
ctx.self.location = ctx.location;
const mainChunk = fs.readFileSync(ASSETS + '/pow_chunk8138.js', 'utf8');
ctx.importScripts = () => { vm.runInContext(mainChunk, ctx); };

let pending = null;
ctx.postMessage = m => {
  if (!pending) return;
  const p = pending;
  pending = null;
  if (m.type === 'pow-answer') p.resolve(m.answer);
  else p.reject(new Error(String((m.error && m.error.message) || m.error)));
};

vm.createContext(ctx);
vm.runInContext(fs.readFileSync(ASSETS + '/pow_worker.js', 'utf8'), ctx);

function ready() { return typeof ctx.onmessage === 'function'; }

const queue = [];
let busy = false;

function pump() {
  if (busy || queue.length === 0) return;
  const msg = queue.shift();
  if (!ready()) {
    parentPort.postMessage({ id: msg.id, error: 'pow worker not ready' });
    return;
  }
  busy = true;
  const done = () => { busy = false; setImmediate(pump); };
  pending = {
    resolve: a => { parentPort.postMessage({ id: msg.id, answer: a }); done(); },
    reject: e => { parentPort.postMessage({ id: msg.id, error: String(e.message || e) }); done(); },
  };
  ctx.onmessage({ data: { type: 'pow-challenge', challenge: msg.challenge } });
}

parentPort.on('message', msg => {
  if (msg.type === 'solve') { queue.push(msg); pump(); }
});

// 每 50ms 报一次就绪，直到 worker 模块加载完
const timer = setInterval(() => {
  if (ready()) { clearInterval(timer); parentPort.postMessage({ type: 'ready' }); }
}, 50);
