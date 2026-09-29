-- Copyright (C) 2026 RK3566 Panther X2 project
-- Licensed to the public under the Apache License 2.0.
--
-- LuCI controller for luci-app-rk356x-yolov5n.
--
-- This app nests under the existing top level menu "NPU OCR"
-- ({"admin","rk356x-ocr"}) which is owned by luci-app-rk356x-ocr.  The parent
-- node is created/merged by LuCI automatically, so the two packages never have
-- to know about each other beyond the shared path prefix.
--
-- Design rules (hard requirements, learned the hard way):
--   * index() performs registration ONLY.  No file probing, no uci read, no
--     engine call at load time.  Everything is wrapped in pcall so a failure
--     can at worst hide this menu, never 500 the whole Web UI.
--   * Only luci.http / luci.sys / luci.util / luci.jsonc are used.  No nixio
--     dependency is needed; plain io/os is enough for the status probes.
--   * Every action is wrapped in pcall and answers with JSON on error, so the
--     page shows a message instead of a blank 500 screen.
--
-- Engine interface used here (verified on the board):
--   /usr/bin/yolov5n <image> [loop_count]
--       -> stdout: "detected N objects", then one line per object
--                  "<label> @ (x1 y1 x2 y2) <score>"
--          and    "BENCH npu_run_avg_ms=... npu_fps=..." /
--                  "BENCH pipeline_avg_ms=... pipeline_fps=..."
--       -> the demo always writes the annotated picture as ./out.png relative
--          to its own working directory, which the wrapper pins to /opt/yolov5.
--     We call the wrapper as-is and relocate /opt/yolov5/out.png onto tmpfs
--     right after the run, so the boxed image never has to live on flash.
--
-- Work areas live under /tmp (tmpfs): uploads, boxed images, per-run JSON and
-- logs.  Nothing is written to flash except the transient demo out.png that we
-- immediately move away.

local http  = require "luci.http"
local sys   = require "luci.sys"
local util  = require "luci.util"
local jsonc = require "luci.jsonc"

module("luci.controller.rk356x_yolov5n", package.seeall)

math.randomseed(os.time())

local BASE    = "/tmp/rk356x-yolov5n"
local UPLOADS = BASE .. "/uploads"
local OUTDIR  = BASE .. "/out"
local LOCKDIR = BASE .. "/.lock"
local LASTF   = BASE .. "/last.json"

local ENGINE   = "/usr/bin/yolov5n"
local DEMO     = "/opt/yolov5/rknn_yolov5_demo"
-- Default model: the enlarged-calibration build (180-image COCO subset).
-- The original 20-image model is still shipped for reference but is not used
-- unless the runner is pointed at it explicitly.
local MODEL    = "/opt/yolov5/model/yolov5n_rk3566_i8_cal200.rknn"
local LABELS   = "/opt/yolov5/model/coco_80_labels_list.txt"
local SAMPLE   = "/opt/yolov5/model/bus.jpg"
local LOADER   = "/opt/glibc/ld-linux-aarch64.so.1"
local NPU_LIB  = "/opt/rknpu/librknnrt.so"
local NPU_DEV  = "/dev/rknpu"
local DEMO_OUT = "/opt/yolov5/out.png"

local UPLOAD_MAX_KB = 12288
local KEEP_JOBS     = 40

-- --------------------------------------------------------------------------
-- pure Lua helpers (only ever called from inside an action, never at load)
-- --------------------------------------------------------------------------

local function sh_quote(s)
	return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function exists(p)
	-- test -e also covers directories and character devices such as /dev/rknpu
	local h = io.popen("test -e " .. sh_quote(p) .. " && echo y 2>/dev/null")
	if not h then return false end
	local r = h:read("*l")
	h:close()
	return r == "y"
end

local function read_file(p)
	local f = io.open(p, "rb")
	if not f then return nil end
	local d = f:read("*a")
	f:close()
	return d
end

local function write_file(p, d)
	local f = io.open(p, "wb")
	if not f then return false end
	f:write(d)
	f:close()
	return true
end

local function ensure_dir(d)
	if not exists(d) then
		sys.call("mkdir -p " .. sh_quote(d) .. " >/dev/null 2>&1")
	end
end

local function ensure_dirs()
	ensure_dir(BASE)
	ensure_dir(UPLOADS)
	ensure_dir(OUTDIR)
end

local function json_out(t)
	http.prepare_content("application/json; charset=UTF-8")
	http.write(jsonc.stringify(t or {}))
end

local function json_err(msg, code)
	if code then http.status(code, "Error") end
	json_out({ ok = false, error = tostring(msg) })
end

local function safe_id(s)
	return (tostring(s or ""):gsub("[^%w%-_%.]", ""))
end

local function rand_id()
	return string.format("%d-%04x", os.time(), math.random(0, 0xffff))
end

-- Wall-clock milliseconds from /proc/uptime (fractional seconds), so the page
-- can report an end-to-end duration with millisecond resolution.
local function now_ms()
	local f = io.open("/proc/uptime", "r")
	if not f then return os.time() * 1000 end
	local d = f:read("*l") or ""
	f:close()
	local s = tonumber(d:match("^(%S+)"))
	if not s then return os.time() * 1000 end
	return math.floor(s * 1000 + 0.5)
end

local function split_lines(s)
	local out = {}
	if not s then return out end
	s = s:gsub("\r\n", "\n"):gsub("\r", "\n")
	for line in (s .. "\n"):gmatch("([^\n]*)\n") do
		out[#out + 1] = line
	end
	if #out > 0 and out[#out] == "" then out[#out] = nil end
	return out
end

-- --------------------------------------------------------------------------
-- single-flight lock: the demo hardcodes ./out.png in /opt/yolov5, so two
-- simultaneous runs would race on that file.  A mkdir based lock serialises
-- detections.  A lock older than a couple of minutes is treated as stale and
-- cleared so a crashed run cannot wedge the page forever.
-- --------------------------------------------------------------------------

local function lock_acquire(iterations)
	iterations = iterations or 200
	pcall(function()
		local h = io.popen("find " .. sh_quote(LOCKDIR) ..
			" -maxdepth 0 -mmin +2 2>/dev/null")
		local s = h and h:read("*l")
		if h then h:close() end
		if s and s ~= "" then
			sys.call("rmdir " .. sh_quote(LOCKDIR) .. " 2>/dev/null")
		end
	end)

	for _ = 1, iterations do
		if sys.call("mkdir " .. sh_quote(LOCKDIR) .. " 2>/dev/null") == 0 then
			return true
		end
		sys.call("sleep 0.1 2>/dev/null")
	end
	return false
end

local function lock_release()
	sys.call("rmdir " .. sh_quote(LOCKDIR) .. " 2>/dev/null")
end

-- Keep the tmpfs work area bounded: drop the oldest runs beyond KEEP_JOBS.
local function prune_jobs()
	local names = {}
	local h = io.popen("ls -1 " .. sh_quote(OUTDIR) .. " 2>/dev/null")
	if not h then return end
	for line in h:lines() do
		if line:match("%.json$") then
			names[#names + 1] = (line:gsub("%.json$", ""))
		end
	end
	h:close()
	if #names <= KEEP_JOBS then return end
	table.sort(names)
	for i = 1, #names - KEEP_JOBS do
		local id = safe_id(names[i])
		if id ~= "" then
			sys.call("rm -f " ..
				sh_quote(OUTDIR .. "/" .. id) .. ".json " ..
				sh_quote(OUTDIR .. "/" .. id) .. ".png " ..
				sh_quote(OUTDIR .. "/" .. id) .. ".log " ..
				sh_quote(UPLOADS .. "/" .. id) .. ".* >/dev/null 2>&1")
		end
	end
end

-- --------------------------------------------------------------------------
-- demo output parsing
-- --------------------------------------------------------------------------

local function parse_demo(out)
	local objs = {}
	local bench = {}

	for _, line in ipairs(split_lines(out or "")) do
		local n = line:match("detected%s+(%d+)%s+objects")
		if n then bench.detected = tonumber(n) end

		-- "filtered N objects (YOLO_ONLY=...)" and "drawn N boxes" are only
		-- printed when the engine supports the class whitelist.
		n = line:match("filtered%s+(%d+)%s+objects")
		if n then bench.engine_filtered = tonumber(n) end
		n = line:match("drawn%s+(%d+)%s+boxes")
		if n then bench.drawn = tonumber(n) end

		-- "<label> @ (x1 y1 x2 y2) <score>"  (label may contain spaces)
		local label, a, b, c, d, sc =
			line:match("^(.+) @ %((%d+) (%d+) (%d+) (%d+)%)%s+([%d%.]+)%s*$")
		if label then
			local x1, y1, x2, y2 = tonumber(a), tonumber(b), tonumber(c), tonumber(d)
			objs[#objs + 1] = {
				label = label,
				score = tonumber(sc),
				x = x1, y = y1, x2 = x2, y2 = y2,
				w = x2 - x1, h = y2 - y1,
			}
		end

		local v
		v = line:match("BENCH npu_run_avg_ms=([%d%.]+)")
		if v and not bench.npu_ms then bench.npu_ms = tonumber(v) end
		v = line:match("BENCH npu_fps=([%d%.]+)")
		if v and not bench.npu_fps then bench.npu_fps = tonumber(v) end
		v = line:match("BENCH pipeline_avg_ms=([%d%.]+)")
		if v and not bench.pipeline_ms then bench.pipeline_ms = tonumber(v) end
		v = line:match("BENCH pipeline_fps=([%d%.]+)")
		if v and not bench.pipeline_fps then bench.pipeline_fps = tonumber(v) end
	end

	return objs, bench
end

-- --------------------------------------------------------------------------
-- class filter ("only")
--
-- The page may send only=car,bus,person to restrict the result to those COCO
-- label names.  An absent or empty value means "no filtering": every class the
-- model reports is returned.  Label names may contain spaces ("traffic light",
-- "hot dog"), so only commas separate entries.
-- --------------------------------------------------------------------------

local function parse_only(s)
	local set, list = {}, {}
	s = util.trim(tostring(s or ""))
	if s == "" then return set, list end
	for tok in s:gmatch("[^,]+") do
		tok = util.trim(tok):lower()
		-- keep only characters that can appear in a COCO label
		tok = tok:gsub("[^%w %-_%.]", "")
		tok = util.trim(tok)
		if tok ~= "" and not set[tok] then
			set[tok] = true
			list[#list + 1] = tok
		end
	end
	return set, list
end

local function filter_objs(objs, set)
	if next(set) == nil then return objs end
	local keep = {}
	for _, o in ipairs(objs) do
		local lbl = tostring(o.label or ""):lower()
		if set[lbl] then keep[#keep + 1] = o end
	end
	return keep
end

-- --------------------------------------------------------------------------
-- export builders
-- --------------------------------------------------------------------------

local function csv_field(s)
	s = tostring(s or ""):gsub("\r\n", " "):gsub("[%r%n]", " ")
	if s:find('[%s,"]') then
		return '"' .. s:gsub('"', '""') .. '"'
	end
	return s
end

local function build_export(res, fmt)
	local objs = res.objects or {}

	if fmt == "csv" then
		local rows = { "index,label,score,x1,y1,x2,y2,w,h" }
		for i, o in ipairs(objs) do
			rows[#rows + 1] = table.concat({
				csv_field(i), csv_field(o.label), csv_field(o.score),
				csv_field(o.x), csv_field(o.y), csv_field(o.x2), csv_field(o.y2),
				csv_field(o.w), csv_field(o.h),
			}, ",")
		end
		return table.concat(rows, "\n") .. "\n", "text/csv; charset=UTF-8", "csv"
	end

	if fmt == "json" then
		return jsonc.stringify(res) .. "\n", "application/json; charset=UTF-8", "json"
	end

	-- default: a human readable plain text summary
	local L = {}
	L[#L + 1] = "YOLOv5n object detection"
	L[#L + 1] = string.format("objects: %d", #objs)
	if res.npu_ms then
		L[#L + 1] = string.format("npu: %.2f ms (%.2f fps)", res.npu_ms,
			res.npu_fps or 0)
	end
	if res.pipeline_ms then
		L[#L + 1] = string.format("pipeline: %.2f ms (%.2f fps)", res.pipeline_ms,
			res.pipeline_fps or 0)
	end
	if res.ms then
		L[#L + 1] = string.format("wall: %d ms", res.ms)
	end
	L[#L + 1] = ""
	for i, o in ipairs(objs) do
		L[#L + 1] = string.format("%d. %s %.3f @ (%d %d %d %d)",
			i, o.label, o.score or 0, o.x or 0, o.y or 0, o.x2 or 0, o.y2 or 0)
	end
	return table.concat(L, "\n") .. "\n", "text/plain; charset=UTF-8", "txt"
end

-- --------------------------------------------------------------------------
-- routes
-- --------------------------------------------------------------------------

function index()
	-- Registration only.  Wrapped in pcall: a broken registration may hide
	-- this app, it must never break the shared menu tree.
	local ok, err = pcall(function()
		-- Nest under the existing "NPU OCR" top level menu.  LuCI creates or
		-- merges the intermediate {"admin","rk356x-ocr"} node for us.
		entry({ "admin", "rk356x-ocr", "yolov5n" },
			firstchild(), _("YOLOv5n"), 60)

		entry({ "admin", "rk356x-ocr", "yolov5n", "overview" },
			template("rk356x_yolov5n/main"), _("Detect"), 10)

		entry({ "admin", "rk356x-ocr", "yolov5n", "status" },
			call("action_status")).leaf = true
		entry({ "admin", "rk356x-ocr", "yolov5n", "run" },
			call("action_run")).leaf = true
		entry({ "admin", "rk356x-ocr", "yolov5n", "img" },
			call("action_img")).leaf = true
		entry({ "admin", "rk356x-ocr", "yolov5n", "export" },
			call("action_export")).leaf = true
	end)
	if not ok then
		-- swallow on purpose; see comment above
	end
end

-- GET: status panel --------------------------------------------------------

function action_status()
	local ok, st = pcall(function()
		local s = {
			ok          = true,
			engine      = ENGINE,
			engine_ok   = exists(ENGINE),
			demo        = DEMO,
			demo_ok     = exists(DEMO),
			model       = MODEL,
			model_ok    = exists(MODEL),
			labels      = LABELS,
			labels_ok   = exists(LABELS),
			loader      = LOADER,
			loader_ok   = exists(LOADER),
			npu         = NPU_LIB,
			npu_ok      = exists(NPU_LIB),
			npu_dev     = NPU_DEV,
			npu_dev_ok  = exists(NPU_DEV),
			sample      = SAMPLE,
			sample_ok   = exists(SAMPLE),
			last        = nil,
		}

		local last = read_file(LASTF)
		if last then
			local parsed
			pcall(function() parsed = jsonc.parse(last) end)
			if type(parsed) == "table" then s.last = parsed end
		end

		local missing = {}
		if not s.engine_ok then missing[#missing + 1] = ENGINE end
		if not s.demo_ok then missing[#missing + 1] = DEMO end
		if not s.model_ok then missing[#missing + 1] = MODEL end
		if not s.labels_ok then missing[#missing + 1] = LABELS end
		if not s.loader_ok then missing[#missing + 1] = LOADER end
		if not s.npu_ok then missing[#missing + 1] = NPU_LIB end
		if not s.npu_dev_ok then missing[#missing + 1] = NPU_DEV end
		s.missing = missing
		s.ready = (#missing == 0)
		return s
	end)

	if ok and type(st) == "table" then
		json_out(st)
	else
		json_out({ ok = false, ready = false, engine_ok = false,
			missing = { "status probe failed" }, error = tostring(st) })
	end
end

-- POST: upload an image (or point at one on the box) and run YOLOv5n --------

function action_run()
	ensure_dirs()

	local clen = tonumber(http.getenv("CONTENT_LENGTH") or "0") or 0
	if clen > UPLOAD_MAX_KB * 1024 then
		return json_err(string.format("upload too large: %d bytes (limit %d KB)",
			clen, UPLOAD_MAX_KB))
	end

	if not exists(ENGINE) then
		return json_err("engine not found at " .. ENGINE ..
			" (install rk356x-yolov5n)")
	end
	if not exists(MODEL) then
		return json_err("model not found at " .. MODEL ..
			" (install rk356x-yolov5n)")
	end

	local id = rand_id()
	local fd, upath = nil, nil

	http.setfilehandler(function(m, chunk, eof)
		if not fd then
			local ext = ".img"
			local fn = tostring((m and m.file) or "")
			local e = fn:lower():match("%.([%w]+)$")
			if e and e:match("^[a-z0-9]+$") and #e <= 5 then ext = "." .. e end
			upath = UPLOADS .. "/" .. id .. ext
			fd = io.open(upath, "wb")
		end
		if fd and chunk then fd:write(chunk) end
		if eof and fd then fd:close() fd = nil end
	end)

	-- Reading "path" triggers the multipart parse (and therefore the file
	-- handler above).  Everything else is read afterwards from the cache.
	local path = http.formvalue("path")
	local loops = tonumber(http.formvalue("loops") or "1") or 1
	local only_set, only_list = parse_only(http.formvalue("only"))
	http.setfilehandler(nil)

	if loops < 1 then loops = 1 end
	if loops > 50 then loops = 50 end

	local input = upath
	if not input then
		path = util.trim(path or "")
		if path ~= "" then input = path end
	end
	if not input then
		return json_err("no image: choose a file to upload")
	end
	if not exists(input) then
		return json_err("input not found: " .. input)
	end

	local logf = OUTDIR .. "/" .. id .. ".log"
	local png  = OUTDIR .. "/" .. id .. ".png"

	local t0 = now_ms()

	if not lock_acquire(200) then
		return json_err("engine busy: another detection is still running, " ..
			"please retry in a moment")
	end

	local rc = -1
	local okrun, err = pcall(function()
		-- 2>&1 so the BENCH lines and any error text land in one log file;
		-- sys.call gives us the exit status as well.
		-- YOLO_ONLY is exported to the engine so the demo drops the boxes of
		-- every class that is not selected before it draws the annotated
		-- image.  The page-side filter below uses the same list, so the table
		-- and the picture always agree.
		local env = ""
		if #only_list > 0 then
			env = "YOLO_ONLY=" .. sh_quote(table.concat(only_list, ",")) .. " "
		end
		rc = sys.call(string.format("%s%s %s %d >%s 2>&1",
			env, sh_quote(ENGINE), sh_quote(input), loops, sh_quote(logf)))
	end)

	lock_release()

	local t1 = now_ms()

	local out = read_file(logf) or ""
	if not okrun then
		out = out .. "\n" .. tostring(err)
		rc = -1
	end

	-- The demo always writes its annotated picture to /opt/yolov5/out.png.
	-- Move it onto tmpfs immediately so it never lingers on flash.
	local have_png = false
	if exists(DEMO_OUT) then
		have_png = (sys.call("mv " .. sh_quote(DEMO_OUT) .. " " ..
			sh_quote(png) .. " 2>/dev/null") == 0) and exists(png)
	end

	local all_objs, bench = parse_demo(out)
	local objs = filter_objs(all_objs, only_set)

	-- raw_count is the unfiltered total, so the page can show "shown X / total
	-- Y".  The demo always prints the unfiltered total on its "detected N
	-- objects" line, even when it already dropped boxes for the picture.
	local raw_count = bench.detected or #all_objs
	if raw_count < #all_objs then raw_count = #all_objs end

	local res = {
		ok          = (rc == 0),
		rc          = rc,
		id          = id,
		objects     = objs,
		count       = #objs,
		raw_count   = raw_count,
		filtered    = (#only_list > 0),
		only        = table.concat(only_list, ","),
		-- Boxes the engine actually drew on the annotated image, and whether
		-- the engine understood YOLO_ONLY at all (an old binary prints
		-- neither line, in which case the table is filtered but the picture is
		-- not - the page warns about that instead of lying).
		image_boxes = bench.drawn,
		engine_filtered = (bench.engine_filtered ~= nil),
		detected    = bench.detected,
		ms          = math.max(0, t1 - t0),
		npu_ms      = bench.npu_ms,
		npu_fps     = bench.npu_fps,
		pipeline_ms = bench.pipeline_ms,
		pipeline_fps= bench.pipeline_fps,
		loops       = loops,
		input       = input,
		out_png     = have_png and png or "",
		log         = out:sub(1, 4000),
	}

	if rc ~= 0 then
		res.error = "engine exit " .. tostring(rc)
	end
	if not have_png and rc == 0 then
		res.error = res.error or "no boxed image produced by the demo"
	end

	pcall(function()
		write_file(OUTDIR .. "/" .. id .. ".json", jsonc.stringify(res))
		write_file(LASTF, jsonc.stringify({
			id = id, ts = os.time(), ok = res.ok, count = #objs,
			raw_count = raw_count, only = res.only,
			ms = res.ms, npu_ms = res.npu_ms, npu_fps = res.npu_fps,
			pipeline_ms = res.pipeline_ms, pipeline_fps = res.pipeline_fps,
			labels = (function()
				local c = {}
				for _, o in ipairs(objs) do c[#c + 1] = o.label end
				return table.concat(c, ", ")
			end)(),
		}))
	end)

	prune_jobs()

	json_out(res)
end

-- GET: stream a stored image (boxed output or the uploaded original) --------

function action_img()
	local f = safe_id(http.formvalue("f") or "")
	if f == "" or not f:match("%.png$") and not f:match("%.jpg$") and
			not f:match("%.jpeg$") and not f:match("%.bmp$") then
		return json_err("bad image reference", 400)
	end
	if f:find("%.%.") then return json_err("bad image reference", 400) end

	local p = OUTDIR .. "/" .. f
	if not exists(p) then
		p = UPLOADS .. "/" .. f
	end
	if not exists(p) then
		return json_err("image not found: " .. f, 404)
	end

	local fh = io.open(p, "rb")
	if not fh then return json_err("cannot read image", 500) end
	local data = fh:read("*a")
	fh:close()

	local mime = "image/png"
	if f:lower():match("%.jpg$") or f:lower():match("%.jpeg$") then
		mime = "image/jpeg"
	elseif f:lower():match("%.bmp$") then
		mime = "image/bmp"
	end

	http.prepare_content(mime)
	http.write(data or "")
end

-- GET: export a finished result -------------------------------------------

function action_export()
	local id = safe_id(http.formvalue("id") or "")
	local fmt = (http.formvalue("fmt") or "txt"):lower()
	if id == "" then return json_err("missing result id", 404) end

	local raw = read_file(OUTDIR .. "/" .. id .. ".json")
	if not raw then return json_err("unknown result: " .. id, 404) end

	local res = jsonc.parse(raw) or {}
	local body, mime, ext = build_export(res, fmt)

	http.header("Content-Disposition",
		string.format('attachment; filename="yolov5n-%s.%s"', id, ext))
	http.prepare_content(mime)
	http.write(body or "")
end
