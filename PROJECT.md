# PROJECT.md — what this port is, and what it is not

This is the long version of the post. It exists because the Discord message had to
be short, and because a short message cannot carry the caveats this project needs.

**Read this before you build anything.** Three facts decide whether this is useful
to you:

1. **It is not a mod, and not an APK mod pack.** It replaces the engine, not the
   content. Everything under `resources/packed/` is untouched.
2. **There is no APK and no `.so` in this archive.** Source and documentation only.
3. **No recorded run reaches an ending.** Two instructions classes are still
   untranslated. See §5.

---

## 1. What it actually is

*The Binding of Isaac: Repentance* ships as a 32-bit x86 Windows PE. This project
takes that executable and **statically recompiles** it:

```
isaac-ng.exe (v1.7.9b, J835)
  -> SteamStub removed
  -> 15,071 functions translated to C  (4,669,714 lines, 192 units)
  -> image base shifted 0x30000000 -> 0x3C000000
  -> cross-compiled to arm64
  -> linked into libisaac.so
  -> packaged into an APK
```

No emulator. No interpreter in the hot path. No DLL translation layer. The
generated C is the game's own machine code, restructured.

**Upstream:** this builds on [`repentogxm`](https://github.com/0xl0cal/repentogxm),
the PlayStation Vita port by 0xl0cal, GPL-2.0. That project supplies the
recompiler (`gen_all.py`) and the runtime (`recomp/runtime`: `guest.c`,
`gl_bridge.c`, `kage_pc_backend.*`). This project adds the **Android host layer**
and the GL/EGL/audio/input bindings for it.

If you want to understand *how* recompilation works, read their repo first. This
project is a layer on top.

**Snapshot caveat.** This fork was taken on **2026-11-01**. Upstream has released
`v0.1.2-alpha` (2026-11-06) and `v0.2.0-alpha` (2026-11-13) since. Those releases
are almost entirely `host_vita_*` and `gl_vita_backend*` work — Vita-specific, not
applicable to an Android gl4es host. Do not assume their bug list and ours match:
they fixed endings and Chinese font atlases for Vita, and neither fix lives in a
file this project uses. Re-diff `recomp/runtime` before porting anything from
their tree.

---

## 2. What was added on top of upstream

| Layer | Files | What it does |
|---|---|---|
| CRT / heap | `host_crt*.c` | MSVC runtime imports, low-4GB guest heap |
| Win32 | `host_user32.c`, `host_win_sys.c` | 59 USER32 imports, window ledger |
| Graphics | `host_gl4es.c`, `gl4es_bridge_gen.c` | GL 2.1 -> GLES 2.0 via gl4es |
| Audio | `host_gl_audio.c`, `host_openal.c` | DirectSound / OpenAL Soft |
| Input | `host_dinput.c`, `host_xinput.c` | DirectInput8 + XInput, fake devices |
| Lua | `host_lua.c`, `host_lua_tramp.inc` | 67 Lua 5.3.3 exports |
| Steam | `host_steam.c` | Steamworks stubs |
| JNI | `jni_bridge.c` | 8 JNI entry points for the APK |

The 15,071 translated functions live in `gen/`, which is **not** in this archive
(213 MB, and regenerable from the generator plus your own PE).

---

## 3. Architecture, in one paragraph

Guest addresses are `uint32_t`. So the whole guest address space is mapped inside
the low 4 GB of the host process, and `ld32()` is literally
`*(uint32_t*)addr`. Callbacks into Windows land in host-layer functions registered
in an import table; callbacks out of the guest (vtable calls, GL, Lua) go through
a token space so 32-bit guest pointers never reach 64-bit host code by accident.
The GL layer is the interesting part: gl4es translates fixed-function calls to
GLES2 shaders, but this game turns out to use shaders directly, so the FPE path is
mostly dead weight — see `PITFALLS.md` §1 for how that was discovered.

---

## 4. Current state (measured, one device)

| Item | Value |
|---|---|
| Device | Adreno 830 / Android 17 (Snapdragon 8 Elite class) |
| Frame rate | median 56.9 fps over 142 samples (p10 43.6, max 60.4) |
| Best case, earlier build | 58-60 fps (17.10 ms/frame) |
| Cold start | ~18.7 s |
| Crashes | none in the current build (measured to frame 8400) |

**Verified on exactly one device.** Other GPUs are unknown. If you test on
something else, that is genuinely new information.

**Demo recording (3 min):** https://youtu.be/V-F3hw618yU — real device, no edits.
It shows both known defects as they actually look.

The 58-60 fps figure comes from a build before 2026-11-12. The current build
carries diagnostic probes added while chasing the font-atlas bug, and measures
slower. Removing the probes should recover roughly 3 fps; that was never measured.

---

## 5. Known defects — root causes found, not fixed

**5.1 Font atlas uploads empty.** 105 of 538 textures are all-zero. The confirmed
real one is the font atlas, so every HUD digit renders as the same `µ` and the
main menu list is blank. Six hypotheses were ruled out; the lead points at a
seventh `SetTexelData` call path.

**5.2 Dense rooms drop frames.** Traced to gl4es's `ToBuffer()` being gated behind
`vao->locked == 1`. The fix is described in `PITFALLS.md` §10 but was never
applied.

**5.3 Untranslated instructions.** 20 instructions remain unimplemented, all
MMX/SSE: `pabsd`, `pcmpeqd`, `pshufb`, `vpcmpeqb`, `pmaddubsw`, `movq`, `pxor`,
`movd`, `stmxcsr`, `popfd`. They sit on referenced code paths but normal gameplay
does not reach them. **This is why no recorded run reaches an ending, and why
"it plays fine" is not the same as "it is completable".**

Each unimplemented instruction calls `guest_fault(c, addr, "untranslated: ...")`,
so if you hit one the instruction name lands directly in the log.

---

## 6. What is not verified

- **Behaviour parity with PC.** No same-seed comparison exists. x87 is modelled as
  `double`, not 80-bit; `long double` on this platform is binary128 (113-bit) so a
  fix is possible, but it requires rebuilding all 192 units and profiling.
- **Long sessions.** The guest heap cap is 2640 MB since 2026-11-10 (raised from
  1664 MB after two `std::bad_alloc` aborts). No long run after the change.
- **Other devices.** One GPU.
- **Mods.** Zero mods have ever been loaded on the test device. The mod path is
  untested, and three host-layer defects in the Windows file APIs (non-ASCII
  names, 8 concurrent directory handles, wildcards beyond `*.ext`) would break
  many mods at once.

`LIMITATIONS.md` is the full list, with a verify command for each item.

---

## 7. Why it was abandoned

**Iteration cost, not difficulty.** The entire port was built on one Android phone
running Termux — no PC, no CI. Every change meant recompiling all 192 translation
units (208 MB of generated C, 10-20 minutes), then hand-installing the APK
(`pm install` is refused by SELinux from Termux, so it has to be tapped), then
playing until the bug appeared. That is 20-30 minutes per round, mostly waiting,
and the two remaining defects each needed many rounds.

On a desktop the same rebuild is a couple of minutes and ADB replaces the
hand-install. The work is not mysterious; the hardware was the bottleneck.

---

## 8. What you need to build it

- **Your own** legitimate Repentance v1.7.9b (J835) — 8,650,240 bytes,
  sha256 `31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404`
- Its resources: `resources/packed/*.a` (22 archives, ~1.0 GiB)
- Upstream `repentogxm` — for `recomp/runtime`, `gl_bridge.c`,
  `kage_pc_backend.h`, and the `windows.h` shim
- Lua 5.3.3 headers
- gl4es — build `libGL.a` from `ptitSeb/gl4es`
- A build machine: any x86-64 with 8 GB RAM. No GPU (nothing renders during a
  build). 8 parallel compiles peak at 1.48 GB RSS.

`BUILD_SPEC_FOR_AGENTS.md` in the archive is the full pipeline, written as
stage contracts with pass/fail criteria. It is written for an AI agent as much as
for a human, which means it is dense but complete.

**The archive is not a buildable tree.** On a fresh unpack,
`clang -fsyntax-only -I source source/minimal_kage.c` fails with
`'kage_pc_backend.h' file not found`. That is expected, not a broken upload.

---

## 9. Provenance

Every document in the archive — this one, `PITFALLS.md`,
`BUILD_SPEC_FOR_AGENTS.md`, `DISTRIBUTION.md`, `LIMITATIONS.md`,
`ANNOUNCEMENT.md`, `README.md` — was written by an AI across many sessions, from
device logs, the source, and commands run against it.

The measurements are traceable: each number comes from a log or from a command
you can re-run. What is **not** first-hand is the observation — no AI ever
installed this on a device or played it. Every device test was run by a human; the
AI wrote the record.

If a claim here disagrees with what you measure, trust your measurement.
