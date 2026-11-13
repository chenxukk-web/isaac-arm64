# Build Spec (for AI Agents) — 2026-11-12

> **The reader is an AI coding agent, not a human.** So there are no hard-coded
> paths and no ready-made scripts here — only **contracts, invariants, pass
> criteria, and traps**. You generate the scripts; you know the target machine's
> directory layout better than this document can.
>
> Target artifact: a `dlopen`-able `libisaac.so` (arm64), plus an optional APK.
> Intended user: builds on **x86_64 Linux**, installs onto an arm64 Android
> device.

---

> **When something breaks, check [`PITFALLS.md`](PITFALLS.md) first.** It lists
> every pitfall hit, ordered by cost, across build-time, ABI, allocators, Android
> platform and diagnostic methodology.

## 0. Read these five facts first

They determine how you should generate your scripts. **Fact 1 is the most
counter-intuitive part of the whole pipeline.**

| # | Fact | Consequence |
|---|---|---|
| 1 | `gen_all.py --image-base` **supports only 0x30000000 and Vita's 0x98000000** | You must generate at 0x30000000, then **textually shift** to 0x3C000000 |
| 2 | This project is **pure C**; the artifact does not depend on libc++ | Cross-compiling needs only libc; no `libc++_shared.so` |
| 3 | clang on Termux **is the NDK clang** (`Target: aarch64-unknown-linux-android24`) | Native output == cross-compiled output; reproducible equivalently |
| 4 | `gen/*.c` does `-I` into upstream `recomp/runtime` for `guest.h` | This project is a **GPL-2.0-or-later** derivative |
| 5 | **20 untranslated stubs remain** (all MMX/SSE instructions) | A "successful" build does not mean 0 stubs; do not use this as a failure criterion |

---

## 1. Platform mapping: native → cross-compile

The original build was completed on Termux (arm64 native), where `clang` emits
arm64 even without `--target`. On x86_64 you must specify it explicitly,
otherwise you produce x86 objects and only find out at link time.

### Equivalence

```
Termux native:  clang <flags> -c x.c        (implicit target = aarch64-...-android24)
x86 cross:      clang --target=aarch64-linux-android24 <flags> -c x.c
```

The `24` in `aarch64-linux-android24` is the **min API level**. The artifact is
tagged `for Android 24`. The APK's `minSdkVersion` is 26; the two do not
conflict (24 < 26 is forward-compatible).

### Toolchain you need

**Option A: Android NDK (recommended)**

```
NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
```

The NDK clang ships its own sysroot; no `--sysroot` needed. Reference versions
**r27 ~ r30** (the original artifact was produced by r30; r25+ should work, r30
is the verified one).

**Option B: system clang + your own sysroot**

Requires an explicit `--sysroot=<NDK>/toolchains/llvm/prebuilt/linux-x86_64/sysroot`,
and the API level must match the `24` in `--target`. **Not recommended**; many
sharp edges.

### One thing that will not appear when cross-compiling, but that you must handle

The original artifact carries a RUNPATH:

```
RUNPATH  /data/data/com.termux/files/usr/bin/../../usr/lib
```

Termux's clang adds that automatically (a leftover libc++ search path). It will
not appear with NDK clang. Still, **your verification stage should confirm the
artifact has no odd absolute paths**:

```bash
readelf -d libisaac.so | grep -i runpath     # expected: no output
```

If one appears, add `-Wl,--disable-new-dtags -Wl,-rpath,''` at link time, or
ignore it (Android does not read RUNPATH, but cleaner is better).

---

## 1.5 Quick reference (read this first, then the contracts below)

The nine steps, in order. Every step has a checkable artifact — **if the
artifact is wrong, do not continue.**

```bash
# ---- prerequisite: install and verify the NDK ----
export ANDROID_NDK_HOME=<your NDK path>
export TOOLBIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$(ls $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/ | head -1)/bin"
export CC="$TOOLBIN/clang"
"$CC" --version | grep -q android || echo "NDK clang looks wrong; fix this first"

# ---- 1. unpack ----
python3 tools/steamstub_unpack.py isaac-ng.exe -o work/isaac-ng.exe.unpacked
# check: 8,650,240 bytes + sha256 31846486...

# ---- 2. generate C (~13 min) ----
export REPENTOGXM_WORKDIR="$PWD/work"
export REPENTOGXM_PE="$PWD/work/isaac-ng.exe.unpacked"
cd <upstream repentogxm>/recomp
python3 gen_all.py --image-base 0x30000000 --outdir "$REPENTOGXM_WORKDIR/gen"
# check: 192 guest_*.c files, 4,669,714 lines total

# ---- 3. shift the image base ★ easiest to forget ----
cd <this package>
python3 tools/shift_base.py --from 0x30000000 --to 0x3C000000 --apply --dir "$PWD"
# check: ~320k changes / 210 files, including host_*.c and jni_bridge.c

# ---- 4. Lua (33 .o) ---- see S4.2
# ---- 5. gl4es (libGL.a, -DSTATICLIB=ON required) ---- see S4.1

# ---- 6. compile the 192 translation units ----
mkdir -p obj
for f in work/gen/guest_*.c; do
  "$CC" --target=aarch64-linux-android24 -O2 -fPIC \
     -DGUEST_TRACE_FUNCTIONS=1 -DGUEST_CHECKED_MEMORY=1 \
     -I <upstream>/recomp/runtime -c "$f" -o "obj/$(basename "$f" .c).o" || break
done
# check: 192/192 succeed

# ---- 7. compile the host layer ----
for f in source/host_*.c source/minimal_kage.c source/cpu_seam.c \
         source/missing_stubs.c source/gl4es_bridge_gen.c; do
  "$CC" --target=aarch64-linux-android24 -O2 -fPIC -Dfprintf=host_fprintf \
     -I <upstream>/recomp/runtime -c "$f" -o "obj/$(basename "$f" .c).o" || break
done
# compile jni_bridge.c separately, without the -Dfprintf macro

# ---- 8. link ----
"$CC" --target=aarch64-linux-android24 -shared obj/*.o luapic/*.o \
  -o libisaac.so \
  -Wl,-soname,libisaac.so -Wl,--version-script=source/libisaac.map \
  -lm -ldl -lpthread \
  <gl4es>/build/lib/libGL.a
# check: ~74 MB, exactly 8 exported symbols, NEEDED only libm/libdl/libc

# ---- 9. verify ----
readelf -sW --dyn-syms libisaac.so | awk '$5=="GLOBAL" && $7!="UND"' | wc -l   # 8
readelf -d libisaac.so | grep -c NEEDED                                        # 3
```

At runtime the `[boot]` section must print `✓ image loaded base=0x3c000000`.

**When stuck, suspect in this order**:
1. `PE load failed` → step 3 was skipped, or the shift missed `host_*.c`
2. Link reports `undefined symbol` → step 4/5 incomplete, or the `libGL.a` path is wrong
3. Far more than 8 exported symbols → the version script is not taking effect
4. Crash during startup → read the low-4GB occupancy printed by the `[probe]` section

---

## 2. Stage contracts

Each stage states **input / output / pass criteria**. All criteria are
machine-checkable.

### S1 Unpack the SteamStub

```
input   isaac-ng.exe (SteamStub Variant 3.1 packed)
tool    tools/steamstub_unpack.py (pure Python, no dependencies)
output  isaac-ng.exe.unpacked
criteria 8,650,240 bytes
         sha256 = 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404
```

**If the criteria do not match, stop.** If it is not v1.7.9b (J835), none of the
15,071 functions will line up. Upstream `gen_all.py` additionally verifies
`body_sha256` / `order_sha256` / `sites_sha256`, so it will reject a wrong PE
for you.

### S2 Generate the C code

```
input   isaac-ng.exe.unpacked
tool    upstream repentogxm recomp/gen_all.py
        (env-driven: REPENTOGXM_WORKDIR / REPENTOGXM_PE)
output  <workdir>/gen/guest_0000.c … guest_0191.c + guest_table.c
criteria 192 files, 4,669,714 lines of C total
         ~13 minutes on 8 cores
```

Use `--image-base 0x30000000`. **Do not try to pass 0x3C000000** — see fact 1.

### S3 Shift the image base ★

```
input   gen/, gen_extra/, host_*.c, jni_bridge.c from S2
tool    shift_base.py --from 0x30000000 --to 0x3C000000 --apply --dir <build>
criteria ~320,000 changes across 210 files
         no in-image address of the form 0x3[0-9a-f]XXXXXX remains
```

**Why it is mandatory**: in an Android app process, ART occupies part of the low
4 GiB, so the guest image must land on the base that is hard-coded at build
time. Otherwise at runtime you get
`PE load failed: cannot allocate fixed guest image`.

**Why you cannot just change the generator**: a new base requires recomputing the
coverage-contract hashes in `COVERAGE_CROSS_ROOT_CONTRACTS_BY_IMAGE_BASE`, which
is expensive.

**Why `host_*.c` must be included**: the host layer also hard-codes image
addresses (resource-table probes, xinput's xprobe, and others). Shifting only
`gen/` leaves inconsistencies behind.

Shift safety: only hex literals whose value falls inside
`[0x30000000, 0x30000000+0x0085F000)` are rewritten. Not a single byte outside
that range is touched — those 8.6 MB belong exclusively to the guest image.

### S4 Dependencies (three, all arm64)

| Library | License | Requirement |
|---|---|---|
| **gl4es 1.1.7** | MIT | Static library `libGL.a`; **`-DSTATICLIB=ON` is mandatory** |
| **Lua 5.3.3** | MIT | Compile file-by-file into `luapic/*.o`, **excluding `lua.c` / `luac.c`**. Should be **33 .o files** |
| **OpenAL Soft 1.23.1** | **LGPL-2.1** | Needed only for audio. ⚠ Distribution requires dynamic linking + full LGPL text + source availability |

#### S4.0 Install the NDK (the step most likely to block you)

Pick one:

```bash
# Option A: Android Studio already installed -- SDK Manager -> SDK Tools ->
#           check "NDK (Side by side)". Lands in $ANDROID_SDK_ROOT/ndk/<version>/

# Option B: command line (needs cmdline-tools first)
sdkmanager --install "ndk;27.0.12077973"

# Option C: official zip from developer.android.com/ndk/downloads
unzip android-ndk-r27*-linux.zip -d ~/
```

**Verify it** (do not skip):

```bash
export ANDROID_NDK_HOME=~/android-ndk-r27   # your actual path
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version
# expect: clang version 18.x or 19.x, Target: aarch64-unknown-linux-android
```

`prebuilt/linux-x86_64` is the Linux path. If that directory name differs
(it is `darwin-x86_64` on macOS), find the real one:

```bash
ls "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/"
```

Use that absolute path for every `clang` below, or export it once:

```bash
export CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/<yours>/bin/clang"
```

> Version note: the original artifact was built with **r30**. r27+ should work.
> Below r25 the NDK clang may lack some C11 support; not recommended.

#### S4.1 gl4es (produces libGL.a)

**`-DSTATICLIB=ON` is required.** Without it CMake takes the
`add_library(GL SHARED)` branch, produces a `.so` instead of a `.a`, and the
final link fails.

```bash
git clone https://github.com/ptitSeb/gl4es.git
cd gl4es && git checkout v1.1.7

cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DANDROID=ON \
  -DUSE_ANDROID_LOG=ON \
  -DSTATICLIB=ON \
  -DNOX11=ON \
  -DNO_GBM=ON

cmake --build build -j"$(nproc)"
```

`-DANDROID=ON` already adds `-DNOX11 -DNO_GBM -DDEFAULT_ES=2` internally, so the
extra `-DNOX11` / `-DNO_GBM` are redundant. Harmless, and they save the AI from
having to confirm.

**Verify**:

```bash
ls -l build/lib/libGL.a        # should exist, ~7 MB
```
The location may be `build/lib/libGL.a` or the repository-root `lib/libGL.a`
depending on the gl4es version. **If unsure, `find . -name libGL.a`.**

#### S4.2 Lua (produces luapic/*.o)

Must be an **unmodified** official lua-5.3.3 tree:

```bash
curl -LO https://www.lua.org/ftp/lua-5.3.3.tar.gz
tar xf lua-5.3.3.tar.gz
LUA_SRC="$PWD/lua-5.3.3/src"

mkdir -p build/luapic
for f in "$LUA_SRC"/*.c; do
  case "$(basename "$f")" in
    lua.c|luac.c) continue ;;   # CLI entry points, not part of the library
  esac
  "$CC" -O2 -fPIC -c "$f" -I "$LUA_SRC" -o "build/luapic/$(basename "$f" .c).o"
done
```

**Verify**: `ls build/luapic/*.o | wc -l` must be **33**.

#### S4.3 OpenAL Soft (optional, audio only)

Without it the link lacks `libopenal.so`; audio stays silent but the game runs
(`host_openal.c` has a "real OpenAL unavailable" branch).

```bash
git clone --branch 1.23.1 https://github.com/kcat/openal-soft.git
# build arm64-v8a with the NDK CMake toolchain -> libopenal.so
```

⚠ **LGPL-2.1**: if you distribute an APK you must link this dynamically (do not
`-static` it in), ship the full LGPL text, and provide a way to obtain the
source. Extracting a `libopenal.so` from some third-party APK is **not
acceptable** -- the provenance is unknown, so the LGPL obligations cannot be met.

### S5 Compile the translation units

```
input   gen/guest_*.c (after the S3 shift)
flags   -O2 -fPIC -DGUEST_TRACE_FUNCTIONS=1 -DGUEST_CHECKED_MEMORY=1
        -I <upstream>/recomp/runtime
output  obj_pic/guest_XXXX.o × 192
criteria 192/192 succeed
```

**`-fPIC` is not optional.** A shared library requires global data access through
the GOT (`adrp/ldr`); PC-relative direct access fails with:
```
relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol '...'
recompile with -fPIC
```

**The incremental predicate must check two timestamps** — `.c` and `guest.h`.
Comparing only `.c` produces a silent inconsistency where the header holds new
logic and the object file holds old logic.

### S6 Compile the host layer

```
input   host_*.c / minimal_kage.c / cpu_seam.c / missing_stubs.c /
        gl4es_bridge_gen.c / jni_bridge.c
flags   -O2 -fPIC -Dfprintf=host_fprintf  -I <runtime>  -I source
        compile jni_bridge.c separately (without the -Dfprintf macro)
        host_lua.c additionally needs  -I <path-to>lua-5.3.3/src
output  obj_pic/*.o
```

**Headers two of these files need.** `host_path.h` is written by this project and
ships in `source/` — add `-I source` (or copy it next to the sources).
`kage_pc_backend.h` comes from upstream `recomp/runtime`, the same place as
`guest.h`; it is not part of this archive. Without `-I <runtime>` pointing at a
real upstream checkout, `minimal_kage.c` will not compile.

`-Dfprintf=host_fprintf` makes stderr logging land in `inproc.log` (an app
process has no visible stderr). Do not add this macro to `jni_bridge.c` — its
own `JLOG` macro already handles it, and the macro would make it write twice.

**`gl4es_bridge_gen.c` is generated** — ~2300 GL entry points against 75
parameter signatures, written by `tools/gen_gl_bridge.py`. Its own header records
the exact counts; read that rather than trusting a size in bytes, which changes
whenever the table is regenerated. Regenerate it with that same script when
needed. The script contains a build-time gate: if the
signature table has no `'f'`/`'d'`, it errors out and exits. That gate exists to
prevent a real incident — see trap 1 in section 4.

### S7 Link

```
input   obj_pic/*.o + luapic/*.o + libGL.a
flags   -shared -Wl,-soname,libisaac.so -Wl,--version-script=libisaac.map
        -lm -ldl -lpthread
output  libisaac.so
criteria ~74 MB
         exactly 8 exported symbols, all of the form Java_com_isaacport_launcher_*
         readelf -d shows NEEDED only libm.so / libdl.so / libc.so (no libc++)
```

**`--version-script` is absolutely mandatory.** Without it you export **31,104
symbols**, and after `dlopen` into an ART process the symbol space mixes with the
runtime: lookups slow down, and name collisions become possible. `libisaac.map`
wildcards `Java_com_isaacport_launcher_*` and makes everything else local.

**The three criteria above describe the in-process build** — a real `-shared`
JNI library dlopen'ed into the app. The other supported shape is the
**child-process build**: the same object is linked as a PIE executable and
shipped as `lib/arm64-v8a/libisaac.so` so the package manager extracts it and
Java can `execve` it. That variant has **no** `Java_com_*` symbols and exports
tens of thousands of names, because it is a program, not a JNI library —
applying the 8-symbol rule to it is a false failure. To tell them apart, read
the ELF header: `PT_INTERP` present means executable. `apk/build_apk.sh` does
exactly this check in step 1 and prints which shape it is packaging.

### S8 APK (optional)

```
input   libisaac.so + apk/src/*.java
tool    apk/build_apk.sh (env vars ANDROID_JAR / R8_JAR / ELF)
criteria APK structure check passes, arm64-v8a/libisaac.so present
```

`android.jar` / `r8.jar` belong to the Android SDK. **Do not take them from this
project — install them yourself.**

### S9 Resources (not inside the APK)

```
device path  /storage/emulated/0/Android/media/com.isaacport.launcher/files/game/
needed       resources/packed/*.a      ~1.0 GiB (22 archives)
             isaac-ng.exe.unpacked
             data/options.ini          the resolution is read from it
```

**Game resources are never distributed.** See the policy notes in
`DISTRIBUTION.md`.

---

## 3. Runtime verification

Once running, the `[boot]` section should look like this:

```
=== The Binding of Isaac: Repentance - Android arm64 (repentogxm) ===
layer entries implemented: 370 (with overlap, 366 of 413 imports covered)
  KERNEL32 76  CRT 85  Lua 67  USER32 59  GL 11  Audio 24
  Steam 11  stdio 4  file 9  time 8  sys 16
✓ image loaded base=0x3c000000
[gl4es] ✓ GL_VERSION  = 2.1 gl4es wrapper 1.1.7
[gl4es] ✓ GL_RENDERER = GL4ES using <your GPU>
```

> Log tags (`[drawstat]` `[texfinal]` `[alphachk]` `[heap]` …) keep their English
> names — they are the diagnostic search keys. **All runtime log strings are in
> English** (unified on 2026-11-12) for automated parsing. Source comments in
> this tree are English too (translated on 2026-11-12 from the investigation
> notes; the Chinese originals are in `isaac-arm64-zh/`). Comments do not affect
> the artifact.

**`base=0x3c000000` is the only reliable signal that the shift worked.** Any
other value → go back to S3. `0x30000000` will fail on the region ART occupies.

---

## 4. Trap list (ordered by cost)

### 1. The GL bridge passed float arguments as integers — fixed, but know why the gate exists

The signature parser `kind_of()` used to compare a **full declaration including
the variable name** (e.g. `"GLfloat r"`) against a type name (`"GLfloat"`). They
never matched, so all 16 signatures degraded to `'i'`. Consequence: the `float`
arguments of 275 GL entry points were read off the x86 stack as integers.
`glClearColor(0,0,0,1)` became garbage → red blocks when changing rooms.

The fix strips the variable name before matching; array parameters (which decay
to pointers in C) are always passed as `'i'`. **The gate catches regressions. Do
not loosen it to make a build pass.**

### 2. Probes that actively call GL crash the process

Hit twice; both times the crash was `std::terminate`:
- `glGetProgramiv` / `glGetActiveUniform` probes → crash in `glDeleteProgram`
- adding `glBindFramebuffer` to the frame-capture path → same

**Rule: reading guest memory is safe; calling GL is not.** Any probe that
actively calls GL must first be proven locally (subprocess mode) before being
pushed to a device.

### 3. `access()` lies on Android FUSE

`/storage/emulated` goes through FUSE, where `access(F_OK)` frequently returns
`EACCES` **even when `fopen` works perfectly**. This trap kept two diagnostic
switches in a state of "file exists, code exists, never takes effect".

**Every file switch must be probed with `fopen`.**

### 4. A bare `return` inside the dispatch function swallows the GL call

A mid-function `return 0` in `host_gl_dynamic` skips the following
`gl_bridge_wrapper`, which means the GL call never happens. Especially easy to do
while adding probes.

### 5. In-process mode forbids `dup2` / `chdir` / `abort()` / signal handlers

Inside an app process, in-process mode shares everything with ART. Those four
will damage the runtime. Subprocess mode (the `game/exec` switch) may do
whatever it likes.

### 6. Static instruction count ≠ performance

`objdump` counted a 9.1% reduction in instructions; the A/B run differed by
**0.2%** (within noise). Any performance claim needs runtime data.

### 7. Judging textures by CPU-side reads marks every FBO attachment as broken

The game builds offscreen RTs from a **transparent canvas** (`rep stosd` filling
`0x00000000`) and then renders into them through an FBO. Sampling a GPU render
target from the CPU necessarily reads zeros. The "14.3% all zero" reported by
`[texfinal]` includes this batch of **legitimate** offscreen canvases.

### 8. Shared scratch pool: never "replay by pointer" to re-upload textures

Twelve texture ids may share the same ptr. Replaying by ptr copies the last
image to every id in the pool → cross-wired atlases. Such a "re-upload" scheme
was implemented once; measurement showed it made the starting room entirely
covered in the wrong textures. It is retained but **disabled by default**.

---

## 5. Two defects that persist even after a "successful" build

Do not read these as build failures — they are known, unsolved problems.

### A. Some UI atlases upload empty

Symptom: every HUD digit renders as the same `µ`, and the main menu list is
blank.

Six hypotheses ruled out (all with evidence):
- Data layer: a local, independent Python PNG decoder matches the game's decode
  result **byte for byte** (`bulletatlas.png` 1024x512: 65062 == 65062 non-zero
  bytes)
- Format is ordinary (bd=8, ct=6, non-interlaced)
- Not cleared by premultiply (alpha is zero too)
- Not cross-wiring through a shared pool
- Not case sensitivity (the `.fnt` references `TeamMeatFont16_0.png`, disk has
  lowercase — 9 of 12 mismatch; after adding a case-insensitive fallback the
  **hit count was 0**, so it is not read from loose files)
- Not `glTexSubImage2D` (**0 calls**)

**Root cause not found.** Current lead: the font atlas uploads with return
address `0x5b0a13`, which is **not** among the 6 known `SetTexelData` call sites
(4 of those 6 have a count of 0). A seventh, uninstrumented path exists.

### B. Frame drops in dense rooms

Measured model (fitted from two data points):

```
frame time ≈ 3.21 ms + 0.88 ms × offscreen-RT draws per frame
```

Each offscreen draw costs **879 μs**; a normal GLES draw should be 20-50 μs.

**Root cause located**:
- Logs prove the game uses **zero VBOs** (no `glGenBuffers`/`glBufferData` in
  7800 frames)
- It uses client-side vertex arrays: `glVertexAttribPointer` is called exactly
  **once** for the whole run
- On the ES2 + shader path, gl4es hands the guest pointer **unchanged** to the
  driver (`src/gl/drawing.c:427-433`), with no intermediate layer
- So the driver re-reads and converts vertices from CPU memory on every draw

**Fix is clear but not implemented**: gl4es's `ToBuffer()` (`src/gl/gl4es.c:707`)
supports interleaved layouts, exactly matching Isaac's `vec2 Position / vec2 UV /
vec4 Color`. It is gated behind `glstate->vao->locked==1` (only triggered by
`glLockArraysEXT`, which Isaac never calls). Add a path before the `drawing.c`
dispatch that unconditionally uses VBO caching on ES2 + shader + client array,
using `len_indices()`'s vertex count plus a (ptr, stride, len, content hash)
check to decide whether a re-upload is needed.

---

## 6. Hard limits of the original local environment (do not waste time)

If you want to validate locally on x86 first, note that the original project's
local issues **may not apply to you** (it ran under Termux + pbuffer). But these
conclusions are confirmed:

1. Font loading never happens locally (no font atlas upload in 7800 frames)
2. Local frame capture is always blank (gl4es's FBO chain is broken on pbuffer)
3. Frame drops cannot be measured locally (60 fps cap)
4. The app cannot be started (Android 12+ restrictions; the icon must be tapped
   manually)

**Therefore: conclusions about frame rate, visuals, and performance must come
from a real device.**
What you can do locally: build verification, pure read-only probes, and
`TERMINATE_SELFTEST=N` self-tests.

`TERMINATE_SELFTEST=N` forces the terminate forensics path on the Nth GL call.
Run it before touching crash-path code; `sig=11` must be 0.

---

## 7. Size and timing reference (to judge whether a build is normal)

| Item | Value |
|---|---|
| PE after unpacking | 8,650,240 bytes |
| gen/ | 192 files / 4,669,714 lines / ~213 MB |
| Translation time | ~13 minutes (8 cores) |
| Compiling 192 units | ~6-8 minutes |
| gen_all coverage | 14,920 ok / 109 unsupported / 42 failed (of 15,071) |
| Untranslated stubs | **20** (all MMX/SSE: pabsd, pcmpeqd, pshufb, vpcmpeqb, pmaddubsw, movq, pxor, movd, stmxcsr, popfd) |
| libisaac.so | ~74 MB |
| Exported symbols | 8 JNI (31,104 without a version script) |
| Image size constant | 0x0085F000 |
| Import coverage | 366 / 413 |
| Device frame rate | 58-60 fps best case (Adreno 830 / Android 17); a build carrying the 2026-11-12 diagnostic probes measures a median of 56.9 fps — see `LIMITATIONS.md` §3 |
| Startup | ~18.7 s |

Those 20 untranslated stubs **do sit on referenced code paths** (some are
referenced 1-3 times), but normal gameplay never reaches them. If your build
crashes in some unusual scenario, suspect one of them first — each calls
`guest_fault(c, <addr>, "untranslated: <instruction>")`, so the instruction name
appears directly in the log.

---

## 8. License (put this in your artifact's documentation)

| Component | License |
|---|---|
| This project (host layer + gen output) | **GPL-2.0-or-later** |
| Upstream repentogxm toolchain + `guest.h`/`guest.c` | GPL-2.0-or-later |
| gl4es | MIT |
| Lua 5.3.3 | MIT |
| OpenAL Soft 1.23.1 | **LGPL-2.1** (distribution requires dynamic linking + full license text + source availability) |
| SteamStub unpacking algorithm | Independently implemented from public documentation; Steamless (CC BY-NC-ND 4.0) was consulted, no code copied |

Where the GPL obligation comes from: `gen/*.c` compiles with `-I` pointing at
upstream `recomp/runtime` and directly uses its `guest.h` (the CPU model, address
mapping, and calling conventions all live there). That is a derivative
relationship, not "reference".

**Shipping source adds no extra burden.** Shipping binaries requires providing
the complete source alongside them.
