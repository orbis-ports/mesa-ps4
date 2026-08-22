#!/usr/bin/env python3
"""Compile every shader under tools/shaders/ and emit the C arrays both probes compile.

    python3 tools/shaders/gen.py

KEPT AS A SCRIPT RATHER THAN A BUILD STEP because the PS4 side has no shader compiler in its build. The
sources beside the generated files are the truth: re-run this and `git diff` should be empty.
"""

import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# (source, glslang stage, generated header, C symbol)
SHADERS = [
    ("pattern.comp", "comp", "pattern_spv.h", "pattern_comp_spv"),
    ("tri.vert", "vert", "tri_vert_spv.inc", "tri_vert_spv"),
    ("tri.frag", "frag", "tri_frag_spv.inc", "tri_frag_spv"),
]

HEADER_PREAMBLE = """/* GENERATED from %s - do not edit. Regenerate with:
 *
 *     python3 tools/shaders/gen.py
 *
 * COMMITTED RATHER THAN BUILT, and there is exactly one copy: the PS4 build has no shader-compilation step,
 * and a second generated array in the Tempest tree would be two things that have to stay equal. ps4/radv
 * includes these from here.
 *
 * SPDX-License-Identifier: MIT
 */
"""


def emit(words, sym, guard):
    out = []
    if guard:
        out.append("#ifndef %s\n#define %s\n\n#include <stdint.h>\n" % (guard, guard))
    out.append("static const uint32_t %s[] = {" % sym)
    for i in range(0, len(words), 6):
        out.append("   " + " ".join("0x%08x," % w for w in words[i:i + 6]))
    out.append("};")
    if guard:
        out.append("\n#endif /* %s */" % guard)
    return "\n".join(out) + "\n"


for src, stage, hdr, sym in SHADERS:
    spv = HERE / (Path(src).stem + "_" + stage + ".spv")
    subprocess.run(["glslangValidator", "-V", "-S", stage, str(HERE / src), "-o", str(spv)], check=True,
                   stdout=subprocess.DEVNULL)
    data = spv.read_bytes()
    if len(data) % 4:
        sys.exit("%s: SPIR-V is not a whole number of words" % src)
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    if words[0] != 0x07230203:
        sys.exit("%s: not SPIR-V" % src)
    guard = "ORBIS_" + hdr.replace(".", "_").upper() if hdr.endswith(".h") else None
    (HERE / hdr).write_text(HEADER_PREAMBLE % src + emit(words, sym, guard))
    # The .spv is an intermediate on the way to the header and nothing reads it afterwards. Three of
    # them sat in this directory checked in for weeks, looking like inputs; delete it here so they
    # cannot come back.
    spv.unlink()
    print("%-14s %4d words -> %s" % (src, len(words), hdr))
