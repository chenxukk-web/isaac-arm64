#!/usr/bin/env python3
"""shift_base.py —— 把 guest 镜像基址整体平移

为什么要这个工具
----------------
Android app 进程里，ART 会占住低 4GB 的一段地址。我们的 guest 镜像
必须落在**编译期选定**的基址上（gen 代码里是硬编码常量），否则
    guest: PE load failed: cannot allocate fixed guest image

生成器 gen_all.py 的 `--image-base` 只支持 0x30000000 和 Vita 的
0x98000000（COVERAGE_CROSS_ROOT_CONTRACTS_BY_IMAGE_BASE 里写死了契约
哈希），加新基址要重算覆盖率契约，代价很高。

所以这里走文本平移：镜像范围内的所有字面量都是"基址 + RVA"，
整体加同一个 delta 就等价于按新基址重新生成。

安全性
------
只改数值落在 [old_base, old_base + image_size) 的十六进制字面量。
这个区间被 guest 镜像独占（8.6 MB），落在这里的常量必然是该镜像里的地址。
区间外的一个字节都不碰。

用法
----
    python3 shift_base.py --from 0x30000000 --to 0x20000000 --apply
    python3 shift_base.py --from 0x30000000 --to 0x20000000           # 只统计
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys

IMAGE_SIZE = 0x0085F000          # 与 guest_pe.h 的 GUEST_PE_EXPECTED_IMAGE_SIZE 一致

# 需要处理的文件：生成的 gen 单元、gen_extra、以及宿主层里硬编码了
# 镜像地址的地方（host_crt.c 的资源表探针、host_xinput.c 的 xprobe 等）
PATTERNS = [
    "gen/guest_*.c",
    "gen_extra/*.c",
    "host_*.c",
    "jni_bridge.c",
]

# 十六进制字面量：0x30a1b2c3 / 0x30A1B2C3，后面可以跟 U/u
LIT = re.compile(r"0x([0-9A-Fa-f]{8})[Uu]?")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--to", dest="dst", required=True)
    ap.add_argument("--dir", default=".", help="构建根目录")
    ap.add_argument("--apply", action="store_true", help="真的写回")
    a = ap.parse_args(argv)

    lo = int(a.src, 0)
    hi = lo + IMAGE_SIZE
    delta = int(a.dst, 0) - lo
    delta &= 0xFFFFFFFF

    print("镜像区间: %08x .. %08x   delta = %+d (0x%08x)" %
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
                suffix = m.group(0)[10:]          # 保留 U/u
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
    print("合计 %d 处，涉及 %d 个文件%s" %
          (total, len(touched_files), "（已写回）" if a.apply else "（未写回）"))

    if a.apply and total:
        print("\n别忘了同步这些常量：")
        print("  guest.h      GUEST_IMAGE_BASE          = 0x%08x" % int(a.dst, 0))
        print("  guest_pe.h   （不需要改：那只是期望镜像大小）")
        print("  检查 heap/stack/DInput/USER32 的固定地址是否与新基址重叠")
    return 0


if __name__ == "__main__":
    sys.exit(main())
