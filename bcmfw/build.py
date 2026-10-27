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
"""Build bcmfw, our VideoCore II image for the BCM2722, with vc2cc.

Output (default build/bcmfw/): bcmfw.bin (flat image, loads at 0),
bcmfw.map (sections and symbols), bcmfw.lst (VC II listing), and a size
report against the 1.25 MB of internal SRAM.

Usage:
  bcmfw/build.py [--out DIR] [-D NAME=VALUE ...] [--quiet]
      -D overrides a switch from bcmfw/config.h (e.g. -D BCMFW_IDLE_SLEEP=1)

Needs vc4-elf-gcc (VC4_GCC, see docs/research/bcm2722/compiler.md);
exits 2 with a message when it is missing.
"""
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C  # noqa: E402

SOURCES = ['crt0.S', 'main.c', 'lcd.c', 'panels.c', 'timer.c', 'clock.c', 'push.S', 'spinprobe.S',
           'dma.c', 'hostproto.c', 'probe.S', 'bench/bench.c',
           'bench/bench_tab.c', 'bench/bench_gen.S', 'bench/bench_scalar.S',
           'bench/algn.c', 'bench/algn.S']
# BCMFW_VIDEO builds add the decoder and the display path
VIDEO_SOURCES = ['video/video.c', 'video/disp.c', 'video/disp_kernels.S',
                 'video/selftest.c',
                 # PW3/PW4's vector sum (the plain video image only)
                 'pwsum.S'] + \
    ['h264/' + f for f in ('h264.c', 'ps.c', 'slice.c', 'mb.c', 'cavlc.c',
                           'mvpred.c', 'intra.c', 'idct.c', 'mc.c', 'inter.c',
                           'deblock.c', 'tables.c', 'vlc_tables.c',
                           'vpu_kernels.S')]
# BCMFW_AAC builds (every video build, M4) add the AAC-LC decoder: its
# files (aac/SOURCES) compiled with aac_config.h's environment, as
# aac/test/aac_sim_test.py builds them, and the image's glue
AAC_DIR = os.path.join(HERE, 'aac')
AAC_IMAGE_SOURCES = ['aac/aac_image.c', 'aac/brprobe.S']
AAC_CFLAGS = ['-std=gnu99', '-funsigned-char',
              '-include', os.path.join(AAC_DIR, 'aac_config.h'),
              '-I' + os.path.join(AAC_DIR, 'sys'),
              '-I' + os.path.join(ROOT, 'lib', 'rbcodec', 'codecs', 'libfaad'),
              '-I' + os.path.join(ROOT, 'lib', 'rbcodec', 'codecs', 'lib'),
              '-I' + os.path.join(ROOT, 'lib', 'rbcodec'), '-I' + AAC_DIR,
              # upstream warnings are not ours (aac/test/run.sh builds
              # aac.c with -Werror on the host)
              '-w', '-fno-tree-loop-distribute-patterns']
# The decoder's far data (config.h BCMFW_AAC_FAR, video.h VIDEO_AAC_*):
# the big tables (inverse quantisation, the FFT's bit reversal, the long
# windows) and the channel elements; the code, the hot buffers, the
# twiddles and the Huffman codebooks stay in SRAM.  audio-spike.md
# section 7, "M4".
AAC_FAR = {'iq_table': 'xdata', 'revtab': 'xdata', 'sine_long_1024': 'xdata',
           'kbd_long_1024': 'xdata', 'sce': 'sdbss', 'cpe': 'sdbss'}
AAC_FAR_MERGED_OVL = {'spec_data': 'sdbss'}


def aac_sources():
    with open(os.path.join(AAC_DIR, 'SOURCES')) as f:
        return [os.path.join(ROOT, ln.strip()) for ln in f
                if ln.strip() and not ln.startswith('#')]


def aac_units():
    """The decoder's files as assembly units for C.build."""
    return [('aac_%s.s' % os.path.splitext(os.path.basename(p))[0],
             C.compile_c(p, '-O2', AAC_CFLAGS)) for p in aac_sources()]


# BCMFW_DRIFT builds (stage 5b's mode B) add the drift ops and the cross-fade
DRIFT_SOURCES = ['video/drift.c', 'video/xfade.c', 'video/xfade_kernels.S']
# BCMFW_JOBS builds (the merged display image) add the job executor and its
# glue (docs/research/bcm2722/jobs.md)
JOBS_SOURCES = ['video/jobs_bcm.c', 'video/jobs_kernels.S', 'jobs/jref.c']
# ... and, in the display-only image (BCMFW_JOBS without BCMFW_VIDEO:
# config.h BCMFW_JOBS_DECODE), J_OP_DECODE: jdec over the input ring
# (jobs/jdec_ring.c), and jdec with the shell's decoder and resizer,
# unchanged, built against bcmfw/jpeg/env as tests/jpeg_test.py builds
# them (docs/research/bcm2722/album-render.md section 5)
DECODE_SOURCES = ['jobs/jdec_ring.c', 'video/jdec_kernels.S']
BSCHK_SOURCES = ['video/bschk.c']
JPEG_DIR = os.path.join(HERE, 'jpeg')
REC_DIR = os.path.join(ROOT, 'apps', 'recorder')
JDEC_C = [os.path.join(REC_DIR, f) for f in ('jpeg_load.c', 'resize.c', 'bmp.c')] + \
    [os.path.join(HERE, 'jobs', 'jdec.c'), os.path.join(JPEG_DIR, 'libc.c'),
     # the colour stage on the vector unit (jdec_kernels.S, gen_jdec.py)
     os.path.join(HERE, 'video', 'jdec_vpu.c')]
JDEC_CFLAGS = ['-std=gnu11', '-I' + os.path.join(JPEG_DIR, 'env', 'bcm'),
               '-I' + os.path.join(JPEG_DIR, 'env'), '-I' + REC_DIR, '-I' + JPEG_DIR,
               '-I' + os.path.join(HERE, 'jobs'), '-I' + os.path.join(HERE, 'video'),
               '-fno-tree-loop-distribute-patterns', '-Wno-attributes',
               # no progressive store in the display-only image (no room, no
               # SDRAM): such a file is UNSUPPORTED here and the PP's twin
               # makes the same pixels (jpeg/jprog.h)
               '-DJDEC_NO_PROGRESSIVE']


# BCMFW_VIZ (the visualizer image: the display-only image with SPECTRUM and
# BARS in place of DECODE; #126, proposed, sim only): the engine
# (jobs/jviz.c, one definition with the PP), its vector FFT and the glue
VIZ_SOURCES = ['jobs/jviz.c', 'jobs/jref_viz.c', 'video/viz_vpu.c', 'video/viz_kernels.S']


# The display-only image's overlay (config.h BCMFW_JOBS_OVL, jobs.h
# J_OP_OVERLAY; proposed, sim only): the visualizer's ops linked at the
# region's base against the image's map (build_overlay), its own jviz.c,
# FFT kernels and tables.  OVL_IMPORTS: what it may use of the image, in
# a fixed order (the interface: J_OVL_ABI); vc2cc's __vc2_abi hashes
# their addresses, so an overlay loads only into the build it was linked
# against.  Everything else it needs (libgcc) is linked into it.
OVL_SOURCES = ['video/viz_ovl.c', 'jobs/jref_viz.c', 'jobs/jviz.c', 'video/viz_vpu.c',
               'video/viz_kernels.S']
OVL_IMPORTS = ['jref_f16w', 'jv_store16']
# MERGED IMAGE, next re-verify batch (album-render.md section 5.3): the
# merged image's overlay is DECODE (video/dec_ovl.c): jdec, the decoder,
# the ring reader and the vector colour stage, as the display-only image
# carries them resident; its one call into the image is the service
DEC_OVL_SOURCES = ['video/dec_ovl.c', 'jobs/jdec_ring.c', 'video/jdec_kernels.S']
DEC_OVL_IMPORTS = ['bcmfw_service', 'jv_store16', 'jv_store16_12', 'jv_store16_8']


def ovl_kind(defines=()):
    """'decode' for the merged image's overlay, 'viz' for the
    display-only image's"""
    return 'decode' if config_value('BCMFW_VIDEO', defines) else 'viz'


def ovl_name(defines=()):
    """the overlay's file stem: dec-ovl (the merged image's) or viz-ovl"""
    return 'dec-ovl' if ovl_kind(defines) == 'decode' else 'viz-ovl'


def ovl_imports(defines=()):
    return DEC_OVL_IMPORTS if ovl_kind(defines) == 'decode' else OVL_IMPORTS
OVL_MARGIN = 1024           # the region keeps at least this much spare


def ovl_on(defines=()):
    """config.h BCMFW_JOBS_OVL: the display-only image's overlay stub"""
    return bool(config_value('BCMFW_JOBS', defines)) and \
        not config_value('BCMFW_VIZ', defines) and \
        bool(config_value('BCMFW_JOBS_OVL', defines))


def viz_on(defines=()):
    return bool(config_value('BCMFW_JOBS', defines)) and \
        bool(config_value('BCMFW_VIZ', defines))


def jdec_units():
    """jdec and the decoder as assembly units for C.build."""
    return [('jdec_%s.s' % os.path.splitext(os.path.basename(p))[0],
             C.compile_c(p, '-O2', JDEC_CFLAGS)) for p in JDEC_C]


def decode_on(defines=()):
    """config.h BCMFW_JOBS_DECODE: the display-only image (jobs, no video)."""
    return bool(config_value('BCMFW_JOBS', defines)) and \
        not config_value('BCMFW_VIDEO', defines) and \
        not config_value('BCMFW_VIZ', defines)

# The warm swap's park stub (HP_OP_PARK): assembled in every build, empty
# unless BCMFW_HOSTPROTO
PARK_SOURCES = ['park.S']
# BCMFW_R3D builds (the 3D render test) add the renderer and its command
R3D_SOURCES = ['r3d/r3d.c', 'r3d/r3d_scene.c', 'r3d/r3d_cmd.c']
VIDEO_CFLAGS = ['-I' + os.path.join(HERE, 'h264'), '-I' + os.path.join(HERE, 'video'),
                '-DH264_VPU=1', '-DH264_VPU_MC=1', '-DH264_VPU_IDCT=1',
                '-DH264_VPU_DBK=1', '-DH264_VPU_INTRA=1',
                # dev14: the C kernels too, chosen per OPEN (HP_OPEN_C_*)
                '-DH264_VPU_REF=1', '-DH264_VPU_RUNTIME=1',
                '-DH264_PROG_WORD=0x1C4']   # video.h VIDEO_MB_PROG
LIB_SOURCES = ['lib/string.c']
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
          # never turn a copy/clear loop into a call to memcpy/memset
          '-fno-tree-loop-distribute-patterns']
DEFAULT_OUT = os.path.join(ROOT, 'build', 'bcmfw')


def _put(path, data):
    """path's contents become data (str or bytes), whole: written to a
    temporary file and renamed over it, and left alone when equal.  Several
    benches build into the same out directory at once (a gate's BCM_JOBS),
    and a truncate-then-write let one read another's empty lib/string.s or
    half a .bin"""
    mode = 'b' if isinstance(data, (bytes, bytearray)) else ''
    try:
        with open(path, 'r' + mode) as f:
            if f.read() == data:
                return
    except FileNotFoundError:
        pass
    tmp = '%s.%d.tmp' % (path, os.getpid())
    with open(tmp, 'w' + mode) as f:
        f.write(data)
    os.replace(tmp, path)


def _map(prog, lines=()):
    """prog's map text, with report lines after it"""
    f = io.StringIO()
    prog.write_map(f)
    return f.getvalue() + ('\n' + '\n'.join(lines) + '\n' if lines else '')


_CONFIG_MACROS = {}


def _config_macros(defines):
    """config.h's macros as the compiler sees them for these -D defines:
    the preprocessor resolves every #if/#ifndef, which a regex can't (a
    regex took the merged image's BCMFW_VIDEO_ARENA for every image)."""
    key = tuple(defines)
    if key not in _CONFIG_MACROS:
        out = subprocess.run(['cc', '-E', '-dM', '-x', 'c', '-'] +
                             ['-D' + d for d in defines],
                             input='#include "config.h"\n', cwd=HERE,
                             capture_output=True, text=True, check=True).stdout
        macros = {}
        for line in out.splitlines():
            m = re.match(r'#define\s+(\w+)\s+(.*)$', line)
            if m:
                macros[m.group(1)] = m.group(2).strip()
        _CONFIG_MACROS[key] = macros
    return _CONFIG_MACROS[key]


def config_value(name, defines=()):
    """A numeric #define from config.h, with the -D defines applied: a
    number or an arithmetic expression of numbers and other macros."""
    macros = _config_macros(defines)
    if name not in macros:
        raise KeyError(name)

    def expand(text, depth=0):
        if depth > 16:
            raise ValueError('%s: macro nesting too deep' % name)
        return re.sub(r'\b[A-Za-z_]\w*\b',
                      lambda m: '(%s)' % expand(macros[m.group(0)], depth + 1)
                      if m.group(0) in macros else m.group(0), text)

    expr = expand(macros[name] or '1')
    expr = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', expr)
    if not re.fullmatch(r'[\s0-9a-fA-FxX()+\-*/%<>|&~^]*', expr):
        raise ValueError('%s: not a numeric expression: %s' % (name, expr))
    return int(eval(expr.replace('/', '//'), {'__builtins__': {}}))


def jobs_value(name):
    """A constant from jobs.h: a number or a product/sum of numbers."""
    text = open(os.path.join(HERE, 'jobs.h')).read()
    m = re.search(r'^#define\s+%s\s+(\([\d\s*+]+\)|0[xX][0-9a-fA-F]+|\d+)' % name, text, re.M)
    if not m:
        raise KeyError(name)
    return eval(m.group(1), {'__builtins__': {}})


def aac_on(defines=()):
    """BCMFW_AAC: an explicit -D, else as BCMFW_VIDEO (config.h)."""
    for d in defines:
        k, _, v = d.partition('=')
        if k == 'BCMFW_AAC':
            return bool(int(v or '1', 0))
    return bool(config_value('BCMFW_VIDEO', defines))


def have_gcc():
    return os.path.exists(C.GCC)


def build(out=DEFAULT_OUT, defines=(), listing=True, conservative=True):
    """Build into out; return (Program, report dict).  Raises on errors.
    conservative (default): only instruction kinds Apple's own code
    exercises in the vc2sim self-test (vc2cc --conservative)."""
    os.makedirs(os.path.join(out, 'lib'), exist_ok=True)
    extra = ['-I' + HERE] + ['-D' + d for d in defines] + CFLAGS
    # the libc subset becomes a library directory: linked on demand
    for src in LIB_SOURCES:
        text = C.compile_c(os.path.join(HERE, src), '-O2', extra)
        name = os.path.splitext(os.path.basename(src))[0] + '.s'
        _put(os.path.join(out, 'lib', name), text)
    entry = config_value('BCMFW_ENTRY', defines)
    sources = list(SOURCES)
    if config_value('BCMFW_VIDEO', defines):
        sources += VIDEO_SOURCES
        extra += VIDEO_CFLAGS
    if config_value('BCMFW_DRIFT', defines):
        sources += DRIFT_SOURCES
        extra += ['-I' + os.path.join(HERE, 'video')]
    sources += PARK_SOURCES     # empty unless BCMFW_HOSTPROTO (config.h
                                # sets that inside #if blocks, which
                                # config_value() can't follow)
    if config_value('BCMFW_JOBS', defines):
        sources += JOBS_SOURCES
        # the executor with the vector hooks and DECODE's (jobs/jref.h):
        # the PP's CPU path builds it without them
        extra += ['-I' + os.path.join(HERE, 'jobs'), '-DJREF_HOOKS=1']
    if decode_on(defines):
        sources += DECODE_SOURCES
    if config_value('BCMFW_JOBS', defines) and config_value('BCMFW_VIDEO', defines):
        # the merged image: batch 4's form checks and load timings
        # (video/bschk.c, gen_bschk.py; bench information only)
        sources += BSCHK_SOURCES
        if config_value('BCMFW_BSCHK', defines):
            sources += ['video/bschk_kernels.S']
    exports = None
    if ovl_on(defines):
        extra += ['-DJREF_EXT=1']
        exports = (ovl_imports(defines), jobs_value('J_OVL_ABI'))
    if viz_on(defines):
        sources += VIZ_SOURCES
        extra += ['-DJREF_VIZ=1', '-I' + os.path.join(HERE, 'video')]
    if config_value('BCMFW_R3D', defines):
        sources += R3D_SOURCES
        extra += ['-I' + os.path.join(HERE, 'r3d')]
    inputs = [os.path.join(HERE, s) for s in sources]
    place = far = None
    if aac_on(defines):
        inputs += [os.path.join(HERE, s) for s in AAC_IMAGE_SOURCES] + aac_units()
        place, far = dict(AAC_FAR), config_value('BCMFW_AAC_FAR', defines)
        if ovl_on(defines):     # (aac_on: a video image, so the merged one)
            # MERGED IMAGE, next re-verify batch: the overlay loader and
            # the decode's service need 640 B of image, past the 68 B
            # left and the arena's 0x100 move: one frame's spectral data
            # (2 KB, written once and read once per AAC frame, video
            # sessions only) goes to the far bss
            place.update(AAC_FAR_MERGED_OVL)
    if decode_on(defines):
        inputs += jdec_units()
    prog = C.build(inputs, '-O2', base=0,
                   extra=extra, libs=[os.path.join(out, 'lib')] + C.DEFAULT_LIBS,
                   entry=entry, no_float=True, conservative=conservative,
                   place=place, far_base=far, exports=exports)
    image = prog.load_image()
    _put(os.path.join(out, 'bcmfw.bin'), image)
    if listing:
        _put(os.path.join(out, 'bcmfw.lst'), C.listing(prog) + '\n')
    rep = report(prog, defines)
    _put(os.path.join(out, 'bcmfw.map'), _map(prog, rep['lines']))
    return prog, rep


def ovl_flags(defines=()):
    """The overlay's compile flags: the resident image's that shape struct
    jref and the job block (JREF_HOOKS, JREF_EXT, config.h with the same
    -D defines), so the two agree on every structure they share"""
    return ['-I' + HERE, '-I' + os.path.join(HERE, 'jobs'), '-I' + os.path.join(HERE, 'video')] + \
        ['-D' + d for d in defines] + CFLAGS + ['-DJREF_HOOKS=1', '-DJREF_EXT=1',
                                                 '-DJVIZ_OVL=1']


def build_overlay(resident, out=DEFAULT_OUT, defines=(), listing=True,
                  conservative=True, abi=None, kind=None):
    """The visualizer's overlay for the display-only image built with these
    defines (resident: its Program, or {symbol: address} from its map):
    linked at config.h BCMFW_JOBS_OVL, its calls into the image through
    OVL_IMPORTS.  Writes viz-ovl.bin (the PP's upload: the load image,
    a whole number of words), viz-ovl.map and viz-ovl.lst into out.
    abi: tests only, another interface version (J_OVL_ABI).  kind: 'viz'
    or 'decode', the overlay's ops (default ovl_kind: the image's own);
    either links against the image's exports (ovl_imports), so the
    merged image takes the visualizer's overlay too (viz-ovl-m.bin,
    visualizer.md 8.5).  Returns (Program, report dict); raises C.Gap if
    it doesn't fit."""
    if not ovl_on(defines):
        raise C.Gap('no overlay region in this image (BCMFW_JOBS_OVL)')
    os.makedirs(os.path.join(out, 'lib'), exist_ok=True)
    flags = ovl_flags(defines)
    for src in LIB_SOURCES:
        _put(os.path.join(out, 'lib', os.path.splitext(os.path.basename(src))[0] + '.s'),
             C.compile_c(os.path.join(HERE, src), '-O2', flags))
    syms = resident.symbols if hasattr(resident, 'symbols') else resident
    base = config_value('BCMFW_JOBS_OVL', defines)
    kind = kind or ovl_kind(defines)
    imports = C.Imports(syms, ovl_imports(defines),
                        jobs_value('J_OVL_ABI') if abi is None else abi)
    if kind == 'decode':
        # the merged image's DECODE (its flags as the display-only
        # image's jdec: JDEC_CFLAGS, no progressive store)
        inputs = [os.path.join(HERE, s) for s in DEC_OVL_SOURCES] + jdec_units()
        name = 'dec-ovl'
    else:
        inputs = [os.path.join(HERE, s) for s in OVL_SOURCES]
        name = 'viz-ovl'
    prog = C.build(inputs, '-O2', base=base,
                   extra=flags, libs=[os.path.join(out, 'lib')] + C.DEFAULT_LIBS,
                   no_float=True, conservative=conservative, imports=imports)
    image = prog.load_image()
    image += b'\0' * (-len(image) % 4)
    _put(os.path.join(out, name + '.bin'), image)
    if listing:
        _put(os.path.join(out, name + '.lst'), C.listing(prog) + '\n')
    rep = ovl_report(prog, image, defines, kind)
    _put(os.path.join(out, name + '.map'), _map(prog, rep['lines']))
    return prog, rep


HIBSTUB_SOURCES = ['hibstub.c']


def hibwake_value(name):
    """A constant from hibwake.h (the stub's and the PP's addresses)"""
    text = open(os.path.join(HERE, 'hibwake.h')).read()
    m = re.search(r'^#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)\b' % name, text, re.M)
    if not m:
        raise KeyError(name)
    return int(m.group(1), 0)


def build_hibstub(out=DEFAULT_OUT, listing=True, conservative=True):
    """The hibernate park's stub (hibwake.h, power-contract.md section
    6.6): linked at HW_STUB, self-contained (no calls into any image), its
    header first.  Writes bcmhib.bin (the PP's upload, whole words),
    bcmhib.map and bcmhib.lst into out.  Returns (Program, report dict);
    raises C.Gap if it doesn't fit HW_STUB_MAX or its header is wrong."""
    os.makedirs(os.path.join(out, 'lib'), exist_ok=True)
    flags = ['-I' + HERE] + CFLAGS
    for src in LIB_SOURCES:
        _put(os.path.join(out, 'lib', os.path.splitext(os.path.basename(src))[0] + '.s'),
             C.compile_c(os.path.join(HERE, src), '-O2', flags))
    base, room = hibwake_value('HW_STUB'), hibwake_value('HW_STUB_MAX')
    prog = C.build([os.path.join(HERE, s) for s in HIBSTUB_SOURCES], '-O2', base=base,
                   extra=flags, libs=[os.path.join(out, 'lib')] + C.DEFAULT_LIBS,
                   no_float=True, conservative=conservative)
    image = prog.load_image()
    image += b'\0' * (-len(image) % 4)
    _put(os.path.join(out, 'bcmhib.bin'), image)
    _put(os.path.join(out, 'bcmhib.map'), _map(prog))
    if listing:
        _put(os.path.join(out, 'bcmhib.lst'), C.listing(prog) + '\n')
    e, errors = prog.ends, []
    words = [int.from_bytes(image[4 * k:4 * k + 4], 'little') for k in range(5)]
    if prog.symbols.get('hw_header') != base:
        errors.append('stub header (hw_header) not at 0x%x' % base)
    if words[:4] != [hibwake_value('HW_STUB_MAGIC'), hibwake_value('HW_STUB_VERSION'),
                     base, base + len(image)] or not base < words[4] < base + len(image):
        errors.append('stub header %s' % ' '.join('%08x' % w for w in words))
    if e['bss'] != e['data']:
        errors.append('the stub has .bss (%d bytes): the PP uploads the load image only' % (
            e['bss'] - e['data']))
    if base + len(image) > base + room:
        errors.append('stub %d bytes, HW_STUB_MAX is %d' % (len(image), room))
    if errors:
        raise C.Gap('; '.join(errors))
    lines = ['hibernate stub: 0x%x-0x%x (%d bytes of %d), entry 0x%x' % (
        base, base + len(image), len(image), room, words[4])]
    _put(os.path.join(out, 'bcmhib.map'), _map(prog, lines))
    return prog, {'load': len(image), 'image': image, 'entry': words[4], 'lines': lines}


def ovl_report(prog, image, defines=(), kind=None):
    """The overlay's layout checks: the header first at the region's base,
    its load image and .bss inside the region with OVL_MARGIN spare, the
    header's words as the resident checks them"""
    base = config_value('BCMFW_JOBS_OVL', defines)
    size = config_value('BCMFW_JOBS_OVL_SIZE', defines)
    e, errors = prog.ends, []
    end = e['bss']
    hdr = 'dec_ovl' if (kind or ovl_kind(defines)) == 'decode' else 'viz_ovl'
    if prog.symbols.get(hdr) != base:
        errors.append('overlay header (%s) not at the region base 0x%x' % (hdr, base))
    if base & 0xFFF or size & 0xFFF:
        errors.append('overlay region 0x%x+0x%x: not whole 4 KB (jobs.h J_OVL)' % (base, size))
    if end + OVL_MARGIN > base + size:
        errors.append('overlay ends at 0x%x: less than %d bytes spare below 0x%x' % (
            end, OVL_MARGIN, base + size))
    words = [int.from_bytes(image[4 * k:4 * k + 4], 'little') for k in range(5)]
    want = [jobs_value('J_OVL_MAGIC'), prog.abi, base, base + len(image), end]
    if words[:4] != want[:4] or words[4] < want[4]:
        errors.append('overlay header %s, want %s' % (
            ' '.join('%08x' % w for w in words), ' '.join('%08x' % w for w in want)))
    sizes = {'text': e['text'] - e['vectors'], 'rodata': e['rodata'] - e['text'],
             'data': e['data'] - e['rodata'], 'bss': e['bss'] - e['data']}
    lines = ['overlay: 0x%x-0x%x (load %d bytes: header %d, text %d, rodata %d, data %d; '
             'bss %d), %d of the region\'s %d bytes spare; ABI %08x' % (
                 base, end, len(image), e['vectors'] - base, sizes['text'], sizes['rodata'],
                 sizes['data'], sizes['bss'], base + size - end, size, prog.abi)]
    if errors:
        raise C.Gap('; '.join(errors))
    return {'load': len(image), 'end': end, 'spare': base + size - end, 'abi': prog.abi,
            'sizes': sizes, 'lines': lines}


def report(prog, defines=()):
    sram = config_value('BCMFW_SRAM_SIZE', defines)
    stack_top = config_value('BCMFW_STACK_TOP', defines)
    stack_size = config_value('BCMFW_STACK_SIZE', defines)
    fb = config_value('BCMFW_FB', defines)
    e = prog.ends
    text = e['text'] - prog.symbols['__text_start']
    sizes = {'vectors': e['vectors'] - prog.base, 'text': text,
             'rodata': e['rodata'] - e['text'], 'data': e['data'] - e['rodata'],
             'bss': e['bss'] - e['data']}
    load = len(prog.load_image())
    lines = ['size: load image %d bytes (vectors+mailbox %d, text %d, rodata %d, '
             'data %d), bss %d' % (load, prog.symbols['__text_start'], sizes['text'],
                                   sizes['rodata'], sizes['data'], sizes['bss']),
             'SRAM: image+bss 0x0-0x%x = %.2f%% of %d KB; stack 0x%x-0x%x; '
             'framebuffer 0x%x-0x%x' % (e['bss'], 100.0 * e['bss'] / sram,
                                         sram // 1024, stack_top - stack_size,
                                         stack_top, fb, fb + 320 * 240 * 2)]
    errors = []
    limit = config_value('BCMFW_IMAGE_LIMIT', defines)
    if e['bss'] > limit:
        errors.append('image+bss (0x%x) runs past 0x%x, the decoder working set' % (
            e['bss'], limit))
    if config_value('BCMFW_VIDEO', defines) and \
            e['bss'] > config_value('BCMFW_VIDEO_ARENA', defines):
        errors.append('image+bss (0x%x) runs into the decoder arena (0x%x)' % (
            e['bss'], config_value('BCMFW_VIDEO_ARENA', defines)))
    if config_value('BCMFW_VIDEO', defines):
        # a 320x240 stream with 4 references keeps its frames in SRAM
        # (h264_arena_size(20, 15, 4) = 617,024, plus 32 to align)
        arena = stack_top - stack_size - config_value('BCMFW_VIDEO_ARENA', defines)
        if arena < 617056:
            errors.append('the decoder arena (%d bytes) no longer holds 320x240 '
                          'with 4 references (617,056)' % arena)
    if 'xdata' in e:
        # the far data (AAC): inside its SDRAM window; parked below the
        # stack, above the image, until the first OPEN copies it
        far = config_value('BCMFW_AAC_FAR', defines)
        far_end = far + config_value('BCMFW_AAC_FAR_SIZE', defines)
        top = max(e['xdata'], e.get('sdbss', 0))
        if prog.symbols['__xdata_start'] != far or top > far_end:
            errors.append('far data 0x%x-0x%x outside 0x%x-0x%x' % (
                prog.symbols['__xdata_start'], top, far, far_end))
        park = config_value('BCMFW_AAC_PARK_END', defines) - (
            prog.symbols['__xdata_end'] - prog.symbols['__xdata_start'])
        if park < e['bss']:
            errors.append('the parked tables (from 0x%x) overlap the image' % park)
        lines.append('far (SDRAM): xdata 0x%x-0x%x (%d bytes, loaded at 0x%x, '
                     'parked at 0x%x), sdbss to 0x%x (%d bytes)' % (
                         far, e['xdata'], e['xdata'] - far,
                         prog.symbols['__xdata_load'], park, e['sdbss'],
                         e['sdbss'] - prog.symbols['__sdbss_start']))
    if e['bss'] > stack_top - stack_size:
        errors.append('image+bss (0x%x) runs into the stack (0x%x)' % (
            e['bss'], stack_top - stack_size))
    if stack_top > fb:
        errors.append('stack top above the framebuffer')
    if config_value('BCMFW_DRIFT', defines):
        # the covers and planes (config.h BCMFW_DRIFT_*) overlap nothing
        side = config_value('BCMFW_DRIFT_SIDE', defines)
        cover, planes = 2 * side * side, 3 * 2 * 160 * 240
        drift = [('drift cover 1', config_value('BCMFW_DRIFT_SLOT1', defines), cover),
                 ('drift state planes', config_value('BCMFW_DRIFT_STATE', defines), planes),
                 ('drift fade planes', config_value('BCMFW_DRIFT_SRC', defines), planes),
                 ('drift staging', config_value('BCMFW_DRIFT_STG', defines), 2 * 160 * 240),
                 ('the framebuffer', fb, 320 * 240 * 2),
                 ('drift parameters', config_value('BCMFW_DRIFT_PARAM', defines), 64),
                 ('drift cover 0', config_value('BCMFW_DRIFT_SLOT0', defines), cover),
                 ('image+bss', 0, e['bss']),
                 ('the stack', stack_top - stack_size, stack_size)]
        for i, (a, lo, n) in enumerate(drift):
            if lo + n > config_value('BCMFW_SRAM_SIZE', defines):
                errors.append('%s runs past SRAM' % a)
            for b, lo2, n2 in drift[i + 1:]:
                if lo < lo2 + n2 and lo2 < lo + n:
                    errors.append('%s (0x%x-0x%x) overlaps %s (0x%x-0x%x)' % (
                        a, lo, lo + n, b, lo2, lo2 + n2))
    if config_value('BCMFW_R3D', defines):
        # the renderer's tables and the second frame (config.h BCMFW_R3D_*)
        r3d = [('r3d textures', config_value('BCMFW_R3D_TEX', defines), 8 * 64 * 64),
               ('r3d colour table', config_value('BCMFW_R3D_CLUT', defines), 48 * 256 * 2),
               ('r3d vertices', config_value('BCMFW_R3D_VERTS', defines), 2048 * 8),
               ('r3d triangles', config_value('BCMFW_R3D_TRIS', defines), 4096 * 16),
               ('r3d view vertices', config_value('BCMFW_R3D_XVERTS', defines), 2048 * 20),
               ('r3d ordering table', config_value('BCMFW_R3D_OT', defines), 512 * 2),
               ('r3d screen triangles', config_value('BCMFW_R3D_PRIMS', defines), 6144 * 24),
               ('r3d parameters', config_value('BCMFW_R3D_PARAM', defines),
                4 * config_value('BCMFW_R3D_P_N', defines)),
               ('r3d second frame', config_value('BCMFW_R3D_FB2', defines), 320 * 240 * 2),
               ('the framebuffer', fb, 320 * 240 * 2),
               ('image+bss', 0, e['bss']),
               ('the stack', stack_top - stack_size, stack_size)]
        for i, (a, lo, n) in enumerate(r3d):
            if lo + n > config_value('BCMFW_SRAM_SIZE', defines):
                errors.append('%s runs past SRAM' % a)
            for b, lo2, n2 in r3d[i + 1:]:
                if lo < lo2 + n2 and lo2 < lo + n:
                    errors.append('%s (0x%x-0x%x) overlaps %s (0x%x-0x%x)' % (
                        a, lo, lo + n, b, lo2, lo2 + n2))
    if decode_on(defines) or viz_on(defines):
        # the display-only image's layout (config.h BCMFW_JOBS_DO_*): the
        # image below DECODE's work area, which holds the NP box (jobs.h
        # J_DEC_WORK_MIN), and the pools clear of everything else
        work = config_value('BCMFW_JOBS_DEC_WORK', defines)
        drift = config_value('BCMFW_JOBS_DO_DRIFT', defines)
        jmin = int(re.search(r'^#define\s+J_DEC_WORK_MIN\s+(\d+)',
                             open(os.path.join(HERE, 'jobs.h')).read(), re.M).group(1))
        # J_DEC_WORK_MIN holds at least the NP box: jdec.h's jdec_work()
        # for one 142-wide output (JDEC_WORK_BASE + JDEC_WORK_ROWS(142) +
        # JDEC_WORK_OUT(142))
        jh = open(os.path.join(HERE, 'jobs', 'jdec.h')).read()
        base = re.search(r'^#define\s+JDEC_WORK_BASE\s+\((\d+)u \* 1024\)', jh, re.M)
        rows = re.search(r'^#define\s+JDEC_WORK_ROWS\(dw\)\s+\(\(uint32_t\)\(dw\) \* (\d+) \+ (\d+)\)',
                         jh, re.M)
        out = re.search(r'^#define\s+JDEC_WORK_OUT\(dw\)\s+\(\(uint32_t\)\(dw\) \* (\d+) \+ (\d+)\)',
                        jh, re.M)
        if not (base and rows and out):
            errors.append('jdec.h: JDEC_WORK_BASE, _ROWS or _OUT not found')
        else:
            np_box = (int(base.group(1)) * 1024 + 142 * int(rows.group(1)) + int(rows.group(2)) +
                      142 * int(out.group(1)) + int(out.group(2)))
            if jmin < np_box:
                errors.append('J_DEC_WORK_MIN (%d) is below jdec_work() of the NP box (%d)' % (
                    jmin, np_box))
        if e['bss'] > work:
            errors.append('image+bss (0x%x) runs into the decode work area (0x%x)' % (
                e['bss'], work))
        if drift - work < jmin:
            errors.append('decode work area %d bytes, J_DEC_WORK_MIN is %d' % (
                drift - work, jmin))
        lines.append('decode work 0x%x-0x%x (%d bytes, %d above J_DEC_WORK_MIN; '
                     'image+bss %d below it)' % (work, drift, drift - work, drift - work - jmin,
                                                 work - e['bss']))
        regions = [('image+bss', 0, e['bss']), ('decode work', work, drift - work),
                   ('J_CLS_DRIFT', drift, jobs_value('J_BUDGET_DRIFT')),
                   ('J_CLS_STRIP', config_value('BCMFW_JOBS_DO_STRIP', defines), 128 * 1024),
                   ('J_CLS_APP', config_value('BCMFW_JOBS_DO_APP', defines),
                    jobs_value('J_BUDGET_APP')),
                   ('J_CLS_THUMB', config_value('BCMFW_JOBS_DO_THUMB', defines),
                    jobs_value('J_BUDGET_THUMB')),
                   ('the stack', stack_top - stack_size, stack_size),
                   ('the rings and DMA blocks', 0xD0000, 0x4000),
                   ('J_CLS_SCRATCH', 0xD4000, 0x8000),
                   ('the framebuffer', fb, 320 * 240 * 2),
                   ('J_SURF_BACK', 0x105800, 320 * 240 * 2)]
        if ovl_on(defines):
            regions.append(('the overlay region', config_value('BCMFW_JOBS_OVL', defines),
                            config_value('BCMFW_JOBS_OVL_SIZE', defines)))
            lines.append('overlay region 0x%x-0x%x; ABI %08x over %s' % (
                config_value('BCMFW_JOBS_OVL', defines),
                config_value('BCMFW_JOBS_OVL', defines) +
                config_value('BCMFW_JOBS_OVL_SIZE', defines), prog.abi,
                ', '.join(OVL_IMPORTS)))
        for i, (a, lo, n) in enumerate(regions):
            if lo + n > sram:
                errors.append('%s runs past SRAM' % a)
            for b, lo2, n2 in regions[i + 1:]:
                if lo < lo2 + n2 and lo2 < lo + n:
                    errors.append('%s (0x%x-0x%x) overlaps %s (0x%x-0x%x)' % (
                        a, lo, lo + n, b, lo2, lo2 + n2))
    elif config_value('BCMFW_JOBS', defines):
        # the merged image's pools (config.h BCMFW_JOBS_*): inside the
        # decoder arena, which they share in UI mode, and apart
        arena = config_value('BCMFW_VIDEO_ARENA', defines)
        pools = [('J_CLS_DRIFT', config_value('BCMFW_JOBS_DRIFT', defines),
                  jobs_value('J_BUDGET_DRIFT')),
                 ('J_CLS_STRIP', config_value('BCMFW_JOBS_STRIP', defines), 128 * 1024),
                 ('J_CLS_THUMB', config_value('BCMFW_JOBS_THUMB', defines),
                  jobs_value('J_BUDGET_THUMB')),
                 ('J_CLS_APP', config_value('BCMFW_JOBS_APP', defines),
                  jobs_value('J_BUDGET_APP'))]
        for i, (a, lo, n) in enumerate(pools):
            if lo < arena or lo + n > stack_top - stack_size:
                errors.append('%s (0x%x-0x%x) outside the arena (0x%x-0x%x)' % (
                    a, lo, lo + n, arena, stack_top - stack_size))
            if lo & 31:
                errors.append('%s at 0x%x: not 32-byte aligned' % (a, lo))
            for b, lo2, n2 in pools[i + 1:]:
                if lo < lo2 + n2 and lo2 < lo + n:
                    errors.append('%s (0x%x-0x%x) overlaps %s (0x%x-0x%x)' % (
                        a, lo, lo + n, b, lo2, lo2 + n2))
    if 'bcmfw_park_stub' in prog.symbols:
        # the warm swap: the stub fits its place, and no image's upload
        # (any build's load image) reaches it while it runs
        stub = config_value('BCMFW_PARK_STUB', defines)
        n = prog.symbols['bcmfw_park_stub_end'] - prog.symbols['bcmfw_park_stub']
        if prog.symbols['bcmfw_park_stub'] & 3:
            errors.append('park stub at 0x%x: not word aligned (hostproto.c copies '
                          'words)' % prog.symbols['bcmfw_park_stub'])
        if n > config_value('BCMFW_PARK_STUB_MAX', defines):
            errors.append('park stub %d bytes, BCMFW_PARK_STUB_MAX is %d' % (
                n, config_value('BCMFW_PARK_STUB_MAX', defines)))
        if len(prog.load_image()) > stub:
            errors.append('load image (%d bytes) reaches the park stub (0x%x)' % (
                len(prog.load_image()), stub))
        if stub < 0xDC000 or stub + n > fb:
            errors.append('park stub 0x%x overlaps the rings or the framebuffer' % stub)
    # the probe v2 results block (probe builds write it) overlaps nothing
    probe = config_value('BCMFW_PROBE_RESULTS', defines)
    psize = 4 * (config_value('BCMFW_PROBE_HDR_WORDS', defines) +
                 config_value('BCMFW_PROBE_REC_WORDS', defines) *
                 (config_value('BCMFW_PROBE_NCHECKS', defines) + 1))
    bench = config_value('BCMFW_BENCH_RESULTS', defines)
    others = [('image+bss', 0, e['bss']),
              ('the stack', stack_top - stack_size, stack_top),
              ('the framebuffer', fb, fb + 320 * 240 * 2),
              ('the bench block', bench,
               bench + config_value('BCMFW_BENCH_RESULTS_SIZE', defines)),
              ('the host-protocol rings', 0xD0000, 0xDC000)]
    if 'bench_nkernels' in prog.symbols:       # bench.h: header + two tables
        a = prog.symbols['bench_nkernels'] - prog.base
        nk = int.from_bytes(prog.load_image()[a:a + 4], 'little')
        need = 4 * (32 + 2 * 8 * nk)
        if need > 4 * 0x800:                    # bench.h BR_VREC_AT
            errors.append('bench timing tables (%d bytes) reach the vector records' % need)
        if 'bench_nvcases' in prog.symbols:     # bench.h: 12-word vector records
            a = prog.symbols['bench_nvcases'] - prog.base
            nv = int.from_bytes(prog.load_image()[a:a + 4], 'little')
            need = max(need, 4 * (0x800 + 12 * nv))
        if need > others[3][2] - bench:
            errors.append('bench block needs %d bytes, BCMFW_BENCH_RESULTS_SIZE is %d' % (
                need, others[3][2] - bench))
    for what, lo, hi in others:
        if probe < hi and lo < probe + psize:
            errors.append('probe results block 0x%x-0x%x overlaps %s (0x%x-0x%x)' % (
                probe, probe + psize, what, lo, hi))
    lines.append('probe results block 0x%x-0x%x (%d bytes)' % (probe, probe + psize, psize))
    if errors:
        raise C.Gap('; '.join(errors))
    return {'load': load, 'end': e['bss'], 'sizes': sizes, 'lines': lines}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('-D', dest='defines', action='append', default=[])
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--overlay', action='store_true',
                    help="also the visualizer's overlay (display-only image)")
    a = ap.parse_args(argv)
    if not have_gcc():
        print('bcmfw: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC,
              file=sys.stderr)
        return 2
    try:
        prog, rep = build(a.out, a.defines)
        if a.overlay:
            rep['lines'] += build_overlay(prog, a.out, a.defines)[1]['lines']
    except (C.Gap, RuntimeError, KeyError) as e:
        print('bcmfw: %s' % (e.args[0] if e.args else e), file=sys.stderr)
        return 1
    if not a.quiet:
        print('\n'.join(rep['lines']))
        print('wrote %s/bcmfw.bin, bcmfw.map, bcmfw.lst' % a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
