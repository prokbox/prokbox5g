# H.264 test corpus

Test streams for our own H.264 Baseline decoder (`bcmfw/h264`, issue #60). Everything is **synthetic**: ffmpeg's lavfi sources (`testsrc2`, `mandelbrot`, `smptebars`) with motion added by `rotate` and `scroll`. No third-party footage, nothing from Apple.

```sh
tests/video/make_corpus.sh           # builds what is missing (~2 s)
tests/video/make_corpus.sh --force   # rebuilds everything
tests/video/make_corpus.sh --check   # sha256 against corpus.sha256
```

The media goes to `build/video/corpus/` and is never committed. Each clip is a raw Annex B stream (`.264`) plus ffmpeg's own decode of it as raw yuv420p (`.yuv`), the reference our decoder must match **bit for bit**. `corpus.sha256` was made with ffmpeg 9.0.2 and x264 0.165.3222 b35605a (Homebrew; check yours with `x264 --version` or `ffmpeg -version`). **The checksums depend on the x264 build:** another version produces different streams. The decoder tests stay valid, because the reference is decoded from the same stream, but `--check` will report the difference. **The MP4 demux test (`mp4demux_test.py`) doesn't depend on them:** it muxes whatever corpus is present and compares against that corpus's own `.yuv`.

Open: on 2026-10-11 a corpus rebuilt with the same ffmpeg 9.0.2 and x264 0.165.3222 b35605a failed `--check` on 2 files, so something besides the version (possibly threading) also changes the output. Not investigated yet.

All clips are libx264, `-profile:v baseline` (CAVLC), Annex B.

**The one piece of real footage** is not in this corpus: stage 6's second device-test clip, `bcmfw/video/stage6-real.mp4` (committed, 786 KB). It's 13 s (15.0–28.0 s) of the *Sintel* trailer, © Blender Foundation | durian.blender.org, [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/): the letterboxed picture centre-cropped to 4:3, scaled to 320×240 at 24 fps, H.264 Baseline at about 400 kbit/s with AAC mono, made by `bcmfw/video/mkclip-real.sh` from `https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4` (the script checks the source's sha256; the source is never committed). The same bytes come out on every run with the x264 build above. It has fast motion, pans and five cuts, so stage 6 can judge pacing and sync on real material; our decoder decodes all 312 pictures bit-exact against ffmpeg. A second excerpt, `bcmfw/video/stage7-conv.m4v` (3.5 MB, 4.0–44.0 s of the same trailer, same licence), is the converter's own output: `bcmfw/video/mkclip-conv.sh` cuts it near-lossless and runs `scripts/prokbox-convert.py --movie --fill --bitexact-test` on it (320×240, 24 fps, Constrained Baseline, about 570 kbit/s, AAC); the device test ships it as `bcmtest/stage7.m4v` for stage 7 and the bench's pace row.

| Clip | Size | Frames | What it covers |
|---|---|---|---|
| `a_ionly_qp{8,20,32,44,51}` | 320×240 | 4 | (a) I-only (`keyint=1`) at fixed QPs from near-lossless to the maximum: Intra 4x4 and 16x16, all intra modes, the dequantiser and the loop filter's whole QP range |
| `a_ionly_mandel` | 320×240 | 4 | (a) I-only, smooth gradients (plane prediction) |
| `b_ippp_gop10` | 320×240 | 30 | (b) IPPP, GOP 10, three references, panning |
| `b_ippp_mandel` | 320×240 | 20 | (b) IPPP, GOP 12, two references, zoom |
| `c_fastdecode` | 320×240 | 30 | (c) `--tune fastdecode`: the loop filter is off in the stream |
| `d_ipod_768k` | 320×240 | 60 | (d) the iPod spec: 768 kbit/s, 30 fps, 2 s, level 3.0, keyint 30 |
| `e_vga` | 640×480 | 10 | (e) the largest size we accept, for the scaling work later |
| `f_slices` | 320×240 | 12 | four slices per picture, deblocking offsets (−2, +2), constrained intra prediction |
| `g_crop` | 176×120 | 10 | a height that isn't a multiple of 16 (frame cropping), one reference |
| `h_partitions` | 320×240 | 16 | `partitions=all`: every P partition down to 4x4, four references, mixed references |

Not covered by any clip: `I_PCM` macroblocks (x264 never emits them in Baseline), multiple slice groups (FMO) and arbitrary slice order (we refuse FMO), long-term references and frame_num gaps (refused).

The decoder tests that use this corpus are `bcmfw/h264/test/run.sh` (host, bit-exact against the `.yuv`, plus fuzzing) and `bcmfw/h264/test/sim_test.py` (the same decoder in the VideoCore simulator). Results are in `docs/research/bcm2722/decoder.md`.

## MP4 demux prototype

`mp4demux.py` is the host prototype of the PP-side MP4/M4V demuxer (#64, `docs/research/bcm2722/player.md`): sample table, Annex B packets, seek to a key sample, and the sidecar index (`.vix`). `mp4demux_test.py` muxes five corpus clips with a synthetic AAC track (moov first, moov last, and `co64`) under `build/video/mp4/`, then checks the sample table against ffprobe, the Annex B output against the corpus reference decode (bit-exact), a mid-file seek, and the index round trip:

```sh
tests/video/run.sh     # 57 checks, about 2 s; skipped without ffmpeg/ffprobe (run by scripts/test.sh)
```
