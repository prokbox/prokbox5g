#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Converter tests (Phase 14a V1).  Run through tests/convert/run.sh.
#
# GPL v2 or later, see the repository's COPYING.
#
# Inputs are synthetic (lavfi testsrc/sine, synthetic JPEG posters) or the
# committed CC BY clip bcmfw/video/stage6-real.mp4; nothing else is read.
# Scratch: build/convert-test/ (deleted at the start of each run).
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

import hashlib
import importlib.machinery
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")
WARBLE = os.environ.get("WARBLE") or ""
TOOL = os.path.join(ROOT, "scripts", "prokbox-convert.py")
CLIP = os.path.join(ROOT, "bcmfw", "video", "stage6-real.mp4")
W = os.path.join(ROOT, "build", "convert-test")

cv = importlib.machinery.SourceFileLoader("prokbox_convert", TOOL).load_module()

failures = []
passed = 0


def check(name, cond, detail=""):
    global passed
    if cond:
        passed += 1
        print("PASS %s" % name)
    else:
        failures.append(name)
        print("FAIL %s%s" % (name, ": " + str(detail) if detail != "" else ""),
              file=sys.stderr)


def sh(cmd):
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout, p.stderr


def conv(*args):
    return sh([sys.executable, TOOL] + list(args))


def ff(*args):
    rc, o, e = sh([FFMPEG, "-v", "error", "-nostdin", "-y"] + list(args))
    if rc:
        print("ffmpeg setup failed: %s" % e, file=sys.stderr)
        sys.exit(1)


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def tree(d):
    out = {}
    for r, _, fs in os.walk(d):
        for n in fs:
            out[os.path.relpath(os.path.join(r, n), d)] = sha(os.path.join(r, n))
    return out


def probe(p):
    return cv.probe(p)


def vstream(pr):
    return [s for s in pr["streams"] if s["codec_type"] == "video"
            and not s["disposition"].get("attached_pic")][0]


def ilst_int(info, key):
    return int.from_bytes(info["ilst"][key], "big")


def ilst_str(info, key):
    return info["ilst"][key].decode("utf-8")


# ------------------------------------------------------------ fixtures
shutil.rmtree(W, ignore_errors=True)
os.makedirs(W)
SRC = os.path.join(W, "src")
os.makedirs(SRC)


def mk(name, vsrc, extra=(), audio=True, dur=3):
    p = os.path.join(SRC, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    cmd = ["-f", "lavfi", "-i", vsrc + ":d=%d" % dur]
    if audio:
        cmd += ["-f", "lavfi", "-i", "sine=f=440:d=%d" % dur]
    cmd += ["-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"]
    if audio:
        cmd += ["-c:a", "aac"]
    ff(*(cmd + list(extra) + [p]))
    return p


poster_jpg = os.path.join(SRC, "poster-art.jpg")
ff("-f", "lavfi", "-i", "testsrc=s=600x900:d=1", "-frames:v", "1", "-update", "1", poster_jpg)
poster_png = os.path.join(SRC, "side.png")
ff("-f", "lavfi", "-i", "smptebars=s=300x450:d=1", "-frames:v", "1", "-update", "1", poster_png)
chfile = os.path.join(SRC, "ch.ffmeta")
with open(chfile, "w") as f:
    f.write(";FFMETADATA1\n")
    for s, e, t in ((2000, 6000, "Opening; the sea"), (6000, 10000, "Middle"),
                    (10000, 13000, "End = credits")):
        f.write("[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=%s\n"
                % (s, e, cv.ffmeta_escape(t)))

wide = mk("wide50.mp4", "testsrc2=s=640x360:r=50", audio=False)
tv1 = mk("TV Shows/Breaking Foo/Season 2/Breaking Foo S02E03 - The Title.mp4",
         "testsrc2=s=320x240:r=24", dur=2)
tv2 = mk("Show.C.S01E02.mp4", "testsrc2=s=320x240:r=24", dur=2)
plain = mk("nameless.mp4", "testsrc2=s=320x240:r=24", dur=2)
pod = mk("Podcasts/Pod Show/ep1.mp4", "testsrc2=s=320x240:r=24", dur=2)
high = mk("high.mp4", "testsrc2=s=640x480:r=30",
          ("-profile:v", "high", "-bf", "2", "-refs", "4"))
dup1 = mk("a/Same Name.mp4", "testsrc2=s=320x240:r=24", dur=2)
dup2 = mk("b/Same Name.mp4", "testsrc2=s=320x240:r=24", dur=2)
srt = os.path.join(SRC, "subs.srt")
with open(srt, "w") as f:
    f.write("1\n00:00:00,500 --> 00:00:02,000\nHello\n\n")

before_src = tree(SRC)
before_clip = sha(CLIP)

# --------------------------------------------------- pure-function cases
def eq(name, got, want):
    check(name, got == want, "got %r want %r" % (got, want))


eq("parse S01E02", cv.parse_episode("Show.C.S01E02")[1:3], (1, 2))
eq("parse 3x07", cv.parse_episode("Foo 3x07 bar")[1:3], (3, 7))
eq("parse none", cv.parse_episode("A Movie 2020"), None)
eq("parse show text", cv.parse_episode("Show.C.S01E02.Pilot")[0::3], ("Show C", "Pilot"))
eq("classify TV Shows dir",
   cv.classify_path("/x/TV Shows/Foo/Season 4/ep.m4v")["show"], "Foo")
eq("classify season dir",
   cv.classify_path("/x/TV Shows/Foo/Season 4/ep.m4v")["season"], 4)
eq("classify podcasts", cv.classify_path("/x/Podcasts/Cast/a.m4v")["kind"], "podcast")
eq("classify movies", cv.classify_path("/x/Movies/a.m4v")["kind"], "movie")
eq("classify leading number",
   cv.classify_path("/x/TV Shows/Foo/03 - Title.m4v")["episode"], 3)
eq("norm chapters first at 0",
   [c[0] for c in cv.norm_chapters([(2000, 6000, "a"), (6000, 9000, "b")], 9000)], [0, 6000])
eq("norm chapters raw keeps 2000",
   cv.norm_chapters([(2000, 6000, "a")], 9000, raw=True)[0][0], 2000)
eq("norm chapters dedupe",
   len(cv.norm_chapters([(0, 5, "a"), (0, 9, "b"), (5000, 9000, "c")], 9000)), 2)
eq("safe_name", cv.safe_name('a/b:c*?"<>|. '), "a-b-c------")
eq("ffmeta escape", cv.ffmeta_escape("a=b;c#d\\e"), "a\\=b\\;c\\#d\\\\e")

# ------------------------------------------------------ movie (the clip)
O1 = os.path.join(W, "out1")
rc, out, err = conv(CLIP, "-o", O1, "--poster", poster_jpg, "--chapters", chfile,
                    "--title", "Test Film")
check("movie: exit 0", rc == 0, err + out)
mv = os.path.join(O1, "Movies", "Test Film.m4v")
check("movie: layout Movies/Test Film.m4v", os.path.isfile(mv), tree(O1).keys())
check("movie: no .part left", not any(".part." in n for n in tree(O1)))
pr = probe(mv)
v = vstream(pr)
check("movie: H.264 Baseline", v["codec_name"] == "h264" and "Baseline" in v["profile"], v.get("profile"))
check("movie: 320x240", (v["width"], v["height"]) == (320, 240), (v["width"], v["height"]))
a = [s for s in pr["streams"] if s["codec_type"] == "audio"][0]
check("movie: AAC-LC stereo 44.1k", (a["codec_name"], a["profile"], a["channels"], a["sample_rate"])
      == ("aac", "LC", 2, "44100"), a)
mi = cv.mp4_info(mv)
check("movie: avc1 trak present", any(t["hdlr"] == "vide" and t["stsd"] == "avc1" for t in mi["traks"]), mi["traks"])
check("movie: soun trak present", any(t["hdlr"] == "soun" for t in mi["traks"]))
cov = mi["ilst"].get("covr", b"")
check("movie: covr starts FFD8FFE0", cov[:4].hex() == "ffd8ffe0", cov[:16].hex())
check("movie: poster is baseline JFIF, <= 480x720",
      b"JFIF" in cov[:16] and not re.search(rb"\xff\xc2", cov[:2000]), cov[:16].hex())
rp = sh([FFPROBE, "-v", "error", "-show_entries", "stream=width,height", "-of", "csv=p=0",
         "-select_streams", "v:1", mv])[1].strip()
check("movie: poster size fits 480x720", rp == "480,720", rp)
check("movie: stik=9", ilst_int(mi, "stik") == 9, mi["ilst"].get("stik"))
check("movie: title tag", ilst_str(mi, "©nam") == "Test Film")
# chapters: both forms, first at 0
check("movie: chpl present", mi["chpl"])
check("movie: text chapter trak + tref/chap present", mi["text_chapter_trak"])
check("movie: chpl first chapter at 0", mi["chpl_chapters"][:1] == [(0, "Opening; the sea")], mi["chpl_chapters"])
check("movie: chpl 3 chapters", [t for _, t in mi["chpl_chapters"]] == ["Opening; the sea", "Middle", "End = credits"], mi["chpl_chapters"])
check("movie: chapters via ffprobe first at 0",
      [c["start"] for c in pr["chapters"]][0] == 0 and len(pr["chapters"]) == 3)
# moov before mdat (faststart)
with open(mv, "rb") as f:
    data = f.read()
order = [t for t, _, _ in cv.iter_boxes(data)]
check("movie: faststart (moov before mdat)", order.index(b"moov") < order.index(b"mdat"), order)
# GOP <= 3 s, no B, refs <= 3, one slice: the tool's own checker
rc, out, err = conv("--check", mv)
check("movie: --check OK", rc == 0 and "OK:" in out, out + err)
fr = sh([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
         "frame=pict_type,pts_time", "-of", "csv=p=0", mv])[1].split()
ipts = [float(x.split(",")[0]) for x in fr if x.split(",")[1] == "I"]
gaps = [b - a for a, b in zip(ipts, ipts[1:])]
check("movie: keyframes every <= 3 s", gaps and max(gaps) <= 3.01 and ipts[0] == 0.0, (ipts, gaps))
check("movie: no B-frames", fr and not any(x.split(",")[1] == "B" for x in fr))

# three chapter forms parse identically with the host chapter parser
def warble_chapters(path):
    rc, out, err = sh([WARBLE, "-m", path])
    return [l for l in out.splitlines() if re.match(r"Chapter \d+:", l)]


def patch(src, dst, boxtype, newtype, parent=None):
    d = bytearray(open(src, "rb").read())

    def walk(start, end):
        for typ, s, e in cv.iter_boxes(bytes(d), start, end):
            if typ == boxtype and (parent is None or parent == cur[-1]):
                d[s - 4:s] = newtype
            if typ in cv.CONTAINERS:
                cur.append(typ)
                walk(s, e)
                cur.pop()
            elif typ == b"meta":
                pass
    cur = [b""]
    walk(0, len(d))
    with open(dst, "wb") as f:
        f.write(d)


if WARBLE and os.path.isfile(WARBLE):
    both = os.path.join(W, "both.m4a")
    chpl_only = os.path.join(W, "chpl.m4a")
    text_only = os.path.join(W, "text.m4a")
    shutil.copy(mv, both)
    patch(mv, chpl_only, b"chap", b"free", parent=b"tref")
    patch(mv, text_only, b"chpl", b"free", parent=b"udta")
    wb, wc, wt = (warble_chapters(p) for p in (both, chpl_only, text_only))
    # source chapters were 2 s/6 s/10 s: starts shift only for the first
    want = ['Chapter 1: 0 ms "Opening; the sea"', 'Chapter 2: 6000 ms "Middle"',
            'Chapter 3: 10000 ms "End = credits"']
    check("chapters: warble reads both forms", wb == want, wb)
    check("chapters: chpl-only parses identically", wc == wb, (wc, wb))
    check("chapters: text-trak-only parses identically", wt == wb, (wt, wb))
else:
    print("SKIP chapters via warble: no warble binary (run scripts/test.sh)")

# idempotency and manifest
mp = os.path.join(O1, cv.MANIFEST)
man = json.load(open(mp))
ent = list(man["entries"].values())
check("manifest: one entry, profile 1, output recorded",
      len(ent) == 1 and ent[0]["output"] == os.path.join("Movies", "Test Film.m4v")
      and ent[0]["options"]["profile"] == cv.PROFILE_VERSION, man)
m0 = os.stat(mv).st_mtime_ns
rc, out, err = conv(CLIP, "-o", O1, "--poster", poster_jpg, "--chapters", chfile,
                    "--title", "Test Film")
check("idempotent: second run converts 0 files", rc == 0 and "0 converted, 1 up to date" in out, out + err)
check("idempotent: output untouched", os.stat(mv).st_mtime_ns == m0)
rc, out, err = conv(CLIP, "-o", O1, "--poster", poster_jpg, "--chapters", chfile,
                    "--title", "Test Film", "--force")
check("idempotent: --force redoes", rc == 0 and "1 converted" in out, out + err)
rc, out, err = conv(CLIP, "-o", O1, "--title", "Test Film", "--fill")
check("idempotent: changed options redo", rc == 0 and "1 converted" in out, out + err)

# ------------------------------------------------------- TV, parsed paths
O2 = os.path.join(W, "out2")
rc, out, err = conv(os.path.join(SRC, "TV Shows"), tv2, "-o", O2, "-j", "3")
check("tv: exit 0", rc == 0, err + out)
t1 = os.path.join(O2, "TV Shows", "Breaking Foo", "Season 2", "Breaking Foo S02E03.m4v")
check("tv: layout TV Shows/<Show>/Season N/", os.path.isfile(t1), sorted(tree(O2)))
ti = cv.mp4_info(t1)
check("tv: stik=10", ilst_int(ti, "stik") == 10)
check("tv: tvsh", ilst_str(ti, "tvsh") == "Breaking Foo", ti["ilst"].get("tvsh"))
check("tv: tvsn=2", ilst_int(ti, "tvsn") == 2)
check("tv: tves=3", ilst_int(ti, "tves") == 3)
check("tv: tven=S02E03", ilst_str(ti, "tven") == "S02E03")
check("tv: title from filename", ilst_str(ti, "©nam") == "The Title", ti["ilst"].get("©nam"))
t2 = os.path.join(O2, "TV Shows", "Show C", "Season 1", "Show C S01E02.m4v")
check("tv: Show.C.S01E02 parsed (no folder)", os.path.isfile(t2), sorted(tree(O2)))
check("tv: no poster, no covr", "covr" not in cv.mp4_info(t2)["ilst"])
check("tv: no chapters, no chpl and no text trak",
      not cv.mp4_info(t2)["chpl"] and not cv.mp4_info(t2)["text_chapter_trak"])

O3 = os.path.join(W, "out3")
rc, out, err = conv(plain, "-o", O3, "--tv", "--show", "Show B", "--season", "1", "--episode", "3",
                    "--poster", poster_png)
tb = os.path.join(O3, "TV Shows", "Show B", "Season 1", "Show B S01E03.m4v")
check("tv: options override, png poster", rc == 0 and os.path.isfile(tb), err + out)
check("tv: png poster re-encoded to FFD8FFE0",
      cv.mp4_info(tb)["ilst"]["covr"][:4].hex() == "ffd8ffe0")

# ------------------------------------------------------------- podcast
O4 = os.path.join(W, "out4")
rc, out, err = conv(pod, "-o", O4, "--podcast", "--show", "My Cast", "--title", "Ep One")
pp = os.path.join(O4, "Podcasts", "My Cast", "Ep One.m4v")
check("podcast: layout Podcasts/<Show>/", rc == 0 and os.path.isfile(pp), err + out + str(sorted(tree(O4))))
pi = cv.mp4_info(pp)
check("podcast: genre=Podcast", ilst_str(pi, "©gen") == "Podcast", pi["ilst"].keys())
check("podcast: album=show", ilst_str(pi, "©alb") == "My Cast")
O4b = os.path.join(W, "out4b")
rc, out, err = conv(pod, "-o", O4b)
check("podcast: inferred from Podcasts/ folder",
      rc == 0 and os.path.isfile(os.path.join(O4b, "Podcasts", "Pod Show", "ep1.m4v")), sorted(tree(O4b)))

# ------------------------------------- odd sources: no audio, 50 fps, 16:9
O5 = os.path.join(W, "out5")
rc, out, err = conv(wide, "-o", O5)
wo = os.path.join(O5, "Movies", "wide50.m4v")
check("wide: exit 0", rc == 0, err + out)
if os.path.isfile(wo):
    wp = probe(wo)
    wv = vstream(wp)
    check("wide: 320x240 letterboxed", (wv["width"], wv["height"]) == (320, 240))
    check("wide: 50 fps halved to 25", wv["avg_frame_rate"] == "25/1", wv["avg_frame_rate"])
    check("wide: silent AAC track added", any(s["codec_type"] == "audio" and s["codec_name"] == "aac"
                                              for s in wp["streams"]))
    row = sh([FFMPEG, "-v", "error", "-i", wo, "-frames:v", "1", "-vf", "crop=320:4:0:0,format=gray",
              "-f", "rawvideo", "-"])
    check("wide: letterbox bars are black", row[0] == 0)
    rc, out, err = conv("--check", wo)
    check("wide: --check OK", rc == 0, out)
else:
    check("wide: output exists", False, sorted(tree(O5)))
O5b = os.path.join(W, "out5b")
rc, out, err = conv(wide, "-o", O5b, "--fill")
check("fill: exit 0 and 320x240", rc == 0 and (vstream(probe(os.path.join(O5b, "Movies", "wide50.m4v")))["width"] == 320), err)

# duplicate titles get distinct names
O6 = os.path.join(W, "out6")
rc, out, err = conv(os.path.join(SRC, "a"), os.path.join(SRC, "b"), "-o", O6)
check("duplicates: Same Name.m4v and Same Name (2).m4v",
      rc == 0 and sorted(os.listdir(os.path.join(O6, "Movies"))) == ["Same Name (2).m4v", "Same Name.m4v"],
      sorted(tree(O6)))

# ------------------------------------------------------ --check negatives
rc, out, err = conv("--check", high)
check("check: High profile refused", rc == 1 and "NO:" in out and "Baseline" in out, out)
rc, out, err = conv("--check", CLIP)
check("check: committed clip accepted", rc == 0, out)

# ---------------------------------------------------- dry-run, subs, safety
O7 = os.path.join(W, "out7")
rc, out, err = conv(wide, "-o", O7, "--dry-run")
check("dry-run: plans, writes nothing", rc == 0 and "plan" in out and not os.path.exists(O7), out + err)
O8 = os.path.join(W, "out8")
rc, out, err = conv(plain, "-o", O8, "--burn-subs", srt)
if cv.have_libass():
    check("burn-subs: converts with libass", rc == 0 and os.path.isfile(os.path.join(O8, "Movies", "nameless.m4v")), err)
else:
    check("burn-subs: refuses cleanly without libass",
          rc == 2 and "libass" in err and not os.path.exists(O8), (rc, err))
rc, out, err = conv(plain, "-o", os.path.join(W, "out8b"), "--burn-subs", os.path.join(W, "nope.srt"))
check("burn-subs: missing file reported", rc != 0)

# bit-exact fixtures are reproducible
O9a, O9b = os.path.join(W, "out9a"), os.path.join(W, "out9b")
for o in (O9a, O9b):
    conv(CLIP, "-o", o, "--bitexact-test", "--poster", poster_jpg, "--chapters", chfile,
         "--title", "Test Film")
a9 = sha(os.path.join(O9a, "Movies", "Test Film.m4v"))
check("bitexact-test: two runs are byte-identical",
      a9 == sha(os.path.join(O9b, "Movies", "Test Film.m4v")))
rc, out, err = conv(CLIP, "-o", os.path.join(W, "out9c"), "--bitexact-test", "--raw-chapters",
                    "--chapters", chfile, "--title", "Late Chapter")
lc = cv.mp4_info(os.path.join(W, "out9c", "Movies", "Late Chapter.m4v"))
check("raw-chapters: first chapter kept at 2000 ms (fixture mode)",
      lc["chpl_chapters"][:1] == [(2000, "Opening; the sea")], lc["chpl_chapters"])

# sources never modified, nothing written beside them
check("sources: byte-identical, no new files beside them", tree(SRC) == before_src,
      set(tree(SRC)) ^ set(before_src))
check("sources: committed clip unchanged", sha(CLIP) == before_clip)

shutil.rmtree(W, ignore_errors=True)
print("%d passed, %d failed" % (passed, len(failures)))
if failures:
    print("FAILED: " + ", ".join(failures), file=sys.stderr)
    sys.exit(1)
