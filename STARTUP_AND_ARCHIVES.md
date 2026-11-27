# Startup time and the `ARCH000` archives

Measured 2026-11-27 on the same Adreno 830 / Android 17 device everything else
in this archive was measured on. Every number below came from a run, not from an
estimate; where something is inferred it says so.

**Summary:** startup is ~17.5 s of resource loading, and 97% of that is
`inflate()` executing as translated x86. Repacking the archives so the game never
has to inflate cuts it to 8 s — measured. That repack is blocked by a
pre-existing memory bug that the change in file sizes exposes. There is no
per-start cache: every start decompresses the same 1.2 GB again.

---

## 1. Where the 17.5 seconds go

`[boot]` marks on a normal start:

```
T+      0.0 ms  main entered
T+     89.0 ms  PE image loaded          (141 MB .so, 89 ms)
T+    109.6 ms  GL/host init complete
T+    325.7 ms  first frame rendered
T+ 17,804.4 ms  first XInput poll        <-- the game has finished loading
```

Other runs measured 16.4 s and 21.2 s for the same mark, so ~17-21 s is the
normal range. The 17.5 s between the first frame and the first poll is resource
loading.

Ruled out by measurement:

| candidate | evidence | verdict |
|---|---|---|
| Reading the files | `fopen` on the private dir (ext4) is **5.64 µs**; 22,522 archive opens ≈ 0.13 s | not the bottleneck |
| Reading the data | 1.2 GB read at 1.1 GB/s ≈ 1.1 s | not the bottleneck |
| PE image load | 89 ms for 141 MB | not the bottleneck |
| GL/host init | 11 ms | not the bottleneck |
| Import dispatch | adding a pointer-keyed cache changed nothing (still 16-22 s) | not the bottleneck |

What is left is CPU time inside `inflate()`, and `inflate()` here is **translated
x86**, not a native library: zlib is statically linked into `isaac-ng.exe`, so it
was lifted to C along with everything else. The import table contains no
`inflate`/`uncompress` entry (verified: zero matches in `gen/guest_table.c`).

Native zlib on the same data, measured with `bench_arch.c` in this archive:

| archive | stored chunks | deflate chunks | native time |
|---|---|---|---|
| `afterbirth.a` | 129,236 | 39,022 | 0.56 s |
| `afterbirthp.a` | 113,638 | 56,523 | 0.77 s |
| `repentance.a` | 350,691 | 59,999 | 0.96 s |
| 11 smaller archives | — | — | 0.16 s |
| **total** | **671,439** | **187,035** | **~2.5 s** |

Same bytes, same machine: **2.5 s native vs 17.5 s translated — a 7× factor.**
That factor is the cost of the translation layer, and it is the whole story of
startup time.

### 1.1 There is no cache

A start reads 1.2 GB and inflates 166 MB, and **nothing is persisted**. No file
under `packed/` is a cache, the host layer keeps no archive state between runs,
and `host_crt_file.c` explicitly disables its persistent `FILE*` cache. The same
work is repeated on every launch.

---

## 2. The `ARCH000` format

Recovered from `docs/arch000-format.md` and `src/data/codec.cpp` in
`caos-obliquo/tboi-recomp` (GPL-3.0, read as reference; no code copied), then
verified against the retail archives in this install.

```
0x00  8 bytes   "ARCH000" + one mode byte
0x08  u32       offset of the entry index
0x0C  u16       entry count
0x0E  ...       payload
index count * 20 bytes, at the end of the file
```

`index_offset + count * 20 == file_size` holds for every archive here.

Each index entry is 20 bytes:

| offset | field |
|---|---|
| +0x00 | `hash_lo` — 32-bit djb2-add(5381) of the lowercased resource path |
| +0x04 | `seed` — per-entry cipher seed (NOT the upper half of a 64-bit hash) |
| +0x08 | `offset` — strictly ascending across entries |
| +0x0C | `size` — **decompressed** length |
| +0x10 | `crc` |

Three codecs, selected by the mode byte:

| mode | archives | behaviour |
|---|---|---|
| `0x00` | graphics, music, sfx, videos | Bogocrypt1: XOR + conditional swaps + key schedule. Symmetric. |
| `0x01` | config, fonts, rooms, animations | LZW: dictionary persists across the 4-byte-prefixed chunks of one entry; width grows before each read. |
| `0x02` | afterbirth*, repentance*, all language packs | 4-byte length prefix + raw DEFLATE (`wbits=-15`); a chunk of length exactly `0x400` is **stored verbatim**. |

### 2.1 Name resolution

`hash_lo` is reversible against the shipped resource-path manifest, which is in
this archive at:

```
work/recourse/(pc)tboi,rep/tools/ResourceExtractor/filelist.txt   (18,328 paths)
```

Measured coverage against the 22,522 installed entries:

```
resolved 20,676 / 22,522 = 91.8%
```

Higher than the 81.46% reported upstream because this install's filelist is
newer. The unresolved remainder is concentrated in `afterbirth_jp.a`,
`afterbirth_kr.a`, `rooms.a` (3/13) and `animations.a` (0/1).

### 2.2 What the archives actually contain

`fonts.a` holds ten `.fnt` bitmap-font descriptors:

```
resources/font/pftempestasevencondensed.fnt
resources/font/teammeatfont10.fnt
resources/font/teammeatfont12.fnt
resources/font/teammeatfont16.fnt
resources/font/teammeatfont16bold.fnt
resources/font/teammeatfont20bold.fnt
resources/font/upheaval.fnt
resources/font/droid.fnt
resources/font/terminus.fnt
resources/font/terminus8.fnt
```

10 of 10 resolve and 10 of 10 decode. **The font PNGs are not in `fonts.a`** —
they live in `graphics.a`, `afterbirth.a` and `afterbirthp.a`
(`resources/font/teammeatfont16_0.png` is present in `graphics.a` and
`afterbirthp.a`).

**Note for the missing-font defect:** the `.fnt` decompressed out of `fonts.a`
is the **English original** (`Team Meat font`), while the loose copy under
`resources/font/` is a Chinese-localised variant (`Team Meat Font X HappyZCool`).
Both reference `TeamMeatFont16_0.png` in mixed case while the file on disk is
all-lowercase. This is a lead worth following and has **not** been tested.

---

## 3. Repacking so the game never inflates — measured, blocked

The game has a native "no decompression" path: a chunk of length exactly `0x400`
is copied verbatim. Verified in the translated code of the mode-`0x02` chunk
loop (`gen/guest_0172.c`, function containing RVA `0x59c341`):

```
0059c39e  mov al, byte ptr [edx + 0x10]   ; already-decoded flag
0059c3b9  cmp esi, 0x400                  ; is this chunk exactly 1024 bytes?
0059c3bf  jne 0x59c3c5                    ; no  -> clear the flag
0059c3c1  mov al, 1                       ; yes -> set it
0059c3cc  jne 0x59c40c                    ; flag set -> fast path
...
0059c412  call sub_005ec14c               ; fast path: copy straight through
```

And 97.1% of the deflate chunks inflate to **exactly 1024 bytes**:

```
inflation output size    count      share
1024 bytes              149,518     97.1%
anything else             4,461      2.9%
```

So a deflate chunk can be replaced, offline, by a stored chunk holding the same
1024 bytes. The game then does `memcpy` instead of `inflate`.

### 3.1 Byte-equivalence check

`repack_stored.py` in this archive converts every deflate chunk whose output is
exactly 1024 bytes into a stored chunk, leaves everything else untouched, and
rebuilds the index with new offsets. Verified by decoding both the original and
the repacked archive with the same reader and comparing every entry:

```
repentance.a      4,180 entries    identical 4,180    differing 0
afterbirth.a      2,647 entries    identical 2,647    differing 0
afterbirthp.a     5,512 entries    identical 5,512    differing 0
repentance_zh.a     943 entries    identical   943    differing 0
```

Content is unchanged. Only the storage form differs.

### 3.2 Result on device

| | startup mark | crash |
|---|---|---|
| original archives | **T+ 17,804 ms** | none |
| 14 archives repacked | **T+ 8,000 ms** | **SIGSEGV at startup** |

**9.8 seconds saved, matching the 2.5 s-native vs 17.5 s-translated gap.** The
approach works.

### 3.3 The crash it exposes

```
size         14 repacked archives: 719.1 MB -> 831.1 MB (+112 MB, +16%)
crash        [SEGV] sig=11 fault_addr=0x87e7fff0
where        DInput initialisation, i.e. long after the archives were read
heap note    "[heap] skipping occupied region 0x87e80000..0x9fe80000"
```

The fault address is `0x87e80000 - 16`: **sixteen bytes below a page boundary
the allocator itself reports as occupied.** That is an out-of-bounds write at the
end of an allocation, not a corrupt archive — the game had finished reading
resources and was setting up input devices.

The same failure mode appeared three times this session, each time after a change
that shifted the memory layout:

| change | outcome |
|---|---|
| original code + original archives | 600 frames, no crash |
| + diagnostic probes in `host_gl4es.c` | abort at frame 77 |
| + repacked archives (full) | SIGSEGV during startup |
| + repacked `repentance_zh.a` only (+3.5 MB) | no crash, 786 frames |

`[heap] released an object still in the resource table` appears **10 times in
every run**, including the ones that survive. That double-free is pre-existing
and unrelated to this work; it is the most likely parent of the out-of-bounds
write above.

### 3.4 A smaller repack was inconclusive

Repacking only `repentance_zh.a` (17.1 MB -> 20.5 MB, +3.5 MB) did **not** crash,
but it also could not answer the startup question: that archive is 1.7% of the
total, so even a perfect result is lost in noise. That run measured T+ 37,218 ms,
which is *worse* than baseline because replacing files forces the Java layer to
re-copy all 2.2 GB of resources to internal storage, evicting the page cache.

**The experiment design was wrong**, and is recorded here so it is not repeated:
with a full re-copy costing ~30 s per attempt, single-archive experiments cannot
measure startup. Only the full repack produces a signal large enough to see — and
that is the configuration that crashes.

---

## 4. Comparison with the official mobile builds

The official Android and iOS builds do **not** use this format at all, which is
why their startup is short and why their approach cannot be copied directly.

Measured on the shipped binaries:

| | PC (what this project translates) | iOS `isaac.arm64` | Android APK |
|---|---|---|---|
| `ARCH000` support | yes, 22 archives | **0 occurrences** | **0 `.a` files** |
| Resource form | packed archives | loose files via `CFBundle` | 586 loose PNGs in `assets/` |
| Lua engine | present | **0 `lua_*` symbols** | — |

The iOS build is a different engine: no archive layer, no Lua. Its 3-second
startup is not a faster version of this one — it does less work, because it reads
loose files that were never compressed into a container.

**This project translates the PC executable, so it inherits the PC data layer**,
archives included. Pre-decompression is the only way to get the same effect
without discarding the translation.

---

## 5. What is proven, and what is not

**Proven by measurement:**

- Startup baseline is 16.4-21.2 s across runs; 17.5 s sits in `inflate()`.
- Native zlib does the same work in 2.5 s; the translation layer costs 7×.
- 97.1% of deflate chunks inflate to exactly 1024 bytes; the game copies such
  chunks without inflating.
- Repacking those chunks preserves every entry byte-for-byte (4 archives,
  13,282 entries compared).
- A full repack cuts startup to 8 s **and** crashes during input setup.
- There is no cache between starts.

**Not proven / not done:**

- The crash is attributed to a pre-existing out-of-bounds write and a separate
  double-free; nobody has located the writing instruction.
- A repack has never completed a full session without crashing.
- The `.fnt`-language mismatch described in §2.2 has not been tested.
- 1,846 entries (8.2%) have no resolved name; `rooms.a` and `animations.a` decode
  incompletely (12/13 and 0/1).
- No attempt was made to reduce the +112 MB size growth.

**If someone picks this up:** fix the out-of-bounds write first. Everything else
is waiting on it, and the repack is a proven 10-second win once it lands.
