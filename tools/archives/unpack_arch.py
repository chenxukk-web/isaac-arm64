#!/usr/bin/env python3
"""unpack_arch.py — 把 ARCH000 归档解压成散文件树

格式来源：上游 docs/arch000-format.md + src/data/codec.cpp（tboi-recomp）。
三种 codec：
  0x00  Bogocrypt1（XOR + 条件交换 + 密钥调度），对称
  0x01  LZW（字典跨 chunk 保留，宽度先增后读）
  0x02  chunked：4 字节长度前缀 + raw DEFLATE(wbits=-15)；长度恰为 0x400 时直存

名字：条目 +0x00 是 djb2-add(5381) 的 32 位路径哈希；+0x04 是每条目 cipher seed。
官方 ResourceExtractor/filelist.txt 提供路径语料。

用法：
  unpack_arch.py <archive.a> <outdir> [--filelist PATH] [--verify]
"""
import argparse
import os
import struct
import sys
import zlib

HDR = 14
ENTRY = 20
K_STORED_CHUNK = 0x400
K_MAX_CHUNK = 0x7FF


# ---------------------------------------------------------------- 名字
def resource_name_hash(path: str) -> int:
    h = 5381
    for b in path.lower().encode("latin-1", "replace"):
        h = (h * 33 + b) & 0xFFFFFFFF
    return h


def load_names(path: str) -> dict:
    table = {}
    with open(path, encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if line:
                table[resource_name_hash(line)] = line
    return table


# ---------------------------------------------------------------- codec 0x00
def bogocrypt1(buf: bytearray, offset: int, count: int, key: int) -> int:
    """原地加/解密。对称：同样的调用即逆运算。"""
    BLOCK = 1024
    for start in range(0, count, BLOCK):
        n = min(BLOCK, count - start)
        block = bytearray(BLOCK)
        block[:n] = buf[offset + start: offset + start + n]

        for word in range(BLOCK // 4):
            i = word * 4
            block[i + 0] ^= (key >> 0) & 0xFF
            block[i + 1] ^= (key >> 8) & 0xFF
            block[i + 2] ^= (key >> 16) & 0xFF
            block[i + 3] ^= (key >> 24) & 0xFF

            low = key & 15
            if low == 2:
                block[i], block[i + 3] = block[i + 3], block[i]
                block[i + 1], block[i + 2] = block[i + 2], block[i + 1]
            elif low == 9:
                block[i], block[i + 1] = block[i + 1], block[i]
                block[i + 2], block[i + 3] = block[i + 3], block[i + 2]
            elif low == 13:
                block[i], block[i + 2] = block[i + 2], block[i]
                block[i + 1], block[i + 3] = block[i + 3], block[i + 1]

            t = (key ^ ((key << 8) & 0xFFFFFFFF)) & 0xFFFFFFFF
            key = (key ^ ((t >> 9) & 0xFFFFFFFF) ^ ((key << 8) & 0xFFFFFFFF)
                   ^ ((((t >> 9) ^ key ^ ((key << 8) & 0xFFFFFFFF)) << 23) & 0xFFFFFFFF)) & 0xFFFFFFFF

        buf[offset + start: offset + start + n] = block[:n]
    return key


# ---------------------------------------------------------------- codec 0x01
class Lzw:
    SLOTS = 0x1000

    def __init__(self):
        self.prefix = [-1] * self.SLOTS
        self.chain = [-1] * self.SLOTS
        self.nxt = [-1] * self.SLOTS
        self.first = [i & 0xFF for i in range(self.SLOTS)]
        self.reset()

    def reset(self):
        for i in range(self.SLOTS):
            self.chain[i] = -1
        self.next_code = 0x100
        self.width = 8

    def emit(self, code: int, out: bytearray) -> int:
        scratch = bytearray(self.SLOTS)
        count = 0
        while code != -1 and count < self.SLOTS:
            scratch[count] = self.first[code]
            count += 1
            code = self.prefix[code]
        for i in range(count - 1, -1, -1):
            out.append(scratch[i])
        return scratch[count - 1] if count else 0

    def insert(self, prefix: int, first_char: int):
        if prefix < 0 or prefix >= self.SLOTS or self.next_code > 0xFFF:
            return
        slot = self.next_code
        self.prefix[slot] = prefix
        self.chain[slot] = -1
        self.first[slot] = first_char
        self.nxt[slot] = self.chain[prefix]
        self.chain[prefix] = slot
        self.next_code += 1


class LzwBits:
    """0xa85090：字节累加器，MSB 优先。"""
    __slots__ = ("data", "size", "pos", "acc", "bits")

    def __init__(self, data, size):
        self.data = data
        self.size = size
        self.pos = 0
        self.acc = 0
        self.bits = 0

    def read(self, count):
        while self.bits < count:
            if self.pos >= self.size:
                return -1
            self.acc = ((self.acc << 8) | self.data[self.pos]) & 0xFFFFFFFFFFFF
            self.pos += 1
            self.bits += 8
        self.bits -= count
        return (self.acc >> self.bits) & ((1 << count) - 1)


def decode_lzw_chunk(state: Lzw, chunk: bytes, size: int, out: bytearray):
    """0xa85320 的逐 chunk 循环。字典跨 chunk 保留，只有位读取器是新的。"""
    r = LzwBits(chunk, size)

    code = r.read(state.width)
    if code < 0 or code >= Lzw.SLOTS:
        return
    first_char = state.emit(code, out)
    prev = code

    while True:
        if state.next_code + 1 >= Lzw.SLOTS:
            state.reset()
            code = r.read(8)
            if code < 0:
                break
            first_char = state.emit(code, out)
            prev = code

        while (1 << state.width) <= state.next_code:
            state.width += 1

        code = r.read(state.width)
        if code < 0 or code > state.next_code:
            break

        if code == state.next_code:
            state.insert(prev, first_char)
            first_char = state.emit(code, out)
        else:
            first_char = state.emit(code, out)
            state.insert(prev, first_char)
        prev = code


def decode_lzw(data: bytes, span: int, expected: int) -> bytearray:
    """entry 由若干 (4 字节长度 + 压缩字节) 组成；字典跨 chunk 保留。"""
    out = bytearray()
    if span < 4:
        return out
    state = Lzw()
    cursor = 0
    while cursor + 4 <= span:
        length = struct.unpack_from("<i", data, cursor)[0]
        cursor += 4
        if length <= 0 or cursor + length > span:
            break
        decode_lzw_chunk(state, data[cursor:cursor + length], length, out)
        cursor += length
    return out


# ---------------------------------------------------------------- codec 0x02
def decode_chunked(data: bytes, span: int, expected: int) -> bytearray:
    out = bytearray()
    at = 0
    while len(out) < expected and at + 4 <= span:
        raw = struct.unpack_from("<I", data, at)[0]
        at += 4
        length = raw & 0x7FFFFFFF
        if length > K_MAX_CHUNK:
            raise ValueError(f"chunk length 0x{length:x} > 0x{K_MAX_CHUNK:x}")
        if span - at < length:
            raise ValueError("chunk runs past span")

        if length == K_STORED_CHUNK:
            out.extend(data[at: at + length])
            at += length
            continue

        d = zlib.decompressobj(-15)
        try:
            out.extend(d.decompress(data[at: at + length]))
        except zlib.error as e:
            raise ValueError(f"deflate failed: {e}")
        at += length

    return out


# ---------------------------------------------------------------- 归档
class Entry:
    __slots__ = ("name_hash", "seed", "offset", "size", "crc", "span")


def parse(path: str):
    with open(path, "rb") as fh:
        blob = fh.read()
    if blob[:7] != b"ARCH000":
        raise ValueError("not an ARCH000 container")
    mode = blob[7]
    index_off = struct.unpack_from("<I", blob, 8)[0]
    count = struct.unpack_from("<H", blob, 12)[0]
    if index_off + count * ENTRY != len(blob):
        raise ValueError(
            f"index {index_off} + {count}*20 != size {len(blob)}")

    entries = []
    for i in range(count):
        at = index_off + i * ENTRY
        e = Entry()
        e.name_hash, e.seed, e.offset, e.size, e.crc = \
            struct.unpack_from("<IIIII", blob, at)
        entries.append(e)

    # span = 下一个（按 offset 排序）的 offset - 当前 offset；末条到 index_off
    by_off = sorted(entries, key=lambda x: x.offset)
    for i, e in enumerate(by_off):
        nxt = by_off[i + 1].offset if i + 1 < len(by_off) else index_off
        e.span = nxt - e.offset
    return mode, blob, entries, index_off


def unpack_one(mode, blob, e, names):
    payload = blob[e.offset: e.offset + e.span]
    if mode == 0x00:
        buf = bytearray(payload)
        key = ((e.seed ^ 0xF9524287) | 1) & 0xFFFFFFFF
        bogocrypt1(buf, 0, e.span, key)
        return bytes(buf[:e.size])
    if mode == 0x01:
        return bytes(decode_lzw(payload, e.span, e.size))
    if mode == 0x02:
        return bytes(decode_chunked(payload, e.span, e.size))
    raise ValueError(f"unknown mode 0x{mode:02x}")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("outdir")
    ap.add_argument("--filelist")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args(argv)

    names = load_names(args.filelist) if args.filelist else {}
    mode, blob, entries, _ = parse(args.archive)
    base = os.path.basename(args.archive)

    ok = bad = unnamed = 0
    errors = []
    for e in entries:
        name = names.get(e.name_hash)
        if not name:
            unnamed += 1
            name = f"_unnamed/{e.name_hash:08x}.bin"
        try:
            data = unpack_one(mode, blob, e, names)
            if len(data) != e.size:
                raise ValueError(f"size {len(data)} != recorded {e.size}")
            out = os.path.join(args.outdir, name)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as fh:
                fh.write(data)
            ok += 1
        except Exception as exc:
            bad += 1
            errors.append((e.name_hash, str(exc)))

    print(f"{base:22} mode=0x{mode:02x}  entries={len(entries):5}  "
          f"ok={ok:5}  bad={bad:4}  unnamed={unnamed:5}")
    for h, msg in errors[:5]:
        print(f"    {h:08x}: {msg}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
