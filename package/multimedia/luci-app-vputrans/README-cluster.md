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

## Logo and picture in picture need one rebuild

The shipped `ffmpeg-rkrga` only has the RGA compositor (`overlay_rkrga`), which
demands DRM_PRIME frames on both inputs. On this firmware `/dev/dri/card0` is
`vgem` (the vendor kernel does not bring up rockchip-drm), so `hwupload` cannot
turn a PNG into a DRM_PRIME frame - measured: `Function not implemented`. The
package therefore now enables the software `overlay`, `hstack` and `vstack`
filters; until `ffmpeg-rkrga` has been rebuilt with them the backend refuses
with a clear message instead of failing inside ffmpeg:

    This ffmpeg-rkrga build has no overlay/hstack filter - rebuild ffmpeg-rkrga ...

After the rebuild: logo = PNG with alpha overlaid at a corner (`overlay` keeps
the PNG's per-pixel alpha, so `--logo-alpha` is not needed), PiP = second video
scaled to `--pip-scale` percent and placed in a corner, `side`/`top` = hstack /
vstack instead of overlay.

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
