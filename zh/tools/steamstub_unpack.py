#!/usr/bin/env python3
"""SteamStub DRM Variant 3.1 脱壳器 —— 纯 Python 实现

对 The Binding of Isaac: Repentance v1.7.9b (J835) 的 isaac-ng.exe 实测通过，
输出与 Steamless 逐字节一致：

    sha256 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404
    大小   8,650,240 字节

算法照抄 Steamless（atom0s/Steamless，CC BY-NC-ND 4.0）的
Steamless.Unpacker.Variant31.x86，没有直接使用其代码，是按公开文档和
结构定义重新实现的。

用法:
    python3 steamstub_unpack.py <加壳的.exe> [-o 输出路径]

原理:
    1. 从 EntryPoint 往前 0xF0 字节处读 SteamStub 头部
    2. SteamXor 解码（首 4 字节是 key，之后逐 4 字节 val^key，key=val）
    3. 校验 Signature == 0xC0DEC0DF
    4. 读到 OriginalEntryPoint
    5. 移除 .bind 段：节数-1、入口点换成 OEP、重算 SizeOfImage
    6. 清零 DOS stub、清零节表后的空洞（对应 Steamless 的
       ZeroDosStubData=true 和 FileStream 跳写产生的空洞）
    7. 截断文件到 .bind 起点

    ★ 默认不对齐各节的 VirtualSize（Steamless 的 DontRealignSections=true）。
      这一点很反直觉，但实测必须如此才能逐字节匹配。

限制:
    只处理 Flags 含 NoEncryption(0x4) 的简体壳。如果代码段真被加密，
    需要补 AES 解密（结构里已有 AES_Key / AES_IV / CodeSectionStolenData 字段）。
"""
import argparse
import hashlib
import struct
import sys

SIGNATURE = 0xC0DEC0DF
HEADER_SIZE = 0xF0
DRM_FLAG_NO_ENCRYPTION = 0x04


class PE:
    def __init__(self, data: bytes):
        self.data = data
        self.e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
        self.opt = self.e_lfanew + 24
        n = struct.unpack_from('<H', data, self.e_lfanew + 6)[0]
        self.nsec = n
        opt_size = struct.unpack_from('<H', data, self.e_lfanew + 20)[0]
        self.secoff = self.opt + opt_size
        self.entry = struct.unpack_from('<I', data, self.opt + 16)[0]
        self.section_align = struct.unpack_from('<I', data, self.opt + 32)[0]
        self.file_align = struct.unpack_from('<I', data, self.opt + 36)[0]
        self.size_of_image = struct.unpack_from('<I', data, self.opt + 56)[0]
        self.size_of_headers = struct.unpack_from('<I', data, self.opt + 60)[0]
        self.sections = []
        for i in range(n):
            o = self.secoff + i * 40
            name = data[o:o + 8].rstrip(b'\x00').decode('latin1')
            vs, va, rs, ro = struct.unpack_from('<IIII', data, o + 8)
            self.sections.append(dict(name=name, vsize=vs, vaddr=va,
                                      rawsize=rs, rawoff=ro, index=i,
                                      hdr_off=o))

    def rva_to_off(self, rva: int):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rawsize']):
                return s['rawoff'] + (rva - s['vaddr'])
        return None

    def find(self, name: str):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None


def steam_xor(buf: bytearray, size: int, key: int = 0):
    """Steamless 的 SteamXor：首 4 字节是 key，之后逐 4 字节解码。"""
    off = 0
    if key == 0:
        off = 4
        key = struct.unpack_from('<I', buf, 0)[0]
    x = off
    while x < size:
        val = struct.unpack_from('<I', buf, x)[0]
        struct.pack_into('<I', buf, x, (val ^ key) & 0xFFFFFFFF)
        key = val
        x += 4
    return key


def parse_header(h: bytes) -> dict:
    """SteamStub32Var31Header —— 注意 uint64 字段的 8 字节对齐。"""
    return dict(
        xor_key=struct.unpack_from('<I', h, 0)[0],
        signature=struct.unpack_from('<I', h, 4)[0],
        image_base=struct.unpack_from('<Q', h, 8)[0],
        address_of_entry_point=struct.unpack_from('<Q', h, 16)[0],
        bind_section_offset=struct.unpack_from('<I', h, 24)[0],
        original_entry_point=struct.unpack_from('<Q', h, 32)[0],
        payload_size=struct.unpack_from('<I', h, 44)[0],
        drmp_dll_offset=struct.unpack_from('<I', h, 48)[0],
        drmp_dll_size=struct.unpack_from('<I', h, 52)[0],
        steam_app_id=struct.unpack_from('<I', h, 56)[0],
        flags=struct.unpack_from('<I', h, 60)[0],
        bind_section_virtual_size=struct.unpack_from('<I', h, 64)[0],
        code_section_va=struct.unpack_from('<Q', h, 72)[0],
        code_section_raw_size=struct.unpack_from('<Q', h, 80)[0],
        aes_key=h[88:120],
        aes_iv=h[120:136],
        stolen=h[136:152],
    )


def unpack(path: str, out_path: str = None) -> str:
    data = open(path, 'rb').read()
    pe = PE(data)

    # --- Step 1: 读并解码头部 ---
    ep_off = pe.rva_to_off(pe.entry)
    if ep_off is None or ep_off < HEADER_SIZE:
        raise SystemExit('入口点不在任何节里，或太靠前')
    hdr = bytearray(data[ep_off - HEADER_SIZE:ep_off])
    steam_xor(hdr, HEADER_SIZE)
    h = parse_header(bytes(hdr))
    if h['signature'] != SIGNATURE:
        raise SystemExit(
            'SteamStub 签名不符（得到 %#x，期望 %#x）。'
            '可能不是 Variant 3.1，或已被脱壳。' % (h['signature'], SIGNATURE))
    print(f'SteamStub 3.1 头部校验通过  (XorKey={h["xor_key"]:#010x})')
    print(f'  OriginalEntryPoint = {h["original_entry_point"]:#x}')
    print(f'  SteamAppId         = {h["steam_app_id"]} ({h["steam_app_id"]:#x})')
    print(f'  Flags              = {h["flags"]:#x}')

    if not (h['flags'] & DRM_FLAG_NO_ENCRYPTION):
        raise SystemExit(
            '代码段被加密（Flags 无 NoEncryption 位），本脚本只处理不加密的壳。\n'
            '需要补 AES-CBC 解密：Key=%s IV=%s Stolen=%s'
            % (h['aes_key'].hex(), h['aes_iv'].hex(), h['stolen'].hex()))

    # --- Step 4: 移除 .bind ---
    bind = pe.find('.bind')
    if bind is None:
        raise SystemExit('找不到 .bind 段')
    print(f'  .bind: va={bind["vaddr"]:#x} rawoff={bind["rawoff"]:#x} '
          f'rawsize={bind["rawsize"]:#x}')

    b = bytearray(data[:bind['rawoff']])
    struct.pack_into('<H', b, pe.e_lfanew + 6, pe.nsec - 1)          # 节数
    struct.pack_into('<I', b, pe.opt + 16, h['original_entry_point'])  # OEP
    struct.pack_into('<I', b, pe.opt + 64, 0)                        # CheckSum

    # SizeOfImage = align(最后节.VA + 最后节.VSize)
    last = pe.sections[-2]                                           # 去掉 .bind
    a = pe.section_align
    new_img = (last['vaddr'] + last['vsize'] + a - 1) & ~(a - 1)
    struct.pack_into('<I', b, pe.opt + 56, new_img)
    print(f'  SizeOfImage: {pe.size_of_image:#x} -> {new_img:#x}')

    # --- Step 6: 清零（对应 Steamless 的 ZeroDosStubData + FileStream 空洞）---
    e_lfanew = struct.unpack_from('<I', b, 0x3C)[0]
    b[0x40:e_lfanew] = b'\x00' * (e_lfanew - 0x40)
    sec_end = pe.secoff + (pe.nsec - 1) * 40
    if sec_end < pe.size_of_headers:
        b[sec_end:pe.size_of_headers] = b'\x00' * (pe.size_of_headers - sec_end)
        print(f'  清零 DOS stub 0x40..{e_lfanew:#x}，'
              f'节表空洞 {sec_end:#x}..{pe.size_of_headers:#x}')

    out_path = out_path or (path + '.unpacked.exe')
    open(out_path, 'wb').write(bytes(b))
    digest = hashlib.sha256(bytes(b)).hexdigest()
    print(f'\n已写出 {out_path}')
    print(f'  size   = {len(b):,}')
    print(f'  sha256 = {digest}')

    # 已知版本的对照（1.7.9b J835）
    known = '31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404'
    if digest == known:
        print('  ✓ 与 repentogxm 期望的 1.7.9b (J835) 完全一致')
    return out_path


def main():
    ap = argparse.ArgumentParser(description='SteamStub DRM Variant 3.1 脱壳器')
    ap.add_argument('input', help='加壳的 PE 文件')
    ap.add_argument('-o', '--output', help='输出路径（默认 <输入>.unpacked.exe）')
    args = ap.parse_args()
    try:
        unpack(args.input, args.output)
    except SystemExit:
        raise
    except Exception as e:
        print(f'失败: {e}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
