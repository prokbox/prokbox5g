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
# ProkBox video converter (Phase 14a, piece V1).
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
"""Convert videos into what the iPod's picture decoder plays.

Needs Python 3 and ffmpeg/ffprobe (Homebrew: /opt/homebrew/bin).  Facts
this script relies on were measured on ffmpeg 9.0.2 (docs/phase14a-brief.md
section 3.1).  The source files are only ever read; output goes under -o.

  prokbox-convert.py [options] FILE_OR_FOLDER...
  prokbox-convert.py --check FILE
"""

import argparse
import concurrent.futures
import fractions
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

PROFILE_VERSION = 1             # bump when the encode recipe changes
MANIFEST = ".prokbox-convert.json"
DEFAULT_OUT = "~/Desktop/ProkBox Videos"
VIDEO_EXTS = {".mp4", ".m4v", ".mov", ".mkv", ".avi", ".webm", ".mpg",
              ".mpeg", ".ts", ".m2ts", ".wmv", ".flv", ".ogv", ".3gp"}
POSTER_SIDECARS = ("poster.jpg", "folder.jpg", "cover.jpg")
TEXT_SUBS = {"subrip", "ass", "ssa", "mov_text", "webvtt", "text", "srt"}
BITMAP_SUBS = {"hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle", "xsub"}

FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
FFPROBE = shutil.which("ffprobe") or "/opt/homebrew/bin/ffprobe"


class ConvertError(Exception):
    pass


# ---------------------------------------------------------------- helpers

def run(cmd, check=True):
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if check and p.returncode != 0:
        raise ConvertError("%s failed (exit %d): %s" % (
            os.path.basename(cmd[0]), p.returncode,
            p.stderr.decode("utf-8", "replace").strip()[-600:]))
    return p


def probe(path):
    p = run([FFPROBE, "-v", "error", "-of", "json", "-show_format",
             "-show_streams", "-show_chapters", path])
    return json.loads(p.stdout)


def have_libass():
    p = run([FFMPEG, "-hide_banner", "-filters"], check=False)
    return re.search(rb"^\s*\S+\s+subtitles\s", p.stdout, re.M) is not None


def safe_name(s, fallback="Untitled"):
    """A name that is legal on FAT32/exFAT and on the Mac."""
    s = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "-", s).strip(" .")
    return s[:100].strip(" .") or fallback


def clean_text(s):
    """File-name text to display text: dots/underscores to spaces."""
    if " " not in s:
        s = re.sub(r"[._]+", " ", s)
    return re.sub(r"\s+", " ", s).strip(" -_.")


def parse_rate(s):
    try:
        f = fractions.Fraction(s)
        return f if f > 0 else None
    except (ValueError, ZeroDivisionError, TypeError):
        return None


# ------------------------------------------------- name/path classification
# Mirrors the scan-time rules of brief section 3.2 so the folder agrees
# with the tags the firmware will read.

EP_RES = (re.compile(r"S(\d{1,2})[ ._-]?E(\d{1,3})", re.I),
          re.compile(r"(?<![0-9])(\d{1,2})x(\d{2,3})(?![0-9])", re.I))
SEASON_DIR = re.compile(r"^(?:Season|Series|S)\s*(\d{1,2})$", re.I)


def parse_episode(name):
    """-> (show_text, season, episode, rest_text) or None."""
    for rx in EP_RES:
        m = rx.search(name)
        if m:
            return (clean_text(name[:m.start()]), int(m.group(1)),
                    int(m.group(2)), clean_text(name[m.end():]))
    return None


def classify_path(src):
    """Guess kind and fields from the path alone."""
    parts = os.path.abspath(src).split(os.sep)[:-1]
    stem = os.path.splitext(os.path.basename(src))[0]
    info = {"kind": None, "show": None, "season": None, "episode": None,
            "title": None}
    low = [p.lower() for p in parts]
    if "podcasts" in low:
        info["kind"] = "podcast"
        i = len(low) - 1 - low[::-1].index("podcasts")
        if i + 1 < len(parts):
            info["show"] = parts[i + 1]
        return info
    ep = parse_episode(stem)
    cat = "tv shows" in low
    if cat or ep:
        info["kind"] = "tv"
        if ep:
            info["season"], info["episode"] = ep[1], ep[2]
            info["title"] = ep[3] or None
        if parts and parts[-1] and SEASON_DIR.match(parts[-1]):
            info["season"] = info["season"] or int(
                SEASON_DIR.match(parts[-1]).group(1))
            if len(parts) > 1:
                info["show"] = parts[-2]
        elif cat:
            i = len(low) - 1 - low[::-1].index("tv shows")
            if i + 1 < len(parts):
                info["show"] = parts[i + 1]
        if not info["show"] and ep and ep[0]:
            info["show"] = ep[0]
        if info["episode"] is None:
            m = re.match(r"\s*(\d{1,3})\b", stem)
            if m:
                info["episode"] = int(m.group(1))
                info["title"] = clean_text(stem[m.end():]) or None
    elif "movies" in low:
        info["kind"] = "movie"
    return info


def resolve_meta(src, pr, args):
    """Combine options, source tags and path into the output's fields."""
    tags = {k.lower(): v for k, v in pr.get("format", {}).get("tags", {}).items()}
    pinfo = classify_path(src)
    stem = os.path.splitext(os.path.basename(src))[0]
    kind = args.kind
    if not kind:
        if tags.get("show") or tags.get("media_type") == "10":
            kind = "tv"
        elif tags.get("genre", "").lower().startswith("podcast"):
            kind = "podcast"
        else:
            kind = pinfo["kind"] or "movie"
    m = {"kind": kind}
    show = args.show or tags.get("show") or pinfo["show"]
    if kind == "podcast":
        show = (args.show or tags.get("album") or tags.get("show")
                or pinfo["show"] or os.path.basename(
                    os.path.dirname(os.path.abspath(src))) or "Podcast")
    m["show"] = show
    season = args.season if args.season is not None else None
    if season is None and tags.get("season_number", "").isdigit():
        season = int(tags["season_number"])
    if season is None:
        season = pinfo["season"]
    episode = args.episode if args.episode is not None else None
    if episode is None and tags.get("episode_sort", "").isdigit():
        episode = int(tags["episode_sort"])
    if episode is None:
        episode = pinfo["episode"]
    if kind == "tv":
        m["season"] = season if season is not None else 1
        m["episode"] = episode if episode is not None else 0
        if not m["show"]:
            m["show"] = "TV Show"
    title = args.title or tags.get("title") or pinfo["title"] or clean_text(stem)
    if kind == "tv" and not (args.title or tags.get("title")):
        ep = parse_episode(stem)
        title = (ep[3] if ep and ep[3] else
                 ("Episode %d" % m["episode"] if m["episode"] else
                  clean_text(stem)))
    m["title"] = title or "Untitled"
    return m


# ------------------------------------------------------------- chapters

def ffmeta_escape(s):
    return re.sub(r"([=;#\\\n])", lambda m: "\\" + m.group(1), s)


def norm_chapters(chs, duration_ms=None, raw=False):
    """chs: [(start_ms, end_ms, title)].  Sorted, de-duplicated, the first
    one forced to start at 0 (a chpl first chapter after 0 becomes
    lead_trim in the firmware, brief M6), ends = next start."""
    chs = sorted(chs, key=lambda c: c[0])
    out = []
    for s, e, t in chs:
        if out and s <= out[-1][0]:
            continue
        out.append([max(0, s), e, t or "Chapter %d" % (len(out) + 1)])
    if not out:
        return []
    if not raw:
        out[0][0] = 0
    for i in range(len(out) - 1):
        out[i][1] = out[i + 1][0]
    last = out[-1]
    if duration_ms is not None and last[1] <= last[0]:
        last[1] = max(duration_ms, last[0] + 1)
    elif last[1] <= last[0]:
        last[1] = last[0] + 1000
    return [tuple(c) for c in out]


def chapters_from_probe(pr):
    out = []
    for c in pr.get("chapters", []):
        tb = parse_rate(c.get("time_base", "1/1000")) or fractions.Fraction(1, 1000)
        s = int(round(float(c["start"]) * tb * 1000))
        e = int(round(float(c["end"]) * tb * 1000))
        out.append((s, e, c.get("tags", {}).get("title", "")))
    return out


def write_ffmeta(path, chs):
    with open(path, "w", encoding="utf-8") as f:
        f.write(";FFMETADATA1\n")
        for s, e, t in chs:
            f.write("[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=%s\n"
                    % (s, e, ffmeta_escape(t)))


# ------------------------------------------------------------ MP4 reading

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"udta",
              b"ilst", b"tref", b"edts"}


def iter_boxes(buf, start=0, end=None):
    end = len(buf) if end is None else end
    pos = start
    while pos + 8 <= end:
        size, typ = struct.unpack(">I4s", buf[pos:pos + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", buf[pos + 8:pos + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - pos
        if size < hdr or pos + size > end:
            return
        yield typ, pos + hdr, pos + size
        pos += size


def read_moov(path):
    """The bytes of the moov box payload (files can be GB; moov is small)."""
    with open(path, "rb") as f:
        f.seek(0, 2)
        total = f.tell()
        pos = 0
        while pos + 8 <= total:
            f.seek(pos)
            h = f.read(16)
            size, typ = struct.unpack(">I4s", h[:8])
            hdr = 8
            if size == 1:
                size = struct.unpack(">Q", h[8:16])[0]
                hdr = 16
            elif size == 0:
                size = total - pos
            if size < hdr:
                break
            if typ == b"moov":
                f.seek(pos + hdr)
                return f.read(size - hdr)
            pos += size
    return None


def mp4_info(path):
    """What the firmware's scanner would see: traks, ilst atoms, chapters."""
    moov = read_moov(path)
    info = {"traks": [], "ilst": {}, "chpl": False, "text_chapter_trak": False,
            "chpl_chapters": []}
    if moov is None:
        return info
    for typ, s, e in iter_boxes(moov):
        if typ == b"trak":
            t = {"hdlr": None, "stsd": None, "chap_ref": False}
            for t2, s2, e2 in iter_boxes(moov, s, e):
                if t2 == b"tref":
                    for t3, _, _ in iter_boxes(moov, s2, e2):
                        if t3 == b"chap":
                            t["chap_ref"] = True
                if t2 == b"mdia":
                    for t3, s3, e3 in iter_boxes(moov, s2, e2):
                        if t3 == b"hdlr":
                            t["hdlr"] = moov[s3 + 8:s3 + 12].decode("latin-1")
                        if t3 == b"minf":
                            for t4, s4, e4 in iter_boxes(moov, s3, e3):
                                if t4 == b"stbl":
                                    for t5, s5, e5 in iter_boxes(moov, s4, e4):
                                        if t5 == b"stsd":
                                            t["stsd"] = moov[s5 + 12:s5 + 16].decode("latin-1")
            info["traks"].append(t)
        elif typ == b"udta":
            for t2, s2, e2 in iter_boxes(moov, s, e):
                if t2 == b"chpl":
                    info["chpl"] = True
                    info["chpl_chapters"] = parse_chpl(moov[s2:e2])
                elif t2 == b"meta":
                    for t3, s3, e3 in iter_boxes(moov, s2 + 4, e2):
                        if t3 != b"ilst":
                            continue
                        for t4, s4, e4 in iter_boxes(moov, s3, e3):
                            for t5, s5, e5 in iter_boxes(moov, s4, e4):
                                if t5 == b"data":
                                    info["ilst"][t4.decode("latin-1")] = \
                                        moov[s5 + 8:e5]
    info["text_chapter_trak"] = any(t["hdlr"] == "text" for t in info["traks"]) \
        and any(t["chap_ref"] for t in info["traks"])
    return info


def parse_chpl(b):
    out = []
    ver = b[0]
    pos = 4 + (4 if ver == 1 else 0)    # version/flags, [reserved]
    n = b[pos]
    pos += 1
    for _ in range(n):
        if pos + 9 > len(b):
            break
        ts = struct.unpack(">Q", b[pos:pos + 8])[0]
        ln = b[pos + 8]
        out.append((ts // 10000, b[pos + 9:pos + 9 + ln].decode("utf-8", "replace")))
        pos += 9 + ln
    return out


# ------------------------------------------------------------------ check

def check_file(path):
    """Print whether the device will show pictures.  Returns an exit code."""
    problems, notes = [], []
    try:
        pr = probe(path)
    except ConvertError as e:
        print("NO: %s" % e)
        return 1
    v = [s for s in pr["streams"] if s["codec_type"] == "video"
         and not s.get("disposition", {}).get("attached_pic")]
    a = [s for s in pr["streams"] if s["codec_type"] == "audio"]
    if not v:
        print("NO: no video stream (it will play as audio only)")
        return 1
    v = v[0]
    prof = v.get("profile", "")
    print("video:   %s %s level %s, %sx%s, %s fps" % (
        v.get("codec_name"), prof, v.get("level"), v.get("width"),
        v.get("height"), v.get("avg_frame_rate")))
    if v.get("codec_name") != "h264":
        problems.append("codec %s is not H.264" % v.get("codec_name"))
    if "Baseline" not in prof:
        problems.append("profile %r is not Baseline" % prof)
    if v.get("width", 0) > 320 or v.get("height", 0) > 240:
        problems.append("%sx%s is larger than 320x240" % (v.get("width"), v.get("height")))
    # B-frames and slices from the stream itself
    pk = run([FFPROBE, "-v", "error", "-select_streams", "v:%d" % (
        [s["index"] for s in pr["streams"]].index(v["index"])),
        "-show_entries", "frame=pict_type", "-of", "csv=p=0",
        "-read_intervals", "%+#300", path], check=False)
    ptypes = pk.stdout.decode().split()
    nb = ptypes.count("B")
    print("B-frames: %d of first %d frames" % (nb, len(ptypes)))
    if nb:
        problems.append("%d B-frames in the first %d frames" % (nb, len(ptypes)))
    tr = run([FFMPEG, "-hide_banner", "-v", "trace", "-i", path, "-map",
              "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers",
              "-frames:v", "10", "-f", "null", "-"], check=False)
    nal = re.findall(rb"\[trace_headers [^\]]*\] nal_unit_type: (\d+)\(",
                     tr.stderr)
    nslices = sum(1 for n in nal if n in (b"1", b"5"))
    m = re.search(rb"max_num_ref_frames\s+\d+ = (\d+)", tr.stderr)
    refs = int(m.group(1)) if m else -1
    print("refs:    %s" % (refs if refs >= 0 else "unknown"))
    if refs > 3:
        problems.append("%d reference frames (max 3)" % refs)
    nfr = max(1, min(10, len(ptypes)))
    slices = nslices / nfr
    print("slices:  %.1f per picture" % slices)
    if slices > 1.01:
        problems.append("%.1f slices per picture (needs 1)" % slices)
    if not a:
        problems.append("no audio track (the scanner needs a sound track)")
    else:
        a = a[0]
        print("audio:   %s %s %s Hz %s ch" % (a.get("codec_name"),
              a.get("profile"), a.get("sample_rate"), a.get("channels")))
        if a.get("codec_name") != "aac" or a.get("profile") not in ("LC", None):
            problems.append("audio %s %s is not AAC-LC" % (
                a.get("codec_name"), a.get("profile")))
    mi = mp4_info(path)
    stsds = [t["stsd"] for t in mi["traks"] if t["hdlr"] == "vide"]
    print("traks:   %s" % ", ".join("%s/%s" % (t["hdlr"], t["stsd"])
                                    for t in mi["traks"]))
    if "avc1" not in stsds:
        problems.append("no avc1 video trak (the firmware will not see video)")
    if pr["format"].get("format_name", "").find("mp4") < 0:
        problems.append("container %s is not MP4" % pr["format"].get("format_name"))
    if mi["ilst"]:
        print("tags:    %s" % ", ".join(sorted(mi["ilst"])))
    if problems:
        print("NO: the device will not show pictures:")
        for p in problems:
            print("  - " + p)
        return 1
    print("OK: the device will show pictures")
    return 0


# ---------------------------------------------------------------- convert

def build_poster(src, pr, args, tmpdir):
    """-> path of a baseline JFIF JPEG <= 480x720, or None."""
    cand = None
    if args.poster:
        cand = ("file", args.poster)
    else:
        for s in pr["streams"]:
            if s["codec_type"] == "video" and s.get("disposition", {}).get("attached_pic"):
                cand = ("stream", s["index"])
                break
        if not cand:
            d = os.path.dirname(os.path.abspath(src))
            stem = os.path.splitext(os.path.basename(src))[0]
            for n in [stem + ".jpg", stem + ".png"] + list(POSTER_SIDECARS):
                p = os.path.join(d, n)
                if os.path.isfile(p):
                    cand = ("file", p)
                    break
    if not cand:
        return None
    out = os.path.join(tmpdir, "poster.jpg")
    cmd = [FFMPEG, "-v", "error", "-nostdin", "-y", "-i",
           cand[1] if cand[0] == "file" else src]
    if cand[0] == "stream":
        cmd += ["-map", "0:%d" % cand[1]]
    cmd += ["-vf", "scale=w=480:h=720:force_original_aspect_ratio=decrease:"
            "force_divisible_by=2,format=yuvj420p", "-frames:v", "1",
            "-c:v", "mjpeg", "-q:v", "3", "-update", "1", "-f", "image2", out]
    run(cmd)
    return out


def sub_plan(src, pr, args):
    """-> None | ("text", file_or_None, sub_index) | ("bitmap", idx)."""
    if not args.burn_subs:
        return None
    if args.burn_subs != "auto":
        if not os.path.isfile(args.burn_subs):
            raise ConvertError("--burn-subs file not found: %s" % args.burn_subs)
        return ("text", os.path.abspath(args.burn_subs), None)
    subs = [s for s in pr["streams"] if s["codec_type"] == "subtitle"]
    for i, s in enumerate(subs):
        if s.get("codec_name") in TEXT_SUBS:
            return ("text", None, i)
        if s.get("codec_name") in BITMAP_SUBS:
            return ("bitmap", i, None)
        break
    stem = os.path.splitext(os.path.abspath(src))[0]
    for ext in (".srt", ".ass"):
        if os.path.isfile(stem + ext):
            return ("text", stem + ext, None)
    return None


def filter_arg_escape(p):
    return (p.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")
            .replace(",", "\\,").replace("[", "\\[").replace("]", "\\]"))


def encode(src, pr, meta, dest, args, tmpdir):
    vs = [s for s in pr["streams"] if s["codec_type"] == "video"
          and not s.get("disposition", {}).get("attached_pic")]
    if not vs:
        raise ConvertError("no video stream in %s" % src)
    v = vs[0]
    aud = [s for s in pr["streams"] if s["codec_type"] == "audio"]
    dur = float(pr["format"].get("duration") or v.get("duration") or 0)
    fps = (parse_rate(v.get("avg_frame_rate")) or parse_rate(v.get("r_frame_rate"))
           or fractions.Fraction(25))
    if fps > 30:
        fps = fps / 2
    gop = max(1, int(round(float(fps) * 3)))

    sar = parse_rate((v.get("sample_aspect_ratio") or "1:1").replace(":", "/"))
    vf = []
    if sar and sar != 1:
        vf.append("scale=trunc(iw*%d/%d/2)*2:ih,setsar=1" % (sar.numerator, sar.denominator))
    if args.fill:
        vf.append("scale=320:240:force_original_aspect_ratio=increase,crop=320:240")
    else:
        vf.append("scale=320:240:force_original_aspect_ratio=decrease,"
                  "pad=320:240:(ow-iw)/2:(oh-ih)/2")
    vf.append("setsar=1")
    vf.append("fps=%d/%d" % (fps.numerator, fps.denominator))
    vf.append("format=yuv420p")
    plan = sub_plan(src, pr, args)
    if plan and plan[0] == "text":
        if not have_libass():
            raise ConvertError("--burn-subs needs an ffmpeg built with libass "
                               "(the 'subtitles' filter); this one has none")
        if plan[1]:
            vf.append("subtitles=filename='%s'" % filter_arg_escape(plan[1]))
        else:
            vf.append("subtitles=filename='%s':si=%d" % (
                filter_arg_escape(os.path.abspath(src)), plan[2]))

    cmd = [FFMPEG, "-v", "error", "-nostdin", "-y", "-i", src]
    nin = 1
    # chapters
    chs = []
    if args.chapters:
        cp = probe(args.chapters)
        chs = chapters_from_probe(cp)
        if not chs:
            raise ConvertError("no chapters found in %s" % args.chapters)
    else:
        chs = chapters_from_probe(pr)
    chs = norm_chapters(chs, int(dur * 1000), raw=args.raw_chapters)
    chap_in = None
    if chs:
        mp = os.path.join(tmpdir, "chapters.ffmeta")
        write_ffmeta(mp, chs)
        cmd += ["-f", "ffmetadata", "-i", mp]
        chap_in = nin
        nin += 1
    poster = build_poster(src, pr, args, tmpdir)
    poster_in = None
    if poster:
        cmd += ["-i", poster]
        poster_in = nin
        nin += 1
    silent_in = None
    if not aud:
        cmd += ["-f", "lavfi", "-t", "%.3f" % dur, "-i",
                "anullsrc=r=44100:cl=stereo"]
        silent_in = nin
        nin += 1

    if plan and plan[0] == "bitmap":
        fc = ("[0:%d]%s[v0];[0:s:%d]scale=320:240[sb];[v0][sb]overlay=0:0,"
              "format=yuv420p[vo]" % (v["index"], ",".join(vf), plan[1]))
        cmd += ["-filter_complex", fc, "-map", "[vo]"]
    else:
        cmd += ["-map", "0:%d" % v["index"], "-filter:v:0", ",".join(vf)]
    if silent_in is not None:
        cmd += ["-map", "%d:a" % silent_in]
    else:
        cmd += ["-map", "0:%d" % aud[0]["index"]]
    if poster_in is not None:
        cmd += ["-map", "%d:v" % poster_in]
    cmd += ["-map_chapters", str(chap_in) if chap_in is not None else "-1",
            "-map_metadata:g", "-1", "-sn", "-dn"]
    cmd += ["-c:v:0", "libx264", "-profile:v:0", "baseline", "-level:v:0", "1.3",
            "-preset", "medium", "-x264-params", "ref=3:slices=1:bframes=0",
            "-pix_fmt:v:0", "yuv420p", "-b:v:0", "600k", "-maxrate:v:0", "768k",
            "-bufsize:v:0", "1536k", "-g:v:0", str(gop), "-sc_threshold:v:0", "0"]
    if poster_in is not None:
        cmd += ["-c:v:1", "copy", "-disposition:v:1", "attached_pic"]
    cmd += ["-c:a", "aac", "-b:a", "128k", "-ac", "2", "-ar", "44100"]
    if silent_in is not None:
        cmd += ["-shortest"]
    tags = {"title": meta["title"]}
    if meta["kind"] == "tv":
        tags.update({"media_type": "10", "show": meta["show"],
                     "album": meta["show"],
                     "season_number": str(meta["season"]),
                     "episode_sort": str(meta["episode"]),
                     "episode_id": "S%02dE%02d" % (meta["season"], meta["episode"])})
    elif meta["kind"] == "podcast":
        tags.update({"genre": "Podcast", "album": meta["show"]})
    else:
        tags["media_type"] = "9"
    for k, val in tags.items():
        cmd += ["-metadata", "%s=%s" % (k, val)]
    if args.bitexact_test:
        cmd += ["-threads", "1", "-fflags", "+bitexact", "-flags:v", "+bitexact",
                "-flags:a", "+bitexact", "-metadata:s:v", "handler_name=",
                "-metadata:s:a", "handler_name="]
    cmd += ["-movflags", "+faststart", "-f", "ipod", dest]
    run(cmd)
    return dur


# --------------------------------------------------------------- planning

def gather(paths, outdir):
    files = []
    outreal = os.path.realpath(outdir)
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs[:] = sorted(d for d in dirs if not d.startswith(".")
                                 and os.path.realpath(os.path.join(root, d)) != outreal)
                for n in sorted(names):
                    if n.startswith(".") or ".part." in n:
                        continue
                    if os.path.splitext(n)[1].lower() in VIDEO_EXTS:
                        files.append(os.path.join(root, n))
        elif os.path.isfile(p):
            files.append(p)
        else:
            raise ConvertError("not found: %s" % p)
    seen, out = set(), []
    for f in files:
        r = os.path.realpath(f)
        if r not in seen:
            seen.add(r)
            out.append(f)
    return out


def rel_output(meta):
    if meta["kind"] == "tv":
        show = safe_name(meta["show"])
        if meta["episode"]:
            nm = "%s S%02dE%02d" % (show, meta["season"], meta["episode"])
        else:
            nm = "%s %s" % (show, safe_name(meta["title"]))
        return os.path.join("TV Shows", show, "Season %d" % meta["season"],
                            safe_name(nm) + ".m4v")
    if meta["kind"] == "podcast":
        return os.path.join("Podcasts", safe_name(meta["show"]),
                            safe_name(meta["title"]) + ".m4v")
    return os.path.join("Movies", safe_name(meta["title"]) + ".m4v")


def options_key(args, meta):
    return {"profile": PROFILE_VERSION, "fill": bool(args.fill),
            "burn_subs": args.burn_subs or None, "poster": args.poster,
            "chapters": args.chapters, "meta": meta,
            "bitexact": bool(args.bitexact_test),
            "raw_chapters": bool(args.raw_chapters)}


def load_manifest(outdir):
    try:
        with open(os.path.join(outdir, MANIFEST)) as f:
            m = json.load(f)
        if isinstance(m.get("entries"), dict):
            return m
    except (OSError, ValueError):
        pass
    return {"version": 1, "entries": {}}


def save_manifest(outdir, m):
    p = os.path.join(outdir, MANIFEST)
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        json.dump(m, f, indent=1, sort_keys=True)
    os.replace(tmp, p)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="prokbox-convert.py",
        description="Convert videos into the iPod's picture profile "
        "(H.264 Baseline 320x240, AAC-LC, .m4v) and file them into "
        "Movies/, TV Shows/<Show>/Season N/ or Podcasts/<Show>/ under -o.  "
        "Sources are never modified.")
    ap.add_argument("inputs", nargs="*", help="video files or folders (recursive)")
    ap.add_argument("-o", "--output", default=DEFAULT_OUT,
                    help="output folder (default: %(default)s)")
    ap.add_argument("-j", "--jobs", type=int, default=2, help="parallel encodes (default 2)")
    k = ap.add_mutually_exclusive_group()
    k.add_argument("--movie", dest="kind", action="store_const", const="movie")
    k.add_argument("--tv", dest="kind", action="store_const", const="tv")
    k.add_argument("--podcast", dest="kind", action="store_const", const="podcast",
                   help="genre=Podcast, album=show, filed under Podcasts/<Show>/")
    ap.add_argument("--show", help="show name (TV or podcast)")
    ap.add_argument("--season", type=int)
    ap.add_argument("--episode", type=int)
    ap.add_argument("--title", help="title (single input only)")
    ap.add_argument("--poster", metavar="FILE", help="poster image (any format ffmpeg reads)")
    ap.add_argument("--chapters", metavar="FILE", help="chapters as an ffmetadata file")
    ap.add_argument("--fill", action="store_true", help="centre-crop to 4:3 instead of letterboxing")
    ap.add_argument("--burn-subs", metavar="auto|FILE",
                    help="burn subtitles in (auto: first subtitle stream or "
                    "<stem>.srt/.ass; text subs need ffmpeg with libass)")
    ap.add_argument("--force", action="store_true", help="redo files the manifest says are done")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    ap.add_argument("--check", metavar="FILE", help="probe FILE and say whether the device will show pictures")
    ap.add_argument("--bitexact-test", action="store_true",
                    help="reproducible output for test fixtures (single thread, bitexact flags)")
    ap.add_argument("--raw-chapters", action="store_true",
                    help="test fixtures only: do not force the first chapter to 0")
    args = ap.parse_args(argv)

    if args.check:
        return check_file(args.check)
    if not args.inputs:
        ap.error("give at least one input file or folder (or --check FILE)")
    for tool in (FFMPEG, FFPROBE):
        if not os.path.exists(tool):
            print("error: %s not found (brew install ffmpeg)" % tool, file=sys.stderr)
            return 2
    if args.burn_subs and args.burn_subs != "auto" and not have_libass():
        print("error: --burn-subs FILE needs an ffmpeg built with libass; "
              "this one has no 'subtitles' filter.  Nothing was converted.",
              file=sys.stderr)
        return 2
    outdir = os.path.abspath(os.path.expanduser(args.output))
    try:
        files = gather(args.inputs, outdir)
    except ConvertError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    if not files:
        print("nothing to convert")
        return 0
    if args.title and len(files) > 1:
        print("error: --title needs exactly one input", file=sys.stderr)
        return 2

    man = load_manifest(outdir)
    used = {e["output"] for e in man["entries"].values()}
    todo, skipped, failed = [], 0, 0
    for src in files:
        key = os.path.realpath(src)
        try:
            pr = probe(src)
            if not any(s["codec_type"] == "video" and not s.get("disposition", {}).get("attached_pic")
                       for s in pr["streams"]):
                raise ConvertError("no video stream")
            meta = resolve_meta(src, pr, args)
        except ConvertError as e:
            print("FAIL  %s: %s" % (src, e), file=sys.stderr)
            failed += 1
            continue
        st = os.stat(src)
        ok = options_key(args, meta)
        ent = man["entries"].get(key)
        if (ent and not args.force and ent["size"] == st.st_size
                and ent["mtime_ns"] == st.st_mtime_ns and ent["options"] == ok
                and os.path.exists(os.path.join(outdir, ent["output"]))):
            print("skip  %s (done: %s)" % (src, ent["output"]))
            skipped += 1
            continue
        rel = ent["output"] if ent and ent["options"].get("meta") == meta else rel_output(meta)
        if not (ent and rel == ent["output"]):
            base, n = rel, 2
            while rel in used:
                rel = "%s (%d).m4v" % (base[:-4], n)
                n += 1
        elif ent:
            used.discard(ent["output"])
        used.add(rel)
        dest_real = os.path.realpath(os.path.join(outdir, rel))
        if dest_real == key:
            print("FAIL  %s: output would overwrite the source" % src, file=sys.stderr)
            failed += 1
            continue
        todo.append((src, key, pr, meta, rel, ok, st))

    for src, key, pr, meta, rel, ok, st in todo:
        print("%s  %s -> %s [%s]" % ("plan " if args.dry_run else "conv ",
              src, rel, meta["kind"]))
    if args.dry_run:
        print("%d to convert, %d up to date, %d failed" % (len(todo), skipped, failed))
        return 1 if failed else 0

    lock = threading.Lock()
    done = 0
    media_s, t0 = 0.0, time.time()

    def work(item):
        src, key, pr, meta, rel, ok, st = item
        dest = os.path.join(outdir, rel)
        part = dest[:-4] + ".part.m4v"
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        tmp = tempfile.mkdtemp(prefix="prokbox-convert-")
        try:
            dur = encode(src, pr, meta, part, args, tmp)
            os.replace(part, dest)
        except BaseException:
            if os.path.exists(part):
                os.remove(part)
            raise
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        with lock:
            man["entries"][key] = {"size": st.st_size, "mtime_ns": st.st_mtime_ns,
                                   "options": ok, "output": rel}
            save_manifest(outdir, man)
        return dur

    if todo:
        os.makedirs(outdir, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = {ex.submit(work, it): it for it in todo}
        for fu in concurrent.futures.as_completed(futs):
            it = futs[fu]
            try:
                media_s += fu.result() or 0
                done += 1
                print("done  %s" % it[4])
            except (ConvertError, OSError) as e:
                failed += 1
                print("FAIL  %s: %s" % (it[0], e), file=sys.stderr)
    el = max(time.time() - t0, 1e-6)
    print("%d converted, %d up to date, %d failed%s" % (
        done, skipped, failed,
        " (%.1fx realtime)" % (media_s / el) if done and media_s else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
