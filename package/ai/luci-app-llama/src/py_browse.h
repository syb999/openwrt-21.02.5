/* 自动生成：browse.py 内嵌到这里，browser.so 不依赖板上任何 .py */
#ifndef PY_BROWSE_SRC
#define PY_BROWSE_SRC \
    "#!/usr/bin/env python3\n" \
    "# -*- coding: utf-8 -*-\n" \
    "\"\"\"browse.py — 登录态网站抓取（selenium + chrome-headless-shell in glibc chroot）\n" \
    "\n" \
    "接口（和 docwrite.py 一致，C 侧 fork python3 -c <本脚本> <op>，数据走 stdin/stdout）：\n" \
    "    argv[1] = op\n" \
    "    stdin   = 参数 JSON\n" \
    "    stdout  = 结果 JSON（永远是单个对象，诊断信息走 stderr）\n" \
    "\n" \
    "op:\n" \
    "    probe   打开 URL，探测可填的输入框和可点的按钮 → 给模型挑\n" \
    "    login   一次性完成：打开 → 按 steps 填表/点击 → 等 → 导出 cookie\n" \
    "    cookie  打开 URL 后导出 cookie（已经登录过的站点，靠 steps 也能带登录）\n" \
    "    shot    截图存文件（调试用）\n" \
    "\n" \
    "为什么 login 必须一次跑完：这块板子上 chromedriver 不能带 --user-data-dir\n" \
    "（chroot 里它找不到 DevToolsActivePort，60 秒后报 unable to discover open pages），\n" \
    "所以会话没法跨进程复用。每次调用都是新浏览器，登录流程得在同一个进程里走完。\n" \
    "\"\"\"\n" \
    "\n" \
    "import sys\n" \
    "import json\n" \
    "import os\n" \
    "import re\n" \
    "import subprocess\n" \
    "import time\n" \
    "\n" \
    "CDRV = \"/opt/chrome-root/cdrv.sh\"\n" \
    "CHROME_BIN = \"/opt/chrome/chrome-headless-shell\"\n" \
    "WORKDIR = \"/tmp/browse\"\n" \
    "os.environ.setdefault(\"TESSDATA_PREFIX\", \"/usr/share/tessdata\")\n" \
    "UA = (\"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 \"\n" \
    "      \"(KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36\")\n" \
    "\n" \
    "# 反自动化检测：webdriver 标记、空 plugins、空 languages、chrome.runtime\n" \
    "STEALTH_JS = r\"\"\"\n" \
    "(function () {\n" \
    "  try { Object.defineProperty(navigator, 'webdriver', { get: function () { return undefined; } }); } catch (e) {}\n" \
    "  try {\n" \
    "    if (!navigator.plugins || !navigator.plugins.length) {\n" \
    "      Object.defineProperty(navigator, 'plugins', { get: function () { return [1, 2, 3, 4, 5]; } });\n" \
    "    }\n" \
    "  } catch (e) {}\n" \
    "  try {\n" \
    "    if (!navigator.languages || !navigator.languages.length) {\n" \
    "      Object.defineProperty(navigator, 'languages', { get: function () { return ['zh-CN', 'zh', 'en']; } });\n" \
    "    }\n" \
    "  } catch (e) {}\n" \
    "  try { window.chrome = window.chrome || { runtime: {} }; } catch (e) {}\n" \
    "})();\n" \
    "\"\"\"\n" \
    "\n" \
    "# 探测页面上的输入框 / 按钮 / iframe，并把元素引用挂到 window.__bz_in / __bz_btn\n" \
    "JS_PROBE = r\"\"\"\n" \
    "return (() => {\n" \
    "  const vis = el => {\n" \
    "    try {\n" \
    "      const r = el.getBoundingClientRect();\n" \
    "      const s = getComputedStyle(el);\n" \
    "      return r.width > 1 && r.height > 1 && s.display !== 'none'\n" \
    "             && s.visibility !== 'hidden' && s.opacity !== '0';\n" \
    "    } catch (e) { return false; }\n" \
    "  };\n" \
    "  const labelOf = el => {\n" \
    "    try {\n" \
    "      if (el.labels && el.labels.length) return (el.labels[0].innerText || '').trim().slice(0, 40);\n" \
    "      const p = el.closest('label');\n" \
    "      if (p) return (p.innerText || '').trim().slice(0, 40);\n" \
    "      const prev = el.previousElementSibling;\n" \
    "      if (prev && prev.innerText && prev.innerText.trim().length < 20) return prev.innerText.trim();\n" \
    "    } catch (e) {}\n" \
    "    return '';\n" \
    "  };\n" \
    "\n" \
    "  /* ---- Shadow DOM 穿透：Web Component 里的登录框藏在 shadowRoot 里，\n" \
    "          document.querySelectorAll 完全看不到。把所有能访问的 root 收集起来，\n" \
    "          open 模式的能穿，closed 模式的穿不透（只能靠外部结构猜）。---- */\n" \
    "  const roots = [];\n" \
    "  const collect = (root, depth) => {\n" \
    "    if (!root || depth > 6 || roots.indexOf(root) >= 0) return;\n" \
    "    roots.push(root);\n" \
    "    let els = [];\n" \
    "    try { els = root.querySelectorAll('*'); } catch (e) { return; }\n" \
    "    els.forEach(el => { if (el.shadowRoot) collect(el.shadowRoot, depth + 1); });\n" \
    "  };\n" \
    "  collect(document, 0);\n" \
    "  window.__bz_roots = roots;\n" \
    "\n" \
    "  window.__bz_in = [];\n" \
    "  window.__bz_btn = [];\n" \
    "  window.__bz_tab = [];\n" \
    "  const inputs = [];\n" \
    "  const buttons = [];\n" \
    "  const SKIP_T = ['hidden','submit','button','image','file','reset','checkbox','radio'];\n" \
    "\n" \
    "  /* 生成一个能直接拿去 find_element(By.CSS_SELECTOR) 的选择器。\n" \
    "     导出复现脚本时要用 —— 脚本里不能写 window.__bz_in[0]，那只能是本进程内的。\n" \
    "     优先级：id > name > type > class > 标签名。 */\n" \
    "  const selOf = el => {\n" \
    "    try {\n" \
    "      if (el.id && /^[A-Za-z][\\w-]*$/.test(el.id)) return '#' + el.id;\n" \
    "      const tag = (el.tagName || '').toLowerCase();\n" \
    "      if (el.name) return tag + \"[name='\" + String(el.name).replace(/'/g, \"\\\\'\") + \"']\";\n" \
    "      if (el.type && tag === 'input') return tag + \"[type='\" + el.type + \"']\";\n" \
    "      const cls = ((el.className || '') + '').trim().split(/\\s+/).filter(function(c){ return c; });\n" \
    "      if (cls.length) return tag + '.' + cls.slice(0, 2).join('.');\n" \
    "      return tag;\n" \
    "    } catch (e) { return ''; }\n" \
    "  };\n" \
    "\n" \
    "  /* 生成 XPath。CSS 选择器对\"靠 JS 绑定事件的 div/a\"经常定位不到\n" \
    "     （没有 name/id/class），XPath 按可见文字反而稳。 */\n" \
    "  const xpathOf = el => {\n" \
    "    try {\n" \
    "      if (el.id && /^[A-Za-z][\\w-]*$/.test(el.id)) return \"//*[@id='\" + el.id + \"']\";\n" \
    "      const tag = (el.tagName || '').toLowerCase();\n" \
    "      const q = s => String(s).replace(/'/g, \"\\\\'\");\n" \
    "      if (tag === 'input' || tag === 'textarea') {\n" \
    "        if (el.name) return \"//\" + tag + \"[@name='\" + q(el.name) + \"']\";\n" \
    "        if (el.placeholder) return \"//\" + tag + \"[@placeholder='\" + q(el.placeholder) + \"']\";\n" \
    "        const t = (el.type || '').toLowerCase();\n" \
    "        if (t && t !== 'text') return \"//\" + tag + \"[@type='\" + t + \"']\";\n" \
    "      }\n" \
    "      const txt = ((el.innerText || el.value || '') + '').trim().replace(/\\s+/g, ' ');\n" \
    "      if (txt && txt.length <= 30) {\n" \
    "        if (tag === 'button') return \"//button[normalize-space(.)='\" + q(txt) + \"']\";\n" \
    "        if (tag === 'a')      return \"//a[normalize-space(.)='\" + q(txt) + \"']\";\n" \
    "        if (tag === 'input')  return \"//input[@value='\" + q(txt) + \"']\";\n" \
    "        return \"//\" + tag + \"[normalize-space(text())='\" + q(txt) + \"']\";\n" \
    "      }\n" \
    "      const cls = ((el.className || '') + '').trim().split(/\\s+/)[0];\n" \
    "      if (cls) return \"//\" + tag + \"[contains(@class,'\" + q(cls) + \"')]\";\n" \
    "      return \"\";\n" \
    "    } catch (e) { return \"\"; }\n" \
    "  };\n" \
    "\n" \
    "  /* 这个元素是不是\"只靠 JS 事件\"才能点。\n" \
    "     href / onclick / form 都没有，说明是组件自己 addEventListener 挂的 ——\n" \
    "     用 CSS/XPath 找到之后必须 el.click() 触发，光看 HTML 看不出可点。 */\n" \
    "  const jsOnlyOf = el => {\n" \
    "    try {\n" \
    "      const tag = (el.tagName || '');\n" \
    "      if (el.getAttribute && el.getAttribute('onclick')) return false;\n" \
    "      const href = el.getAttribute && el.getAttribute('href');\n" \
    "      if (href && href !== '#' && href.indexOf('javascript:') !== 0) return false;\n" \
    "      if (el.form || (tag === 'BUTTON' && el.type === 'submit')) return false;\n" \
    "      if (tag === 'INPUT' && (el.type === 'submit' || el.type === 'button')) return false;\n" \
    "      return true;   /* 其余（div/span/a[href=#]）基本都靠 JS */\n" \
    "    } catch (e) { return false; }\n" \
    "  };\n" \
    "\n" \
    "  /* 点得到吗：中心点上最顶层元素是不是它自己（被遮罩挡住就是 false） */\n" \
    "  const hitOf = el => {\n" \
    "    try {\n" \
    "      const r = el.getBoundingClientRect();\n" \
    "      if (r.width < 2 || r.height < 2) return false;\n" \
    "      const x = r.left + Math.min(r.width / 2, 24);\n" \
    "      const y = r.top + r.height / 2;\n" \
    "      if (x < 0 || y < 0 || x > window.innerWidth || y > window.innerHeight) return false;\n" \
    "      const top = document.elementFromPoint(x, y);\n" \
    "      return !!(top && (top === el || el.contains(top) || (top.contains && top.contains(el))));\n" \
    "    } catch (e) { return false; }\n" \
    "  };\n" \
    "\n" \
    "  const roundRect = el => {\n" \
    "    try {\n" \
    "      const r = el.getBoundingClientRect();\n" \
    "      return [Math.round(r.left), Math.round(r.top),\n" \
    "              Math.round(r.width), Math.round(r.height)];\n" \
    "    } catch (e) { return null; }\n" \
    "  };\n" \
    "\n" \
    "  const addInput = (el, ce) => {\n" \
    "    window.__bz_in.push(el);\n" \
    "    inputs.push({\n" \
    "      i: window.__bz_in.length - 1,\n" \
    "      type: ce ? 'editable' : ((el.type || 'text') + '').toLowerCase(),\n" \
    "      name: ((el.name || '') + '').slice(0, 40),\n" \
    "      id: ((el.id || '') + '').slice(0, 40),\n" \
    "      placeholder: ((el.placeholder || el.getAttribute('data-ph') || '') + '').slice(0, 40),\n" \
    "      autocomplete: ((el.autocomplete || '') + '').slice(0, 30),\n" \
    "      aria: ((el.getAttribute('aria-label') || el.getAttribute('aria-labelledby') || '') + '').slice(0, 40),\n" \
    "      label: labelOf(el),\n" \
    "      ce: ce ? 1 : 0,\n" \
    "      sel: selOf(el),\n" \
    "      xpath: xpathOf(el),\n" \
    "      rect: roundRect(el),\n" \
    "      hit: hitOf(el),\n" \
    "      jsOnly: jsOnlyOf(el)\n" \
    "    });\n" \
    "  };\n" \
    "\n" \
    "  roots.forEach(root => {\n" \
    "    /* ---- 输入框：input / textarea / contenteditable ---- */\n" \
    "    let els = [];\n" \
    "    try {\n" \
    "      els = root.querySelectorAll(\n" \
    "        'input, textarea, [contenteditable=\"\"], [contenteditable=\"true\"], [role=textbox]');\n" \
    "    } catch (e) {}\n" \
    "    els.forEach(el => {\n" \
    "      const t = ((el.type || 'text') + '').toLowerCase();\n" \
    "      if (SKIP_T.indexOf(t) >= 0) return;\n" \
    "      if (el.disabled || el.readOnly) return;\n" \
    "      if (!vis(el)) return;\n" \
    "      const ce = (el.isContentEditable === true)\n" \
    "                 || el.getAttribute('contenteditable') === 'true'\n" \
    "                 || el.getAttribute('contenteditable') === '';\n" \
    "      addInput(el, ce);\n" \
    "    });\n" \
    "\n" \
    "    /* ---- 标准按钮 ---- */\n" \
    "    let bs = [];\n" \
    "    try {\n" \
    "      bs = root.querySelectorAll(\n" \
    "        'button, input[type=submit], input[type=button], a[role=button], [role=button]');\n" \
    "    } catch (e) {}\n" \
    "    bs.forEach(el => {\n" \
    "      if (!vis(el)) return;\n" \
    "      const txt = ((el.innerText || el.value || '') + '').trim().replace(/\\s+/g, ' ').slice(0, 40);\n" \
    "      if (!txt) return;\n" \
    "      window.__bz_btn.push(el);\n" \
    "      buttons.push({\n" \
    "        i: window.__bz_btn.length - 1,\n" \
    "        tag: (el.tagName || '') + '',\n" \
    "        type: ((el.type || '') + '').slice(0, 20),\n" \
    "        text: txt,\n" \
    "        sel: selOf(el),\n" \
    "        xpath: xpathOf(el),\n" \
    "        rect: roundRect(el),\n" \
    "        hit: hitOf(el),\n" \
    "        jsOnly: jsOnlyOf(el)\n" \
    "      });\n" \
    "    });\n" \
    "\n" \
    "    /* ---- 伪装成按钮的元素：div/span/a，靠 cursor:pointer + 文案认 ----\n" \
    "           提交按钮用 div 做是现在很常见的写法（Vue/React 组件库尤其多） */\n" \
    "    let ds = [];\n" \
    "    try { ds = root.querySelectorAll('div, span, a, li, p'); } catch (e) {}\n" \
    "    ds.forEach(el => {\n" \
    "      if (!vis(el)) return;\n" \
    "      if (el.closest && el.closest('button, [role=button]')) return;\n" \
    "      let s;\n" \
    "      try { s = getComputedStyle(el); } catch (e) { return; }\n" \
    "      if (s.cursor !== 'pointer') return;\n" \
    "      const txt = ((el.innerText || '') + '').trim().replace(/\\s+/g, ' ');\n" \
    "      if (!txt || txt.length > 24) return;\n" \
    "      /* 中文按钮常被排版成\"登 录\"/\"登  录\"，得允许字间空格 */\n" \
    "      if (!/登\\s*录|登\\s*陆|提交|确认|下一步|继续|立即|login|sign\\s?in|submit|next|continue|enter/i.test(txt)) return;\n" \
    "      if (window.__bz_btn.indexOf(el) >= 0) return;\n" \
    "      window.__bz_btn.push(el);\n" \
    "      buttons.push({\n" \
    "        i: window.__bz_btn.length - 1,\n" \
    "        tag: (el.tagName || '') + '',\n" \
    "        type: 'fake',\n" \
    "        text: txt.slice(0, 40),\n" \
    "        sel: selOf(el),\n" \
    "        xpath: xpathOf(el),\n" \
    "        rect: roundRect(el),\n" \
    "        hit: hitOf(el),\n" \
    "        jsOnly: jsOnlyOf(el)\n" \
    "      });\n" \
    "      window.__bz_tab.push(el);\n" \
    "    });\n" \
    "  });\n" \
    "\n" \
    "  /* ---- 人机验证检测：图片验证码 / reCAPTCHA / hCaptcha / 滑块 ---- */\n" \
    "  let captcha = false, captcha_kind = '';\n" \
    "  const probeTxt = (el) => ((el && (el.id + ' ' + el.className + ' ' + el.src)) + '').toLowerCase();\n" \
    "  const has = (sel) => {\n" \
    "    let n = false;\n" \
    "    roots.forEach(r => { try { if (!n && r.querySelector(sel)) n = true; } catch (e) {} });\n" \
    "    return n;\n" \
    "  };\n" \
    "  if (has('iframe[src*=recaptcha], .g-recaptcha, #g-recaptcha, [data-sitekey]')) {\n" \
    "    captcha = true; captcha_kind = 'recaptcha';\n" \
    "  } else if (has('iframe[src*=hcaptcha], .h-captcha')) {\n" \
    "    captcha = true; captcha_kind = 'hcaptcha';\n" \
    "  } else if (has('.geetest, [class*=slider], [class*=drag-verify]')) {\n" \
    "    captcha = true; captcha_kind = 'slider';\n" \
    "  } else {\n" \
    "    const CAP = \"img[src*=captcha], img[src*=verify], img[src*=code], img[id*=captcha], \"\n" \
    "              + \"img[id*=cap], img[class*=captcha], img[alt*=验证码], img[alt*=captcha], \"\n" \
    "              + \"canvas[class*=captcha], [class*=captcha] img, [id*=captcha] img\";\n" \
    "    if (has(CAP)) { captcha = true; captcha_kind = 'image'; }\n" \
    "  }\n" \
    "\n" \
    "  let title = '';\n" \
    "  try { title = document.title || ''; } catch (e) {}\n" \
    "\n" \
    "  /* iframes 必须是列表：_probe_anywhere 靠它逐个 switch_to.frame 进去找表单 */\n" \
    "  const iframes = [];\n" \
    "  try {\n" \
    "    document.querySelectorAll('iframe').forEach((f, i) => {\n" \
    "      iframes.push({i: i, src: ((f.src || f.getAttribute('src') || '') + '').slice(0, 60)});\n" \
    "    });\n" \
    "  } catch (e) {}\n" \
    "\n" \
    "  return {\n" \
    "    url: location.href,\n" \
    "    title: title,\n" \
    "    inputs: inputs,\n" \
    "    buttons: buttons,\n" \
    "    captcha: captcha,\n" \
    "    captcha_kind: captcha_kind,\n" \
    "    roots: roots.length,\n" \
    "    iframes: iframes,\n" \
    "    mem_mb: 0\n" \
    "  };\n" \
    "})()\n" \
    "\"\"\"\n" \
    "\n" \
    "JS_STORAGE = r\"\"\"\n" \
    "return (() => {\n" \
    "  const out = {local: {}, session: {}};\n" \
    "  try {\n" \
    "    for (let i = 0; i < localStorage.length; i++) {\n" \
    "      const k = localStorage.key(i);\n" \
    "      out.local[k] = localStorage.getItem(k);\n" \
    "    }\n" \
    "  } catch (e) {}\n" \
    "  try {\n" \
    "    for (let i = 0; i < sessionStorage.length; i++) {\n" \
    "      const k = sessionStorage.key(i);\n" \
    "      out.session[k] = sessionStorage.getItem(k);\n" \
    "    }\n" \
    "  } catch (e) {}\n" \
    "  return out;\n" \
    "})();\n" \
    "\"\"\"\n" \
    "\n" \
    "JS_FILL = r\"\"\"\n" \
    "/* 执行时带上 {el: 元素, value: 字符串}，这里用 arguments[0] 拿 */\n" \
    "return (() => {\n" \
    "  const arg = arguments[0] || {};\n" \
    "  const target = arg.el ? arg.el\n" \
    "    : (arg.i === undefined ? null : (window.__bz_in || [])[arg.i]);\n" \
    "  if (!target) return {ok: false, why: 'no element'};\n" \
    "  const v = arg.value === undefined ? '' : String(arg.value);\n" \
    "  const fire = (node, type) => {\n" \
    "    try {\n" \
    "      node.dispatchEvent(new Event(type, {bubbles: true, cancelable: true}));\n" \
    "      node.dispatchEvent(new InputEvent(type, {bubbles: true, cancelable: true, data: v}));\n" \
    "    } catch (e) {\n" \
    "      try { node.dispatchEvent(new Event(type, {bubbles: true})); } catch (e2) {}\n" \
    "    }\n" \
    "  };\n" \
    "\n" \
    "  /* contenteditable：没有 value，得改 textContent + 手动派发 InputEvent */\n" \
    "  if (target.isContentEditable\n" \
    "      || target.getAttribute('contenteditable') === 'true'\n" \
    "      || target.getAttribute('contenteditable') === '') {\n" \
    "    target.focus();\n" \
    "    target.textContent = v;\n" \
    "    fire(target, 'input');\n" \
    "    fire(target, 'change');\n" \
    "    try { target.blur(); } catch (e) {}\n" \
    "    return {ok: true, val: (target.innerText || '').trim(), ce: true};\n" \
    "  }\n" \
    "\n" \
    "  /* 普通 input/textarea：走原生 setter，绕开 React/Vue 的受控值拦截 */\n" \
    "  try {\n" \
    "    const proto = target.tagName === 'TEXTAREA'\n" \
    "      ? window.HTMLTextAreaElement.prototype\n" \
    "      : window.HTMLInputElement.prototype;\n" \
    "    const setter = Object.getOwnPropertyDescriptor(proto, 'value');\n" \
    "    if (setter && setter.set) {\n" \
    "      setter.set.call(target, '');\n" \
    "      fire(target, 'input');\n" \
    "      setter.set.call(target, v);\n" \
    "    } else {\n" \
    "      target.value = v;\n" \
    "    }\n" \
    "  } catch (e) {\n" \
    "    try { target.value = v; } catch (e2) { return {ok: false, why: 'set failed'}; }\n" \
    "  }\n" \
    "  target.focus();\n" \
    "  fire(target, 'input');\n" \
    "  fire(target, 'change');\n" \
    "  try { target.blur(); } catch (e) {}\n" \
    "  return {ok: true, val: target.value, ce: false};\n" \
    "})()\n" \
    "\"\"\"\n" \
    "\n" \
    "JS_CLICK = r\"\"\"\n" \
    "return (() => {\n" \
    "  const arg = arguments[0] || {};\n" \
    "  const list = arg.kind === 'tab' ? (window.__bz_tab || []) : (window.__bz_btn || []);\n" \
    "  let el = null;\n" \
    "  if (arg.el) el = arg.el;\n" \
    "  else if (arg.i !== undefined) el = list[arg.i];\n" \
    "  if (!el) return {ok: false, why: 'no button at ' + arg.i};\n" \
    "  try { el.scrollIntoView({block: 'center'}); } catch (e) {}\n" \
    "  try { el.focus(); } catch (e) {}\n" \
    "  /* 只调一次 el.click()：它会同时触发 onclick 和 React/Vue 挂在根节点上的\n" \
    "     原生监听。再补 dispatchEvent 会让回调执行两次 —— p3 页面就是这么坏的\n" \
    "     （ok() 跑两遍，第二次时表单状态已经乱了）。 */\n" \
    "  let okc = false;\n" \
    "  try { el.click(); okc = true; } catch (e) {}\n" \
    "  return {ok: okc, tag: (el.tagName || '') + '', text: ((el.innerText || '') + '').trim().slice(0, 30)};\n" \
    "})()\n" \
    "\"\"\"\n" \
    "\n" \
    "\n" \
    "def out(**kw):\n" \
    "    kw.setdefault(\"ok\", True)\n" \
    "    sys.stdout.write(json.dumps(kw, ensure_ascii=False))\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "def fail(msg, **kw):\n" \
    "    kw.update({\"ok\": False, \"error\": msg})\n" \
    "    sys.stdout.write(json.dumps(kw, ensure_ascii=False))\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "def mem_avail_mb():\n" \
    "    try:\n" \
    "        with open(\"/proc/meminfo\") as f:\n" \
    "            for line in f:\n" \
    "                if line.startswith(\"MemAvailable:\"):\n" \
    "                    return int(line.split()[1]) // 1024\n" \
    "    except OSError:\n" \
    "        pass\n" \
    "    return 99999\n" \
    "\n" \
    "\n" \
    "def ensure_dir(p):\n" \
    "    d = os.path.dirname(p)\n" \
    "    if d and not os.path.isdir(d):\n" \
    "        os.makedirs(d, exist_ok=True)\n" \
    "\n" \
    "\n" \
    "# 请求拦截器：把页面自己发出的 XHR/fetch 连同完整请求头记下来。\n" \
    "# 很多站点的 token 是 JS 里算的（X-Sign、X-Token、时间戳+md5 之类），逆向成本\n" \
    "# 极高 —— 但浏览器已经算好了，直接抄下来就能拿去 curl。\n" \
    "# 结果存 sessionStorage：页面一跳转 window 变量就没了，sessionStorage 还在。\n" \
    "JS_HOOK = r\"\"\"\n" \
    "(function(){\n" \
    "  if (window.__bz_hook) return;\n" \
    "  window.__bz_hook = 1;\n" \
    "  var KEY = '__bz_req';\n" \
    "  try { window.__bz_req = JSON.parse(sessionStorage.getItem(KEY) || '[]'); }\n" \
    "  catch (e) { window.__bz_req = []; }\n" \
    "  if (!Array.isArray(window.__bz_req)) window.__bz_req = [];\n" \
    "\n" \
    "  var save = function(){\n" \
    "    try {\n" \
    "      while (window.__bz_req.length > 300) window.__bz_req.shift();\n" \
    "      sessionStorage.setItem(KEY, JSON.stringify(window.__bz_req));\n" \
    "    } catch (e) {}\n" \
    "  };\n" \
    "  var push = function(o){\n" \
    "    try { window.__bz_req.push(o); save(); } catch (e) {}\n" \
    "  };\n" \
    "\n" \
    "  var XO = XMLHttpRequest.prototype.open,\n" \
    "      XH = XMLHttpRequest.prototype.setRequestHeader,\n" \
    "      XS = XMLHttpRequest.prototype.send;\n" \
    "  XMLHttpRequest.prototype.open = function(m, u){\n" \
    "    this.__m = m; this.__u = u; this.__h = {};\n" \
    "    return XO.apply(this, arguments);\n" \
    "  };\n" \
    "  XMLHttpRequest.prototype.setRequestHeader = function(k, v){\n" \
    "    try { this.__h[k] = v; } catch (e) {}\n" \
    "    return XH.apply(this, arguments);\n" \
    "  };\n" \
    "  XMLHttpRequest.prototype.send = function(b){\n" \
    "    try {\n" \
    "      push({\n" \
    "        kind: 'xhr',\n" \
    "        method: (this.__m || 'GET') + '',\n" \
    "        url: (this.__u || '') + '',\n" \
    "        headers: this.__h || {},\n" \
    "        body: (b === undefined || b === null) ? '' : String(b).slice(0, 500),\n" \
    "        page: location.href,\n" \
    "        t: Date.now()\n" \
    "      });\n" \
    "    } catch (e) {}\n" \
    "    return XS.apply(this, arguments);\n" \
    "  };\n" \
    "\n" \
    "  var OF = window.fetch;\n" \
    "  if (OF) {\n" \
    "    window.fetch = function(input, init){\n" \
    "      try {\n" \
    "        var url = (typeof input === 'string') ? input : ((input && input.url) || '');\n" \
    "        var h = {};\n" \
    "        var hs = (init && init.headers) || (input && input.headers);\n" \
    "        if (hs) {\n" \
    "          if (typeof hs.forEach === 'function') { hs.forEach(function(v, k){ h[k] = v; }); }\n" \
    "          else { Object.keys(hs).forEach(function(k){ h[k] = hs[k]; }); }\n" \
    "        }\n" \
    "        var bd = (init && init.body);\n" \
    "        push({\n" \
    "          kind: 'fetch',\n" \
    "          method: (((init && init.method) || (input && input.method) || 'GET') + ''),\n" \
    "          url: url + '',\n" \
    "          headers: h,\n" \
    "          body: bd ? String(bd).slice(0, 500) : '',\n" \
    "          page: location.href,\n" \
    "          t: Date.now()\n" \
    "        });\n" \
    "      } catch (e) {}\n" \
    "      return OF.apply(this, arguments);\n" \
    "    };\n" \
    "  }\n" \
    "})();\n" \
    "\"\"\"\n" \
    "\n" \
    "\n" \
    "def install_hook(drv):\n" \
    "    \"\"\"装拦截器。CDP 的 addScriptToEvaluateOnNewDocument 管后续导航，\n" \
    "    当前页面再 execute_script 补一次（CDP 只对之后的文档生效）。\"\"\"\n" \
    "    try:\n" \
    "        drv.execute_cdp_cmd(\"Page.addScriptToEvaluateOnNewDocument\",\n" \
    "                            {\"source\": JS_HOOK})\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    try:\n" \
    "        drv.execute_script(JS_HOOK)\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "\n" \
    "\n" \
    "def new_driver(width=1366, height=900):\n" \
    "    from selenium import webdriver\n" \
    "    from selenium.webdriver.chrome.options import Options\n" \
    "    from selenium.webdriver.chrome.service import Service\n" \
    "\n" \
    "    o = Options()\n" \
    "    # 板上走 glibc chroot 里的 chrome：binary_location 要写 chroot 内的路径\n" \
    "    # （宿主上 /opt/chrome/chrome-headless-shell 不存在，不能拿 exists 判断）\n" \
    "    # 判断依据是 cdrv.sh 在不在。本机开发时用它启动系统 chrome。\n" \
    "    cdrv_early = os.environ.get(\"BZ_CDRV\", CDRV)\n" \
    "    if os.path.exists(cdrv_early):\n" \
    "        o.binary_location = CHROME_BIN        # 给 chroot 里的 chromedriver 看\n" \
    "    else:\n" \
    "        o.add_argument(\"--headless=new\")      # 本机：系统 chrome 要显式无头\n" \
    "    o.add_argument(\"--window-size=%d,%d\" % (width, height))\n" \
    "    o.add_argument(\"--hide-scrollbars\")\n" \
    "    # 这块板子上绝对不能传 --user-data-dir：chroot 里 chromedriver 找不到\n" \
    "    # DevToolsActivePort，60 秒后报 unable to discover open pages。\n" \
    "    # 会话因此无法跨进程复用，登录流程必须在一次调用里跑完。\n" \
    "    o.page_load_strategy = \"eager\"\n" \
    "    for a in (\"--no-sandbox\", \"--disable-gpu\", \"--disable-dev-shm-usage\",\n" \
    "              \"--no-first-run\", \"--disable-extensions\", \"--mute-audio\",\n" \
    "              \"--disable-background-networking\", \"--disable-sync\",\n" \
    "              \"--disable-blink-features=AutomationControlled\",\n" \
    "              # 密码管理器会往 autocomplete=\"username\" 的表单里灌值，\n" \
    "              # 关掉它免得和脚本填的内容打架\n" \
    "              \"--disable-features=PasswordManagerOnboarding,\"\n" \
    "              \"AutofillServerCommunication,Translate,OptimizationHints\",\n" \
    "              \"--password-store=basic\",\n" \
    "              \"--user-agent=\" + UA, \"--lang=zh-CN\"):\n" \
    "        o.add_argument(a)\n" \
    "\n" \
    "    ensure_dir(WORKDIR + \"/x\")\n" \
    "    cdrv = os.environ.get(\"BZ_CDRV\", CDRV)\n" \
    "    try:\n" \
    "        if os.path.exists(cdrv):\n" \
    "            svc = Service(executable_path=cdrv, log_output=WORKDIR + \"/chromedriver.log\")\n" \
    "        else:\n" \
    "            # 本机开发：用系统 chromedriver\n" \
    "            svc = Service(executable_path=os.environ.get(\"BZ_DRIVER\",\n" \
    "                          \"/tmp/chromedriver-linux64/chromedriver\"))\n" \
    "    except TypeError:\n" \
    "        svc = Service(executable_path=cdrv)\n" \
    "    try:\n" \
    "        drv = webdriver.Chrome(service=svc, options=o)\n" \
    "    except Exception as e:\n" \
    "        tail = \"\"\n" \
    "        try:\n" \
    "            with open(WORKDIR + \"/chromedriver.log\", \"r\", errors=\"replace\") as f:\n" \
    "                tail = \" | \".join(x.strip() for x in f.readlines()[-12:] if x.strip())\n" \
    "        except OSError:\n" \
    "            pass\n" \
    "        fail(\"浏览器启动失败：%s%s\" % (str(e).splitlines()[0][:200],\n" \
    "                                    (\" || \" + tail[:300]) if tail else \"\"))\n" \
    "    # 浏览器挂了以后 selenium 每次调用会在死连接上重试 ~30 秒\n" \
    "    try:\n" \
    "        drv.command_executor.set_timeout(15)\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    try:\n" \
    "        drv.execute_cdp_cmd(\"Page.addScriptToEvaluateOnNewDocument\",\n" \
    "                            {\"source\": STEALTH_JS})\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    install_hook(drv)      # 请求拦截器：抄下 token 这类 JS 算出来的请求头\n" \
    "    return drv\n" \
    "\n" \
    "\n" \
    "def _probe_deep(drv):\n" \
    "    \"\"\"主文档 + 所有 iframe 都探一遍。\n" \
    "\n" \
    "    为什么需要：登录弹窗经常挂在 iframe 里（阿里系尤其多，闲鱼就是），\n" \
    "    document.querySelectorAll 只看主文档，弹窗里的手机号/密码框一个都看不到 ——\n" \
    "    现象就是\"点了登录、截图里弹窗明明出来了、probe 却说 inputs 只有搜索框\"。\n" \
    "\n" \
    "    返回结构和 JS_PROBE 一样，另外给 iframe 里的元素带上 frame 字段。\n" \
    "    \"\"\"\n" \
    "    out = drv.execute_script(JS_PROBE)\n" \
    "    ins0 = out.get(\"inputs\") or []\n" \
    "    # 主文档就已经够用了（有密码框，或 ≥2 个输入框）→ 别去碰 iframe，\n" \
    "    # 每次 switch_to.frame 都要一次 CDP 往返，轮询下来能拖慢十几秒。\n" \
    "    if any((x.get(\"type\") or \"\").lower() == \"password\" for x in ins0) or len(ins0) >= 2:\n" \
    "        return out\n" \
    "    inputs = list(ins0)\n" \
    "    buttons = list(out.get(\"buttons\") or [])\n" \
    "    hit_frames = []\n" \
    "    try:\n" \
    "        from selenium.webdriver.common.by import By\n" \
    "        n = len(drv.find_elements(By.TAG_NAME, \"iframe\"))\n" \
    "        for i in range(n):\n" \
    "            try:\n" \
    "                frames = drv.find_elements(By.TAG_NAME, \"iframe\")\n" \
    "                if i >= len(frames):\n" \
    "                    break\n" \
    "                # 跨域存储 / 埋点 / 广告的 iframe 不用进去看，纯浪费往返\n" \
    "                try:\n" \
    "                    _src = (frames[i].get_attribute(\"src\") or \"\").lower()\n" \
    "                except Exception:\n" \
    "                    _src = \"\"\n" \
    "                if any(k in _src for k in (\"xdomain-storage\", \"analytics\",\n" \
    "                                           \"googleads\", \"doubleclick\", \"beacon\")):\n" \
    "                    continue\n" \
    "                drv.switch_to.frame(frames[i])\n" \
    "                sub = drv.execute_script(JS_PROBE)\n" \
    "                if sub.get(\"inputs\"):\n" \
    "                    for it in sub[\"inputs\"]:\n" \
    "                        it[\"frame\"] = i\n" \
    "                        it[\"i\"] = len(inputs)\n" \
    "                        inputs.append(it)\n" \
    "                    for b in (sub.get(\"buttons\") or []):\n" \
    "                        b[\"frame\"] = i\n" \
    "                        b[\"i\"] = len(buttons)\n" \
    "                        buttons.append(b)\n" \
    "                    hit_frames.append(i)\n" \
    "                drv.switch_to.default_content()\n" \
    "            except Exception:\n" \
    "                try:\n" \
    "                    drv.switch_to.default_content()\n" \
    "                except Exception:\n" \
    "                    pass\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    out[\"inputs\"] = inputs\n" \
    "    out[\"buttons\"] = buttons\n" \
    "    if hit_frames:\n" \
    "        out[\"frames_with_forms\"] = hit_frames\n" \
    "    return out\n" \
    "\n" \
    "\n" \
    "def do_probe(req):\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    if not url:\n" \
    "        fail(\"缺少 url\")\n" \
    "    drv = new_driver()\n" \
    "    try:\n" \
    "        try:\n" \
    "            drv.get(url)\n" \
    "        except Exception as e:\n" \
    "            fail(\"打开页面失败：%s\" % str(e).splitlines()[0][:200])\n" \
    "        # 轮询等真正的登录框。注意不能拿\"有没有输入框\"当条件：闲鱼首页本身\n" \
    "        # 就有个搜索框，一探就退出了，登录框还没弹出来。\n" \
    "        # 所以判定标准是「有没有密码框」；等不到就点一下页面上的\"登录\"入口\n" \
    "        # （很多站点要点了才弹窗），然后再等。\n" \
    "        # \"登录表单出现了\"的判定。不能只等密码框：闲鱼这类站点默认是\n" \
    "        # 【短信登录】（手机号 + 验证码，压根没有密码框），等密码框必然超时。\n" \
    "        # 所以两条都算：有密码框，或者有 2 个以上输入框（手机号+验证码那种）。\n" \
    "        # 首页单个搜索框不会被误判。\n" \
    "        def _login_form_ready(_i):\n" \
    "            ins = _i.get(\"inputs\") or []\n" \
    "            if any((x.get(\"type\") or \"\").lower() == \"password\" for x in ins):\n" \
    "                return True\n" \
    "            return len(ins) >= 2\n" \
    "\n" \
    "        # 三道时限，保证一定会返回，不会一直挂着：\n" \
    "        #   wait      理想等待（默认 6s），期间探到密码框立刻返回\n" \
    "        #   max_wait  硬上限（默认 10s），到点无条件退出（点\"登录\"跳转也卡不住）\n" \
    "        #   probe 本身每次都有 CDP 层的 command timeout 兜着\n" \
    "        t0 = time.time()\n" \
    "        wait_s = float(req.get(\"wait\", 8.0))\n" \
    "        hard_s = max(wait_s, float(req.get(\"max_wait\", 10.0)))\n" \
    "        info = {}\n" \
    "        clicked_open = False\n" \
    "        timed_out = False\n" \
    "        while True:\n" \
    "            info = _probe_deep(drv)          # 含 iframe：登录弹窗常在 iframe 里\n" \
    "            if _login_form_ready(info):\n" \
    "                break\n" \
    "            now = time.time()\n" \
    "            if now - t0 >= wait_s or now - t0 >= hard_s:\n" \
    "                timed_out = True\n" \
    "                break\n" \
    "            if not clicked_open and req.get(\"open_click\", True) \\\n" \
    "                    and (now - t0) < hard_s - 1.0:      # 留一点余量再点\n" \
    "                # 用文字精确匹配点「登录」入口。不能走 _try_click —— 它按\n" \
    "                # <button> 列表顺序找，闲鱼页面上第一个含\"登录\"的是\n" \
    "                # 「登录 订单」，点了只会跳订单页，登录框根本不弹。\n" \
    "                hit = drv.execute_script(FIND_CLICK_JS, \"登录\")\n" \
    "                if not (hit and hit.get(\"found\")):\n" \
    "                    hit = drv.execute_script(FIND_CLICK_JS, \"立即登录\")\n" \
    "                if hit and hit.get(\"found\"):\n" \
    "                    clicked_open = True\n" \
    "                    # 等 iframe 里的登录框加载，但绝不超过硬上限\n" \
    "                    _left = hard_s - (time.time() - t0) - 0.5\n" \
    "                    time.sleep(min(3.0, max(0.3, _left)))\n" \
    "                    continue\n" \
    "            time.sleep(0.8)\n" \
    "\n" \
    "        # 到点了还没探到 —— 再做一次\"临门重扫\"。\n" \
    "        # 闲鱼的登录弹窗在 passport.goofish.com 的跨域 iframe 里，ARM headless\n" \
    "        # 上实测要 10~12 秒才加载完；硬上限一到就收工的话，用户截图里明明有\n" \
    "        # 输入框，结果却是\"没探到\"。多给 3.5 秒能覆盖绝大多数情况。\n" \
    "        if not _login_form_ready(info):\n" \
    "            try:\n" \
    "                time.sleep(3.0)\n" \
    "                info2 = _probe_deep(drv)\n" \
    "                n1 = len(info.get(\"inputs\") or [])\n" \
    "                n2 = len(info2.get(\"inputs\") or [])\n" \
    "                if _login_form_ready(info2) or n2 > n1:\n" \
    "                    info2[\"late_rescan\"] = True\n" \
    "                    info = info2\n" \
    "            except Exception:\n" \
    "                pass\n" \
    "\n" \
    "        info[\"waited_sec\"] = round(time.time() - t0, 1)\n" \
    "        if timed_out and not _login_form_ready(info):\n" \
    "            _ifs = info.get(\"iframes\") or []\n" \
    "            info[\"hint\"] = (\"等了 %.1f 秒（含 3 秒补扫）没探到输入框。\"\n" \
    "                            \"页面上有 %d 个 iframe%s。可以加大 wait 到 15，\"\n" \
    "                            \"或者改用 hunt 用 flow 明确指定怎么进登录页\"\n" \
    "                            % (info[\"waited_sec\"], len(_ifs),\n" \
    "                               (\"（\" + \"、\".join(\n" \
    "                                   (f.get(\"src\") or \"\")[:40] for f in _ifs[:3]) + \"）\")\n" \
    "                               if _ifs else \"\"))\n" \
    "        # 存一张页面截图：光看 JSON 看不出页面长什么样，尤其要判断\n" \
    "        # \"登录框是不是要点一下才弹出来\"这种问题时，图比字段直观得多。\n" \
    "        try:\n" \
    "            _s = _shot(drv, \"probe\")\n" \
    "            if _s:\n" \
    "                info[\"shot\"] = _s\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "        info[\"ok\"] = True\n" \
    "        info[\"mem_mb\"] = mem_avail_mb()\n" \
    "        info.update(_save_artifacts(\"probe\", req, info, info.get(\"inputs\"), info.get(\"buttons\")))\n" \
    "        sys.stdout.write(json.dumps(info, ensure_ascii=False))\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "def do_login(req):\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    steps = req.get(\"steps\") or []\n" \
    "    if not url:\n" \
    "        fail(\"缺少 url\")\n" \
    "    if not steps:\n" \
    "        fail(\"缺少 steps（先用 probe 探测，再把填表/点击动作排进来）\")\n" \
    "\n" \
    "    drv = new_driver()\n" \
    "    try:\n" \
    "        try:\n" \
    "            drv.get(url)\n" \
    "        except Exception as e:\n" \
    "            fail(\"打开页面失败：%s\" % str(e).splitlines()[0][:200])\n" \
    "        time.sleep(float(req.get(\"wait\", 1.5)))\n" \
    "\n" \
    "        # 每次填/点之前重新探测，页面 DOM 变了元素引用就失效\n" \
    "        drv.execute_script(JS_PROBE)\n" \
    "\n" \
    "        done = []\n" \
    "        for st in steps:\n" \
    "            act = (st.get(\"act\") or \"\").lower()\n" \
    "            if act == \"wait\":\n" \
    "                time.sleep(float(st.get(\"sec\", 2)))\n" \
    "                done.append({\"act\": \"wait\", \"sec\": st.get(\"sec\", 2)})\n" \
    "                continue\n" \
    "            if act == \"goto\":\n" \
    "                drv.get(st.get(\"url\") or url)\n" \
    "                time.sleep(float(st.get(\"sec\", 1.5)))\n" \
    "                drv.execute_script(JS_PROBE)\n" \
    "                done.append({\"act\": \"goto\", \"url\": st.get(\"url\", \"\")})\n" \
    "                continue\n" \
    "            if act == \"fill\":\n" \
    "                ok, got = fill_verified(drv, int(st.get(\"i\", 0)), st.get(\"value\", \"\"))\n" \
    "                done.append({\"act\": \"fill\", \"i\": st.get(\"i\"), \"ok\": bool(ok),\n" \
    "                             \"got\": (got or \"\")[:20]})\n" \
    "            elif act == \"click\":\n" \
    "                kind = \"btn\" if st.get(\"kind\", \"btn\") == \"btn\" else \"in\"\n" \
    "                ok = drv.execute_script(JS_CLICK, {\"i\": int(st.get(\"i\", 0)), \"kind\": kind})\n" \
    "                done.append({\"act\": \"click\", \"i\": st.get(\"i\"), \"ok\": bool(ok)})\n" \
    "                time.sleep(float(st.get(\"sec\", 1.5)))\n" \
    "                drv.execute_script(JS_PROBE)\n" \
    "            elif act == \"enter\":\n" \
    "                from selenium.webdriver.common.keys import Keys\n" \
    "                el = drv.execute_script(\"return window.__bz_in[arguments[0]] || null\",\n" \
    "                                        int(st.get(\"i\", 0)))\n" \
    "                if el:\n" \
    "                    el.send_keys(Keys.ENTER)\n" \
    "                time.sleep(float(st.get(\"sec\", 1.5)))\n" \
    "                drv.execute_script(JS_PROBE)\n" \
    "                done.append({\"act\": \"enter\", \"i\": st.get(\"i\")})\n" \
    "            else:\n" \
    "                done.append({\"act\": act, \"error\": \"未知动作\"})\n" \
    "\n" \
    "        # 等页面稳定（跳转后 cookie 才写进去）\n" \
    "        time.sleep(float(req.get(\"settle\", 2.5)))\n" \
    "        cookies = drv.get_cookies()\n" \
    "        info = drv.execute_script(JS_PROBE)\n" \
    "\n" \
    "        res = {\n" \
    "            \"ok\": True,\n" \
    "            \"url\": drv.current_url,\n" \
    "            \"title\": info.get(\"title\", \"\"),\n" \
    "            \"cookies\": cookies,\n" \
    "            \"cookie_count\": len(cookies),\n" \
    "            \"captcha\": info.get(\"captcha\", False),\n" \
    "            \"steps_done\": done,\n" \
    "            \"mem_mb\": mem_avail_mb(),\n" \
    "        }\n" \
    "        # 还停在登录页通常说明登录没成功（有验证码/密码错/选择器不对）\n" \
    "        if req.get(\"url_should_change\"):\n" \
    "            want = req[\"url_should_change\"]\n" \
    "            if want in res[\"url\"]:\n" \
    "                res[\"warning\"] = \"URL 仍包含 %s，可能没登录成功\" % want\n" \
    "        res.update(_save_artifacts(\"login\", req, res, res.get(\"inputs\"), res.get(\"buttons\")))\n" \
    "        sys.stdout.write(json.dumps(res, ensure_ascii=False))\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "def _guess_fields(info):\n" \
    "    \"\"\"从 probe 结果里猜账号框 / 密码框 / 登录按钮。\n" \
    "    3B 组不出 steps，这一步只能框架代劳。\"\"\"\n" \
    "    user_i = pwd_i = None\n" \
    "    USER_HINT = (\"user\", \"account\", \"phone\", \"mobile\", \"email\", \"login\",\n" \
    "                 \"账号\", \"帐号\", \"手机\", \"邮箱\", \"用户名\", \"帐\")\n" \
    "    for inp in info.get(\"inputs\", []):\n" \
    "        blob = \" \".join([inp.get(\"name\", \"\"), inp.get(\"id\", \"\"), inp.get(\"label\", \"\"),\n" \
    "                         inp.get(\"placeholder\", \"\"), inp.get(\"aria\", \"\"),\n" \
    "                         inp.get(\"autocomplete\", \"\")]).lower()\n" \
    "        if inp.get(\"type\") == \"password\":\n" \
    "            if pwd_i is None:\n" \
    "                pwd_i = inp[\"i\"]\n" \
    "        elif user_i is None and any(k in blob for k in USER_HINT):\n" \
    "            user_i = inp[\"i\"]\n" \
    "    # 兜底：第一个不是密码的文本框当账号（editable = contenteditable 的富文本框）\n" \
    "    if user_i is None:\n" \
    "        for inp in info.get(\"inputs\", []):\n" \
    "            if inp.get(\"type\") in (\"text\", \"email\", \"tel\", \"editable\") and inp[\"i\"] != pwd_i:\n" \
    "                user_i = inp[\"i\"]\n" \
    "                break\n" \
    "\n" \
    "    # ---- 提交按钮 ----\n" \
    "    # 真实站点上这里坑最多，全是实测踩出来的：\n" \
    "    #   京东有个 <a>登录页面，改进建议</a>，含\"登录\"但根本不是按钮\n" \
    "    #   \"密码登录\"/\"短信登录\" 是切换登录方式的 tab，点了只会换表单\n" \
    "    #   \"微信登录\"/\"扫码登录\" 是第三方入口\n" \
    "    # 所以先按\"方式词/干扰词\"排除，再按文字精确度打分。\n" \
    "    BTN_MODE = (\"密码登录\", \"短信登录\", \"扫码登录\", \"账号登录\", \"帐号登录\",\n" \
    "                \"验证码登录\", \"微信登录\", \"qq登录\", \"手机登录\", \"邮箱登录\")\n" \
    "    BTN_BAD = (\"改进\", \"建议\", \"反馈\", \"忘记\", \"注册\", \"找回\", \"帮助\",\n" \
    "               \"协议\", \"隐私\", \"下载\", \"客服\", \"关于\", \"问题\")\n" \
    "    BTN_EXACT = (\"登录\", \"登陆\", \"立即登录\", \"马上登录\", \"确认登录\",\n" \
    "                 \"signin\", \"login\", \"submit\", \"提交\", \"确认\")\n" \
    "\n" \
    "    cands = []\n" \
    "    for b in info.get(\"buttons\", []):\n" \
    "        t = (b.get(\"text\") or \"\").strip()\n" \
    "        tl = t.lower().replace(\" \", \"\").replace(\"\\u3000\", \"\")\n" \
    "        if not tl or len(t) > 12:\n" \
    "            continue\n" \
    "        if any(m in tl for m in BTN_MODE):\n" \
    "            continue                      # 切换 tab，不是提交\n" \
    "        if any(x in tl for x in BTN_BAD):\n" \
    "            continue                      # 干扰链接\n" \
    "        if tl in [e.replace(\" \", \"\") for e in BTN_EXACT]:\n" \
    "            cands.append((100, b[\"i\"]))\n" \
    "        elif any(k in tl for k in (\"登录\", \"登陆\", \"login\", \"signin\", \"提交\", \"确认\")):\n" \
    "            cands.append((50 - len(tl), b[\"i\"]))   # 越短越像真按钮\n" \
    "    cands.sort(reverse=True)\n" \
    "    btn_i = cands[0][1] if cands else None\n" \
    "    if btn_i is None:\n" \
    "        for b in info.get(\"buttons\", []):\n" \
    "            if not b.get(\"tab\"):\n" \
    "                btn_i = b[\"i\"]\n" \
    "                break\n" \
    "    return user_i, pwd_i, btn_i\n" \
    "\n" \
    "\n" \
    "def _find_next_btn(info):\n" \
    "    \"\"\"两步登录的第一步按钮（\"下一步\"/\"继续\"/Next）。Google、企业 SSO\n" \
    "    都这么干：先输账号，点下一步，密码框才渲染出来。\"\"\"\n" \
    "    NEXT = (\"下一步\", \"下步\", \"继续\", \"next\", \"continue\", \"proceed\")\n" \
    "    for b in info.get(\"buttons\", []):\n" \
    "        t = (b.get(\"text\") or \"\").lower().replace(\" \", \"\").replace(\"\\u3000\", \"\")\n" \
    "        for k in NEXT:\n" \
    "            if k in t:\n" \
    "                return b[\"i\"]\n" \
    "    return None\n" \
    "\n" \
    "\n" \
    "def _guess_captcha_field(info, used):\n" \
    "    \"\"\"找验证码输入框：先按 name/id/placeholder 里的关键词，找不到就用\n" \
    "    账号密码之外最后一个文本框（验证码框通常在密码框后面）\"\"\"\n" \
    "    KEY = (\"captcha\", \"code\", \"verify\", \"vcode\", \"check\", \"验证\", \"校验\", \"图形\")\n" \
    "    for inp in info.get(\"inputs\", []):\n" \
    "        if inp[\"i\"] in used:\n" \
    "            continue\n" \
    "        blob = \" \".join([inp.get(\"name\", \"\"), inp.get(\"id\", \"\"),\n" \
    "                         inp.get(\"label\", \"\"), inp.get(\"placeholder\", \"\"),\n" \
    "                         inp.get(\"aria\", \"\")]).lower()\n" \
    "        if any(k in blob for k in KEY):\n" \
    "            return inp[\"i\"]\n" \
    "    for inp in reversed(info.get(\"inputs\", [])):\n" \
    "        if inp[\"i\"] not in used and inp.get(\"type\") in (\"text\", \"tel\", \"number\"):\n" \
    "            return inp[\"i\"]\n" \
    "    return None\n" \
    "\n" \
    "\n" \
    "def _probe_anywhere(drv, wait=0.0):\n" \
    "    \"\"\"先主文档找表单，找不到就钻进 iframe（很多站点把登录框放 iframe 里）。\n" \
    "    wait > 0 时轮询等表单出现 —— 骨架屏 / 懒加载的站点打开后要等一会儿才有输入框。\"\"\"\n" \
    "    deadline = time.time() + wait\n" \
    "    while True:\n" \
    "        info = drv.execute_script(JS_PROBE)\n" \
    "        if info.get(\"inputs\") or time.time() >= deadline:\n" \
    "            break\n" \
    "        time.sleep(0.4)\n" \
    "    if info.get(\"inputs\"):\n" \
    "        return info, None\n" \
    "    for fr in info.get(\"iframes\", []):\n" \
    "        try:\n" \
    "            from selenium.webdriver.common.by import By\n" \
    "            frames = drv.find_elements(By.TAG_NAME, \"iframe\")\n" \
    "            if fr[\"i\"] >= len(frames):\n" \
    "                continue\n" \
    "            drv.switch_to.frame(frames[fr[\"i\"]])\n" \
    "            sub = drv.execute_script(JS_PROBE)\n" \
    "            if sub.get(\"inputs\"):\n" \
    "                sub[\"in_iframe\"] = fr[\"i\"]\n" \
    "                return sub, fr[\"i\"]\n" \
    "            drv.switch_to.default_content()\n" \
    "        except Exception:\n" \
    "            try:\n" \
    "                drv.switch_to.default_content()\n" \
    "            except Exception:\n" \
    "                pass\n" \
    "    return info, None\n" \
    "\n" \
    "\n" \
    "TAB_HINT = (\"账号登录\", \"帐号登录\", \"密码登录\", \"账号密码\", \"用户名登录\",\n" \
    "            \"accountlogin\", \"passwordlogin\", \"账号\", \"密码\")\n" \
    "OPEN_HINT = (\"登录\", \"登陆\", \"注册\", \"signin\", \"sign_in\", \"login\", \"account\")\n" \
    "\n" \
    "\n" \
    "def _try_click(drv, info, hints, tried):\n" \
    "    \"\"\"点第一个文字命中 hints 的元素。tried 记录点过的文字，避免死循环。\"\"\"\n" \
    "    for want_tab in (True, False):\n" \
    "        for b in info.get(\"buttons\", []):\n" \
    "            if bool(b.get(\"tab\")) != want_tab:\n" \
    "                continue\n" \
    "            t = (b.get(\"text\") or \"\").replace(\" \", \"\").lower()\n" \
    "            if not t or t in tried:\n" \
    "                continue\n" \
    "            if any(k in t for k in hints):\n" \
    "                drv.execute_script(JS_CLICK, {\"i\": b[\"i\"], \"kind\": \"btn\"})\n" \
    "                tried.add(t)\n" \
    "                time.sleep(2.0)      # 表单可能是异步渲染的（懒加载组件）\n" \
    "                return b.get(\"text\", \"\")\n" \
    "    return \"\"\n" \
    "\n" \
    "\n" \
    "def _open_form(drv, info, frame_i, max_tries=4):\n" \
    "    \"\"\"反复点开登录入口，直到表单出现。\n" \
    "    真实站点常要两步：先点 header 的\"登录\"打开弹窗，再点\"账号登录\"切 tab。\"\"\"\n" \
    "    tried = set()\n" \
    "    hits = []\n" \
    "    for _ in range(max_tries):\n" \
    "        if info.get(\"inputs\"):\n" \
    "            break\n" \
    "        got = _try_click(drv, info, TAB_HINT, tried)\n" \
    "        if not got:\n" \
    "            got = _try_click(drv, info, OPEN_HINT, tried)\n" \
    "        if not got:\n" \
    "            break\n" \
    "        hits.append(got)\n" \
    "        info, frame_i = _probe_anywhere(drv)\n" \
    "    return info, frame_i, hits\n" \
    "\n" \
    "\n" \
    "def _fill_via_keys(drv, i, value):\n" \
    "    \"\"\"用真实键盘事件填值（ActionChains）。返回实际值，失败返回 None。\n" \
    "\n" \
    "    为什么不用 JS 设 value：知乎这类 React 站点会用 onChange 校验并把\n" \
    "    \"非可信事件\"（dispatchEvent 产生的 isTrusted=false）带来的值同步清掉，\n" \
    "    表现就是填进去立刻变空。真实键盘事件 isTrusted=true，框架才会认。\n" \
    "    \"\"\"\n" \
    "    from selenium.webdriver.common.action_chains import ActionChains\n" \
    "    try:\n" \
    "        el = drv.execute_script(\"return window.__bz_in[arguments[0]];\", i)\n" \
    "        if el is None:\n" \
    "            return None\n" \
    "        # 先清空。不能用 el.clear()：在 glibc chroot 的 chromedriver 上会段错误。\n" \
    "        drv.execute_script(\n" \
    "            \"var e = window.__bz_in[arguments[0]]; if (!e) return;\"\n" \
    "            \"if (e.isContentEditable) e.textContent = ''; else e.value = '';\"\n" \
    "            \"try { e.dispatchEvent(new Event('input', {bubbles: true})); } catch (err) {}\", i)\n" \
    "        ActionChains(drv).send_keys_to_element(el, value).perform()\n" \
    "        time.sleep(0.12)\n" \
    "        got = el.get_attribute(\"value\")\n" \
    "        if got is None:\n" \
    "            got = drv.execute_script(\n" \
    "                \"var e = window.__bz_in[arguments[0]]; if (!e) return null;\"\n" \
    "                \"return e.isContentEditable ? (e.innerText || '').trim() : e.value;\", i)\n" \
    "        return got\n" \
    "    except Exception:\n" \
    "        return None\n" \
    "\n" \
    "\n" \
    "def fill_verified(drv, i, value, tries=3):\n" \
    "    \"\"\"填值并读回校验。顺序：真实键盘 → JS 设值。\n" \
    "\n" \
    "    真实站点上的三类坑：\n" \
    "      1. React/Vue 受控组件不认 dispatchEvent 的非可信事件（知乎会直接清空）\n" \
    "      2. contenteditable 没有 .value，读回要走 innerText\n" \
    "      3. 浏览器自动填充/输入法可能把值改掉，所以必须读回确认\n" \
    "    \"\"\"\n" \
    "    got = None\n" \
    "    for _ in range(tries):\n" \
    "        # 1) 真实键盘事件（最可靠）\n" \
    "        got = _fill_via_keys(drv, i, value)\n" \
    "        if got == value:\n" \
    "            return True, got\n" \
    "\n" \
    "        # 2) 退回 JS 设值（contenteditable、或 send_keys 进来不了的情况）\n" \
    "        r = drv.execute_script(JS_FILL, {\"i\": i, \"value\": value})\n" \
    "        time.sleep(0.12)\n" \
    "        if isinstance(r, dict) and r.get(\"val\") is not None:\n" \
    "            got = r[\"val\"]\n" \
    "        else:\n" \
    "            got = drv.execute_script(\n" \
    "                \"var e = window.__bz_in[arguments[0]]; if (!e) return null;\"\n" \
    "                \"if (e.isContentEditable || e.getAttribute('contenteditable') !== null)\"\n" \
    "                \"  return (e.innerText || '').trim();\"\n" \
    "                \"return e.value;\", i)\n" \
    "        if got == value:\n" \
    "            return True, got\n" \
    "        time.sleep(0.1)\n" \
    "    return False, got\n" \
    "\n" \
    "\n" \
    "def cookies_to_header(cookies):\n" \
    "    \"\"\"转成 curl 的 Cookie 头，可以直接喂给 curl 插件\"\"\"\n" \
    "    return \"; \".join(\"%s=%s\" % (c.get(\"name\"), c.get(\"value\"))\n" \
    "                     for c in cookies if c.get(\"name\"))\n" \
    "\n" \
    "\n" \
    "def judge_login(before_url, after_url, cookies, info, storage=None):\n" \
    "    \"\"\"判断这次登录到底成没成。\n" \
    "\n" \
    "    真实站点上没有单一可靠信号，踩过的坑：\n" \
    "      - GitHub 未登录也发 _gh_sess，按 cookie 名判断会误判成功\n" \
    "      - 很多站登录失败也跳 URL（302 回登录页），光看 url_changed 没用\n" \
    "      - SPA 登录成功 URL 完全不变，光看 url_changed 也没用\n" \
    "\n" \
    "    所以按可靠性从强到弱：\n" \
    "      1) 页面上还摆着密码输入框、而且 URL/标题还是登录页 → 铁定没进去\n" \
    "         （成功登录后登录表单必然消失，这条几乎不会错）\n" \
    "      2) 出现\"退出/注销/我的\"这类登录后才有的入口 → 基本成功\n" \
    "      3) 没有密码框了 + 拿到 auth 类 cookie/token → 可能成功\n" \
    "    \"\"\"\n" \
    "    inputs = info.get(\"inputs\") or []\n" \
    "    still_pwd = any((i.get(\"type\") or \"\").lower() == \"password\" for i in inputs)\n" \
    "\n" \
    "    # 只看\"现在\"在哪儿，不能把 before_url 拼进来 ——\n" \
    "    # 登录成功后 before_url 仍然含 login，一拼就永远判\"还在登录页\"\n" \
    "    urls = \" \".join([(info.get(\"url\") or \"\"), (after_url or \"\")]).lower()\n" \
    "    LOGIN_PATH = (\"login\", \"signin\", \"sign-in\", \"passport\", \"sso\", \"sign_in\", \"auth/\")\n" \
    "    on_login_path = any(k in urls for k in LOGIN_PATH)\n" \
    "\n" \
    "    title = (info.get(\"title\") or \"\").lower()\n" \
    "    # 标题含\"登录\"不等于还在登录页 —— \"登录成功\"、\"欢迎登录\" 都含这两个字。\n" \
    "    # 必须先把\"成功/欢迎/首页\"这类排除掉，否则登录成功会被判成没登进去。\n" \
    "    TITLE_LOGIN = (\"登录\", \"登陆\", \"sign in\", \"signin\", \"log in\", \"login\", \"passport\")\n" \
    "    TITLE_DONE = (\"成功\", \"欢迎\", \"首页\", \"工作台\", \"主页\", \"dashboard\", \"home\",\n" \
    "                  \"welcome\", \"已登录\", \"signed in\", \"logged in\")\n" \
    "    title_login = (any(k in title for k in TITLE_LOGIN)\n" \
    "                   and not any(k in title for k in TITLE_DONE))\n" \
    "\n" \
    "    buttons = info.get(\"buttons\") or []\n" \
    "    has_logout = any(\n" \
    "        k in (b.get(\"text\") or \"\").lower()\n" \
    "        for b in buttons\n" \
    "        for k in (\"退出\", \"注销\", \"登出\", \"退出登录\", \"logout\", \"sign out\", \"signout\", \"log out\"))\n" \
    "\n" \
    "    # 只认明确表示\"已登录\"的 cookie 名。\n" \
    "    # 注意别把 xx_sess / SESSION 算进来 —— 那是匿名会话，未登录就有。\n" \
    "    AUTH = (\"user_session\", \"auth_token\", \"access_token\", \"refresh_token\",\n" \
    "            \"jwt\", \"ticket\", \"sessionid\", \"session_id\", \"remember_token\",\n" \
    "            \"logged_in\", \"is_login\", \"sid\", \"uid\", \"token\")\n" \
    "    # 光看名字不够：GitHub 未登录也发 logged_in=no，名字对了值照样骗人。\n" \
    "    # 必须连值一起看，值是 no/false/0 这类的一律不算。\n" \
    "    BAD_VAL = (\"\", \"no\", \"false\", \"0\", \"null\", \"none\", \"deleted\", \"guest\",\n" \
    "               \"anonymous\", \"-1\", \"undefined\")\n" \
    "    cookie_auth = False\n" \
    "    for c in cookies:\n" \
    "        n = (c.get(\"name\") or \"\").lower()\n" \
    "        v = ((c.get(\"value\") or \"\") + \"\").strip().lower()\n" \
    "        if not any(k in n for k in AUTH):\n" \
    "            continue\n" \
    "        if v in BAD_VAL:\n" \
    "            continue\n" \
    "        cookie_auth = True\n" \
    "        break\n" \
    "\n" \
    "    blob = \"\"\n" \
    "    if storage:\n" \
    "        blob = \" \".join(list(storage.get(\"local\", {}).keys())\n" \
    "                        + list(storage.get(\"session\", {}).keys())).lower()\n" \
    "    token_key = any(k in blob for k in (\"token\", \"auth\", \"jwt\", \"ticket\", \"user_id\", \"uid\"))\n" \
    "\n" \
    "    reasons = []\n" \
    "    if still_pwd:\n" \
    "        reasons.append(\"页面上还有密码输入框\")\n" \
    "    if on_login_path:\n" \
    "        reasons.append(\"URL 还在登录相关路径\")\n" \
    "    if title_login:\n" \
    "        reasons.append(\"页面标题还是登录页\")\n" \
    "\n" \
    "    # ---- 按可靠性判定 ----\n" \
    "    if still_pwd and (on_login_path or title_login):\n" \
    "        # 密码框还在、页面也还是登录页：不管有没有 cookie，肯定没进去\n" \
    "        likely, score = False, 0\n" \
    "    elif has_logout:\n" \
    "        likely, score = True, 4\n" \
    "    elif (not still_pwd) and (cookie_auth or token_key):\n" \
    "        likely, score = True, 3\n" \
    "    else:\n" \
    "        likely, score = False, (1 if (cookie_auth or token_key) else 0)\n" \
    "\n" \
    "    return {\n" \
    "        \"score\": score,\n" \
    "        \"likely_ok\": likely,\n" \
    "        \"still_has_password\": still_pwd,\n" \
    "        \"on_login_page\": on_login_path,\n" \
    "        \"left_login_page\": not on_login_path,      # 老字段，保留兼容\n" \
    "        \"title_is_login\": title_login,\n" \
    "        \"cookie_has_session\": cookie_auth,         # 只算 auth 类，不算匿名会话\n" \
    "        \"storage_has_token\": token_key,\n" \
    "        \"logged_in_ui\": has_logout,\n" \
    "        \"url_changed\": before_url != after_url,\n" \
    "        \"reasons\": reasons,\n" \
    "    }\n" \
    "\n" \
    "\n" \
    "SHOT_DIR = \"/tmp/llama_agent/shots\"     # 统一放在 WORK_DIR 下，?file= 才找得到\n" \
    "\n" \
    "\n" \
    "def _shot(drv, tag):\n" \
    "    \"\"\"截当前页面存文件，返回路径。失败场景靠它给人工看现场。\"\"\"\n" \
    "    if drv is None:\n" \
    "        return None\n" \
    "    try:\n" \
    "        os.makedirs(SHOT_DIR, exist_ok=True)\n" \
    "        p = os.path.join(SHOT_DIR, \"%s_%s.png\" % (tag, time.strftime(\"%H%M%S\")))\n" \
    "        drv.save_screenshot(p)\n" \
    "        return p if os.path.getsize(p) > 0 else None\n" \
    "    except Exception:\n" \
    "        return None\n" \
    "\n" \
    "\n" \
    "PWD_ERR_PAT = (\n" \
    "    \"密码错误\", \"密码不正确\", \"密码有误\", \"账号或密码\", \"帐号或密码\",\n" \
    "    \"用户名或密码\", \"账号密码\", \"帐号密码\",\n" \
    "    \"账号不存在\", \"帐号不存在\", \"用户不存在\", \"用户名不存在\", \"账号未注册\",\n" \
    "    \"登录失败\", \"登陆失败\", \"认证失败\", \"身份验证失败\",\n" \
    "    \"账号已锁定\", \"账号被锁定\", \"帐号已锁定\", \"账号冻结\", \"账号被冻结\",\n" \
    "    \"尝试次数过多\", \"错误次数过多\", \"请稍后再试\", \"账号异常\",\n" \
    "    \"incorrect password\", \"invalid password\", \"wrong password\",\n" \
    "    \"incorrect username or password\", \"incorrect username\",\n" \
    "    \"invalid username or password\", \"username or password\",\n" \
    "    \"login failed\", \"log in failed\", \"authentication failed\",\n" \
    "    \"invalid credentials\", \"bad credentials\",\n" \
    "    \"account does not exist\", \"user not found\", \"no such user\",\n" \
    "    \"account locked\", \"account is locked\", \"too many attempts\", \"too many failed\",\n" \
    ")\n" \
    "\n" \
    "CAPTCHA_ERR_PAT = (\n" \
    "    \"验证码错误\", \"验证码不正确\", \"验证码有误\", \"校验码错误\", \"图形码错误\",\n" \
    "    \"验证码已过期\", \"验证码失效\", \"请重新输入验证码\", \"验证码输入错误\",\n" \
    "    \"captcha is invalid\", \"invalid captcha\", \"wrong captcha\", \"captcha error\",\n" \
    "    \"verification code is invalid\", \"invalid verification code\",\n" \
    ")\n" \
    "\n" \
    "\n" \
    "def _page_text(drv, limit=4000):\n" \
    "    \"\"\"整个页面的可见文字，用来找失败提示。\"\"\"\n" \
    "    try:\n" \
    "        return ((drv.execute_script(\n" \
    "            \"return (document.body && document.body.innerText) || '';\") or \"\")[:limit])\n" \
    "    except Exception:\n" \
    "        return \"\"\n" \
    "\n" \
    "\n" \
    "def _classify_failure(drv, res):\n" \
    "    \"\"\"这次失败属于哪一类 —— 决定能不能重试。\n" \
    "\n" \
    "    这是账号安全的关键：很多站点密码错 5 次就锁号/锁 IP。拿凭据反复试探\n" \
    "    是最危险的做法，宁可少试、失败就报，也不能把账号试废。\n" \
    "\n" \
    "    返回：\n" \
    "      credentials  账号或密码错误 → 绝对不能重试\n" \
    "      captcha      验证码错误     → 可以重试（换一张验证码）\n" \
    "      captcha_unreadable 验证码认不出来（还没提交过） → 可以重试\n" \
    "      recaptcha    人机验证       → 重试无用\n" \
    "      npu          NPU/内存问题   → 重试无用\n" \
    "      structure    页面结构找不到 → 重试无用（下次还是一样）\n" \
    "      unknown      判不出来       → 保守：不重试\n" \
    "    \"\"\"\n" \
    "    if res.get(\"npu_unavailable\"):\n" \
    "        return \"npu\"\n" \
    "    if (res.get(\"captcha_kind\") or \"\").lower() in (\"recaptcha\", \"hcaptcha\", \"slider\"):\n" \
    "        return \"recaptcha\"\n" \
    "    if res.get(\"captcha_failed\") and not res.get(\"verdict\"):\n" \
    "        # 连识别都没做成就返回了，说明是\"页面上的验证码认不出来\"，还没提交过凭据\n" \
    "        return \"captcha_unreadable\"\n" \
    "\n" \
    "    err = ((res.get(\"error\") or \"\") + \" \" + (res.get(\"warning\") or \"\"))\n" \
    "    if \"没找到账号框\" in err or \"没找到密码框\" in err or \"没找到验证码图片\" in err:\n" \
    "        return \"structure\"\n" \
    "\n" \
    "    # 优先用 _login_once 当场采下来的页面文字：错误提示常常几秒就消失，\n" \
    "    # 等这里再读就读不到了（恩山论坛的提示就是闪一下就没了）\n" \
    "    txt = ((res.get(\"page_text\") or _page_text(drv)) + \"\").lower()\n" \
    "    res.pop(\"page_text\", None)          # 只是内部判据用的，别带进输出\n" \
    "\n" \
    "    # 先看账号/密码类 —— 优先级最高，命中就绝不再试\n" \
    "    for k in PWD_ERR_PAT:\n" \
    "        if k in txt:\n" \
    "            return \"credentials\"\n" \
    "\n" \
    "    for k in CAPTCHA_ERR_PAT:\n" \
    "        if k in txt:\n" \
    "            return \"captcha\"\n" \
    "\n" \
    "    # 判不出来：保守当不能重试\n" \
    "    return \"unknown\"\n" \
    "\n" \
    "\n" \
    "def _login_once(drv, url, user, pwd, req, attempt):\n" \
    "    \"\"\"跑一次完整登录，返回 dict（不退出进程，失败也让调用方决定）。\n" \
    "    attempt 只用来把每轮的验证码截图分开放。\"\"\"\n" \
    "    try:\n" \
    "        drv.get(url)\n" \
    "    except Exception as e:\n" \
    "        return {\"ok\": False, \"error\": \"打开页面失败：%s\" % str(e).splitlines()[0][:180]}\n" \
    "    time.sleep(float(req.get(\"wait\", 1.5)))\n" \
    "    before_url = drv.current_url\n" \
    "\n" \
    "    info, frame_i = _probe_anywhere(drv, wait=float(req.get(\"form_wait\", 4.0)))\n" \
    "    # 表单没出现 → 可能要先把登录入口点开、再切 tab（最多试 4 步）\n" \
    "    info, frame_i, tabbed = _open_form(drv, info, frame_i)\n" \
    "\n" \
    "    # 人机验证（reCAPTCHA / hCaptcha / 滑块）：原理上没法自动过，直接说清楚。\n" \
    "    # 必须放在跑 OCR 之前 —— 不然先白起一遍识别再报\"识别不出来\"，误导人。\n" \
    "    kind = (info.get(\"captcha_kind\") or \"\").lower()\n" \
    "    if kind in (\"recaptcha\", \"hcaptcha\", \"slider\"):\n" \
    "        return {\n" \
    "            \"ok\": False, \"captcha_failed\": True, \"captcha_kind\": kind,\n" \
    "            \"error\": (\"这个站点要过 %s 人机验证，自动化过不去，得人工点一下\"\n" \
    "                      % {\"recaptcha\": \"reCAPTCHA\", \"hcaptcha\": \"hCaptcha\",\n" \
    "                         \"slider\": \"滑块\"}.get(kind, kind)),\n" \
    "            \"inputs\": info.get(\"inputs\", []),\n" \
    "        }\n" \
    "\n" \
    "    cap_text = \"\"\n" \
    "    cap_info = None\n" \
    "    cap_low = False\n" \
    "    cap_png = \"\"\n" \
    "    if info.get(\"captcha\") and req.get(\"ocr\", True) and not req.get(\"force\"):\n" \
    "        cap_info = _solve_captcha(\n" \
    "            drv, SHOT_DIR, int(req.get(\"cap_retry\", 2)), \"t%d_\" % attempt)\n" \
    "        if cap_info and cap_info.get(\"text\"):\n" \
    "            cap_text = cap_info[\"text\"]\n" \
    "            cap_low = cap_info.get(\"low_confidence\", False)\n" \
    "            # 识别用的那张图要带出去：验证码一提交就换新的，事后只剩这张能证明\n" \
    "            # \"OCR 当时看到的是什么\"，人工才好判断是认错了还是站点判错了\n" \
    "            pngs = cap_info.get(\"pngs\") or []\n" \
    "            cap_png = pngs[-1] if pngs else \"\"\n" \
    "            # 点过刷新按钮，DOM 可能重建 → 重新探测\n" \
    "            info, frame_i = _probe_anywhere(drv)\n" \
    "        else:\n" \
    "            if cap_info and cap_info.get(\"npu_unavailable\"):\n" \
    "                # 环境问题：NPU 分不到内存 / ppocr 没装。重试也没用，直接报清楚\n" \
    "                return {\n" \
    "                    \"ok\": False,\n" \
    "                    \"captcha_failed\": True,\n" \
    "                    \"npu_unavailable\": True,\n" \
    "                    \"error\": cap_info.get(\"error\") or \"NPU 不可用\",\n" \
    "                    \"pngs\": cap_info.get(\"pngs\", []),\n" \
    "                }\n" \
    "            return {\n" \
    "                \"ok\": False,\n" \
    "                \"captcha_failed\": True,\n" \
    "                \"error\": \"验证码识别不出来\",\n" \
    "                \"pngs\": (cap_info or {}).get(\"pngs\", []),\n" \
    "                \"attempts\": (cap_info or {}).get(\"attempts\", []),\n" \
    "            }\n" \
    "\n" \
    "    user_i, pwd_i, btn_i = _guess_fields(info)\n" \
    "\n" \
    "    # 默认是短信/扫码登录的站点（知乎、很多国内站就是这么设计的）：\n" \
    "    # 页面上有手机号框、但压根没有密码框。这种情况先点\"密码登录\"切过去，\n" \
    "    # 别直接判\"没找到密码框\"就放弃。\n" \
    "    if user_i is not None and pwd_i is None:\n" \
    "        switched = _try_click(drv, info,\n" \
    "                              (\"密码登录\", \"账号密码\", \"帐号密码\", \"用密码登录\",\n" \
    "                               \"密码登陆\", \"password\"), set())\n" \
    "        if switched:\n" \
    "            time.sleep(1.0)\n" \
    "            info2, frame_i2 = _probe_anywhere(drv, wait=2.0)\n" \
    "            u2, p2, b2 = _guess_fields(info2)\n" \
    "            if p2 is not None:\n" \
    "                info, user_i, pwd_i, btn_i = info2, u2, p2, b2\n" \
    "                if frame_i2 is not None:\n" \
    "                    frame_i = frame_i2\n" \
    "\n" \
    "    # 两步登录：先填账号 → 点「下一步」→ 密码框这时才渲染出来。\n" \
    "    # 直接按\"没找到密码框\"失败会把 Google / 企业 SSO 这类站点全判死。\n" \
    "    if user_i is not None and pwd_i is None:\n" \
    "        fill_verified(drv, user_i, user)\n" \
    "        time.sleep(0.2)\n" \
    "        nxt = _find_next_btn(info)\n" \
    "        if nxt is not None:\n" \
    "            drv.execute_script(JS_CLICK, {\"i\": nxt, \"kind\": \"btn\"})\n" \
    "            for _ in range(15):                 # 最多等 6 秒\n" \
    "                time.sleep(0.4)\n" \
    "                info2, frame_i = _probe_anywhere(drv)\n" \
    "                u2, p2, b2 = _guess_fields(info2)\n" \
    "                if p2 is not None:\n" \
    "                    info, user_i, pwd_i, btn_i = info2, u2, p2, b2\n" \
    "                    break\n" \
    "\n" \
    "    if user_i is None or pwd_i is None:\n" \
    "        return {\"ok\": False, \"error\": \"没找到账号框或密码框\",\n" \
    "                \"inputs\": info.get(\"inputs\", []),\n" \
    "                \"buttons\": info.get(\"buttons\", [])}\n" \
    "\n" \
    "    ok_u, _ = fill_verified(drv, user_i, user)\n" \
    "    time.sleep(0.15)\n" \
    "    ok_p, _ = fill_verified(drv, pwd_i, pwd)\n" \
    "    time.sleep(0.15)\n" \
    "\n" \
    "    cap_ok = None\n" \
    "    cap_i = None\n" \
    "    if cap_text:\n" \
    "        cap_i = _guess_captcha_field(info, {user_i, pwd_i})\n" \
    "        if cap_i is None:\n" \
    "            return {\"ok\": False, \"error\": \"识别出验证码「%s」但找不到输入框\" % cap_text,\n" \
    "                    \"pngs\": (cap_info or {}).get(\"pngs\", []),\n" \
    "                    \"inputs\": info.get(\"inputs\", [])}\n" \
    "        cap_ok, _ = fill_verified(drv, cap_i, cap_text)\n" \
    "        time.sleep(0.15)\n" \
    "\n" \
    "    # 提交前截一张：这是唯一能证明\"我到底填了什么、当时的验证码长什么样\"的证据。\n" \
    "    # 提交后验证码必然换新（服务端把旧的消费掉了），再截就只剩新图 + 错误提示。\n" \
    "    pre_shot = _shot(drv, \"before%d\" % attempt) if cap_text else \"\"\n" \
    "\n" \
    "    clicked = drv.execute_script(JS_CLICK, {\"i\": btn_i if btn_i is not None else 0,\n" \
    "                                            \"kind\": \"btn\"})\n" \
    "    time.sleep(float(req.get(\"settle\", 2.5)))\n" \
    "    post_shot = _shot(drv, \"after%d\" % attempt)\n" \
    "\n" \
    "    try:\n" \
    "        drv.switch_to.default_content()\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    # 探测最终页面状态。失败常常是一次 302 回到登录页、页面要重新渲染，\n" \
    "    # 抢在重定向中间 probe 会拿到空表单 —— 实测 GitHub 就这样，判据因此\n" \
    "    # 误以为\"密码框消失了\"，把登录失败判成了成功。所以拿不到输入框就再等再探。\n" \
    "    after_info = {}\n" \
    "    for _w in range(3):\n" \
    "        try:\n" \
    "            after_info = drv.execute_script(JS_PROBE)\n" \
    "        except Exception:\n" \
    "            after_info = {}\n" \
    "        if after_info.get(\"inputs\"):\n" \
    "            break\n" \
    "        if _w < 2:\n" \
    "            time.sleep(0.9)\n" \
    "\n" \
    "    cookies = drv.get_cookies()\n" \
    "    dom = req.get(\"domain\") or \"\"\n" \
    "    if dom:\n" \
    "        cookies = [c for c in cookies if dom in (c.get(\"domain\") or \"\")]\n" \
    "    try:\n" \
    "        storage = drv.execute_script(JS_STORAGE)\n" \
    "    except Exception:\n" \
    "        storage = {}\n" \
    "\n" \
    "    # 趁错误提示还在，先把页面文字采下来。\n" \
    "    # 很多站点的\"密码错误\"提示几秒后自动消失（恩山就是这样），\n" \
    "    # 等外层判失败原因时再读就读不到了。\n" \
    "    page_text_snapshot = _page_text(drv, 3000)\n" \
    "\n" \
    "    verdict = judge_login(before_url, drv.current_url, cookies, after_info, storage)\n" \
    "\n" \
    "    res = {\n" \
    "        \"ok\": True,\n" \
    "        \"url\": drv.current_url,\n" \
    "        \"title\": after_info.get(\"title\") or (drv.title if hasattr(drv, \"title\") else \"\"),\n" \
    "        \"page_text\": page_text_snapshot,\n" \
    "        \"cookies\": cookies,\n" \
    "        \"cookie_count\": len(cookies),\n" \
    "        \"cookie_header\": cookies_to_header(cookies),\n" \
    "        \"storage\": storage,\n" \
    "        \"verdict\": verdict,\n" \
    "        \"filled\": {\"user_i\": user_i, \"pass_i\": pwd_i, \"btn_i\": btn_i,\n" \
    "                   \"clicked\": bool(clicked), \"in_iframe\": frame_i,\n" \
    "                   \"tab_clicked\": \" + \".join(tabbed),\n" \
    "                   \"user_ok\": ok_u, \"pass_ok\": ok_p,\n" \
    "                   \"captcha_i\": cap_i, \"captcha_ok\": cap_ok,\n" \
    "                   \"captcha_text\": cap_text, \"captcha_png\": cap_png},\n" \
    "        \"shot\": post_shot,\n" \
    "        \"shot_before\": pre_shot,\n" \
    "        \"shot_after\": post_shot,\n" \
    "        \"mem_mb\": mem_avail_mb(),\n" \
    "    }\n" \
    "    if after_info.get(\"buttons\"):\n" \
    "        res[\"buttons\"] = after_info[\"buttons\"][:12]\n" \
    "        if after_info.get(\"inputs\"):\n" \
    "            res[\"inputs\"] = after_info[\"inputs\"][:12]\n" \
    "\n" \
    "    if not verdict[\"likely_ok\"]:\n" \
    "        # 直接用判据算出来的原因，别再自己拼一套（两边会不一致）\n" \
    "        reasons = list(verdict.get(\"reasons\") or [])\n" \
    "        if cap_text:\n" \
    "            reasons.append(\"验证码填的是「%s」%s\" % (\n" \
    "                cap_text, \"（置信度低，很可能认错了）\" if cap_low else \"\"))\n" \
    "        if not reasons:\n" \
    "            reasons.append(\"页面既没有登录后的入口，也没拿到鉴权 cookie\")\n" \
    "        res[\"warning\"] = \"这次没登进去（判据 %d/4）\" % verdict[\"score\"]\n" \
    "        res[\"diagnosis\"] = {\"reasons\": reasons}\n" \
    "    return res\n" \
    "\n" \
    "\n" \
    "def do_autologin(req):\n" \
    "    \"\"\"登录主流程：失败就让插件自己重新识别验证码再来一次。\n" \
    "    每轮都重新加载页面（验证码会跟着刷新），最多 tries 轮。\"\"\"\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    user = req.get(\"user\") or req.get(\"username\") or \"\"\n" \
    "    pwd = req.get(\"pass\") or req.get(\"password\") or \"\"\n" \
    "    if not url:\n" \
    "        fail(\"缺少 url\")\n" \
    "    if not user or not pwd:\n" \
    "        fail(\"缺少账号或密码（user / pass 字段）\")\n" \
    "    tries = max(1, int(req.get(\"tries\", 3)))\n" \
    "\n" \
    "    history = []\n" \
    "    last = None\n" \
    "    drv = new_driver()\n" \
    "    try:\n" \
    "        for attempt in range(tries):\n" \
    "            r = _login_once(drv, url, user, pwd, req, attempt)\n" \
    "            last = r\n" \
    "            v = r.get(\"verdict\") or {}\n" \
    "            f = r.get(\"filled\") or {}\n" \
    "            ok_now = bool(r.get(\"ok\") and v.get(\"likely_ok\"))\n" \
    "\n" \
    "            # 判失败原因：决定还能不能再来一次\n" \
    "            kind = \"\" if ok_now else _classify_failure(drv, r)\n" \
    "            history.append({\n" \
    "                \"attempt\": attempt + 1,\n" \
    "                \"captcha\": f.get(\"captcha_text\", \"\"),\n" \
    "                \"score\": v.get(\"score\", 0),\n" \
    "                \"logged_in\": ok_now,\n" \
    "                \"failure_kind\": kind,\n" \
    "                \"error\": (r.get(\"error\") or \"\")[:60],\n" \
    "            })\n" \
    "            if ok_now:\n" \
    "                r[\"tries_used\"] = attempt + 1\n" \
    "                r[\"history\"] = history\n" \
    "                r.pop(\"page_text\", None)      # 内部判据用的，别带进输出\n" \
    "                r.update(_save_artifacts(\"autologin\", req, r, r.get(\"inputs\"), r.get(\"buttons\")))\n" \
    "                sys.stdout.write(json.dumps(r, ensure_ascii=False))\n" \
    "                sys.stdout.flush()\n" \
    "                return\n" \
    "\n" \
    "            # 没成功 → 先截现场图（页面停在这一步的样子）\n" \
    "            s = _shot(drv, \"try%d\" % (attempt + 1))\n" \
    "            if s:\n" \
    "                r[\"shot\"] = s\n" \
    "                history[-1][\"shot\"] = s\n" \
    "\n" \
    "            r[\"failure_kind\"] = kind\n" \
    "            r[\"tries_used\"] = attempt + 1\n" \
    "            r[\"history\"] = history\n" \
    "\n" \
    "            # ===== 关键：只有\"验证码认错\"允许重试 =====\n" \
    "            # 账号密码错绝对不能重试 —— 很多站点错 5 次就锁号、锁 IP，\n" \
    "            # 拿凭据反复试探是最危险的做法。其它类型重试也没意义。\n" \
    "            if kind not in (\"captcha\", \"captcha_unreadable\"):\n" \
    "                if kind == \"credentials\":\n" \
    "                    r[\"error\"] = (\"账号或密码错误 —— 已立即停止，不再重试。\"\n" \
    "                                  \"（站点通常错几次就锁号/锁 IP，不能拿凭据试探）\")\n" \
    "                    r[\"stopped_for_safety\"] = True\n" \
    "                elif kind == \"recaptcha\":\n" \
    "                    r[\"error\"] = r.get(\"error\") or \"站点要求人机验证，重试无用\"\n" \
    "                elif kind == \"npu\":\n" \
    "                    r[\"error\"] = r.get(\"error\") or \"NPU/内存不足，重试无用\"\n" \
    "                    r[\"hint\"] = \"跑 /root/freemm.sh 重启设备回收 NPU 内存后再试\"\n" \
    "                elif kind == \"structure\":\n" \
    "                    r[\"error\"] = r.get(\"error\") or \"页面结构对不上，重试无用\"\n" \
    "                else:\n" \
    "                    r[\"error\"] = (r.get(\"error\") or \"登录没通过\") + \\\n" \
    "                                 \"（判断不出具体原因，为安全起见不再重试）\"\n" \
    "                r[\"ok\"] = False\n" \
    "                r.update(_save_artifacts(\"autologin\", req, r, r.get(\"inputs\"), r.get(\"buttons\")))\n" \
    "                sys.stdout.write(json.dumps(r, ensure_ascii=False))\n" \
    "                sys.stdout.flush()\n" \
    "                return\n" \
    "\n" \
    "            # 验证码类：换一张再试（不算拿凭据试探，因为还没提交成功过）\n" \
    "            if attempt + 1 < tries:\n" \
    "                time.sleep(float(req.get(\"retry_wait\", 1.5)))\n" \
    "\n" \
    "        # 全部轮次用完还是没进去\n" \
    "        out = dict(last or {\"ok\": False})\n" \
    "        out[\"ok\"] = False\n" \
    "        out[\"tries_used\"] = tries\n" \
    "        out[\"history\"] = history\n" \
    "        if not out.get(\"error\"):\n" \
    "            out[\"error\"] = out.get(\"warning\") or (\"试了 %d 轮都没登进去\" % tries)\n" \
    "        out.pop(\"warning\", None)\n" \
    "        captcha_seen = any(h.get(\"captcha\") for h in history)\n" \
    "        if captcha_seen:\n" \
    "            out[\"next_steps\"] = [\n" \
    "                \"每轮都重新识别了验证码，还是没过 —— 干扰线太强的验证码 ppocr 也认不了\",\n" \
    "                \"把 pngs 里的图捞出来人工读，再用 op=login 的 steps 填\",\n" \
    "                \"或者 ocr=0 关掉自动识别，全程自己用 steps 控制\",\n" \
    "            ]\n" \
    "        out.update(_save_artifacts(\"autologin\", req, out, out.get(\"inputs\"), out.get(\"buttons\")))\n" \
    "        sys.stdout.write(json.dumps(out, ensure_ascii=False))\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "\n" \
    "def _cma_free_kb():\n" \
    "    \"\"\"CMA（连续内存）剩余，单位 KB。NPU 的 RKNN 模型要从这里分 DMA 内存。\"\"\"\n" \
    "    try:\n" \
    "        with open(\"/proc/meminfo\") as f:\n" \
    "            for line in f:\n" \
    "                if line.startswith(\"CmaFree:\"):\n" \
    "                    return int(line.split()[1])\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    return -1\n" \
    "\n" \
    "\n" \
    "NPU_NEED_KB = int(os.environ.get(\"BZ_NPU_NEED_KB\", \"8192\"))\n" \
    "# ppocr 的 det 模型实测要 6912000 字节 ≈ 6.6MB，上面留了点余量。\n" \
    "# 允许用 BZ_NPU_NEED_KB 覆盖，方便在 CMA 充足的机器上演练\"内存不足\"的提示。\n" \
    "\n" \
    "\n" \
    "def _ppocr_usable():\n" \
    "    \"\"\"返回 (能不能用, 说明)。不能用的话说明是给人看的，会一路传到页面上。\n" \
    "    这块板子的 CMA 是启动参数 cma=256M 分的，开机后基本被 GPU/RGA 吃掉，\n" \
    "    跑几次 chrome 就彻底不够了 —— 只有重启能回收。\"\"\"\n" \
    "    if not os.path.exists(\"/usr/bin/ppocr\"):\n" \
    "        return False, \"板上没装 ppocr（rk356x-npu-ocr 包），验证码识别不可用\"\n" \
    "    free = _cma_free_kb()\n" \
    "    if free >= 0 and free < NPU_NEED_KB:\n" \
    "        return False, (\"NPU 内存不足：CMA 连续内存只剩 %d KB，ppocr 至少要 %d KB。\"\n" \
    "                       \"这是设备内存被占满了（浏览器任务最容易吃光），\"\n" \
    "                       \"跑 /root/freemm.sh 重启设备就能回收\" % (free, NPU_NEED_KB))\n" \
    "    return True, \"\"\n" \
    "\n" \
    "\n" \
    "def _ocr_ppocr(path):\n" \
    "    \"\"\"板上 rk356x-npu-ocr 的 PP-OCRv4（走 NPU，0.7 秒一张）。\n" \
    "    返回 (识别文本, 错误说明)。\"\"\"\n" \
    "    try:\n" \
    "        r = subprocess.run([\"ppocr\", path], capture_output=True, text=True, timeout=25)\n" \
    "    except FileNotFoundError:\n" \
    "        return \"\", \"板上没装 ppocr（rk356x-npu-ocr），验证码识别不可用\"\n" \
    "    except Exception as e:\n" \
    "        return \"\", \"ppocr 执行失败：%s\" % str(e).splitlines()[0][:120]\n" \
    "\n" \
    "    blob = (r.stderr or \"\") + (r.stdout or \"\")\n" \
    "    if (\"rknn_init fail\" in blob or \"DMA Heap\" in blob\n" \
    "            or \"malloc npu memory\" in blob or \"init det model fail\" in blob):\n" \
    "        free = _cma_free_kb()\n" \
    "        return \"\", (\"NPU 初始化失败：DMA 内存分配不到（CMA 只剩 %s KB）。\"\n" \
    "                    \"跑 /root/freemm.sh 重启设备可回收\"\n" \
    "                    % (free if free >= 0 else \"?\"))\n" \
    "\n" \
    "    return re.sub(r\"[^0-9A-Za-z]\", \"\", r.stdout or \"\"), \"\"\n" \
    "\n" \
    "\n" \
    "CAP_SEL = (\"img[src*=captcha], img[src*=verify], img[src*=code], \"\n" \
    "           \"img[id*=captcha], img[id*=cap], img[class*=captcha], \"\n" \
    "           \"img[alt*=验证码], img[alt*=captcha], canvas[class*=captcha], \"\n" \
    "           \"[class*=captcha] img, [id*=captcha] img, .verify-img img, \"\n" \
    "           \"[class*=verify] img, [class*=captcha] canvas\")\n" \
    "\n" \
    "OCR_WL = \"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\"\n" \
    "\n" \
    "\n" \
    "def _ocr_png(data):\n" \
    "    \"\"\"验证码识别：只用板上的 ppocr（PP-OCRv4，RKNN 走 NPU，0.7 秒一张）。\n" \
    "\n" \
    "    ppocr 不在、或者 NPU 分不到内存，就返回明确的错误说明（由上层传到页面上），\n" \
    "    不做静默降级 —— 否则页面上只会看到\"验证码识别不出来\"，跟真的认错混在一起。\n" \
    "    \"\"\"\n" \
    "    path = os.path.join(WORKDIR, \"_cap.png\")\n" \
    "    ensure_dir(path)\n" \
    "    with open(path, \"wb\") as f:\n" \
    "        f.write(data)\n" \
    "    return _ocr_ppocr(path)\n" \
    "\n" \
    "\n" \
    "def _find_captcha_img(drv):\n" \
    "    \"\"\"找页面上的验证码图片元素。\n" \
    "    注意：这里的 except 不能吞掉 NameError 之类的编程错误，只能吞\n" \
    "    WebDriver 的定位失败 —— 否则改代码时少个常量会静默变成\"找不到图片\"。\"\"\"\n" \
    "    try:\n" \
    "        from selenium.webdriver.common.by import By\n" \
    "        els = drv.find_elements(By.CSS_SELECTOR, CAP_SEL)\n" \
    "    except Exception:\n" \
    "        return None\n" \
    "    for el in els:\n" \
    "        try:\n" \
    "            sz = el.size\n" \
    "            if sz.get(\"width\", 0) >= 30 and sz.get(\"height\", 0) >= 14:\n" \
    "                return el\n" \
    "        except Exception:\n" \
    "            continue\n" \
    "    return None\n" \
    "\n" \
    "\n" \
    "def _grab_captcha_bytes(drv, el):\n" \
    "    \"\"\"拿验证码图片的原始字节。\n" \
    "    优先走 src 下载（截图会被页面缩放/加边框，OCR 就认不出来了：\n" \
    "    原图 120x40 识别正确，截图版本 m 变 n），canvas 没 src 才退回截图。\"\"\"\n" \
    "    try:\n" \
    "        src = el.get_attribute(\"src\") or \"\"\n" \
    "        if src.startswith(\"data:image\"):\n" \
    "            import base64\n" \
    "            return base64.b64decode(src.split(\",\", 1)[1])\n" \
    "        if src and not src.startswith(\"blob:\"):\n" \
    "            from urllib.parse import urljoin\n" \
    "            import urllib.request\n" \
    "            full = urljoin(drv.current_url, src)\n" \
    "            ck = \"; \".join(\"%s=%s\" % (c.get(\"name\"), c.get(\"value\"))\n" \
    "                           for c in drv.get_cookies())\n" \
    "            req = urllib.request.Request(full, headers={\n" \
    "                \"User-Agent\": UA,\n" \
    "                \"Referer\": drv.current_url,\n" \
    "                \"Cookie\": ck,\n" \
    "            })\n" \
    "            with urllib.request.urlopen(req, timeout=10) as r:\n" \
    "                return r.read()\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    try:\n" \
    "        return el.screenshot_as_png\n" \
    "    except Exception:\n" \
    "        return None\n" \
    "\n" \
    "\n" \
    "def _solve_captcha(drv, out_dir, retry=3, tag=\"\"):\n" \
    "    \"\"\"截图 → OCR → 长度不对就点图刷新再来。\n" \
    "    每次尝试的 PNG 都留着，tag 用来区分是第几轮的（多轮会互相覆盖）。\"\"\"\n" \
    "    os.makedirs(out_dir, exist_ok=True)\n" \
    "    attempts = []\n" \
    "    pngs = []\n" \
    "    el = _find_captcha_img(drv)\n" \
    "    if el is None:\n" \
    "        return None\n" \
    "    for i in range(max(1, retry)):\n" \
    "        data = _grab_captcha_bytes(drv, el)\n" \
    "        if not data:\n" \
    "            break\n" \
    "        p = os.path.join(out_dir, \"%scaptcha_%02d.png\" % (tag, i))\n" \
    "        with open(p, \"wb\") as f:\n" \
    "            f.write(data)\n" \
    "        pngs.append(p)\n" \
    "        text, oerr = _ocr_png(data)\n" \
    "        if oerr:\n" \
    "            # NPU 起不来 / ppocr 缺失：这是环境问题，不是\"认错了\"，\n" \
    "            # 直接往上抛，别浪费轮次重试\n" \
    "            return {\"text\": \"\", \"pngs\": pngs, \"attempts\": attempts,\n" \
    "                    \"error\": oerr, \"npu_unavailable\": True}\n" \
    "        attempts.append({\"png\": p, \"text\": text})\n" \
    "        if 4 <= len(text) <= 6:\n" \
    "            return {\"text\": text, \"pngs\": pngs, \"attempts\": attempts}\n" \
    "        try:\n" \
    "            el.click()          # 点一下通常换一张\n" \
    "            time.sleep(1.0)\n" \
    "        except Exception:\n" \
    "            break\n" \
    "    best = max(attempts, key=lambda a: len(a[\"text\"])) if attempts else None\n" \
    "    return {\"text\": (best or {}).get(\"text\", \"\"), \"pngs\": pngs,\n" \
    "            \"attempts\": attempts, \"low_confidence\": True}\n" \
    "\n" \
    "\n" \
    "def _curl_from_req(r, page_url=\"\"):\n" \
    "    \"\"\"把一个捕获的请求拼成可以直接粘到终端的 curl\"\"\"\n" \
    "    def q(s):\n" \
    "        return \"'\" + str(s).replace(\"'\", \"'\\\\''\") + \"'\"\n" \
    "\n" \
    "    url = (r.get(\"url\") or \"\")\n" \
    "    if url.startswith(\"/\") and page_url:\n" \
    "        from urllib.parse import urljoin\n" \
    "        url = urljoin(page_url, url)\n" \
    "    parts = [\"curl\"]\n" \
    "    m = (r.get(\"method\") or \"GET\").upper()\n" \
    "    if m != \"GET\":\n" \
    "        parts.append(\"-X \" + m)\n" \
    "    parts.append(q(url))\n" \
    "    SKIP = (\"content-length\", \"host\", \"connection\", \"accept-encoding\",\n" \
    "            \"origin\", \"referer\", \"user-agent\", \"cookie\")\n" \
    "    for k, v in (r.get(\"headers\") or {}).items():\n" \
    "        if k.lower() in SKIP:\n" \
    "            continue\n" \
    "        parts.append(\"-H \" + q(\"%s: %s\" % (k, v)))\n" \
    "    b = r.get(\"body\") or \"\"\n" \
    "    if b:\n" \
    "        parts.append(\"--data-raw \" + q(b))\n" \
    "    return \" \".join(parts)\n" \
    "\n" \
    "\n" \
    "def _rank_req(r):\n" \
    "    \"\"\"给捕获的请求打分：越像\"业务数据接口\"分越高。静态资源直接淘汰。\"\"\"\n" \
    "    u = (r.get(\"url\") or \"\").lower()\n" \
    "    if re.search(r\"\\.(js|css|png|jpe?g|gif|svg|woff2?|ico|map)(\\?|$)\", u):\n" \
    "        return -99\n" \
    "    s = 0\n" \
    "    for kw in (\"api\", \"token\", \"list\", \"query\", \"data\", \"get\", \"info\", \"detail\", \"page\"):\n" \
    "        if kw in u:\n" \
    "            s += 2\n" \
    "    if (r.get(\"method\") or \"GET\").upper() == \"POST\":\n" \
    "        s += 3\n" \
    "    # 带自定义签名头的，基本就是我们要找的那种\n" \
    "    for k in (r.get(\"headers\") or {}):\n" \
    "        kl = k.lower()\n" \
    "        if kl.startswith(\"x-\") or \"sign\" in kl or \"token\" in kl or \"auth\" in kl:\n" \
    "            s += 5\n" \
    "    if r.get(\"body\"):\n" \
    "        s += 1\n" \
    "    return s\n" \
    "\n" \
    "\n" \
    "FIND_CLICK_JS = r\"\"\"\n" \
    "return (() => {\n" \
    "  const want = arguments[0];\n" \
    "  const norm = s => (s || '').replace(/[\\s\\u3000]+/g, '').toLowerCase();\n" \
    "  const w = norm(want);\n" \
    "  const roots = [];\n" \
    "  const collect = (root, d) => {\n" \
    "    if (!root || d > 6 || roots.indexOf(root) >= 0) return;\n" \
    "    roots.push(root);\n" \
    "    try { root.querySelectorAll('*').forEach(el => { if (el.shadowRoot) collect(el.shadowRoot, d + 1); }); } catch (e) {}\n" \
    "  };\n" \
    "  collect(document, 0);\n" \
    "  const cands = [];\n" \
    "  roots.forEach(root => {\n" \
    "    let els = [];\n" \
    "    try { els = root.querySelectorAll('a, div, span, li, button, td, p, i, em'); } catch (e) {}\n" \
    "    els.forEach(el => {\n" \
    "      let t = '';\n" \
    "      try { t = norm(el.innerText); } catch (e) { return; }\n" \
    "      if (!t || t.length > 60) return;\n" \
    "      /* 需要是\"看得见\"的 */\n" \
    "      try {\n" \
    "        const rc = el.getBoundingClientRect();\n" \
    "        if (rc.width < 2 || rc.height < 2) return;\n" \
    "      } catch (e) { return; }\n" \
    "      if (t === w) cands.push({el: el, score: 100});\n" \
    "      else if (t.indexOf(w) >= 0) cands.push({el: el, score: 60 - Math.min(40, t.length - w.length)});\n" \
    "    });\n" \
    "  });\n" \
    "  if (!cands.length) return {found: false};\n" \
    "  cands.sort((a, b) => b.score - a.score);\n" \
    "  const best = cands[0].el;\n" \
    "  try { best.scrollIntoView({block: 'center'}); } catch (e) {}\n" \
    "  try { best.click(); } catch (e) { return {found: false, why: 'click failed'}; }\n" \
    "  return {found: true, tag: (best.tagName || '') + '',\n" \
    "          text: ((best.innerText || '') + '').trim().replace(/\\s+/g, ' ').slice(0, 40)};\n" \
    "})()\n" \
    "\"\"\"\n" \
    "\n" \
    "\n" \
    "def _find_input_by_hint(drv, hint):\n" \
    "    \"\"\"按 placeholder / name / id / aria-label 找输入框，返回 __bz_in 下标\"\"\"\n" \
    "    js = r\"\"\"\n" \
    "    return (() => {\n" \
    "      const nh = (arguments[0] || '').replace(/\\s+/g, '').toLowerCase();\n" \
    "      if (!nh) return -1;\n" \
    "      const ins = window.__bz_in || [];\n" \
    "      let best = -1, bestScore = 0;\n" \
    "      ins.forEach((el, i) => {\n" \
    "        let blob = '';\n" \
    "        try {\n" \
    "          blob = [el.placeholder, el.name, el.id,\n" \
    "                  el.getAttribute('aria-label'), el.getAttribute('data-ph')].join(' ');\n" \
    "        } catch (e) {}\n" \
    "        const nb = (blob || '').replace(/\\s+/g, '').toLowerCase();\n" \
    "        let sc = 0;\n" \
    "        if (nb.indexOf(nh) >= 0) sc = 50;\n" \
    "        if (el.placeholder && el.placeholder.replace(/\\s+/g, '').toLowerCase() === nh) sc = 100;\n" \
    "        if (sc > bestScore) { bestScore = sc; best = i; }\n" \
    "      });\n" \
    "      return best;\n" \
    "    })()\n" \
    "    \"\"\"\n" \
    "    try:\n" \
    "        return int(drv.execute_script(js, hint or \"\"))\n" \
    "    except Exception:\n" \
    "        return -1\n" \
    "\n" \
    "\n" \
    "def _run_flow_step(drv, step, req, idx):\n" \
    "    \"\"\"执行流程里的一步。返回 {step, act, page, note, ok, ...}\n" \
    "\n" \
    "    每个 act 都尽量给出\"为什么失败\"，因为真实站点上失败的方式千奇百怪：\n" \
    "    文字不对、元素被遮挡、要等异步渲染、输入框换成了 contenteditable……\n" \
    "    \"\"\"\n" \
    "    act = ((step.get(\"act\") or \"click\") + \"\").lower()\n" \
    "    label = step.get(\"text\") or step.get(\"field\") or \"\"\n" \
    "    r = {\n" \
    "        \"step\": idx,\n" \
    "        \"act\": act,\n" \
    "        \"page\": (step.get(\"page\") or \"\")[:40],      # 这一步在哪个页面做\n" \
    "        \"note\": (step.get(\"note\") or \"\")[:80],      # 人话说明，回显出去\n" \
    "        \"target\": label[:40],\n" \
    "        \"ok\": False,\n" \
    "    }\n" \
    "    try:\n" \
    "        # 每次动作前先刷新元素索引。上一步可能已经把页面换掉了，\n" \
    "        # window.__bz_in / __bz_btn 还是旧页面的，直接拿来 fill/click 会打空。\n" \
    "        if act not in (\"goto\",):\n" \
    "            try:\n" \
    "                drv.execute_script(JS_PROBE)\n" \
    "            except Exception:\n" \
    "                pass\n" \
    "\n" \
    "        if act == \"goto\":\n" \
    "            u = step.get(\"url\") or req.get(\"url\") or \"\"\n" \
    "            drv.get(u)\n" \
    "            time.sleep(float(step.get(\"sec\", 2.0)))\n" \
    "            r[\"ok\"] = True\n" \
    "\n" \
    "        elif act == \"login\":\n" \
    "            cur = drv.current_url\n" \
    "            lr = _login_once(drv, cur,\n" \
    "                             step.get(\"user\") or req.get(\"user\") or \"\",\n" \
    "                             step.get(\"pass\") or req.get(\"pass\") or \"\",\n" \
    "                             req, 0)\n" \
    "            v = lr.get(\"verdict\") or {}\n" \
    "            r[\"ok\"] = bool(v.get(\"likely_ok\"))\n" \
    "            r[\"cookie_count\"] = lr.get(\"cookie_count\", 0)\n" \
    "            r[\"captcha_text\"] = (lr.get(\"filled\") or {}).get(\"captcha_text\", \"\")\n" \
    "            if not r[\"ok\"]:\n" \
    "                r[\"error\"] = (lr.get(\"error\") or lr.get(\"warning\")\n" \
    "                              or \"登录没通过（判据 %d/4）\" % v.get(\"score\", 0))\n" \
    "\n" \
    "        elif act == \"click\":\n" \
    "            got = drv.execute_script(FIND_CLICK_JS, label)\n" \
    "            r[\"ok\"] = bool(got and got.get(\"found\"))\n" \
    "            r[\"matched\"] = (got or {}).get(\"text\", \"\")\n" \
    "            r[\"tag\"] = (got or {}).get(\"tag\", \"\")\n" \
    "            if not r[\"ok\"]:\n" \
    "                r[\"error\"] = (\"页面上找不到「%s」—— 文字要跟显示的一模一样\"\n" \
    "                              \"（全角空格、繁体、图标字符都会影响）\" % label)\n" \
    "\n" \
    "        elif act == \"fill\":\n" \
    "            hint = step.get(\"field\") or step.get(\"hint\") or \"\"\n" \
    "            i = _find_input_by_hint(drv, hint)\n" \
    "            if i < 0:\n" \
    "                r[\"error\"] = \"找不到输入框「%s」\" % hint\n" \
    "            else:\n" \
    "                ok, got = fill_verified(drv, i, step.get(\"value\") or \"\")\n" \
    "                r[\"ok\"] = ok\n" \
    "                r[\"input_i\"] = i\n" \
    "                r[\"value\"] = got\n" \
    "\n" \
    "        elif act == \"wait\":\n" \
    "            time.sleep(float(step.get(\"sec\", 2.0)))\n" \
    "            r[\"ok\"] = True\n" \
    "\n" \
    "        elif act == \"wait_for\":\n" \
    "            pat = step.get(\"text\") or \"\"\n" \
    "            tries = int(step.get(\"tries\", 20))\n" \
    "            ok = False\n" \
    "            for _ in range(max(1, tries)):\n" \
    "                try:\n" \
    "                    hit = drv.execute_script(\n" \
    "                        \"return (document.body && document.body.innerText || '')\"\n" \
    "                        \".indexOf(arguments[0]) >= 0;\", pat)\n" \
    "                    if hit:\n" \
    "                        ok = True\n" \
    "                        break\n" \
    "                except Exception:\n" \
    "                    pass\n" \
    "                time.sleep(0.4)\n" \
    "            r[\"ok\"] = ok\n" \
    "            if not ok:\n" \
    "                r[\"error\"] = \"等了 %.1f 秒也没出现「%s」\" % (0.4 * tries, pat)\n" \
    "\n" \
    "        elif act == \"press\":\n" \
    "            key = step.get(\"key\") or \"Enter\"\n" \
    "            # 注意两点：\n" \
    "            #   1. fill 之后元素已经 blur，activeElement 是 body，键会发丢；\n" \
    "            #      所以支持用 field 指定往哪个输入框发（不给才退回 activeElement）\n" \
    "            #   2. forEach 回调里的 arguments 是 forEach 自己的，必须先存变量\n" \
    "            focus_i = -1\n" \
    "            hint = step.get(\"field\") or step.get(\"hint\") or \"\"\n" \
    "            if hint:\n" \
    "                focus_i = _find_input_by_hint(drv, hint)\n" \
    "            drv.execute_script(\n" \
    "                \"var k = arguments[0], idx = arguments[1];\"\n" \
    "                \"var e = (idx >= 0 && window.__bz_in && window.__bz_in[idx])\"\n" \
    "                \"  ? window.__bz_in[idx] : (document.activeElement || document.body);\"\n" \
    "                \"try { e.focus(); } catch (err) {}\"\n" \
    "                \"['keydown','keypress','keyup'].forEach(function(t){\"\n" \
    "                \"  e.dispatchEvent(new KeyboardEvent(t, {key: k,\"\n" \
    "                \"    bubbles: true, cancelable: true}));});\"\n" \
    "                \"return true;\", key, focus_i)\n" \
    "            r[\"ok\"] = True\n" \
    "            r[\"key\"] = key\n" \
    "            r[\"target\"] = hint or \"(当前焦点)\"\n" \
    "\n" \
    "        elif act == \"capture\":\n" \
    "            # 只是个标记：从这里开始（含）之后的请求才算目标页的\n" \
    "            r[\"ok\"] = True\n" \
    "            r[\"capture\"] = True\n" \
    "\n" \
    "        else:\n" \
    "            r[\"error\"] = \"不认识的 act：%s（支持 goto/login/click/fill/wait/wait_for/press/capture）\" % act\n" \
    "\n" \
    "    except Exception as e:\n" \
    "        r[\"error\"] = str(e).splitlines()[0][:180]\n" \
    "    return r\n" \
    "\n" \
    "\n" \
    "def do_hunt(req):\n" \
    "    \"\"\"按\"流程\"走完一个真实网站，把目标页发出的请求头抄回来。\n" \
    "\n" \
    "    为什么需要 flow：真实站点不是一个\"点一串文字\"能描述的。典型过程是\n" \
    "    第 1 页输账号密码 → 第 2 页点某个模块 → 第 3 页点子模块 → 第 4 页是目标页，\n" \
    "    每页做的事都不一样，中间还可能要先等异步渲染、填搜索框、按回车。\n" \
    "    flow 就是把这件事写成一步一步的脚本。\n" \
    "\n" \
    "    flow 每步的字段：\n" \
    "      act    必填，动作类型\n" \
    "      page   可选，\"这一步在哪个页面\"，只用于回显（方便人看懂走到哪了）\n" \
    "      note   可选，人话说明，同样只用于回显\n" \
    "      text    click / wait_for 的目标文字\n" \
    "      field   fill 的输入框提示（placeholder/name/id 的片段）\n" \
    "      value   fill 要填的值\n" \
    "      sec     wait / goto 等多少秒\n" \
    "      key     press 的键名，默认 Enter\n" \
    "\n" \
    "    act 支持：\n" \
    "      goto      直接跳 URL\n" \
    "      login     用账号密码登录当前页（会自动识别验证码）\n" \
    "      click     点文字匹配的元素（含 div 假按钮、Shadow DOM、iframe）\n" \
    "      fill      填输入框（含 contenteditable）\n" \
    "      wait      死等 N 秒\n" \
    "      wait_for  等某段文字出现（最多 tries 次，每次 0.4 秒）\n" \
    "      press     给当前焦点元素发按键\n" \
    "      capture   标记\"目标页在这里\"，这之后的请求才算数\n" \
    "\n" \
    "    没给 flow 时退回老写法：path 是一串要点的文字，前面自动补一步 login。\n" \
    "    \"\"\"\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    user = req.get(\"user\") or \"\"\n" \
    "    pwd = req.get(\"pass\") or \"\"\n" \
    "\n" \
    "    # ---- 组装 flow ----\n" \
    "    flow = req.get(\"flow\")\n" \
    "    if isinstance(flow, dict):\n" \
    "        flow = [flow]\n" \
    "    if not flow:\n" \
    "        path = req.get(\"path\") or []\n" \
    "        if isinstance(path, str):\n" \
    "            path = [path]\n" \
    "        flow = [{\"act\": \"click\", \"text\": t} for t in path]\n" \
    "        if user and pwd:\n" \
    "            flow = [{\"act\": \"login\", \"user\": user, \"pass\": pwd,\n" \
    "                     \"page\": \"登录页\", \"note\": \"输账号密码\"}] + flow\n" \
    "    if not flow:\n" \
    "        fail(\"缺少 flow（流程步骤）或 path（要点的菜单文字）\")\n" \
    "    if not url:\n" \
    "        # 允许 flow 第一步就是 goto 带 url\n" \
    "        first = flow[0] if isinstance(flow[0], dict) else {}\n" \
    "        url = first.get(\"url\") or \"\"\n" \
    "    if not url:\n" \
    "        fail(\"缺少 url（起始页地址）\")\n" \
    "\n" \
    "    drv = new_driver()\n" \
    "    try:\n" \
    "        # ---- 打开起始页 ----\n" \
    "        try:\n" \
    "            drv.get(url)\n" \
    "        except Exception as e:\n" \
    "            fail(\"打开起始页失败：%s\" % str(e).splitlines()[0][:160])\n" \
    "        time.sleep(float(req.get(\"wait\", 2.0)))\n" \
    "\n" \
    "        # ---- 逐步执行 ----\n" \
    "        steps = []\n" \
    "        all_ok = True\n" \
    "        capture_ts = 0\n" \
    "        for i, st in enumerate(flow, 1):\n" \
    "            if not isinstance(st, dict):\n" \
    "                continue\n" \
    "            time.sleep(float(req.get(\"step_wait\", 1.0)))\n" \
    "            try:\n" \
    "                t_before = int(drv.execute_script(\"return Date.now();\") or 0)\n" \
    "            except Exception:\n" \
    "                t_before = 0\n" \
    "            r = _run_flow_step(drv, st, req, i)\n" \
    "            r[\"t\"] = t_before\n" \
    "            try:\n" \
    "                r[\"url\"] = drv.current_url\n" \
    "            except Exception:\n" \
    "                r[\"url\"] = \"\"\n" \
    "            steps.append(r)\n" \
    "            if not r.get(\"ok\"):\n" \
    "                all_ok = False\n" \
    "                break\n" \
    "            if r.get(\"capture\"):\n" \
    "                # 切分点要用\"进入目标页\"的那一步，不能用 capture 这一步自己的时刻。\n" \
    "                # 因为页面一跳进目标页就立刻发请求了，等轮到 capture 步执行时，\n" \
    "                # 请求早就发出去了，拿它的时刻去切会把目标请求本身切掉。\n" \
    "                capture_ts = (steps[-2].get(\"t\") if len(steps) >= 2 else 0) or t_before\n" \
    "            time.sleep(float(req.get(\"settle\", 1.2)))\n" \
    "\n" \
    "        # 没显式标 capture → 拿最后一步的时刻当切分点\n" \
    "        if not capture_ts and steps:\n" \
    "            capture_ts = steps[-1].get(\"t\") or 0\n" \
    "\n" \
    "        # ---- 等目标页把数据拉完 ----\n" \
    "        time.sleep(float(req.get(\"wait\", 3.0)))\n" \
    "        try:\n" \
    "            raw = drv.execute_script(\"return window.__bz_req || [];\")\n" \
    "        except Exception:\n" \
    "            raw = []\n" \
    "\n" \
    "        # ---- 只留目标页之后的请求 ----\n" \
    "        all_reqs = [it for it in (raw or []) if isinstance(it, dict)]\n" \
    "        cut = max(0, (capture_ts or 0) - 300)      # 宽容 300ms\n" \
    "        final_only = [it for it in all_reqs if (it.get(\"t\") or 0) >= cut] if cut else all_reqs\n" \
    "        dropped = len(all_reqs) - len(final_only)\n" \
    "\n" \
    "        pat = (req.get(\"match\") or \"\").strip()\n" \
    "\n" \
    "        def _pick(src):\n" \
    "            out = []\n" \
    "            for it in src:\n" \
    "                u = it.get(\"url\") or \"\"\n" \
    "                if pat and pat not in u:\n" \
    "                    continue\n" \
    "                sc = _rank_req(it)\n" \
    "                if sc < 0:\n" \
    "                    continue\n" \
    "                it[\"_score\"] = sc\n" \
    "                out.append(it)\n" \
    "            return out\n" \
    "\n" \
    "        reqs = _pick(final_only)\n" \
    "        fell_back = False\n" \
    "        if cut and not reqs:\n" \
    "            reqs = _pick(all_reqs)\n" \
    "            fell_back = True\n" \
    "            dropped = 0\n" \
    "        reqs.sort(key=lambda x: -(x.get(\"_score\") or 0))\n" \
    "        for it in reqs:\n" \
    "            it.pop(\"_score\", None)\n" \
    "\n" \
    "        best = reqs[0] if reqs else None\n" \
    "        page_url = drv.current_url\n" \
    "        obj = {\n" \
    "            \"ok\": bool(all_ok and reqs),\n" \
    "            \"stage\": \"done\" if (all_ok and reqs) else (\"flow\" if not all_ok else \"capture\"),\n" \
    "            \"final_url\": page_url,\n" \
    "            \"title\": (drv.title or \"\")[:60],\n" \
    "            \"flow_total\": len([s for s in flow if isinstance(s, dict)]),\n" \
    "            \"flow_done\": sum(1 for s in steps if s.get(\"ok\")),\n" \
    "            \"steps\": steps,\n" \
    "            \"final_page_only\": bool(cut) and not fell_back,\n" \
    "            \"noise_dropped\": dropped,\n" \
    "            \"request_count\": len(reqs),\n" \
    "            \"requests\": reqs[:12],\n" \
    "            \"best\": best,\n" \
    "            \"curl\": _curl_from_req(best, page_url) if best else \"\",\n" \
    "        }\n" \
    "        if fell_back:\n" \
    "            obj[\"warning\"] = (\"按时间切分没切出请求（拦截器时间戳可能没生效），\"\n" \
    "                              \"下面是全部请求，可能混了中间页的噪音\")\n" \
    "        try:\n" \
    "            obj[\"cookie_header\"] = cookies_to_header(drv.get_cookies())\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "        if not all_ok and steps:\n" \
    "            bad = steps[-1]\n" \
    "            obj[\"error\"] = (\"第 %d 步没成（%s%s）：%s\" % (\n" \
    "                bad.get(\"step\", 0),\n" \
    "                bad.get(\"page\") and (bad[\"page\"] + \" / \") or \"\",\n" \
    "                bad.get(\"note\") or bad.get(\"act\"),\n" \
    "                bad.get(\"error\") or \"未知原因\"))\n" \
    "        elif not reqs:\n" \
    "            obj[\"error\"] = (\"流程走完了但没捕获到请求：页面可能还在加载，\"\n" \
    "                            \"加大 wait；或者用 match 过滤关键字\")\n" \
    "        obj.update(_save_artifacts(\"hunt\", req, obj, obj.get(\"inputs\"), obj.get(\"buttons\")))\n" \
    "        sys.stdout.write(json.dumps(obj, ensure_ascii=False))\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "THUMB_SEL = (\".geetest_slider_button, .nc_iconfont.btn_slide, [class*=slide-btn], \"\n" \
    "             \"[class*=slider-btn], [class*=sliderbtn], [class*=slider] [class*=btn], \"\n" \
    "             \"[id*=sliderBtn], [id*=slideBtn], [class*=thumb], [id*=thumb], \"\n" \
    "             \"[class*=handler], [class*=drag-btn], [class*=verify-btn]\")\n" \
    "\n" \
    "BG_SEL = (\"canvas, img[class*=bg], img[class*=background], img[id*=bg], \"\n" \
    "          \"[class*=geetest_bg] img, [class*=captcha-bg] img, [class*=canvas] img\")\n" \
    "\n" \
    "\n" \
    "def _find_gap_x(img_bytes, min_w=12):\n" \
    "    \"\"\"找缺口左边缘的 x（按图片原始像素算）。\n" \
    "\n" \
    "    不能用\"暗区/亮区\"这种全局亮度阈值：背景本身可能整体偏暗、也可能花花绿绿，\n" \
    "    任何全局阈值都会被背景带偏（实测把暗色渐变背景判成整片都是洞）。\n" \
    "\n" \
    "    缺口的稳定特征是「左右各有一条完整的竖直边」，所以这里算\n" \
    "    \"每列与右移 N 列的逐像素差\"，找差异最大的几处：\n" \
    "      - 最强的第一个峰 → 缺口左边界\n" \
    "      - 第二个峰       → 缺口右边界（顺便可验证缺口宽度像不像拼图块）\n" \
    "    这样跟背景深浅、颜色都没关系。\n" \
    "    \"\"\"\n" \
    "    import io\n" \
    "    from PIL import Image\n" \
    "    try:\n" \
    "        im = Image.open(io.BytesIO(img_bytes)).convert(\"L\")\n" \
    "    except Exception:\n" \
    "        return None, 0\n" \
    "    w, h = im.size\n" \
    "    if w < 40 or h < 20:\n" \
    "        return None, w\n" \
    "    px = im.load()\n" \
    "\n" \
    "    N = 4\n" \
    "    diff = []\n" \
    "    for x in range(w - N):\n" \
    "        s = 0\n" \
    "        for y in range(h):\n" \
    "            s += abs(px[x, y] - px[x + N, y])\n" \
    "        diff.append(s / float(h))\n" \
    "    if not diff:\n" \
    "        return None, w\n" \
    "    mx = max(diff)\n" \
    "    if mx < 10:                      # 整张图没有明显竖直边界\n" \
    "        return None, w\n" \
    "\n" \
    "    hot = [x for x in range(len(diff)) if diff[x] >= mx * 0.55]\n" \
    "    if not hot:\n" \
    "        return None, w\n" \
    "    groups, cur = [], [hot[0]]\n" \
    "    for x in hot[1:]:\n" \
    "        if x - cur[-1] <= 4:\n" \
    "            cur.append(x)\n" \
    "        else:\n" \
    "            groups.append(cur)\n" \
    "            cur = [x]\n" \
    "    groups.append(cur)\n" \
    "    groups = [g for g in groups if len(g) >= 2]\n" \
    "    if not groups:\n" \
    "        return None, w\n" \
    "\n" \
    "    # 跳过贴左图边的噪声（x < 12 通常是图片自身边缘）\n" \
    "    cands = [g[0] for g in groups if g[0] > 12]\n" \
    "    if not cands:\n" \
    "        cands = [g[0] for g in groups]\n" \
    "    left = cands[0]\n" \
    "    # 峰位是\"差异开始变大\"的那一列，真实边界在它右边约 N/2 处\n" \
    "    left += N // 2\n" \
    "    # 太靠右说明可能认反了（拼图块一般从左边出发）\n" \
    "    if left > w * 0.85:\n" \
    "        return None, w\n" \
    "    return left, w\n" \
    "\n" \
    "\n" \
    "def _human_steps(distance, n=26):\n" \
    "    \"\"\"生成一段像人手的位移序列：先加速、中间匀速、最后减速，带轻微抖动。\"\"\"\n" \
    "    import random\n" \
    "    pts = []\n" \
    "    cur = 0.0\n" \
    "    for i in range(n):\n" \
    "        t = (i + 1) / float(n)\n" \
    "        target = distance * (1.0 - (1.0 - t) ** 2.4)      # ease-out\n" \
    "        step = target - cur\n" \
    "        if 0 < i < n - 1:\n" \
    "            step += random.uniform(-0.8, 0.8)             # 手抖\n" \
    "        cur += step\n" \
    "        pts.append(step)\n" \
    "    # 最后一步精确补到目标\n" \
    "    pts[-1] += distance - sum(pts)\n" \
    "    return pts\n" \
    "\n" \
    "\n" \
    "def _human_path(distance, n=42):\n" \
    "    \"\"\"更像真人的轨迹：返回 [(dx, dy, dt_ms), ...]，总和精确等于 distance。\n" \
    "\n" \
    "    为什么 ActionChains 过不了阿里 nc：它每次 perform() 都走一遍完整 action\n" \
    "    chain，事件时间戳的间隔几乎恒定（10ms 上下），加速度曲线是死板的 ease-out。\n" \
    "    nc 的风控就是照这些指标判机器的。这里自己造轨迹：\n" \
    "      1. 起手先抖 1-3px（手指按下去的微动）\n" \
    "      2. 三次 ease-out（比二次的减速尾更平，接近真人）\n" \
    "      3. 抖动幅度与剩余距离正相关 —— 快段不抖，慢段才抖\n" \
    "      4. 时间间隔跟着速度走：快段 8-16ms，慢段 20-45ms，再加随机\n" \
    "      5. 末端过冲 2-6px 再拉回（真人几乎都会过头）\n" \
    "    \"\"\"\n" \
    "    import random\n" \
    "    pts = []\n" \
    "    # 起手微动\n" \
    "    for _ in range(random.randint(1, 2)):\n" \
    "        pts.append((random.uniform(-1.5, 1.5), random.uniform(-1.0, 1.0),\n" \
    "                    random.randint(18, 45)))\n" \
    "    prev = 0.0\n" \
    "    for i in range(n):\n" \
    "        u = (i + 1) / float(n)\n" \
    "        frac = 1.0 - (1.0 - u) ** 3.0\n" \
    "        target = distance * frac\n" \
    "        step = target - prev\n" \
    "        jit = 0.0\n" \
    "        if 0 < i < n - 1:\n" \
    "            amp = 1.5 * (1.0 - frac) + 0.15      # 越慢抖得越明显\n" \
    "            jit = random.uniform(-amp, amp)\n" \
    "        prev = target\n" \
    "        dt = int(8 + 30 * (u ** 1.5) + random.randint(-3, 6))\n" \
    "        pts.append((step + jit, random.uniform(-0.6, 0.6), max(6, dt)))\n" \
    "    # 末端过冲\n" \
    "    over = random.uniform(2.0, 6.0)\n" \
    "    for _ in range(random.randint(2, 3)):\n" \
    "        pts.append((over / 3.0, random.uniform(-0.4, 0.4), random.randint(25, 50)))\n" \
    "    # 回拉\n" \
    "    back = sum(p[0] for p in pts) - distance\n" \
    "    for _ in range(random.randint(2, 3)):\n" \
    "        pts.append((-back / 2.5, random.uniform(-0.3, 0.3), random.randint(35, 70)))\n" \
    "    # 精修到恰好 distance\n" \
    "    err = distance - sum(p[0] for p in pts)\n" \
    "    pts.append((err, 0.0, random.randint(40, 90)))\n" \
    "    return pts\n" \
    "\n" \
    "\n" \
    "def _drag_cdp(drv, thumb, distance):\n" \
    "    \"\"\"用 CDP Input.dispatchMouseEvent 拖滑块，自己控制每个点的时间戳。\n" \
    "\n" \
    "    比 ActionChains 强的地方：时间戳是真实毫秒，不受 python 循环和 webdriver\n" \
    "    往返影响；轨迹可以任意造。缺点是每个事件一次 CDP 往返，40+ 个点会慢\n" \
    "    一两秒 —— 但拖拽本来就要慢才像人。\n" \
    "    返回 (ok, msg, moved)\n" \
    "    \"\"\"\n" \
    "    import time as _t\n" \
    "    try:\n" \
    "        rect = drv.execute_script(\n" \
    "            \"var r = arguments[0].getBoundingClientRect();\"\n" \
    "            \"return {x: r.left + r.width / 2, y: r.top + r.height / 2,\"\n" \
    "            \"        w: r.width, h: r.height};\", thumb)\n" \
    "    except Exception as e:\n" \
    "        return False, \"读滑块坐标失败：%s\" % str(e).splitlines()[0][:80], 0\n" \
    "    x0 = float(rect.get(\"x\") or 0)\n" \
    "    y0 = float(rect.get(\"y\") or 0)\n" \
    "    if not x0 and not y0:\n" \
    "        return False, \"滑块坐标是 (0,0)，可能不可见\", 0\n" \
    "\n" \
    "    path = _human_path(distance)\n" \
    "    # timestamp 单位是【秒】(TimeSinceEpoch)。传毫秒会让 chrome 段错误。\n" \
    "    t0 = _t.time()\n" \
    "    try:\n" \
    "        drv.execute_cdp_cmd(\"Input.dispatchMouseEvent\", {\n" \
    "            \"type\": \"mousePressed\", \"x\": x0, \"y\": y0,\n" \
    "            \"button\": \"left\", \"buttons\": 1, \"clickCount\": 1, \"timestamp\": t0})\n" \
    "    except Exception as e:\n" \
    "        return False, \"按下失败：%s\" % str(e).splitlines()[0][:80], 0\n" \
    "\n" \
    "    elapsed = 0\n" \
    "    cx, cy = x0, y0\n" \
    "    for dx, dy, dt in path:\n" \
    "        elapsed += dt\n" \
    "        cx += dx\n" \
    "        cy += dy\n" \
    "        try:\n" \
    "            drv.execute_cdp_cmd(\"Input.dispatchMouseEvent\", {\n" \
    "                \"type\": \"mouseMoved\", \"x\": cx, \"y\": cy,\n" \
    "                \"button\": \"left\", \"buttons\": 1, \"timestamp\": t0 + elapsed / 1000.0})\n" \
    "        except Exception:\n" \
    "            break\n" \
    "\n" \
    "    # 松手前真停一下（真人到位会稳一下再放）\n" \
    "    _t.sleep(random_uniform(0.08, 0.2))\n" \
    "    try:\n" \
    "        drv.execute_cdp_cmd(\"Input.dispatchMouseEvent\", {\n" \
    "            \"type\": \"mouseReleased\", \"x\": cx, \"y\": cy,\n" \
    "            \"button\": \"left\", \"buttons\": 0, \"clickCount\": 1,\n" \
    "            \"timestamp\": t0 + (elapsed + 130) / 1000.0})\n" \
    "    except Exception as e:\n" \
    "        return False, \"松手失败：%s\" % str(e).splitlines()[0][:80], 0\n" \
    "    return True, \"\", cx - x0\n" \
    "\n" \
    "\n" \
    "def random_uniform(a, b):\n" \
    "    import random\n" \
    "    return random.uniform(a, b)\n" \
    "\n" \
    "\n" \
    "def _drag_thumb(drv, thumb, distance, n=26):\n" \
    "    \"\"\"按住滑块拖 distance 像素。用 ActionChains 走真实鼠标事件。\n" \
    "\n" \
    "    有个坑：浏览器会把鼠标坐标取整，多次 move_by_offset 累积下来实际位移\n" \
    "    会比参数少 10px 左右（实测差 11~12px）。所以松手前要读一次滑块的真实\n" \
    "    位置，把差值补上，否则永远差那么一点点、验证不通过。\n" \
    "    \"\"\"\n" \
    "    import random\n" \
    "    from selenium.webdriver.common.action_chains import ActionChains\n" \
    "    try:\n" \
    "        x0 = thumb.location.get(\"x\", 0)\n" \
    "    except Exception:\n" \
    "        x0 = None\n" \
    "    try:\n" \
    "        ActionChains(drv).move_to_element(thumb).click_and_hold().perform()\n" \
    "    except Exception as e:\n" \
    "        return False, \"按住滑块失败：%s\" % str(e).splitlines()[0][:90]\n" \
    "    time.sleep(random.uniform(0.08, 0.2))\n" \
    "    moved = 0\n" \
    "    for step in _human_steps(distance, n):\n" \
    "        try:\n" \
    "            ActionChains(drv).move_by_offset(step, 0).perform()\n" \
    "        except Exception:\n" \
    "            break\n" \
    "        moved += step\n" \
    "        time.sleep(random.uniform(0.008, 0.03))\n" \
    "\n" \
    "    # ---- 松手前校正累积误差 ----\n" \
    "    if x0 is not None:\n" \
    "        try:\n" \
    "            x1 = thumb.location.get(\"x\", 0)\n" \
    "            actual = x1 - x0\n" \
    "            # 只有滑块确实跟着鼠标走了（至少 60%）才微调。\n" \
    "            # actual≈0 有两种可能：① 这个元素压根不跟鼠标动（选错了）；\n" \
    "            # ② location 读的是另一个参照系。这时候再补 miss 会把距离翻倍 ——\n" \
    "            # 实测淘宝 distance=320 结果拖了 640，滑块直接冲过头验证不过。\n" \
    "            if actual >= distance * 0.6:\n" \
    "                miss = distance - actual\n" \
    "                if abs(miss) >= 2:\n" \
    "                    ActionChains(drv).move_by_offset(miss, 0).perform()\n" \
    "                    moved += miss\n" \
    "                    time.sleep(0.05)\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "\n" \
    "    # 松手前停顿一下（真人也会犹豫）\n" \
    "    time.sleep(random.uniform(0.05, 0.15))\n" \
    "    try:\n" \
    "        ActionChains(drv).release().perform()\n" \
    "    except Exception as e:\n" \
    "        return False, \"松手失败：%s\" % str(e).splitlines()[0][:90]\n" \
    "    return True, moved\n" \
    "\n" \
    "\n" \
    "def do_drag(req):\n" \
    "    \"\"\"处理滑块 / 拼图拖拽验证码。\n" \
    "\n" \
    "    两种用法：\n" \
    "      distance 给了 → 直接拖这么多 CSS 像素（最稳，适合已知尺寸的站点）\n" \
    "      auto=true     → 从背景图里识别缺口位置，自己算距离\n" \
    "\n" \
    "    为什么不用匀速直线：那是最容易被判定成机器人的特征。这里用\n" \
    "    ease-out 轨迹 + 随机抖动 + 松手前停顿，尽量贴近人手。\n" \
    "    \"\"\"\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    if not url:\n" \
    "        fail(\"缺少 url\")\n" \
    "\n" \
    "    auto = bool(req.get(\"auto\", True))\n" \
    "    manual_dist = req.get(\"distance\")\n" \
    "    n_steps = int(req.get(\"steps\", 26))\n" \
    "    tries = max(1, int(req.get(\"tries\", 3)))\n" \
    "\n" \
    "    drv = new_driver()\n" \
    "    history = []\n" \
    "    try:\n" \
    "        drv.get(url)\n" \
    "        time.sleep(float(req.get(\"wait\", 2.5)))\n" \
    "\n" \
    "        # 有的站点要先点一下\"验证\"按钮才弹滑块\n" \
    "        open_sel = req.get(\"open_click\") or \"\"\n" \
    "        if open_sel:\n" \
    "            try:\n" \
    "                r = drv.execute_script(FIND_CLICK_JS, open_sel)\n" \
    "                time.sleep(1.2)\n" \
    "                history.append({\"step\": \"open\", \"click\": open_sel, \"result\": r})\n" \
    "            except Exception:\n" \
    "                pass\n" \
    "\n" \
    "        # ---- 找滑块 ----\n" \
    "        thumb_sel = req.get(\"thumb\") or THUMB_SEL\n" \
    "        try:\n" \
    "            thumbs = drv.find_elements(\"css selector\", thumb_sel)\n" \
    "        except Exception:\n" \
    "            thumbs = []\n" \
    "        thumb = None\n" \
    "        for t in thumbs:\n" \
    "            try:\n" \
    "                sz = t.size\n" \
    "                if sz.get(\"width\", 0) >= 18 and sz.get(\"height\", 0) >= 12 and t.is_displayed():\n" \
    "                    thumb = t\n" \
    "                    break\n" \
    "            except Exception:\n" \
    "                continue\n" \
    "        if thumb is None:\n" \
    "            # 兜底：找 cursor:grab/pointer 的、尺寸像滑块的元素\n" \
    "            thumb = drv.execute_script(\n" \
    "                \"var out = null;\"\n" \
    "                \"document.querySelectorAll('div,span,a,button').forEach(function(e){\"\n" \
    "                \"  if (out) return;\"\n" \
    "                \"  var s; try { s = getComputedStyle(e); } catch (err) { return; }\"\n" \
    "                \"  if (s.cursor !== 'grab' && s.cursor !== 'pointer' && s.cursor !== 'move') return;\"\n" \
    "                \"  var r = e.getBoundingClientRect();\"\n" \
    "                \"  if (r.width >= 28 && r.height >= 20 && r.height <= 70 && r.width <= 120) out = e;\"\n" \
    "                \"});\"\n" \
    "                \"return out;\")\n" \
    "        if thumb is None:\n" \
    "            out(ok=False, stage=\"find_thumb\",\n" \
    "                error=\"页面上没找到滑块（可以传 thumb 选择器指定）\")\n" \
    "\n" \
    "        # ---- 算要拖多远 ----\n" \
    "        dist = None\n" \
    "        gap_info = {}\n" \
    "        if manual_dist is not None:\n" \
    "            try:\n" \
    "                dist = float(manual_dist)\n" \
    "            except Exception:\n" \
    "                dist = None\n" \
    "        if dist is None:\n" \
    "            if not auto:\n" \
    "                fail(\"既没给 distance 也没开 auto\")\n" \
    "            # 从背景图识别缺口\n" \
    "            bg_sel = req.get(\"bg\") or BG_SEL\n" \
    "            bg_data = drv.execute_script(\n" \
    "                \"var sel = arguments[0];\"\n" \
    "                \"var els = [];\"\n" \
    "                \"try { els = document.querySelectorAll(sel); } catch (e) {}\"\n" \
    "                \"for (var i = 0; i < els.length; i++) {\"\n" \
    "                \"  var e = els[i];\"\n" \
    "                \"  var r = e.getBoundingClientRect();\"\n" \
    "                \"  if (r.width < 100 || r.height < 60) continue;\"\n" \
    "                \"  if (e.tagName === 'CANVAS') {\"\n" \
    "                \"    try { return {kind:'dataurl', data: e.toDataURL('image/png'),\"\n" \
    "                \"                   w: e.width, h: e.height, css_w: r.width}; } catch (err) {}\"\n" \
    "                \"    continue; }\"\n" \
    "                \"  if (e.tagName === 'IMG' && e.src) {\"\n" \
    "                \"    return {kind: 'url', data: e.src, w: e.naturalWidth, h: e.naturalHeight,\"\n" \
    "                \"            css_w: r.width}; }\"\n" \
    "                \"}\"\n" \
    "                \"return null;\", bg_sel)\n" \
    "            if not bg_data:\n" \
    "                fail(\"没找到背景图（传 bg 选择器，或者直接用 distance 指定距离）\")\n" \
    "            # 拿图片字节\n" \
    "            blob = b\"\"\n" \
    "            try:\n" \
    "                if bg_data.get(\"kind\") == \"dataurl\":\n" \
    "                    import base64\n" \
    "                    blob = base64.b64decode(bg_data[\"data\"].split(\",\", 1)[1])\n" \
    "                else:\n" \
    "                    src = bg_data.get(\"data\") or \"\"\n" \
    "                    if src.startswith(\"data:image\"):\n" \
    "                        import base64\n" \
    "                        blob = base64.b64decode(src.split(\",\", 1)[1])\n" \
    "                    else:\n" \
    "                        import urllib.request\n" \
    "                        from urllib.parse import urljoin\n" \
    "                        ck = \"; \".join(\"%s=%s\" % (c.get(\"name\"), c.get(\"value\"))\n" \
    "                                       for c in drv.get_cookies())\n" \
    "                        rq = urllib.request.Request(\n" \
    "                            urljoin(drv.current_url, src),\n" \
    "                            headers={\"User-Agent\": UA, \"Referer\": drv.current_url,\n" \
    "                                     \"Cookie\": ck})\n" \
    "                        with urllib.request.urlopen(rq, timeout=10) as resp:\n" \
    "                            blob = resp.read()\n" \
    "            except Exception as e:\n" \
    "                fail(\"下载背景图失败：%s\" % str(e).splitlines()[0][:120])\n" \
    "\n" \
    "            gap_x, img_w = _find_gap_x(blob)\n" \
    "            if gap_x is None:\n" \
    "                fail(\"没能从背景图识别出缺口位置（这类验证码可能只有白色描边），\"\n" \
    "                     \"请用 distance 手动指定要拖多少像素\")\n" \
    "            # 图片像素 → CSS 像素\n" \
    "            css_w = float(bg_data.get(\"css_w\") or img_w or 1)\n" \
    "            scale = css_w / float(img_w or 1)\n" \
    "            dist = gap_x * scale\n" \
    "            gap_info = {\"gap_x_img\": gap_x, \"img_w\": img_w,\n" \
    "                        \"css_w\": round(css_w, 1), \"scale\": round(scale, 3),\n" \
    "                        \"distance_css\": round(dist, 1)}\n" \
    "\n" \
    "        # ---- 拖 ----\n" \
    "        last = None\n" \
    "        for k in range(tries):\n" \
    "            # 默认走 CDP（时间戳真实、轨迹可控）；出问题可以传 cdp=false 回退\n" \
    "            if req.get(\"cdp\", True):\n" \
    "                okd, _msg, info = _drag_cdp(drv, thumb, dist)\n" \
    "                if not okd:\n" \
    "                    okd, info = _drag_thumb(drv, thumb, dist, n_steps)\n" \
    "            else:\n" \
    "                okd, info = _drag_thumb(drv, thumb, dist, n_steps)\n" \
    "            time.sleep(float(req.get(\"settle\", 2.2)))\n" \
    "            # 判定：cookie / 标题 / 页面提示\n" \
    "            try:\n" \
    "                ck = drv.get_cookies()\n" \
    "            except Exception:\n" \
    "                ck = []\n" \
    "            names = {c.get(\"name\", \"\").lower() for c in ck}\n" \
    "            title = (drv.title or \"\")[:40]\n" \
    "            tip = drv.execute_script(\n" \
    "                \"var out = '';\"\n" \
    "                \"document.querySelectorAll('div,span,p,label').forEach(function(e){\"\n" \
    "                \"  if (out) return;\"\n" \
    "                \"  var t='';\"\n" \
    "                \"  try { t = (e.innerText || '').trim(); } catch (err) { return; }\"\n" \
    "                \"  if (!t || t.length > 40) return;\"\n" \
    "                \"  if (/通过|成功|失败|重试|请重试|正确|passed|success|fail|retry/i.test(t)) out = t;\"\n" \
    "                \"});\"\n" \
    "                \"return out;\") or \"\"\n" \
    "            done = (bool(re.search(r\"通过|成功|正确|passed|success\", tip, re.I))\n" \
    "                    or \"验证通过\" in title\n" \
    "                    or any(\"slider\" in n or \"verify\" in n or \"captcha\" in n for n in names))\n" \
    "            # 通过还有一种表现：验证弹窗直接消失了（阿里 nc 过了就把遮罩拆掉，\n" \
    "            # 页面上什么都不留，只看 tip 文字会漏判成失败）\n" \
    "            if not done:\n" \
    "                try:\n" \
    "                    gone = drv.execute_script(\n" \
    "                        \"var sels=['.nc-container','#nc_1_wrapper','#nc_1_n1t',\"\n" \
    "                        \"'[class*=slide-verify]','[class*=verify-wrap]','.baxia-dialog'];\"\n" \
    "                        \"for (var i=0;i<sels.length;i++){\"\n" \
    "                        \"  var e=document.querySelector(sels[i]);\"\n" \
    "                        \"  if (e){var r=e.getBoundingClientRect();\"\n" \
    "                        \"         if (r.width>2&&r.height>2) return false;}}\"\n" \
    "                        \"return true;\")\n" \
    "                    if gone:\n" \
    "                        done = True\n" \
    "                        tip = tip or \"验证弹窗已消失\"\n" \
    "                except Exception:\n" \
    "                    pass\n" \
    "            history.append({\n" \
    "                \"try\": k + 1, \"distance\": round(dist, 1),\n" \
    "                \"drag_ok\": okd, \"moved\": round(info, 1) if isinstance(info, (int, float)) else info,\n" \
    "                \"tip\": tip, \"title\": title,\n" \
    "                \"cookies\": sorted(names)[:8], \"passed\": done,\n" \
    "            })\n" \
    "            last = history[-1]\n" \
    "            if done:\n" \
    "                break\n" \
    "            # 没过：滑块通常会回原位，重拖一次（距离微调 ±3px）\n" \
    "            if k + 1 < tries:\n" \
    "                import random\n" \
    "                dist += random.choice([-3, -2, 2, 3])\n" \
    "                try:\n" \
    "                    drv.execute_script(\n" \
    "                        \"var s = document.querySelector(arguments[0]);\"\n" \
    "                        \"if (s) { s.dispatchEvent(new MouseEvent('mouseup', {bubbles:true})); }\",\n" \
    "                        thumb_sel)\n" \
    "                except Exception:\n" \
    "                    pass\n" \
    "                time.sleep(1.0)\n" \
    "\n" \
    "        page_url = drv.current_url\n" \
    "        passed = bool(last and last.get(\"passed\"))\n" \
    "        obj = {\n" \
    "            \"ok\": passed,\n" \
    "            \"stage\": \"done\" if passed else \"drag\",\n" \
    "            \"distance_used\": round(dist, 1),\n" \
    "            \"manual\": manual_dist is not None,\n" \
    "            \"gap\": gap_info,\n" \
    "            \"history\": history,\n" \
    "            \"final_url\": page_url,\n" \
    "            \"title\": (drv.title or \"\")[:60],\n" \
    "        }\n" \
    "        if not passed:\n" \
    "            obj[\"error\"] = (\"拖了 %s px 还是没过（试了 %d 次）。可能缺口算偏了、\"\n" \
    "                            \"或者这个验证码还要别的动作（点选/旋转）。\"\n" \
    "                            \"可以先用 distance 手动试几个值\" % (round(dist, 1), len(history)))\n" \
    "        try:\n" \
    "            obj[\"cookie_header\"] = cookies_to_header(drv.get_cookies())\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "        obj.update(_save_artifacts(\"drag\", req, obj, obj.get(\"inputs\"), obj.get(\"buttons\")))\n" \
    "        sys.stdout.write(json.dumps(obj, ensure_ascii=False))\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "ARTIFACT_DIR = \"/tmp/llama_agent/gen\"      # cgi ?file= 白名单里有 gen/\n" \
    "\n" \
    "\n" \
    "def _gen_dir():\n" \
    "    d = os.environ.get(\"BZ_GEN_DIR\") or ARTIFACT_DIR\n" \
    "    try:\n" \
    "        os.makedirs(d, exist_ok=True)\n" \
    "    except Exception:\n" \
    "        return \"\"\n" \
    "    return d\n" \
    "\n" \
    "\n" \
    "def _q(s):\n" \
    "    \"\"\"python 字面量里的字符串，保持中文可读\"\"\"\n" \
    "    return json.dumps(s if s is not None else \"\", ensure_ascii=False)\n" \
    "\n" \
    "\n" \
    "SCRIPT_HEAD = '''#!/usr/bin/env python3\n" \
    "# -*- coding: utf-8 -*-\n" \
    "\"\"\"llama agent · browser 插件自动生成的复现脚本\n" \
    "\n" \
    "生成时间 : {ts}\n" \
    "操作     : {op}\n" \
    "目标     : {url}\n" \
    "结果     : {result}\n" \
    "{extra}\n" \
    "这是插件实际执行的动作序列，可以直接跑（改一下 USER / PASS 就能用）：\n" \
    "    pip install selenium\n" \
    "    python3 {fname}\n" \
    "\n" \
    "需要本机装了 Chrome。元素定位用的是插件当时识别到的 CSS 选择器，\n" \
    "页面改版了就换一个选择器。\n" \
    "\n" \
    "账号密码从环境变量读，不写在文件里：\n" \
    "    LOGIN_USER='你的账号' LOGIN_PASS='你的密码' python3 {fname}\n" \
    "\"\"\"\n" \
    "import json\n" \
    "import os\n" \
    "import sys\n" \
    "import time\n" \
    "\n" \
    "from selenium import webdriver\n" \
    "from selenium.webdriver.chrome.options import Options\n" \
    "from selenium.webdriver.common.by import By\n" \
    "from selenium.webdriver.common.action_chains import ActionChains\n" \
    "\n" \
    "URL  = {url_lit}\n" \
    "\n" \
    "# 不把凭据写进文件：要跑就临时给环境变量\n" \
    "#     LOGIN_USER=xxx LOGIN_PASS=yyy python3 {fname}\n" \
    "USER = os.environ.get(\"LOGIN_USER\", \"\")\n" \
    "PASS = os.environ.get(\"LOGIN_PASS\", \"\")\n" \
    "\n" \
    "\n" \
    "HOOK_JS = r\"\"\"\n" \
    "(function(){\n" \
    "  if (window.__req) return;\n" \
    "  window.__req = [];\n" \
    "  var XO = XMLHttpRequest.prototype.open,\n" \
    "      XH = XMLHttpRequest.prototype.setRequestHeader,\n" \
    "      XS = XMLHttpRequest.prototype.send;\n" \
    "  XMLHttpRequest.prototype.open = function(m, u){ this.__m = m; this.__u = u; this.__h = {}; return XO.apply(this, arguments); };\n" \
    "  XMLHttpRequest.prototype.setRequestHeader = function(k, v){ try { this.__h[k] = v; } catch (e) {} return XH.apply(this, arguments); };\n" \
    "  XMLHttpRequest.prototype.send = function(b){\n" \
    "    try { window.__req.push({kind: 'xhr', method: this.__m, url: this.__u, headers: this.__h,\n" \
    "                             body: (b == null ? '' : String(b).slice(0, 500))}); } catch (e) {}\n" \
    "    return XS.apply(this, arguments);\n" \
    "  };\n" \
    "  var OF = window.fetch;\n" \
    "  if (OF) {\n" \
    "    window.fetch = function(input, init){\n" \
    "      try {\n" \
    "        var u = (typeof input === 'string') ? input : ((input && input.url) || ''), h = {};\n" \
    "        var hs = (init && init.headers) || (input && input.headers);\n" \
    "        if (hs) {\n" \
    "          if (hs.forEach) hs.forEach(function(v, k){ h[k] = v; });\n" \
    "          else Object.keys(hs).forEach(function(k){ h[k] = hs[k]; });\n" \
    "        }\n" \
    "        window.__req.push({kind: 'fetch',\n" \
    "                           method: ((init && init.method) || 'GET') + '',\n" \
    "                           url: u + '', headers: h,\n" \
    "                           body: ((init && init.body) || '') + ''\n" \
    "                                 ? String((init && init.body) || '').slice(0, 500) : ''});\n" \
    "      } catch (e) {}\n" \
    "      return OF.apply(this, arguments);\n" \
    "    };\n" \
    "  }\n" \
    "})();\n" \
    "\"\"\"\n" \
    "\n" \
    "\n" \
    "def make_driver():\n" \
    "    o = Options()\n" \
    "    o.add_argument(\"--headless=new\")\n" \
    "    o.add_argument(\"--no-sandbox\")\n" \
    "    o.add_argument(\"--disable-dev-shm-usage\")\n" \
    "    o.add_argument(\"--window-size=1366,900\")\n" \
    "    o.add_argument(\"--password-store=basic\")\n" \
    "    o.add_argument(\"--disable-features=PasswordManagerOnboarding,AutofillServerCommunication\")\n" \
    "    o.add_experimental_option(\"excludeSwitches\", [\"enable-automation\"])\n" \
    "    o.add_experimental_option(\"useAutomationExtension\", False)\n" \
    "    d = webdriver.Chrome(options=o)\n" \
    "    try:\n" \
    "        d.execute_cdp_cmd(\"Page.addScriptToEvaluateOnNewDocument\", {\n" \
    "            \"source\": \"Object.defineProperty(navigator, 'webdriver', {get: () => undefined});\"})\n" \
    "        d.execute_cdp_cmd(\"Page.addScriptToEvaluateOnNewDocument\", {\"source\": HOOK_JS})\n" \
    "        d.execute_script(HOOK_JS)\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    return d\n" \
    "\n" \
    "\n" \
    "def fill(drv, sel, value):\n" \
    "    \"\"\"填值。必须用真实键盘事件 —— React 站点只认 isTrusted 的输入。\"\"\"\n" \
    "    el = drv.find_element(By.CSS_SELECTOR, sel)\n" \
    "    drv.execute_script(\"var e = arguments[0];\"\n" \
    "                       \"if (e.isContentEditable) e.textContent = ''; else e.value = '';\", el)\n" \
    "    ActionChains(drv).send_keys_to_element(el, value).perform()\n" \
    "    time.sleep(0.15)\n" \
    "    return el\n" \
    "\n" \
    "\n" \
    "def click_text(drv, text):\n" \
    "    \"\"\"按可见文字点元素（div 假按钮也适用）。\"\"\"\n" \
    "    js = (\"var want = arguments[0].replace(/[\\\\\\\\s\\\\\\\\u3000]+/g, '').toLowerCase(), hit = null;\"\n" \
    "          \"document.querySelectorAll('a,div,span,li,button,td,p,i,em').forEach(function(e){\"\n" \
    "          \"  if (hit) return;\"\n" \
    "          \"  var t = ''; try { t = (e.innerText || '').replace(/[\\\\\\\\s\\\\\\\\u3000]+/g, '').toLowerCase(); }\"\n" \
    "          \"  catch (x) { return; }\"\n" \
    "          \"  if (!t || t.length > 60) return;\"\n" \
    "          \"  var r = e.getBoundingClientRect();\"\n" \
    "          \"  if (r.width < 2 || r.height < 2) return;\"\n" \
    "          \"  if (t === want || t.indexOf(want) >= 0) hit = e;\"\n" \
    "          \"});\"\n" \
    "          \"if (hit) { hit.scrollIntoView({block: 'center'}); hit.click(); return true; }\"\n" \
    "          \"return false;\")\n" \
    "    return drv.execute_script(js, text)\n" \
    "\n" \
    "\n" \
    "if not USER or not PASS:\n" \
    "    sys.exit(\"需要账号密码，用环境变量给：\\\\n\"\n" \
    "             \"    LOGIN_USER='你的账号' LOGIN_PASS='你的密码' python3 \" + os.path.basename(__file__))\n" \
    "\n" \
    "drv = make_driver()\n" \
    "try:\n" \
    "    drv.get(URL)\n" \
    "    time.sleep(2.5)\n" \
    "'''\n" \
    "\n" \
    "SCRIPT_TAIL = '''\n" \
    "    # ---- 收尾：导出本次会话凭据 + 抓到的请求 ----\n" \
    "    print('当前页面:', drv.current_url)\n" \
    "    print('标题   :', drv.title)\n" \
    "    print()\n" \
    "    print('=== cookies ===')\n" \
    "    print(json.dumps(drv.get_cookies(), ensure_ascii=False, indent=2))\n" \
    "    print()\n" \
    "    print('=== localStorage / sessionStorage ===')\n" \
    "    print(json.dumps(drv.execute_script(\n" \
    "        \"var o={local:{},session:{}};\"\n" \
    "        \"for (var i=0;i<localStorage.length;i++){var k=localStorage.key(i);o.local[k]=localStorage.getItem(k);}\"\n" \
    "        \"for (var i=0;i<sessionStorage.length;i++){var k=sessionStorage.key(i);o.session[k]=sessionStorage.getItem(k);}\"\n" \
    "        \"return o;\"), ensure_ascii=False, indent=2))\n" \
    "    print()\n" \
    "    print('=== 页面发出的请求（含 JS 算出来的签名头）===')\n" \
    "    reqs = drv.execute_script('return window.__req || [];') or []\n" \
    "    for r in reqs:\n" \
    "        print(r.get('method'), r.get('url'))\n" \
    "        for k, v in (r.get('headers') or {}).items():\n" \
    "            print('    %s: %s' % (k, v))\n" \
    "    with open('requests_headers.json', 'w', encoding='utf-8') as f:\n" \
    "        json.dump(reqs, f, ensure_ascii=False, indent=2)\n" \
    "    print()\n" \
    "    print('请求头已存到 requests_headers.json')\n" \
    "finally:\n" \
    "    drv.quit()\n" \
    "'''\n" \
    "\n" \
    "\n" \
    "def _render_script(op, req, res):\n" \
    "    \"\"\"把这次执行还原成一段能独立跑的 Python 脚本。\n" \
    "\n" \
    "    目的很实际：插件跑挂了用户得能自己复现、自己改。所以脚本里全是真实\n" \
    "    selenium 调用（find_element + send_keys + click），元素定位用 CSS 选择器\n" \
    "    而不是本进程内的 window.__bz_in[0]，参数直接写死，改一下就能用。\n" \
    "    \"\"\"\n" \
    "    import time as _t\n" \
    "    filled = res.get(\"filled\") or {}\n" \
    "    verdict = res.get(\"verdict\") or {}\n" \
    "    okv = bool(res.get(\"ok\") or verdict.get(\"likely_ok\"))\n" \
    "\n" \
    "    extra = \"\"\n" \
    "    if res.get(\"error\"):\n" \
    "        extra = \"失败原因 : %s\\n\" % str(res.get(\"error\"))[:140]\n" \
    "\n" \
    "    # 用占位符替换，不用 str.format —— 模板里有一大堆 JS 花括号（{{ }} 转义\n" \
    "    # 起来太容易漏），replace 干净得多。注意顺序：{url_lit} 必须在 {url} 之前，\n" \
    "    # 否则 {url} 会把 {url_lit} 的前半截吃掉。\n" \
    "    head = SCRIPT_HEAD\n" \
    "    for key, val in (\n" \
    "            (\"{url_lit}\", _q(req.get(\"url\") or \"\")),\n" \
    "            (\"{user_lit}\", _q(req.get(\"user\") or \"\")),\n" \
    "            (\"{pass_lit}\", _q(req.get(\"pass\") or \"\")),\n" \
    "            (\"{extra}\", extra),\n" \
    "            (\"{fname}\", os.path.basename(res.get(\"_fname\") or \"repro.py\")),\n" \
    "            (\"{result}\", (\"成功\" if okv else \"失败\")),\n" \
    "            (\"{ts}\", _t.strftime(\"%Y-%m-%d %H:%M:%S\")),\n" \
    "            (\"{op}\", op),\n" \
    "            (\"{url}\", (req.get(\"url\") or \"\")),\n" \
    "    ):\n" \
    "        head = head.replace(key, val)\n" \
    "\n" \
    "    B = []\n" \
    "    ins = res.get(\"probe_inputs\") or []\n" \
    "    btns = res.get(\"probe_buttons\") or []\n" \
    "\n" \
    "    def sel_of(kind, idx, fallback):\n" \
    "        pool = ins if kind == \"in\" else btns\n" \
    "        for it in pool:\n" \
    "            if it.get(\"i\") == idx and it.get(\"sel\"):\n" \
    "                return it[\"sel\"]\n" \
    "        return fallback\n" \
    "\n" \
    "    # ---- 登录 ----\n" \
    "    if op in (\"autologin\", \"login\") or (op == \"hunt\" and filled.get(\"user_i\") is not None):\n" \
    "        u_sel = sel_of(\"in\", filled.get(\"user_i\"), \"input[type='text']\")\n" \
    "        p_sel = sel_of(\"in\", filled.get(\"pass_i\"), \"input[type='password']\")\n" \
    "        b_sel = sel_of(\"btn\", filled.get(\"btn_i\"), \"button[type='submit']\")\n" \
    "        B.append(\"    # ---- 登录 ----\")\n" \
    "        B.append(\"    fill(drv, %s, USER)      # 账号框\" % _q(u_sel))\n" \
    "        B.append(\"    fill(drv, %s, PASS)      # 密码框\" % _q(p_sel))\n" \
    "        if filled.get(\"captcha_text\"):\n" \
    "            B.append(\"    # 这次页面上有验证码，插件识别成 %r（复现时要自己处理）\"\n" \
    "                     % filled[\"captcha_text\"])\n" \
    "        B.append(\"    # 点登录按钮\")\n" \
    "        B.append(\"    try:\")\n" \
    "        B.append(\"        drv.find_element(By.CSS_SELECTOR, %s).click()\" % _q(b_sel))\n" \
    "        B.append(\"    except Exception:\")\n" \
    "        B.append(\"        click_text(drv, '登录')     # 兜底：按文字点\")\n" \
    "        B.append(\"    time.sleep(2.5)\")\n" \
    "        B.append(\"\")\n" \
    "\n" \
    "    # ---- hunt 流程 ----\n" \
    "    if op == \"hunt\":\n" \
    "        flow = req.get(\"flow\") or []\n" \
    "        if not flow and req.get(\"path\"):\n" \
    "            flow = [{\"act\": \"click\", \"text\": t} for t in req[\"path\"]]\n" \
    "        if flow:\n" \
    "            B.append(\"    # ---- 流程：按页面顺序走 ----\")\n" \
    "            for i, st in enumerate(flow, 1):\n" \
    "                if not isinstance(st, dict):\n" \
    "                    continue\n" \
    "                act = (st.get(\"act\") or \"click\").lower()\n" \
    "                cmt = \"    # 第%d步\" % i\n" \
    "                if st.get(\"page\"):\n" \
    "                    cmt += \" · \" + str(st[\"page\"])\n" \
    "                if st.get(\"note\"):\n" \
    "                    cmt += \" · \" + str(st[\"note\"])\n" \
    "                B.append(cmt)\n" \
    "                if act == \"login\":\n" \
    "                    B.append(\"    fill(drv, \\\"input[name='username']\\\", USER)\")\n" \
    "                    B.append(\"    fill(drv, \\\"input[type='password']\\\", PASS)\")\n" \
    "                    B.append(\"    click_text(drv, '登录')\")\n" \
    "                    B.append(\"    time.sleep(2.5)\")\n" \
    "                elif act == \"click\":\n" \
    "                    B.append(\"    click_text(drv, %s)\" % _q(st.get(\"text\") or \"\"))\n" \
    "                    B.append(\"    time.sleep(1.5)\")\n" \
    "                elif act == \"fill\":\n" \
    "                    ph = st.get(\"field\") or \"账号\"\n" \
    "                    B.append(\"    fill(drv, \\\"input[placeholder*='%s']\\\", %s)\"\n" \
    "                             % (str(ph).replace(\"'\", \"\"), _q(st.get(\"value\") or \"\")))\n" \
    "                elif act == \"press\":\n" \
    "                    B.append(\"    drv.execute_script(\\\"var e=document.activeElement||document.body;\"\n" \
    "                             \"['keydown','keypress','keyup'].forEach(function(t){\"\n" \
    "                             \"e.dispatchEvent(new KeyboardEvent(t,{key:%s,bubbles:true}));});\\\")\"\n" \
    "                             % json.dumps(st.get(\"key\") or \"Enter\"))\n" \
    "                    B.append(\"    time.sleep(1.5)\")\n" \
    "                elif act == \"wait\":\n" \
    "                    B.append(\"    time.sleep(%s)\" % (st.get(\"sec\") or 2))\n" \
    "                elif act == \"wait_for\":\n" \
    "                    B.append(\"    # 等「%s」出现\" % (st.get(\"text\") or \"\"))\n" \
    "                    B.append(\"    for _ in range(%d):\" % int(st.get(\"tries\") or 20))\n" \
    "                    B.append(\"        if drv.execute_script(\\\"return document.body.innerText\"\n" \
    "                             \".indexOf(arguments[0]) >= 0;\\\", %s):\" % _q(st.get(\"text\") or \"\"))\n" \
    "                    B.append(\"            break\")\n" \
    "                    B.append(\"        time.sleep(0.4)\")\n" \
    "                elif act == \"capture\":\n" \
    "                    B.append(\"    # ↑ 目标页：从这里开始的请求才是要抓的\")\n" \
    "                B.append(\"\")\n" \
    "\n" \
    "    # ---- drag ----\n" \
    "    if op == \"drag\":\n" \
    "        B.append(\"    # ---- 拖拽滑块 ----\")\n" \
    "        B.append(\"    thumb = drv.find_element(By.CSS_SELECTOR, %s)\"\n" \
    "                 % _q(req.get(\"thumb\") or \".slider-btn, [class*=thumb]\"))\n" \
    "        B.append(\"    Distance = %s        # 插件这次拖的像素数\"\n" \
    "                 % (res.get(\"distance_used\") or req.get(\"distance\") or 0))\n" \
    "        B.append(\"    x0 = thumb.location['x']\")\n" \
    "        B.append(\"    ActionChains(drv).move_to_element(thumb).click_and_hold().perform()\")\n" \
    "        B.append(\"    time.sleep(0.15)\")\n" \
    "        B.append(\"    ActionChains(drv).move_by_offset(Distance, 0).perform()\")\n" \
    "        B.append(\"    # 浏览器对鼠标坐标取整，累积下来会差十几像素，补一次\")\n" \
    "        B.append(\"    miss = Distance - (thumb.location['x'] - x0)\")\n" \
    "        B.append(\"    if abs(miss) >= 2:\")\n" \
    "        B.append(\"        ActionChains(drv).move_by_offset(miss, 0).perform()\")\n" \
    "        B.append(\"    time.sleep(0.1)\")\n" \
    "        B.append(\"    ActionChains(drv).release().perform()\")\n" \
    "        B.append(\"    time.sleep(2)\")\n" \
    "        B.append(\"\")\n" \
    "\n" \
    "    return head + \"\\n\".join(B) + SCRIPT_TAIL\n" \
    "\n" \
    "\n" \
    "def _save_artifacts(op, req, res, probe_inputs=None, probe_buttons=None):\n" \
    "    \"\"\"写复现脚本（成功时再加请求头 JSON 和 curl）。返回要补进 res 的字段。\"\"\"\n" \
    "    d = _gen_dir()\n" \
    "    if not d:\n" \
    "        return {}\n" \
    "    import time as _t\n" \
    "    stamp = _t.strftime(\"%Y%m%d_%H%M%S\")\n" \
    "    base = \"%s_%s\" % (op, stamp)\n" \
    "    out = {}\n" \
    "\n" \
    "    if probe_inputs:\n" \
    "        res[\"probe_inputs\"] = probe_inputs\n" \
    "    if probe_buttons:\n" \
    "        res[\"probe_buttons\"] = probe_buttons\n" \
    "\n" \
    "    # ---- 1. 复现脚本（成功失败都生成）----\n" \
    "    try:\n" \
    "        py = os.path.join(d, base + \".py\")\n" \
    "        res[\"_fname\"] = os.path.basename(py)\n" \
    "        code = _render_script(op, req, res)\n" \
    "        with open(py, \"w\", encoding=\"utf-8\") as f:\n" \
    "            f.write(code)\n" \
    "        try:\n" \
    "            os.chmod(py, 0o644)\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "        out[\"script_path\"] = py\n" \
    "    except Exception as e:\n" \
    "        out[\"script_error\"] = str(e)[:140]\n" \
    "    finally:\n" \
    "        res.pop(\"_fname\", None)\n" \
    "        res.pop(\"probe_inputs\", None)\n" \
    "        res.pop(\"probe_buttons\", None)\n" \
    "\n" \
    "    # ---- 2. 成功时导出请求头 + curl ----\n" \
    "    reqs = res.get(\"requests\") or ([res[\"best\"]] if res.get(\"best\") else [])\n" \
    "    if res.get(\"ok\") and reqs:\n" \
    "        try:\n" \
    "            hj = os.path.join(d, base + \"_headers.json\")\n" \
    "            with open(hj, \"w\", encoding=\"utf-8\") as f:\n" \
    "                json.dump(reqs, f, ensure_ascii=False, indent=2)\n" \
    "            out[\"headers_path\"] = hj\n" \
    "        except Exception as e:\n" \
    "            out[\"headers_error\"] = str(e)[:140]\n" \
    "        try:\n" \
    "            sh = os.path.join(d, base + \"_curl.sh\")\n" \
    "            page = res.get(\"final_url\") or req.get(\"url\") or \"\"\n" \
    "            lines = [\"#!/bin/sh\",\n" \
    "                     \"# 由 llama agent 生成：把抓到的请求还原成 curl\",\n" \
    "                     \"# 生成时间 %s\" % stamp, \"\"]\n" \
    "            for r in reqs:\n" \
    "                lines.append(\"# %s %s\" % (r.get(\"method\") or \"GET\",\n" \
    "                                          (r.get(\"url\") or \"\")[:100]))\n" \
    "                lines.append(_curl_from_req(r, page))\n" \
    "                lines.append(\"\")\n" \
    "            with open(sh, \"w\", encoding=\"utf-8\") as f:\n" \
    "                f.write(\"\\n\".join(lines))\n" \
    "            try:\n" \
    "                os.chmod(sh, 0o755)\n" \
    "            except Exception:\n" \
    "                pass\n" \
    "            out[\"curl_path\"] = sh\n" \
    "        except Exception as e:\n" \
    "            out[\"curl_error\"] = str(e)[:140]\n" \
    "\n" \
    "    return out\n" \
    "\n" \
    "\n" \
    "def do_captcha(req):\n" \
    "    \"\"\"单独识别验证码：返回识别结果 + 每张截图，识别不出来给人看\"\"\"\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    if not url:\n" \
    "        fail(\"缺少 url\")\n" \
    "    out_dir = req.get(\"out_dir\") or SHOT_DIR\n" \
    "    # 明确要识别验证码 → 起 chrome 之前先看 NPU 能不能用，\n" \
    "    # 不然白起一个 300MB 的浏览器再失败\n" \
    "    ok_npu, why_npu = _ppocr_usable()\n" \
    "    if not ok_npu:\n" \
    "        fail(why_npu, npu_unavailable=True, cma_free_kb=_cma_free_kb())\n" \
    "    drv = new_driver()\n" \
    "    try:\n" \
    "        try:\n" \
    "            drv.get(url)\n" \
    "        except Exception as e:\n" \
    "            fail(\"打开页面失败：%s\" % str(e).splitlines()[0][:200])\n" \
    "        time.sleep(float(req.get(\"wait\", 2)))\n" \
    "        drv.execute_script(JS_PROBE)\n" \
    "        got = _solve_captcha(drv, out_dir, int(req.get(\"retry\", 3)))\n" \
    "        if not got:\n" \
    "            fail(\"页面上没找到验证码图片\",\n" \
    "                 hint=\"有些站点验证码在 iframe 或 canvas 里，用 op=shot 整页截图看看\")\n" \
    "        if got.get(\"npu_unavailable\"):\n" \
    "            fail(got.get(\"error\") or \"NPU 不可用，验证码识别起不来\",\n" \
    "                 npu_unavailable=True, pngs=got.get(\"pngs\", []))\n" \
    "        out(ok=True, url=drv.current_url, **got)\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "def do_cookie(req):\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    drv = new_driver()\n" \
    "    try:\n" \
    "        if url:\n" \
    "            try:\n" \
    "                drv.get(url)\n" \
    "            except Exception as e:\n" \
    "                fail(\"打开页面失败：%s\" % str(e).splitlines()[0][:200])\n" \
    "            time.sleep(float(req.get(\"wait\", 2)))\n" \
    "        cookies = drv.get_cookies()\n" \
    "        dom = req.get(\"domain\") or \"\"\n" \
    "        if dom:\n" \
    "            cookies = [c for c in cookies if dom in (c.get(\"domain\") or \"\")]\n" \
    "        sys.stdout.write(json.dumps({\n" \
    "            \"ok\": True,\n" \
    "            \"url\": drv.current_url if url else \"\",\n" \
    "            \"cookies\": cookies,\n" \
    "            \"cookie_count\": len(cookies),\n" \
    "            \"cookie_header\": cookies_to_header(cookies),\n" \
    "            \"storage\": (drv.execute_script(JS_STORAGE) if url else {}),\n" \
    "            \"mem_mb\": mem_avail_mb(),\n" \
    "        }, ensure_ascii=False))\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "    sys.exit(0)\n" \
    "\n" \
    "\n" \
    "def do_shot(req):\n" \
    "    url = req.get(\"url\") or \"\"\n" \
    "    path = req.get(\"path\") or (WORKDIR + \"/shot.png\")\n" \
    "    ensure_dir(path)\n" \
    "    drv = new_driver(int(req.get(\"width\", 1366)), int(req.get(\"height\", 900)))\n" \
    "    try:\n" \
    "        if url:\n" \
    "            try:\n" \
    "                drv.get(url)\n" \
    "            except Exception as e:\n" \
    "                fail(\"打开页面失败：%s\" % str(e).splitlines()[0][:200])\n" \
    "            time.sleep(float(req.get(\"wait\", 2)))\n" \
    "        ok = drv.save_screenshot(path)\n" \
    "        out(path=path, saved=bool(ok), size=os.path.getsize(path) if ok else 0,\n" \
    "            title=drv.title)\n" \
    "    finally:\n" \
    "        try:\n" \
    "            drv.quit()\n" \
    "        except Exception:\n" \
    "            pass\n" \
    "\n" \
    "\n" \
    "def main():\n" \
    "    if len(sys.argv) < 2:\n" \
    "        fail(\"缺少 op（probe / login / cookie / shot）\")\n" \
    "    op = sys.argv[1]\n" \
    "    raw = sys.stdin.read()\n" \
    "    try:\n" \
    "        req = json.loads(raw) if raw.strip() else {}\n" \
    "    except ValueError as e:\n" \
    "        fail(\"参数不是合法 JSON：%s\" % e)\n" \
    "\n" \
    "    avail = mem_avail_mb()\n" \
    "    if avail < 350:\n" \
    "        fail(\"可用内存只有 %d MB，Chrome 起不来。先跑 /root/freemm.sh 重启释放内存\" % avail)\n" \
    "\n" \
    "    if op == \"probe\":\n" \
    "        do_probe(req)\n" \
    "    elif op == \"login\":\n" \
    "        do_login(req)\n" \
    "    elif op == \"autologin\":\n" \
    "        do_autologin(req)\n" \
    "    elif op == \"captcha\":\n" \
    "        do_captcha(req)\n" \
    "    elif op == \"drag\":\n" \
    "        do_drag(req)\n" \
    "    elif op == \"hunt\":\n" \
    "        do_hunt(req)\n" \
    "    elif op == \"cookie\":\n" \
    "        do_cookie(req)\n" \
    "    elif op == \"shot\":\n" \
    "        do_shot(req)\n" \
    "    else:\n" \
    "        fail(\"未知 op：%s\" % op)\n" \
    "\n" \
    "\n" \
    "if __name__ == \"__main__\":\n" \
    "    main()\n" \
    "\n" \
    "\n" \
    "\n" \
    "\n"
#endif
