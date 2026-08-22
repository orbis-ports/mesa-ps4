#!/usr/bin/env python3
"""Generate Liverpool's GB_TILE_MODE / GB_MACROTILE_MODE tables from the oracles.

    tools/gen-tile-tables.py [--oracles ~/src/unemups4/oracles] > src/amd/common/orbis_tile_tables.h

WHY GENERATED RATHER THAN TYPED. RADV wants the 32 GB_TILE_MODE and 16 GB_MACROTILE_MODE register words
verbatim (ac_fill_tiling_info memcpy's both arrays), and the values are a transcription of the kernel's
cik_tiling_mode_table_init() - 48 bit-packed constants. Typing those by hand is a class of error this
project cannot check by eye, so the transcription is done by machine from two oracles and every emitted
value carries the symbolic expression it came from.

WHICH BRANCH, AND WHY IT IS THE RIGHT ONE. cik_tiling_mode_table_init() switches on num_pipe_configs and
this takes `case 8:`. That is not an assumption: the Tempest fork MEASURED it (backlog/docs/gnm-tiling.md,
H5 "Liverpool takes the 8-pipe branch of the CIK tile table" and H6 "PIPE_CONFIG is P8_32x32_16x16", both
confirmed by rung 2 of the tiling test passing on the console).

TWO ORACLES, AND THEY AGREE ON EVERY SHIFT:

    field                 gfx7.json bits    kernel-cikd.h macro
    ARRAY_MODE            [2, 5]            (x) << 2
    PIPE_CONFIG           [6, 10]           (x) << 6
    TILE_SPLIT            [11, 13]          (x) << 11
    MICRO_TILE_MODE_NEW   [22, 24]          (x) << 22
    SAMPLE_SPLIT          [25, 26]          (x) << 25
    BANK_WIDTH            [0, 1]            (x) << 0
    BANK_HEIGHT           [2, 3]            (x) << 2
    MACRO_TILE_ASPECT     [4, 5]            (x) << 4
    NUM_BANKS             [6, 7]            (x) << 6

The enum VALUES come from gfx7.json, whose entry names are identical to the kernel's, so nothing is
translated between the two. This script checks the shifts against each other and refuses to emit if they
ever disagree - that check is the point of using both.
"""

import argparse
import json
import os
import re
import sys

FIELDS = {
    'ARRAY_MODE': ('GB_TILE_MODE0', 'ArrayMode'),
    'PIPE_CONFIG': ('GB_TILE_MODE0', 'PipeConfig'),
    'TILE_SPLIT': ('GB_TILE_MODE0', 'TileSplit'),
    'MICRO_TILE_MODE_NEW': ('GB_TILE_MODE0', 'MicroTileMode'),
    'SAMPLE_SPLIT': ('GB_TILE_MODE0', None),
    'BANK_WIDTH': ('GB_MACROTILE_MODE0', 'BankWidth'),
    'BANK_HEIGHT': ('GB_MACROTILE_MODE0', 'BankHeight'),
    'MACRO_TILE_ASPECT': ('GB_MACROTILE_MODE0', 'MacroTileAspect'),
    'NUM_BANKS': ('GB_MACROTILE_MODE0', 'NumBanks'),
}

# Not in gfx7.json's enums: SAMPLE_SPLIT has no enum_ref, and the kernel computes
# split_equal_to_row_size from the memory row size. 2 KB is the kernel's own default arm and matches the
# fork's H7 (gnm-tiling.md: "Row size 2 KB").
EXTRA_ENUMS = {
    'ADDR_SURF_SAMPLE_SPLIT_1': 0,   # kernel-cikd.h:1254
    'ADDR_SURF_SAMPLE_SPLIT_2': 1,   # kernel-cikd.h:1255
    'ADDR_SURF_SAMPLE_SPLIT_4': 2,
    'ADDR_SURF_SAMPLE_SPLIT_8': 3,
}
# The memory row size the tables are built for. NOT a constant any more: the kernel's own arm assumes
# 2 KB and nothing has ever measured Liverpool's, so both variants are emitted and the driver picks one
# at runtime. A table and a GB_ADDR_CONFIG that disagree would be worse than either being wrong, so the
# two must always come from the same knob.
ROW_SIZE_KB = 2


def load_json_oracle(path):
    d = json.load(open(path))
    bits, enums = {}, {}
    for field, (reg, enum_ref) in FIELDS.items():
        f = next(x for x in d['register_types'][reg]['fields'] if x['name'] == field)
        bits[field] = f['bits']
        if enum_ref:
            assert f.get('enum_ref') == enum_ref, (field, f.get('enum_ref'), enum_ref)
    for name, v in d['enums'].items():
        for e in v['entries']:
            if e['name'] in enums and enums[e['name']] != e['value']:
                sys.exit(f"enum {e['name']} has two values in gfx7.json")
            enums[e['name']] = e['value']
    return bits, enums


def load_kernel_shifts(path):
    """The same shifts from the other oracle. Disagreement here is a hard stop."""
    shifts = {}
    pat = re.compile(r'#\s*define\s+([A-Z_]+)\(x\)\s+\(\(x\)\s*<<\s*(\d+)\)')
    for line in open(path, errors='ignore'):
        m = pat.search(line)
        if m and m.group(1) in FIELDS:
            shifts.setdefault(m.group(1), int(m.group(2)))
    return shifts


def extract_case8(path):
    """The tile[]/macrotile[] assignments of cik_tiling_mode_table_init()'s 8-pipe branch."""
    src = open(path, errors='ignore').read()
    i = src.index('switch(num_pipe_configs)')
    j = src.index('case 8:', i)
    k = src.index('case 4:', j)
    body = src[j:k]
    out = []
    for m in re.finditer(r'(tile|macrotile)\[(\d+)\]\s*=\s*\((.*?)\);', body, re.S):
        expr = ' '.join(m.group(3).split())
        out.append((m.group(1), int(m.group(2)), expr))
    return out


def evaluate(expr, bits, enums):
    """Turn 'ARRAY_MODE(ARRAY_2D_TILED_THIN1) | TILE_SPLIT(...)' into a number."""
    value = 0
    for m in re.finditer(r'([A-Z_]+)\(([A-Za-z0-9_]+)\)', expr):
        field, sym = m.group(1), m.group(2)
        if field not in bits:
            sys.exit(f"unknown field {field} in: {expr}")
        if sym == 'split_equal_to_row_size':
            sym = {1: 'ADDR_SURF_TILE_SPLIT_1KB', 2: 'ADDR_SURF_TILE_SPLIT_2KB',
                   4: 'ADDR_SURF_TILE_SPLIT_4KB'}[ROW_SIZE_KB]
        if sym not in enums:
            sys.exit(f"unknown symbol {sym} in: {expr}")
        lo, hi = bits[field]
        v = enums[sym]
        if v > (1 << (hi - lo + 1)) - 1:
            sys.exit(f"{sym}={v} does not fit {field} bits {lo}..{hi}")
        value |= v << lo
    return value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--oracles', default=os.path.expanduser('~/src/unemups4/oracles'))
    a = ap.parse_args()

    js = os.path.join(a.oracles, 'mesa/mesa/src/amd/registers/gfx7.json')
    cik = os.path.join(a.oracles, 'amd/kernel-cik.c')
    cikd = os.path.join(a.oracles, 'amd/kernel-cikd.h')
    for p in (js, cik, cikd):
        if not os.path.exists(p):
            sys.exit(f"missing oracle: {p}")

    bits, enums = load_json_oracle(js)
    enums.update(EXTRA_ENUMS)

    shifts = load_kernel_shifts(cikd)
    for field, (lo, _hi) in bits.items():
        if field in shifts and shifts[field] != lo:
            sys.exit(f"ORACLES DISAGREE on {field}: gfx7.json says bit {lo}, "
                     f"kernel-cikd.h says {shifts[field]}")
    missing = sorted(set(FIELDS) - set(shifts))
    if missing:
        sys.exit(f"kernel-cikd.h did not define shifts for {missing}; cross-check impossible")

    global ROW_SIZE_KB
    variants = {}
    for kb in (2, 4):
        ROW_SIZE_KB = kb
        tile = [(0, None)] * 32
        macro = [(0, None)] * 16
        for kind, idx, expr in extract_case8(cik):
            v = evaluate(expr, bits, enums)
            (tile if kind == 'tile' else macro)[idx] = (v, expr)
        variants[kb] = (tile, macro)

    w = sys.stdout.write
    w("/*\n")
    w(" * GENERATED by tools/gen-tile-tables.py - do not edit.\n *\n")
    w(" * Liverpool's GB_TILE_MODE and GB_MACROTILE_MODE register tables: the 8-pipe branch of the\n")
    w(" * kernel's cik_tiling_mode_table_init(), which the Tempest fork MEASURED as Liverpool's\n")
    w(" * (backlog/docs/gnm-tiling.md H5/H6, confirmed by rung 2 passing on the console).\n *\n")
    w(" * Bit positions cross-checked between two oracles - gfx7.json and kernel-cikd.h - which agree on\n")
    w(" * every field. Enum values from gfx7.json, whose entry names are the kernel's own.\n *\n")
    w(" * ROW SIZE IS NOT AN ASSUMPTION ANY MORE, and the header said it was for as long as this file existed.\n")
    w(" * unemups4 crates/core/src/tiling.rs closes the chain from a real-PS4 capture: DB_DEPTH_INFO = 0x001d0c41\n")
    w(" * gives PIPE_CONFIG = 12 = ADDR_SURF_P8_32x32_16x16, which is EIGHT PIPES; the 8-pipe branch of\n")
    w(" * cik_tiling_mode_table_init() is therefore the one this console runs, and that branch programs\n")
    w(" * split_equal_to_row_size as ADDR_SURF_TILE_SPLIT_2KB - so 2 KB is implied by the branch rather than picked.\n")
    w(" * Four falsifiable cross-checks there back it: macrotile[0] is literally the console's DB_DEPTH_INFO word,\n")
    w(" * macrotile[4]'s 8_BANK predicts an observed pitch no 16-bank row can produce, and tile[13]/tile[14]\n")
    w(" * reproduce array modes derived independently from captured pitch alignments. Permitted source: decision-4.\n")
    w(" *\n")
    w(" * ROW SIZE still sets TILE_SPLIT wherever the kernel writes\n")
    w(" * split_equal_to_row_size, which sets the macro-tile geometry, which is what a GFX7 texture unit\n")
    w(" * uses to derive EVERY MIP LEVEL's address - a T# carries no per-level offsets. An error there is a\n")
    w(" * quarter-area region of the wrong content, not a subtly wrong texel. So both tables are emitted and\n")
    w(" * ac_orbis_drm.c picks one together with the matching GB_ADDR_CONFIG.ROW_SIZE, from one env knob.\n")
    w(" *\n * SPDX-License-Identifier: MIT\n */\n\n")
    w("#ifndef ORBIS_TILE_TABLES_H\n#define ORBIS_TILE_TABLES_H\n\n#include <stdint.h>\n\n")
    for kb in (2, 4):
        tile, macro = variants[kb]
        for name, arr in (('orbis_gb_tile_mode_%dkb' % kb, tile),
                          ('orbis_gb_macro_tile_mode_%dkb' % kb, macro)):
            w("static const uint32_t %s[%d] = {\n" % (name, len(arr)))
            for i, (v, expr) in enumerate(arr):
                if expr is None:
                    w("   [%2d] = 0,   /* the kernel leaves this slot zero in the 8-pipe branch */\n" % i)
                else:
                    w("   [%2d] = 0x%08x,   /* %s */\n" % (i, v, expr))
            w("};\n\n")

    w("/* The 2 KB arm keeps the unsuffixed names: it is what every run before this knob existed used, so a\n")
    w(" * build that ignores the knob behaves exactly as before. */\n")
    w("#define orbis_gb_tile_mode       orbis_gb_tile_mode_2kb\n")
    w("#define orbis_gb_macro_tile_mode orbis_gb_macro_tile_mode_2kb\n\n")
    w("#endif /* ORBIS_TILE_TABLES_H */\n")


if __name__ == '__main__':
    main()
