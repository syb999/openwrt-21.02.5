'use strict';
'require view';
'require fs';

/*
 * luci-app-chrome - frontend view
 *
 * Renders /usr/bin/chromeui: a headless-Chrome page grabber for the Panther X2
 * (RK3566).  One input box, one button: the page is fetched once in a single
 * browser session and everything comes back together - title, final URL,
 * visible text and the media resources the page pulled in
 * (video/audio/HLS/DASH/images) as URLs - this app never downloads anything -
 * taken from three independent sources because
 * each one alone misses cases the others catch:
 *
 *   network - the DevTools performance log: EVERY response Chrome made,
 *             including m3u8/mp4/xhr media and anything loaded inside
 *             cross-origin iframes that the DOM cannot be read for.
 *   dom     - querySelectorAll on <video>/<audio>/<source>/<img>/... (and
 *             same-origin frames): gives the element tag and currentSrc.
 *   html    - bare urls in the rendered HTML, for resources referenced in
 *             script text that were never fetched.
 *
 * The grab runs DETACHED (chromeui start) and this page polls chromeui job, so
 * the browser shows live progress instead of freezing on one long call.
 *
 * i18n: the source strings here are ENGLISH and wrapped in _(); Chinese lives in
 * po/zh_Hans/chrome.po (installed as luci-i18n-chrome-zh-cn).  The backend never
 * sends display text - it sends an english template plus arguments, which
 * fmtTpl() renders through the very same _() lookup.
 *
 * NOTE: template literals are avoided on purpose (CONFIG_LUCI_JSMIN breaks
 * them), and so are regex literals containing "://" (jsmin truncates them).
 */

/* stamped with PKG_VERSION at build time - it MUST equal the backend's version,
 * otherwise this file is a stale copy in the browser cache */
var CHROME_VER = '__PKG_VERSION__';

/* ---------------------------------------------------------------------------
 * 🔴 Translations do NOT come from LuCI's dictionary.  That one is an HTTP
 * resource served with "Cache-Control: max-age=31536000", so a browser keeps the
 * catalogue it first downloaded for a whole year and every string added later
 * shows up in English - forever, without a "clear site data".  The backend hands
 * the same catalogue over the RPC channel instead (nothing is cached there), and
 * these overrides route every call in this view through it.
 * ------------------------------------------------------------------------- */
var I18N = {};
function _(s) {
	var k = String(s);
	return (I18N[k] !== undefined) ? I18N[k] : k;
}
function T(s) { return _(s); }
var EXEC = '/usr/bin/chromeui';
var QQEXEC = '/usr/bin/qqurl';

/* ---------------------------------------------------------------------------
 * Backend vocabulary.  The backend emits these english templates as msgid plus
 * arguments; listing them here as _() literals is what makes the translation
 * scanner pick them up into the catalogue.  At runtime fmtTpl() does the same
 * lookup, translating the arguments as well (so a word like "Timed out" in an
 * argument gets translated while paths and numbers pass through).
 * ------------------------------------------------------------------------- */
/* eslint-disable no-unused-vars */
var BACKEND_MSGS = [
	_('No URL given'),
	_('Unknown argument: %s'),
	_('Unknown argument: --%s'),
	_('Missing value for --%s'),
	_('Invalid number: %s'),
	_('chrome-headless-shell is not installed (%s)'),
	_('chromedriver is not installed (%s)'),
	_('Another grab is already running, wait for it to finish'),
	_('Cannot start the browser session: %s || driver log: %s'),
	_('Cannot start the browser worker: %s'),
	_('Cannot open %s (%s)'),
	_('Element %s did not appear within %ss'),
	_('Grab failed: %s'),
	_('Cannot reach %s (%s)'),
	_('Download failed: %s'),
	_('Cannot create the output directory: %s'),
	_('Cannot write %s (%s)'),
	_('Cannot stop the job: %s'),
	_('No job is currently running'),
	_('The job already ended'),
	_('No grabbed page yet - fetch one first'),
	_('The last grab found no media resources'),
	_('The last grab found no audio/video streams (use --all to include images too)'),
	_('Timed out after %ss'),
	_('HEAD refused, used a 1-byte ranged GET (%s)'),
	_('playlist download failed: %s'),
	_('usage: %s'),
	_('The page did not finish loading within %ss - showing what has rendered so far'),
	_('The page redirected to %s - the data below belongs to that page'),
	_("CSV ready"),
	_("columns"),
	_("rows"),
	_("Building the CSV…"),
	_("Make a CSV table from this JSON"),
	_("timeout"),
	_("Working"),
	_("Starting…"),
	_("v.qq.com: its public API only hands over one segment (%s) - for the WHOLE episode press the Tencent button in the media card (it asks /usr/bin/qqurl for the HLS playlist)."),
	_("The last grab was interrupted (its browser was cleaned up). Start it again."),
	_("v.qq.com: the site's own API answered (%s) but did not hand over a playable address. These videos usually need your login Cookie - paste your request headers and retry."),
	_("v.qq.com: the address comes from the site's own API (no browser needed). %s"),
	_("Time cap reached (%ss): the page kept the browser busy and could not be read. The grab was stopped and the browser cleaned up. Raise --max-total to wait longer, or use a lighter page."),
	_("This episode does not play on its own page (the site blocks that). It belongs to the album %s - grab the album page instead, and tick 'Walk every episode' to press each one."),
	_('This document is painted on a canvas - it has no per-page URL. %s page image(s) were exported and can be downloaded'),
	_('Unknown error, please check the log')
];
/* eslint-enable no-unused-vars */

/* ---- helpers -------------------------------------------------------------- */

/* v.qq.com 整集地址走独立 API（/usr/bin/qqurl）：它必须在真浏览器里听网络（~1 分钟） */
function qqexec(args) {
	return fs.exec(QQEXEC, args).then(function (res) {
		return { code: res.code, out: (res.stdout || '').trim(), err: (res.stderr || '').trim() };
	}).catch(function (e) {
		return { code: -1, out: '', err: String((e && e.message) || e) };
	});
}

function xexec(args) {
	return fs.exec(EXEC, args).then(function (res) {
		return { code: res.code, out: (res.stdout || '').trim(), err: (res.stderr || '').trim() };
	}).catch(function (e) {
		return { code: -1, out: '', err: String((e && e.message) || e) };
	});
}

function parseJSON(s, dflt) { try { return JSON.parse(s); } catch (e) { return dflt; } }

function T(s) { return (s === null || s === undefined || s === '') ? '' : _(String(s)); }

function fmtTpl(tpl, args) {
	var i = 0;
	return T(tpl).replace(/%s/g, function () {
		return (args && i < args.length) ? T(args[i++]) : '';
	});
}
/* a backend message object {error:"tpl", error_args:[...]} -> display string */
function msg(o, field) {
	if (!o || !o[field]) return '';
	return fmtTpl(o[field], o[field + '_args'] || []);
}

var URL_RE = new RegExp('^[a-zA-Z][a-zA-Z0-9+.-]*://');

function hostOf(u) {
	var m = String(u || '').match(/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\/([^/?#]+)/);
	return m ? m[1] : '';
}
function baseName(u) {
	var p = String(u || '').split(/[?#]/)[0].replace(/\/+$/, '');
	var a = p.split('/');
	var n = a[a.length - 1] || '';
	try { n = decodeURIComponent(n); } catch (e) { /* keep the raw name */ }
	return n || hostOf(u);
}
function fmtSize(b) {
	b = Number(b) || 0;
	if (b < 0) return '?';
	if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GB';
	if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
	if (b >= 1024) return (b / 1024).toFixed(0) + ' KB';
	return b + ' B';
}
function copyText(s, cb) {
	function fallback() {
		var ta = E('textarea', { 'style': 'position:fixed;top:-1000px;left:-1000px' }, [ s ]);
		document.body.appendChild(ta);
		ta.focus();
		ta.select();
		var okv = false;
		try { okv = document.execCommand('copy'); } catch (e) { okv = false; }
		document.body.removeChild(ta);
		if (cb) cb(okv);
	}
	if (window.navigator && navigator.clipboard && navigator.clipboard.writeText) {
		navigator.clipboard.writeText(s).then(function () { if (cb) cb(true); },
			function () { fallback(); });
	} else {
		fallback();
	}
}

function kindLabel(k) {
	if (k === 'hls') return 'HLS (m3u8)';
	if (k === 'dash') return 'DASH (mpd)';
	if (k === 'smooth') return 'SmoothStreaming';
	if (k === 'ts') return 'MPEG-TS';
	if (k === 'flash') return 'Flash';
	if (k === 'video') return _('Video file');
	if (k === 'audio') return _('Audio file');
	if (k === 'image') return _('Image file');
	if (k === 'subtitle') return _('Subtitle file');
	return k || '';
}
function sourceLabel(s) {
	if (s === 'network') return _('network request');
	if (s === 'dom') return _('page element');
	if (s === 'html') return _('page source');
	if (s === 'page') return _('page data');
	if (s === 'canvas') return _('page canvas');
	if (s === 'api') return _('interface response');
	return s || '';
}
function phaseText(p) {
	if (p === 'queued') return _('Queued');
	if (p === 'starting') return _('Starting the browser');
	if (p === 'loading') return _('Loading the page');
	if (p === 'collecting') return _('Collecting resources');
	if (p === 'screenshot') return _('Taking the screenshot');
	if (p === 'done') return _('Finished');
	if (p === 'error') return _('Error');
	if (p === 'stopped') return _('Stopped');
	return p || '';
}
/* signed / time limited url?  built from a string: LuCI's jsmin has truncated
   regex literals in this tree before, so no literal here */
var SIGNED_RE = new RegExp('[?&](deadline|expires?|token|signature|sign|auth_key|authkey|wssecret|ekey|wstime)=');
function looksSigned(u) { return SIGNED_RE.test(String(u || '')); }
function httpLabel(st) { return st ? ('HTTP ' + st) : ''; }

/* shell-quote a value for the copy-as-curl / copy-as-ffmpeg helpers */
function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }
function hdrLines(hs) {
	var out = [];
	Object.keys(hs).sort().forEach(function (k) { out.push(k + ': ' + hs[k]); });
	return out;
}
function curlCmd(u, hs) {
	var a = [ 'curl -L -o out.bin' ];
	hdrLines(hs).forEach(function (l) { a.push('-H ' + shq(l)); });
	a.push(shq(u));
	return a.join(' \\\n  ');
}
function ffmpegCmd(u, hs) {
	return 'ffmpeg -headers "' + hdrLines(hs).join('\\r\\n') + '\\r\\n" -i ' + shq(u) +
		' -c copy out.bin';
}

function stateLabel(s) {
	if (s === 'running') return _('Running');
	if (s === 'done') return _('Finished');
	if (s === 'error') return _('Error');
	if (s === 'stopped') return _('Stopped');
	return _('Idle');
}
/* the "real media" kinds: everything except plain images */
function isStreamKind(k) {
	return k === 'hls' || k === 'dash' || k === 'smooth' || k === 'ts' ||
		k === 'video' || k === 'audio' || k === 'flash' || k === 'subtitle';
}

return view.extend({

	load: function () {
		var lang = 'zh-cn';
		try {
			lang = String((navigator.language || navigator.userLanguage || 'zh-cn')).toLowerCase();
		} catch (e) {}
		return Promise.all([
			xexec([ 'status' ]),
			xexec([ 'job' ]),
			xexec([ 'i18n', lang ])
		]);
	},

	render: function (data) {
		/* the catalogue must be in place BEFORE any element is built */
		I18N = parseJSON((data[2] || {}).out, {}) || {};
		var st = parseJSON(data[0].out, null);
		var job = parseJSON(data[1].out, { state: 'idle' });
		var self = this;
		var media = [];          /* current media list (may be capped) */
		var lastQQ = '';         /* last whole-episode playlist URL from the Tencent API */
		var mediaCut = false;    /* backend had more items than it could return */
		var mediaTotal = 0;
		var poll = null;
		var pollBusy = false;     /* one job request at a time */
		var doneKey = '';         /* the job the result was rendered for */
		var hdrSets = [];         /* request-header sets of the last grab */
		var pageUrl = '';         /* url of the rendered result */
		var pollStart = 0;
		var lastJobElapsed = 0;

		var el = {};
		function inOpts(extra) {
			return Object.assign({ 'type': 'text', 'style': 'width:100%;box-sizing:border-box;padding:6px' }, extra || {});
		}

		/* ---- status card ---- */
		var stBody = E('div', { 'id': 'cg-status' }, [ E('span', { 'class': 'cg-dim' }, [ _('Reading status…') ]) ]);
		/* 🔴 the version shown is the one the BACKEND reports (i.e. what is
		 * installed).  The view script has its own stamped copy: if the browser
		 * is still running an old main.js these two differ, and that is exactly
		 * the "why does the page never update" trap - so say it out loud. */
		var stVer = E('span', { 'class': 'cg-ver' }, [ 'chromeui ' + CHROME_VER ]);
		function setVer(backend) {
			var js = String(CHROME_VER);
			var be = String(backend || '');
			stVer.textContent = 'chromeui ' + (be || js);
			if (be && js.indexOf('__PKG') !== 0 && js !== be) {
				stVer.textContent = 'chromeui ' + be + ' / ' + _('page script') + ' ' + js;
				stVer.className = 'cg-ver cg-err';
				stVer.title = _('The browser is still running an older page script - press Ctrl+Shift+R to reload it');
			}
		}
		var selfOut = E('pre', { 'class': 'cg-pre', 'id': 'cg-selftest', 'style': 'display:none' }, [ '' ]);
		var btnRefresh = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Refresh status') ]);
		var btnSelftest = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Self-test') ]);

		/* ---- grab card ---- */
		var urlIn = E('input', inOpts({ id: 'cg-url', placeholder: 'https://example.com/page' }));
		var waitIn = E('input', inOpts({ id: 'cg-wait', placeholder: 'body' }));
		var clickIn = E('input', inOpts({ id: 'cg-click', placeholder: '#jymain .sound-container xm-player div' }));
		/* 🔴 login-gated pages: paste the request headers your OWN (logged in)
		 * browser sends and the grab carries them on every request, so the site
		 * answers as that user.  DevTools -> Network -> the request that returns
		 * the resource -> Copy request headers / Copy as cURL. */
		var hdrCk = E('input', { 'type': 'checkbox', 'id': 'cg-hdrck' });
		var mineTa = E('textarea', { 'id': 'cg-mineta', 'class': 'cbi-input-textarea cg-hdrpaste',
			'placeholder': _('Paste here: DevTools -> Network -> the request that returns the resource -> Copy as cURL (or Copy request headers)'),
			'style': 'width:100%;box-sizing:border-box;font-family:monospace;font-size:11px;height:96px' });
		var hdrState = E('span', { 'class': 'cg-dim' }, []);
		var mineWrap = E('div', { 'id': 'cg-minewrap', 'style': 'display:none;margin-top:6px' }, [
			mineTa,
			E('div', { 'style': 'margin-top:4px' }, [ hdrState ])
		]);
		try {                                    /* survives a reload - pasting the
			* same block for every grab is the annoying part */
			mineTa.value = localStorage.getItem('cg.headers') || '';
		} catch (e) {}
		function updateHdrState() {
			var t = String(mineTa.value || ''), n = 0;
			t.split('\n').forEach(function (l) {
				if (/^\s*[^#\s:][^:]*:\s*\S/.test(l)) n++;
			});
			var isCurl = /(^|\s)curl\s/i.test(t);
			hdrState.textContent = n ? (n + ' ' + _('header(s) pasted') + (isCurl ? ' · cURL' : ''))
				: _('nothing pasted yet');
			return n;
		}
		mineTa.addEventListener('change', function () {
			try { localStorage.setItem('cg.headers', mineTa.value); } catch (e) {}
			updateHdrState();
		});
		hdrCk.addEventListener('click', function () {
			mineWrap.style.display = hdrCk.checked ? '' : 'none';
			if (hdrCk.checked) { try { mineTa.focus(); } catch (e) {} }
		});
		function mineText() { return String(mineTa.value || ''); }
		var toIn = E('input', inOpts({ id: 'cg-timeout', value: '30' }));
		var wIn = E('input', inOpts({ id: 'cg-width', value: '1280' }));
		var hIn = E('input', inOpts({ id: 'cg-height', value: '900' }));
		var fullCk = E('input', { 'type': 'checkbox' });
		var scrollCk = E('input', { 'type': 'checkbox', 'checked': 'checked' });
		var waitCk = E('input', { 'type': 'checkbox' });
		var htmlCk = E('input', { 'type': 'checkbox' });

		var btnGrab = E('button', { 'class': 'btn cbi-button cbi-button-apply cg-btn', 'id': 'cg-grab' }, [ _('Fetch page') ]);
		var btnStop = E('button', { 'class': 'btn cbi-button cg-btn', 'id': 'cg-stop', 'disabled': 'disabled' }, [ _('Abort') ]);
		var progLine = E('div', { 'class': 'cg-dim', 'id': 'cg-prog' }, [ '' ]);
		var banner = E('div', { 'class': 'cg-banner', 'id': 'cg-banner' }, [ _('No task yet.') ]);
		var notice = E('div', { 'class': 'cg-dim', 'id': 'cg-notice' }, [ '' ]);

		/* ---- screenshot ---- */
		var shotImg = E('img', { 'class': 'cg-shot', 'id': 'cg-shot', 'style': 'display:none' });
		var shotMeta = E('div', { 'class': 'cg-dim' }, [ _('No screenshot captured (tick the screenshot box before fetching).') ]);

		/* ---- text ---- */
		var textMeta = E('div', { 'class': 'cg-dim' }, [ '' ]);
		var textPre = E('pre', { 'class': 'cg-pre' }, [ '' ]);
		var btnCopyText = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Copy all text') ]);

		/* ---- media ---- */
		var mediaMeta = E('div', { 'class': 'cg-dim' }, [ _('No resources yet.') ]);
		var filterSel = E('select', { 'style': 'width:100%;box-sizing:border-box;padding:6px' }, [
			E('option', { value: 'stream' }, [ _('Audio / video streams') ]),
			E('option', { value: 'all' }, [ _('Everything') ]),
			E('option', { value: 'image' }, [ _('Images only') ]),
			E('option', { value: 'subtitle' }, [ _('Subtitles only') ])
		]);
		var mediaBox = E('div', { 'id': 'cg-media' }, [ E('span', { 'class': 'cg-dim' }, [ _('No resources yet.') ]) ]);
		/* ---- request headers of the page ---- */
		var hdrBox = E('div', { 'id': 'cg-hdrs' }, [ E('span', { 'class': 'cg-dim' }, [ _('No headers yet.') ]) ]);
		var apiBox = E('div', { 'id': 'cg-apis' }, []);
		var apiCalls = [];
		var btnHdrCopy = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Copy all headers') ]);
		var btnHdrDl = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Download headers.txt') ]);

		function headerText() {
			var out = [ '# request headers seen by the headless browser',
				'# page: ' + pageUrl,
				'# ' + new Date().toISOString(), '' ];
			var used = {};
			media.forEach(function (m) {
				if (m.hdr === undefined || !hdrSets[m.hdr]) return;
				if (used[m.hdr]) return;
				used[m.hdr] = 1;
				out.push('# ' + m.url);
				hdrLines(hdrSets[m.hdr]).forEach(function (l) { out.push(l); });
				out.push('');
			});
			if (!Object.keys(used).length) out.push('# (none captured)');
			return out.join('\n');
		}
		btnHdrCopy.addEventListener('click', function () {
			copyText(headerText(), function (o) {
				setNotice(o ? _('Headers copied.') : _('Copy failed - select the text manually.'), !o);
			});
		});
		btnHdrDl.addEventListener('click', function () {
			try {
				var blob = new Blob([ headerText() ], { type: 'text/plain;charset=utf-8' });
				var u = URL.createObjectURL(blob);
				var a = document.createElement('a');
				a.href = u;
				a.download = 'headers.txt';
				document.body.appendChild(a);
				a.click();
				setTimeout(function () { document.body.removeChild(a); URL.revokeObjectURL(u); }, 4000);
				setNotice(_('Download started') + ': headers.txt', false);
			} catch (e) { setNotice(_('Download failed') + ': ' + e, true); }
		});

		/* ---- fetch a direct file/api URL with request headers ------------------
		 * NOTE: this downloads ONE address as-is.  If the answer is a page (HTML),
		 * the address is a page, not the resource - for those use the grab with
		 * "Paste my own request headers" ticked instead, which parses the page and
		 * hands you the resource URLs. */
		var furlIn = E('input', inOpts({ id: 'cg-furl', placeholder: 'https://…/file.mp4  (' + _('a direct file/api URL, not a page') + ')' }));
		var btnFetch = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Fetch this address') ]);
		var btnFetchDl = E('button', { 'class': 'btn cbi-button cg-btn', 'style': 'display:none' },
			[ _('Download to computer') ]);
		var fetchOut = E('div', { 'id': 'cg-fetchout', 'class': 'cg-dim' }, [ '' ]);
		var fetchHint = E('div', { 'class': 'cg-dim' }, [ '' ]);
		var lastFetchFile = '';

		function doFetch(u, hdrText) {
			/* hdrText given  -> this row's own captured headers (per resource)
			   otherwise       -> the pasted login headers, else the captured set */
			var hdrs = (hdrText !== undefined) ? hdrText
				: ((hdrCk.checked && mineText().trim()) ? mineText() : (hdrSets.length ? hdrLines(hdrSets[0]).join('\n') : ''));
			if (u) furlIn.value = u;
			var url = String(furlIn.value || '').trim();
			var isCurl = /(^|\s)curl\s/i.test(mineText());
			if (!url && isCurl) url = '-';    /* backend takes it from the curl paste */
			if (!url) {
				setNotice(_('Enter a URL, or paste a "Copy as cURL" command (its URL will be used)'), true);
				return;
			}
			var curlNote = isCurl ? (' (' + _('using the URL/method/body from the pasted cURL') + ')') : '';
			btnFetch.disabled = true;
			fetchOut.textContent = _('Fetching…');
			fetchHint.textContent = _('headers used') + ': ' + (hdrs ? (hdrs.split('\n').length + ' ' + _('lines')) : '-');
			xexec([ 'fetch', url, '--out', '/tmp/chromeui/fetched.bin', '--headers', hdrs ])
				.then(function (r) {
					btnFetch.disabled = false;
					var f = parseJSON(r.out, null);
					if (!f || f.ok === false) {
						fetchOut.textContent = f ? msg(f, 'error') : (r.err || _('Unknown error, please check the log'));
						fetchOut.className = 'cg-err';
						setNotice(fetchOut.textContent, true);
						return;
					}
					lastFetchFile = f.file || '';
					fetchOut.className = 'cg-dim';
					fetchOut.innerHTML = '';
					/* what came back, what we sent, and - when it failed - why */
					fetchOut.appendChild(E('div', {}, [
						'HTTP ' + (f.status || '?') + ' · ' + (f.content_type || '-') + ' · ' + fmtSize(f.bytes) +
						(f.stopped_early ? ' (' + _('partial') + ')' : '') +
						(f.method && f.method !== 'GET' ? (' · ' + f.method) : '') +
						(f.file ? ('  →  /tmp/chromeui/' + f.file) : (f.path ? ('  →  ' + f.path) : '')) +
						curlNote ]));
					fetchOut.appendChild(E('div', { 'class': 'cg-dim' }, [
						_('Headers sent') + ': ' + (f.header_count || 0) + '  (' +
						((f.headers_sent || []).join(', ') || '-') + ')' ]));
					if (f.cookies_set && f.cookies_set.length) {
						fetchOut.appendChild(E('div', { 'class': 'cg-dim' }, [
							_('cookies set by the server') + ': ' + f.cookies_set.join(', ') ]));
					}
					if (f.chain && f.chain.length) {
						fetchOut.appendChild(E('div', { 'class': 'cg-dim' }, [ _('Request chain') ]));
						f.chain.forEach(function (c) {
							fetchOut.appendChild(E('div', { 'class': 'cg-dim' }, [
								'  ' + c.status + '  ' + c.from + '  →  ' + c.to ]));
						});
					}
					/* 🔴 纯 JSON 的返回 -> 一键生成 CSV 表格（列=键，行=记录；带 BOM，Excel 直开） */
					var btnCsv = E('button', { 'class': 'btn cbi-button cg-btn' },
						[ _('Make a CSV table from this JSON') ]);
					var csvOut = E('div', { 'class': 'cg-dim' }, []);
					btnCsv.addEventListener('click', function () {
						btnCsv.disabled = true;
						csvOut.className = 'cg-dim';
						csvOut.textContent = _('Building the CSV…');
						xexec([ 'csv', url, '--out', '/tmp/chromeui/data.csv', '--headers', hdrs ])
							.then(function (rr) {
								btnCsv.disabled = false;
								var d = parseJSON(rr.out, null);
								if (!d || !d.ok) {
									var why = d ? (d.error || '') : (rr.err || _('Unknown error, please check the log'));
									csvOut.textContent = why;
									csvOut.className = 'cg-err';
									setNotice(String(why).slice(0, 90), true);
									return;
								}
								csvOut.className = 'cg-dim';
								csvOut.textContent = d.rows + ' ' + _('rows') + ' × ' + d.cols + ' ' + _('columns') +
									': ' + (d.columns || []).join(', ');
								setNotice(_('CSV ready') + ': ' + d.rows + ' ' + _('rows') + ' × ' + d.cols + ' ' +
									_('columns'), false);
								downloadFromDevice(d.file, 'data.csv', 'text/csv');
							});
					});
					fetchOut.appendChild(E('div', { 'style': 'margin-top:6px' }, [ btnCsv ]));
					fetchOut.appendChild(csvOut);
					if (f.final_url && f.final_url !== f.url) {
						fetchOut.appendChild(E('div', { 'class': 'cg-dim' }, [
							_('redirected to') + ' ' + f.final_url ]));
					}
					if (f.looks_login) {
						fetchOut.appendChild(E('div', { 'class': 'cg-err' }, [
							_('The server answered with a login / no-permission page - the cookie is probably expired, or a needed header is missing') ]));
					}
					if (f.preview && (f.looks_login || f.bytes < 4000)) {
						fetchOut.appendChild(E('div', { 'class': 'cg-dim' }, [ _('Response preview') ]));
						fetchOut.appendChild(E('pre', { 'class': 'cg-hdrs' }, [ String(f.preview).slice(0, 600) ]));
					}
					if (lastFetchFile) btnFetchDl.style.display = '';
					setNotice(_('Fetched') + ': HTTP ' + (f.status || '?') + ' · ' + fmtSize(f.bytes) +
						' · ' + _('Headers sent') + ' ' + (f.header_count || 0), !!f.looks_login);
				});
		}
		btnFetch.addEventListener('click', function () { doFetch(); });
		btnFetchDl.addEventListener('click', function () {
			if (!lastFetchFile) return;
			var nm = 'fetched.bin';
			try {
				var p = String(furlIn.value || '').split('?')[0].split('/');
				var b = decodeURIComponent(p[p.length - 1] || '') || 'fetched';
				nm = b.replace(/[\\/:*?"<>|\s]+/g, '_').slice(0, 60);
			} catch (e) {}
			downloadFromDevice(lastFetchFile, nm);
		});

		/* the XHR/fetch calls the page made: that is where a login-gated page gets
		 * its real media address from */
		function renderApis() {
			apiBox.innerHTML = '';
			if (!apiCalls.length) return;
			apiBox.appendChild(E('div', { 'class': 'cg-dim', 'style': 'margin-top:8px' }, [
				_('Interfaces the page called') + ' (' + apiCalls.length + ')' ]));
			apiCalls.forEach(function (u) {
				apiBox.appendChild(E('div', { 'class': 'cg-dim cg-clip' }, [ u ]));
			});
		}


		function renderHdrs() {
			hdrBox.innerHTML = '';
			if (!hdrSets.length) {
				hdrBox.appendChild(E('span', { 'class': 'cg-dim' }, [ _('No request headers were captured for this page.') ]));
				return;
			}

			hdrSets.forEach(function (hs, i) {
				var users = media.filter(function (m) { return m.hdr === i; });
				hdrBox.appendChild(E('div', { 'class': 'cg-dim', 'style': 'margin-top:4px' }, [
					_('Header set') + ' ' + (i + 1) + ' - ' + users.length + ' ' + _('request(s)')
				]));
				hdrBox.appendChild(E('pre', { 'class': 'cg-hdrs' }, [ hdrLines(hs).join('\n') ]));
			});
		}
		var m3uOutIn = E('input', inOpts({ value: '/tmp/chromeui/playlist.m3u' }));
		var btnCopyAll = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Copy all URLs') ]);
		var btnExport = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Save .m3u on the device') ]);
		var btnDl = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Download .m3u to this computer') ]);
		/* the request headers the browser sent for these resources - the tokens and
		   cookies the page's own JS produced, which is what makes such an address
		   usable elsewhere */
		var btnHdrAll = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Copy all request headers') ]);
		var btnHdrDl = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Export request headers (HEADERS.TXT)') ]);
		/* v.qq.com: 整集地址（HLS 清单）只能靠一次真浏览器监听拿到 -> 独立 API */
		var btnQQ = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Tencent: get the whole-episode URL') ]);
		var btnQQCopy = E('button', { 'class': 'btn cbi-button cg-btn', 'style': 'display:none' }, [ _('Copy') ]);
		var btnQQDl = E('button', { 'class': 'btn cbi-button cg-btn', 'style': 'display:none' }, [ _('Download to computer') ]);
		var qqOut = E('div', { 'class': 'cg-dim cg-clip', 'id': 'cg-qqout' }, [ _('Tencent (v.qq.com) only: press the button above to ask the API for the whole-episode playlist URL.') ]);

		/* every captured header set as one pasteable block */
		function allHdrText() {
			var out = [];
			hdrSets.forEach(function (hs, i) {
				out.push('# --- set ' + (i + 1) + ' ---');
				hdrLines(hs).forEach(function (l) { out.push(l); });
			});
			return out.join('\n');
		}
		btnQQ.addEventListener('click', function () {
			var u = String(urlIn.value || '').trim();
			if (!/v\.qq\.com/.test(u)) { setNotice(_('Put a v.qq.com page URL in the box above first.'), true); return; }
			btnQQ.disabled = true;
			qqOut.textContent = _('Starting…');
			btnQQCopy.style.display = 'none';
			btnQQDl.style.display = 'none';
			/* 🔴 不能同步等：这个 API 要开真浏览器听 1~4 分钟，LuCI 的 XHR 必超时。
			   照抓取流程的做法：--start 立刻返回，然后轮询 --job（每次读一个小文件）。 */
			qqexec([ '--start', u ]).then(function (rr) {
				var d = parseJSON(rr.out, null);
				if (!d || !d.ok) {
					btnQQ.disabled = false;
					var why = d ? (msg(d, 'error') || d.error || '') : (rr.err || '');
					qqOut.textContent = _('Tencent API failed') + ': ' + String(why).slice(0, 160);
					setNotice(_('Tencent API failed') + ': ' + String(why).slice(0, 90), true);
					return;
				}
				qqPoll(0);
			});
		});
		function qqPoll(n) {
			if (n > 220) {                      /* ~11 分钟上限 */
				btnQQ.disabled = false;
				qqOut.textContent = _('Tencent API failed') + ': ' + _('timeout');
				setNotice(_('Tencent API failed') + ': ' + _('timeout'), true);
				return;
			}
			qqexec([ '--job' ]).then(function (rr) {
				var j = parseJSON(rr.out, null) || { 'state': 'idle' };
				if (j.state === 'running') {
					qqOut.textContent = _('Working') + '… ' + (j.phase || '');
					window.setTimeout(function () { qqPoll(n + 1); }, 3000);
					return;
				}
				btnQQ.disabled = false;
				if (j.state === 'done' && j.playlist) {
					lastQQ = j.playlist;
					qqOut.textContent = j.playlist;
					btnQQCopy.style.display = '';
					btnQQDl.style.display = '';
					setNotice(_('Whole-episode playlist') + ': ' + j.segments + ' ' + _('segments') +
						', ' + j.duration_s + ' s' + (j.cached ? ' (' + _('cached') + ')' : ''),
						false);
				} else {
					var why2 = j.error || j.state || '?';
					qqOut.textContent = _('Tencent API failed') + ': ' + String(why2).slice(0, 180);
					setNotice(_('Tencent API failed') + ': ' + String(why2).slice(0, 90), true);
				}
			});
		}
		btnQQCopy.addEventListener('click', function () {
			if (!lastQQ) return;
			copyText(lastQQ, function (okv) {
				setNotice(okv ? _('Playlist URL copied.') : _('Copy failed - select the text manually.'), !okv);
			});
		});
		btnQQDl.addEventListener('click', function () {
			if (!lastQQ) return;
			btnQQDl.disabled = true;
			setNotice(_('Fetching the playlist…'), false);
			xexec([ 'fetch', lastQQ, '--out', '/tmp/chromeui/qq_ep.m3u8' ]).then(function (rr) {
				btnQQDl.disabled = false;
				var f = parseJSON(rr.out, null);
				if (!f || f.error) {
					setNotice(f ? msg(f, 'error') : (rr.err || _('Unknown error, please check the log')), true);
					return;
				}
				downloadFromDevice(f.file || 'qq_ep.m3u8', 'qq_ep.m3u8');
				setNotice('HTTP ' + (f.status || '?') + ' · ' + (f.bytes || 0) + ' ' + _('bytes'), false);
			});
		});

		btnHdrAll.addEventListener('click', function () {
			if (!hdrSets.length) { setNotice(_('No request headers were captured for this page.'), true); return; }
			copyText(allHdrText(), function (okv) {
				setNotice(okv ? _('All request headers copied.') : _('Copy failed - select the text manually.'), !okv);
			});
		});
		btnHdrDl.addEventListener('click', function () {
			if (!hdrSets.length) { setNotice(_('No request headers were captured for this page.'), true); return; }
			btnHdrDl.disabled = true;
			xexec([ 'export', 'headers', '--out', '/tmp/chromeui/headers.txt' ]).then(function (rr) {
				btnHdrDl.disabled = false;
				var f = parseJSON(rr.out, null);
				if (!f || f.error) {
					setNotice(f ? msg(f, 'error') : (rr.err || _('Unknown error, please check the log')), true);
					return;
				}
				downloadFromDevice(f.file || 'headers.txt', 'headers.txt');
				setNotice(_('HEADERS.TXT downloaded') + ' (' + f.sets + ' ' + _('header set(s)') + ')', false);
			});
		});

		/* ---- html ---- */
		var htmlMeta = E('div', { 'class': 'cg-dim' }, [ '' ]);
		var htmlPre = E('pre', { 'class': 'cg-pre' }, [ '' ]);
		var btnCopyHtml = E('button', { 'class': 'btn cbi-button cg-btn' }, [ _('Copy HTML') ]);

		function setNotice(t, bad) {
			notice.textContent = t || '';
			notice.setAttribute('class', bad ? 'cg-err' : 'cg-ok');
		}

		/* ------------------------------------------------------------------ */
		/* status                                                             */
		/* ------------------------------------------------------------------ */
		function renderStatus() {
			stBody.innerHTML = '';
			if (!st) {
				stBody.appendChild(E('span', { 'class': 'cg-err' }, [ _('Cannot read the status (is chromeui installed?)') ]));
				return;
			}
			setVer(st.version);
			var tbl = E('table', { 'class': 'cg-table' });
			(st.components || []).forEach(function (c) {
				var okc = c.installed || c.file_ok;
				var tr = E('tr', {});
				tr.appendChild(E('td', { 'class': okc ? 'cg-ok' : 'cg-err' }, [ okc ? '✔' : '✘' ]));
				tr.appendChild(E('td', {}, [ c.name ]));
				tr.appendChild(E('td', { 'class': 'cg-clip' }, [ c.opkg_version || '-' ]));
				tr.appendChild(E('td', { 'class': 'cg-dim cg-clip' }, [ c.path ]));
				tbl.appendChild(tr);
			});
			stBody.appendChild(tbl);

			var m = st.mounts || {};
			var mline = [ 'dev', 'proc', 'sys', 'tmp' ].map(function (k) {
				return E('span', { 'class': m[k] ? 'cg-ok' : 'cg-warn' }, [ k + (m[k] ? ' ✔' : ' ✘') ]);
			});
			var d = st.disk;
			stBody.appendChild(E('div', { 'style': 'margin-top:6px' }, [
				_('Bind mounts') + ': ', E('span', {}, mline),
				E('span', { 'class': 'cg-dim' }, [ '   ' + _('Disk') + ': ' +
					(d ? (fmtSize(d.used) + ' / ' + fmtSize(d.total) + ' (' + d.use_pct + '%)') : '-') ]),
				E('span', { 'class': st.driver_listening ? 'cg-ok' : 'cg-dim' }, [ '   ' +
					_('Chromedriver port 9515') + ': ' + (st.driver_listening ? _('listening') : _('not listening')) ]),
				E('span', { 'class': 'cg-dim' }, [ '   selenium: ' + (st.selenium || '-') ])
			]));
			if ((st.components || []).some(function (c) { return !(c.installed || c.file_ok); })) {
				stBody.appendChild(E('div', { 'class': 'cg-warn' }, [ _('Some components are missing - install chrome-headless-shell / chromedriver / chrome-glibc-runtime.') ]));
			}
		}

		btnRefresh.addEventListener('click', function () {
			btnRefresh.disabled = true;
			xexec([ 'status' ]).then(function (r) {
				btnRefresh.disabled = false;
				st = parseJSON(r.out, st);
				renderStatus();
			});
		});
		btnSelftest.addEventListener('click', function () {
			btnSelftest.disabled = true;
			selfOut.style.display = '';
			selfOut.textContent = _('Running…');
			xexec([ 'selftest' ]).then(function (r) {
				btnSelftest.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j) {
					selfOut.textContent = r.err || _('Unknown error, please check the log');
					return;
				}
				selfOut.textContent = (j.steps || []).map(function (s) {
					return (s.code === 0 ? '✔' : '✘') + ' ' + s.name + ': ' +
						(s.out || s.error || '') + (s.ms ? '  (' + s.ms + ' ms)' : '');
				}).join('\n') + '\n' + (j.pass ? _('All components responded.') : _('Component self-test failed.'));
			});
		});

		/* ------------------------------------------------------------------ */
		/* results                                                           */
		/* ------------------------------------------------------------------ */
		function renderText(t, r) {
			t = t || '';
			r = r || {};
			var bits = [ t.length ? fmtTpl(_('%s characters'), [ String(r.text_len || t.length) ]) : _('No text.') ];
			if (r.text_cut) bits.push(_('truncated') + ' · ' + _('full text on the device') + ': ' + (r.text_path || ''));
			textMeta.textContent = bits.join('  ·  ');
			textPre.textContent = t.length > 200000 ? t.substring(0, 200000) : t;
		}

		/* The screenshot, the full text and the rendered HTML live as files on
		 * the device: a ubus reply is capped (131 KB ok / 262 KB rejected, see
		 * the backend), so they are read back in chunks and glued together
		 * here instead of travelling inside one JSON blob. */
		function readChunks(name, asB64, onData, onDone) {
			var off = 0, guard = 0, acc = '';
			function step() {
				if (guard++ > 400) { onDone(null, _('The file is too large to read.')); return; }
				var args = [ 'filedata', '--file', name, '--offset', String(off) ];
				if (asB64) args.push('--b64');
				xexec(args).then(function (r) {
					var j = parseJSON(r.out, null);
					if (!j || (asB64 ? j.b64 === undefined : j.text === undefined)) {
						onDone(null, (j ? msg(j, 'error') : (r.err || _('Unknown error, please check the log'))));
						return;
					}
					acc += (asB64 ? j.b64 : j.text);
					off = j.next;
					if (onData) onData(acc, j);
					if (j.done) { onDone(acc, ''); return; }
					step();
				});
			}
			step();
		}

		function renderShot(s) {
			s = s || {};
			shotImg.style.display = 'none';
			shotImg.removeAttribute('src');
			if (!s.path) {
				shotMeta.textContent = _('No screenshot captured (tick the screenshot box before fetching).');
				return;
			}
			var bits = [ _('Saved on the device') + ': ' + s.path, fmtSize(s.bytes) ];
			if (s.error) bits.push(_('Error') + ': ' + s.error);
			shotMeta.textContent = bits.join('  ·  ');
			if (!s.name || s.error) return;
			shotMeta.textContent = _('Loading the screenshot…') + '  ·  ' + bits.join('  ·  ');
			readChunks(s.name, true, null, function (b64, err) {
				if (err) {
					shotMeta.textContent = _('Cannot display the screenshot') + ': ' + err + '  ·  ' + bits.join('  ·  ');
					return;
				}
				shotImg.setAttribute('src', 'data:image/png;base64,' + b64);
				shotImg.style.display = 'block';
				shotMeta.textContent = bits.join('  ·  ');
			});
		}

		function filtered() {
			var f = filterSel.value;
			if (f === 'all') return media;
			if (f === 'image') return media.filter(function (m) { return m.kind === 'image'; });
			if (f === 'subtitle') return media.filter(function (m) { return m.kind === 'subtitle'; });
			/* canvas page exports have no URL but they ARE the document content -
			 * keep them visible in the default view instead of hiding them behind
			 * the image filter */
			return media.filter(function (m) { return m.file || isStreamKind(m.kind); });
		}

		function detailRow(row, list) {
			var td = E('div', { 'class': 'cg-mdetail' }, [ _('Analyzing…') ]);
			row.appendChild(td);
			var first = true;
			list.forEach(function (m) {
				xexec([ 'mediaprobe', m.url ]).then(function (r) {
					var j = parseJSON(r.out, null);
					var box = E('div', { 'style': 'margin-bottom:8px' });
					/* drop the "analyzing…" placeholder once real output arrives */
					if (first) { td.textContent = ''; first = false; }
					if (!j) {
						box.appendChild(E('div', { 'class': 'cg-err' }, [
							msg(null, 'error') || (r.err || _('Unknown error, please check the log')) ]));
						td.appendChild(box);
							return;
					}
					/* the full url WRAPS here (overflow-wrap) instead of widening
					 * the page - 700 char CDN urls are normal on video sites */
					box.appendChild(E('div', {}, [ E('b', {}, [ m.url ]) ]));
					var bits = [ kindLabel(j.kind || m.kind), (j.content_type || '-') ];
					bits.push(j.size >= 0 ? fmtSize(j.size) : _('size unknown'));
					if (j.status) bits.push(httpLabel(j.status));
					if (j.codec_name) bits.push(j.codec_name + (j.codec ? (' (' + j.codec + ')') : ''));
					box.appendChild(E('div', { 'class': 'cg-dim' }, [ bits.join('  ·  ') ]));
					var hints = [];
					if (j.status === 206) hints.push(_('HTTP 206 just means Chrome fetched it in ranges - the URL itself is fine'));
					if (j.needs_referer) hints.push(_('Needs the page as Referer - without it the CDN answers 403, so a plain browser window cannot open it')
						+ (j.referer ? ('  (' + _('send') + ' Referer: ' + j.referer + ')') : ''));
					else if (j.referer && j.status) hints.push(_('Referer used') + ': ' + j.referer);
					if (j.status_no_referer) hints.push(_('Without Referer') + ': HTTP ' + j.status_no_referer);
					if (j.picky) hints.push(_('This track uses a codec many players cannot decode (AV1/HEVC/Opus) - it may refuse to open'));
					if (j.expires) hints.push(_('Signed URL, valid until') + ' ' + new Date(j.expires * 1000).toLocaleString());
					else if (looksSigned(m.url)) hints.push(_('Carries authentication parameters - it may expire'));
					if (/\.m4s($|\?)/.test(m.url)) hints.push(_('This is a DASH/fMP4 segment (a video-only or audio-only track), not a complete file'));
					hints.forEach(function (h) { box.appendChild(E('div', { 'class': 'cg-warn' }, [ h ])); });
					/* the request headers this resource was fetched with: the
					 * tokens/cookies the page's own JS produced, which is what
					 * makes such an address usable elsewhere */
					var hs = (m.hdr === undefined) ? null : hdrSets[m.hdr];
					if (hs) {
						var lines = hdrLines(hs);
						box.appendChild(E('div', { 'class': 'cg-dim', 'style': 'margin-top:4px' }, [
							_('Request headers actually sent (tokens/cookies come from the page JS)') ]));
						box.appendChild(E('pre', { 'class': 'cg-hdrs' }, [ lines.join('\n') ]));
						var bH = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Copy headers') ]);
						var bCurl = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Copy as curl') ]);
						var bFf = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Copy as ffmpeg') ]);
						var bDl = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Download with these headers') ]);
						function cp(txt, okmsg) {
							copyText(txt, function (okv) {
								setNotice(okv ? okmsg : _('Copy failed - select the text manually.'), !okv);
							});
						}
						bH.addEventListener('click', function () { cp(lines.join('\n'), _('Headers copied.')); });
						bCurl.addEventListener('click', function () { cp(curlCmd(m.url, hs), _('curl command copied.')); });
						bFf.addEventListener('click', function () { cp(ffmpegCmd(m.url, hs), _('ffmpeg command copied.')); });
						bDl.addEventListener('click', function () {
							/* download exactly this address with exactly these headers */
							bDl.disabled = true;
							setNotice(_('Fetching with these headers…'), false);
							xexec([ 'fetch', m.url, '--out', '/tmp/chromeui/fetched.bin',
								'--headers', lines.join('\n') ]).then(function (rr) {
								bDl.disabled = false;
								var f = parseJSON(rr.out, null);
								if (!f || f.error) {
									setNotice(f ? msg(f, 'error') : (rr.err || _('Unknown error, please check the log')), true);
									return;
								}
								if (f.file) downloadFromDevice(f.file, baseName(m.url) || 'fetched.bin');
								setNotice('HTTP ' + (f.status || '?') + ' · ' + (f.content_type || '-') +
									' · ' + fmtSize(f.bytes), false);
							});
						});
						box.appendChild(E('div', { 'style': 'margin-top:4px' }, [ bH, bCurl, bFf, bDl ]));
					}
					if (j.note) box.appendChild(E('div', { 'class': 'cg-dim' }, [ j.note ]));
					var pl = j.playlist;
					if (pl) {
						box.appendChild(E('div', {}, [
							_('Playlist type') + ': ' + (pl.type === 'master' ? _('master (variant streams)') : _('media (segments)')) +
							'  ·  ' + _('segments') + ': ' + pl.segments +
							'  ·  ' + _('duration') + ': ' + (Number(pl.duration) || 0).toFixed(1) + ' s' +
							(pl.live ? ('  ·  ' + _('live')) : '') +
							(pl.encryption ? ('  ·  ' + _('encrypted') + ': ' + pl.encryption) : '')
						]));
						(pl.variants || []).forEach(function (v) {
							box.appendChild(E('div', { 'class': 'cg-dim' }, [
								(v.resolution || '-') + '  ·  ' +
								(v.bandwidth ? (Math.round(v.bandwidth / 1000) + ' kbps  ·  ') : '') + v.uri ]));
						});
						if ((pl.first_segments || []).length) {
							box.appendChild(E('div', { 'class': 'cg-dim' }, [ E('b', {}, [ _('First segments') ]) ]));
							pl.first_segments.forEach(function (u) {
								box.appendChild(E('div', { 'class': 'cg-dim' }, [ u ]));
							});
						}
					}
					if (m.url && (m.url.split('?')[0].substr(-5) === '.m3u8') && !pl && !j.note) {
						box.appendChild(E('div', { 'class': 'cg-dim' }, [ _('No playlist could be downloaded (it may need a referer/token).') ]));
					}
					td.appendChild(box);
				});
			});
		}

		function renderMedia() {
			mediaBox.innerHTML = '';
			if (!media.length) {
				mediaBox.appendChild(E('span', { 'class': 'cg-dim' }, [ _('No resources yet.') ]));
				mediaMeta.textContent = _('No resources yet.');
				return;
			}
			var counts = {};
			media.forEach(function (m) { counts[m.kind] = (counts[m.kind] || 0) + 1; });
			var parts = [];
			Object.keys(counts).forEach(function (k) { parts.push(kindLabel(k) + ' ' + counts[k]); });
			mediaMeta.textContent = fmtTpl(_('%s resources in total'), [ String(mediaTotal || media.length) ]) + ': ' + parts.join('  ·  ') +
				(mediaCut ? ('  (' + fmtTpl(_('showing the first %s'), [ String(media.length) ]) + ')') : '');

			var list = filtered();
			if (!list.length) {
				mediaBox.appendChild(E('span', { 'class': 'cg-dim' }, [ _('Nothing matches the current filter.') ]));
				return;
			}
			var listBox = E('div', { 'class': 'cg-mlist' });
			list.forEach(function (m) {
				var row = E('div', { 'class': 'cg-mrow' });
				row.appendChild(E('div', { 'class': 'cg-mkind' }, [ kindLabel(m.kind) ]));
				var maind = E('div', { 'class': 'cg-mmain' });
				maind.appendChild(E('div', { 'class': 'cg-mname' }, [ m.title || baseName(m.url) || hostOf(m.url) ]));
				var meta = [];
				if (m.mime) meta.push(m.mime);
				if (m.source) meta.push(sourceLabel(m.source));
				if (m.status) meta.push(httpLabel(m.status));
				if (m.tag) meta.push('<' + String(m.tag).toLowerCase() + '>');
				if (looksSigned(m.url)) meta.push(_('signed / temporary'));
				if (m.codec_name) meta.push(m.codec_name + (m.codec ? (' (' + m.codec + ')') : ''));
				if (m.hdr !== undefined && hdrSets[m.hdr]) meta.push(_('with headers'));
				maind.appendChild(E('div', { 'class': 'cg-dim' }, [ meta.join(' · ') ]));
				/* a video track the player cannot decode is the number one reason
				 * an extracted url "does not open" - say it up front */
				if (m.picky) maind.appendChild(E('div', { 'class': 'cg-warn' }, [
					_('This track uses a codec many players cannot decode (AV1/HEVC/Opus) - it may refuse to open') ]));
				if (String(m.url || '').indexOf('canvas:') !== 0) {
					maind.appendChild(E('div', { 'class': 'cg-dim cg-clip' }, [ m.url ]));
				}
				row.appendChild(maind);
				var acts = E('div', { 'class': 'cg-macts' });
				var btnP = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Analyze URL') ]);
				var btnC = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Copy URL') ]);
				/* a canvas page has no URL - it is a PNG on the device instead, so
				 * offer both ways out: leave it on the device, or pull it down */
				var btnD = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Download to computer') ]);
				var btnKeep = E('button', { 'class': 'btn cbi-button cg-mini' }, [ _('Keep on device') ]);
				btnC.addEventListener('click', function () {
					copyText(m.url, function (okv) {
						setNotice(okv ? _('URL copied.') : _('Copy failed - select the text manually.'), !okv);
					});
				});
				btnP.addEventListener('click', function () {
					btnP.disabled = true;
					detailRow(row, [ m ]);
				});
				if (m.file) {
					btnKeep.addEventListener('click', function () {
						var p = '/tmp/chromeui/' + m.file;
						copyText(p, function (ok) {
							setNotice((ok ? _('Stored on the device') : _('On the device')) + ': ' + p, false);
						});
					});
					btnD.addEventListener('click', function () {
						downloadFromDevice(m.file, (m.title ? (m.title.replace(/[\\/:*?"<>|\s]+/g, '_') + '.png') : m.file));
					});
					acts.appendChild(btnKeep);
					acts.appendChild(btnD);
				} else {
					acts.appendChild(btnP);
					acts.appendChild(btnC);
				}
				row.appendChild(acts);
				listBox.appendChild(row);
			});
			mediaBox.appendChild(listBox);
		}

		function renderHtml(r) {
			r = r || {};
			htmlPre.textContent = '';
			if (!r.html_len) {
				htmlMeta.textContent = _('Not fetched (tick "HTML source" before fetching).');
				return;
			}
			htmlMeta.textContent = fmtTpl(_('%s characters'), [ String(r.html_len) ]) + '  ·  ' +
				_('full HTML on the device') + ': ' + (r.html_path || '');
			/* preview: the first chunk only - the rest stays on the device and
			 * is available through Copy HTML / the path above */
			xexec([ 'filedata', '--file', 'last.html', '--offset', '0', '--len', '8000' ]).then(function (rr) {
				var j = parseJSON(rr.out, null);
				if (!j || j.text === undefined) return;
				htmlPre.textContent = j.text + (j.done ? '' : '\n… ' + _('truncated'));
			});
		}

		function renderResult(j) {
			banner.innerHTML = '';
			var s = (j && j.state) || 'idle';
			banner.setAttribute('class', 'cg-banner');
			if (s === 'running') {
				banner.appendChild(E('span', {}, [ stateLabel(s) ]));
				progLine.textContent = phaseText(j.phase) + '  ·  ' + _('elapsed') + ' ' +
					(Number(j.elapsed) || 0).toFixed(1) + ' s';
				btnGrab.disabled = true;
				btnStop.disabled = false;
				return;
			}
			progLine.textContent = '';
			btnGrab.disabled = false;
			btnStop.disabled = true;
			if (s === 'done') {
				/* `job` deliberately carries no result (it is polled, and a ubus
				 * reply is capped) - pull the reduced view separately, and only
				 * ONCE per job (a late in-flight poll must not re-render the
				 * list and wipe an opened 解析 detail) */
				if (j.result_ready) {
					var key = String(j.url || '') + '|' + String(j.elapsed || '') + '|' + String(j.phase || '');
					if (key === doneKey) return;
					doneKey = key;
					loadResult();
					return;
				}
				banner.appendChild(E('span', { 'class': 'cg-dim' }, [ stateLabel(s) ]));
				return;
			}
			if (s === 'error') {
				banner.setAttribute('class', 'cg-banner cg-banner-bad');
				banner.appendChild(E('span', { 'class': 'cg-err' }, [ _('Error') + ': ' +
					(msg(j, 'error') || _('Unknown error, please check the log')) ]));
				banner.appendChild(E('div', { 'class': 'cg-dim' }, [ _('See /tmp/chromeui/worker.log on the device for details.') ]));
				return;
			}
			if (s === 'stopped') {
				banner.setAttribute('class', 'cg-banner cg-banner-warn');
				banner.appendChild(E('span', { 'class': 'cg-warn' }, [ _('Aborted.') ]));
				return;
			}
			banner.appendChild(E('span', { 'class': 'cg-dim' }, [ _('No task yet.') ]));
		}

		function loadResult() {
			xexec([ 'result' ]).then(function (r) {
				var j = parseJSON(r.out, null);
				if (!j || !j.result) {
					banner.setAttribute('class', 'cg-banner cg-banner-bad');
					banner.innerHTML = '';
					banner.appendChild(E('span', { 'class': 'cg-err' }, [ _('Error') + ': ' +
						(j ? msg(j, 'error') : (r.err || _('Unknown error, please check the log'))) ]));
					return;
				}
				renderDone(j.result, j.elapsed);
			});
		}

		function renderDone(r, elapsed) {
			r = r || {};
			banner.setAttribute('class', 'cg-banner cg-banner-ok');
			banner.innerHTML = '';
			banner.appendChild(E('span', { 'class': 'cg-ok' }, [
				_('Finished in %s s').replace('%s', String(Number(elapsed != null ? elapsed : r.elapsed) || 0)) ]));
			banner.appendChild(E('div', {}, [ E('b', {}, [ _('Title') + ': ' ]), r.title || _('(no title)') ]));
			banner.appendChild(E('div', { 'class': 'cg-dim cg-clip' }, [ _('Final URL') + ': ' + (r.url || '') ]));
			banner.appendChild(E('div', { 'class': 'cg-dim' }, [
				fmtTpl(_('%s characters'), [ String(r.text_len || 0) ]) + '  ·  ' +
				fmtTpl(_('%s resources in total'), [ String(r.media_count || 0) ]) ]));
			var nt = msg(r, 'notice');
			if (nt) banner.appendChild(E('div', { 'class': 'cg-warn' }, [ nt ]));
			renderShot(r.shot);
			renderText(r.text, r);
			media = r.media || [];
			hdrSets = r.hdrs || [];
			apiCalls = r.api_calls || [];
			pageUrl = r.url || '';
			renderHdrs();
			renderApis();
			mediaCut = !!r.media_cut;
			mediaTotal = r.media_count || media.length;
			renderMedia();
			renderHtml(r);
		}


		/* Hand a file that lives on the device to the browser: its content is
		 * read in chunks (a ubus reply is capped) and offered as a real
		 * download, so the file lands on the computer that opened the page. */
		function b64ToBlob(b64, mime) {
			var bin = atob(b64), n = bin.length, u8 = new Uint8Array(n);
			for (var i = 0; i < n; i++) u8[i] = bin.charCodeAt(i);
			return new Blob([ u8 ], { type: mime || 'application/octet-stream' });
		}

		/* 🔴 a PNG must be read with --b64: reading it as text mangles the bytes
		 * (that is why the canvas download button did nothing usable) */
		function downloadFromDevice(name, asName, mime) {
			var isBin = /\.(png|jpe?g|webp|gif|bmp|svg|ico|mp4|m4s|m4a|m4v|mp3|aac|flac|wav|ogg|opus|weba|ts|webm|mkv|flv|mov|avi|zip|gz|pdf|bin|dat)$/i.test(name);
			setNotice(_('Reading the file from the device…'), false);
			readChunks(name, isBin, null, function (txt, err) {
				if (err) { setNotice(err, true); return; }
				try {
					var blob = isBin ? b64ToBlob(txt, mime)
						: new Blob([ txt ], { type: mime || 'text/plain;charset=utf-8' });
					var u = URL.createObjectURL(blob);
					var a = document.createElement('a');
					a.href = u;
					a.download = asName || name;
					document.body.appendChild(a);
					a.click();
					setTimeout(function () {
						document.body.removeChild(a);
						URL.revokeObjectURL(u);
					}, 4000);
					setNotice(_('Download started') + ': ' + (asName || name) +
						' (' + blob.size + ' B)', false);
				} catch (e) {
					setNotice(_('Download failed') + ': ' + e, true);
				}
			});
		}

		btnDl.addEventListener('click', function () {
			/* export first, so the download always matches what is on screen */
			btnDl.disabled = true;
			xexec([ 'export', '--out', m3uOutIn.value || '/tmp/chromeui/playlist.m3u' ]).then(function (r) {
				btnDl.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || j.ok === false) {
					setNotice((j ? msg(j, 'error') : (r.err || _('Unknown error, please check the log'))), true);
					return;
				}
				var t = (job && job.result && job.result.title) || '';
				var base = t.replace(/[\\/:*?"<>|\s]+/g, '_').slice(0, 48);
				downloadFromDevice('playlist.m3u', base ? (base + '.m3u') : 'playlist.m3u');
			});
		});

		/* ------------------------------------------------------------------ */
		/* job polling                                                        */
		/* ------------------------------------------------------------------ */
		function stopPoll() {
			if (poll) { clearInterval(poll); poll = null; }
		}
		function pollOnce() {
			if (pollBusy) return;
			pollBusy = true;
			xexec([ 'job' ]).then(function (r) {
				pollBusy = false;
				var j = parseJSON(r.out, null);
				if (!j) return;
				job = j;
				renderResult(j);
				if (j.state !== 'running') {
					stopPoll();
					touchVersion();
				}
			});
		}
		function startPoll() {
			stopPoll();
			pollOnce();
			poll = setInterval(function () {
				if (!document.body.contains(banner)) { stopPoll(); return; }
				pollOnce();
			}, 1500);
		}
		function touchVersion() { /* keeps a hook for future use */ }

		btnGrab.addEventListener('click', function () {
			var u = urlIn.value.trim();
			setNotice('', false);
			if (!u && hdrCk.checked && /(^|\s)curl\s/i.test(mineText())) {
				u = '-';       /* a pasted cURL carries the URL */
			}
			if (!u) {
				setNotice(_('Enter a URL first.'), true);
				return;
			}
			if (u !== '-' && !URL_RE.test(u)) u = 'http://' + u;
			var args = [ 'start', u, '--width', (wIn.value.trim() || '1280'),
				'--height', (hIn.value.trim() || '900'),
				'--timeout', (toIn.value.trim() || '30') ];
			if (waitIn.value.trim()) args.push('--wait', waitIn.value.trim());
			if (fullCk.checked) args.push('--screenshot', '--full');
			if (scrollCk.checked) args.push('--scroll');
			if (waitCk.checked) args.push('--waitload');
			if (clickIn.value) args.push('--click', clickIn.value);
			if (hdrCk.checked) {
				var ht = mineText();
				var hn = updateHdrState();
				if (!ht.trim() || !hn) {
					setNotice(_('Tick the box, paste your request headers (cookie/token) and try again.'), true);
					return;
				}
				args.push('--headers', ht);
				setNotice(_('Grab will send your pasted headers') + ' (' + hn + ')', false);
			}
			if (htmlCk.checked) args.push('--html');
			btnGrab.disabled = true;
			/* switch the banner to "running" BEFORE the RPC goes out: otherwise
			 * the previous job's result (or "aborted") sits there for the whole
			 * round trip, which reads as a stale value. */
			renderResult({ state: 'running', phase: 'queued', elapsed: 0, url: u });
			xexec(args).then(function (r) {
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					btnGrab.disabled = false;
					progLine.textContent = '';
					renderResult(job);
					banner.setAttribute('class', 'cg-banner cg-banner-bad');
					banner.innerHTML = '';
					banner.appendChild(E('span', { 'class': 'cg-err' }, [ _('Error') + ': ' +
						(msg(j, 'error') || r.err || _('Unknown error, please check the log')) ]));
					return;
				}
				media = [];
				renderMedia();
				renderShot(null);
				renderText('');
				renderHtml('', false);
				startPoll();
			});
		});

		btnStop.addEventListener('click', function () {
			btnStop.disabled = true;
			xexec([ 'stop' ]).then(function (r) {
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					setNotice(msg(j, 'error') || r.err || _('Unknown error, please check the log'), true);
					btnStop.disabled = false;
					return;
				}
				setNotice(_('Stop requested…'), false);
				setTimeout(pollOnce, 600);
			});
		});

		/* ---- media card actions ---- */
		filterSel.addEventListener('change', renderMedia);
		btnCopyAll.addEventListener('click', function () {
			var u = filtered().map(function (m) { return m.url; });
			if (!u.length) { setNotice(_('Nothing to copy.'), true); return; }
			copyText(u.join('\n'), function (okv) {
				setNotice(okv ? fmtTpl(_('%s URLs copied.'), [ String(u.length) ]) : _('Copy failed - select the text manually.'), !okv);
			});
		});
		btnExport.addEventListener('click', function () {
			var out = m3uOutIn.value.trim();
			btnExport.disabled = true;
			xexec([ 'export' ].concat(out ? [ '--out', out ] : [])).then(function (r) {
				btnExport.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					setNotice(msg(j, 'error') || r.err || _('Unknown error, please check the log'), true);
					return;
				}
				setNotice(_('Playlist written') + ': ' + j.path + '  (' + j.count + ')', false);
			});
		});
		btnCopyText.addEventListener('click', function () {
			/* the inline text is capped - copy the WHOLE text from the device */
			btnCopyText.disabled = true;
			setNotice(_('Reading from the device…'), false);
			readChunks('last.txt', false, null, function (txt, err) {
				btnCopyText.disabled = false;
				if (err) { setNotice(err, true); return; }
				copyText(txt, function (okv) {
					setNotice(okv ? _('Text copied.') : _('Copy failed - select the text manually.'), !okv);
				});
			});
		});
		btnCopyHtml.addEventListener('click', function () {
			btnCopyHtml.disabled = true;
			setNotice(_('Reading from the device…'), false);
			readChunks('last.html', false, null, function (txt, err) {
				btnCopyHtml.disabled = false;
				if (err) { setNotice(err, true); return; }
				copyText(txt, function (okv) {
					setNotice(okv ? _('HTML copied.') : _('Copy failed - select the text manually.'), !okv);
				});
			});
		});
		urlIn.addEventListener('keydown', function (ev) {
			if (ev.key === 'Enter') { ev.preventDefault(); btnGrab.click(); }
		});

		/* ------------------------------------------------------------------ */
		var style = E('style', {}, [ [
			'.cg-wrap{max-width:1040px;overflow-wrap:anywhere}',
			/* The resource list is DIV based on purpose: a 700+ character CDN
			 * url inside a <table> widens the table and with it the whole LuCI
			 * page.  .cg-mmain needs min-width:0 for the ellipsis to kick in -
			 * exactly what a table cell cannot provide. */
			'.cg-mlist{border-top:1px solid #eee}',
			'.cg-mrow{display:flex;flex-wrap:wrap;gap:6px;align-items:flex-start;border-bottom:1px solid #eee;padding:6px 0}',
			'.cg-mkind{flex:0 0 84px;font-size:12px}',
			'.cg-mmain{flex:1 1 260px;min-width:0}',
			'.cg-macts{flex:0 0 auto;display:flex;flex-wrap:wrap}',
			'.cg-mname{font-size:12px;overflow-wrap:anywhere}',
			'.cg-hdrpaste{border:2px solid #2d70b3 !important;background:#f5faff !important}',
			'.cg-hdrs{max-height:200px;overflow:auto;background:#2b2b2b;color:#ddd;font-size:11px;padding:6px;margin:4px 0;white-space:pre-wrap;overflow-wrap:anywhere}',
			'.cg-mdetail{flex:1 1 100%;background:#fbfbfb;border-left:3px solid #2d70b3;padding:6px 8px;margin-top:4px;font-size:12px;overflow-wrap:anywhere}',
			'.cg-card{border:1px solid #d4d4d4;border-radius:6px;padding:12px;margin:10px 0;background:#fff}',
			'.cg-card h3{margin:0 0 8px;font-size:15px}',
			'.cg-ver{font-weight:bold;color:#2d70b3}',
			'.cg-row{display:flex;flex-wrap:wrap;gap:10px;align-items:flex-end}',
			'.cg-row>div{flex:1 1 150px;min-width:110px}',
			'.cg-row label{display:block;font-size:12px;color:#555;margin-bottom:3px}',
			'.cg-btn{margin:2px 4px 2px 0}',
			'.cg-mini{font-size:11px;padding:2px 6px;margin:0 3px 3px 0}',
			'.cg-acts{display:flex;flex-wrap:wrap}',
			'.cg-banner{border-left:4px solid #999;background:#f6f6f6;padding:8px 12px;border-radius:4px;margin:8px 0;font-size:13px}',
			'.cg-banner-ok{border-left-color:#2d9c4f;background:#eef8f0}',
			'.cg-banner-bad{border-left-color:#c0392b;background:#fdecea}',
			'.cg-banner-warn{border-left-color:#e67e22;background:#fdf3e7}',
			'.cg-dim{color:#888;font-size:12px}',
			'.cg-ok{color:#0a0;font-weight:bold}',
			'.cg-err{color:#c00;font-weight:bold;font-size:12px}',
			'.cg-warn{color:#e67e22;font-weight:bold}',
			'.cg-pre{background:#111;color:#ddd;font-family:monospace;font-size:12px;padding:8px;border-radius:4px;min-height:40px;max-height:260px;overflow:auto;white-space:pre-wrap;word-break:break-all}',
			'.cg-table{border-collapse:collapse;width:100%;font-size:12px}',
			'.cg-table th,.cg-table td{border-bottom:1px solid #eee;padding:4px 6px;text-align:left;vertical-align:top}',
			'.cg-table th{color:#555;background:#fafafa}',
			'.cg-clip{max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
			'.cg-nowrap{white-space:nowrap}',
			'.cg-detail{font-size:12px;overflow-wrap:anywhere}',
			'.cg-shot{max-width:100%;border:1px solid #ccc;border-radius:4px;margin-top:6px}',
			'.cg-ck{display:inline-block;margin:4px 12px 0 0;font-size:12px}'
		].join('') ]) ;

		var wrap = E('div', { 'class': 'cg-wrap' }, [
			style,
			E('div', { 'class': 'cg-card' }, [
				E('h3', {}, [ stVer, '  ', E('span', { 'class': 'cg-dim' }, [
					_('Headless Chrome on the device · page grabber and media resource extractor') ]) ]),
				stBody,
				E('div', { 'style': 'margin-top:8px' }, [ btnRefresh, btnSelftest ]),
				selfOut
			]),
			E('div', { 'class': 'cg-card' }, [
				E('h3', {}, [ _('Fetch a page') ]),
				E('div', { 'class': 'cg-row' }, [
					E('div', { 'style': 'flex:3 1 340px' }, [ E('label', {}, [ _('URL') ]), urlIn ])
				]),
				E('div', { 'class': 'cg-row', 'style': 'margin-top:8px' }, [
					E('div', {}, [ E('label', {}, [ _('Wait for a CSS selector') ]), waitIn ]),
					E('div', {}, [ E('label', {}, [ _('Timeout (s)') ]), toIn ]),
					E('div', {}, [ E('label', {}, [ _('Window width') ]), wIn ]),
					E('div', {}, [ E('label', {}, [ _('Window height') ]), hIn ])
				]),
				E('div', { 'class': 'cg-row', 'style': 'margin-top:8px' }, [
					E('div', { 'style': 'flex:3 1 340px' }, [
						E('label', {}, [ _('Click this selector before collecting (optional)') ]), clickIn ])
				]),
				E('div', { 'style': 'margin-top:6px' }, [
					E('label', { 'class': 'cg-ck' }, [ fullCk, ' ' + _('Capture a screenshot') ]),
					E('label', { 'class': 'cg-ck' }, [ scrollCk, ' ' + _('Scroll to load lazy content') ]),
					E('label', { 'class': 'cg-ck' }, [ waitCk, ' ' + _('Wait for the whole page (slow)') ]),
					E('label', { 'class': 'cg-ck' }, [ htmlCk, ' ' + _('HTML source') ]),
					E('label', { 'class': 'cg-ck' }, [ hdrCk, ' ' + _('Paste my own request headers (for pages that need a login)') ])
				]),
				mineWrap,
				E('div', { 'class': 'cg-dim', 'style': 'margin-top:4px' }, [
					_('Players that only load the media after a play click (ximalaya and other web-component players) are started automatically; give an exact selector here if that is not enough') ]),
				E('div', { 'style': 'margin-top:8px' }, [ btnGrab, btnStop ]),
				progLine,
				banner
			]),
			E('div', { 'class': 'cg-card' }, [
				E('h3', {}, [ _('Screenshot') ]),
				shotMeta,
				shotImg
			]),
			E('div', { 'class': 'cg-card' }, [
				E('h3', {}, [ _('Visible text') ]),
				textMeta,
				E('div', { 'style': 'margin-top:4px' }, [ btnCopyText ]),
				textPre
			]),
			E('div', { 'class': 'cg-card' }, [
				E('h3', {}, [ _('Media resources') ]),
				mediaMeta,
				E('div', { 'class': 'cg-row', 'style': 'margin-top:8px' }, [
					E('div', {}, [ E('label', {}, [ _('Resource filter') ]), filterSel ]),
					E('div', { 'style': 'flex:2 1 240px' }, [ E('label', {}, [ _('Playlist output path') ]), m3uOutIn ])
				]),
				E('div', { 'style': 'margin-top:6px' }, [ btnCopyAll, btnExport, btnDl ]),
				E('div', { 'style': 'margin-top:6px' }, [ btnHdrAll, btnHdrDl ]),
				E('div', { 'style': 'margin-top:6px' }, [ btnQQ, btnQQCopy, btnQQDl ]),
				qqOut,
				E('div', { 'class': 'cg-dim', 'style': 'margin-top:4px' }, [
					_('The exported .m3u carries the page Referer and a browser user-agent, so VLC/ffmpeg can fetch these URLs directly') ]),
				mediaBox
			]),
			E('div', { 'class': 'cg-card' }, [
				E('h3', {}, [ _('HTML source') ]),
				htmlMeta,
				E('div', { 'style': 'margin-top:4px' }, [ btnCopyHtml ]),
				htmlPre
			]),
			notice
		]);

		renderStatus();
		renderResult(job);
		if (job && job.state === 'running') startPoll();

		return wrap;
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
