import struct, sys, os, zlib
sys.path.insert(0,'.')
from unpack_arch import parse, unpack_one, load_names, resource_name_hash

arc, want, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
import os
FL = os.environ.get("ISAEC_FILELIST",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "../../../recourse/(pc)tboi,rep/tools/ResourceExtractor/filelist.txt"))
names = load_names(FL)
mode, blob, entries, idx = parse(arc)
target = resource_name_hash(want)
for e in entries:
    if e.name_hash == target:
        data = unpack_one(mode, blob, e, names)
        out = os.path.join(outdir, os.path.basename(want))
        os.makedirs(outdir, exist_ok=True)
        open(out,'wb').write(data)
        print(f"{os.path.basename(arc):18} mode=0x{mode:02x} {want}")
        print(f"  size={len(data)}  head={data[:8].hex()}  -> {out}")
        sys.exit(0)
print(f"  {os.path.basename(arc)}: not found")
