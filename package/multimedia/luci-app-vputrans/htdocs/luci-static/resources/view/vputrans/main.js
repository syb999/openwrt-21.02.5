'use strict';
'require view';
'require fs';

/*
 * luci-app-vputrans - frontend view (v3)
 *
 * i18n: the source strings in this file are ENGLISH and are wrapped in _(),
 * which is the LuCI translator (window.TR is filled by
 * /cgi-bin/luci/admin/translations/<lang>, loaded by the theme header before
 * cbi.js defines _()).  Chinese comes from po/zh_Hans/vputrans.po, installed as
 * /usr/lib/lua/luci/i18n/vputrans.zh-cn.lmo by luci-i18n-vputrans-zh-cn.
 *
 * The backend (/usr/bin/vputrans) never sends display text: it sends an
 * English template plus arguments ({"reason":"...%s...","reason_args":[...]}
 * and log events {"ts","t","a"}), and this file renders them through the same
 * _() lookup - including every argument, so backend words such as
 * "hardware decode" get translated while paths/numbers pass through.
 *
 * One input box accepts BOTH a local path and an online URL
 * (http/https/rtsp/rtmp/rtmps/hls/udp/...).  The scheme is auto-detected by the
 * backend - there is deliberately NO "mode" switch, and there is exactly ONE
 * transcoder: /usr/bin/ffmpeg-rkrga (hardware decode -> RGA scale -> hardware
 * encode, zero copy).
 *
 * NOTE: template literals are avoided on purpose (CONFIG_LUCI_JSMIN breaks
 * them), and so are regex literals containing "://" (jsmin truncates them).
 */

var VPU_VER = 'v3.3.3';

/* ---------------------------------------------------------------------------
 * Backend vocabulary.  The backend sends these strings as message templates
 * (msgid) and as arguments; listing them here as _() literals is what makes the
 * LuCI i18n scanner pick them up into the .pot/.po files.  At runtime the same
 * lookup happens through _() in fmtTpl()/T().
 * ------------------------------------------------------------------------- */
/* eslint-disable no-unused-vars */
var VPU_BACKEND_MSGS = [
	/* probe reasons */
	_('Source is 4:2:0 %s: hardware decoding available (%s)'),
	_('Source pixel format is %s (the VPU only decodes 4:2:0), switching to software decode'),
	_('No hardware decoder for %s, switching to software decode'),
	/* errors */
	_('No input path or URL given'),
	_('File not found: %s'),
	_('This is a directory, not a video file: %s'),
	_('Cannot parse the protocol of: %s'),
	_('Unrecognised source (not a video, or damaged)'),
	_('No input selected (local path or URL)'),
	_('Transcoder %s is missing, please install ffmpeg-rkrga'),
	_('Invalid resolution: %s'),
	_('Output must differ from the input file'),
	_('Cannot create the output directory: %s'),
	_('Output directory is not writable: %s'),
	_('Another job is already running, wait for it or press Stop first'),
	_('Unknown argument: %s'),
	_('No job is currently running'),
	_('usage: vputrans {version|probe <src>|list|run <src> [opt]|status|stop|history}'),
	_('Connection refused: nothing is listening on that address/port (check the URL or whether the server is up)'),
	_('Connection timed out: no response within %ss (network unreachable, or the stream has not started)'),
	_('No route to host: the network is unreachable'),
	_('Server returned 404: the address does not exist or has expired'),
	_('Server returned 403: access denied (authentication may be required)'),
	_('Server returned 401: authentication required (user/password or token)'),
	_('Server error (5xx): the origin is broken, please retry later'),
	_('Unsupported protocol: not in the list of available protocols'),
	_('Unrecognised data: not a valid video stream (maybe a web page or plain text)'),
	_('Job stopped'),
	_('Stream ended (the source closed early)'),
	_('Authentication required: wrong user/password or token'),
	_('DNS resolution failed: check the device DNS settings or the address spelling'),
	_('Unknown error, please check the log'),
	/* status / notices */
	_('batch needs --dir DIR or --list FILE'),
	_('Empty file list'),
	_('File list not found: %s'),
	_('Directory not found: %s'),
	_('No matching video files in %s'),
	_('✔ Batch finished: %s/%s ok, %s failed, %s skipped'),
	_('⏹ Batch stopped: %s/%s ok, %s failed, %s skipped'),
	_('Unsupported encoder: %s (this SoC only has H.264/H.265 hardware encoders)'),
	_('Source duration metadata (%ss) is unreliable - already exceeded, showing processed time instead'),
	_('Stopping…'),
	_('Stopped manually'),
	_('Transcode ended early (ffmpeg exit code %s), the output file was produced'),
	_('Transcode failed (exit code %s)'),
	_('Source audio %s cannot be stored in mp4, re-encoding to AAC'),
	/* log events */
	_('▶ Starting: %s (%s) | %s %s | %s + hardware encode %s | %s %s'),
	_('Processed %ss / %ss (%s%)'),
	_('Processed %ss / %ss (%s%) at %s'),
	_('Recorded %ss (live stream)'),
	_('Recorded %ss (live stream) at %s'),
	_('✔ Finished: %s (%s) in %ss'),
	_('✔ Finished: %s (%s) in %ss, average %s'),
	_('⏹ Stopped: %s (%s) in %ss'),
	_('Stop requested, finishing the current job'),
	/* vocabulary used as arguments */
	_('local file'),
	_('online URL'),
	_('live stream'),
	_('hardware decode'),
	_('software decode'),
	_('CBR bitrate'),
	_('CQP QP')
];
/* eslint-enable no-unused-vars */

function vexec(args) {
	return fs.exec('/usr/bin/vputrans', args).then(function(res) {
		return { code: res.code, out: (res.stdout || '').trim(), err: (res.stderr || '').trim() };
	}).catch(function(e) {
		return { code: -1, out: '', err: String((e && e.message) || e) };
	});
}

function parseJSON(s, dflt) { try { return JSON.parse(s); } catch (e) { return dflt; } }

function basename(p) { var a = String(p || '').replace(/\/+$/, '').split('/'); return a[a.length - 1] || ''; }
function dirname(p) { var i = String(p || '').lastIndexOf('/'); return i > 0 ? String(p).substring(0, i) : '/'; }
function stripExt(n) { var i = String(n || '').lastIndexOf('.'); return i > 0 ? String(n).substring(0, i) : n; }

/* NOTE: the firmware build uses CONFIG_LUCI_JSMIN=y, and luci's jsmin.c
 * truncates a regex LITERAL at "://" (it mistakes the "//" for a comment
 * start, aborting the line -> "Invalid regular expression: missing /").
 * So the scheme matcher is built from a STRING instead of a literal. */
var URL_RE = new RegExp('^[a-zA-Z][a-zA-Z0-9+.-]*://');
function isUrl(p) { return URL_RE.test(String(p || '')); }

/* ---- i18n helpers -------------------------------------------------------- */

function T(s) { return (s === null || s === undefined || s === '') ? '' : _(String(s)); }

/* translate a message template and substitute its %s arguments in order */
function fmtTpl(tpl, args) {
	var s = T(tpl), i = 0;
	return s.replace(/%s/g, function() {
		return (args && i < args.length) ? T(args[i++]) : '';
	});
}
/* a backend message object {tpl, tpl_args} -> display string */
function msg(o, field) {
	if (!o || !o[field]) return '';
	return fmtTpl(o[field], o[field + '_args'] || []);
}
/* a log event {ts, t, a} -> display string */
function evText(e) {
	if (!e) return '';
	return String(e.ts || '') + ' ' + fmtTpl(e.t, e.a || []);
}

function fmtSize(b) {
	b = Number(b) || 0;
	if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GB';
	if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
	if (b >= 1024) return (b / 1024).toFixed(0) + ' KB';
	return b + ' B';
}
function fmtDur(s) {
	s = Number(s) || 0;
	if (s < 0) s = 0;
	var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = Math.floor(s % 60);
	function p2(n) { return (n < 10 ? '0' : '') + n; }
	return (h > 0 ? (h + ':') : '') + p2(m) + ':' + p2(ss);
}
function srcKind(stype) {
	if (stype === 'live') return _('Live stream');
	if (stype === 'url') return _('Online URL');
	return _('Local file');
}
function stateText(s) {
	if (s === 'done') return _('finished');
	if (s === 'error') return _('failed');
	if (s === 'stopped') return _('stopped');
	if (s === 'running') return _('running');
	return s || '';
}
/* the encoder actually chosen in the form, spelled out (the source codec and
 * the output encoder are different things - mixing them up is the classic
 * "H.265 does not work" misunderstanding).
 * H.264/H.265 are the hardware encoders on RK3566 (the SoC has no VP8/VP9
 * encoder), while H.264/HEVC/VP8/VP9 sources are all decoded by the VPU. */
function encName(v) { return (v === 'hevc') ? 'hevc_rkmpp' : 'h264_rkmpp'; }
function encLabel(v) { return (v === 'hevc') ? 'H.265 (hevc_rkmpp)' : 'H.264 (h264_rkmpp)'; }
/* pipeline description, e.g. "HW decode -> HW encode hevc_rkmpp (zero copy)".
 * This app is hardware-only by design: every supported source codec is decoded
 * by the VPU and the H.264/H.265 hardware encoder writes the output. */
function pipeText(mode, scaleeng, enc) {
	var t;
	if (mode === 'hw')
		t = _('HW decode') + ' → ' + _('HW encode') + (enc ? ' ' + enc : '') + ' · ' + _('zero copy');
	else
		t = _('SW decode') + ' → ' + _('HW encode') + (enc ? ' ' + enc : '');
	if (scaleeng === 'rga') t += ' · ' + _('RGA hardware scaling');
	else if (scaleeng === 'sw') t += ' · ' + _('software scaling');
	return t;
}

return view.extend({

	load: function() {
		/* 'list' is deliberately NOT fetched here: load() gates the FIRST
		 * paint, and the candidate-file scan must never block the page.
		 * It is kicked off asynchronously from render() instead. */
		return Promise.all([
			vexec([ 'version' ]),
			vexec([ 'status' ]),
			vexec([ 'history' ])
		]);
	},

	render: function(data) {
		var sres = data[1], hres = data[2];
		var files = [];
		var st = parseJSON(sres.out, { state: 'idle' });
		var self = this;
		var running = (st.state === 'running');

		var el = {};
		function inputOpts() { return { 'type': 'text', 'style': 'width:100%;box-sizing:border-box;padding:6px' }; }
		function selectOpts() { return { 'style': 'width:100%;box-sizing:border-box;padding:6px' }; }

		/* ---- source ----
		 * Free-form text field: the user may type ANY local path or URL.
		 * Scanned candidates are attached as <datalist> suggestions only -
		 * picking one fills the box, but the box stays fully editable. */
		var srcList = E('datalist', { id: 'vpu-src-list' });
		var pathIn = E('input', Object.assign(inputOpts(), {
			id: 'vpu-src',
			list: 'vpu-src-list',
			autocomplete: 'off',
			placeholder: '/mnt/video/a.mp4   ' + _('or') + '   http://host/a.mkv   ' + _('or') + '   rtsp://user:pass@ip:554/stream' }));
		var srcHint = E('div', { 'class': 'vpu-dim', 'style': 'margin-top:4px' }, [
			_('Free-form input: local paths and http/https/rtsp/rtmp/rtmps/hls/udp URLs are all accepted (the backend detects the scheme automatically, there is no mode switch). The candidates below are suggestions only - pick one or just type over it.')
		]);
		var listHint = E('div', { 'class': 'vpu-dim', 'style': 'margin-top:2px' }, [ _('Candidate files: loading…') ]);

		/* ---- parameters ---- */
		var qualitySel = E('select', selectOpts(), [
			E('option', { value: 'cbr' }, [ _('Target bitrate (CBR, fast)') ]),
			E('option', { value: 'crf' }, [ _('Constant quality (CQP, slow)') ])
		]);
		/* 🔴 下拉菜单（不用文本框）：文本框会被浏览器恢复上次输入的值，改了默认也没用 ✗ */
		var brIn = E('select', selectOpts(), [
			E('option', { value: '' }, [ _('Follow the source (default)') ]),
			E('option', { value: '500k' }, [ '500k' ]),
			E('option', { value: '800k' }, [ '800k' ]),
			E('option', { value: '1M' }, [ '1M' ]),
			E('option', { value: '1.5M' }, [ '1.5M' ]),
			E('option', { value: '2M' }, [ '2M' ]),
			E('option', { value: '3M' }, [ '3M' ]),
			E('option', { value: '4M' }, [ '4M' ]),
			E('option', { value: '6M' }, [ '6M' ]),
			E('option', { value: '8M' }, [ '8M' ]),
			E('option', { value: '10M' }, [ '10M' ])
		]);
		var qpIn = E('input', Object.assign(inputOpts(), { value: '26', placeholder: _('0-51, default 26') }));

		var encSel = E('select', selectOpts(), [
			E('option', { value: 'h264' }, [ 'H.264 (h264_rkmpp)' ]),
			E('option', { value: 'hevc' }, [ 'H.265 (hevc_rkmpp)' ])
		]);
		/* hardware-only by design - say so, so nobody looks for VP8/VP9 output */
		var encHint = E('div', { 'class': 'vpu-dim', 'style': 'margin-top:4px' },
			[ _('Sources are hardware-decoded (H.264/HEVC/VP8/VP9) and encoded by the H.264/H.265 hardware encoder - the output codecs are the only two this SoC encodes in hardware') ]);
		var resSel = E('select', selectOpts(), [
			E('option', { value: 'source' }, [ _('Source size (no scaling)') ]),
			E('option', { value: '480p' }, [ '480p' ]),
			E('option', { value: '720p' }, [ '720p' ]),
			E('option', { value: '1080p' }, [ '1080p' ])
		]);
		var fpsIn = E('input', Object.assign(inputOpts(), { value: 'source', placeholder: _('source or a number, e.g. 25') }));
		var scalerSel = E('select', selectOpts(), [
			E('option', { value: 'bicubic' }, [ _('bicubic (default)') ]),
			E('option', { value: 'bilinear' }, [ _('bilinear (faster)') ]),
			E('option', { value: 'lanczos' }, [ _('lanczos (sharper)') ])
		]);
		var ssIn = E('input', Object.assign(inputOpts(), { value: '0', placeholder: '0' }));
		var durIn = E('input', Object.assign(inputOpts(), { value: '0', placeholder: _('0 = until the end / manual stop') }));
		var audioSel = E('select', selectOpts(), [
			E('option', { value: 'copy' }, [ _('Copy (default, no re-encode)') ]),
			E('option', { value: 'none' }, [ _('Drop audio') ]),
			E('option', { value: 'aac' }, [ _('Transcode to AAC') ])
		]);
		var contSel = E('select', selectOpts(), [
			E('option', { value: 'auto' }, [ _('Auto (live → mkv, file → mp4)') ]),
			E('option', { value: 'mp4' }, [ 'mp4' ]),
			E('option', { value: 'mkv' }, [ 'mkv' ]),
			E('option', { value: 'ts' }, [ 'ts' ])
		]);
		var outIn = E('input', Object.assign(inputOpts(), { placeholder: _('empty = generate automatically') }));

		/* ---- readouts ---- */
		var banner = E('div', { 'class': 'vpu-banner', id: 'vpu-banner' }, [ _('Reading status…') ]);
		var probBox = E('div', { 'class': 'vpu-probe', id: 'vpu-probe' }, [ E('span', { 'class': 'vpu-dim' }, [ _('Enter a path or URL and the source is probed automatically.') ]) ]);
		var barFill = E('div', { 'class': 'vpu-bar-fill', 'style': 'width:0%' });
		var bar = E('div', { 'class': 'vpu-bar' }, [ barFill ]);
		var statLine = E('div', { 'class': 'vpu-dim', id: 'vpu-stat' }, [ '' ]);
		var logPre = E('pre', { 'class': 'vpu-log', id: 'vpu-log' }, [ '' ]);
		var histBox = E('div', { 'class': 'vpu-hist', id: 'vpu-hist' }, [ E('span', { 'class': 'vpu-dim' }, [ _('No history yet.') ]) ]);

		var btnRun = E('button', { 'class': 'btn cbi-button cbi-button-apply vpu-btn', id: 'vpu-btn-run' }, [ _('Start') ]);
		var btnStop = E('button', { 'class': 'btn cbi-button vpu-btn', id: 'vpu-btn-stop' }, [ _('Stop') ]);
		var btnProbe = E('button', { 'class': 'btn cbi-button vpu-btn', id: 'vpu-btn-probe' }, [ _('Probe') ]);
		var btnList = E('button', { 'class': 'cbi-button', id: 'vpu-btn-list' }, [ _('Refresh list') ]);
		/* ---- batch (queue) widgets ---- */
		var bdirIn = E('input', { 'type': 'text', id: 'vpu-bdir', 'placeholder': '/mnt/iptv' });
		var brecIn = E('input', { 'type': 'checkbox', id: 'vpu-brec' });
		var bListArea = E('textarea', { id: 'vpu-blist', 'rows': 6, 'spellcheck': 'false' });
		var boutDirIn = E('input', { 'type': 'text', id: 'vpu-boutdir', 'placeholder': _('empty = next to each source file') });
		var bskipIn = E('input', { 'type': 'checkbox', id: 'vpu-bskip', 'checked': 'checked' });
		var bStat = E('div', { 'class': 'vpu-dim', id: 'vpu-bstat' }, [ _('Idle') ]);
		var bRes = E('div', { 'class': 'vpu-dim', id: 'vpu-bres' }, [ '' ]);
		var btnScan = E('button', { 'class': 'btn cbi-button vpu-btn', id: 'vpu-btn-scan' }, [ _('Scan directory') ]);
		var btnBStart = E('button', { 'class': 'btn cbi-button cbi-button-apply vpu-btn', id: 'vpu-btn-brun' }, [ _('Start batch') ]);
		var btnBStop = E('button', { 'class': 'btn cbi-button vpu-btn', id: 'vpu-btn-bstop' }, [ _('Stop batch') ]);

		function syncQuality() {
			var crf = (qualitySel.value === 'crf');
			brIn.disabled = crf;
			qpIn.disabled = !crf;
		}
		qualitySel.addEventListener('change', syncQuality);
		syncQuality();
		btnStop.disabled = !running;

		function setOutDefault() {
			var p = pathIn.value.trim();
			if (!p || isUrl(p)) { outIn.value = ''; return; }
			outIn.value = dirname(p) + '/' + stripExt(basename(p)) + '_vput.mp4';
		}

		function renderProbe(j) {
			probBox.innerHTML = '';
			if (!j || !j.ok) {
				probBox.appendChild(E('span', { 'class': 'vpu-err' }, [
					_('Probe failed') + ': ' + (msg(j, 'error') || _('Unknown error, please check the log')) ]));
				var d = msg(j, 'detail');
				if (d) probBox.appendChild(E('div', { 'class': 'vpu-dim' }, [ d ]));
				return;
			}
			probBox.appendChild(E('div', [
				E('span', { 'class': (j.mode === 'hw') ? 'vpu-ok' : 'vpu-warn' },
					[ pipeText(j.mode, '', encLabel(encSel.value)) ])
			]));
			probBox.appendChild(E('div', { 'class': 'vpu-dim' }, [
				_('Source codec') + ' ' + (j.codec || '-') + ' → ' +
				_('Output encoder') + ' ' + encLabel(encSel.value) ]));
			probBox.appendChild(E('div', { 'class': 'vpu-dim' }, [ msg(j, 'reason') ]));
			var durTxt = (j.live || Number(j.duration) <= 0)
				? _('live stream (no total duration)')
				: (Number(j.duration).toFixed(2) + ' s');
			probBox.appendChild(E('div', { 'class': 'vpu-dim' }, [
				_('Type') + ' ' + srcKind(j.sourcetype) + (j.protocol ? (' (' + j.protocol + ')') : '') + ' / ' +
				_('source codec') + ' ' + j.codec + ' / ' + _('pixel format') + ' ' + j.pix_fmt + ' / ' +
				j.width + 'x' + j.height + ' / ' + j.fps + ' fps / ' + durTxt +
				' / ' + _('audio') + ' ' + (j.audio || _('none')) +
				((j.bitrate && Number(j.bitrate) > 0)
					? (' / ' + _('source bitrate') + ' ~' + fmtSize(Number(j.bitrate) / 8) + '/s') : '')
			]));
			/* 🔴 不预填：码率框留空 = 跟随源片（视频轨码率）。这里只说清楚会用什么 */
			if (j.bitrate && Number(j.bitrate) > 0 && qualitySel.value === 'cbr') {
				probBox.appendChild(E('div', { 'class': 'vpu-dim' }, [
					_('CBR: following the source') + ' ~' +
					fmtSize(Number(j.bitrate) / 8) + '/s  (' +
					_('type a value here to override, e.g. 1M') + ')' ]));
			}
			setOutDefault();
		}

		function doProbe() {
			var p = pathIn.value.trim();
			if (!p) { probBox.textContent = _('Please enter a local path or URL first.'); return; }
			probBox.textContent = _('Probing…');
			vexec([ 'probe', p ]).then(function(r) {
				var j = parseJSON(r.out, null);
				/* never swallow the backend message: fall back to stderr/stdout verbatim */
				if (!j) j = { ok: false, error: '%s', error_args: [ (r.err || r.out || _('Unknown error, please check the log')) ] };
				if (!j.ok && !j.error && r.err) j = { ok: false, error: '%s', error_args: [ r.err ] };
				renderProbe(j);
			});
		}

		var histSig = '';
		function renderHistory() {
			vexec([ 'history' ]).then(function(r) {
				var j = parseJSON(r.out, { items: [] });
				var items = j.items || [];
				/* 🔴 定时器每 4 秒会调这里：只有内容真变了才重建表格，
				 * 否则选区/滚动位置每次都被清掉 */
				var sig = items.length + '|' + items.map(function (it) {
					return (it.ts || '') + (it.state || '') + (it.size || '');
				}).join(',');
				if (sig === histSig) { return; }
				histSig = sig;
				histBox.innerHTML = '';
				if (!items.length) { histBox.appendChild(E('span', { 'class': 'vpu-dim' }, [ _('No history yet.') ])); return; }
				var tbl = E('table', { 'class': 'vpu-table' });
				var hr = E('tr', {});
				[ _('Time'), _('Input source'), _('Pipeline'), _('Output file'), _('Elapsed'), _('Result') ].forEach(function(h) { hr.appendChild(E('th', {}, [ h ])); });
				tbl.appendChild(hr);
				items.forEach(function(it) {
					var tr = E('tr', {});
					var stCls = (it.state === 'done') ? 'vpu-ok' : (it.state === 'error' ? 'vpu-err' : 'vpu-warn');
					tr.appendChild(E('td', {}, [ it.time ]));
					tr.appendChild(E('td', { 'class': 'vpu-clip' }, [ it.src ]));
					tr.appendChild(E('td', {}, [ pipeText(it.mode, '', it.enc) ]));
					tr.appendChild(E('td', { 'class': 'vpu-clip' }, [ it.out ]));
					tr.appendChild(E('td', {}, [ (it.elapsed || 0) + 's' ]));
					tr.appendChild(E('td', { 'class': stCls }, [ stateText(it.state) + '  ' + fmtSize(it.size) ]));
					tbl.appendChild(tr);
				});
				histBox.appendChild(tbl);
			});
		}

		function applyStatus(j) {
			var isRun = (j.state === 'running');
			running = isRun;
			btnRun.disabled = isRun;
			btnStop.disabled = !isRun;

			var live = (j.live === true) || (j.sourcetype === 'live');
			banner.className = 'vpu-banner';
			if (j.state === 'running') {
				banner.textContent = (live ? _('Recording…') : _('Transcoding…'));
			} else if (j.state === 'done') {
				banner.className = 'vpu-banner vpu-banner-ok';
				banner.textContent = (live ? _('Recording finished ✓') : _('Transcode finished ✓'));
			} else if (j.state === 'stopped') {
				banner.className = 'vpu-banner vpu-banner-warn';
				banner.textContent = _('Stopped');
			} else if (j.state === 'error') {
				banner.className = 'vpu-banner vpu-banner-bad';
				banner.textContent = _('Error') + ': ' + (msg(j, 'error') || _('Unknown error, please check the log'));
			} else {
				banner.textContent = _('Idle');
			}
			/* secondary line: backend detail / start-up notice */
			var extra = [];
			var det = msg(j, 'detail');
			if (det && j.state === 'error') extra.push(det);
			var no = msg(j, 'notice');
			if (no) extra.push(no);
			if (extra.length) {
				banner.appendChild(E('div', { 'class': 'vpu-dim' }, [ extra.join(' — ') ]));
			}

			var pct = Number(j.progress) || 0;
			/* progmode 'time': the source declares no usable length (live stream,
			 * HLS/IPTV rolling playlist, or a recording whose header duration is
			 * wrong) -> show processed time, never a percentage that would sit at
			 * 100% for the rest of the job. */
			var timemode = (j.progmode === 'time') || live;
			if (isRun && timemode) { barFill.style.width = '100%'; barFill.className = 'vpu-bar-fill vpu-live'; }
			else { barFill.className = 'vpu-bar-fill'; barFill.style.width = Math.max(0, Math.min(100, pct)) + '%'; }

			var bits = [];
			if (timemode) {
				bits.push(_('processed') + ' ' + fmtDur(j.out_time) + ' / ' +
					(live ? _('live stream') : _('total length unknown')));
			} else {
				bits.push(pct.toFixed(1) + '%');
				if (Number(j.total) > 0)
					bits.push(_('processed') + ' ' + fmtDur(j.out_time) + ' / ' + fmtDur(j.total));
			}
			if (j.fps) bits.push(j.fps + ' fps');
			if (j.speed) bits.push(j.speed);
			bits.push(_('written') + ' ' + fmtSize(j.outsize));
			if (j.elapsed) bits.push(_('elapsed') + ' ' + j.elapsed + 's');
			if (j.mode) bits.push(pipeText(j.mode, j.scaleeng, j.encoder));
			if (j.state === 'done' || j.state === 'stopped')
				bits.push(_('output file') + ' ' + (j.out || '') + '  ' + fmtSize(j.outsize));
			if (j.error && j.state !== 'error' && isRun) bits.push(msg(j, 'error'));
			statLine.textContent = bits.join('   ');

			var lines = [];
			if (Array.isArray(j.log)) {
				/* entries are {ts,t,a} objects; tolerate plain strings too so a
				 * half-updated install can never print "[object Object]" */
				j.log.forEach(function(e) {
					lines.push((e && typeof e === 'object') ? evText(e) : String(e));
				});
			}
			if (j.state === 'error' && !lines.length) lines.push(msg(j, 'error'));
			logPre.textContent = lines.join('\n');
		}

		function refreshStatus() {
			vexec([ 'status' ]).then(function(r) {
				var j = parseJSON(r.out, null);
				if (j) applyStatus(j);
			});
		}

		/* ---- candidate file list (async, never blocks first paint) ---- */
		function loadFileList() {
			listHint.textContent = _('Candidate files: loading…');
			vexec([ 'list' ]).then(function(r) {
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					listHint.className = 'vpu-dim';
					listHint.textContent = _('Candidate files: read failed') + ' (' + (r.err || _('no reply')) + ')';
					return;
				}
				srcList.innerHTML = '';
				(j.files || []).forEach(function(f) {
					srcList.appendChild(E('option', { value: f.path, label: f.path + '  (' + fmtSize(f.size) + ')' }));
				});
				var n = (j.count != null) ? j.count : (j.files || []).length;
				listHint.className = 'vpu-dim';
				listHint.textContent = _('Candidate files') + ': ' + n + (j.truncated ? (' (' + _('limit reached') + ')') : '');
			}).catch(function() {
				listHint.className = 'vpu-dim';
				listHint.textContent = _('Candidate files: read error');
			});
		}

		/* ---- events ---- */
		pathIn.addEventListener('change', doProbe);
		btnProbe.addEventListener('click', doProbe);

		/* ---- batch (queue) ----------------------------------------------------
		 * Two ways to feed the queue: scan a directory, or type/paste a list
		 * (one path per line - fully UTF-8, Chinese names are fine).  The
		 * backend stores the pasted list (the UI has no write ACL) and runs
		 * `vputrans run` per file, so every file behaves exactly like a manual
		 * run; we only poll the queue status here.
		 */
		function batchOpts() {
			var a = [ '--quality', qualitySel.value,
				'--codec', encSel.value,
				'--res', resSel.value,
				'--scaler', scalerSel.value,
				'--fps', (fpsIn.value.trim() || 'source'),
				'--audio', audioSel.value,
				'--container', contSel.value,
				'--skip-existing', bskipIn.checked ? '1' : '0' ];
			if (qualitySel.value === 'crf') a.push('--qp', (qpIn.value.trim() || '26'));
			else if (brIn.value.trim()) a.push('--bitrate', brIn.value.trim());
			if (ssIn.value.trim() && ssIn.value.trim() !== '0') a.push('--ss', ssIn.value.trim());
			if (durIn.value.trim() && durIn.value.trim() !== '0') a.push('--dur', durIn.value.trim());
			if (boutDirIn.value.trim()) a.push('--outdir', boutDirIn.value.trim());
			return a;
		}
		function batchLines() {
			return (bListArea.value || '').split('\n')
				.map(function (x) { return x.trim(); })
				.filter(function (x) { return x.length > 0; });
		}
		function renderBatch(j) {
			if (!j) { return; }
			var st = j.state || 'idle';
			if (st === 'idle' || st === '') { bStat.textContent = _('Idle'); bRes.textContent = ''; return; }
			var l = _('Batch') + ' ' + (j.i || 0) + '/' + (j.n || 0) +
				'  ' + _('ok') + ' ' + (j.ok || 0) +
				'  ' + _('failed') + ' ' + (j.bad || 0) +
				'  ' + _('skipped') + ' ' + (j.skipped || 0);
			if (st === 'finished') l += '  ·  ' + _('queue finished');
			else if (st === 'stopped') l += '  ·  ' + _('queue stopped');
			bStat.textContent = l;
			var rows = (j.results || []).slice(-40).map(function (r) {
				var m = (r.state === 'done') ? '✔ ' : (r.state === 'skipped') ? '⏭ ' : '✘ ';
				return m + basename(r.src) + ' → ' + (r.out || '');
			});
			if (j.cur) rows.unshift(_('current') + ': ' + basename(j.cur));
			bRes.textContent = rows.join('\n');
		}
		function refreshBatch() {
			vexec([ 'batchstatus' ]).then(function (r) { renderBatch(parseJSON(r.out, null)); });
		}
		btnScan.addEventListener('click', function () {
			var d = bdirIn.value.trim();
			if (!d) { banner.className = 'vpu-banner vpu-banner-bad'; banner.textContent = _('Enter a directory first.'); return; }
			btnScan.disabled = true;
			vexec([ 'list', '--dir', d, brecIn.checked ? '1' : '0' ]).then(function (r) {
				btnScan.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					banner.className = 'vpu-banner vpu-banner-bad';
					banner.textContent = _('Scan failed') + ': ' + (r.err || _('Unknown error'));
					return;
				}
				bListArea.value = (j.files || []).map(function (f) { return f.path; }).join('\n');
				banner.className = 'vpu-banner';
				banner.textContent = fmtTpl(_('Found %s file(s)'), [ String((j.files || []).length) ]);
			});
		});
		btnBStart.addEventListener('click', function () {
			var lines = batchLines();
			var d = bdirIn.value.trim();
			var opts = batchOpts();
			var call;
			if (lines.length) {
				call = vexec([ 'putlist' ].concat(lines)).then(function (r1) {
					var j1 = parseJSON(r1.out, null);
					if (!j1 || !j1.ok) { return { out: r1.out, err: r1.err }; }
					return vexec([ 'batch', '--list', '/tmp/vputrans.batch.queue' ].concat(opts));
				});
			} else if (d) {
				call = vexec([ 'batch', '--dir', d ].concat(brecIn.checked ? [ '--recursive' ] : []).concat(opts));
			} else {
				banner.className = 'vpu-banner vpu-banner-bad';
				banner.textContent = _('Enter a directory or a file list first.');
				return;
			}
			btnBStart.disabled = true;
			call.then(function (r) {
				btnBStart.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					banner.className = 'vpu-banner vpu-banner-bad';
					banner.textContent = _('Cannot start') + ': ' + (msg(j, 'error') || r.err || _('Unknown error'));
					return;
				}
				banner.className = 'vpu-banner';
				banner.textContent = fmtTpl(_('Batch started: %s file(s) - one at a time'), [ String(j.count) ]);
				refreshBatch();
			});
		});
		btnBStop.addEventListener('click', function () {
			vexec([ 'batchstop' ]).then(function () {
				banner.className = 'vpu-banner vpu-banner-warn';
				banner.textContent = _('Stop requested - finishing the current file');
			});
		});
		brecIn.addEventListener('change', function () {
			if (bListArea.value.trim()) { return; }   /* keep a hand-edited list */
		});
		btnList.addEventListener('click', loadFileList);

		btnRun.addEventListener('click', function() {
			var p = pathIn.value.trim();
			if (!p) { banner.className = 'vpu-banner vpu-banner-bad'; banner.textContent = _('Please enter a local path or URL first.'); return; }
			btnRun.disabled = true;
			var args = [ 'run', p,
				'--quality', qualitySel.value,
				'--codec', encSel.value,
				'--res', resSel.value,
				'--scaler', scalerSel.value,
				'--fps', (fpsIn.value.trim() || 'source'),
				'--audio', audioSel.value,
				'--container', contSel.value ];
			if (qualitySel.value === 'crf') args.push('--qp', (qpIn.value.trim() || '26'));
			else if (brIn.value.trim()) args.push('--bitrate', brIn.value.trim());
			if (ssIn.value.trim() && ssIn.value.trim() !== '0') args.push('--ss', ssIn.value.trim());
			if (durIn.value.trim() && durIn.value.trim() !== '0') args.push('--dur', durIn.value.trim());
			var o = outIn.value.trim();
			if (o) args.push('--out', o);
			vexec(args).then(function(r) {
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					banner.className = 'vpu-banner vpu-banner-bad';
					banner.textContent = _('Cannot start') + ': ' + (msg(j, 'error') || r.err || _('Unknown error, please check the log'));
					btnRun.disabled = false;
					return;
				}
				banner.className = 'vpu-banner';
				banner.textContent = _('Started…');
				var no = msg(j, 'notice');
				if (no) banner.appendChild(E('div', { 'class': 'vpu-dim' }, [ no ]));
				refreshStatus();
				setTimeout(renderHistory, 1500);
			});
		});

		btnStop.addEventListener('click', function() {
			btnStop.disabled = true;
			vexec([ 'stop' ]).then(function(r) {
				var j = parseJSON(r.out, null);
				banner.textContent = (j && j.ok) ? _('Stop requested…') : (_('Cannot stop') + ': ' + (msg(j, 'error') || _('Unknown error, please check the log')));
				setTimeout(refreshStatus, 1000);
				setTimeout(renderHistory, 3000);
			});
		});

		var style = E('style', {}, [ [
			'.vpu-wrap{max-width:1040px}',
			'.vpu-card{border:1px solid #d4d4d4;border-radius:6px;padding:12px;margin:10px 0;background:#fff}',
			'.vpu-card h3{margin:0 0 8px;font-size:15px}',
			'.vpu-ver{font-weight:bold;color:#2d70b3}',
			'.vpu-row{display:flex;flex-wrap:wrap;gap:10px;align-items:flex-end}',
			'.vpu-row>div{flex:1 1 200px;min-width:150px}',
			'.vpu-row label{display:block;font-size:12px;color:#555;margin-bottom:3px}',
			'.vpu-btn{margin:2px 4px 2px 0}',
			'#vpu-blist{width:100%;font-family:monospace;font-size:12px;white-space:pre}',
			'#vpu-bres{max-height:170px;overflow:auto;font-family:monospace;font-size:12px;white-space:pre;margin-top:6px}',
			'#vpu-bdir,#vpu-boutdir{width:100%}',
			'.vpu-banner{border-left:4px solid #999;background:#f6f6f6;padding:8px 12px;border-radius:4px;margin:8px 0;font-size:13px}',
			'.vpu-banner-ok{border-left-color:#2d9c4f;background:#eef8f0}',
			'.vpu-banner-bad{border-left-color:#c0392b;background:#fdecea}',
			'.vpu-banner-warn{border-left-color:#e67e22;background:#fdf3e7}',
			'.vpu-probe{margin:8px 0;font-size:13px}',
			'.vpu-bar{height:16px;background:#eee;border-radius:8px;overflow:hidden;margin:6px 0}',
			'.vpu-bar-fill{height:100%;background:#2d70b3;transition:width .3s}',
			'.vpu-live{background:repeating-linear-gradient(45deg,#c0392b,#c0392b 10px,#e05545 10px,#e05545 20px)}',
			'.vpu-dim{color:#888;font-size:12px}',
			'.vpu-ok{color:#0a0;font-weight:bold}',
			'.vpu-err{color:#c00;font-weight:bold}',
			'.vpu-warn{color:#e67e22;font-weight:bold}',
			'.vpu-log{background:#111;color:#ddd;font-family:monospace;font-size:12px;padding:8px;border-radius:4px;min-height:50px;max-height:200px;overflow:auto;white-space:pre-wrap}',
			'.vpu-table{border-collapse:collapse;width:100%;font-size:12px}',
			'.vpu-table th,.vpu-table td{border-bottom:1px solid #eee;padding:4px 6px;text-align:left;vertical-align:top}',
			'.vpu-table th{color:#555;background:#fafafa}',
			'.vpu-clip{max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}'
		].join('') ]);

		var wrap = E('div', { 'class': 'vpu-wrap' }, [
			style,
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ E('span', { 'class': 'vpu-ver' }, [ 'vputrans ' + VPU_VER ]),
					'  ', E('span', { 'class': 'vpu-dim' }, [ _('RK3566 hardware transcode · local/online source auto-detection') ]) ]),
				banner, bar, statLine
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('① Source (type a local path or an online URL - auto-detected)') ]),
				E('div', { 'class': 'vpu-row' }, [
					E('div', { 'style': 'flex:3 1 420px' }, [ E('label', {}, [ _('Input / path / URL') ]), pathIn, srcList, srcHint, listHint ]),
					E('div', { 'style': 'flex:0 0 auto' }, [ btnList, btnProbe ])
				]),
				probBox
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('② Parameters') ]),
				E('div', { 'class': 'vpu-row' }, [
					E('div', {}, [ E('label', {}, [ _('Quality mode') ]), qualitySel ]),
					E('div', {}, [ E('label', {}, [ _('Bitrate (CBR)') ]), brIn ]),
					E('div', {}, [ E('label', {}, [ _('QP (CQP)') ]), qpIn ]),
					E('div', {}, [ E('label', {}, [ _('Output encoder') ]), encSel ])
				]),
				encHint,
				E('div', { 'class': 'vpu-row', 'style': 'margin-top:8px' }, [
					E('div', {}, [ E('label', {}, [ _('Target resolution') ]), resSel ]),
					E('div', {}, [ E('label', {}, [ _('Scaler (software-decode path only)') ]), scalerSel ]),
					E('div', {}, [ E('label', {}, [ _('Output frame rate') ]), fpsIn ]),
					E('div', {}, [ E('label', {}, [ _('Audio') ]), audioSel ])
				]),
				E('div', { 'class': 'vpu-row', 'style': 'margin-top:8px' }, [
					E('div', {}, [ E('label', {}, [ _('Trim start -ss (seconds)') ]), ssIn ]),
					E('div', {}, [ E('label', {}, [ _('Duration -t (seconds)') ]), durIn ]),
					E('div', {}, [ E('label', {}, [ _('Output container') ]), contSel ]),
					E('div', { 'style': 'flex:2 1 320px' }, [ E('label', {}, [ _('Output file (empty = automatic)') ]), outIn ])
				]),
				E('div', { 'style': 'margin-top:10px' }, [ btnRun, btnStop ])
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('③ Batch (a directory, or a list of files one per line)') ]),
				E('div', { 'class': 'vpu-row' }, [
					E('div', { 'style': 'flex:2' }, [ E('label', {}, [ _('Directory') ]), bdirIn ]),
					E('div', {}, [ E('label', {}, [ ' ' ]), btnScan ]),
					E('div', {}, [ E('label', {}, [ ' ' ]), E('label', {}, [ brecIn, ' ' + _('recursive') ]) ])
				]),
				E('div', { 'style': 'margin-top:8px' }, [
					E('label', {}, [ _('File list (one path per line - Chinese names are fine)') ]),
					bListArea
				]),
				E('div', { 'class': 'vpu-row', 'style': 'margin-top:8px' }, [
					E('div', { 'style': 'flex:2' }, [ E('label', {}, [ _('Output directory') ]), boutDirIn ]),
					E('div', {}, [ E('label', {}, [ ' ' ]), E('label', {}, [ bskipIn, ' ' + _('skip files that already have an output') ]) ]),
					E('div', { 'style': 'display:flex;gap:8px' }, [ E('label', {}, [ ' ' ]), btnBStart, btnBStop ])
				]),
				bStat,
				bRes
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('④ Progress and log') ]),
				logPre
			]),
			E('div', { 'class': 'vpu-card' }, [
			E('h3', {}, [ _('⑤ Recent jobs (newest 5)') ]),
				histBox
			])
		]);

		applyStatus(st);
		renderHistory();
		loadFileList();
		setTimeout(refreshStatus, 400);

		this._vpuTimer = setInterval(function() {
			if (!document.body.contains(wrap)) { clearInterval(self._vpuTimer); return; }
			refreshStatus();
			refreshBatch();
			renderHistory();   /* 🔴 原来漏了这句，Recent jobs 只在加载时刷一次 ✗ */
		}, 4000);
		refreshBatch();

		return wrap;
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
