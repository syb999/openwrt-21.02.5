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

var VPU_VER = 'v3.4.0';

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
			vexec([ 'history' ]),
			vexec([ 'clustercfg' ])
		]);
	},

	render: function(data) {
		var sres = data[1], hres = data[2], clres = data[3];
		var files = [];
		var st = parseJSON(sres.out, { state: 'idle' });
		var self = this;
		var running = (st.state === 'running');

		var el = {};
		function inputOpts() { return { 'type': 'text', 'class': 'vpu-in', 'style': 'width:100%;box-sizing:border-box' }; }
		function selectOpts() { return { 'class': 'vpu-in', 'style': 'width:100%;box-sizing:border-box' }; }

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
			E('option', { value: '240p' }, [ '240p' ]),
			E('option', { value: '360p' }, [ '360p' ]),
			E('option', { value: '480p' }, [ '480p' ]),
			E('option', { value: '720p' }, [ '720p' ]),
			E('option', { value: '1080p' }, [ '1080p' ]),
			E('option', { value: '1440p' }, [ '1440p (2K)' ]),
			E('option', { value: '2160p' }, [ '2160p (4K)' ])
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
			E('option', { value: 'ts' }, [ 'ts' ]),
			E('option', { value: 'gif' }, [ 'gif' + ' (' + _('animated picture') + ')' ])
		]);
		var outIn = E('input', Object.assign(inputOpts(), { placeholder: _('empty = generate automatically') }));

		/* ---- readouts ---- */
		var banner = E('div', { 'class': 'vpu-banner', id: 'vpu-banner' }, [ _('Reading status…') ]);
		var probBox = E('div', { 'class': 'vpu-probe', id: 'vpu-probe' }, [ E('span', { 'class': 'vpu-dim' }, [ _('Enter a path or URL and the source is probed automatically.') ]) ]);
		var barFill = E('div', { 'class': 'vpu-bar-fill', 'style': 'width:0%' });
		var bar = E('div', { 'class': 'vpu-bar' }, [ barFill ]);
		var statLine = E('div', { 'class': 'vpu-stats', id: 'vpu-stat' }, []);
		var statMeta = E('div', { 'class': 'vpu-meta', id: 'vpu-meta' }, [ '' ]);
		var logPre = E('pre', { 'class': 'vpu-log', id: 'vpu-log' }, [ '' ]);
		var histBox = E('div', { 'class': 'vpu-hist', id: 'vpu-hist' }, [ E('span', { 'class': 'vpu-dim' }, [ _('No history yet.') ]) ]);

		var btnRun = E('button', { 'class': 'btn cbi-button vpu-btn vpu-btn-main', id: 'vpu-btn-run' }, [ _('Start transcoding') ]);
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
		var btnBStart = E('button', { 'class': 'btn cbi-button vpu-btn vpu-btn-main', id: 'vpu-btn-brun' }, [ _('Start batch') ]);
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
			else {
				barFill.className = 'vpu-bar-fill' + (j.state === 'done' ? ' vpu-done' : '');
				barFill.style.width = Math.max(0, Math.min(100, pct)) + '%';
			}

			/* one pill per number: the old single sentence was unreadable */
			statLine.innerHTML = '';
			function addStat(lbl, val) {
				if (val === undefined || val === null || val === '') { return; }
				statLine.appendChild(E('div', { 'class': 'vpu-stat' }, [
					E('span', { 'class': 'vpu-stat-l' }, [ lbl ]),
					E('span', { 'class': 'vpu-stat-v' }, [ String(val) ])
				]));
			}
			if (timemode) {
				addStat(_('Processed'), fmtDur(j.out_time));
				addStat(_('Length'), live ? _('live stream') : _('unknown'));
			} else {
				addStat(_('Progress'), pct.toFixed(1) + '%');
				if (Number(j.total) > 0) addStat(_('Time'), fmtDur(j.out_time) + ' / ' + fmtDur(j.total));
			}
			addStat(_('Speed'), j.speed);
			addStat(_('FPS'), j.fps);
			addStat(_('Written'), fmtSize(j.outsize));
			addStat(_('Elapsed'), j.elapsed ? fmtDur(j.elapsed) : '');
			var meta = [];
			if (j.mode) meta.push(pipeText(j.mode, j.scaleeng, j.encoder));
			if (j.state === 'done' || j.state === 'stopped') meta.push(_('output file') + ': ' + (j.out || ''));
			if (j.error && j.state !== 'error' && isRun) meta.push(msg(j, 'error'));
			statMeta.textContent = meta.join('   ·   ');

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

		function chipUpdate(state) {
			if (!stateChip) { return; }
			var map = { running: 'run', done: 'ok', error: 'bad', stopped: 'stop', idle: '' };
			var word = { running: _('Running'), done: _('Finished'), error: _('Failed'), stopped: _('Stopped'), idle: _('Idle') };
			var k = map[state];
			stateChip.className = 'vpu-chip' + (k ? (' vpu-chip-' + k) : '');
			stateChip.textContent = word[state] || state || _('Idle');
		}
		function refreshStatus() {
			vexec([ 'status' ]).then(function(r) {
				var j = parseJSON(r.out, null);
				if (j) { applyStatus(j); chipUpdate(j.state); }
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

		/* ---- cluster: several boxes on one file -------------------------------
		 * The token and the node list live in /etc/config/vputrans: this view
		 * has no write ACL, so the backend stores them (same pattern as the
		 * batch list above).  A node is another box running this firmware; it
		 * is driven over HTTP through /cgi-bin/vputrans-node, so there is
		 * nothing to install and no ssh key to copy around.
		 */
		var clTokenIn = E('input', Object.assign(inputOpts(), {
			id: 'vpu-cltoken', autocomplete: 'off',
			placeholder: _('shared token (identical on every node)') }));
		var clNodesIn = E('textarea', {
			id: 'vpu-clnodes', rows: 3,
			style: 'width:100%;box-sizing:border-box;font-family:monospace;font-size:12px',
			placeholder: '10.80.1.126\n10.80.1.127:80' });
		var clSegIn = E('input', Object.assign(inputOpts(), { id: 'vpu-clseg', value: '0', style: 'width:90px' }));
		var clLocalIn = E('input', { id: 'vpu-cllocal', type: 'checkbox', checked: true });
		var btnClSave = E('button', { class: 'cbi-button vpu-btn', id: 'vpu-clsave' }, [ _('Save cluster settings') ]);
		var btnClCheck = E('button', { class: 'cbi-button vpu-btn', id: 'vpu-clcheck' }, [ _('Check nodes') ]);
		var btnClRun = E('button', { class: 'cbi-button vpu-btn vpu-btn-main', id: 'vpu-clrun' }, [ _('Cluster transcode') ]);
		var clMasterIn = E('input', Object.assign(inputOpts(), {
			id: 'vpu-clmaster', placeholder: _('master ip, e.g. 10.80.1.125') }));
		var clFollowIn = E('input', { id: 'vpu-clfollow', type: 'checkbox' });
		var clIvIn = E('input', Object.assign(inputOpts(), { id: 'vpu-cliv', value: '15', style: 'width:80px' }));
		var btnClRoster = E('button', { class: 'cbi-button vpu-btn', id: 'vpu-clroster' }, [ _('Refresh nodes') ]);
		var rosterBox = E('div', { style: 'margin-top:2px' }, []);
		var clStat = E('div', { class: 'vpu-dim', style: 'margin-top:6px' }, [ '' ]);
		var clBox = E('div', { style: 'margin-top:6px' }, []);

		function clNodeLines() {
			return (clNodesIn.value || '').split('\n')
				.map(function (x) { return x.trim(); })
				.filter(function (x) { return x.length > 0; });
		}
		function loadClusterCfg(j) {
			if (!j) { return; }
			clTokenIn.value = j.token || '';
			clNodesIn.value = (j.nodes || []).join('\n');
			clSegIn.value = String(j.segments || 0);
			clLocalIn.checked = (String(j.local) !== '0');
			clMasterIn.value = j.master || '';
			clFollowIn.checked = (String(j.follow) === '1');
			clIvIn.value = String(j.interval || 15);
		}
		function renderRoster(j) {
			rosterBox.innerHTML = '';
			if (!j || !j.ok) {
				rosterBox.appendChild(E('div', { class: 'vpu-dim' }, [ _('Cannot read the node list') ]));
				return;
			}
			var tbl = E('table', { class: 'vpu-table' });
			var hr = E('tr', {});
			[ _('Node'), _('Seen'), _('Version'), _('CPUs'), _('State') ].forEach(function (h) {
				hr.appendChild(E('th', {}, [ h ]));
			});
			tbl.appendChild(hr);
			(j.nodes || []).forEach(function (n) {
				var tr = E('tr', {});
				tr.appendChild(E('td', { class: 'vpu-clip' }, [ n.node ]));
				tr.appendChild(E('td', {}, [ n.age + ' s' ]));
				tr.appendChild(E('td', {}, [ n.ver || '' ]));
				tr.appendChild(E('td', {}, [ String(n.cpus || '') ]));
				tr.appendChild(E('td', { class: n.busy ? 'vpu-warn' : 'vpu-ok' },
					[ n.busy ? _('busy') : _('ready') ]));
				tbl.appendChild(tr);
			});
			if (!(j.nodes || []).length) {
				tbl.appendChild(E('tr', {}, [ E('td', { colspan: 5, class: 'vpu-dim' },
					[ _('No node has announced itself yet. On the other boxes set Master to this box\'s address, tick "follow" and save.') ]) ]));
			}
			rosterBox.appendChild(tbl);
			rosterBox.appendChild(E('div', { class: 'vpu-dim', style: 'margin-top:6px' }, [
				_('This box') + ': ' + (j.me || '?') + '  ·  ' + _('interval') + ' ' + (j.interval || 15) +
				' s  ·  ' + _('a node counts as present for 3 intervals')
			]));
		}
		function refreshRoster() {
			vexec([ 'roster' ]).then(function (r) { renderRoster(parseJSON(r.out, null)); });
		}
		function renderNodes(j) {
			clBox.innerHTML = '';
			if (!j || !j.ok) { clStat.textContent = _('Cannot read the cluster status'); return; }
			var tbl = E('table', { class: 'vpu-table' });
			var hr = E('tr', {});
			[ _('Node'), _('State'), _('Version') ].forEach(function (h) { hr.appendChild(E('th', {}, [ h ])); });
			tbl.appendChild(hr);
			(j.nodes || []).forEach(function (n) {
				var tr = E('tr', {});
				tr.appendChild(E('td', { class: 'vpu-clip' }, [ n.node ]));
				var txt = n.up ? (n.busy ? _('busy') : _('ready')) : _('unreachable');
				tr.appendChild(E('td', { class: n.up ? (n.busy ? 'vpu-warn' : 'vpu-ok') : 'vpu-err' }, [ txt ]));
				tr.appendChild(E('td', {}, [ n.ver || '' ]));
				tbl.appendChild(tr);
			});
			var lc = j.local || {};
			var lt = E('tr', {});
			lt.appendChild(E('td', { class: 'vpu-clip' }, [ (lc.node || '?') + '  (' + _('this box') + ')' ]));
			lt.appendChild(E('td', { class: 'vpu-ok' }, [ _('ready') ]));
			lt.appendChild(E('td', {}, [ lc.ver || '' ]));
			tbl.appendChild(lt);
			clBox.appendChild(tbl);
			clStat.textContent = j.token_set ? _('Cluster settings loaded')
				: _('No token yet - type one and press Save cluster settings');
		}
		function refreshNodes() {
			vexec([ 'clusterlist' ]).then(function (r) { renderNodes(parseJSON(r.out, null)); });
		}
		function renderCluster(j) {
			if (!j) { return; }
			var st = j.state || 'idle';
			if (st === 'idle') { clStat.textContent = _('Idle'); clBox.innerHTML = ''; return; }
			var done = 0;
			(j.segs || []).forEach(function (x) { if (x.state === 'done') { done++; } });
			var word = (st === 'finished') ? _('finished') : stateText(st);
			clStat.textContent = _('Segments') + ' ' + done + '/' + (j.n || 0) + '  ·  ' + word;
			clBox.innerHTML = '';
			var tbl = E('table', { class: 'vpu-table' });
			var hr = E('tr', {});
			[ _('Segment'), _('Node'), _('State'), '%' ].forEach(function (h) { hr.appendChild(E('th', {}, [ h ])); });
			tbl.appendChild(hr);
			(j.segs || []).forEach(function (x) {
				var tr = E('tr', {});
				tr.appendChild(E('td', {}, [ String(x.i) ]));
				tr.appendChild(E('td', { class: 'vpu-clip' }, [ x.node || '-' ]));
				var cls = (x.state === 'done') ? 'vpu-ok' : ((x.state === 'error') ? 'vpu-err' : 'vpu-warn');
				tr.appendChild(E('td', { class: cls }, [ stateText(x.state) ]));
				tr.appendChild(E('td', {}, [ (Number(x.pct) || 0).toFixed(1) ]));
				tbl.appendChild(tr);
			});
			clBox.appendChild(tbl);
		}
		function refreshCluster() {
			vexec([ 'clusterstatus' ]).then(function (r) { renderCluster(parseJSON(r.out, null)); });
		}
		btnClSave.addEventListener('click', function () {
			btnClSave.disabled = true;
			vexec([ 'clusterset',
				'--token', clTokenIn.value.trim(),
				'--nodes', clNodeLines().join(','),
				'--segments', (clSegIn.value.trim() || '0'),
				'--local', clLocalIn.checked ? '1' : '0',
				'--master', clMasterIn.value.trim(),
				'--follow', clFollowIn.checked ? '1' : '0',
				'--interval', (clIvIn.value.trim() || '15') ]).then(function (r) {
				btnClSave.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					banner.className = 'vpu-banner vpu-banner-bad';
					banner.textContent = _('Cannot save the cluster settings') + ': ' + (r.err || _('Unknown error'));
					return;
				}
				banner.className = 'vpu-banner';
				banner.textContent = _('Cluster settings saved');
				refreshNodes();
				refreshRoster();
			});
		});
		btnClCheck.addEventListener('click', function () {
			clStat.textContent = _('Checking…');
			refreshNodes();
		});
		function clusterArgs() {
			var a = [ 'cluster', pathIn.value.trim(),
				'--quality', qualitySel.value,
				'--codec', encSel.value,
				'--res', resSel.value,
				'--scaler', scalerSel.value,
				'--fps', (fpsIn.value.trim() || 'source'),
				'--audio', audioSel.value,
				'--container', contSel.value,
				'--local', clLocalIn.checked ? '1' : '0',
				'--segments', (clSegIn.value.trim() || '0') ];
			var n = clNodeLines();
			if (n.length) { a.push('--nodes', n.join(',')); }
			if (fxSel.value !== 'none') { a.push('--effect', fxSel.value); }
			if (qualitySel.value === 'crf') { a.push('--qp', (qpIn.value.trim() || '26')); }
			else if (brIn.value.trim()) { a.push('--bitrate', brIn.value.trim()); }
			if (ssIn.value.trim() && ssIn.value.trim() !== '0') { a.push('--ss', ssIn.value.trim()); }
			if (durIn.value.trim() && durIn.value.trim() !== '0') { a.push('--dur', durIn.value.trim()); }
			if (outIn.value.trim()) { a.push('--out', outIn.value.trim()); }
			return a;
		}
		btnClRun.addEventListener('click', function () {
			if (!pathIn.value.trim()) {
				banner.className = 'vpu-banner vpu-banner-bad';
				banner.textContent = _('Please enter a local path or URL first.');
				return;
			}
			btnClRun.disabled = true;
			vexec(clusterArgs()).then(function (r) {
				btnClRun.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					banner.className = 'vpu-banner vpu-banner-bad';
					banner.textContent = _('Cannot start') + ': ' + (msg(j, 'error') || r.err || _('Unknown error, please check the log'));
					return;
				}
				banner.className = 'vpu-banner';
				banner.textContent = fmtTpl(_('Cluster job started: %s segments, %s workers'), [ String(j.segments), String(j.workers) ]);
				refreshStatus();
				refreshCluster();
			});
		});

		/* ---- effects / logo / pip / join / gif -------------------------------
		 * Everything here rides on what ffmpeg-rkrga can actually do on this
		 * SoC (checked on the box, not assumed):
		 *   flip / rotate  -> vpp_rkrga (RGA hardware, zero copy)
		 *   black and white-> VPU decode + format=gray, hardware encode
		 *   gif            -> RGA NV12->BGRA in hardware, software gif writer
		 *   logo / pip     -> software overlay/hstack after one hwdownload
		 *                     (overlay_rkrga needs DRM_PRIME frames and this
		 *                     kernel brings up no real DRM device)
		 *   two files      -> both re-encoded to the same parameters, then
		 *                     concatenated with -c copy
		 */
		var fxSel = E('select', selectOpts(), [
			E('option', { value: 'none' }, [ _('No effect') ]),
			E('option', { value: 'gray' }, [ _('Black and white') ]),
			E('option', { value: 'hflip' }, [ _('Mirror horizontally') ]),
			E('option', { value: 'vflip' }, [ _('Mirror vertically') ]),
			E('option', { value: 'reversal' }, [ _('Turn 180°') ]),
			E('option', { value: 'rot90' }, [ _('Rotate 90° right') ]),
			E('option', { value: 'rot270' }, [ _('Rotate 90° left') ])
		]);
		var logoIn = E('input', Object.assign(inputOpts(), { placeholder: '/root/logo.png   ' + _('(png with transparency)') }));
		var logoPosSel = E('select', selectOpts(), [
			E('option', { value: 'tr' }, [ _('top right') ]),
			E('option', { value: 'tl' }, [ _('top left') ]),
			E('option', { value: 'br' }, [ _('bottom right') ]),
			E('option', { value: 'bl' }, [ _('bottom left') ])
		]);
		var logoSizeIn = E('input', Object.assign(inputOpts(), { placeholder: _('empty = original size, or WxH, e.g. 160x48') }));
		var pipIn = E('input', Object.assign(inputOpts(), { placeholder: '/mnt/b.mp4   ' + _('(second video)') }));
		var pipPosSel = E('select', selectOpts(), [
			E('option', { value: 'br' }, [ _('small in bottom right') ]),
			E('option', { value: 'bl' }, [ _('small in bottom left') ]),
			E('option', { value: 'tr' }, [ _('small in top right') ]),
			E('option', { value: 'tl' }, [ _('small in top left') ]),
			E('option', { value: 'side' }, [ _('side by side') ]),
			E('option', { value: 'top' }, [ _('one above the other') ])
		]);
		var pipScaleIn = E('input', Object.assign(inputOpts(), { value: '30', style: 'width:70px', placeholder: '30' }));
		var joinIn = E('input', Object.assign(inputOpts(), { placeholder: '/mnt/b.mp4   ' + _('(appended after the source)') }));
		var gifFpsIn = E('input', Object.assign(inputOpts(), { value: '12', style: 'width:70px' }));
		/* A/V sync repair: a page of its own, because "the sound drifts after a
		 * few minutes" has nothing to do with effects */
		var syncSrcIn = E('input', Object.assign(inputOpts(), {
			id: 'vpu-sync-src', list: 'vpu-src-list', autocomplete: 'off',
			placeholder: '/mnt/video/a.mkv   ' + _('or') + '   http://host/a.mkv' }));
		var syncAudioSel = E('select', selectOpts(), [
			E('option', { value: 'aac' }, [ _('Rebuild audio clock (AAC)') ]),
			E('option', { value: 'none' }, [ _('Drop the audio (video only)') ])
		]);
		var syncContSel = E('select', selectOpts(), [
			E('option', { value: 'auto' }, [ _('Auto (file -> mp4)') ]),
			E('option', { value: 'mp4' }, [ 'mp4' ]),
			E('option', { value: 'mkv' }, [ 'mkv' ])
		]);
		var syncOutIn = E('input', Object.assign(inputOpts(), {
			id: 'vpu-sync-out', placeholder: _('empty = <name>_sync.mp4 next to the source') }));
		var btnSyncProbe = E('button', { 'class': 'btn cbi-button vpu-btn', id: 'vpu-sync-probe' }, [ _('Check the track lengths') ]);
		var btnSyncRun = E('button', { 'class': 'btn cbi-button vpu-btn vpu-btn-main', id: 'vpu-sync-run' }, [ _('Repair and write the output') ]);
		var syncBox = E('div', { id: 'vpu-sync-box', 'style': 'margin-top:14px' }, []);

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
			if (fxSel.value !== 'none') args.push('--effect', fxSel.value);
			if (logoIn.value.trim()) {
				args.push('--logo', logoIn.value.trim(), '--logo-pos', logoPosSel.value);
				if (logoSizeIn.value.trim()) args.push('--logo-size', logoSizeIn.value.trim());
			}
			if (pipIn.value.trim()) {
				args.push('--pip', pipIn.value.trim(), '--pip-pos', pipPosSel.value,
					'--pip-scale', (pipScaleIn.value.trim() || '30'));
			}
			if (joinIn.value.trim()) args.push('--join', joinIn.value.trim());

			if (contSel.value === 'gif') args.push('--gif-fps', (gifFpsIn.value.trim() || '12'));
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
			'.vpu-wrap{max-width:1120px;color:#1f2937;font-size:13px}',
			/* header */
			'.vpu-head{display:flex;align-items:flex-start;justify-content:space-between;gap:14px;flex-wrap:wrap}',
			'.vpu-title{font-size:18px;font-weight:700;color:#0f172a;display:flex;align-items:center;gap:9px}',
			'.vpu-sub{font-size:12px;color:#94a3b8;margin-top:4px}',
			'.vpu-badge{background:#eef2ff;color:#3b5bdb;font-size:11px;font-weight:700;padding:2px 8px;border-radius:6px;letter-spacing:.2px}',
			'.vpu-chip{display:inline-flex;align-items:center;gap:7px;font-size:12px;font-weight:600;padding:5px 13px;border-radius:999px;background:#f1f5f9;color:#64748b}',
			'.vpu-chip:before{content:"";width:7px;height:7px;border-radius:50%;background:currentColor;opacity:.85}',
			'.vpu-chip-run{background:#eff6ff;color:#1d4ed8}',
			'.vpu-chip-ok{background:#ecfdf5;color:#047857}',
			'.vpu-chip-bad{background:#fef2f2;color:#b91c1c}',
			'.vpu-chip-stop{background:#fffbeb;color:#b45309}',
			/* progress + stats */
			'.vpu-bar{height:6px;background:#eef2f7;border-radius:999px;overflow:hidden;margin-top:16px}',
			'.vpu-bar-fill{height:100%;width:0;background:linear-gradient(90deg,#60a5fa,#2563eb);border-radius:999px;transition:width .35s}',
			'.vpu-bar-fill.vpu-done{background:linear-gradient(90deg,#34d399,#059669)}',
			'.vpu-live{background:repeating-linear-gradient(45deg,#dc2626,#dc2626 8px,#f87171 8px,#f87171 16px)}',
			'.vpu-stats{display:flex;flex-wrap:wrap;gap:10px;margin-top:14px}',
			'.vpu-stat{background:#f8fafc;border:1px solid #eef2f7;border-radius:10px;padding:7px 13px;min-width:84px}',
			'.vpu-stat-l{display:block;font-size:10px;font-weight:700;letter-spacing:.6px;text-transform:uppercase;color:#9aa5b4}',
			'.vpu-stat-v{display:block;font-size:14px;font-weight:650;color:#0f172a;margin-top:1px;font-variant-numeric:tabular-nums}',
			'.vpu-meta{margin-top:9px;font-size:11px;color:#94a3b8;word-break:break-all;line-height:1.5}',
			/* tabs */
			'.vpu-tabs{display:inline-flex;gap:4px;background:#f1f5f9;padding:4px;border-radius:12px;margin:16px 0 0}',
			'.vpu-tab{border:0;background:transparent;padding:8px 16px;border-radius:9px;font-size:13px;font-weight:600;color:#64748b;cursor:pointer}',
			'.vpu-tab:hover{color:#1d4ed8}',
			'.vpu-tab-on{background:#fff;color:#1d4ed8;box-shadow:0 1px 2px rgba(16,24,40,.1)}',
			'.vpu-panel{display:none;padding-top:16px}',
			/* cards */
			'.vpu-card{background:#fff;border:1px solid #e8edf3;border-radius:14px;padding:16px 18px;margin:0 0 14px;box-shadow:0 1px 2px rgba(16,24,40,.04)}',
			'.vpu-card h3{margin:0 0 14px;font-size:13px;font-weight:700;color:#334155;display:flex;align-items:center;gap:8px}',
			'.vpu-card h3:before{content:"";width:3px;height:14px;border-radius:2px;background:#3b82f6}',
			'.vpu-sub2{font-size:10px;font-weight:700;color:#a2adbb;letter-spacing:.7px;text-transform:uppercase;margin:18px 0 9px}',
			'.vpu-sub2:first-of-type{margin-top:0}',
			/* fields */
			'.vpu-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px 16px}',
			'.vpu-grid .vpu-full{grid-column:1/-1}',
			'.vpu-grid label,.vpu-row label{display:block;font-size:11.5px;font-weight:600;color:#64748b;margin-bottom:5px}',
			'.vpu-row{display:flex;flex-wrap:wrap;gap:14px;align-items:flex-end}',
			'.vpu-row>div{flex:1 1 200px;min-width:150px}',
			'input.vpu-in,select.vpu-in,textarea.vpu-in{height:34px;padding:0 10px;border:1px solid #d8e0ea;border-radius:9px;background:#fff;color:#0f172a;font-size:13px;line-height:32px}',
			'select.vpu-in{padding:0 8px}',
			'input.vpu-in:focus,select.vpu-in:focus,textarea.vpu-in:focus{outline:0;border-color:#93b4f5;box-shadow:0 0 0 3px rgba(59,130,246,.14)}',
			'textarea.vpu-in{height:auto;padding:8px 10px;line-height:1.5;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px}',
			'.vpu-checks{display:flex;gap:22px;flex-wrap:wrap;margin-top:14px}',
			'.vpu-checks label{display:inline-flex;align-items:center;gap:7px;font-size:13px;color:#334155;margin:0}',
			'.vpu-checks input[type=checkbox]{width:16px;height:16px;accent-color:#2563eb;margin:0}',
			/* buttons */
			'.vpu-actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:16px;padding-top:14px;border-top:1px solid #f1f5f9}',
			'.vpu-btn{height:34px;padding:0 16px;border-radius:9px;font-size:13px;font-weight:600;cursor:pointer}',
			'.vpu-btn-main{background:#2563eb;border:1px solid #2563eb;color:#fff;box-shadow:0 1px 2px rgba(37,99,235,.3)}',
			'.vpu-btn-main:hover{background:#1d4ed8;border-color:#1d4ed8}',
			'.vpu-btn-main[disabled]{background:#a9c2f7;border-color:#a9c2f7;box-shadow:none;cursor:default}',
			/* banner / probe / log / tables */
			'.vpu-banner{border-left:3px solid #cbd5e1;background:#f8fafc;padding:9px 13px;border-radius:0 9px 9px 0;margin-top:14px;font-size:13px;color:#334155}',
			'.vpu-banner-ok{border-left-color:#059669;background:#f0fdf4}',
			'.vpu-banner-bad{border-left-color:#dc2626;background:#fef2f2}',
			'.vpu-banner-warn{border-left-color:#d97706;background:#fffbeb}',
			'.vpu-probe{margin-top:14px;padding:11px 13px;background:#f8fafc;border:1px solid #eef2f7;border-radius:10px;font-size:12.5px;color:#3f4b5b}',
			'.vpu-log{background:#0b1220;color:#cbd5e1;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.6;padding:13px;border-radius:11px;min-height:70px;max-height:280px;overflow:auto;white-space:pre-wrap;margin:0}',
			'.vpu-table{border-collapse:collapse;width:100%;font-size:12px}',
			'.vpu-table th,.vpu-table td{border-bottom:1px solid #f1f5f9;padding:7px 9px;text-align:left;vertical-align:top}',
			'.vpu-table th{color:#7c8794;background:#fafbfc;font-weight:600;font-size:11px;letter-spacing:.3px;text-transform:uppercase}',
			'.vpu-table tr:last-child td{border-bottom:0}',
			'.vpu-table tbody tr:hover td{background:#fbfdff}',
			'.vpu-dim{color:#94a3b8;font-size:12px;line-height:1.6}',
			'.vpu-ok{color:#047857;font-weight:600}',
			'.vpu-err{color:#b91c1c;font-weight:600}',
			'.vpu-warn{color:#b45309;font-weight:600}',
			'.vpu-clip{max-width:230px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
			'#vpu-blist{width:100%;box-sizing:border-box;min-height:120px}',
			'#vpu-bres{max-height:170px;overflow:auto;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;white-space:pre;margin-top:8px}',
			'#vpu-bdir,#vpu-boutdir{width:100%;box-sizing:border-box}',
			'#vpu-clnodes{width:100%;box-sizing:border-box;min-height:74px}'
		].join('') ]);

		/* ---- tabs ---------------------------------------------------------
		 * Seven stacked cards had turned into a wall of fields; the panels are
		 * the same widgets, only grouped: what you touch per job (source +
		 * parameters) stays in the first tab, the exotic parts moved away.
		 */
		var stateChip = E('span', { 'class': 'vpu-chip', 'id': 'vpu-chip' }, [ _('Idle') ]);
		var panels = {}, tabBtns = {};

		function showTab(key) {
			var k;
			/* 'block', not '': an empty inline style falls back to the class
			 * rule (.vpu-panel{display:none}) and the tab stays blank */
			for (k in panels) { panels[k].style.display = (k === key) ? 'block' : 'none'; }
			for (k in tabBtns) { tabBtns[k].className = 'vpu-tab' + (k === key ? ' vpu-tab-on' : ''); }
			if (key === 'cluster') { refreshNodes(); refreshRoster(); }
			if (key === 'tasks') { renderHistory(); }
		}
		function mkTab(key, text) {
			var b = E('button', { 'class': 'vpu-tab', 'type': 'button' }, [ text ]);
			b.addEventListener('click', function () { showTab(key); });
			tabBtns[key] = b;
			return b;
		}
		function mkPanel(key, children) {
			var p = E('div', { 'class': 'vpu-panel' }, children);
			panels[key] = p;
			return p;
		}

		/* ---- transcode tab ---- */
		var pTranscode = mkPanel('transcode', [
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Source') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Input / path / URL') ]), pathIn, srcList, srcHint, listHint ])
				]),
				E('div', { 'class': 'vpu-actions' }, [ btnList, btnProbe ]),
				probBox
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Parameters') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', {}, [ E('label', {}, [ _('Quality mode') ]), qualitySel ]),
					E('div', {}, [ E('label', {}, [ _('Bitrate (CBR)') ]), brIn ]),
					E('div', {}, [ E('label', {}, [ _('QP (CQP)') ]), qpIn ]),
					E('div', {}, [ E('label', {}, [ _('Output encoder') ]), encSel ]),
					E('div', {}, [ E('label', {}, [ _('Target resolution') ]), resSel ]),
					E('div', {}, [ E('label', {}, [ _('Scaler (software-decode path only)') ]), scalerSel ]),
					E('div', {}, [ E('label', {}, [ _('Output frame rate') ]), fpsIn ]),
					E('div', {}, [ E('label', {}, [ _('Audio') ]), audioSel ]),
					E('div', {}, [ E('label', {}, [ _('Trim start -ss (seconds)') ]), ssIn ]),
					E('div', {}, [ E('label', {}, [ _('Duration -t (seconds)') ]), durIn ]),
					E('div', {}, [ E('label', {}, [ _('Output container') ]), contSel ]),
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Output file (empty = automatic)') ]), outIn ])
				]),
				encHint,
				E('div', { 'class': 'vpu-actions' }, [ btnRun, btnStop ])
			])
		]);

		/* ---- batch tab ---- */
		var pBatch = mkPanel('batch', [
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Batch (a directory, or a list of files one per line)') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Directory') ]), bdirIn ]),
					E('div', {}, [ E('label', {}, [ _('Output directory') ]), boutDirIn ]),
					E('div', {}, [ E('label', {}, [ ' ' ]), E('label', {}, [ brecIn, ' ' + _('recursive') ]),
						E('label', {}, [ bskipIn, ' ' + _('skip files that already have an output') ]) ])
				]),
				E('div', { 'style': 'margin-top:10px' }, [
					E('label', {}, [ _('File list (one path per line - Chinese names are fine)') ]),
					bListArea
				]),
				E('div', { 'class': 'vpu-actions' }, [ btnScan, btnBStart, btnBStop ]),
				bStat, bRes
			])
		]);

		/* ---- cluster tab ---- */
		var pCluster = mkPanel('cluster', [
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Cluster settings') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Shared token (identical on every node)') ]), clTokenIn ]),
					E('div', {}, [ E('label', {}, [ _('Master IP') ]), clMasterIn ]),
					E('div', {}, [ E('label', {}, [ _('Announce interval (s)') ]), clIvIn ]),
					E('div', {}, [ E('label', {}, [ _('Segments (0 = auto)') ]), clSegIn ])
				]),
				E('div', { 'class': 'vpu-checks' }, [
					E('label', {}, [ clFollowIn, _('follow the master (run the beacon)') ]),
					E('label', {}, [ clLocalIn, _('this box also encodes') ])
				]),
				E('div', { 'style': 'margin-top:12px' }, [
					E('label', {}, [ _('Manual node list (one per line; leave empty to use the auto-discovered nodes)') ]),
					clNodesIn
				]),
				E('div', { 'class': 'vpu-actions' }, [ btnClSave, btnClCheck, btnClRoster, btnClRun ]),
				clStat, clBox
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Nodes that announced themselves (followers)') ]),
				rosterBox
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('How it works') ]),
				E('div', { 'class': 'vpu-dim' }, [
					_('Set the same token on every box and put this box\'s IP into Master on the others, tick "follow" there: each follower then announces itself every N seconds and this box lists it above. Start a job here and every announced node joins automatically - the source is cut into keyframe-aligned pieces, every box encodes one, this box joins them. Stop stops all workers.')
				])
			])
		]);

		/* ---- effects tab ---- */
		var pEffects = mkPanel('effects', [
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Effects, logo, second video and GIF') ]),
				E('div', { 'class': 'vpu-sub2' }, [ _('Effect') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', {}, [ E('label', {}, [ _('Picture effect') ]), fxSel ]),
					E('div', {}, [ E('label', {}, [ _('GIF frame rate') ]), gifFpsIn ])
				]),
				E('div', { 'class': 'vpu-sub2' }, [ _('Logo') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Logo (png)') ]), logoIn ]),
					E('div', {}, [ E('label', {}, [ _('Position') ]), logoPosSel ]),
					E('div', {}, [ E('label', {}, [ _('Logo size') ]), logoSizeIn ])
				]),
				E('div', { 'class': 'vpu-sub2' }, [ _('Second video') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Second video (picture in picture / side by side)') ]), pipIn ]),
					E('div', {}, [ E('label', {}, [ _('Layout') ]), pipPosSel ]),
					E('div', {}, [ E('label', {}, [ _('PIP width %') ]), pipScaleIn ])
				]),
				E('div', { 'class': 'vpu-sub2' }, [ _('Merge and clock') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Append a second video (one file after another)') ]), joinIn ])
				]),
				E('div', { 'class': 'vpu-dim', 'style': 'margin-top:12px' }, [
					_('Effect and GIF work with the shipped transcoder (flip/rotate run on the RGA, black and white keeps the hardware encoder). Resync rebuilds the video clock from the frame index and forces a constant frame rate - that is the fix for "plays fine, then the audio drifts, seeking helps"; the audio side needs the aresample filter, the backend says so when it is missing. Logo and picture in picture need the overlay filter as well. Cluster mode uses the effect, the other options are ignored.')
				])
			])
		]);

		/* ---- A/V sync page ---- */
		function syncVerdict(vd, ad) {
			syncBox.innerHTML = '';
			var drift = vd - ad;
			var tbl = E('table', { 'class': 'vpu-table' });
			var hr = E('tr', {});
			[ _('Track'), _('Length'), _('Codec') ].forEach(function (h) { hr.appendChild(E('th', {}, [ h ])); });
			tbl.appendChild(hr);
			var r1 = E('tr', {});
			r1.appendChild(E('td', {}, [ _('Video') ]));
			r1.appendChild(E('td', {}, [ fmtDur(vd) + '  (' + vd.toFixed(3) + ' s)' ]));
			r1.appendChild(E('td', {}, [ _('hardware decode') ]));
			tbl.appendChild(r1);
			var r2 = E('tr', {});
			r2.appendChild(E('td', {}, [ _('Audio') ]));
			r2.appendChild(E('td', {}, [ ad > 0 ? (fmtDur(ad) + '  (' + ad.toFixed(3) + ' s)') : _('no audio track') ]));
			r2.appendChild(E('td', {}, [ _('re-encoded to AAC') ]));
			tbl.appendChild(r2);
			var r3 = E('tr', {});
			r3.appendChild(E('td', { 'class': 'vpu-warn' }, [ _('Difference') ]));
			r3.appendChild(E('td', { 'class': 'vpu-warn' }, [ (drift >= 0 ? '+' : '') + drift.toFixed(3) + ' s' ]));
			r3.appendChild(E('td', {}, [ (Math.abs(drift) < 0.05) ? _('nothing to fix') : _('this is what drifts the sound') ]));
			tbl.appendChild(r3);
			syncBox.appendChild(tbl);
			if (ad <= 0) {
				syncBox.appendChild(E('div', { 'class': 'vpu-banner vpu-banner-warn' }, [
					_('This source has no audio track: only the video timeline can be rebuilt.') ]));
			} else if (Math.abs(drift) < 0.05) {
				syncBox.appendChild(E('div', { 'class': 'vpu-banner vpu-banner-ok' }, [
					_('The two tracks are the same length. If the sound still creeps out of step during playback, the drift is inside the audio stream (a broken sample rate) - repair still helps, it glues the audio to the rebuilt video clock.') ]));
			} else {
				syncBox.appendChild(E('div', { 'class': 'vpu-banner vpu-banner-warn' }, [
					_('The tracks differ by %s s: repair rebuilds the video timeline from the frame index and resamples the audio onto it.')
						.replace('%s', Math.abs(drift).toFixed(3)) ]));
			}
		}
		function syncProbe() {
			var src = syncSrcIn.value.trim();
			if (!src) {
				syncBox.innerHTML = '';
				syncBox.appendChild(E('div', { 'class': 'vpu-dim' }, [ _('Enter a path or URL first.') ]));
				return;
			}
			syncBox.innerHTML = '';
			syncBox.appendChild(E('div', { 'class': 'vpu-dim' }, [ _('Reading the source…') ]));
			vexec([ 'probe', src ]).then(function (r) {
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					syncBox.innerHTML = '';
					syncBox.appendChild(E('div', { 'class': 'vpu-err' }, [
						_('Cannot read this source') + ': ' + (msg(j, 'error') || r.err || _('Unknown error')) ]));
					return;
				}
				syncVerdict(Number(j.vdur) || 0, Number(j.adur) || 0);
			});
		}
		btnSyncProbe.addEventListener('click', syncProbe);
		btnSyncRun.addEventListener('click', function() {
			var src = syncSrcIn.value.trim();
			if (!src) { syncProbe(); return; }
			var a = [ 'run', src, '--resync', '--audio', syncAudioSel.value,
				'--container', syncContSel.value ];
			var o = syncOutIn.value.trim();
			if (!o && !isUrl(src)) {
				/* never aim at the source file: a repair writes a new file
				 * called <name>_sync.<ext> next to it */
				o = dirname(src) + '/' + stripExt(basename(src)) + '_sync.' +
					(syncContSel.value === 'mkv' ? 'mkv' : 'mp4');
			}
			if (o) a.push('--out', o);
			btnSyncRun.disabled = true;
			vexec(a).then(function(r) {
				btnSyncRun.disabled = false;
				var j = parseJSON(r.out, null);
				if (!j || !j.ok) {
					banner.className = 'vpu-banner vpu-banner-bad';
					banner.textContent = _('Cannot start') + ': ' + (msg(j, 'error') || r.err || _('Unknown error, please check the log'));
					return;
				}
				banner.className = 'vpu-banner';
				banner.textContent = _('Repair started…');
				var no = msg(j, 'notice');
				if (no) banner.appendChild(E('div', { 'class': 'vpu-dim' }, [ no ]));
				refreshStatus();
				setTimeout(renderHistory, 1500);
			});
		});

		var pSync = mkPanel('sync', [
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Repair audio/video sync') ]),
				E('div', { 'class': 'vpu-grid' }, [
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Input / path / URL') ]), syncSrcIn ]),
					E('div', {}, [ E('label', {}, [ _('Audio') ]), syncAudioSel ]),
					E('div', {}, [ E('label', {}, [ _('Output container') ]), syncContSel ]),
					E('div', { 'class': 'vpu-full' }, [ E('label', {}, [ _('Output file') ]), syncOutIn ])
				]),
				E('div', { 'class': 'vpu-actions' }, [ btnSyncProbe, btnSyncRun ]),
				syncBox
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('What it does') ]),
				E('div', { 'class': 'vpu-dim' }, [
					_('Audio and video keep separate clocks. If the video clock comes from the timestamps of a damaged source, or the audio arrives at a slightly different rate, the sound slowly creeps out of step - the classic "starts fine, drifts after a few minutes, seeking helps for a moment". Repair rebuilds the video timeline from the frame index (constant frame rate, regenerated timestamps, no re-scaling) and re-encodes the audio with aresample=async so it is glued to that clock. Pixels are not touched and no effect is applied. Measured on this box: a 20.400 s video against 20.000 s of audio comes out as 20.020 s / 20.010 s, 600 frames, 0 decode errors.') ])
			])
		]);

		/* ---- tasks tab ---- */
		var pTasks = mkPanel('tasks', [
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Progress and log') ]),
				logPre
			]),
			E('div', { 'class': 'vpu-card' }, [
				E('h3', {}, [ _('Recent jobs (newest 5)') ]),
				histBox
			])
		]);

		var wrap = E('div', { 'class': 'vpu-wrap' }, [
			style,
			E('div', { 'class': 'vpu-card' }, [
				E('div', { 'class': 'vpu-head' }, [
					E('div', {}, [
						E('div', { 'class': 'vpu-title' }, [
							E('span', {}, [ _('Video transcoder') ]),
							E('span', { 'class': 'vpu-badge' }, [ VPU_VER ])
						]),
						E('div', { 'class': 'vpu-sub' }, [ _('RK3566 hardware transcode · local/online source auto-detection') ])
					]),
					stateChip
				]),
				bar, statLine, statMeta, banner
			]),
			E('div', { 'class': 'vpu-tabs' }, [
				mkTab('transcode', _('Transcode')),
				mkTab('batch', _('Batch')),
				mkTab('cluster', _('Cluster')),
				mkTab('sync', _('A/V sync')),
				mkTab('effects', _('Effects & GIF')),
				mkTab('tasks', _('Tasks'))
			]),
			pTranscode, pBatch, pCluster, pSync, pEffects, pTasks
		]);

		applyStatus(st);
		loadClusterCfg(parseJSON(clres.out, null));
		renderHistory();
		loadFileList();
		setTimeout(refreshStatus, 400);
		refreshCluster();
		refreshRoster();
		showTab('transcode');
		chipUpdate(st && st.state);

		this._vpuTimer = setInterval(function() {
			if (!document.body.contains(wrap)) { clearInterval(self._vpuTimer); return; }
			refreshStatus();
			refreshBatch();
			refreshCluster();
			renderHistory();   /* 🔴 原来漏了这句，Recent jobs 只在加载时刷一次 ✗ */
		}, 4000);
		refreshBatch();

		return wrap;
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
