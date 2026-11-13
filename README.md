# The Binding of Isaac: Repentance — Android arm64 Static Recompilation

A full static recompilation of the PC engine of *The Binding of Isaac:
Repentance* (x86 PE32) into native Android arm64 code. **Not an emulator** —
15,071 x86 functions are translated to C at build time and cross-compiled into
a native `.so`.

> **Maintenance status: the original author has stopped maintaining this.**
> It is released in this state for others to take over, build, and improve.
> Everything below is a measured snapshot from one point in time, not a promise.
>
> **Why it stopped:** the entire port was built on one Android phone running
> Termux — no PC, no CI. Every change meant a full rebuild of all 192 translation
> units (10-20 minutes), followed by a hand-installed APK and a manual test.
> One iteration cost 20-30 minutes of mostly waiting, and the two remaining
> defects each needed many iterations. `LIMITATIONS.md` §8.0 has the details.
> **On a desktop this project is very different** — that rebuild is a couple of
> minutes and ADB replaces the hand-install.
>
> **Authorship:** every document in this archive was written by an AI, across
> many sessions, from device logs and source. That includes this README,
> `PITFALLS.md`, `BUILD_SPEC_FOR_AGENTS.md`, `DISTRIBUTION.md`,
> `LIMITATIONS.md` and `ANNOUNCEMENT.md`. A human directed the work and ran every
> device test; the AI wrote the code changes, the analysis and the prose.
>
> What that means: measurements are traceable to logs or to re-runnable
> commands, but the *interpretation* is not first-hand. No AI ever installed this
> on a device (§8e of `LIMITATIONS.md` explains why). If a claim conflicts with
> what you measure, trust your measurement. Upstream
> [`repentogxm`](https://github.com/0xl0cal/repentogxm) is the independent
> reference: this project is a host layer on top of it.

---

## ⚠ Read the disclaimer first

**This archive contains no game and no game resources.** You must supply your own
legitimate copy of *The Binding of Isaac: Repentance*.

This project is provided **"as is"**, with no warranty. The original author has
stopped maintaining it and provides no support.

Using a modified client **may** violate the game's EULA or Steam's Subscriber
Agreement, and could invalidate achievements, cause cloud-save conflicts, or
restrict your account -- **you bear that risk**. Device damage and data loss are
also at your own risk.

Full terms: **[`DISCLAIMER.md`](DISCLAIMER.md)**. It is not legal
advice; consult a lawyer before redistributing or using commercially.

---

## ★ Start with `PROJECT.md`

If you read one file, read that one: it is a single-page overview — what the port
is, what was layered on top of upstream, the architecture in one paragraph,
measured state, known defects, what is unverified, why it was abandoned, and what
you need to build it. It is written for someone arriving cold.

The rest of this README assumes you already know what the project is.

---

## ★ Read `PITFALLS.md` second

**It is the most valuable document in this project.**

Every pitfall actually hit, ordered by cost, each with the evidence recorded at the
time (addresses, logs, numbers):

- Build: the image base must be shifted twice, a missing `-DSTATICLIB=ON` breaks
  the link, a missing `--version-script` exports 30k symbols
- ABI: the GL bridge passed float arguments as integers for 275 entry points;
  one missing JNI pointer argument crashed on device
- Allocators: a too-small block is far worse than NULL; a failure branch advancing
  the pointer
- Android: `access()` lies on FUSE; `dup2` is forbidden in-process; symlinks do
  not survive into an APK
- Diagnostic methodology: GL-calling probes crash; the two reasons a signal
  handler "looks like it never ran"
- Performance: four real numbers showing diagnostics destroying performance;
  static instruction count ≠ performance
- **Rejected routes**: why MobileGlues does not work; why replaying textures by
  pointer cross-wires atlases

**Reading it once saves two weeks.**

---

## ★ Then read `LIMITATIONS.md`

`PITFALLS.md` records what was **hit and solved**. `LIMITATIONS.md` records what
was **never settled** before the project stopped — and it is the document that
keeps this release honest:

- **It can boot and still not finish.** The translator emits a loud stub for any
  instruction it does not implement. Ordinary rooms never exercise the whole
  translated set; endings and odd item combinations do. No recorded run reaches
  an ending, so "completable" is unproven.
- **Numbers from the Vita port do not apply here.** "81 MiB heap", "OOM after
  27 minutes" and "33-34 fps" are Vita figures. This build's guest heap cap is
  2640 MB and its measured median is 56.9 fps over 142 samples.
- **x87 is modelled as `double`, not 80-bit.** ~750 FPU-stack sites plus 331
  arithmetic sites exist in the generated code. `long double` on this platform is
  binary128 (113-bit), so a fix is available and was checked — but it requires
  rebuilding all 192 `gen/` units, and nothing proves the current build is wrong.
- **No behaviour-parity test exists.** Byte-identical objects prove the comment
  translation was clean, not that the program behaves like the PC original.
- **No mod has ever been loaded.** Every recorded run had an empty `mods/`
  directory. Three host-layer defects (non-ASCII names, 8 concurrent directory
  handles, wildcards beyond `*.ext`) would break many mods at once.

Every claim in that file is labelled **measured** or **unverified**, and each one
carries the command to check it yourself. Read the unverified rows as work items,
not as reassurance.

**Section 8 is the practical part:** what was left undone, where each change
goes (file and line), how long the build steps actually take (measured: 0.24 s
compile, 0.43 s link, 1.78 s repack), and why none of it was validated. The
short answer: the maintainer stopped, and the tooling that wrote these documents
could not install an APK on device — `pm install` is refused by SELinux from
Termux, and the APK it built was a local throwaway, never published. So nothing
here was runtime-verified. That limit applies to this whole archive, not just to
the unfinished work.

Note the chain this implies: **this archive contains no runnable artifact.** To
check anything on a device you build the `.so` and APK yourself (S7/S8), install
by tapping, and from then on host-layer changes only need the external `.so`
replaced — no reinstall.

---

---

## Current state (measured on device)

| Item | Value |
|---|---|
| Playability | Playable — see the frame-rate row below |
| Frame rate | **median 56.9 fps** over 142 `[perf]` samples (p10 43.6, max 60.4). The 58-60 fps figure below came from an earlier build. |
| Crashes | None in the current build (measured to frame 8400, SEGV count 0). Two `std::bad_alloc` aborts happened *before* the 2026-11-10 heap-cap change — see `LIMITATIONS.md` §2. |
| Startup | ~18.7 s |
| Verified device | **Adreno 830 / Android 17** (Snapdragon 8 Elite class, this one device only) |
| GL | gl4es 1.1.7, GL 2.1 → GLES 2.0 |

**Verified on exactly one device.** Other GPUs are unknown.

**Demo recording (3 min, real device):** https://youtu.be/V-F3hw618yU
— recorded on the device above. It shows the defects in §"Two known defects"
exactly as they look: HUD digits rendering as `µ`, blank menu list.

A 58-60 fps best case was measured on a build from before 2026-11-12. The current
build carries diagnostic probes added that day and measures slower; §3 of
`LIMITATIONS.md` records the evidence and the fix (rebuild without the probes).

---

## Runtime logs are entirely in English (2026-11-12)

All `fprintf` log strings were converted from Chinese to English. The count taken
at the time was 421 in the 18 host-layer files that log, plus 81 in
`runtime_patched/guest.c`. That figure is a **record of the translation pass and
cannot be re-derived from this archive** — the Chinese originals are gone, so
there is nothing left to count. What *can* be verified is the outcome (zero
Chinese log strings remain), which is the check below.

**Why**: it makes automated / LLM parsing practical. The old logs mixed Chinese
prose with numbers, which added noise to statistics and pattern matching.

Log tags are unchanged (`[drawstat]` `[texfinal]` `[alphachk]` `[heap]`
`[glring]` `[draw]` …) — they are the search keys. Only the descriptive text was
translated.

How to verify (reproducible):

```bash
# Extract log strings from the .so and confirm none contain Chinese
python3 - <<'EOF'
import re
d=open('libisaac.so','rb').read().decode('utf-8','ignore')
pat=re.compile(r'\[[A-Za-z][A-Za-z0-9_\-]{1,14}\][^\x00\n]{0,120}')
cn=[s for s in (m.group(0) for m in pat.finditer(d))
    if any(0x4e00<=ord(c)<=0x9fff for c in s)]
print(len(cn), "log strings with Chinese")
EOF
# Expected output: 0 log strings with Chinese
```

> **Source comments are in English in this tree.** They are the record of the
> investigation (each tagged with a date and its evidence), translated from the
> original Chinese notes; the Chinese originals ship unchanged in
> `isaac-arm64-zh/`. Comments do not enter the artifact and affect neither the
> build nor runtime behaviour.

---

## Two known defects (root causes located, neither fixed)

### 1. Some UI atlases upload empty → every HUD digit renders as the same `µ`, main menu list is blank

Measured on device: 105 of 538 uploaded textures are all zero (19.5%).
**Some of those are legitimate offscreen canvases** (see "traps" below); the
confirmed real defect is the font atlas:

```
[alphachk] id=83 512x128 rgb_nonzero=0 alpha_nonzero=0 -> decoder wrote nothing (all zero)
```

Six hypotheses have been ruled out (the data layer was compared byte-for-byte
against an independent PNG decoder: 65062 == 65062 non-zero bytes, proving
decoding is completely correct).

**Current lead**: the font atlas uploads with return address `0x5b0a13`, but it
is **not** among the 6 known `SetTexelData` call sites (4 of those 6 have a
count of 0). So there is a seventh path that has never been instrumented.

### 2. Frame drops in dense rooms

Measured on device: frame time ≈ 3.21 ms + 0.88 ms × offscreen-RT draws per
frame. Each offscreen draw costs **879 μs**, where a normal GLES draw should be
20-50 μs.

**Root cause located**:
- The game uses **zero VBOs** (not a single `glGenBuffers`/`glBufferData` in
  7800 frames)
- It uses client-side vertex arrays; `glVertexAttribPointer` is called exactly
  once for the whole run
- On the ES2 + shader path, gl4es hands the guest pointer straight to the driver
  (`src/gl/drawing.c:427-433`) with no intermediate layer
- So the driver re-reads and converts vertices from CPU memory on every draw

**Fix is clear but not implemented**: gl4es's `ToBuffer()` (`src/gl/gl4es.c:707`)
supports interleaved layouts, which matches Isaac's `vec2 Position / vec2 UV /
vec4 Color` exactly. It is gated behind `glstate->vao->locked==1` (only triggered
when the game calls `glLockArraysEXT`, which Isaac never does). The fix is to add
a path before the `drawing.c` dispatch that unconditionally uses VBO caching
when on ES2 + shader + client array.

---

## Building

**See `BUILD_SPEC_FOR_AGENTS.md`.** It is a build spec written for an AI agent,
with per-stage input/output contracts, machine-checkable pass criteria, and 8
traps.

Why a spec and not a script: the original build ran on Termux/arm64. On x86 you
cross-compile, and paths, toolchain, and sysroot all differ. The spec lets an AI
generate a build system that fits your machine.

### ⚠ `runtime_patched/` is mandatory

The `gen/*.c` files compile with `-I` pointing at upstream repentogxm's
`recomp/runtime`, and **we modified 3 files in it**:

```
guest.c        heavy diagnostic instrumentation added (heap/UAF/wild-address/
               stack-imbalance forensics); log strings now English
guest.h        inline checks and vtable-watch hooks added
guest_pe.h     expected PE values
```

**How to use**: take `recomp/runtime` from upstream repentogxm and overwrite the
same-named files with these three. Otherwise the build will not match the
diagnostic behaviour described here (it may fail to compile, or behave
differently).

The pristine `guest.c` is backed up as `.bak_*` inside the upstream directory.
If you want the runtime to stay upstream-clean, use the original — but you lose
all `[guest]` / `[wild]` / `[heap]` / `[stack]` diagnostic output.

**You must supply**: a legitimate PC copy of v1.7.9b (J835).
Check: 8,650,240 bytes after unpacking,
sha256 `31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404`.

**Game resources (~1 GB) are not distributed** — copy them from your own PC
installation.

---

## Rejected approach: switching to MobileGlues

Rejected based on measurement. Recorded here so nobody repeats the work:

1. **Its shader translation pipeline rejects Isaac's GLSL** (4/4 failed). Isaac
   uses GLSL 1.10 legacy syntax (`varying`/`attribute`/`gl_FragColor`/
   `texture2D`, and no `#version` directive). Adding `#version 120` also fails,
   because of a hard glslang limitation:
   `compilation for SPIR-V does not support the compatibility profile`
2. **It does not support client-side vertex arrays**: the backend context is
   always GLES 3, which removed client-side arrays, and Isaac uses zero VBOs →
   no geometry can be drawn at all
3. Failure is silent: on translation failure it returns the original GLSL, and
   `ignore_error=Partial` rewrites `COMPILE_STATUS` to `GL_TRUE`

---

## Key traps (all 8 are in the spec)

- **The image base must be shifted**: `gen_all.py --image-base` supports only
  0x30000000 and Vita's 0x98000000, so generate at 0x30000000 then shift the text
  to 0x3C000000 — and the shift must include `host_*.c`
- **The link requires a version script**: without it you export 31,104 symbols
  and pollute the ART symbol space
- **`access()` lies on Android FUSE**: probe file switches with `fopen`
- **Probes that actively call GL crash the process**: reading guest memory is
  safe, calling GL is not
- **Judging textures by CPU-side reads marks every FBO attachment as broken**

---

## License

| Component | License |
|---|---|
| This project (host layer + gen output) | **GPL-2.0-or-later** |
| Upstream repentogxm toolchain + runtime headers | GPL-2.0-or-later |
| gl4es | MIT |
| Lua 5.3.3 | MIT |
| OpenAL Soft 1.23.1 | **LGPL-2.1** (distribution requires dynamic linking + full license text + source availability) |

Where the GPL obligation comes from: `gen/*.c` compiles with `-I` pointing at
upstream `recomp/runtime` and directly uses its `guest.h`. That is a derivative
relationship, not "reference".

**Shipping source adds no extra burden.** This project ships source only, never
build artifacts.

---

## Source language status (2026-11-12)

This project ships **two source trees** that build to functionally identical
`.so` files — comments never enter the artifact:

- **the repository root** (English) — English throughout wherever it matters.
- **`zh/`** — identical code, Chinese working notes.

(The release archive splits them into `isaac-arm64-en/` and `isaac-arm64-zh/`
instead; the contents are the same.)

Verified by compiling the same file both ways (`clang -O2 -fPIC`) and comparing
object files: every file translated in the second pass produces a
**byte-identical `.o`** to its Chinese original — `host_gl4es.c`,
`host_user32.c`, `host_crt.c`, `host_gl_audio.c`, `host_crt_file.c`,
`host_msvcpp.c`, `host_dinput.c` — as do the files from the first pass
(`host_main.c`, `jni_bridge.c`, `host_android.c`, `runtime_patched/guest.c`).
That is the mechanical proof that only comment text changed: no code, string
literal, numeric constant or RVA moved. Line counts are identical too.

**What is English here**

| Area | Status |
|---|---|
| Runtime logs (`fprintf` strings inside the .so) | 100% English |
| All eight documents | English |
| Java (`MainActivity`, `NativeBridge`, `VirtualPad`) | 100% English |
| `apk/build_apk.sh`, `source/libisaac.map` | 100% English |
| `runtime_patched/` (`guest.c`, `guest.h`, `guest_pe.h`) | 100% English |
| `tools/*.py` (three scripts) | 100% English |
| Host entry points: `jni_bridge.c`, `host_main.c`, `host_android.c` | 100% English |
| Host layer: all `host_*.c` + `host_lua_tramp.inc` (comments included) | 100% English |

**What is still Chinese:** nothing. Not in the code, not in the comments, not in
any document. The 9 host-layer files carrying the deepest internals notes (2551
comment lines) are fully translated — `host_gl4es.c` (721), `host_user32.c`
(449), `host_crt.c` (397), `host_gl_audio.c` (354), `host_crt_file.c` (170),
`host_msvcpp.c` (166), `host_dinput.c` (139), `host_xinput.c` (90),
`host_lua.c` (65).

Checked mechanically, not by eye:

```
cd <repo-or-archive>/            # the English tree
grep -rlP '[\x{4e00}-\x{9fff}]' .      # no output
```

All the section numbering, dated log excerpts, RVA values and measured numbers
in the notes were carried across verbatim; only the prose around them is
English. The code is identical in both trees.

The Java sources were verified to still compile: `javac` against
`android-35/android.jar` produces the same 40 class files as the original.

