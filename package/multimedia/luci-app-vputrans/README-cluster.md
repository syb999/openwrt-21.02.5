# vputrans: cluster, effects, GIF and file merging

Two things live here: the multi-box cluster (first half) and the
effects / GIF / merge options (second half).

`vputrans cluster` splits one source into keyframe-aligned pieces, hands one
piece to every box in the group, and joins the results with `-c copy`. Every
box runs this same package; there is no extra daemon, no SSH key and no shared
filesystem.

```
        master (this box)                         nodes (other boxes)
  ┌───────────────────────────┐            ┌──────────────────────────┐
  │ probe + cut at keyframes  │  HTTP GET  │ /cgi-bin/vputrans-node   │
  │ serve  seg001.ts ─────────┼───────────▶│  action=src  (pull slice)│
  │        seg002.ts          │            │  action=start (encode)   │
  │ encode one slice itself   │            │  action=status           │
  │ pull results ◀────────────┼────────────│  action=get   (out.ts)   │
  │ concat -c copy → final    │            │  action=clean            │
  └───────────────────────────┘            └──────────────────────────┘
                     shared token in /etc/config/vputrans
```

## Requirements

* Every box runs this package (same `vputrans` version - `clusterlist` shows it).
* The same `token` in `/etc/config/vputrans` on the master and on every node.
* uhttpd reachable on the nodes (`/cgi-bin/` is already enabled on this
  firmware). The node's own `vputrans` single-job lock is *not* used by cluster
  slices, so a node can keep serving other things - but do not start a manual
  transcode on a node while it works for a cluster.
* Space for the slices on the master (`workdir`, default `/tmp`) and on each
  node (`/tmp/vputrans.node`). Master peak usage ≈ 2x the source size at the
  end of the job (slices + pulled results); it is deleted on success.

## Configure (LuCI: Hardware Media → Video Transcode → ④ Cluster)

    option token 'some-long-random-string'
    list node '10.80.1.126'
    list node '10.80.1.127:80'
    option segments '0'      # 0 = one segment per worker
    option local '1'         # this box also encodes a slice
    option workdir ''        # '' = /tmp

Same file, by CLI:

    vputrans clusterset --token 'xxx' --nodes '10.80.1.126,10.80.1.127' --segments 0 --local 1
    vputrans clusterlist          # ping every node + this box
    vputrans cluster /mnt/a.mkv --bitrate 2M --segments 3
    vputrans clusterstatus        # per-segment state, polled by the UI
    vputrans status               # same progress/stop as a normal job

## How it works

1. `ffprobe` lists the video keyframes; the cut points are the keyframes
   closest to the ideal boundaries. Cutting anywhere else with `-c copy` makes
   ffmpeg snap back to a keyframe, which duplicates a GOP at every join.
2. Each slice is cut with `-c copy -f mpegts` (no re-encode, so the split is
   fast) and stays on the master, which serves it to the node that owns it.
3. The node pulls its slice, encodes it with exactly the same pipeline as a
   normal job (`h264_rkmpp`/`hevc_rkmpp`, hardware decode, same bitrate/QP/res)
   and writes a `.ts`.
4. The master pulls every `.ts` back, concatenates them in order with
   `-c copy` and verifies the result length against the planned one.
5. `Stop` kills the worker, which also tells every node to clean up.

Audio is stream-copied per slice (like a normal job); if the final MP4 cannot
hold the source codec the join step re-encodes audio to AAC.

Trimming (`--ss`/`--dur`) works: the first slice starts at the keyframe before
the trim point and the node discards the head with an output-side seek (an
input seek in a `.ts` has no index and silently drops packets - measured as a
3.3 s hole).

URL sources are supported too: there is nothing to cut locally, so each node
seeks its own slice with an input-side seek (works with servers that support
Range).

## Honest numbers (Panther X2, 1280x720 30fps 62s, 2 Mbps)

| setup                                        | wall clock |
|----------------------------------------------|-----------|
| single box, normal run (62 s clip)           | 13 s      |
| 2 workers on the SAME box (measured)         | 24 s      |
| 2 real boxes (estimate, 62 s clip)           | ~10 s     |
| 2 real boxes (estimate, 1 h movie)           | ~2x       |

Clustering only pays off with real, separate boxes: the RK3566 VPU is shared,
so two workers on one box just halve each other. There is a fixed overhead of
a few seconds (keyframe split + join), which is why a one-minute clip barely
improves while a feature-length file approaches the worker count.

## Limits

* Only the picture is split; the whole file must be seekable (no live stream).
* One slice per worker at a time; with more segments than workers the extra
  slices queue on the same box.
* The output is a re-encoded file, so per-slice parameters must be identical
  (they are, they come from one form).

---

# Effects, GIF, logo, picture in picture, merging two files

All of it is driven from the same form (card ⑤ in the UI) - the backend just
gets extra `run` options:

    --effect none|gray|hflip|vflip|reversal|rot90|rot270
    --logo logo.png --logo-pos tl|tr|bl|br --logo-size WxH
    --pip b.mp4 --pip-pos br|bl|tr|tl|side|top --pip-scale 30
    --join b.mp4            (append the second file)
    --container gif --gif-fps 12

What actually runs on the hardware (measured on the box, not assumed):

| feature                | pipeline                                             |
|------------------------|------------------------------------------------------|
| mirror / rotate        | `vpp_rkrga=transpose=...` - RGA, zero copy            |
| black and white        | VPU decode + `format=gray,format=yuv420p` + HW encode |
| GIF                    | RGA NV12->BGRA + software gif writer (no HW gif)      |
| logo / PiP             | software `overlay`/`hstack` after one `hwdownload`    |
| two files              | both re-encoded with the same params, then `-c copy`  |

Verified with an objective check, not by eye: a flipped output matches the
mirrored source frame to within 0.8/255 (232/255 against the unmirrored one),
the grey output has R=G=B on every sampled pixel (mean spread 0.00 vs 246.82
in the source), the 90 degree output really is 360x640 from a 640x360 source,
a 6 s GIF at 10 fps has exactly 60 frames, and a 20 s + 8 s merge comes out at
28.056 s / 800 frames.

## Logo, picture in picture, side by side (needs the rebuilt ffmpeg-rkrga)

`package/multimedia/ffmpeg-rkrga` must be rebuilt once: the shipped binary only
had the RGA compositor (`overlay_rkrga`), which requires DRM_PRIME frames on
both inputs, and `/dev/dri/card0` is `vgem` here (the vendor kernel brings up no
rockchip-drm), so `hwupload` fails with ENOSYS. The Makefile now also enables
the software `overlay`, `hstack`, `vstack` (and, for resync, `aresample`,
`asetpts`, `atempo`).

After rebuilding, install it on the running box or rebuild the image:

    opkg install /path/bin/packages/*/base/ffmpeg-rkrga_6.0.2-1_*.ipk

Three things had to be fixed on the app side, all measured on the box:

1. `overlay=...:format=yuv420p` is invalid - the option takes
   `yuv420/yuv422/yuv444/rgb/gbrp/auto`, not the pixel-format name:
   `[Eval] Undefined constant or missing '(' in 'yuv420p'`. The format is left
   to negotiation now and the chain hands the encoder yuv420p explicitly.
2. **hstack/vstack deadlock the mp4 muxer**: the picture is written, ffmpeg
   never exits and the file has no `moov` (measured: killed after 40 s, 262 KB
   written). Matroska and mpegts are fine, so the stack modes mux to a temporary
   `.mkv` (`<out>.stack.mkv`) and are remuxed with `-c copy` into the requested
   container afterwards.
3. hstack/vstack also destroy the output frame rate (measured:
   `r_frame_rate=16000/1`, ~50 fps of duplicated frames for a 30 fps source), so
   the stack modes force `-r <source fps> -fps_mode cfr`.

Verified objectively on raw frames (not by eye): the logo region changed from
R/G/B 109/145/248 to 178/71/110 (semi-transparent red box at bottom-right) with
the rest of the frame untouched (diff 0.3/255), the PiP region differs 75/255
from the main picture (0.1/255 outside), side-by-side is 1280x360 and
top/bottom 640x720, both 120 frames at 30 fps for a 4 s clip.

Measured speed (20 s, 640x360, wall clock):

| job                     | speed |
|-------------------------|-------|
| plain transcode         | 2.86x |
| mirror / rotate (RGA)   | 2.86x |
| resync                  | 2.22x |
| logo                    | 2.22x |
| picture in picture      | 1.82x |
| side by side            | 1.82x |
| black and white         | 1.54x |

Everything that composites (logo, PiP, stack) and black and white pays one
software frame copy per frame on the A55 cores; flip/rotate stay free because
the RGA does them.

## Two files into one

`--join` appends the second file. Both halves are re-encoded with the same
parameters first (they may differ in codec/size/fps - that is the point), then
concatenated with `-c copy`; the audio is re-encoded to AAC when the two
audios differ or when the target container cannot hold the source codec.
`--ss`/`--dur` trim the FIRST file only. The joined length is checked against
the sum of both inputs and the job fails instead of leaving a truncated file.

## Cluster + effects

Cluster mode ignores card ⑤ (the pieces must be encoded with identical
parameters, and a per-box effect chain would still be identical - but trimming
and effects are applied on the master only when it is not a cluster job).
The cluster does honour `--ss/--dur` on the source.

## Keyframe limit (important)

Splitting requires a keyframe at every cut, so the number of pieces is limited
by the GOP: a 20 s clip with one keyframe every 8.3 s can only make 3 pieces.
When the requested segment count is higher the backend reduces it and logs
"Only N usable keyframe boundary(ies) here", and when the range has no interior
keyframe at all it refuses:
`The selected range has no keyframe to split on - use fewer segments, or drop the duration limit`.
(A silent fallback would repeat the last GOP of the previous piece - that
produced an 8 s output that was really 2 s during testing.)

---

# Resync (audio/video out of step after a few minutes)

    --resync

"Plays fine for a few minutes, then the audio drifts out, and seeking fixes it
for a while" is the classic timestamp-drift case: the container spreads the
same frames over a slightly wrong timeline, so every player slowly drifts
against the audio clock until a seek re-anchors it.

`--resync` rebuilds the video clock from the frame index
(`setpts=N/(fps*TB)`, with the frame rate snapped to the nearest standard rate
within 5% - a 30 fps source stretched by 2% reports 29.41 and keeping that
number would reproduce the drift), regenerates missing PTS (`-fflags +genpts`),
forces a constant frame rate (`-fps_mode cfr`) and re-encodes the audio with
`aresample=async=1:first_pts=0` so the audio is glued to that clock instead of
its own. Hardware decode/encode are untouched - only the frame timestamps are
rewritten, not the pixels.

Measured on a fixture whose video timeline was stretched by 2% (600 frames,
20.400 s of video against 20.000 s of audio):

| run                      | video       | audio   | frames |
|--------------------------|-------------|---------|--------|
| plain re-encode          | 20.400 s    | 20.000 s| 612    |
| `--resync`               | 20.020 s    | 20.010 s| 600    |

The audio half needs the `aresample` filter, which the shipped ffmpeg-rkrga
does not have yet (it was added to `package/multimedia/ffmpeg-rkrga/Makefile`
together with overlay/hstack/vstack). Until that rebuild the backend applies
the video-side fix and says so in the job notice:

    Resync: this ffmpeg-rkrga has no aresample filter, so only the video
    timestamps are rebuilt (the audio keeps its own timing)

## Effects in cluster mode

The effect (mirror/rotate/black and white) is carried in the segment plan and
applied by every node, so a cluster job comes out in the requested effect:
verified with a 3-segment job - the output is 600 frames / 20.000 s, 0 decode
errors, the flipped frame matches the mirrored source to 1.24/255 (228/255
against the unmirrored one) and the grey frame has R=G=B on every sample.

Note: an effect makes the RGA/hwdownload stages break the timing of a raw .ts
slice (measured: 180 frames of a 6.017 s slice came out as 10.733 s grey and
6.183 s flipped), so whenever an effect is in the chain the frames are re-timed
from their index (`setpts=N/(fps*TB)`), with the frame rate taken from the
original source - a .ts slice reports nonsense rates (18 and 8.7 fps measured).

---

# "Start on one box, the others follow" (beacon + roster)

Instead of listing every node on every box, a follower only has to know who the
master is:

    # on the master (this box): no master, nothing to run
    vputrans clusterset --token 'xxx' --master '' --follow 0 --nodes ''
    # on each follower: point it at the master and switch the beacon on
    vputrans clusterset --token 'xxx' --master 10.80.1.125 --follow 1 --interval 15
    /etc/init.d/vputrans enable && /etc/init.d/vputrans start

`/etc/init.d/vputrans` is a procd service running `vputrans beacon`: it posts
`/cgi-bin/vputrans-node?action=announce&me=<this box>&ver=..&cpus=..&busy=..`
to the master every `interval` seconds. The master keeps the announcements in
`/tmp/vputrans.roster/` (one file per box, refreshed by the beacon), lists them
in the UI (Cluster tab -> "Nodes that announced themselves") and - when no node
list is configured - uses them automatically for the next job:

    vputrans roster                     # JSON: who is here, how long ago, busy?
    vputrans cluster /mnt/a.mkv         # nodes? taken from the roster

A node counts as present for 3 intervals, so a box that is switched off simply
drops out of the list. `clusterset` restarts the beacon, so changing the master
or the interval takes effect at once; the init script is not enabled by the
package, `follow 1` plus `enable` is what turns it on.

Verified on one box with a loopback announcement (`me=127.0.0.1`): the roster
listed it, a cluster job started with an EMPTY node list reported
`segments 2, workers 2`, ran both slices (one on the announced node, one
locally) and produced 600 frames / 20.000 s with 0 decode errors. The beacon
itself was verified through procd: one `vputrans beacon` process, its roster
file timestamp advancing exactly every `interval` seconds, and the process gone
again after `disable`.

## UI layout

The view is organised in tabs now (Transcode / Batch / Cluster / Effects & GIF /
Tasks) instead of seven stacked cards, with a status chip in the header. The
layout was checked by rendering the real view module in a headless browser with
stubbed LuCI primitives (`E`, `_`, `fs.exec`, `view.extend`) - that caught a
real bug: `panel.style.display = ''` falls back to the class rule
(`display:none`), so the first tab came up empty until it was set to `block`.

---

# Round: cluster settings, GIF hardware use, 4K, translations

## Cluster now really reads the form

The form's default bitrate is "follow the source", which reaches the backend as
an EMPTY `--bitrate`. The cluster path had no equivalent of the single-job
default, so every worker would have run `-b:v` with no value. Fixed, together
with a validation pass so a bad value is refused instead of silently ignored:

* `--quality cbr|crf`, `--codec h264|hevc`, `--scaler bicubic|bilinear|lanczos`,
  `--audio copy|none|aac`, `--container auto|mp4|mkv|ts`, `--fps source|<number>`
  are checked, unknown values return a specific error;
* empty bitrate + CBR now follows the source video bitrate (same rule as a
  normal job), empty QP becomes 26;
* `--container gif` is refused (a GIF is one file, it cannot be split);
* a 4K target is refused in the cluster path as well.

Verified end to end: `--codec hevc --res 480p --fps 25 --audio aac --container mkv --effect hflip`
with the bitrate left empty produced **hevc, 854x480, 25 fps, AAC, matroska**, 0
decode errors, and the plan file shows `cbr|26|812k|hevc|480p|bicubic|aac|25|hflip`
(the 812k is the auto-filled source bitrate). A second run with
`--quality crf --qp 30 --res 720p` produced h264 1280x720 with the plan showing
`crf|30||h264|720p`.

## GIF does use the hardware - up to the point that exists

Measured from the running command line of a GIF job:

    -hwaccel rkmpp -hwaccel_output_format drm_prime ...
    -vf scale_rkrga=w=-2:h=480:format=bgra,hwdownload,format=bgra,fps=10 -c:v gif

So the VPU decodes and the RGA does both the scaling and the NV12->BGRA colour
conversion in hardware; only the GIF writer itself is software - there is no
hardware GIF encoder on any SoC. The RGB conversion was merged into the scaling
pass (it used to run the RGA twice per frame).

Measured speed: a 4 s clip becomes a 10 fps 480p GIF in 10 s (~0.4x), a 12 fps
source-size GIF in 9 s. If it is too slow, lower the GIF frame rate or the
resolution - both cut the work directly (frames and pixels).

## 4K removed from the UI

The option was misleading: this board cannot decode or encode 4K at all
(encoder max 1920x1080; the kernel reserves cma=128M while a 4K stream needs
~300 MB of contiguous DMA). The dropdown is back to source/480p/720p/1080p, and
the backend still refuses a 4K request from the CLI with the reason instead of
crashing.

## Translations

The zh-cn file was checked against the **runtime** strings instead of the source
literals: the view module was rendered in a headless browser with `_()` replaced
by a collector, every tab was clicked, and the collected set was diffed against
the .po msgids. That found the real gaps - two long hint sentences whose msgids
did not match because of quoting (`box\'s` instead of `box's`, and two unescaped
`"` inside a msgid). Both were regenerated with proper escaping; the diff is
now empty except for data values (file names, "H.264", "4M"). Backend message
templates were checked the same way and the missing ones (the GIF/cluster
refusals, the new validation errors, the ✔/⏹ cluster log lines) were added.

---

# Why a 4K source sits at 0% (and what the code does about it)

A 97 minute 3840x2160 HEVC source (9.7 GB, 12.5 Mbps) stalled at `0.0s /
5872.3s` for six minutes. The log says why:

    mpp_dma_heap: ioctl alloc failed for Out of memory
    mpp_buffer: mpp_buffer_create failed to create buffer with size 15792537
    cma: cma_alloc: alloc failed, req-size: 3117 pages, ret: -12

One 4K NV12 frame is 12.4 MB and MPP keeps 20+ of them, so a 4K stream needs
roughly 300 MB of *contiguous* DMA memory. This board boots with `cma=128M`
(`CmaTotal: 131072 kB`), so the decoder never gets its buffers and ffmpeg never
produces a frame - which is also why the progress bar sat at 0%: nothing was
slow, nothing was happening at all.

Choosing a smaller *output* resolution does not help, and that is what the
earlier guard got wrong: it only refused when the target was "source size", so
asking for 1080p output sailed through and then stalled anyway. The check is now
on the SOURCE height, whatever the target is:

    This source is 3840x2160: decoding it needs about 300 MB of contiguous DMA
    memory and the board reserves only 128 MB ... Downscaling does not help.

Two escape hatches: use a 1080p source, or raise `cma=` to 512M in the bootargs
(4K then decodes; the encoder still stops at 1920x1080).

There is also a stall watchdog now: if 60 s pass with no decoded frame and no
output bytes, the job is killed and reports

    Nothing was decoded in 60s: the hardware decoder never got a frame. A 4K
    source needs about 300 MB of contiguous DMA memory while this board
    reserves 128 MB ...

instead of showing 0.0% for ever. `VPU_STALL_SEC` overrides the 60 s for tests.

Note on the box state after such a run: CMA stays fragmented. Measured with
`CmaFree: 78012 kB` - plenty on paper - a 4.9 MB contiguous allocation still
failed (33 `cma_alloc` failures in dmesg) and even a 1080p H.264 encode
segfaulted. A reboot is required to get a clean CMA pool; nothing in the app can
defrag it.

---

# Measured on hardware: 4K really works with cma=512M

Earlier notes in this file said the encoder stops at 1920x1080 (that is the
RK3566 figure) and that 4K is decode-only. Both were wrong for this board -
measured, on the same Panther X2, after `cma=128M` -> `cma=512M`:

    source  : hevc 3840x2160, video 5872.283 s, audio (eac3) 5842.848 s
    output  : h264 3840x2160, 23.976 fps CFR, 140794 frames, video 5872.289 s
              aac 5872.314 s, 9.61 GB, 13.1 Mbps
    job     : 12057 s wall (3h21m) = 0.49x on a 97 minute film, 0 decode errors

Two things worth keeping:

1. **4K decode needs the big CMA pool.** With 128M the VPU never allocated its
   buffers (`cma: cma_alloc: alloc failed, req-size 3117 pages`) and the job sat
   at 0.0% for ever; with 512M CmaFree stays at ~522 MB of 524 MB and 4K decodes
   fine. The guard in the app refuses a >1080p source when CmaTotal < 300 MB and
   lifts itself automatically once the pool is big enough - no code change needed
   after the bootargs change.
2. **The encoder does 4K on this board**, contrary to the RK3566 datasheet
   figure, at about 0.5x. So a 2K/4K target is not nonsense - it is just slow.
   The notice the app prints for those targets says exactly that now.

The source also demonstrates what the A/V sync page is for: its audio track was
29.4 s shorter than the video track. After `--resync` the two streams are
5872.289 s and 5872.314 s - a 25 ms difference instead of 29 s, with the frame
count unchanged (140794), i.e. the repair rebuilt the clocks without touching a
single pixel.

---

# Dedicated video CMA pool (pool isolation)

The default CMA area is shared with every other DMA user on the box, so another
program allocating dma-heap memory can fragment it until a 4K VPU job cannot get
its ~12 MB contiguous blocks - the classic `mpp_dma_heap: ioctl alloc failed`
while the pool still looks half empty. The app-side guards only keep *this* app
from making it worse, so the pool is now isolated:

* `patches-5.4/329-rockchip-video-cma-heap.patch` - reserves a second CMA area
  when the kernel command line carries `rk_video_cma=<size>` (boot-time
  `cma_declare_contiguous()`, this tree has no DT-based CMA creation) and
  registers it as `/dev/dma_heap/cma-uncached` and `/dev/dma_heap/cma`, both
  backed by that private area.  Without the parameter nothing changes.
* `package/multimedia/rockchip-mpp/patches/0001-prefer-dedicated-video-cma-heap.patch`
  - libmpp opens those heaps for its uncached (type 0) and cached (type 2)
  buffers, i.e. where every VPU frame comes from; it keeps the
  `system-uncached` / `system` fallback when they are absent.
* `image/mmc.bootscript` - rk3566/rk3568/rk3588 get `cma=256M
  rk_video_cma=512M`; other SoCs keep the single 128M area.

Verify after flashing:

    dmesg | grep -i "video CMA"                  # "dedicated video CMA pool: 512 MiB"
    ls /dev/dma_heap/                            # cma cma-uncached system system-uncached
    dma_heap_debug=0xff ffmpeg-rkrga ... 2>&1 | grep "open dma_heap"

The last one must show `open dma_heap cma-uncached -> fd N` instead of
`system-uncached`. Then the acceptance test: start a 4K job and, while it runs,
make another program allocate dma-heap memory (a second transcode, or any
camera/display app) - the 4K job must not die on an allocation failure.

---

# Pool isolation: acceptance test result

Two CMA areas after the flash (`cma=256M rk_video_cma=512M`):

    dmesg : cma: rk_video_cma: dedicated video pool: 512 MiB
    /dev/dma_heap : cma  cma-uncached  system  system-uncached
    MPP   : open dma_heap cma-uncached -> fd 4   (was: system-uncached)

The point of the split is that another program filling the *default* pool can no
longer starve the transcoder.  Measured with a small helper that allocates and
holds dma-heap memory:

    hold 200 MiB of the default pool (it has 256 MiB)   -> CmaFree 581 MB / 768 MB
    then run 20 s of 4K (hevc 3840x2160 -> h264 3840x2160)
      -> done, 480 frames, 0.67x, 0 oops        (baseline without the holder: 0.69x)
    release the holder -> CmaFree back to 784 MB / 768 MB

The job needed ~300 MB while the default area had ~56 MB left, so the buffers
can only have come from the private pool - that is the isolation working.  Both
runs produced identical output, so the extra pool costs nothing in speed.

Fallback stays available: without `rk_video_cma=` in the bootargs the two heaps
are backed by the default area (old behaviour, no isolation, nothing else
changes).

Kernel note (learned the hard way): a CMA area must be reserved in the early
path (`kernel/dma/contiguous.c`, i.e. `dma_contiguous_reserve()`), because
`mm/cma.c` activates every declared area from its own `core_initcall`.  An area
declared from an `arch_initcall` is never activated, `cma->bitmap` stays NULL
and the first `cma_alloc()` oopses with a NULL pointer dereference in
`find_next_zero_bit`.  `struct cma` is also opaque outside mm/, so a driver must
use `cma_get_size()` instead of touching the fields.

