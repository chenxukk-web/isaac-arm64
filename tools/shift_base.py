#!/usr/bin/env python3
"""shift_base.py -- shift the guest image base as a whole

Why this tool exists
--------------------
In an Android app process, ART occupies part of the low 4GB. The guest image
**must** land on the base chosen at build time (the generated code hard-codes
it), otherwise you get
    guest: PE load failed: cannot allocate fixed guest image

The generator gen_all.py's `--image-base` accepts only 0x30000000 and Vita's
0x98000000 (the contract hashes in COVERAGE_CROSS_ROOT_CONTRACTS_BY_IMAGE_BASE
are hard-wired), and adding a new base means recomputing the coverage
contracts, which is expensive.

So this does a textual shift instead: every literal inside the image range is
"base + RVA", so adding one delta to all of them is equivalent to regenerating
at the new base.

Safety
------
Only hex literals whose value falls in [old_base, old_base + image_size) are
rewritten. That range belongs exclusively to the guest image (8.6 MB), so any
constant landing inside it must be an address within that image. Not a single
byte outside the range is touched.

Usage
-----
    python3 shift_base.py --from 0x30000000 --to 0x20000000 --apply
    python3 shift_base.py --from 0x30000000 --to 0x20000000           # count only
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys

IMAGE_SIZE = 0x0085F000          # matches GUEST_PE_EXPECTED_IMAGE_SIZE in guest_pe.h

# Files to process: the generated gen units, gen_extra, and the places in the
# host layer that hard-code image addresses (the resource-table probes in
# host_crt.c, xprobe in host_xinput.c, and others)
PATTERNS = [
    "gen/guest_*.c",
    "gen_extra/*.c",
    "host_*.c",
    "jni_bridge.c",
]

# Hex literal: 0x30a1b2c3 / 0x30A1B2C3, optionally followed by U/u
LIT = re.compile(r"0x([0-9A-Fa-f]{8})[Uu]?")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--to", dest="dst", required=True)
    ap.add_argument("--dir", default=".", help="build root directory")
    ap.add_argument("--apply", action="store_true", help="actually write back")
    a = ap.parse_args(argv)

    lo = int(a.src, 0)
    hi = lo + IMAGE_SIZE
    delta = int(a.dst, 0) - lo
    delta &= 0xFFFFFFFF

    print("image range: %08x .. %08x   delta = %+d (0x%08x)" %
          (lo, hi, int(a.dst, 0) - lo, delta))

    total = 0
    touched_files = []
    for pat in PATTERNS:
        for path in sorted(glob.glob(os.path.join(a.dir, pat))):
            try:
                txt = open(path, encoding="utf-8", errors="surrogateescape").read()
            except OSError:
                continue

            n = [0]

            def repl(m):
                v = int(m.group(1), 16)
                if not (lo <= v < hi):
                    return m.group(0)
                n[0] += 1
                newv = (v + delta) & 0xFFFFFFFF
                suffix = m.group(0)[10:]          # keep the U/u
                return "0x%08x%s" % (newv, suffix)

            out = LIT.sub(repl, txt)
            if n[0]:
                total += n[0]
                touched_files.append((path, n[0]))
                if a.apply:
                    tmp = path + ".shifting"
                    with open(tmp, "w", encoding="utf-8",
                              errors="surrogateescape") as f:
                        f.write(out)
                    os.replace(tmp, path)

    for p, c in touched_files:
        print("  %-40s %d" % (os.path.relpath(p, a.dir), c))
    print("total %d sites across %d files%s" %
          (total, len(touched_files), " (written back)" if a.apply else " (not written)"))

    if a.apply and total:
        print("\nremember to sync these constants:")
        print("  guest.h      GUEST_IMAGE_BASE          = 0x%08x" % int(a.dst, 0))
        print("  guest_pe.h   (no change needed: that is only the expected image size)")
        print("  check that the fixed addresses for heap/stack/DInput/USER32 "
              "do not overlap the new base")
    return 0


if __name__ == "__main__":
    sys.exit(main())
