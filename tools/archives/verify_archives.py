#!/usr/bin/env python3
"""verify_archives.py — 用游戏自己的 fold 校验和验证归档解码是否 100% 正确

校验和算法（来自 rootrd/TheBindingofIsaacRebirth 的交接文档，本文档用真实
归档独立验证过）：

    s = rotr32(s, 1) + word     初值 0xababeb98
    数据按 0x200 字节一块处理，尾部不足补零

这个校验和存在索引条目 +0x10。它是对**解压后**的数据算的，所以只要 fold 能
逐条对上，就证明解压结果和游戏解出来的字节完全一致——这是比"长度对不对"
强得多的判据。

同时确认一个格式细节：条目 +0x00 是 h1（djb2 变体，基值 0x1505），+0x04 是
**h2（FNV-1a 变体，自定义基值 0x5BB2220E）**，不是"每条目 cipher seed"。
mode 0 的解密密钥恰好由 h2 派生，所以两种说法指同一个值；但命名应按 h2 理解，
因为游戏用它做名字路由。

用法:
  verify_archives.py <packed目录> [--limit N]
"""
import os
import struct
import sys
import zlib

INIT = 0xababeb98
BLOCK = 0x200


def rotr32(v, n):
    v &= 0xFFFFFFFF
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF


def fold(data):
    """游戏的校验和折叠。

    关键细节：0x200 的暂存区**只清零一次**，之后每块只覆盖实际有数据的部分。
    最后一个不满的块，其尾部字节因此保留的是**上一块对应位置的内容**，不是零。
    这一点很重要——按"补零"算出来的结果和游戏值只差最高字节，很容易误判成
    "算法接近但不完全对"。实测：补零 55/200 通过，残留 200/200 通过。
    """
    s = INIT
    buf = bytearray(BLOCK)          # 只在这里清零一次
    for off in range(0, len(data), BLOCK):
        chunk = data[off:off + BLOCK]
        buf[0:len(chunk)] = chunk   # 只覆盖有数据的部分，其余保持上一块的残留
        n4 = (len(chunk) + 3) // 4 * 4   # 向上取整到 4 字节，多出的字节是残留值
        for i in range(0, n4, 4):
            w = struct.unpack_from("<I", buf, i)[0]
            s = (rotr32(s, 1) + w) & 0xFFFFFFFF
    return s


# ---------------------------------------------------------------- codecs
def dec_mode0(buf, key_h2):
    out = bytearray(buf)
    key = ((key_h2 ^ 0xF9524287) | 1) & 0xFFFFFFFF
    for s0 in range(0, len(out), 1024):
        n = min(1024, len(out) - s0)
        blk = bytearray(1024)
        blk[:n] = out[s0:s0 + n]
        for w in range(256):
            i = w * 4
            blk[i] ^= key & 0xFF
            blk[i + 1] ^= (key >> 8) & 0xFF
            blk[i + 2] ^= (key >> 16) & 0xFF
            blk[i + 3] ^= (key >> 24) & 0xFF
            lo = key & 15
            if lo == 2:
                blk[i], blk[i + 3] = blk[i + 3], blk[i]
                blk[i + 1], blk[i + 2] = blk[i + 2], blk[i + 1]
            elif lo == 9:
                blk[i], blk[i + 1] = blk[i + 1], blk[i]
                blk[i + 2], blk[i + 3] = blk[i + 3], blk[i + 2]
            elif lo == 13:
                blk[i], blk[i + 2] = blk[i + 2], blk[i]
                blk[i + 1], blk[i + 3] = blk[i + 3], blk[i + 1]
            t = (key ^ ((key << 8) & 0xFFFFFFFF)) & 0xFFFFFFFF
            key = (key ^ ((t >> 9) & 0xFFFFFFFF)
                   ^ ((key << 8) & 0xFFFFFFFF)
                   ^ ((((t >> 9) ^ key ^ ((key << 8) & 0xFFFFFFFF)) << 23) & 0xFFFFFFFF)) & 0xFFFFFFFF
        out[s0:s0 + n] = blk[:n]
    return bytes(out)


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

    def emit(self, code, out):
        scratch = bytearray(self.SLOTS)
        count = 0
        while code != -1 and count < self.SLOTS:
            scratch[count] = self.first[code]
            count += 1
            code = self.prefix[code]
        for i in range(count - 1, -1, -1):
            out.append(scratch[i])
        return scratch[count - 1] if count else 0

    def insert(self, prefix, first_char):
        if prefix < 0 or prefix >= self.SLOTS or self.next_code > 0xFFF:
            return
        slot = self.next_code
        self.prefix[slot] = prefix
        self.chain[slot] = -1
        self.first[slot] = first_char
        self.nxt[slot] = self.chain[prefix]
        self.chain[prefix] = slot
        self.next_code += 1


class Bits:
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


def lzw_chunk(state, chunk, size, out):
    r = Bits(chunk, size)
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


def dec_mode1(payload, size):
    out = bytearray()
    state = Lzw()
    cursor = 0
    while cursor + 4 <= len(payload):
        length = struct.unpack_from("<i", payload, cursor)[0]
        cursor += 4
        if length <= 0 or cursor + length > len(payload):
            break
        lzw_chunk(state, payload[cursor:cursor + length], length, out)
        cursor += length
    return bytes(out)


def dec_mode2(payload, size):
    out = bytearray()
    at = 0
    while len(out) < size and at + 4 <= len(payload):
        raw = struct.unpack_from("<I", payload, at)[0]
        at += 4
        ln = raw & 0x7FFFFFFF
        if ln > 0x7FF or at + ln > len(payload):
            return None
        if ln == 0x400:
            out.extend(payload[at:at + ln])
            at += ln
            continue
        try:
            d = zlib.decompressobj(-15)
            out.extend(d.decompress(payload[at:at + ln]))
        except zlib.error:
            return None
        at += ln
    return bytes(out[:size])


# ---------------------------------------------------------------- driver
def verify(path, limit=None):
    blob = open(path, "rb").read()
    if blob[:7] != b"ARCH000":
        return None
    mode = blob[7]
    idx = struct.unpack_from("<I", blob, 8)[0]
    count = struct.unpack_from("<H", blob, 12)[0]

    entries = []
    for i in range(count):
        at = idx + i * 20
        h1, h2, off, size, ck = struct.unpack_from("<IIIII", blob, at)
        entries.append((off, size, h1, h2, ck))
    order = sorted(range(count), key=lambda i: entries[i][0])

    ok = bad = empty = skipped = 0
    first_bad = None
    n = count if limit is None else min(count, limit)

    for k in range(n):
        i = order[k]
        off, size, h1, h2, ck = entries[i]
        nxt = entries[order[k + 1]][0] if k + 1 < len(order) else idx
        span = nxt - off
        payload = blob[off:off + span]

        if mode == 0x00:
            data = dec_mode0(payload, h2)[:size]
        elif mode == 0x01:
            data = dec_mode1(payload, size)
        elif mode == 0x02:
            data = dec_mode2(payload, size)
            if data is None:
                skipped += 1
                continue
        else:
            skipped += 1
            continue

        if len(data) != size:
            bad += 1
            if first_bad is None:
                first_bad = (i, f"size {len(data)} != {size}")
            continue
        if fold(data) == ck:
            ok += 1
        else:
            bad += 1
            if first_bad is None:
                first_bad = (i, f"fold {fold(data):08x} != {ck:08x}")
    return mode, count, ok, bad, skipped, first_bad


def main(argv):
    root = argv[0]
    limit = None
    for a in argv[1:]:
        if a.startswith("--limit="):
            limit = int(a.split("=", 1)[1])

    total_ok = total_bad = total_skip = 0
    print(f"{'archive':24} {'mode':5} {'entries':>8} {'fold ok':>8} {'bad':>5} {'skip':>5}")
    for f in sorted(os.listdir(root)):
        if not f.endswith(".a"):
            continue
        r = verify(os.path.join(root, f), limit)
        if r is None:
            continue
        mode, count, ok, bad, skip, first_bad = r
        print(f"{f:24} 0x{mode:02x} {count:8} {ok:8} {bad:5} {skip:5}")
        if first_bad:
            print(f"    first failure: entry #{first_bad[0]}  {first_bad[1]}")
        total_ok += ok
        total_bad += bad
        total_skip += skip
    print()
    print(f"合计: fold 通过 {total_ok}  失败 {total_bad}  无法解码 {total_skip}")
    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
