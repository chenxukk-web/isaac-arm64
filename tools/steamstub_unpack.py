#!/usr/bin/env python3
"""SteamStub DRM Variant 3.1 unpacker -- pure Python implementation

Verified against isaac-ng.exe from The Binding of Isaac: Repentance
v1.7.9b (J835). Output is byte-identical to Steamless:

    sha256 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404
    size   8,650,240 bytes

The algorithm follows Steamless (atom0s/Steamless, CC BY-NC-ND 4.0) module
Steamless.Unpacker.Variant31.x86. No code was copied: this is an independent
implementation from the public documentation and structure definitions.

Usage:
    python3 steamstub_unpack.py <packed.exe> [-o output_path]

How it works:
    1. Read the SteamStub header at EntryPoint - 0xF0
    2. SteamXor decode (first 4 bytes are the key, then val^key per 4 bytes,
       key=val)
    3. Verify Signature == 0xC0DEC0DF
    4. Read OriginalEntryPoint
    5. Remove the .bind section: section count -1, entry point = OEP,
       recompute SizeOfImage
    6. Zero the DOS stub and the gap after the section table (matching
       Steamless's ZeroDosStubData=true and the hole left by FileStream's
       seek-and-write)
    7. Truncate the file at the .bind start

    * Section VirtualSize is deliberately NOT realigned (Steamless's
      DontRealignSections=true). That is counter-intuitive, but it is
      required for a byte-identical match.

Limitation:
    Handles only the variant whose Flags include NoEncryption(0x4). If the
    code section is genuinely encrypted you must add AES decryption (the
    structure already carries AES_Key / AES_IV / CodeSectionStolenData).
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
    """Steamless's SteamXor: the first 4 bytes are the key, then decode per 4 bytes."""
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
    """SteamStub32Var31Header -- note the 8-byte alignment of uint64 fields."""
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

    # --- Step 1: read and decode the header ---
    ep_off = pe.rva_to_off(pe.entry)
    if ep_off is None or ep_off < HEADER_SIZE:
        raise SystemExit('entry point is in no section, or too far forward')
    hdr = bytearray(data[ep_off - HEADER_SIZE:ep_off])
    steam_xor(hdr, HEADER_SIZE)
    h = parse_header(bytes(hdr))
    if h['signature'] != SIGNATURE:
        raise SystemExit(
            'SteamStub signature mismatch (got %#x, expected %#x). '
            'Not Variant 3.1, or already unpacked.' % (h['signature'], SIGNATURE))
    print(f'SteamStub 3.1 header verified  (XorKey={h["xor_key"]:#010x})')
    print(f'  OriginalEntryPoint = {h["original_entry_point"]:#x}')
    print(f'  SteamAppId         = {h["steam_app_id"]} ({h["steam_app_id"]:#x})')
    print(f'  Flags              = {h["flags"]:#x}')

    if not (h['flags'] & DRM_FLAG_NO_ENCRYPTION):
        raise SystemExit(
            'code section is encrypted (Flags lacks the NoEncryption bit); '
            'this script handles unencrypted stubs only.\n'
            'AES-CBC decryption must be added: Key=%s IV=%s Stolen=%s'
            % (h['aes_key'].hex(), h['aes_iv'].hex(), h['stolen'].hex()))

    # --- Step 4: remove .bind ---
    bind = pe.find('.bind')
    if bind is None:
        raise SystemExit('.bind section not found')
    print(f'  .bind: va={bind["vaddr"]:#x} rawoff={bind["rawoff"]:#x} '
          f'rawsize={bind["rawsize"]:#x}')

    b = bytearray(data[:bind['rawoff']])
    struct.pack_into('<H', b, pe.e_lfanew + 6, pe.nsec - 1)          # section count
    struct.pack_into('<I', b, pe.opt + 16, h['original_entry_point'])  # OEP
    struct.pack_into('<I', b, pe.opt + 64, 0)                        # CheckSum

    # SizeOfImage = align(last section .VA + last section .VSize)
    last = pe.sections[-2]                                           # .bind removed
    a = pe.section_align
    new_img = (last['vaddr'] + last['vsize'] + a - 1) & ~(a - 1)
    struct.pack_into('<I', b, pe.opt + 56, new_img)
    print(f'  SizeOfImage: {pe.size_of_image:#x} -> {new_img:#x}')

    # --- Step 6: zeroing (matches Steamless ZeroDosStubData + FileStream hole) ---
    e_lfanew = struct.unpack_from('<I', b, 0x3C)[0]
    b[0x40:e_lfanew] = b'\x00' * (e_lfanew - 0x40)
    sec_end = pe.secoff + (pe.nsec - 1) * 40
    if sec_end < pe.size_of_headers:
        b[sec_end:pe.size_of_headers] = b'\x00' * (pe.size_of_headers - sec_end)
        print(f'  zeroed DOS stub 0x40..{e_lfanew:#x}, '
              f'section-table hole {sec_end:#x}..{pe.size_of_headers:#x}')

    out_path = out_path or (path + '.unpacked.exe')
    open(out_path, 'wb').write(bytes(b))
    digest = hashlib.sha256(bytes(b)).hexdigest()
    print(f'\nwrote {out_path}')
    print(f'  size   = {len(b):,}')
    print(f'  sha256 = {digest}')

    # Cross-check against the known release (1.7.9b J835)
    known = '31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404'
    if digest == known:
        print('  ✓ matches the 1.7.9b (J835) build repentogxm expects')
    return out_path


def main():
    ap = argparse.ArgumentParser(description='SteamStub DRM Variant 3.1 unpacker')
    ap.add_argument('input', help='packed PE file')
    ap.add_argument('-o', '--output', help='output path (default <input>.unpacked.exe)')
    args = ap.parse_args()
    try:
        unpack(args.input, args.output)
    except SystemExit:
        raise
    except Exception as e:
        print(f'failed: {e}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
