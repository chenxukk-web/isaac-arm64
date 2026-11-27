#!/usr/bin/env python3
"""repack_stored.py — 把 ARCH000 里的 deflate 块转成直存块

为什么：一次启动里游戏要 inflate 187,035 个 deflate 块（输出 166 MB），
而这段代码是翻译过的 x86，比原生 zlib 慢约 7 倍。原生跑完全部归档只要
2.5 秒，我们实测启动 17.5 秒 —— 差额几乎全在 inflate 上。

关键观察（实测）：97.1% 的 deflate 块输出恰好 1024 字节，而 1024 字节
恰好等于「直存块」的固定长度。所以这些块可以原地换成直存形式：游戏读到
它时只做 memcpy，不再调 inflate。剩余 2.9% 输出不是 1024，保持 deflate。

不解密、不解压直存块、不动条目内容语义 —— 只改变「存储形式」。
未解的第三条臂（加密直存块）原样保留，行为与现在完全一致。

用法：
  repack_stored.py <in.a> <out.a> [--dry-run]
"""
import argparse
import struct
import sys
import zlib

HDR = 14
ENTRY = 20
STORED_LEN = 0x400


def build_chunks(blob, off, span, want, stats):
    """把一条目的 chunk 序列重写成尽量多直存块的形式。"""
    out = bytearray()
    at = 0
    produced = 0
    while produced < want and at + 4 <= span:
        raw = struct.unpack_from("<I", blob, off + at)[0]
        at += 4
        ln = raw & 0x7FFFFFFF
        if ln > 0x7FF or at + ln > span:
            stats["bad_frame"] += 1
            return None
        payload = blob[off + at: off + at + ln]
        at += ln

        if ln == STORED_LEN:
            # 已经是直存，原样搬运
            out += struct.pack("<I", raw)
            out += payload
            produced += ln
            stats["kept_stored"] += 1
            continue

        # deflate 块：尝试解开
        try:
            d = zlib.decompressobj(-15)
            plain = d.decompress(payload)
        except zlib.error:
            # 解不开 —— 原样保留（未解臂 / 损坏块），游戏会像现在一样处理
            out += struct.pack("<I", raw)
            out += payload
            stats["kept_deflate"] += 1
            stats["deflate_bytes_in"] += ln
            continue

        if len(plain) == STORED_LEN:
            # 输出恰好 1024 —— 换成直存块，游戏只做 memcpy
            out += struct.pack("<I", STORED_LEN)
            out += plain
            produced += STORED_LEN
            stats["converted"] += 1
            stats["deflate_bytes_in"] += ln
            stats["plain_bytes_out"] += STORED_LEN
        else:
            # 输出不是 1024（通常是条目末尾的残块），保留 deflate
            out += struct.pack("<I", raw)
            out += payload
            stats["kept_deflate"] += 1
            stats["deflate_bytes_in"] += ln
            produced += len(plain)      # 已知输出长度，便于循环推进
    return bytes(out)


def repack(path_in, path_out, dry_run):
    blob = open(path_in, "rb").read()
    if blob[:7] != b"ARCH000":
        raise SystemExit("not an ARCH000 container")
    mode = blob[7]
    if mode != 0x02:
        print(f"  mode=0x{mode:02x}，本工具只处理 0x02，跳过")
        return

    idx = struct.unpack_from("<I", blob, 8)[0]
    count = struct.unpack_from("<H", blob, 12)[0]

    entries = []
    for i in range(count):
        at = idx + i * ENTRY
        h, seed, off, size, crc = struct.unpack_from("<IIIII", blob, at)
        entries.append({"hash": h, "seed": seed, "off": off,
                        "size": size, "crc": crc})

    order = sorted(range(count), key=lambda i: entries[i]["off"])
    stats = dict(converted=0, kept_stored=0, kept_deflate=0,
                 bad_frame=0, deflate_bytes_in=0, plain_bytes_out=0)

    new_payload = bytearray()
    for k, i in enumerate(order):
        e = entries[i]
        nxt = entries[order[k + 1]]["off"] if k + 1 < len(order) else idx
        span = nxt - e["off"]
        chunks = build_chunks(blob, e["off"], span, e["size"], stats)
        if chunks is None:
            # 帧结构异常，整条原样保留
            chunks = blob[e["off"]: e["off"] + span]
        e["new_off"] = HDR + len(new_payload)
        new_payload += chunks

    new_index_off = HDR + len(new_payload)
    out = bytearray()
    out += b"ARCH000"
    out += bytes([mode])
    out += struct.pack("<I", new_index_off)
    out += struct.pack("<H", count)
    out += new_payload
    for e in entries:
        out += struct.pack("<IIIII", e["hash"], e["seed"],
                           e["new_off"], e["size"], e["crc"])

    grown = len(out) - len(blob)
    print(f"  {path_in.split('/')[-1]:22} {len(blob)/1048576:6.1f} MB -> {len(out)/1048576:6.1f} MB "
          f"({grown/1048576:+.1f})")
    print(f"    直存块转换 {stats['converted']:,}   保留 deflate {stats['kept_deflate']:,}   "
          f"原有直存 {stats['kept_stored']:,}   坏帧 {stats['bad_frame']}")
    if stats["deflate_bytes_in"]:
        print(f"    deflate 输入 {stats['deflate_bytes_in']/1048576:.1f} MB -> "
              f"直存 {stats['plain_bytes_out']/1048576:.1f} MB")

    if dry_run:
        print("    (dry-run，未写文件)")
        return
    with open(path_out, "wb") as fh:
        fh.write(out)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    repack(a.inp, a.out, a.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
