-- Copyright (C) 2026 RK3566 Panther X2 project
-- Licensed to the public under the Apache License 2.0.
--
-- LuCI controller for luci-app-rk356x-ocr.
--
-- Design rules (hard requirements, learned the hard way):
--   * index() performs registration ONLY.  No file probing, no uci read, no
--     engine call at load time.  Everything is wrapped in pcall so a failure
--     can at worst hide this menu, never 500 the whole Web UI.
--   * Only luci.http / luci.sys / luci.util / luci.jsonc are used.  No nixio
--     dependency is needed; plain io/os is enough for the status probes.
--   * The menu entry sits directly under {"admin", ...} so it is a sibling of
--     Network / Services / System / Status.
--
-- Engine interface used here (verified on the board):
--   /opt/ppocr/ppocr <image>              -> one recognised line per stdout line
--   /opt/ppocr/ppocr <image> --verbose    -> "TEXT x1 y1 ... y4 score <text>"
--                                            on stdout, TIMING on stderr
-- Both are POSIX sh wrappers around the glibc engine; they are launched as-is.

local http  = require "luci.http"
local sys   = require "luci.sys"
local util  = require "luci.util"
local jsonc = require "luci.jsonc"

module("luci.controller.rk356x_ocr", package.seeall)

math.randomseed(os.time())

local BASE    = "/tmp/rk356x-ocr"
local UPLOADS = BASE .. "/uploads"
local JOBS    = BASE .. "/jobs"
local TRASH   = BASE .. "/old"

local ENGINE_MAIN = "/opt/ppocr/ppocr"
local ENGINE_ALT  = "/opt/ppocr/ocr_high"
local DET_MODEL   = "/opt/ppocr/models/ppocrv4_det.rknn"
local REC_MODEL   = "/opt/ppocr/models/ppocrv4_rec.rknn"
local KEYS_FILE   = "/opt/ppocr/keys/ppocr_keys_v1.txt"
local LOADER      = "/opt/glibc/ld-linux-aarch64.so.1"
local NPU_LIB     = "/opt/rknpu/librknnrt.so"
local NPU_DEV     = "/dev/rknpu"
local ENGINE_RAW  = "/opt/ppocr/ppocr_engine_t"
local LIB_PATH    = "/opt/glibc:/opt/rknpu"

local UPLOAD_MAX_KB = 12288

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
		os.execute("mkdir -p " .. sh_quote(d) .. " >/dev/null 2>&1")
	end
end

local function ensure_dirs()
	ensure_dir(UPLOADS)
	ensure_dir(JOBS)
	ensure_dir(TRASH)
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
	return (tostring(s or ""):gsub("[^%w%-_]", ""))
end

local function rand_id()
	return string.format("%d-%04x", os.time(), math.random(0, 0xffff))
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

local function engine_bin()
	-- Prefer the real engine binary: it is what the table mode needs (the
	-- plain-text wrappers cannot forward --verbose/--grid) and it is the file
	-- the package actually ships.  The wrappers are only a fallback for
	-- hand-installed setups.
	if exists(ENGINE_RAW) then return ENGINE_RAW end
	if exists(ENGINE_MAIN) then return ENGINE_MAIN end
	if exists(ENGINE_ALT) then return ENGINE_ALT end
	return nil
end

-- Full engine command line (no trailing redirect).
--
-- The shipped /opt/ppocr/ppocr wrapper is tuned, but its "--verbose" branch
-- runs only "run $1", so it cannot forward --grid.  Run the engine through the
-- bundled glibc loader directly with the same tuned arguments; a single pass
-- then yields the recognised text AND the drawn rules (GRIDC/GRIDR/GRIDS).
-- Falls back to the plain wrapper when the raw binary or loader is absent.
local function engine_cmd(img)
	if exists(LOADER) and exists(ENGINE_RAW) and exists(DET_MODEL)
		and exists(REC_MODEL) then
		return string.format(
			"%s --library-path %s %s --det %s --rec %s --image %s " ..
			"--tile --pre-upscale 2.0 --unclip-ratio 2.0 --box-threshold 0.4 " ..
			"--grid --verbose",
			sh_quote(LOADER), sh_quote(LIB_PATH), sh_quote(ENGINE_RAW),
			sh_quote(DET_MODEL), sh_quote(REC_MODEL), sh_quote(img))
	end
	local eng = engine_bin()
	if eng then
		return sh_quote(eng) .. " " .. sh_quote(img) .. " --verbose"
	end
	return nil
end

-- --------------------------------------------------------------------------
-- table reconstruction from the --verbose "TEXT x1 y1 ... y4 score <text>"
-- geometry: cluster lines into rows by vertical overlap, order cells by x.
-- --------------------------------------------------------------------------

local function parse_verbose(d)
	local lines = {}
	if not d then return lines end
	for raw in (d .. "\n"):gmatch("([^\n]*)\n") do
		local coords, text = raw:match(
			"^TEXT%s+(%S+%s+%S+%s+%S+%s+%S+%s+%S+%s+%S+%s+%S+%s+%S+%s+%S+)%s?(.*)$")
		if coords then
			local n = {}
			for v in coords:gmatch("%S+") do n[#n + 1] = tonumber(v) end
			local xs = { n[1], n[3], n[5], n[7] }
			local ys = { n[2], n[4], n[6], n[8] }
			table.sort(xs)
			table.sort(ys)
			lines[#lines + 1] = {
				x = xs[1], xmax = xs[4], y = ys[1], ymax = ys[4],
				h = math.max(1, ys[4] - ys[1]),
				score = n[9], text = text or "",
			}
		end
	end
	return lines
end

-- Forward declaration: the CSV quoting helper is defined with the other
-- export builders further down, but build_table() needs it already.
local csv_quote

-- Group detected boxes into table rows. Boxes whose vertical extents overlap
-- by more than half of the shorter one belong to the same visual row, so a
-- tall glyph and a short digit on one line end up in the same row.
local function cluster_rows(items)
	local sorted = {}
	local n = #items
	for i = 1, n do sorted[i] = items[i] end
	table.sort(sorted, function(a, b)
		if math.abs(a.y - b.y) > 5 then return a.y < b.y end
		return a.x < b.x
	end)

	local rows = {}
	for _, it in ipairs(sorted) do
		local row = rows[#rows]
		local joins = false
		if row then
			local ov = math.min(row.y1, it.ymax) - math.max(row.y0, it.y)
			local mn = math.min(row.y1 - row.y0, it.h)
			if mn < 1 then mn = 1 end
			if ov > 0 and (ov / mn) >= 0.5 then joins = true end
		end
		if joins then
			row.items[#row.items + 1] = it
			if it.y < row.y0 then row.y0 = it.y end
			if it.ymax > row.y1 then row.y1 = it.ymax end
		else
			rows[#rows + 1] = { y0 = it.y, y1 = it.ymax, items = { it } }
		end
	end
	return rows
end

-- Merge two x intervals, measuring how strongly they agree. Two boxes belong
-- to the same column when they overlap by at least half of the narrower one;
-- a partial touch is only accepted when no better candidate exists.
local function x_overlap_score(a0, a1, b0, b1)
	local ov = math.min(a1, b1) - math.max(a0, b0)
	if ov <= 0 then return 0 end
	local mn = math.min(a1 - a0, b1 - b0)
	if mn < 1 then mn = 1 end
	return ov / mn
end

-- Cluster the x extents of every detected box into table columns. This works
-- for both a drawn grid (each column is a set of aligned cells) and a loose
-- layout (columns of left-aligned text separated by whitespace), because the
-- test is on the horizontal extent, not on an exact x coordinate.
local function cluster_cols(items)
	local sorted = {}
	local n = #items
	for i = 1, n do sorted[i] = items[i] end
	table.sort(sorted, function(a, b) return a.x < b.x end)

	local cols = {}
	for _, it in ipairs(sorted) do
		local best, bestScore = nil, 0
		for _, c in ipairs(cols) do
			local sc = x_overlap_score(it.x, it.xmax, c.x0, c.x1)
			if sc > bestScore then bestScore, best = sc, c end
		end
		if best and bestScore >= 0.5 then
			if it.x < best.x0 then best.x0 = it.x end
			if it.xmax > best.x1 then best.x1 = it.xmax end
			best.n = best.n + 1
		else
			cols[#cols + 1] = { x0 = it.x, x1 = it.xmax, n = 1 }
		end
	end

	table.sort(cols, function(a, b)
		return (a.x0 + a.x1) < (b.x0 + b.x1)
	end)
	for i, c in ipairs(cols) do c.i = i end
	return cols
end

-- Pick the column a box belongs to: strongest overlap wins, otherwise the
-- nearest column centre.
local function col_of_box(cols, it)
	local best, bestScore = nil, 0
	for _, c in ipairs(cols) do
		local sc = x_overlap_score(it.x, it.xmax, c.x0, c.x1)
		if sc > bestScore then bestScore, best = sc, c end
	end
	if best and bestScore > 0 then return best end

	local cx = (it.x + it.xmax) / 2
	local bestDist = nil
	for _, c in ipairs(cols) do
		local d = math.abs((c.x0 + c.x1) / 2 - cx)
		if bestDist == nil or d < bestDist then bestDist, best = d, c end
	end
	return best
end

-- Join the boxes that landed in one cell. Boxes that are almost touching are
-- concatenated (a recogniser sometimes splits one phrase), a wide gap becomes
-- a single space.
local function join_cell(items)
	table.sort(items, function(a, b) return a.x < b.x end)
	local out = ""
	local prev = nil
	for _, it in ipairs(items) do
		local t = it.text or ""
		if t ~= "" then
			if out ~= "" and prev then
				local gap = it.x - prev.xmax
				local h = math.max(1, math.max(it.h, prev.h))
				out = out .. (gap > 0.4 * h and " " or "")
			end
			out = out .. t
			prev = it
		end
	end
	return out
end

local function md_cell(s)
	s = tostring(s or ""):gsub("\r", " "):gsub("\n", " ")
	s = s:gsub("|", "\\|")
	return s
end

-- --------------------------------------------------------------------------
-- drawn rules (optional engine enhancement, never the only source of truth)
--
-- With --grid the engine prints the drawn table rules in SOURCE-image pixels
-- (i.e. before --pre-upscale):
--     GRIDC x1 x2 ... xn     vertical rules    -> column borders
--     GRIDR y1 y2 ... ym     horizontal rules  -> row borders
--     GRIDS  s               working = source * s
-- The TEXT boxes live in WORKING-image pixels, so every rule is multiplied by
-- s before it is compared with a box.  When the table has no drawn rules the
-- parser returns empty lists and every caller falls back to clustering.
-- --------------------------------------------------------------------------

local function parse_grid(d)
	local g = { cols = {}, rows = {}, scale = 1, ok = false }
	if not d then return g end
	for raw in (d .. "\n"):gmatch("([^\n]*)\n") do
		local tag, rest = raw:match("^(GRID%a+)%s+(.*)$")
		if tag == "GRIDC" then
			for v in rest:gmatch("%S+") do
				local n = tonumber(v)
				if n then g.cols[#g.cols + 1] = n end
			end
		elseif tag == "GRIDR" then
			for v in rest:gmatch("%S+") do
				local n = tonumber(v)
				if n then g.rows[#g.rows + 1] = n end
			end
		elseif tag == "GRIDS" then
			local n = tonumber(rest:match("%S+"))
			if n and n > 0 then g.scale = n end
		end
	end
	table.sort(g.cols)
	table.sort(g.rows)
	g.ok = (#g.cols >= 2 or #g.rows >= 2)
	return g
end

-- Turn a sorted list of border coordinates into consecutive spans [lo, hi].
local function spans(lines)
	local out = {}
	for i = 1, #lines - 1 do
		if lines[i + 1] - lines[i] > 1 then
			out[#out + 1] = { lo = lines[i], hi = lines[i + 1] }
		end
	end
	return out
end

-- --------------------------------------------------------------------------
-- row anchor: the "序号" column
--
-- The question tables put one question per row and the running number
-- ("第11733题") in the first column.  That number cell is the row anchor: it
-- is unique per row and short, so it tells us how many rows the table has and
-- (through its vertical position) where each row sits.  The header cell of the
-- same column is the literal label "序号"/"编号"/"号码"/"No".
-- --------------------------------------------------------------------------

local function is_anchor_label(t)
	t = tostring(t or ""):gsub("%s", "")
	if t == "" then return false end
	if t:find("序号", 1, true) then return true end
	if t:find("编号", 1, true) then return true end
	if t:find("号码", 1, true) then return true end
	local l = t:lower()
	return l == "no" or l == "no." or l == "no:"
end

local function is_anchor_value(t)
	t = tostring(t or ""):gsub("%s", "")
	if t == "" then return false end
	-- "第11733题" / "第013941题" / a bare "11733"
	return t:match("^第%d+题?$") ~= nil or t:match("^%d+$") ~= nil
end

-- Pick the anchor column: its header must be a numbering label ("序号"), or --
-- as a fallback when the header row was not recognised -- it is the leftmost
-- column and every cell is a bare question number.  A right-hand numeric
-- column such as table.png's "参考价" therefore never qualifies, and that table
-- keeps using the legacy clustering path.
local function detect_anchor(cols, items)
	local best, bestScore = nil, 0
	for ci, c in ipairs(cols) do
		local cells = {}
		for _, it in ipairs(items) do
			local cx = (it.x + it.xmax) / 2
			if cx >= c.x0 and cx < c.x1 then cells[#cells + 1] = it end
		end
		table.sort(cells, function(a, b) return a.y < b.y end)
		if #cells >= 2 then
			local allnum = true
			for i = 2, #cells do
				if not is_anchor_value(cells[i].text) then allnum = false break end
			end
			local islab = is_anchor_label(cells[1].text)
			if islab or (allnum and ci == 1) then
				local score = (islab and 3 or 0) + (allnum and 1 or 0)
				if score > bestScore then
					bestScore, best = score, { ci = c.i, cells = cells }
				end
			end
		end
	end
	return best
end

-- Row bands from the drawn horizontal rules (exact cell borders).
local function bands_from_rules(rules, scale)
	local out = {}
	local sp = spans(rules)
	for i, s in ipairs(sp) do
		out[i] = { y0 = s.lo * scale, y1 = s.hi * scale }
	end
	return out
end

-- Row bands from the anchor column alone (no drawn rules): the header cell
-- closes the header band, then each pair of neighbouring anchors is split at
-- the midpoint of their centres.
local function bands_from_anchor(anchor)
	local cells = anchor.cells
	local bands = {}
	local top = cells[1].ymax
	bands[1] = { y0 = -1e9, y1 = top, header = true }
	for i = 2, #cells do
		local lo = top
		if i > 2 then
			lo = ((cells[i - 1].y + cells[i - 1].ymax) +
				(cells[i].y + cells[i].ymax)) / 4
		end
		local hi = 1e9
		if i < #cells then
			hi = ((cells[i].y + cells[i].ymax) +
				(cells[i + 1].y + cells[i + 1].ymax)) / 4
		end
		bands[#bands + 1] = { y0 = lo, y1 = hi }
	end
	return bands
end

-- Fallback column borders from the header row: its labels sit in cleanly
-- separated cells, so their x extents give the column count and the gaps
-- between neighbouring labels give the borders.
local function header_columns(items)
	local rows = cluster_rows(items)
	if #rows == 0 then return nil end
	local hdr = rows[1].items
	if #hdr < 2 then return nil end
	local seeds = {}
	for _, it in ipairs(hdr) do
		seeds[#seeds + 1] = { c = (it.x + it.xmax) / 2, x0 = it.x, x1 = it.xmax }
	end
	table.sort(seeds, function(a, b) return a.c < b.c end)
	local cols = {}
	local edges = { -1e9 }
	for i = 1, #seeds - 1 do
		edges[#edges + 1] = (seeds[i].x1 + seeds[i + 1].x0) / 2
	end
	edges[#edges + 1] = 1e9
	for i = 1, #seeds do
		cols[i] = { x0 = edges[i], x1 = edges[i + 1], i = i }
	end
	return cols
end

-- Column index of a box centre: containing span, else the nearest centre.
local function col_index_of(cols, cx)
	local best, bestD = 1, nil
	for i, c in ipairs(cols) do
		if cx >= c.x0 and cx < c.x1 then return i end
		local d = math.abs((c.x0 + c.x1) / 2 - cx)
		if bestD == nil or d < bestD then bestD, best = d, i end
	end
	return best
end

-- UTF-8 helpers.  Lua 5.1 has no utf8 library and OpenWrt ships 5.1, so the
-- character boundaries are walked by hand; this keeps a cross-border split
-- from cutting a multi-byte glyph in half (which would corrupt the export).
local function utf8_len(s)
	local n, i, len = 0, 1, #s
	while i <= len do
		local b = s:byte(i)
		if b < 0x80 then i = i + 1
		elseif b < 0xE0 then i = i + 2
		elseif b < 0xF0 then i = i + 3
		else i = i + 4 end
		n = n + 1
	end
	return n
end

-- Byte offset just after the first k characters of s.
local function utf8_off(s, k)
	local n, i, len = 0, 1, #s
	while i <= len and n < k do
		local b = s:byte(i)
		if b < 0x80 then i = i + 1
		elseif b < 0xE0 then i = i + 2
		elseif b < 0xF0 then i = i + 3
		else i = i + 4 end
		n = n + 1
	end
	if i > len + 1 then i = len + 1 end
	return i - 1
end

-- A detected line can straddle a drawn column border (a long option line and
-- the answer letter on the same baseline become one box).  Split such a box at
-- the border so the fragment on the far side is filed into the right cell.
-- CJK glyphs are near-equal width, so the split point is found proportionally
-- to the width overhang; the text is only ever divided, never dropped.
local function split_crossing(items, cols)
	local out = {}
	for _, it in ipairs(items) do
		local text = it.text or ""
		local cx = (it.x + it.xmax) / 2
		local w = it.xmax - it.x
		local cut = nil
		for i = 1, #cols - 1 do
			local b = cols[i].x1
			if b > it.x and b < it.xmax then
				if cut == nil or math.abs(b - cx) < math.abs(cut - cx) then
					cut = b
				end
			end
		end
		local split = false
		if cut and w > 0 then
			local frac, right_side
			if cx < cut then
				frac, right_side = (it.xmax - cut) / w, true
			else
				frac, right_side = (cut - it.x) / w, false
			end
			local nchar = utf8_len(text)
			local k = math.floor(nchar * frac + 0.5)
			if k >= 1 and k < nchar then
				local keep = right_side and (nchar - k) or k
				local off = utf8_off(text, keep)
				local left_t, right_t
				if right_side then
					left_t = text:sub(1, off)
					right_t = text:sub(off + 1)
				else
					left_t = text:sub(off + 1)
					right_t = text:sub(1, off)
				end
				local function part(x0, x1, t)
					return { x = x0, xmax = x1, y = it.y, ymax = it.ymax,
						h = it.h, score = it.score, text = t }
				end
				out[#out + 1] = part(it.x, cut, left_t)
				out[#out + 1] = part(cut, it.xmax, right_t)
				split = true
			end
		end
		if not split then out[#out + 1] = it end
	end
	return out
end

-- Join the boxes of one cell into a single string: order by vertical centre
-- first (so a multi-line option cell reads A/B/C/D top to bottom) and by x
-- within the same line.  A line break becomes a space, a horizontal gap
-- becomes a space, glyphs that touch stay glued.
local function join_cell_ordered(items)
	local its = {}
	for i = 1, #items do its[i] = items[i] end
	table.sort(its, function(a, b)
		local ay = (a.y + a.ymax) / 2
		local by = (b.y + b.ymax) / 2
		if math.abs(ay - by) > 3 then return ay < by end
		return a.x < b.x
	end)
	local out, prev = "", nil
	for _, it in ipairs(its) do
		local t = it.text or ""
		if t ~= "" then
			if out ~= "" and prev then
				local same = math.abs(((it.y + it.ymax) - (prev.y + prev.ymax)) / 2) <= 3
				if not same then
					out = out .. " "
				else
					local gap = it.x - prev.xmax
					local h = math.max(1, math.max(it.h, prev.h))
					if gap > 0.4 * h then out = out .. " " end
				end
			end
			out = out .. t
			prev = it
		end
	end
	return out
end

-- Rebuild the table from the raw boxes.
--
-- Structure (confirmed with the user): one question = one row, and inside a
-- row the option cell carries the A/B/C/D text as several lines.  Therefore
--   * the rows come from the "序号" anchor column, snapped to the drawn
--     horizontal rules when the rule count agrees with the anchor count;
--   * the columns come from the drawn vertical rules (the real cell borders),
--     else from the header row, else from horizontal clustering;
--   * every text box inside a row is filed into its column and the column's
--     boxes are joined, top to bottom, into one cell.
-- The legacy path (no anchor column, no rules) clusters the boxes vertically,
-- which is what a small regular table such as table.png needs.
local function build_table(items, lines, grid)
	local cols
	if grid and #grid.cols >= 2 then
		local sp = spans(grid.cols)
		cols = {}
		for i, s in ipairs(sp) do
			cols[i] = { x0 = s.lo * grid.scale, x1 = s.hi * grid.scale, i = i }
		end
	end
	if not cols then cols = header_columns(items) end
	if not cols then cols = cluster_cols(items) end
	local ncol = #cols

	local bands, anchored = nil, false
	local anchor = detect_anchor(cols, items)
	if anchor then
		local qn = #anchor.cells - 1
		local rb = (grid and #grid.rows >= 2)
			and bands_from_rules(grid.rows, grid.scale) or nil
		if rb and #rb == qn + 1 then
			bands = rb
			anchored = true
		elseif qn >= 1 then
			bands = bands_from_anchor(anchor)
			anchored = true
		end
	end
	if not bands then
		-- legacy fallback: vertical clustering (regular table, no anchor)
		local rr = cluster_rows(items)
		bands = {}
		for i, r in ipairs(rr) do bands[i] = { y0 = r.y0, y1 = r.y1 } end
	end

	local pieces = anchored and split_crossing(items, cols) or items

	local gridout = {}
	for ri, b in ipairs(bands) do
		gridout[ri] = {}
		local bucket = {}
		for _, it in ipairs(pieces) do
			local cy = (it.y + it.ymax) / 2
			if cy >= b.y0 and cy < b.y1 then
				local ci = col_index_of(cols, (it.x + it.xmax) / 2)
				bucket[ci] = bucket[ci] or {}
				bucket[ci][#bucket[ci] + 1] = it
			end
		end
		for ci = 1, ncol do
			local cell = bucket[ci] or {}
			gridout[ri][ci] = anchored and join_cell_ordered(cell)
				or join_cell(cell)
		end
	end

	-- Safety net: every box must stay visible.  A box that fell outside every
	-- band (a rule the detector missed) is filed into the nearest band so the
	-- export can never lose text.
	local nrows = #bands
	if anchored and nrows > 0 and ncol > 0 then
		for _, it in ipairs(pieces) do
			local cy = (it.y + it.ymax) / 2
			local inside = false
			for _, b in ipairs(bands) do
				if cy >= b.y0 and cy < b.y1 then inside = true break end
			end
			if not inside then
				local ri2, bestD = 1, nil
				for i, b in ipairs(bands) do
					local m = (b.y0 + b.y1) / 2
					if b.y0 <= -1e8 then m = b.y1 end
					if b.y1 >= 1e8 then m = b.y0 end
					local d = math.abs(m - cy)
					if bestD == nil or d < bestD then bestD, ri2 = d, i end
				end
				local ci = col_index_of(cols, (it.x + it.xmax) / 2)
				local sep = gridout[ri2][ci] ~= "" and " " or ""
				gridout[ri2][ci] = gridout[ri2][ci] .. sep .. (it.text or "")
			end
		end
	end

	local cells = {}
	for ri = 1, nrows do
		for ci = 1, ncol do
			if gridout[ri][ci] ~= "" then
				cells[#cells + 1] = {
					r = ri, c = ci,
					x0 = cols[ci].x0, x1 = cols[ci].x1,
					y0 = bands[ri].y0, y1 = bands[ri].y1,
					text = gridout[ri][ci],
				}
			end
		end
	end

	local md = {}
	if nrows > 0 and ncol > 0 then
		local function mdrow(t)
			local c = {}
			for i = 1, ncol do c[i] = md_cell(t[i]) end
			return "| " .. table.concat(c, " | ") .. " |"
		end
		md[#md + 1] = mdrow(gridout[1])
		local sep = {}
		for i = 1, ncol do sep[i] = "---" end
		md[#md + 1] = "| " .. table.concat(sep, " | ") .. " |"
		for i = 2, nrows do md[#md + 1] = mdrow(gridout[i]) end
	end

	local csv = {}
	if ncol > 0 then
		for ri = 1, nrows do
			local q = {}
			for ci = 1, ncol do q[ci] = csv_quote(gridout[ri][ci]) end
			csv[#csv + 1] = table.concat(q, ",")
		end
	else
		for _, l in ipairs(lines or {}) do csv[#csv + 1] = csv_quote(l) end
	end

	return {
		rows = nrows,
		cols = ncol,
		cells = cells,
		grid = gridout,
		anchored = anchored,
		markdown = table.concat(md, "\n") .. (#md > 0 and "\n" or ""),
		csv = table.concat(csv, "\n") .. (#csv > 0 and "\n" or ""),
	}
end

-- Backwards-compatible helper: the 2D cell grid only.
local function build_grid(lines)
	local t = build_table(lines, nil, nil)
	return t.grid
end


local function timing_of(stderr)
	if not stderr then return "" end
	for raw in (stderr .. "\n"):gmatch("([^\n]*)\n") do
		if raw:match("^TIMING") then return raw end
	end
	return ""
end

-- --------------------------------------------------------------------------
-- export builders
-- --------------------------------------------------------------------------

-- Strict RFC 4180 field quoting, tuned for Excel:
--   * any embedded line break is folded to a single space so a record stays on
--     one physical line;
--   * the cell is trimmed at both ends (inner word spaces are preserved);
--   * the field is double-quoted when it contains a comma, a double quote or
--     any whitespace, and every embedded double quote is doubled.
-- Prior versions only quoted on [\n,\"] and left spaces alone, which produced a
-- CSV whose column count drifted whenever a cell (or the OCR output) contained
-- stray spaces -- the "format is broken because of unmatched spaces" report.
csv_quote = function(s)
	s = tostring(s or "")
	s = s:gsub("\r\n", " "):gsub("[\r\n]", " ")
	s = s:gsub("^%s+", ""):gsub("%s+$", "")
	if s:find('[%s,"]') then
		return '"' .. s:gsub('"', '""') .. '"'
	end
	return s
end

-- Build the download body for one format. The table view is produced once by
-- build_table() and cached on the result, so every export sees exactly the
-- same cells the browser shows.
local function build_export(res, fmt)
	local grid = res.grid or {}
	local lines = res.lines or {}

	if fmt == "csv" then
		if res.csv and res.csv ~= "" then
			return res.csv, "text/csv; charset=UTF-8", "csv"
		end
		local rows = {}
		for _, cells in ipairs(grid) do
			local q = {}
			for i, c in ipairs(cells) do q[i] = csv_quote(c) end
			rows[#rows + 1] = table.concat(q, ",")
		end
		if #rows == 0 then
			for _, l in ipairs(lines) do rows[#rows + 1] = csv_quote(l) end
		end
		return table.concat(rows, "\n") .. "\n", "text/csv; charset=UTF-8", "csv"
	end

	if fmt == "md" then
		if res.markdown and res.markdown ~= "" then
			return res.markdown, "text/markdown; charset=UTF-8", "md"
		end
		return table.concat(lines, "\n") .. "\n", "text/markdown; charset=UTF-8", "md"
	end

	return table.concat(lines, "\n") .. "\n", "text/plain; charset=UTF-8", "txt"
end

-- --------------------------------------------------------------------------
-- job bookkeeping
-- --------------------------------------------------------------------------

local function list_dir(d)
	local out = {}
	local h = io.popen("ls -1 " .. sh_quote(d) .. " 2>/dev/null")
	if not h then return out end
	for line in h:lines() do out[#out + 1] = line end
	h:close()
	return out
end

local function prune_jobs()
	local names = list_dir(JOBS)
	if #names <= 30 then return end
	table.sort(names)
	ensure_dir(TRASH)
	for i = 1, #names - 30 do
		os.execute("mv " .. sh_quote(JOBS .. "/" .. names[i]) .. " " ..
			sh_quote(TRASH .. "/" .. names[i]) .. " >/dev/null 2>&1")
	end
end

local function job_read_result(jd)
	local meta = jsonc.parse(read_file(jd .. "/meta.json") or "")
	if type(meta) ~= "table" then meta = {} end
	local mode = (meta.mode == "table") and "table" or "text"

	local rc = tonumber(((read_file(jd .. "/rc") or ""):gsub("%s", "")))
	local tout = read_file(jd .. "/text.out") or ""
	local vraw = read_file(jd .. "/verbose.out")

	local geo = nil
	if vraw then geo = parse_verbose(vraw) end
	local ginfo = nil
	if vraw then ginfo = parse_grid(vraw) end

	local lines = split_lines(tout)
	-- Defensive recovery: the plain text is normally produced by the job's sed
	-- pass over the verbose stream.  If that produced nothing but the verbose
	-- stream did carry TEXT lines, rebuild the text from the geometry.
	if #lines == 0 and geo and #geo > 0 then
		for _, g in ipairs(geo) do lines[#lines + 1] = g.text or "" end
	end

	local res = {
		ok = (rc == 0), rc = rc, mode = mode,
		lines = lines,
		text = table.concat(lines, "\n"),
	}

	-- Table data is built ONLY for table mode.  In text mode the response must
	-- not carry grid/markdown/csv/cells at all, so the page cannot be tempted
	-- into rendering a table the user did not ask for.
	if mode == "table" and geo then
		local tbl = build_table(geo, lines, ginfo)
		res.geo = geo
		res.rows = tbl.rows
		res.cols = tbl.cols
		res.cells = tbl.cells
		res.grid = tbl.grid
		res.markdown = tbl.markdown
		res.csv = tbl.csv
		res.table_source = tbl.anchored
			and (ginfo and ginfo.ok and "anchor+rules" or "anchor")
			or "cluster"
	end

	res.timing = timing_of(read_file(jd .. "/verbose.err") or "")

	if rc ~= 0 then
		res.error = "engine exit " .. tostring(rc)
		local t = util.trim(read_file(jd .. "/verbose.err") or "")
		if t ~= "" then res.error = res.error .. ": " .. t:sub(1, 300) end
	end
	return res
end

-- --------------------------------------------------------------------------
-- routes
-- --------------------------------------------------------------------------

function index()
	-- Registration only.  Wrapped in pcall: a broken registration may hide
	-- this app, it must never break the shared menu tree.
	local ok, err = pcall(function()
		local p = entry({ "admin", "rk356x-ocr" }, firstchild(), _("NPU OCR"), 30)
		p.dependent = false

		entry({ "admin", "rk356x-ocr", "overview" },
			template("rk356x_ocr/main"), _("OCR"), 10)

		entry({ "admin", "rk356x-ocr", "status" },
			call("action_status")).leaf = true
		entry({ "admin", "rk356x-ocr", "recognize" },
			call("action_recognize")).leaf = true
		entry({ "admin", "rk356x-ocr", "job" },
			call("action_job")).leaf = true
		entry({ "admin", "rk356x-ocr", "export" },
			call("action_export")).leaf = true
	end)
	if not ok then
		-- swallow on purpose; see comment above
	end
end

-- GET: status panel --------------------------------------------------------

function action_status()
	local ok, st = pcall(function()
		local eng = engine_bin()
		local s = {
			ok = true,
			engine = eng or "",
			engine_main = ENGINE_MAIN,
			engine_alt = ENGINE_ALT,
			engine_ok = (eng ~= nil),
			det_ok = exists(DET_MODEL),
			rec_ok = exists(REC_MODEL),
			keys_ok = exists(KEYS_FILE),
			loader_ok = exists(LOADER),
			npu_ok = exists(NPU_LIB),
			npu_dev_ok = exists(NPU_DEV),
			det_model = DET_MODEL,
			rec_model = REC_MODEL,
			last = nil,
		}

		local last = read_file(BASE .. "/last.json")
		if last then
			local parsed
			pcall(function() parsed = jsonc.parse(last) end)
			if type(parsed) == "table" then s.last = parsed end
		end

		local missing = {}
		if not s.engine_ok then missing[#missing + 1] = "engine (/opt/ppocr/ppocr)" end
		if not s.det_ok then missing[#missing + 1] = DET_MODEL end
		if not s.rec_ok then missing[#missing + 1] = REC_MODEL end
		if not s.keys_ok then missing[#missing + 1] = KEYS_FILE end
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

-- POST: upload an image and spawn a detached recognition job ----------------

function action_recognize()
	ensure_dirs()

	local clen = tonumber(http.getenv("CONTENT_LENGTH") or "0") or 0
	if clen > UPLOAD_MAX_KB * 1024 then
		return json_err(string.format("upload too large: %d bytes (limit %d KB)",
			clen, UPLOAD_MAX_KB))
	end

	local eng = engine_bin()
	if not eng then
		return json_err("OCR engine not found at " .. ENGINE_MAIN ..
			" or " .. ENGINE_ALT .. " (install rk356x-npu-ocr)")
	end

	local id = rand_id()
	local jd = JOBS .. "/" .. id

	local fd, upath = nil, nil
	http.setfilehandler(function(m, chunk, eof)
		if not fd then
			local ext = ".img"
			local fn = tostring((m and m.file) or "")
			local e = fn:lower():match("%.([%w]+)$")
			if e and e:match("^[a-z0-9]+$") and #e <= 5 then ext = "." .. e end
			ensure_dir(jd)
			upath = UPLOADS .. "/" .. id .. ext
			fd = io.open(upath, "w")
		end
		if fd and chunk then fd:write(chunk) end
		if eof and fd then fd:close() fd = nil end
	end)

	local path = http.formvalue("path")
	http.setfilehandler(nil)

	-- Read the form fields only AFTER the multipart body has been parsed.
	-- luci.http.formvalue() triggers the parse, which invokes the file handler
	-- installed above; reading "mode" before that would consume the body and the
	-- uploaded image would never reach the handler.
	local mode = (http.formvalue("mode") == "table") and "table" or "text"

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

	-- Single output file for the recognised text; the job writes it by stripping
	-- the coordinate prefix off the one verbose engine stream (see below).
	local out = jd .. "/text.out"
	local vout = jd .. "/verbose.out"   -- engine debug + "TEXT ... score <text>"
	local verr = jd .. "/verbose.err"    -- TIMING / errors
	local rcf = jd .. "/rc"
	local fin = jd .. "/finished"

	write_file(jd .. "/meta.json", jsonc.stringify({
		id = id, mode = mode, input = input, engine = eng,
		started = os.time(), ts = os.time(),
	}))
	write_file(jd .. "/started", tostring(os.time()))

	-- ONE engine pass only.  Previously the job ran the engine twice: a plain
	-- pass for the text and a second --verbose pass for the box coordinates.
	-- That doubled the wall time (a 4 MP image needed 2 x ~29 s = ~58 s).
	-- The --verbose stream already carries the recognised lines AND their
	-- geometry, so a single pass is enough: the plain text is recovered by
	-- stripping the "TEXT x1 y1 ... y4 score" prefix from every TEXT line.
	-- The same pass now also carries the drawn rules (--grid), which the table
	-- builder uses to place the row/column borders.
	--
	-- Ordering: the exit status is kept in a shell variable, the text is
	-- extracted, "finished" is written, and "rc" is written LAST.  "rc" is the
	-- completion marker the poller waits for, so its appearance guarantees the
	-- whole job is already on disk.
	local sed_text = "sed -n 's/^TEXT \\([^ ]* \\)\\{9\\}//p'"

	local ecmd = engine_cmd(input)
	if not ecmd then
		return json_err("OCR engine not found at " .. ENGINE_MAIN ..
			" or " .. ENGINE_ALT .. " (install rk356x-npu-ocr)")
	end

	-- Detached, no kill and no external timeout (the board has none): a long
	-- run only has to survive uhttpd's ~60s CGI limit, which backgrounding does.
	local cmd = string.format(
		"( %s >%s 2>%s; RKVAL=$?; %s %s >%s; " ..
		"date +%%s >%s; echo $RKVAL >%s ) </dev/null >/dev/null 2>&1 &",
		ecmd, sh_quote(vout), sh_quote(verr),
		sed_text, sh_quote(vout), sh_quote(out),
		sh_quote(fin), sh_quote(rcf))
	sys.call(cmd)

	prune_jobs()

	json_out({ ok = true, job = id, mode = mode, engine = eng })
end

-- GET: poll a job ----------------------------------------------------------

function action_job()
	local id = safe_id(http.formvalue("id"))
	if id == "" then return json_err("missing job id") end

	local jd = JOBS .. "/" .. id
	if not exists(jd) then return json_err("unknown job: " .. id) end

	-- Completion marker.  The job writes "finished" and then "rc" LAST, so the
	-- moment "rc" exists every file of the job (verbose stream, extracted text)
	-- is already complete.  The job now runs the engine a single time, so there
	-- is no second "rc_verbose" marker to wait for any more.
	if not exists(jd .. "/rc") then
		local started = tonumber(read_file(jd .. "/started") or "") or os.time()
		return json_out({
			ok = true, running = true, job = id,
			elapsed = math.max(0, os.time() - started),
		})
	end

	local res = job_read_result(jd)
	res.ok = res.ok and true or false
	res.job = id

	-- keep a small "last run" summary for the status panel
	local rmeta = jsonc.parse(read_file(jd .. "/meta.json") or "") or {}
	pcall(function()
		write_file(BASE .. "/last.json", jsonc.stringify({
			job = id, mode = rmeta.mode or "", ts = rmeta.ts or os.time(),
			ok = res.ok, nlines = #(res.lines or {}),
			elapsed = math.max(0, os.time() -
				(tonumber(read_file(jd .. "/started") or "") or os.time())),
			timing = res.timing or "",
		}))
	end)

	json_out(res)
end

-- GET: export a finished job ----------------------------------------------

function action_export()
	local id = safe_id(http.formvalue("id"))
	local fmt = (http.formvalue("fmt") or "txt"):lower()
	if id == "" then return json_err("missing job id", 404) end

	local jd = JOBS .. "/" .. id
	if not exists(jd .. "/rc") then return json_err("job not finished", 404) end

	local res = job_read_result(jd)
	local body, mime, ext = build_export(res, fmt)

	http.header("Content-Disposition",
		string.format('attachment; filename="ocr-%s.%s"', id, ext))
	http.prepare_content(mime)
	http.write(body or "")
end
