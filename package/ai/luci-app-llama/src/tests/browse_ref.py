#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""browse.py — 登录态网站抓取（selenium + chrome-headless-shell in glibc chroot）

接口（和 docwrite.py 一致，C 侧 fork python3 -c <本脚本> <op>，数据走 stdin/stdout）：
    argv[1] = op
    stdin   = 参数 JSON
    stdout  = 结果 JSON（永远是单个对象，诊断信息走 stderr）

op:
    probe   打开 URL，探测可填的输入框和可点的按钮 → 给模型挑
    login   一次性完成：打开 → 按 steps 填表/点击 → 等 → 导出 cookie
    cookie  打开 URL 后导出 cookie（已经登录过的站点，靠 steps 也能带登录）
    shot    截图存文件（调试用）

为什么 login 必须一次跑完：这块板子上 chromedriver 不能带 --user-data-dir
（chroot 里它找不到 DevToolsActivePort，60 秒后报 unable to discover open pages），
所以会话没法跨进程复用。每次调用都是新浏览器，登录流程得在同一个进程里走完。
"""

import sys
import json
import os
import re
import subprocess
import time

CDRV = "/opt/chrome-root/cdrv.sh"
CHROME_BIN = "/opt/chrome/chrome-headless-shell"
WORKDIR = "/tmp/browse"
os.environ.setdefault("TESSDATA_PREFIX", "/usr/share/tessdata")
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36")

# 反自动化检测：webdriver 标记、空 plugins、空 languages、chrome.runtime
STEALTH_JS = r"""
(function () {
  try { Object.defineProperty(navigator, 'webdriver', { get: function () { return undefined; } }); } catch (e) {}
  try {
    if (!navigator.plugins || !navigator.plugins.length) {
      Object.defineProperty(navigator, 'plugins', { get: function () { return [1, 2, 3, 4, 5]; } });
    }
  } catch (e) {}
  try {
    if (!navigator.languages || !navigator.languages.length) {
      Object.defineProperty(navigator, 'languages', { get: function () { return ['zh-CN', 'zh', 'en']; } });
    }
  } catch (e) {}
  try { window.chrome = window.chrome || { runtime: {} }; } catch (e) {}
})();
"""

# 探测页面上的输入框 / 按钮 / iframe，并把元素引用挂到 window.__bz_in / __bz_btn
JS_PROBE = r"""
return (() => {
  const vis = el => {
    try {
      const r = el.getBoundingClientRect();
      const s = getComputedStyle(el);
      return r.width > 1 && r.height > 1 && s.display !== 'none'
             && s.visibility !== 'hidden' && s.opacity !== '0';
    } catch (e) { return false; }
  };
  const labelOf = el => {
    try {
      if (el.labels && el.labels.length) return (el.labels[0].innerText || '').trim().slice(0, 40);
      const p = el.closest('label');
      if (p) return (p.innerText || '').trim().slice(0, 40);
      const prev = el.previousElementSibling;
      if (prev && prev.innerText && prev.innerText.trim().length < 20) return prev.innerText.trim();
    } catch (e) {}
    return '';
  };

  /* ---- Shadow DOM 穿透：Web Component 里的登录框藏在 shadowRoot 里，
          document.querySelectorAll 完全看不到。把所有能访问的 root 收集起来，
          open 模式的能穿，closed 模式的穿不透（只能靠外部结构猜）。---- */
  const roots = [];
  const collect = (root, depth) => {
    if (!root || depth > 6 || roots.indexOf(root) >= 0) return;
    roots.push(root);
    let els = [];
    try { els = root.querySelectorAll('*'); } catch (e) { return; }
    els.forEach(el => { if (el.shadowRoot) collect(el.shadowRoot, depth + 1); });
  };
  collect(document, 0);
  window.__bz_roots = roots;

  window.__bz_in = [];
  window.__bz_btn = [];
  window.__bz_tab = [];
  const inputs = [];
  const buttons = [];
  const SKIP_T = ['hidden','submit','button','image','file','reset','checkbox','radio'];

  /* 生成一个能直接拿去 find_element(By.CSS_SELECTOR) 的选择器。
     导出复现脚本时要用 —— 脚本里不能写 window.__bz_in[0]，那只能是本进程内的。
     优先级：id > name > type > class > 标签名。 */
  const selOf = el => {
    try {
      if (el.id && /^[A-Za-z][\w-]*$/.test(el.id)) return '#' + el.id;
      const tag = (el.tagName || '').toLowerCase();
      if (el.name) return tag + "[name='" + String(el.name).replace(/'/g, "\\'") + "']";
      if (el.type && tag === 'input') return tag + "[type='" + el.type + "']";
      const cls = ((el.className || '') + '').trim().split(/\s+/).filter(function(c){ return c; });
      if (cls.length) return tag + '.' + cls.slice(0, 2).join('.');
      return tag;
    } catch (e) { return ''; }
  };

  /* 生成 XPath。CSS 选择器对"靠 JS 绑定事件的 div/a"经常定位不到
     （没有 name/id/class），XPath 按可见文字反而稳。 */
  const xpathOf = el => {
    try {
      if (el.id && /^[A-Za-z][\w-]*$/.test(el.id)) return "//*[@id='" + el.id + "']";
      const tag = (el.tagName || '').toLowerCase();
      const q = s => String(s).replace(/'/g, "\\'");
      if (tag === 'input' || tag === 'textarea') {
        if (el.name) return "//" + tag + "[@name='" + q(el.name) + "']";
        if (el.placeholder) return "//" + tag + "[@placeholder='" + q(el.placeholder) + "']";
        const t = (el.type || '').toLowerCase();
        if (t && t !== 'text') return "//" + tag + "[@type='" + t + "']";
      }
      const txt = ((el.innerText || el.value || '') + '').trim().replace(/\s+/g, ' ');
      if (txt && txt.length <= 30) {
        if (tag === 'button') return "//button[normalize-space(.)='" + q(txt) + "']";
        if (tag === 'a')      return "//a[normalize-space(.)='" + q(txt) + "']";
        if (tag === 'input')  return "//input[@value='" + q(txt) + "']";
        return "//" + tag + "[normalize-space(text())='" + q(txt) + "']";
      }
      const cls = ((el.className || '') + '').trim().split(/\s+/)[0];
      if (cls) return "//" + tag + "[contains(@class,'" + q(cls) + "')]";
      return "";
    } catch (e) { return ""; }
  };

  /* 这个元素是不是"只靠 JS 事件"才能点。
     href / onclick / form 都没有，说明是组件自己 addEventListener 挂的 ——
     用 CSS/XPath 找到之后必须 el.click() 触发，光看 HTML 看不出可点。 */
  const jsOnlyOf = el => {
    try {
      const tag = (el.tagName || '');
      if (el.getAttribute && el.getAttribute('onclick')) return false;
      const href = el.getAttribute && el.getAttribute('href');
      if (href && href !== '#' && href.indexOf('javascript:') !== 0) return false;
      if (el.form || (tag === 'BUTTON' && el.type === 'submit')) return false;
      if (tag === 'INPUT' && (el.type === 'submit' || el.type === 'button')) return false;
      return true;   /* 其余（div/span/a[href=#]）基本都靠 JS */
    } catch (e) { return false; }
  };

  /* 点得到吗：中心点上最顶层元素是不是它自己（被遮罩挡住就是 false） */
  const hitOf = el => {
    try {
      const r = el.getBoundingClientRect();
      if (r.width < 2 || r.height < 2) return false;
      const x = r.left + Math.min(r.width / 2, 24);
      const y = r.top + r.height / 2;
      if (x < 0 || y < 0 || x > window.innerWidth || y > window.innerHeight) return false;
      const top = document.elementFromPoint(x, y);
      return !!(top && (top === el || el.contains(top) || (top.contains && top.contains(el))));
    } catch (e) { return false; }
  };

  const roundRect = el => {
    try {
      const r = el.getBoundingClientRect();
      return [Math.round(r.left), Math.round(r.top),
              Math.round(r.width), Math.round(r.height)];
    } catch (e) { return null; }
  };

  const addInput = (el, ce) => {
    window.__bz_in.push(el);
    inputs.push({
      i: window.__bz_in.length - 1,
      type: ce ? 'editable' : ((el.type || 'text') + '').toLowerCase(),
      name: ((el.name || '') + '').slice(0, 40),
      id: ((el.id || '') + '').slice(0, 40),
      placeholder: ((el.placeholder || el.getAttribute('data-ph') || '') + '').slice(0, 40),
      autocomplete: ((el.autocomplete || '') + '').slice(0, 30),
      aria: ((el.getAttribute('aria-label') || el.getAttribute('aria-labelledby') || '') + '').slice(0, 40),
      label: labelOf(el),
      ce: ce ? 1 : 0,
      sel: selOf(el),
      xpath: xpathOf(el),
      rect: roundRect(el),
      hit: hitOf(el),
      jsOnly: jsOnlyOf(el)
    });
  };

  roots.forEach(root => {
    /* ---- 输入框：input / textarea / contenteditable ---- */
    let els = [];
    try {
      els = root.querySelectorAll(
        'input, textarea, [contenteditable=""], [contenteditable="true"], [role=textbox]');
    } catch (e) {}
    els.forEach(el => {
      const t = ((el.type || 'text') + '').toLowerCase();
      if (SKIP_T.indexOf(t) >= 0) return;
      if (el.disabled || el.readOnly) return;
      if (!vis(el)) return;
      const ce = (el.isContentEditable === true)
                 || el.getAttribute('contenteditable') === 'true'
                 || el.getAttribute('contenteditable') === '';
      addInput(el, ce);
    });

    /* ---- 标准按钮 ---- */
    let bs = [];
    try {
      bs = root.querySelectorAll(
        'button, input[type=submit], input[type=button], a[role=button], [role=button]');
    } catch (e) {}
    bs.forEach(el => {
      if (!vis(el)) return;
      const txt = ((el.innerText || el.value || '') + '').trim().replace(/\s+/g, ' ').slice(0, 40);
      if (!txt) return;
      window.__bz_btn.push(el);
      buttons.push({
        i: window.__bz_btn.length - 1,
        tag: (el.tagName || '') + '',
        type: ((el.type || '') + '').slice(0, 20),
        text: txt,
        sel: selOf(el),
        xpath: xpathOf(el),
        rect: roundRect(el),
        hit: hitOf(el),
        jsOnly: jsOnlyOf(el)
      });
    });

    /* ---- 伪装成按钮的元素：div/span/a，靠 cursor:pointer + 文案认 ----
           提交按钮用 div 做是现在很常见的写法（Vue/React 组件库尤其多） */
    let ds = [];
    try { ds = root.querySelectorAll('div, span, a, li, p'); } catch (e) {}
    ds.forEach(el => {
      if (!vis(el)) return;
      if (el.closest && el.closest('button, [role=button]')) return;
      let s;
      try { s = getComputedStyle(el); } catch (e) { return; }
      if (s.cursor !== 'pointer') return;
      const txt = ((el.innerText || '') + '').trim().replace(/\s+/g, ' ');
      if (!txt || txt.length > 24) return;
      /* 中文按钮常被排版成"登 录"/"登  录"，得允许字间空格 */
      if (!/登\s*录|登\s*陆|提交|确认|下一步|继续|立即|login|sign\s?in|submit|next|continue|enter/i.test(txt)) return;
      if (window.__bz_btn.indexOf(el) >= 0) return;
      window.__bz_btn.push(el);
      buttons.push({
        i: window.__bz_btn.length - 1,
        tag: (el.tagName || '') + '',
        type: 'fake',
        text: txt.slice(0, 40),
        sel: selOf(el),
        xpath: xpathOf(el),
        rect: roundRect(el),
        hit: hitOf(el),
        jsOnly: jsOnlyOf(el)
      });
      window.__bz_tab.push(el);
    });
  });

  /* ---- 人机验证检测：图片验证码 / reCAPTCHA / hCaptcha / 滑块 ---- */
  let captcha = false, captcha_kind = '';
  const probeTxt = (el) => ((el && (el.id + ' ' + el.className + ' ' + el.src)) + '').toLowerCase();
  const has = (sel) => {
    let n = false;
    roots.forEach(r => { try { if (!n && r.querySelector(sel)) n = true; } catch (e) {} });
    return n;
  };
  if (has('iframe[src*=recaptcha], .g-recaptcha, #g-recaptcha, [data-sitekey]')) {
    captcha = true; captcha_kind = 'recaptcha';
  } else if (has('iframe[src*=hcaptcha], .h-captcha')) {
    captcha = true; captcha_kind = 'hcaptcha';
  } else if (has('.geetest, [class*=slider], [class*=drag-verify]')) {
    captcha = true; captcha_kind = 'slider';
  } else {
    const CAP = "img[src*=captcha], img[src*=verify], img[src*=code], img[id*=captcha], "
              + "img[id*=cap], img[class*=captcha], img[alt*=验证码], img[alt*=captcha], "
              + "canvas[class*=captcha], [class*=captcha] img, [id*=captcha] img";
    if (has(CAP)) { captcha = true; captcha_kind = 'image'; }
  }

  let title = '';
  try { title = document.title || ''; } catch (e) {}

  /* iframes 必须是列表：_probe_anywhere 靠它逐个 switch_to.frame 进去找表单 */
  const iframes = [];
  try {
    document.querySelectorAll('iframe').forEach((f, i) => {
      iframes.push({i: i, src: ((f.src || f.getAttribute('src') || '') + '').slice(0, 60)});
    });
  } catch (e) {}

  return {
    url: location.href,
    title: title,
    inputs: inputs,
    buttons: buttons,
    captcha: captcha,
    captcha_kind: captcha_kind,
    roots: roots.length,
    iframes: iframes,
    mem_mb: 0
  };
})()
"""

JS_STORAGE = r"""
return (() => {
  const out = {local: {}, session: {}};
  try {
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i);
      out.local[k] = localStorage.getItem(k);
    }
  } catch (e) {}
  try {
    for (let i = 0; i < sessionStorage.length; i++) {
      const k = sessionStorage.key(i);
      out.session[k] = sessionStorage.getItem(k);
    }
  } catch (e) {}
  return out;
})();
"""

JS_FILL = r"""
/* 执行时带上 {el: 元素, value: 字符串}，这里用 arguments[0] 拿 */
return (() => {
  const arg = arguments[0] || {};
  const target = arg.el ? arg.el
    : (arg.i === undefined ? null : (window.__bz_in || [])[arg.i]);
  if (!target) return {ok: false, why: 'no element'};
  const v = arg.value === undefined ? '' : String(arg.value);
  const fire = (node, type) => {
    try {
      node.dispatchEvent(new Event(type, {bubbles: true, cancelable: true}));
      node.dispatchEvent(new InputEvent(type, {bubbles: true, cancelable: true, data: v}));
    } catch (e) {
      try { node.dispatchEvent(new Event(type, {bubbles: true})); } catch (e2) {}
    }
  };

  /* contenteditable：没有 value，得改 textContent + 手动派发 InputEvent */
  if (target.isContentEditable
      || target.getAttribute('contenteditable') === 'true'
      || target.getAttribute('contenteditable') === '') {
    target.focus();
    target.textContent = v;
    fire(target, 'input');
    fire(target, 'change');
    try { target.blur(); } catch (e) {}
    return {ok: true, val: (target.innerText || '').trim(), ce: true};
  }

  /* 普通 input/textarea：走原生 setter，绕开 React/Vue 的受控值拦截 */
  try {
    const proto = target.tagName === 'TEXTAREA'
      ? window.HTMLTextAreaElement.prototype
      : window.HTMLInputElement.prototype;
    const setter = Object.getOwnPropertyDescriptor(proto, 'value');
    if (setter && setter.set) {
      setter.set.call(target, '');
      fire(target, 'input');
      setter.set.call(target, v);
    } else {
      target.value = v;
    }
  } catch (e) {
    try { target.value = v; } catch (e2) { return {ok: false, why: 'set failed'}; }
  }
  target.focus();
  fire(target, 'input');
  fire(target, 'change');
  try { target.blur(); } catch (e) {}
  return {ok: true, val: target.value, ce: false};
})()
"""

JS_CLICK = r"""
return (() => {
  const arg = arguments[0] || {};
  const list = arg.kind === 'tab' ? (window.__bz_tab || []) : (window.__bz_btn || []);
  let el = null;
  if (arg.el) el = arg.el;
  else if (arg.i !== undefined) el = list[arg.i];
  if (!el) return {ok: false, why: 'no button at ' + arg.i};
  try { el.scrollIntoView({block: 'center'}); } catch (e) {}
  try { el.focus(); } catch (e) {}
  /* 只调一次 el.click()：它会同时触发 onclick 和 React/Vue 挂在根节点上的
     原生监听。再补 dispatchEvent 会让回调执行两次 —— p3 页面就是这么坏的
     （ok() 跑两遍，第二次时表单状态已经乱了）。 */
  let okc = false;
  try { el.click(); okc = true; } catch (e) {}
  return {ok: okc, tag: (el.tagName || '') + '', text: ((el.innerText || '') + '').trim().slice(0, 30)};
})()
"""


def out(**kw):
    kw.setdefault("ok", True)
    sys.stdout.write(json.dumps(kw, ensure_ascii=False))
    sys.exit(0)


def fail(msg, **kw):
    kw.update({"ok": False, "error": msg})
    sys.stdout.write(json.dumps(kw, ensure_ascii=False))
    sys.exit(0)


def mem_avail_mb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 99999


def ensure_dir(p):
    d = os.path.dirname(p)
    if d and not os.path.isdir(d):
        os.makedirs(d, exist_ok=True)


# 请求拦截器：把页面自己发出的 XHR/fetch 连同完整请求头记下来。
# 很多站点的 token 是 JS 里算的（X-Sign、X-Token、时间戳+md5 之类），逆向成本
# 极高 —— 但浏览器已经算好了，直接抄下来就能拿去 curl。
# 结果存 sessionStorage：页面一跳转 window 变量就没了，sessionStorage 还在。
JS_HOOK = r"""
(function(){
  if (window.__bz_hook) return;
  window.__bz_hook = 1;
  var KEY = '__bz_req';
  try { window.__bz_req = JSON.parse(sessionStorage.getItem(KEY) || '[]'); }
  catch (e) { window.__bz_req = []; }
  if (!Array.isArray(window.__bz_req)) window.__bz_req = [];

  var save = function(){
    try {
      while (window.__bz_req.length > 300) window.__bz_req.shift();
      sessionStorage.setItem(KEY, JSON.stringify(window.__bz_req));
    } catch (e) {}
  };
  var push = function(o){
    try { window.__bz_req.push(o); save(); } catch (e) {}
  };

  var XO = XMLHttpRequest.prototype.open,
      XH = XMLHttpRequest.prototype.setRequestHeader,
      XS = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function(m, u){
    this.__m = m; this.__u = u; this.__h = {};
    return XO.apply(this, arguments);
  };
  XMLHttpRequest.prototype.setRequestHeader = function(k, v){
    try { this.__h[k] = v; } catch (e) {}
    return XH.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function(b){
    try {
      push({
        kind: 'xhr',
        method: (this.__m || 'GET') + '',
        url: (this.__u || '') + '',
        headers: this.__h || {},
        body: (b === undefined || b === null) ? '' : String(b).slice(0, 500),
        page: location.href,
        t: Date.now()
      });
    } catch (e) {}
    return XS.apply(this, arguments);
  };

  var OF = window.fetch;
  if (OF) {
    window.fetch = function(input, init){
      try {
        var url = (typeof input === 'string') ? input : ((input && input.url) || '');
        var h = {};
        var hs = (init && init.headers) || (input && input.headers);
        if (hs) {
          if (typeof hs.forEach === 'function') { hs.forEach(function(v, k){ h[k] = v; }); }
          else { Object.keys(hs).forEach(function(k){ h[k] = hs[k]; }); }
        }
        var bd = (init && init.body);
        push({
          kind: 'fetch',
          method: (((init && init.method) || (input && input.method) || 'GET') + ''),
          url: url + '',
          headers: h,
          body: bd ? String(bd).slice(0, 500) : '',
          page: location.href,
          t: Date.now()
        });
      } catch (e) {}
      return OF.apply(this, arguments);
    };
  }
})();
"""


def install_hook(drv):
    """装拦截器。CDP 的 addScriptToEvaluateOnNewDocument 管后续导航，
    当前页面再 execute_script 补一次（CDP 只对之后的文档生效）。"""
    try:
        drv.execute_cdp_cmd("Page.addScriptToEvaluateOnNewDocument",
                            {"source": JS_HOOK})
    except Exception:
        pass
    try:
        drv.execute_script(JS_HOOK)
    except Exception:
        pass


def new_driver(width=1366, height=900):
    from selenium import webdriver
    from selenium.webdriver.chrome.options import Options
    from selenium.webdriver.chrome.service import Service

    o = Options()
    # 板上走 glibc chroot 里的 chrome：binary_location 要写 chroot 内的路径
    # （宿主上 /opt/chrome/chrome-headless-shell 不存在，不能拿 exists 判断）
    # 判断依据是 cdrv.sh 在不在。本机开发时用它启动系统 chrome。
    cdrv_early = os.environ.get("BZ_CDRV", CDRV)
    if os.path.exists(cdrv_early):
        o.binary_location = CHROME_BIN        # 给 chroot 里的 chromedriver 看
    else:
        o.add_argument("--headless=new")      # 本机：系统 chrome 要显式无头
    o.add_argument("--window-size=%d,%d" % (width, height))
    o.add_argument("--hide-scrollbars")
    # 这块板子上绝对不能传 --user-data-dir：chroot 里 chromedriver 找不到
    # DevToolsActivePort，60 秒后报 unable to discover open pages。
    # 会话因此无法跨进程复用，登录流程必须在一次调用里跑完。
    o.page_load_strategy = "eager"
    for a in ("--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
              "--no-first-run", "--disable-extensions", "--mute-audio",
              "--disable-background-networking", "--disable-sync",
              "--disable-blink-features=AutomationControlled",
              # 密码管理器会往 autocomplete="username" 的表单里灌值，
              # 关掉它免得和脚本填的内容打架
              "--disable-features=PasswordManagerOnboarding,"
              "AutofillServerCommunication,Translate,OptimizationHints",
              "--password-store=basic",
              "--user-agent=" + UA, "--lang=zh-CN"):
        o.add_argument(a)

    ensure_dir(WORKDIR + "/x")
    cdrv = os.environ.get("BZ_CDRV", CDRV)
    try:
        if os.path.exists(cdrv):
            svc = Service(executable_path=cdrv, log_output=WORKDIR + "/chromedriver.log")
        else:
            # 本机开发：用系统 chromedriver
            svc = Service(executable_path=os.environ.get("BZ_DRIVER",
                          "/tmp/chromedriver-linux64/chromedriver"))
    except TypeError:
        svc = Service(executable_path=cdrv)
    try:
        drv = webdriver.Chrome(service=svc, options=o)
    except Exception as e:
        tail = ""
        try:
            with open(WORKDIR + "/chromedriver.log", "r", errors="replace") as f:
                tail = " | ".join(x.strip() for x in f.readlines()[-12:] if x.strip())
        except OSError:
            pass
        fail("浏览器启动失败：%s%s" % (str(e).splitlines()[0][:200],
                                    (" || " + tail[:300]) if tail else ""))
    # 浏览器挂了以后 selenium 每次调用会在死连接上重试 ~30 秒
    try:
        drv.command_executor.set_timeout(15)
    except Exception:
        pass
    try:
        drv.execute_cdp_cmd("Page.addScriptToEvaluateOnNewDocument",
                            {"source": STEALTH_JS})
    except Exception:
        pass
    install_hook(drv)      # 请求拦截器：抄下 token 这类 JS 算出来的请求头
    return drv


def _probe_deep(drv):
    """主文档 + 所有 iframe 都探一遍。

    为什么需要：登录弹窗经常挂在 iframe 里（阿里系尤其多，闲鱼就是），
    document.querySelectorAll 只看主文档，弹窗里的手机号/密码框一个都看不到 ——
    现象就是"点了登录、截图里弹窗明明出来了、probe 却说 inputs 只有搜索框"。

    返回结构和 JS_PROBE 一样，另外给 iframe 里的元素带上 frame 字段。
    """
    out = drv.execute_script(JS_PROBE)
    ins0 = out.get("inputs") or []
    # 主文档就已经够用了（有密码框，或 ≥2 个输入框）→ 别去碰 iframe，
    # 每次 switch_to.frame 都要一次 CDP 往返，轮询下来能拖慢十几秒。
    if any((x.get("type") or "").lower() == "password" for x in ins0) or len(ins0) >= 2:
        return out
    inputs = list(ins0)
    buttons = list(out.get("buttons") or [])
    hit_frames = []
    try:
        from selenium.webdriver.common.by import By
        n = len(drv.find_elements(By.TAG_NAME, "iframe"))
        for i in range(n):
            try:
                frames = drv.find_elements(By.TAG_NAME, "iframe")
                if i >= len(frames):
                    break
                # 跨域存储 / 埋点 / 广告的 iframe 不用进去看，纯浪费往返
                try:
                    _src = (frames[i].get_attribute("src") or "").lower()
                except Exception:
                    _src = ""
                if any(k in _src for k in ("xdomain-storage", "analytics",
                                           "googleads", "doubleclick", "beacon")):
                    continue
                drv.switch_to.frame(frames[i])
                sub = drv.execute_script(JS_PROBE)
                if sub.get("inputs"):
                    for it in sub["inputs"]:
                        it["frame"] = i
                        it["i"] = len(inputs)
                        inputs.append(it)
                    for b in (sub.get("buttons") or []):
                        b["frame"] = i
                        b["i"] = len(buttons)
                        buttons.append(b)
                    hit_frames.append(i)
                drv.switch_to.default_content()
            except Exception:
                try:
                    drv.switch_to.default_content()
                except Exception:
                    pass
    except Exception:
        pass
    out["inputs"] = inputs
    out["buttons"] = buttons
    if hit_frames:
        out["frames_with_forms"] = hit_frames
    return out


def do_probe(req):
    url = req.get("url") or ""
    if not url:
        fail("缺少 url")
    drv = new_driver()
    try:
        try:
            drv.get(url)
        except Exception as e:
            fail("打开页面失败：%s" % str(e).splitlines()[0][:200])
        # 轮询等真正的登录框。注意不能拿"有没有输入框"当条件：闲鱼首页本身
        # 就有个搜索框，一探就退出了，登录框还没弹出来。
        # 所以判定标准是「有没有密码框」；等不到就点一下页面上的"登录"入口
        # （很多站点要点了才弹窗），然后再等。
        # "登录表单出现了"的判定。不能只等密码框：闲鱼这类站点默认是
        # 【短信登录】（手机号 + 验证码，压根没有密码框），等密码框必然超时。
        # 所以两条都算：有密码框，或者有 2 个以上输入框（手机号+验证码那种）。
        # 首页单个搜索框不会被误判。
        def _login_form_ready(_i):
            ins = _i.get("inputs") or []
            if any((x.get("type") or "").lower() == "password" for x in ins):
                return True
            return len(ins) >= 2

        # 三道时限，保证一定会返回，不会一直挂着：
        #   wait      理想等待（默认 6s），期间探到密码框立刻返回
        #   max_wait  硬上限（默认 10s），到点无条件退出（点"登录"跳转也卡不住）
        #   probe 本身每次都有 CDP 层的 command timeout 兜着
        t0 = time.time()
        wait_s = float(req.get("wait", 8.0))
        hard_s = max(wait_s, float(req.get("max_wait", 10.0)))
        info = {}
        clicked_open = False
        timed_out = False
        while True:
            info = _probe_deep(drv)          # 含 iframe：登录弹窗常在 iframe 里
            if _login_form_ready(info):
                break
            now = time.time()
            if now - t0 >= wait_s or now - t0 >= hard_s:
                timed_out = True
                break
            if not clicked_open and req.get("open_click", True) \
                    and (now - t0) < hard_s - 1.0:      # 留一点余量再点
                # 用文字精确匹配点「登录」入口。不能走 _try_click —— 它按
                # <button> 列表顺序找，闲鱼页面上第一个含"登录"的是
                # 「登录 订单」，点了只会跳订单页，登录框根本不弹。
                hit = drv.execute_script(FIND_CLICK_JS, "登录")
                if not (hit and hit.get("found")):
                    hit = drv.execute_script(FIND_CLICK_JS, "立即登录")
                if hit and hit.get("found"):
                    clicked_open = True
                    # 等 iframe 里的登录框加载，但绝不超过硬上限
                    _left = hard_s - (time.time() - t0) - 0.5
                    time.sleep(min(3.0, max(0.3, _left)))
                    continue
            time.sleep(0.8)

        # 到点了还没探到 —— 再做一次"临门重扫"。
        # 闲鱼的登录弹窗在 passport.goofish.com 的跨域 iframe 里，ARM headless
        # 上实测要 10~12 秒才加载完；硬上限一到就收工的话，用户截图里明明有
        # 输入框，结果却是"没探到"。多给 3.5 秒能覆盖绝大多数情况。
        if not _login_form_ready(info):
            try:
                time.sleep(3.0)
                info2 = _probe_deep(drv)
                n1 = len(info.get("inputs") or [])
                n2 = len(info2.get("inputs") or [])
                if _login_form_ready(info2) or n2 > n1:
                    info2["late_rescan"] = True
                    info = info2
            except Exception:
                pass

        info["waited_sec"] = round(time.time() - t0, 1)
        if timed_out and not _login_form_ready(info):
            _ifs = info.get("iframes") or []
            info["hint"] = ("等了 %.1f 秒（含 3 秒补扫）没探到输入框。"
                            "页面上有 %d 个 iframe%s。可以加大 wait 到 15，"
                            "或者改用 hunt 用 flow 明确指定怎么进登录页"
                            % (info["waited_sec"], len(_ifs),
                               ("（" + "、".join(
                                   (f.get("src") or "")[:40] for f in _ifs[:3]) + "）")
                               if _ifs else ""))
        # 存一张页面截图：光看 JSON 看不出页面长什么样，尤其要判断
        # "登录框是不是要点一下才弹出来"这种问题时，图比字段直观得多。
        try:
            _s = _shot(drv, "probe")
            if _s:
                info["shot"] = _s
        except Exception:
            pass
        info["ok"] = True
        info["mem_mb"] = mem_avail_mb()
        info.update(_save_artifacts("probe", req, info, info.get("inputs"), info.get("buttons")))
        sys.stdout.write(json.dumps(info, ensure_ascii=False))
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)


def do_login(req):
    url = req.get("url") or ""
    steps = req.get("steps") or []
    if not url:
        fail("缺少 url")
    if not steps:
        fail("缺少 steps（先用 probe 探测，再把填表/点击动作排进来）")

    drv = new_driver()
    try:
        try:
            drv.get(url)
        except Exception as e:
            fail("打开页面失败：%s" % str(e).splitlines()[0][:200])
        time.sleep(float(req.get("wait", 1.5)))

        # 每次填/点之前重新探测，页面 DOM 变了元素引用就失效
        drv.execute_script(JS_PROBE)

        done = []
        for st in steps:
            act = (st.get("act") or "").lower()
            if act == "wait":
                time.sleep(float(st.get("sec", 2)))
                done.append({"act": "wait", "sec": st.get("sec", 2)})
                continue
            if act == "goto":
                drv.get(st.get("url") or url)
                time.sleep(float(st.get("sec", 1.5)))
                drv.execute_script(JS_PROBE)
                done.append({"act": "goto", "url": st.get("url", "")})
                continue
            if act == "fill":
                ok, got = fill_verified(drv, int(st.get("i", 0)), st.get("value", ""))
                done.append({"act": "fill", "i": st.get("i"), "ok": bool(ok),
                             "got": (got or "")[:20]})
            elif act == "click":
                kind = "btn" if st.get("kind", "btn") == "btn" else "in"
                ok = drv.execute_script(JS_CLICK, {"i": int(st.get("i", 0)), "kind": kind})
                done.append({"act": "click", "i": st.get("i"), "ok": bool(ok)})
                time.sleep(float(st.get("sec", 1.5)))
                drv.execute_script(JS_PROBE)
            elif act == "enter":
                from selenium.webdriver.common.keys import Keys
                el = drv.execute_script("return window.__bz_in[arguments[0]] || null",
                                        int(st.get("i", 0)))
                if el:
                    el.send_keys(Keys.ENTER)
                time.sleep(float(st.get("sec", 1.5)))
                drv.execute_script(JS_PROBE)
                done.append({"act": "enter", "i": st.get("i")})
            else:
                done.append({"act": act, "error": "未知动作"})

        # 等页面稳定（跳转后 cookie 才写进去）
        time.sleep(float(req.get("settle", 2.5)))
        cookies = drv.get_cookies()
        info = drv.execute_script(JS_PROBE)

        res = {
            "ok": True,
            "url": drv.current_url,
            "title": info.get("title", ""),
            "cookies": cookies,
            "cookie_count": len(cookies),
            "captcha": info.get("captcha", False),
            "steps_done": done,
            "mem_mb": mem_avail_mb(),
        }
        # 还停在登录页通常说明登录没成功（有验证码/密码错/选择器不对）
        if req.get("url_should_change"):
            want = req["url_should_change"]
            if want in res["url"]:
                res["warning"] = "URL 仍包含 %s，可能没登录成功" % want
        res.update(_save_artifacts("login", req, res, res.get("inputs"), res.get("buttons")))
        sys.stdout.write(json.dumps(res, ensure_ascii=False))
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)


def _guess_fields(info):
    """从 probe 结果里猜账号框 / 密码框 / 登录按钮。
    3B 组不出 steps，这一步只能框架代劳。"""
    user_i = pwd_i = None
    USER_HINT = ("user", "account", "phone", "mobile", "email", "login",
                 "账号", "帐号", "手机", "邮箱", "用户名", "帐")
    for inp in info.get("inputs", []):
        blob = " ".join([inp.get("name", ""), inp.get("id", ""), inp.get("label", ""),
                         inp.get("placeholder", ""), inp.get("aria", ""),
                         inp.get("autocomplete", "")]).lower()
        if inp.get("type") == "password":
            if pwd_i is None:
                pwd_i = inp["i"]
        elif user_i is None and any(k in blob for k in USER_HINT):
            user_i = inp["i"]
    # 兜底：第一个不是密码的文本框当账号（editable = contenteditable 的富文本框）
    if user_i is None:
        for inp in info.get("inputs", []):
            if inp.get("type") in ("text", "email", "tel", "editable") and inp["i"] != pwd_i:
                user_i = inp["i"]
                break

    # ---- 提交按钮 ----
    # 真实站点上这里坑最多，全是实测踩出来的：
    #   京东有个 <a>登录页面，改进建议</a>，含"登录"但根本不是按钮
    #   "密码登录"/"短信登录" 是切换登录方式的 tab，点了只会换表单
    #   "微信登录"/"扫码登录" 是第三方入口
    # 所以先按"方式词/干扰词"排除，再按文字精确度打分。
    BTN_MODE = ("密码登录", "短信登录", "扫码登录", "账号登录", "帐号登录",
                "验证码登录", "微信登录", "qq登录", "手机登录", "邮箱登录")
    BTN_BAD = ("改进", "建议", "反馈", "忘记", "注册", "找回", "帮助",
               "协议", "隐私", "下载", "客服", "关于", "问题")
    BTN_EXACT = ("登录", "登陆", "立即登录", "马上登录", "确认登录",
                 "signin", "login", "submit", "提交", "确认")

    cands = []
    for b in info.get("buttons", []):
        t = (b.get("text") or "").strip()
        tl = t.lower().replace(" ", "").replace("\u3000", "")
        if not tl or len(t) > 12:
            continue
        if any(m in tl for m in BTN_MODE):
            continue                      # 切换 tab，不是提交
        if any(x in tl for x in BTN_BAD):
            continue                      # 干扰链接
        if tl in [e.replace(" ", "") for e in BTN_EXACT]:
            cands.append((100, b["i"]))
        elif any(k in tl for k in ("登录", "登陆", "login", "signin", "提交", "确认")):
            cands.append((50 - len(tl), b["i"]))   # 越短越像真按钮
    cands.sort(reverse=True)
    btn_i = cands[0][1] if cands else None
    if btn_i is None:
        for b in info.get("buttons", []):
            if not b.get("tab"):
                btn_i = b["i"]
                break
    return user_i, pwd_i, btn_i


def _find_next_btn(info):
    """两步登录的第一步按钮（"下一步"/"继续"/Next）。Google、企业 SSO
    都这么干：先输账号，点下一步，密码框才渲染出来。"""
    NEXT = ("下一步", "下步", "继续", "next", "continue", "proceed")
    for b in info.get("buttons", []):
        t = (b.get("text") or "").lower().replace(" ", "").replace("\u3000", "")
        for k in NEXT:
            if k in t:
                return b["i"]
    return None


def _guess_captcha_field(info, used):
    """找验证码输入框：先按 name/id/placeholder 里的关键词，找不到就用
    账号密码之外最后一个文本框（验证码框通常在密码框后面）"""
    KEY = ("captcha", "code", "verify", "vcode", "check", "验证", "校验", "图形")
    for inp in info.get("inputs", []):
        if inp["i"] in used:
            continue
        blob = " ".join([inp.get("name", ""), inp.get("id", ""),
                         inp.get("label", ""), inp.get("placeholder", ""),
                         inp.get("aria", "")]).lower()
        if any(k in blob for k in KEY):
            return inp["i"]
    for inp in reversed(info.get("inputs", [])):
        if inp["i"] not in used and inp.get("type") in ("text", "tel", "number"):
            return inp["i"]
    return None


def _probe_anywhere(drv, wait=0.0):
    """先主文档找表单，找不到就钻进 iframe（很多站点把登录框放 iframe 里）。
    wait > 0 时轮询等表单出现 —— 骨架屏 / 懒加载的站点打开后要等一会儿才有输入框。"""
    deadline = time.time() + wait
    while True:
        info = drv.execute_script(JS_PROBE)
        if info.get("inputs") or time.time() >= deadline:
            break
        time.sleep(0.4)
    if info.get("inputs"):
        return info, None
    for fr in info.get("iframes", []):
        try:
            from selenium.webdriver.common.by import By
            frames = drv.find_elements(By.TAG_NAME, "iframe")
            if fr["i"] >= len(frames):
                continue
            drv.switch_to.frame(frames[fr["i"]])
            sub = drv.execute_script(JS_PROBE)
            if sub.get("inputs"):
                sub["in_iframe"] = fr["i"]
                return sub, fr["i"]
            drv.switch_to.default_content()
        except Exception:
            try:
                drv.switch_to.default_content()
            except Exception:
                pass
    return info, None


TAB_HINT = ("账号登录", "帐号登录", "密码登录", "账号密码", "用户名登录",
            "accountlogin", "passwordlogin", "账号", "密码")
OPEN_HINT = ("登录", "登陆", "注册", "signin", "sign_in", "login", "account")


def _try_click(drv, info, hints, tried):
    """点第一个文字命中 hints 的元素。tried 记录点过的文字，避免死循环。"""
    for want_tab in (True, False):
        for b in info.get("buttons", []):
            if bool(b.get("tab")) != want_tab:
                continue
            t = (b.get("text") or "").replace(" ", "").lower()
            if not t or t in tried:
                continue
            if any(k in t for k in hints):
                drv.execute_script(JS_CLICK, {"i": b["i"], "kind": "btn"})
                tried.add(t)
                time.sleep(2.0)      # 表单可能是异步渲染的（懒加载组件）
                return b.get("text", "")
    return ""


def _open_form(drv, info, frame_i, max_tries=4):
    """反复点开登录入口，直到表单出现。
    真实站点常要两步：先点 header 的"登录"打开弹窗，再点"账号登录"切 tab。"""
    tried = set()
    hits = []
    for _ in range(max_tries):
        if info.get("inputs"):
            break
        got = _try_click(drv, info, TAB_HINT, tried)
        if not got:
            got = _try_click(drv, info, OPEN_HINT, tried)
        if not got:
            break
        hits.append(got)
        info, frame_i = _probe_anywhere(drv)
    return info, frame_i, hits


def _fill_via_keys(drv, i, value):
    """用真实键盘事件填值（ActionChains）。返回实际值，失败返回 None。

    为什么不用 JS 设 value：知乎这类 React 站点会用 onChange 校验并把
    "非可信事件"（dispatchEvent 产生的 isTrusted=false）带来的值同步清掉，
    表现就是填进去立刻变空。真实键盘事件 isTrusted=true，框架才会认。
    """
    from selenium.webdriver.common.action_chains import ActionChains
    try:
        el = drv.execute_script("return window.__bz_in[arguments[0]];", i)
        if el is None:
            return None
        # 先清空。不能用 el.clear()：在 glibc chroot 的 chromedriver 上会段错误。
        drv.execute_script(
            "var e = window.__bz_in[arguments[0]]; if (!e) return;"
            "if (e.isContentEditable) e.textContent = ''; else e.value = '';"
            "try { e.dispatchEvent(new Event('input', {bubbles: true})); } catch (err) {}", i)
        ActionChains(drv).send_keys_to_element(el, value).perform()
        time.sleep(0.12)
        got = el.get_attribute("value")
        if got is None:
            got = drv.execute_script(
                "var e = window.__bz_in[arguments[0]]; if (!e) return null;"
                "return e.isContentEditable ? (e.innerText || '').trim() : e.value;", i)
        return got
    except Exception:
        return None


def fill_verified(drv, i, value, tries=3):
    """填值并读回校验。顺序：真实键盘 → JS 设值。

    真实站点上的三类坑：
      1. React/Vue 受控组件不认 dispatchEvent 的非可信事件（知乎会直接清空）
      2. contenteditable 没有 .value，读回要走 innerText
      3. 浏览器自动填充/输入法可能把值改掉，所以必须读回确认
    """
    got = None
    for _ in range(tries):
        # 1) 真实键盘事件（最可靠）
        got = _fill_via_keys(drv, i, value)
        if got == value:
            return True, got

        # 2) 退回 JS 设值（contenteditable、或 send_keys 进来不了的情况）
        r = drv.execute_script(JS_FILL, {"i": i, "value": value})
        time.sleep(0.12)
        if isinstance(r, dict) and r.get("val") is not None:
            got = r["val"]
        else:
            got = drv.execute_script(
                "var e = window.__bz_in[arguments[0]]; if (!e) return null;"
                "if (e.isContentEditable || e.getAttribute('contenteditable') !== null)"
                "  return (e.innerText || '').trim();"
                "return e.value;", i)
        if got == value:
            return True, got
        time.sleep(0.1)
    return False, got


def cookies_to_header(cookies):
    """转成 curl 的 Cookie 头，可以直接喂给 curl 插件"""
    return "; ".join("%s=%s" % (c.get("name"), c.get("value"))
                     for c in cookies if c.get("name"))


def judge_login(before_url, after_url, cookies, info, storage=None):
    """判断这次登录到底成没成。

    真实站点上没有单一可靠信号，踩过的坑：
      - GitHub 未登录也发 _gh_sess，按 cookie 名判断会误判成功
      - 很多站登录失败也跳 URL（302 回登录页），光看 url_changed 没用
      - SPA 登录成功 URL 完全不变，光看 url_changed 也没用

    所以按可靠性从强到弱：
      1) 页面上还摆着密码输入框、而且 URL/标题还是登录页 → 铁定没进去
         （成功登录后登录表单必然消失，这条几乎不会错）
      2) 出现"退出/注销/我的"这类登录后才有的入口 → 基本成功
      3) 没有密码框了 + 拿到 auth 类 cookie/token → 可能成功
    """
    inputs = info.get("inputs") or []
    still_pwd = any((i.get("type") or "").lower() == "password" for i in inputs)

    # 只看"现在"在哪儿，不能把 before_url 拼进来 ——
    # 登录成功后 before_url 仍然含 login，一拼就永远判"还在登录页"
    urls = " ".join([(info.get("url") or ""), (after_url or "")]).lower()
    LOGIN_PATH = ("login", "signin", "sign-in", "passport", "sso", "sign_in", "auth/")
    on_login_path = any(k in urls for k in LOGIN_PATH)

    title = (info.get("title") or "").lower()
    # 标题含"登录"不等于还在登录页 —— "登录成功"、"欢迎登录" 都含这两个字。
    # 必须先把"成功/欢迎/首页"这类排除掉，否则登录成功会被判成没登进去。
    TITLE_LOGIN = ("登录", "登陆", "sign in", "signin", "log in", "login", "passport")
    TITLE_DONE = ("成功", "欢迎", "首页", "工作台", "主页", "dashboard", "home",
                  "welcome", "已登录", "signed in", "logged in")
    title_login = (any(k in title for k in TITLE_LOGIN)
                   and not any(k in title for k in TITLE_DONE))

    buttons = info.get("buttons") or []
    has_logout = any(
        k in (b.get("text") or "").lower()
        for b in buttons
        for k in ("退出", "注销", "登出", "退出登录", "logout", "sign out", "signout", "log out"))

    # 只认明确表示"已登录"的 cookie 名。
    # 注意别把 xx_sess / SESSION 算进来 —— 那是匿名会话，未登录就有。
    AUTH = ("user_session", "auth_token", "access_token", "refresh_token",
            "jwt", "ticket", "sessionid", "session_id", "remember_token",
            "logged_in", "is_login", "sid", "uid", "token")
    # 光看名字不够：GitHub 未登录也发 logged_in=no，名字对了值照样骗人。
    # 必须连值一起看，值是 no/false/0 这类的一律不算。
    BAD_VAL = ("", "no", "false", "0", "null", "none", "deleted", "guest",
               "anonymous", "-1", "undefined")
    cookie_auth = False
    for c in cookies:
        n = (c.get("name") or "").lower()
        v = ((c.get("value") or "") + "").strip().lower()
        if not any(k in n for k in AUTH):
            continue
        if v in BAD_VAL:
            continue
        cookie_auth = True
        break

    blob = ""
    if storage:
        blob = " ".join(list(storage.get("local", {}).keys())
                        + list(storage.get("session", {}).keys())).lower()
    token_key = any(k in blob for k in ("token", "auth", "jwt", "ticket", "user_id", "uid"))

    reasons = []
    if still_pwd:
        reasons.append("页面上还有密码输入框")
    if on_login_path:
        reasons.append("URL 还在登录相关路径")
    if title_login:
        reasons.append("页面标题还是登录页")

    # ---- 按可靠性判定 ----
    if still_pwd and (on_login_path or title_login):
        # 密码框还在、页面也还是登录页：不管有没有 cookie，肯定没进去
        likely, score = False, 0
    elif has_logout:
        likely, score = True, 4
    elif (not still_pwd) and (cookie_auth or token_key):
        likely, score = True, 3
    else:
        likely, score = False, (1 if (cookie_auth or token_key) else 0)

    return {
        "score": score,
        "likely_ok": likely,
        "still_has_password": still_pwd,
        "on_login_page": on_login_path,
        "left_login_page": not on_login_path,      # 老字段，保留兼容
        "title_is_login": title_login,
        "cookie_has_session": cookie_auth,         # 只算 auth 类，不算匿名会话
        "storage_has_token": token_key,
        "logged_in_ui": has_logout,
        "url_changed": before_url != after_url,
        "reasons": reasons,
    }


SHOT_DIR = "/tmp/llama_agent/shots"     # 统一放在 WORK_DIR 下，?file= 才找得到


def _shot(drv, tag):
    """截当前页面存文件，返回路径。失败场景靠它给人工看现场。"""
    if drv is None:
        return None
    try:
        os.makedirs(SHOT_DIR, exist_ok=True)
        p = os.path.join(SHOT_DIR, "%s_%s.png" % (tag, time.strftime("%H%M%S")))
        drv.save_screenshot(p)
        return p if os.path.getsize(p) > 0 else None
    except Exception:
        return None


PWD_ERR_PAT = (
    "密码错误", "密码不正确", "密码有误", "账号或密码", "帐号或密码",
    "用户名或密码", "账号密码", "帐号密码",
    "账号不存在", "帐号不存在", "用户不存在", "用户名不存在", "账号未注册",
    "登录失败", "登陆失败", "认证失败", "身份验证失败",
    "账号已锁定", "账号被锁定", "帐号已锁定", "账号冻结", "账号被冻结",
    "尝试次数过多", "错误次数过多", "请稍后再试", "账号异常",
    "incorrect password", "invalid password", "wrong password",
    "incorrect username or password", "incorrect username",
    "invalid username or password", "username or password",
    "login failed", "log in failed", "authentication failed",
    "invalid credentials", "bad credentials",
    "account does not exist", "user not found", "no such user",
    "account locked", "account is locked", "too many attempts", "too many failed",
)

CAPTCHA_ERR_PAT = (
    "验证码错误", "验证码不正确", "验证码有误", "校验码错误", "图形码错误",
    "验证码已过期", "验证码失效", "请重新输入验证码", "验证码输入错误",
    "captcha is invalid", "invalid captcha", "wrong captcha", "captcha error",
    "verification code is invalid", "invalid verification code",
)


def _page_text(drv, limit=4000):
    """整个页面的可见文字，用来找失败提示。"""
    try:
        return ((drv.execute_script(
            "return (document.body && document.body.innerText) || '';") or "")[:limit])
    except Exception:
        return ""


def _classify_failure(drv, res):
    """这次失败属于哪一类 —— 决定能不能重试。

    这是账号安全的关键：很多站点密码错 5 次就锁号/锁 IP。拿凭据反复试探
    是最危险的做法，宁可少试、失败就报，也不能把账号试废。

    返回：
      credentials  账号或密码错误 → 绝对不能重试
      captcha      验证码错误     → 可以重试（换一张验证码）
      captcha_unreadable 验证码认不出来（还没提交过） → 可以重试
      recaptcha    人机验证       → 重试无用
      npu          NPU/内存问题   → 重试无用
      structure    页面结构找不到 → 重试无用（下次还是一样）
      unknown      判不出来       → 保守：不重试
    """
    if res.get("npu_unavailable"):
        return "npu"
    if (res.get("captcha_kind") or "").lower() in ("recaptcha", "hcaptcha", "slider"):
        return "recaptcha"
    if res.get("captcha_failed") and not res.get("verdict"):
        # 连识别都没做成就返回了，说明是"页面上的验证码认不出来"，还没提交过凭据
        return "captcha_unreadable"

    err = ((res.get("error") or "") + " " + (res.get("warning") or ""))
    if "没找到账号框" in err or "没找到密码框" in err or "没找到验证码图片" in err:
        return "structure"

    # 优先用 _login_once 当场采下来的页面文字：错误提示常常几秒就消失，
    # 等这里再读就读不到了（恩山论坛的提示就是闪一下就没了）
    txt = ((res.get("page_text") or _page_text(drv)) + "").lower()
    res.pop("page_text", None)          # 只是内部判据用的，别带进输出

    # 先看账号/密码类 —— 优先级最高，命中就绝不再试
    for k in PWD_ERR_PAT:
        if k in txt:
            return "credentials"

    for k in CAPTCHA_ERR_PAT:
        if k in txt:
            return "captcha"

    # 判不出来：保守当不能重试
    return "unknown"


def _login_once(drv, url, user, pwd, req, attempt):
    """跑一次完整登录，返回 dict（不退出进程，失败也让调用方决定）。
    attempt 只用来把每轮的验证码截图分开放。"""
    try:
        drv.get(url)
    except Exception as e:
        return {"ok": False, "error": "打开页面失败：%s" % str(e).splitlines()[0][:180]}
    time.sleep(float(req.get("wait", 1.5)))
    before_url = drv.current_url

    info, frame_i = _probe_anywhere(drv, wait=float(req.get("form_wait", 4.0)))
    # 表单没出现 → 可能要先把登录入口点开、再切 tab（最多试 4 步）
    info, frame_i, tabbed = _open_form(drv, info, frame_i)

    # 人机验证（reCAPTCHA / hCaptcha / 滑块）：原理上没法自动过，直接说清楚。
    # 必须放在跑 OCR 之前 —— 不然先白起一遍识别再报"识别不出来"，误导人。
    kind = (info.get("captcha_kind") or "").lower()
    if kind in ("recaptcha", "hcaptcha", "slider"):
        return {
            "ok": False, "captcha_failed": True, "captcha_kind": kind,
            "error": ("这个站点要过 %s 人机验证，自动化过不去，得人工点一下"
                      % {"recaptcha": "reCAPTCHA", "hcaptcha": "hCaptcha",
                         "slider": "滑块"}.get(kind, kind)),
            "inputs": info.get("inputs", []),
        }

    cap_text = ""
    cap_info = None
    cap_low = False
    cap_png = ""
    if info.get("captcha") and req.get("ocr", True) and not req.get("force"):
        cap_info = _solve_captcha(
            drv, SHOT_DIR, int(req.get("cap_retry", 2)), "t%d_" % attempt)
        if cap_info and cap_info.get("text"):
            cap_text = cap_info["text"]
            cap_low = cap_info.get("low_confidence", False)
            # 识别用的那张图要带出去：验证码一提交就换新的，事后只剩这张能证明
            # "OCR 当时看到的是什么"，人工才好判断是认错了还是站点判错了
            pngs = cap_info.get("pngs") or []
            cap_png = pngs[-1] if pngs else ""
            # 点过刷新按钮，DOM 可能重建 → 重新探测
            info, frame_i = _probe_anywhere(drv)
        else:
            if cap_info and cap_info.get("npu_unavailable"):
                # 环境问题：NPU 分不到内存 / ppocr 没装。重试也没用，直接报清楚
                return {
                    "ok": False,
                    "captcha_failed": True,
                    "npu_unavailable": True,
                    "error": cap_info.get("error") or "NPU 不可用",
                    "pngs": cap_info.get("pngs", []),
                }
            return {
                "ok": False,
                "captcha_failed": True,
                "error": "验证码识别不出来",
                "pngs": (cap_info or {}).get("pngs", []),
                "attempts": (cap_info or {}).get("attempts", []),
            }

    user_i, pwd_i, btn_i = _guess_fields(info)

    # 默认是短信/扫码登录的站点（知乎、很多国内站就是这么设计的）：
    # 页面上有手机号框、但压根没有密码框。这种情况先点"密码登录"切过去，
    # 别直接判"没找到密码框"就放弃。
    if user_i is not None and pwd_i is None:
        switched = _try_click(drv, info,
                              ("密码登录", "账号密码", "帐号密码", "用密码登录",
                               "密码登陆", "password"), set())
        if switched:
            time.sleep(1.0)
            info2, frame_i2 = _probe_anywhere(drv, wait=2.0)
            u2, p2, b2 = _guess_fields(info2)
            if p2 is not None:
                info, user_i, pwd_i, btn_i = info2, u2, p2, b2
                if frame_i2 is not None:
                    frame_i = frame_i2

    # 两步登录：先填账号 → 点「下一步」→ 密码框这时才渲染出来。
    # 直接按"没找到密码框"失败会把 Google / 企业 SSO 这类站点全判死。
    if user_i is not None and pwd_i is None:
        fill_verified(drv, user_i, user)
        time.sleep(0.2)
        nxt = _find_next_btn(info)
        if nxt is not None:
            drv.execute_script(JS_CLICK, {"i": nxt, "kind": "btn"})
            for _ in range(15):                 # 最多等 6 秒
                time.sleep(0.4)
                info2, frame_i = _probe_anywhere(drv)
                u2, p2, b2 = _guess_fields(info2)
                if p2 is not None:
                    info, user_i, pwd_i, btn_i = info2, u2, p2, b2
                    break

    if user_i is None or pwd_i is None:
        return {"ok": False, "error": "没找到账号框或密码框",
                "inputs": info.get("inputs", []),
                "buttons": info.get("buttons", [])}

    ok_u, _ = fill_verified(drv, user_i, user)
    time.sleep(0.15)
    ok_p, _ = fill_verified(drv, pwd_i, pwd)
    time.sleep(0.15)

    cap_ok = None
    cap_i = None
    if cap_text:
        cap_i = _guess_captcha_field(info, {user_i, pwd_i})
        if cap_i is None:
            return {"ok": False, "error": "识别出验证码「%s」但找不到输入框" % cap_text,
                    "pngs": (cap_info or {}).get("pngs", []),
                    "inputs": info.get("inputs", [])}
        cap_ok, _ = fill_verified(drv, cap_i, cap_text)
        time.sleep(0.15)

    # 提交前截一张：这是唯一能证明"我到底填了什么、当时的验证码长什么样"的证据。
    # 提交后验证码必然换新（服务端把旧的消费掉了），再截就只剩新图 + 错误提示。
    pre_shot = _shot(drv, "before%d" % attempt) if cap_text else ""

    clicked = drv.execute_script(JS_CLICK, {"i": btn_i if btn_i is not None else 0,
                                            "kind": "btn"})
    time.sleep(float(req.get("settle", 2.5)))
    post_shot = _shot(drv, "after%d" % attempt)

    try:
        drv.switch_to.default_content()
    except Exception:
        pass
    # 探测最终页面状态。失败常常是一次 302 回到登录页、页面要重新渲染，
    # 抢在重定向中间 probe 会拿到空表单 —— 实测 GitHub 就这样，判据因此
    # 误以为"密码框消失了"，把登录失败判成了成功。所以拿不到输入框就再等再探。
    after_info = {}
    for _w in range(3):
        try:
            after_info = drv.execute_script(JS_PROBE)
        except Exception:
            after_info = {}
        if after_info.get("inputs"):
            break
        if _w < 2:
            time.sleep(0.9)

    cookies = drv.get_cookies()
    dom = req.get("domain") or ""
    if dom:
        cookies = [c for c in cookies if dom in (c.get("domain") or "")]
    try:
        storage = drv.execute_script(JS_STORAGE)
    except Exception:
        storage = {}

    # 趁错误提示还在，先把页面文字采下来。
    # 很多站点的"密码错误"提示几秒后自动消失（恩山就是这样），
    # 等外层判失败原因时再读就读不到了。
    page_text_snapshot = _page_text(drv, 3000)

    verdict = judge_login(before_url, drv.current_url, cookies, after_info, storage)

    res = {
        "ok": True,
        "url": drv.current_url,
        "title": after_info.get("title") or (drv.title if hasattr(drv, "title") else ""),
        "page_text": page_text_snapshot,
        "cookies": cookies,
        "cookie_count": len(cookies),
        "cookie_header": cookies_to_header(cookies),
        "storage": storage,
        "verdict": verdict,
        "filled": {"user_i": user_i, "pass_i": pwd_i, "btn_i": btn_i,
                   "clicked": bool(clicked), "in_iframe": frame_i,
                   "tab_clicked": " + ".join(tabbed),
                   "user_ok": ok_u, "pass_ok": ok_p,
                   "captcha_i": cap_i, "captcha_ok": cap_ok,
                   "captcha_text": cap_text, "captcha_png": cap_png},
        "shot": post_shot,
        "shot_before": pre_shot,
        "shot_after": post_shot,
        "mem_mb": mem_avail_mb(),
    }
    if after_info.get("buttons"):
        res["buttons"] = after_info["buttons"][:12]
        if after_info.get("inputs"):
            res["inputs"] = after_info["inputs"][:12]

    if not verdict["likely_ok"]:
        # 直接用判据算出来的原因，别再自己拼一套（两边会不一致）
        reasons = list(verdict.get("reasons") or [])
        if cap_text:
            reasons.append("验证码填的是「%s」%s" % (
                cap_text, "（置信度低，很可能认错了）" if cap_low else ""))
        if not reasons:
            reasons.append("页面既没有登录后的入口，也没拿到鉴权 cookie")
        res["warning"] = "这次没登进去（判据 %d/4）" % verdict["score"]
        res["diagnosis"] = {"reasons": reasons}
    return res


def do_autologin(req):
    """登录主流程：失败就让插件自己重新识别验证码再来一次。
    每轮都重新加载页面（验证码会跟着刷新），最多 tries 轮。"""
    url = req.get("url") or ""
    user = req.get("user") or req.get("username") or ""
    pwd = req.get("pass") or req.get("password") or ""
    if not url:
        fail("缺少 url")
    if not user or not pwd:
        fail("缺少账号或密码（user / pass 字段）")
    tries = max(1, int(req.get("tries", 3)))

    history = []
    last = None
    drv = new_driver()
    try:
        for attempt in range(tries):
            r = _login_once(drv, url, user, pwd, req, attempt)
            last = r
            v = r.get("verdict") or {}
            f = r.get("filled") or {}
            ok_now = bool(r.get("ok") and v.get("likely_ok"))

            # 判失败原因：决定还能不能再来一次
            kind = "" if ok_now else _classify_failure(drv, r)
            history.append({
                "attempt": attempt + 1,
                "captcha": f.get("captcha_text", ""),
                "score": v.get("score", 0),
                "logged_in": ok_now,
                "failure_kind": kind,
                "error": (r.get("error") or "")[:60],
            })
            if ok_now:
                r["tries_used"] = attempt + 1
                r["history"] = history
                r.pop("page_text", None)      # 内部判据用的，别带进输出
                r.update(_save_artifacts("autologin", req, r, r.get("inputs"), r.get("buttons")))
                sys.stdout.write(json.dumps(r, ensure_ascii=False))
                sys.stdout.flush()
                return

            # 没成功 → 先截现场图（页面停在这一步的样子）
            s = _shot(drv, "try%d" % (attempt + 1))
            if s:
                r["shot"] = s
                history[-1]["shot"] = s

            r["failure_kind"] = kind
            r["tries_used"] = attempt + 1
            r["history"] = history

            # ===== 关键：只有"验证码认错"允许重试 =====
            # 账号密码错绝对不能重试 —— 很多站点错 5 次就锁号、锁 IP，
            # 拿凭据反复试探是最危险的做法。其它类型重试也没意义。
            if kind not in ("captcha", "captcha_unreadable"):
                if kind == "credentials":
                    r["error"] = ("账号或密码错误 —— 已立即停止，不再重试。"
                                  "（站点通常错几次就锁号/锁 IP，不能拿凭据试探）")
                    r["stopped_for_safety"] = True
                elif kind == "recaptcha":
                    r["error"] = r.get("error") or "站点要求人机验证，重试无用"
                elif kind == "npu":
                    r["error"] = r.get("error") or "NPU/内存不足，重试无用"
                    r["hint"] = "跑 /root/freemm.sh 重启设备回收 NPU 内存后再试"
                elif kind == "structure":
                    r["error"] = r.get("error") or "页面结构对不上，重试无用"
                else:
                    r["error"] = (r.get("error") or "登录没通过") + \
                                 "（判断不出具体原因，为安全起见不再重试）"
                r["ok"] = False
                r.update(_save_artifacts("autologin", req, r, r.get("inputs"), r.get("buttons")))
                sys.stdout.write(json.dumps(r, ensure_ascii=False))
                sys.stdout.flush()
                return

            # 验证码类：换一张再试（不算拿凭据试探，因为还没提交成功过）
            if attempt + 1 < tries:
                time.sleep(float(req.get("retry_wait", 1.5)))

        # 全部轮次用完还是没进去
        out = dict(last or {"ok": False})
        out["ok"] = False
        out["tries_used"] = tries
        out["history"] = history
        if not out.get("error"):
            out["error"] = out.get("warning") or ("试了 %d 轮都没登进去" % tries)
        out.pop("warning", None)
        captcha_seen = any(h.get("captcha") for h in history)
        if captcha_seen:
            out["next_steps"] = [
                "每轮都重新识别了验证码，还是没过 —— 干扰线太强的验证码 ppocr 也认不了",
                "把 pngs 里的图捞出来人工读，再用 op=login 的 steps 填",
                "或者 ocr=0 关掉自动识别，全程自己用 steps 控制",
            ]
        out.update(_save_artifacts("autologin", req, out, out.get("inputs"), out.get("buttons")))
        sys.stdout.write(json.dumps(out, ensure_ascii=False))
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)



def _cma_free_kb():
    """CMA（连续内存）剩余，单位 KB。NPU 的 RKNN 模型要从这里分 DMA 内存。"""
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("CmaFree:"):
                    return int(line.split()[1])
    except Exception:
        pass
    return -1


NPU_NEED_KB = int(os.environ.get("BZ_NPU_NEED_KB", "8192"))
# ppocr 的 det 模型实测要 6912000 字节 ≈ 6.6MB，上面留了点余量。
# 允许用 BZ_NPU_NEED_KB 覆盖，方便在 CMA 充足的机器上演练"内存不足"的提示。


def _ppocr_usable():
    """返回 (能不能用, 说明)。不能用的话说明是给人看的，会一路传到页面上。
    这块板子的 CMA 是启动参数 cma=256M 分的，开机后基本被 GPU/RGA 吃掉，
    跑几次 chrome 就彻底不够了 —— 只有重启能回收。"""
    if not os.path.exists("/usr/bin/ppocr"):
        return False, "板上没装 ppocr（rk356x-npu-ocr 包），验证码识别不可用"
    free = _cma_free_kb()
    if free >= 0 and free < NPU_NEED_KB:
        return False, ("NPU 内存不足：CMA 连续内存只剩 %d KB，ppocr 至少要 %d KB。"
                       "这是设备内存被占满了（浏览器任务最容易吃光），"
                       "跑 /root/freemm.sh 重启设备就能回收" % (free, NPU_NEED_KB))
    return True, ""


def _ocr_ppocr(path):
    """板上 rk356x-npu-ocr 的 PP-OCRv4（走 NPU，0.7 秒一张）。
    返回 (识别文本, 错误说明)。"""
    try:
        r = subprocess.run(["ppocr", path], capture_output=True, text=True, timeout=25)
    except FileNotFoundError:
        return "", "板上没装 ppocr（rk356x-npu-ocr），验证码识别不可用"
    except Exception as e:
        return "", "ppocr 执行失败：%s" % str(e).splitlines()[0][:120]

    blob = (r.stderr or "") + (r.stdout or "")
    if ("rknn_init fail" in blob or "DMA Heap" in blob
            or "malloc npu memory" in blob or "init det model fail" in blob):
        free = _cma_free_kb()
        return "", ("NPU 初始化失败：DMA 内存分配不到（CMA 只剩 %s KB）。"
                    "跑 /root/freemm.sh 重启设备可回收"
                    % (free if free >= 0 else "?"))

    return re.sub(r"[^0-9A-Za-z]", "", r.stdout or ""), ""


CAP_SEL = ("img[src*=captcha], img[src*=verify], img[src*=code], "
           "img[id*=captcha], img[id*=cap], img[class*=captcha], "
           "img[alt*=验证码], img[alt*=captcha], canvas[class*=captcha], "
           "[class*=captcha] img, [id*=captcha] img, .verify-img img, "
           "[class*=verify] img, [class*=captcha] canvas")

OCR_WL = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"


def _ocr_png(data):
    """验证码识别：只用板上的 ppocr（PP-OCRv4，RKNN 走 NPU，0.7 秒一张）。

    ppocr 不在、或者 NPU 分不到内存，就返回明确的错误说明（由上层传到页面上），
    不做静默降级 —— 否则页面上只会看到"验证码识别不出来"，跟真的认错混在一起。
    """
    path = os.path.join(WORKDIR, "_cap.png")
    ensure_dir(path)
    with open(path, "wb") as f:
        f.write(data)
    return _ocr_ppocr(path)


def _find_captcha_img(drv):
    """找页面上的验证码图片元素。
    注意：这里的 except 不能吞掉 NameError 之类的编程错误，只能吞
    WebDriver 的定位失败 —— 否则改代码时少个常量会静默变成"找不到图片"。"""
    try:
        from selenium.webdriver.common.by import By
        els = drv.find_elements(By.CSS_SELECTOR, CAP_SEL)
    except Exception:
        return None
    for el in els:
        try:
            sz = el.size
            if sz.get("width", 0) >= 30 and sz.get("height", 0) >= 14:
                return el
        except Exception:
            continue
    return None


def _grab_captcha_bytes(drv, el):
    """拿验证码图片的原始字节。
    优先走 src 下载（截图会被页面缩放/加边框，OCR 就认不出来了：
    原图 120x40 识别正确，截图版本 m 变 n），canvas 没 src 才退回截图。"""
    try:
        src = el.get_attribute("src") or ""
        if src.startswith("data:image"):
            import base64
            return base64.b64decode(src.split(",", 1)[1])
        if src and not src.startswith("blob:"):
            from urllib.parse import urljoin
            import urllib.request
            full = urljoin(drv.current_url, src)
            ck = "; ".join("%s=%s" % (c.get("name"), c.get("value"))
                           for c in drv.get_cookies())
            req = urllib.request.Request(full, headers={
                "User-Agent": UA,
                "Referer": drv.current_url,
                "Cookie": ck,
            })
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.read()
    except Exception:
        pass
    try:
        return el.screenshot_as_png
    except Exception:
        return None


def _solve_captcha(drv, out_dir, retry=3, tag=""):
    """截图 → OCR → 长度不对就点图刷新再来。
    每次尝试的 PNG 都留着，tag 用来区分是第几轮的（多轮会互相覆盖）。"""
    os.makedirs(out_dir, exist_ok=True)
    attempts = []
    pngs = []
    el = _find_captcha_img(drv)
    if el is None:
        return None
    for i in range(max(1, retry)):
        data = _grab_captcha_bytes(drv, el)
        if not data:
            break
        p = os.path.join(out_dir, "%scaptcha_%02d.png" % (tag, i))
        with open(p, "wb") as f:
            f.write(data)
        pngs.append(p)
        text, oerr = _ocr_png(data)
        if oerr:
            # NPU 起不来 / ppocr 缺失：这是环境问题，不是"认错了"，
            # 直接往上抛，别浪费轮次重试
            return {"text": "", "pngs": pngs, "attempts": attempts,
                    "error": oerr, "npu_unavailable": True}
        attempts.append({"png": p, "text": text})
        if 4 <= len(text) <= 6:
            return {"text": text, "pngs": pngs, "attempts": attempts}
        try:
            el.click()          # 点一下通常换一张
            time.sleep(1.0)
        except Exception:
            break
    best = max(attempts, key=lambda a: len(a["text"])) if attempts else None
    return {"text": (best or {}).get("text", ""), "pngs": pngs,
            "attempts": attempts, "low_confidence": True}


def _curl_from_req(r, page_url=""):
    """把一个捕获的请求拼成可以直接粘到终端的 curl"""
    def q(s):
        return "'" + str(s).replace("'", "'\\''") + "'"

    url = (r.get("url") or "")
    if url.startswith("/") and page_url:
        from urllib.parse import urljoin
        url = urljoin(page_url, url)
    parts = ["curl"]
    m = (r.get("method") or "GET").upper()
    if m != "GET":
        parts.append("-X " + m)
    parts.append(q(url))
    SKIP = ("content-length", "host", "connection", "accept-encoding",
            "origin", "referer", "user-agent", "cookie")
    for k, v in (r.get("headers") or {}).items():
        if k.lower() in SKIP:
            continue
        parts.append("-H " + q("%s: %s" % (k, v)))
    b = r.get("body") or ""
    if b:
        parts.append("--data-raw " + q(b))
    return " ".join(parts)


def _rank_req(r):
    """给捕获的请求打分：越像"业务数据接口"分越高。静态资源直接淘汰。"""
    u = (r.get("url") or "").lower()
    if re.search(r"\.(js|css|png|jpe?g|gif|svg|woff2?|ico|map)(\?|$)", u):
        return -99
    s = 0
    for kw in ("api", "token", "list", "query", "data", "get", "info", "detail", "page"):
        if kw in u:
            s += 2
    if (r.get("method") or "GET").upper() == "POST":
        s += 3
    # 带自定义签名头的，基本就是我们要找的那种
    for k in (r.get("headers") or {}):
        kl = k.lower()
        if kl.startswith("x-") or "sign" in kl or "token" in kl or "auth" in kl:
            s += 5
    if r.get("body"):
        s += 1
    return s


FIND_CLICK_JS = r"""
return (() => {
  const want = arguments[0];
  const norm = s => (s || '').replace(/[\s\u3000]+/g, '').toLowerCase();
  const w = norm(want);
  const roots = [];
  const collect = (root, d) => {
    if (!root || d > 6 || roots.indexOf(root) >= 0) return;
    roots.push(root);
    try { root.querySelectorAll('*').forEach(el => { if (el.shadowRoot) collect(el.shadowRoot, d + 1); }); } catch (e) {}
  };
  collect(document, 0);
  const cands = [];
  roots.forEach(root => {
    let els = [];
    try { els = root.querySelectorAll('a, div, span, li, button, td, p, i, em'); } catch (e) {}
    els.forEach(el => {
      let t = '';
      try { t = norm(el.innerText); } catch (e) { return; }
      if (!t || t.length > 60) return;
      /* 需要是"看得见"的 */
      try {
        const rc = el.getBoundingClientRect();
        if (rc.width < 2 || rc.height < 2) return;
      } catch (e) { return; }
      if (t === w) cands.push({el: el, score: 100});
      else if (t.indexOf(w) >= 0) cands.push({el: el, score: 60 - Math.min(40, t.length - w.length)});
    });
  });
  if (!cands.length) return {found: false};
  cands.sort((a, b) => b.score - a.score);
  const best = cands[0].el;
  try { best.scrollIntoView({block: 'center'}); } catch (e) {}
  try { best.click(); } catch (e) { return {found: false, why: 'click failed'}; }
  return {found: true, tag: (best.tagName || '') + '',
          text: ((best.innerText || '') + '').trim().replace(/\s+/g, ' ').slice(0, 40)};
})()
"""


def _find_input_by_hint(drv, hint):
    """按 placeholder / name / id / aria-label 找输入框，返回 __bz_in 下标"""
    js = r"""
    return (() => {
      const nh = (arguments[0] || '').replace(/\s+/g, '').toLowerCase();
      if (!nh) return -1;
      const ins = window.__bz_in || [];
      let best = -1, bestScore = 0;
      ins.forEach((el, i) => {
        let blob = '';
        try {
          blob = [el.placeholder, el.name, el.id,
                  el.getAttribute('aria-label'), el.getAttribute('data-ph')].join(' ');
        } catch (e) {}
        const nb = (blob || '').replace(/\s+/g, '').toLowerCase();
        let sc = 0;
        if (nb.indexOf(nh) >= 0) sc = 50;
        if (el.placeholder && el.placeholder.replace(/\s+/g, '').toLowerCase() === nh) sc = 100;
        if (sc > bestScore) { bestScore = sc; best = i; }
      });
      return best;
    })()
    """
    try:
        return int(drv.execute_script(js, hint or ""))
    except Exception:
        return -1


def _run_flow_step(drv, step, req, idx):
    """执行流程里的一步。返回 {step, act, page, note, ok, ...}

    每个 act 都尽量给出"为什么失败"，因为真实站点上失败的方式千奇百怪：
    文字不对、元素被遮挡、要等异步渲染、输入框换成了 contenteditable……
    """
    act = ((step.get("act") or "click") + "").lower()
    label = step.get("text") or step.get("field") or ""
    r = {
        "step": idx,
        "act": act,
        "page": (step.get("page") or "")[:40],      # 这一步在哪个页面做
        "note": (step.get("note") or "")[:80],      # 人话说明，回显出去
        "target": label[:40],
        "ok": False,
    }
    try:
        # 每次动作前先刷新元素索引。上一步可能已经把页面换掉了，
        # window.__bz_in / __bz_btn 还是旧页面的，直接拿来 fill/click 会打空。
        if act not in ("goto",):
            try:
                drv.execute_script(JS_PROBE)
            except Exception:
                pass

        if act == "goto":
            u = step.get("url") or req.get("url") or ""
            drv.get(u)
            time.sleep(float(step.get("sec", 2.0)))
            r["ok"] = True

        elif act == "login":
            cur = drv.current_url
            lr = _login_once(drv, cur,
                             step.get("user") or req.get("user") or "",
                             step.get("pass") or req.get("pass") or "",
                             req, 0)
            v = lr.get("verdict") or {}
            r["ok"] = bool(v.get("likely_ok"))
            r["cookie_count"] = lr.get("cookie_count", 0)
            r["captcha_text"] = (lr.get("filled") or {}).get("captcha_text", "")
            if not r["ok"]:
                r["error"] = (lr.get("error") or lr.get("warning")
                              or "登录没通过（判据 %d/4）" % v.get("score", 0))

        elif act == "click":
            got = drv.execute_script(FIND_CLICK_JS, label)
            r["ok"] = bool(got and got.get("found"))
            r["matched"] = (got or {}).get("text", "")
            r["tag"] = (got or {}).get("tag", "")
            if not r["ok"]:
                r["error"] = ("页面上找不到「%s」—— 文字要跟显示的一模一样"
                              "（全角空格、繁体、图标字符都会影响）" % label)

        elif act == "fill":
            hint = step.get("field") or step.get("hint") or ""
            i = _find_input_by_hint(drv, hint)
            if i < 0:
                r["error"] = "找不到输入框「%s」" % hint
            else:
                ok, got = fill_verified(drv, i, step.get("value") or "")
                r["ok"] = ok
                r["input_i"] = i
                r["value"] = got

        elif act == "wait":
            time.sleep(float(step.get("sec", 2.0)))
            r["ok"] = True

        elif act == "wait_for":
            pat = step.get("text") or ""
            tries = int(step.get("tries", 20))
            ok = False
            for _ in range(max(1, tries)):
                try:
                    hit = drv.execute_script(
                        "return (document.body && document.body.innerText || '')"
                        ".indexOf(arguments[0]) >= 0;", pat)
                    if hit:
                        ok = True
                        break
                except Exception:
                    pass
                time.sleep(0.4)
            r["ok"] = ok
            if not ok:
                r["error"] = "等了 %.1f 秒也没出现「%s」" % (0.4 * tries, pat)

        elif act == "press":
            key = step.get("key") or "Enter"
            # 注意两点：
            #   1. fill 之后元素已经 blur，activeElement 是 body，键会发丢；
            #      所以支持用 field 指定往哪个输入框发（不给才退回 activeElement）
            #   2. forEach 回调里的 arguments 是 forEach 自己的，必须先存变量
            focus_i = -1
            hint = step.get("field") or step.get("hint") or ""
            if hint:
                focus_i = _find_input_by_hint(drv, hint)
            drv.execute_script(
                "var k = arguments[0], idx = arguments[1];"
                "var e = (idx >= 0 && window.__bz_in && window.__bz_in[idx])"
                "  ? window.__bz_in[idx] : (document.activeElement || document.body);"
                "try { e.focus(); } catch (err) {}"
                "['keydown','keypress','keyup'].forEach(function(t){"
                "  e.dispatchEvent(new KeyboardEvent(t, {key: k,"
                "    bubbles: true, cancelable: true}));});"
                "return true;", key, focus_i)
            r["ok"] = True
            r["key"] = key
            r["target"] = hint or "(当前焦点)"

        elif act == "capture":
            # 只是个标记：从这里开始（含）之后的请求才算目标页的
            r["ok"] = True
            r["capture"] = True

        else:
            r["error"] = "不认识的 act：%s（支持 goto/login/click/fill/wait/wait_for/press/capture）" % act

    except Exception as e:
        r["error"] = str(e).splitlines()[0][:180]
    return r


def do_hunt(req):
    """按"流程"走完一个真实网站，把目标页发出的请求头抄回来。

    为什么需要 flow：真实站点不是一个"点一串文字"能描述的。典型过程是
    第 1 页输账号密码 → 第 2 页点某个模块 → 第 3 页点子模块 → 第 4 页是目标页，
    每页做的事都不一样，中间还可能要先等异步渲染、填搜索框、按回车。
    flow 就是把这件事写成一步一步的脚本。

    flow 每步的字段：
      act    必填，动作类型
      page   可选，"这一步在哪个页面"，只用于回显（方便人看懂走到哪了）
      note   可选，人话说明，同样只用于回显
      text    click / wait_for 的目标文字
      field   fill 的输入框提示（placeholder/name/id 的片段）
      value   fill 要填的值
      sec     wait / goto 等多少秒
      key     press 的键名，默认 Enter

    act 支持：
      goto      直接跳 URL
      login     用账号密码登录当前页（会自动识别验证码）
      click     点文字匹配的元素（含 div 假按钮、Shadow DOM、iframe）
      fill      填输入框（含 contenteditable）
      wait      死等 N 秒
      wait_for  等某段文字出现（最多 tries 次，每次 0.4 秒）
      press     给当前焦点元素发按键
      capture   标记"目标页在这里"，这之后的请求才算数

    没给 flow 时退回老写法：path 是一串要点的文字，前面自动补一步 login。
    """
    url = req.get("url") or ""
    user = req.get("user") or ""
    pwd = req.get("pass") or ""

    # ---- 组装 flow ----
    flow = req.get("flow")
    if isinstance(flow, dict):
        flow = [flow]
    if not flow:
        path = req.get("path") or []
        if isinstance(path, str):
            path = [path]
        flow = [{"act": "click", "text": t} for t in path]
        if user and pwd:
            flow = [{"act": "login", "user": user, "pass": pwd,
                     "page": "登录页", "note": "输账号密码"}] + flow
    if not flow:
        fail("缺少 flow（流程步骤）或 path（要点的菜单文字）")
    if not url:
        # 允许 flow 第一步就是 goto 带 url
        first = flow[0] if isinstance(flow[0], dict) else {}
        url = first.get("url") or ""
    if not url:
        fail("缺少 url（起始页地址）")

    drv = new_driver()
    try:
        # ---- 打开起始页 ----
        try:
            drv.get(url)
        except Exception as e:
            fail("打开起始页失败：%s" % str(e).splitlines()[0][:160])
        time.sleep(float(req.get("wait", 2.0)))

        # ---- 逐步执行 ----
        steps = []
        all_ok = True
        capture_ts = 0
        for i, st in enumerate(flow, 1):
            if not isinstance(st, dict):
                continue
            time.sleep(float(req.get("step_wait", 1.0)))
            try:
                t_before = int(drv.execute_script("return Date.now();") or 0)
            except Exception:
                t_before = 0
            r = _run_flow_step(drv, st, req, i)
            r["t"] = t_before
            try:
                r["url"] = drv.current_url
            except Exception:
                r["url"] = ""
            steps.append(r)
            if not r.get("ok"):
                all_ok = False
                break
            if r.get("capture"):
                # 切分点要用"进入目标页"的那一步，不能用 capture 这一步自己的时刻。
                # 因为页面一跳进目标页就立刻发请求了，等轮到 capture 步执行时，
                # 请求早就发出去了，拿它的时刻去切会把目标请求本身切掉。
                capture_ts = (steps[-2].get("t") if len(steps) >= 2 else 0) or t_before
            time.sleep(float(req.get("settle", 1.2)))

        # 没显式标 capture → 拿最后一步的时刻当切分点
        if not capture_ts and steps:
            capture_ts = steps[-1].get("t") or 0

        # ---- 等目标页把数据拉完 ----
        time.sleep(float(req.get("wait", 3.0)))
        try:
            raw = drv.execute_script("return window.__bz_req || [];")
        except Exception:
            raw = []

        # ---- 只留目标页之后的请求 ----
        all_reqs = [it for it in (raw or []) if isinstance(it, dict)]
        cut = max(0, (capture_ts or 0) - 300)      # 宽容 300ms
        final_only = [it for it in all_reqs if (it.get("t") or 0) >= cut] if cut else all_reqs
        dropped = len(all_reqs) - len(final_only)

        pat = (req.get("match") or "").strip()

        def _pick(src):
            out = []
            for it in src:
                u = it.get("url") or ""
                if pat and pat not in u:
                    continue
                sc = _rank_req(it)
                if sc < 0:
                    continue
                it["_score"] = sc
                out.append(it)
            return out

        reqs = _pick(final_only)
        fell_back = False
        if cut and not reqs:
            reqs = _pick(all_reqs)
            fell_back = True
            dropped = 0
        reqs.sort(key=lambda x: -(x.get("_score") or 0))
        for it in reqs:
            it.pop("_score", None)

        best = reqs[0] if reqs else None
        page_url = drv.current_url
        obj = {
            "ok": bool(all_ok and reqs),
            "stage": "done" if (all_ok and reqs) else ("flow" if not all_ok else "capture"),
            "final_url": page_url,
            "title": (drv.title or "")[:60],
            "flow_total": len([s for s in flow if isinstance(s, dict)]),
            "flow_done": sum(1 for s in steps if s.get("ok")),
            "steps": steps,
            "final_page_only": bool(cut) and not fell_back,
            "noise_dropped": dropped,
            "request_count": len(reqs),
            "requests": reqs[:12],
            "best": best,
            "curl": _curl_from_req(best, page_url) if best else "",
        }
        if fell_back:
            obj["warning"] = ("按时间切分没切出请求（拦截器时间戳可能没生效），"
                              "下面是全部请求，可能混了中间页的噪音")
        try:
            obj["cookie_header"] = cookies_to_header(drv.get_cookies())
        except Exception:
            pass
        if not all_ok and steps:
            bad = steps[-1]
            obj["error"] = ("第 %d 步没成（%s%s）：%s" % (
                bad.get("step", 0),
                bad.get("page") and (bad["page"] + " / ") or "",
                bad.get("note") or bad.get("act"),
                bad.get("error") or "未知原因"))
        elif not reqs:
            obj["error"] = ("流程走完了但没捕获到请求：页面可能还在加载，"
                            "加大 wait；或者用 match 过滤关键字")
        obj.update(_save_artifacts("hunt", req, obj, obj.get("inputs"), obj.get("buttons")))
        sys.stdout.write(json.dumps(obj, ensure_ascii=False))
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)


THUMB_SEL = (".geetest_slider_button, .nc_iconfont.btn_slide, [class*=slide-btn], "
             "[class*=slider-btn], [class*=sliderbtn], [class*=slider] [class*=btn], "
             "[id*=sliderBtn], [id*=slideBtn], [class*=thumb], [id*=thumb], "
             "[class*=handler], [class*=drag-btn], [class*=verify-btn]")

BG_SEL = ("canvas, img[class*=bg], img[class*=background], img[id*=bg], "
          "[class*=geetest_bg] img, [class*=captcha-bg] img, [class*=canvas] img")


def _find_gap_x(img_bytes, min_w=12):
    """找缺口左边缘的 x（按图片原始像素算）。

    不能用"暗区/亮区"这种全局亮度阈值：背景本身可能整体偏暗、也可能花花绿绿，
    任何全局阈值都会被背景带偏（实测把暗色渐变背景判成整片都是洞）。

    缺口的稳定特征是「左右各有一条完整的竖直边」，所以这里算
    "每列与右移 N 列的逐像素差"，找差异最大的几处：
      - 最强的第一个峰 → 缺口左边界
      - 第二个峰       → 缺口右边界（顺便可验证缺口宽度像不像拼图块）
    这样跟背景深浅、颜色都没关系。
    """
    import io
    from PIL import Image
    try:
        im = Image.open(io.BytesIO(img_bytes)).convert("L")
    except Exception:
        return None, 0
    w, h = im.size
    if w < 40 or h < 20:
        return None, w
    px = im.load()

    N = 4
    diff = []
    for x in range(w - N):
        s = 0
        for y in range(h):
            s += abs(px[x, y] - px[x + N, y])
        diff.append(s / float(h))
    if not diff:
        return None, w
    mx = max(diff)
    if mx < 10:                      # 整张图没有明显竖直边界
        return None, w

    hot = [x for x in range(len(diff)) if diff[x] >= mx * 0.55]
    if not hot:
        return None, w
    groups, cur = [], [hot[0]]
    for x in hot[1:]:
        if x - cur[-1] <= 4:
            cur.append(x)
        else:
            groups.append(cur)
            cur = [x]
    groups.append(cur)
    groups = [g for g in groups if len(g) >= 2]
    if not groups:
        return None, w

    # 跳过贴左图边的噪声（x < 12 通常是图片自身边缘）
    cands = [g[0] for g in groups if g[0] > 12]
    if not cands:
        cands = [g[0] for g in groups]
    left = cands[0]
    # 峰位是"差异开始变大"的那一列，真实边界在它右边约 N/2 处
    left += N // 2
    # 太靠右说明可能认反了（拼图块一般从左边出发）
    if left > w * 0.85:
        return None, w
    return left, w


def _human_steps(distance, n=26):
    """生成一段像人手的位移序列：先加速、中间匀速、最后减速，带轻微抖动。"""
    import random
    pts = []
    cur = 0.0
    for i in range(n):
        t = (i + 1) / float(n)
        target = distance * (1.0 - (1.0 - t) ** 2.4)      # ease-out
        step = target - cur
        if 0 < i < n - 1:
            step += random.uniform(-0.8, 0.8)             # 手抖
        cur += step
        pts.append(step)
    # 最后一步精确补到目标
    pts[-1] += distance - sum(pts)
    return pts


def _human_path(distance, n=42):
    """更像真人的轨迹：返回 [(dx, dy, dt_ms), ...]，总和精确等于 distance。

    为什么 ActionChains 过不了阿里 nc：它每次 perform() 都走一遍完整 action
    chain，事件时间戳的间隔几乎恒定（10ms 上下），加速度曲线是死板的 ease-out。
    nc 的风控就是照这些指标判机器的。这里自己造轨迹：
      1. 起手先抖 1-3px（手指按下去的微动）
      2. 三次 ease-out（比二次的减速尾更平，接近真人）
      3. 抖动幅度与剩余距离正相关 —— 快段不抖，慢段才抖
      4. 时间间隔跟着速度走：快段 8-16ms，慢段 20-45ms，再加随机
      5. 末端过冲 2-6px 再拉回（真人几乎都会过头）
    """
    import random
    pts = []
    # 起手微动
    for _ in range(random.randint(1, 2)):
        pts.append((random.uniform(-1.5, 1.5), random.uniform(-1.0, 1.0),
                    random.randint(18, 45)))
    prev = 0.0
    for i in range(n):
        u = (i + 1) / float(n)
        frac = 1.0 - (1.0 - u) ** 3.0
        target = distance * frac
        step = target - prev
        jit = 0.0
        if 0 < i < n - 1:
            amp = 1.5 * (1.0 - frac) + 0.15      # 越慢抖得越明显
            jit = random.uniform(-amp, amp)
        prev = target
        dt = int(8 + 30 * (u ** 1.5) + random.randint(-3, 6))
        pts.append((step + jit, random.uniform(-0.6, 0.6), max(6, dt)))
    # 末端过冲
    over = random.uniform(2.0, 6.0)
    for _ in range(random.randint(2, 3)):
        pts.append((over / 3.0, random.uniform(-0.4, 0.4), random.randint(25, 50)))
    # 回拉
    back = sum(p[0] for p in pts) - distance
    for _ in range(random.randint(2, 3)):
        pts.append((-back / 2.5, random.uniform(-0.3, 0.3), random.randint(35, 70)))
    # 精修到恰好 distance
    err = distance - sum(p[0] for p in pts)
    pts.append((err, 0.0, random.randint(40, 90)))
    return pts


def _drag_cdp(drv, thumb, distance):
    """用 CDP Input.dispatchMouseEvent 拖滑块，自己控制每个点的时间戳。

    比 ActionChains 强的地方：时间戳是真实毫秒，不受 python 循环和 webdriver
    往返影响；轨迹可以任意造。缺点是每个事件一次 CDP 往返，40+ 个点会慢
    一两秒 —— 但拖拽本来就要慢才像人。
    返回 (ok, msg, moved)
    """
    import time as _t
    try:
        rect = drv.execute_script(
            "var r = arguments[0].getBoundingClientRect();"
            "return {x: r.left + r.width / 2, y: r.top + r.height / 2,"
            "        w: r.width, h: r.height};", thumb)
    except Exception as e:
        return False, "读滑块坐标失败：%s" % str(e).splitlines()[0][:80], 0
    x0 = float(rect.get("x") or 0)
    y0 = float(rect.get("y") or 0)
    if not x0 and not y0:
        return False, "滑块坐标是 (0,0)，可能不可见", 0

    path = _human_path(distance)
    # timestamp 单位是【秒】(TimeSinceEpoch)。传毫秒会让 chrome 段错误。
    t0 = _t.time()
    try:
        drv.execute_cdp_cmd("Input.dispatchMouseEvent", {
            "type": "mousePressed", "x": x0, "y": y0,
            "button": "left", "buttons": 1, "clickCount": 1, "timestamp": t0})
    except Exception as e:
        return False, "按下失败：%s" % str(e).splitlines()[0][:80], 0

    elapsed = 0
    cx, cy = x0, y0
    for dx, dy, dt in path:
        elapsed += dt
        cx += dx
        cy += dy
        try:
            drv.execute_cdp_cmd("Input.dispatchMouseEvent", {
                "type": "mouseMoved", "x": cx, "y": cy,
                "button": "left", "buttons": 1, "timestamp": t0 + elapsed / 1000.0})
        except Exception:
            break

    # 松手前真停一下（真人到位会稳一下再放）
    _t.sleep(random_uniform(0.08, 0.2))
    try:
        drv.execute_cdp_cmd("Input.dispatchMouseEvent", {
            "type": "mouseReleased", "x": cx, "y": cy,
            "button": "left", "buttons": 0, "clickCount": 1,
            "timestamp": t0 + (elapsed + 130) / 1000.0})
    except Exception as e:
        return False, "松手失败：%s" % str(e).splitlines()[0][:80], 0
    return True, "", cx - x0


def random_uniform(a, b):
    import random
    return random.uniform(a, b)


def _drag_thumb(drv, thumb, distance, n=26):
    """按住滑块拖 distance 像素。用 ActionChains 走真实鼠标事件。

    有个坑：浏览器会把鼠标坐标取整，多次 move_by_offset 累积下来实际位移
    会比参数少 10px 左右（实测差 11~12px）。所以松手前要读一次滑块的真实
    位置，把差值补上，否则永远差那么一点点、验证不通过。
    """
    import random
    from selenium.webdriver.common.action_chains import ActionChains
    try:
        x0 = thumb.location.get("x", 0)
    except Exception:
        x0 = None
    try:
        ActionChains(drv).move_to_element(thumb).click_and_hold().perform()
    except Exception as e:
        return False, "按住滑块失败：%s" % str(e).splitlines()[0][:90]
    time.sleep(random.uniform(0.08, 0.2))
    moved = 0
    for step in _human_steps(distance, n):
        try:
            ActionChains(drv).move_by_offset(step, 0).perform()
        except Exception:
            break
        moved += step
        time.sleep(random.uniform(0.008, 0.03))

    # ---- 松手前校正累积误差 ----
    if x0 is not None:
        try:
            x1 = thumb.location.get("x", 0)
            actual = x1 - x0
            # 只有滑块确实跟着鼠标走了（至少 60%）才微调。
            # actual≈0 有两种可能：① 这个元素压根不跟鼠标动（选错了）；
            # ② location 读的是另一个参照系。这时候再补 miss 会把距离翻倍 ——
            # 实测淘宝 distance=320 结果拖了 640，滑块直接冲过头验证不过。
            if actual >= distance * 0.6:
                miss = distance - actual
                if abs(miss) >= 2:
                    ActionChains(drv).move_by_offset(miss, 0).perform()
                    moved += miss
                    time.sleep(0.05)
        except Exception:
            pass

    # 松手前停顿一下（真人也会犹豫）
    time.sleep(random.uniform(0.05, 0.15))
    try:
        ActionChains(drv).release().perform()
    except Exception as e:
        return False, "松手失败：%s" % str(e).splitlines()[0][:90]
    return True, moved


def do_drag(req):
    """处理滑块 / 拼图拖拽验证码。

    两种用法：
      distance 给了 → 直接拖这么多 CSS 像素（最稳，适合已知尺寸的站点）
      auto=true     → 从背景图里识别缺口位置，自己算距离

    为什么不用匀速直线：那是最容易被判定成机器人的特征。这里用
    ease-out 轨迹 + 随机抖动 + 松手前停顿，尽量贴近人手。
    """
    url = req.get("url") or ""
    if not url:
        fail("缺少 url")

    auto = bool(req.get("auto", True))
    manual_dist = req.get("distance")
    n_steps = int(req.get("steps", 26))
    tries = max(1, int(req.get("tries", 3)))

    drv = new_driver()
    history = []
    try:
        drv.get(url)
        time.sleep(float(req.get("wait", 2.5)))

        # 有的站点要先点一下"验证"按钮才弹滑块
        open_sel = req.get("open_click") or ""
        if open_sel:
            try:
                r = drv.execute_script(FIND_CLICK_JS, open_sel)
                time.sleep(1.2)
                history.append({"step": "open", "click": open_sel, "result": r})
            except Exception:
                pass

        # ---- 找滑块 ----
        thumb_sel = req.get("thumb") or THUMB_SEL
        try:
            thumbs = drv.find_elements("css selector", thumb_sel)
        except Exception:
            thumbs = []
        thumb = None
        for t in thumbs:
            try:
                sz = t.size
                if sz.get("width", 0) >= 18 and sz.get("height", 0) >= 12 and t.is_displayed():
                    thumb = t
                    break
            except Exception:
                continue
        if thumb is None:
            # 兜底：找 cursor:grab/pointer 的、尺寸像滑块的元素
            thumb = drv.execute_script(
                "var out = null;"
                "document.querySelectorAll('div,span,a,button').forEach(function(e){"
                "  if (out) return;"
                "  var s; try { s = getComputedStyle(e); } catch (err) { return; }"
                "  if (s.cursor !== 'grab' && s.cursor !== 'pointer' && s.cursor !== 'move') return;"
                "  var r = e.getBoundingClientRect();"
                "  if (r.width >= 28 && r.height >= 20 && r.height <= 70 && r.width <= 120) out = e;"
                "});"
                "return out;")
        if thumb is None:
            out(ok=False, stage="find_thumb",
                error="页面上没找到滑块（可以传 thumb 选择器指定）")

        # ---- 算要拖多远 ----
        dist = None
        gap_info = {}
        if manual_dist is not None:
            try:
                dist = float(manual_dist)
            except Exception:
                dist = None
        if dist is None:
            if not auto:
                fail("既没给 distance 也没开 auto")
            # 从背景图识别缺口
            bg_sel = req.get("bg") or BG_SEL
            bg_data = drv.execute_script(
                "var sel = arguments[0];"
                "var els = [];"
                "try { els = document.querySelectorAll(sel); } catch (e) {}"
                "for (var i = 0; i < els.length; i++) {"
                "  var e = els[i];"
                "  var r = e.getBoundingClientRect();"
                "  if (r.width < 100 || r.height < 60) continue;"
                "  if (e.tagName === 'CANVAS') {"
                "    try { return {kind:'dataurl', data: e.toDataURL('image/png'),"
                "                   w: e.width, h: e.height, css_w: r.width}; } catch (err) {}"
                "    continue; }"
                "  if (e.tagName === 'IMG' && e.src) {"
                "    return {kind: 'url', data: e.src, w: e.naturalWidth, h: e.naturalHeight,"
                "            css_w: r.width}; }"
                "}"
                "return null;", bg_sel)
            if not bg_data:
                fail("没找到背景图（传 bg 选择器，或者直接用 distance 指定距离）")
            # 拿图片字节
            blob = b""
            try:
                if bg_data.get("kind") == "dataurl":
                    import base64
                    blob = base64.b64decode(bg_data["data"].split(",", 1)[1])
                else:
                    src = bg_data.get("data") or ""
                    if src.startswith("data:image"):
                        import base64
                        blob = base64.b64decode(src.split(",", 1)[1])
                    else:
                        import urllib.request
                        from urllib.parse import urljoin
                        ck = "; ".join("%s=%s" % (c.get("name"), c.get("value"))
                                       for c in drv.get_cookies())
                        rq = urllib.request.Request(
                            urljoin(drv.current_url, src),
                            headers={"User-Agent": UA, "Referer": drv.current_url,
                                     "Cookie": ck})
                        with urllib.request.urlopen(rq, timeout=10) as resp:
                            blob = resp.read()
            except Exception as e:
                fail("下载背景图失败：%s" % str(e).splitlines()[0][:120])

            gap_x, img_w = _find_gap_x(blob)
            if gap_x is None:
                fail("没能从背景图识别出缺口位置（这类验证码可能只有白色描边），"
                     "请用 distance 手动指定要拖多少像素")
            # 图片像素 → CSS 像素
            css_w = float(bg_data.get("css_w") or img_w or 1)
            scale = css_w / float(img_w or 1)
            dist = gap_x * scale
            gap_info = {"gap_x_img": gap_x, "img_w": img_w,
                        "css_w": round(css_w, 1), "scale": round(scale, 3),
                        "distance_css": round(dist, 1)}

        # ---- 拖 ----
        last = None
        for k in range(tries):
            # 默认走 CDP（时间戳真实、轨迹可控）；出问题可以传 cdp=false 回退
            if req.get("cdp", True):
                okd, _msg, info = _drag_cdp(drv, thumb, dist)
                if not okd:
                    okd, info = _drag_thumb(drv, thumb, dist, n_steps)
            else:
                okd, info = _drag_thumb(drv, thumb, dist, n_steps)
            time.sleep(float(req.get("settle", 2.2)))
            # 判定：cookie / 标题 / 页面提示
            try:
                ck = drv.get_cookies()
            except Exception:
                ck = []
            names = {c.get("name", "").lower() for c in ck}
            title = (drv.title or "")[:40]
            tip = drv.execute_script(
                "var out = '';"
                "document.querySelectorAll('div,span,p,label').forEach(function(e){"
                "  if (out) return;"
                "  var t='';"
                "  try { t = (e.innerText || '').trim(); } catch (err) { return; }"
                "  if (!t || t.length > 40) return;"
                "  if (/通过|成功|失败|重试|请重试|正确|passed|success|fail|retry/i.test(t)) out = t;"
                "});"
                "return out;") or ""
            done = (bool(re.search(r"通过|成功|正确|passed|success", tip, re.I))
                    or "验证通过" in title
                    or any("slider" in n or "verify" in n or "captcha" in n for n in names))
            # 通过还有一种表现：验证弹窗直接消失了（阿里 nc 过了就把遮罩拆掉，
            # 页面上什么都不留，只看 tip 文字会漏判成失败）
            if not done:
                try:
                    gone = drv.execute_script(
                        "var sels=['.nc-container','#nc_1_wrapper','#nc_1_n1t',"
                        "'[class*=slide-verify]','[class*=verify-wrap]','.baxia-dialog'];"
                        "for (var i=0;i<sels.length;i++){"
                        "  var e=document.querySelector(sels[i]);"
                        "  if (e){var r=e.getBoundingClientRect();"
                        "         if (r.width>2&&r.height>2) return false;}}"
                        "return true;")
                    if gone:
                        done = True
                        tip = tip or "验证弹窗已消失"
                except Exception:
                    pass
            history.append({
                "try": k + 1, "distance": round(dist, 1),
                "drag_ok": okd, "moved": round(info, 1) if isinstance(info, (int, float)) else info,
                "tip": tip, "title": title,
                "cookies": sorted(names)[:8], "passed": done,
            })
            last = history[-1]
            if done:
                break
            # 没过：滑块通常会回原位，重拖一次（距离微调 ±3px）
            if k + 1 < tries:
                import random
                dist += random.choice([-3, -2, 2, 3])
                try:
                    drv.execute_script(
                        "var s = document.querySelector(arguments[0]);"
                        "if (s) { s.dispatchEvent(new MouseEvent('mouseup', {bubbles:true})); }",
                        thumb_sel)
                except Exception:
                    pass
                time.sleep(1.0)

        page_url = drv.current_url
        passed = bool(last and last.get("passed"))
        obj = {
            "ok": passed,
            "stage": "done" if passed else "drag",
            "distance_used": round(dist, 1),
            "manual": manual_dist is not None,
            "gap": gap_info,
            "history": history,
            "final_url": page_url,
            "title": (drv.title or "")[:60],
        }
        if not passed:
            obj["error"] = ("拖了 %s px 还是没过（试了 %d 次）。可能缺口算偏了、"
                            "或者这个验证码还要别的动作（点选/旋转）。"
                            "可以先用 distance 手动试几个值" % (round(dist, 1), len(history)))
        try:
            obj["cookie_header"] = cookies_to_header(drv.get_cookies())
        except Exception:
            pass
        obj.update(_save_artifacts("drag", req, obj, obj.get("inputs"), obj.get("buttons")))
        sys.stdout.write(json.dumps(obj, ensure_ascii=False))
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)


ARTIFACT_DIR = "/tmp/llama_agent/gen"      # cgi ?file= 白名单里有 gen/


def _gen_dir():
    d = os.environ.get("BZ_GEN_DIR") or ARTIFACT_DIR
    try:
        os.makedirs(d, exist_ok=True)
    except Exception:
        return ""
    return d


def _q(s):
    """python 字面量里的字符串，保持中文可读"""
    return json.dumps(s if s is not None else "", ensure_ascii=False)


SCRIPT_HEAD = '''#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llama agent · browser 插件自动生成的复现脚本

生成时间 : {ts}
操作     : {op}
目标     : {url}
结果     : {result}
{extra}
这是插件实际执行的动作序列，可以直接跑（改一下 USER / PASS 就能用）：
    pip install selenium
    python3 {fname}

需要本机装了 Chrome。元素定位用的是插件当时识别到的 CSS 选择器，
页面改版了就换一个选择器。

账号密码从环境变量读，不写在文件里：
    LOGIN_USER='你的账号' LOGIN_PASS='你的密码' python3 {fname}
"""
import json
import os
import sys
import time

from selenium import webdriver
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.common.by import By
from selenium.webdriver.common.action_chains import ActionChains

URL  = {url_lit}

# 不把凭据写进文件：要跑就临时给环境变量
#     LOGIN_USER=xxx LOGIN_PASS=yyy python3 {fname}
USER = os.environ.get("LOGIN_USER", "")
PASS = os.environ.get("LOGIN_PASS", "")


HOOK_JS = r"""
(function(){
  if (window.__req) return;
  window.__req = [];
  var XO = XMLHttpRequest.prototype.open,
      XH = XMLHttpRequest.prototype.setRequestHeader,
      XS = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function(m, u){ this.__m = m; this.__u = u; this.__h = {}; return XO.apply(this, arguments); };
  XMLHttpRequest.prototype.setRequestHeader = function(k, v){ try { this.__h[k] = v; } catch (e) {} return XH.apply(this, arguments); };
  XMLHttpRequest.prototype.send = function(b){
    try { window.__req.push({kind: 'xhr', method: this.__m, url: this.__u, headers: this.__h,
                             body: (b == null ? '' : String(b).slice(0, 500))}); } catch (e) {}
    return XS.apply(this, arguments);
  };
  var OF = window.fetch;
  if (OF) {
    window.fetch = function(input, init){
      try {
        var u = (typeof input === 'string') ? input : ((input && input.url) || ''), h = {};
        var hs = (init && init.headers) || (input && input.headers);
        if (hs) {
          if (hs.forEach) hs.forEach(function(v, k){ h[k] = v; });
          else Object.keys(hs).forEach(function(k){ h[k] = hs[k]; });
        }
        window.__req.push({kind: 'fetch',
                           method: ((init && init.method) || 'GET') + '',
                           url: u + '', headers: h,
                           body: ((init && init.body) || '') + ''
                                 ? String((init && init.body) || '').slice(0, 500) : ''});
      } catch (e) {}
      return OF.apply(this, arguments);
    };
  }
})();
"""


def make_driver():
    o = Options()
    o.add_argument("--headless=new")
    o.add_argument("--no-sandbox")
    o.add_argument("--disable-dev-shm-usage")
    o.add_argument("--window-size=1366,900")
    o.add_argument("--password-store=basic")
    o.add_argument("--disable-features=PasswordManagerOnboarding,AutofillServerCommunication")
    o.add_experimental_option("excludeSwitches", ["enable-automation"])
    o.add_experimental_option("useAutomationExtension", False)
    d = webdriver.Chrome(options=o)
    try:
        d.execute_cdp_cmd("Page.addScriptToEvaluateOnNewDocument", {
            "source": "Object.defineProperty(navigator, 'webdriver', {get: () => undefined});"})
        d.execute_cdp_cmd("Page.addScriptToEvaluateOnNewDocument", {"source": HOOK_JS})
        d.execute_script(HOOK_JS)
    except Exception:
        pass
    return d


def fill(drv, sel, value):
    """填值。必须用真实键盘事件 —— React 站点只认 isTrusted 的输入。"""
    el = drv.find_element(By.CSS_SELECTOR, sel)
    drv.execute_script("var e = arguments[0];"
                       "if (e.isContentEditable) e.textContent = ''; else e.value = '';", el)
    ActionChains(drv).send_keys_to_element(el, value).perform()
    time.sleep(0.15)
    return el


def click_text(drv, text):
    """按可见文字点元素（div 假按钮也适用）。"""
    js = ("var want = arguments[0].replace(/[\\\\s\\\\u3000]+/g, '').toLowerCase(), hit = null;"
          "document.querySelectorAll('a,div,span,li,button,td,p,i,em').forEach(function(e){"
          "  if (hit) return;"
          "  var t = ''; try { t = (e.innerText || '').replace(/[\\\\s\\\\u3000]+/g, '').toLowerCase(); }"
          "  catch (x) { return; }"
          "  if (!t || t.length > 60) return;"
          "  var r = e.getBoundingClientRect();"
          "  if (r.width < 2 || r.height < 2) return;"
          "  if (t === want || t.indexOf(want) >= 0) hit = e;"
          "});"
          "if (hit) { hit.scrollIntoView({block: 'center'}); hit.click(); return true; }"
          "return false;")
    return drv.execute_script(js, text)


if not USER or not PASS:
    sys.exit("需要账号密码，用环境变量给：\\n"
             "    LOGIN_USER='你的账号' LOGIN_PASS='你的密码' python3 " + os.path.basename(__file__))

drv = make_driver()
try:
    drv.get(URL)
    time.sleep(2.5)
'''

SCRIPT_TAIL = '''
    # ---- 收尾：导出本次会话凭据 + 抓到的请求 ----
    print('当前页面:', drv.current_url)
    print('标题   :', drv.title)
    print()
    print('=== cookies ===')
    print(json.dumps(drv.get_cookies(), ensure_ascii=False, indent=2))
    print()
    print('=== localStorage / sessionStorage ===')
    print(json.dumps(drv.execute_script(
        "var o={local:{},session:{}};"
        "for (var i=0;i<localStorage.length;i++){var k=localStorage.key(i);o.local[k]=localStorage.getItem(k);}"
        "for (var i=0;i<sessionStorage.length;i++){var k=sessionStorage.key(i);o.session[k]=sessionStorage.getItem(k);}"
        "return o;"), ensure_ascii=False, indent=2))
    print()
    print('=== 页面发出的请求（含 JS 算出来的签名头）===')
    reqs = drv.execute_script('return window.__req || [];') or []
    for r in reqs:
        print(r.get('method'), r.get('url'))
        for k, v in (r.get('headers') or {}).items():
            print('    %s: %s' % (k, v))
    with open('requests_headers.json', 'w', encoding='utf-8') as f:
        json.dump(reqs, f, ensure_ascii=False, indent=2)
    print()
    print('请求头已存到 requests_headers.json')
finally:
    drv.quit()
'''


def _render_script(op, req, res):
    """把这次执行还原成一段能独立跑的 Python 脚本。

    目的很实际：插件跑挂了用户得能自己复现、自己改。所以脚本里全是真实
    selenium 调用（find_element + send_keys + click），元素定位用 CSS 选择器
    而不是本进程内的 window.__bz_in[0]，参数直接写死，改一下就能用。
    """
    import time as _t
    filled = res.get("filled") or {}
    verdict = res.get("verdict") or {}
    okv = bool(res.get("ok") or verdict.get("likely_ok"))

    extra = ""
    if res.get("error"):
        extra = "失败原因 : %s\n" % str(res.get("error"))[:140]

    # 用占位符替换，不用 str.format —— 模板里有一大堆 JS 花括号（{{ }} 转义
    # 起来太容易漏），replace 干净得多。注意顺序：{url_lit} 必须在 {url} 之前，
    # 否则 {url} 会把 {url_lit} 的前半截吃掉。
    head = SCRIPT_HEAD
    for key, val in (
            ("{url_lit}", _q(req.get("url") or "")),
            ("{user_lit}", _q(req.get("user") or "")),
            ("{pass_lit}", _q(req.get("pass") or "")),
            ("{extra}", extra),
            ("{fname}", os.path.basename(res.get("_fname") or "repro.py")),
            ("{result}", ("成功" if okv else "失败")),
            ("{ts}", _t.strftime("%Y-%m-%d %H:%M:%S")),
            ("{op}", op),
            ("{url}", (req.get("url") or "")),
    ):
        head = head.replace(key, val)

    B = []
    ins = res.get("probe_inputs") or []
    btns = res.get("probe_buttons") or []

    def sel_of(kind, idx, fallback):
        pool = ins if kind == "in" else btns
        for it in pool:
            if it.get("i") == idx and it.get("sel"):
                return it["sel"]
        return fallback

    # ---- 登录 ----
    if op in ("autologin", "login") or (op == "hunt" and filled.get("user_i") is not None):
        u_sel = sel_of("in", filled.get("user_i"), "input[type='text']")
        p_sel = sel_of("in", filled.get("pass_i"), "input[type='password']")
        b_sel = sel_of("btn", filled.get("btn_i"), "button[type='submit']")
        B.append("    # ---- 登录 ----")
        B.append("    fill(drv, %s, USER)      # 账号框" % _q(u_sel))
        B.append("    fill(drv, %s, PASS)      # 密码框" % _q(p_sel))
        if filled.get("captcha_text"):
            B.append("    # 这次页面上有验证码，插件识别成 %r（复现时要自己处理）"
                     % filled["captcha_text"])
        B.append("    # 点登录按钮")
        B.append("    try:")
        B.append("        drv.find_element(By.CSS_SELECTOR, %s).click()" % _q(b_sel))
        B.append("    except Exception:")
        B.append("        click_text(drv, '登录')     # 兜底：按文字点")
        B.append("    time.sleep(2.5)")
        B.append("")

    # ---- hunt 流程 ----
    if op == "hunt":
        flow = req.get("flow") or []
        if not flow and req.get("path"):
            flow = [{"act": "click", "text": t} for t in req["path"]]
        if flow:
            B.append("    # ---- 流程：按页面顺序走 ----")
            for i, st in enumerate(flow, 1):
                if not isinstance(st, dict):
                    continue
                act = (st.get("act") or "click").lower()
                cmt = "    # 第%d步" % i
                if st.get("page"):
                    cmt += " · " + str(st["page"])
                if st.get("note"):
                    cmt += " · " + str(st["note"])
                B.append(cmt)
                if act == "login":
                    B.append("    fill(drv, \"input[name='username']\", USER)")
                    B.append("    fill(drv, \"input[type='password']\", PASS)")
                    B.append("    click_text(drv, '登录')")
                    B.append("    time.sleep(2.5)")
                elif act == "click":
                    B.append("    click_text(drv, %s)" % _q(st.get("text") or ""))
                    B.append("    time.sleep(1.5)")
                elif act == "fill":
                    ph = st.get("field") or "账号"
                    B.append("    fill(drv, \"input[placeholder*='%s']\", %s)"
                             % (str(ph).replace("'", ""), _q(st.get("value") or "")))
                elif act == "press":
                    B.append("    drv.execute_script(\"var e=document.activeElement||document.body;"
                             "['keydown','keypress','keyup'].forEach(function(t){"
                             "e.dispatchEvent(new KeyboardEvent(t,{key:%s,bubbles:true}));});\")"
                             % json.dumps(st.get("key") or "Enter"))
                    B.append("    time.sleep(1.5)")
                elif act == "wait":
                    B.append("    time.sleep(%s)" % (st.get("sec") or 2))
                elif act == "wait_for":
                    B.append("    # 等「%s」出现" % (st.get("text") or ""))
                    B.append("    for _ in range(%d):" % int(st.get("tries") or 20))
                    B.append("        if drv.execute_script(\"return document.body.innerText"
                             ".indexOf(arguments[0]) >= 0;\", %s):" % _q(st.get("text") or ""))
                    B.append("            break")
                    B.append("        time.sleep(0.4)")
                elif act == "capture":
                    B.append("    # ↑ 目标页：从这里开始的请求才是要抓的")
                B.append("")

    # ---- drag ----
    if op == "drag":
        B.append("    # ---- 拖拽滑块 ----")
        B.append("    thumb = drv.find_element(By.CSS_SELECTOR, %s)"
                 % _q(req.get("thumb") or ".slider-btn, [class*=thumb]"))
        B.append("    Distance = %s        # 插件这次拖的像素数"
                 % (res.get("distance_used") or req.get("distance") or 0))
        B.append("    x0 = thumb.location['x']")
        B.append("    ActionChains(drv).move_to_element(thumb).click_and_hold().perform()")
        B.append("    time.sleep(0.15)")
        B.append("    ActionChains(drv).move_by_offset(Distance, 0).perform()")
        B.append("    # 浏览器对鼠标坐标取整，累积下来会差十几像素，补一次")
        B.append("    miss = Distance - (thumb.location['x'] - x0)")
        B.append("    if abs(miss) >= 2:")
        B.append("        ActionChains(drv).move_by_offset(miss, 0).perform()")
        B.append("    time.sleep(0.1)")
        B.append("    ActionChains(drv).release().perform()")
        B.append("    time.sleep(2)")
        B.append("")

    return head + "\n".join(B) + SCRIPT_TAIL


def _save_artifacts(op, req, res, probe_inputs=None, probe_buttons=None):
    """写复现脚本（成功时再加请求头 JSON 和 curl）。返回要补进 res 的字段。"""
    d = _gen_dir()
    if not d:
        return {}
    import time as _t
    stamp = _t.strftime("%Y%m%d_%H%M%S")
    base = "%s_%s" % (op, stamp)
    out = {}

    if probe_inputs:
        res["probe_inputs"] = probe_inputs
    if probe_buttons:
        res["probe_buttons"] = probe_buttons

    # ---- 1. 复现脚本（成功失败都生成）----
    try:
        py = os.path.join(d, base + ".py")
        res["_fname"] = os.path.basename(py)
        code = _render_script(op, req, res)
        with open(py, "w", encoding="utf-8") as f:
            f.write(code)
        try:
            os.chmod(py, 0o644)
        except Exception:
            pass
        out["script_path"] = py
    except Exception as e:
        out["script_error"] = str(e)[:140]
    finally:
        res.pop("_fname", None)
        res.pop("probe_inputs", None)
        res.pop("probe_buttons", None)

    # ---- 2. 成功时导出请求头 + curl ----
    reqs = res.get("requests") or ([res["best"]] if res.get("best") else [])
    if res.get("ok") and reqs:
        try:
            hj = os.path.join(d, base + "_headers.json")
            with open(hj, "w", encoding="utf-8") as f:
                json.dump(reqs, f, ensure_ascii=False, indent=2)
            out["headers_path"] = hj
        except Exception as e:
            out["headers_error"] = str(e)[:140]
        try:
            sh = os.path.join(d, base + "_curl.sh")
            page = res.get("final_url") or req.get("url") or ""
            lines = ["#!/bin/sh",
                     "# 由 llama agent 生成：把抓到的请求还原成 curl",
                     "# 生成时间 %s" % stamp, ""]
            for r in reqs:
                lines.append("# %s %s" % (r.get("method") or "GET",
                                          (r.get("url") or "")[:100]))
                lines.append(_curl_from_req(r, page))
                lines.append("")
            with open(sh, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
            try:
                os.chmod(sh, 0o755)
            except Exception:
                pass
            out["curl_path"] = sh
        except Exception as e:
            out["curl_error"] = str(e)[:140]

    return out


def do_captcha(req):
    """单独识别验证码：返回识别结果 + 每张截图，识别不出来给人看"""
    url = req.get("url") or ""
    if not url:
        fail("缺少 url")
    out_dir = req.get("out_dir") or SHOT_DIR
    # 明确要识别验证码 → 起 chrome 之前先看 NPU 能不能用，
    # 不然白起一个 300MB 的浏览器再失败
    ok_npu, why_npu = _ppocr_usable()
    if not ok_npu:
        fail(why_npu, npu_unavailable=True, cma_free_kb=_cma_free_kb())
    drv = new_driver()
    try:
        try:
            drv.get(url)
        except Exception as e:
            fail("打开页面失败：%s" % str(e).splitlines()[0][:200])
        time.sleep(float(req.get("wait", 2)))
        drv.execute_script(JS_PROBE)
        got = _solve_captcha(drv, out_dir, int(req.get("retry", 3)))
        if not got:
            fail("页面上没找到验证码图片",
                 hint="有些站点验证码在 iframe 或 canvas 里，用 op=shot 整页截图看看")
        if got.get("npu_unavailable"):
            fail(got.get("error") or "NPU 不可用，验证码识别起不来",
                 npu_unavailable=True, pngs=got.get("pngs", []))
        out(ok=True, url=drv.current_url, **got)
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)


def do_cookie(req):
    url = req.get("url") or ""
    drv = new_driver()
    try:
        if url:
            try:
                drv.get(url)
            except Exception as e:
                fail("打开页面失败：%s" % str(e).splitlines()[0][:200])
            time.sleep(float(req.get("wait", 2)))
        cookies = drv.get_cookies()
        dom = req.get("domain") or ""
        if dom:
            cookies = [c for c in cookies if dom in (c.get("domain") or "")]
        sys.stdout.write(json.dumps({
            "ok": True,
            "url": drv.current_url if url else "",
            "cookies": cookies,
            "cookie_count": len(cookies),
            "cookie_header": cookies_to_header(cookies),
            "storage": (drv.execute_script(JS_STORAGE) if url else {}),
            "mem_mb": mem_avail_mb(),
        }, ensure_ascii=False))
    finally:
        try:
            drv.quit()
        except Exception:
            pass
    sys.exit(0)


def do_shot(req):
    url = req.get("url") or ""
    path = req.get("path") or (WORKDIR + "/shot.png")
    ensure_dir(path)
    drv = new_driver(int(req.get("width", 1366)), int(req.get("height", 900)))
    try:
        if url:
            try:
                drv.get(url)
            except Exception as e:
                fail("打开页面失败：%s" % str(e).splitlines()[0][:200])
            time.sleep(float(req.get("wait", 2)))
        ok = drv.save_screenshot(path)
        out(path=path, saved=bool(ok), size=os.path.getsize(path) if ok else 0,
            title=drv.title)
    finally:
        try:
            drv.quit()
        except Exception:
            pass


def main():
    if len(sys.argv) < 2:
        fail("缺少 op（probe / login / cookie / shot）")
    op = sys.argv[1]
    raw = sys.stdin.read()
    try:
        req = json.loads(raw) if raw.strip() else {}
    except ValueError as e:
        fail("参数不是合法 JSON：%s" % e)

    avail = mem_avail_mb()
    if avail < 350:
        fail("可用内存只有 %d MB，Chrome 起不来。先跑 /root/freemm.sh 重启释放内存" % avail)

    if op == "probe":
        do_probe(req)
    elif op == "login":
        do_login(req)
    elif op == "autologin":
        do_autologin(req)
    elif op == "captcha":
        do_captcha(req)
    elif op == "drag":
        do_drag(req)
    elif op == "hunt":
        do_hunt(req)
    elif op == "cookie":
        do_cookie(req)
    elif op == "shot":
        do_shot(req)
    else:
        fail("未知 op：%s" % op)


if __name__ == "__main__":
    main()



