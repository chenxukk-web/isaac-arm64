# Contributing

The original author no longer develops this, but still tests it and records what
turns up. That shapes what is useful here: **a reproducible observation is worth
far more than a fix**, because a fix cannot be verified by the author and an
observation can be added to the archive.

Read this before opening an issue. Half the reports this project could receive are
about the two defects already documented below.

---

## Reporting a problem

The single most useful thing you can send is **evidence that can be re-read**.
This project has lost hours to conclusions drawn from partial logs — three
separate rounds were spent chasing a "seventh texture path" that turned out to be
a misread return address. So:

### What to attach

**1. The startup block.** First ~20 lines of `inproc.log`, covering:

```
[boot] T+   0.0 ms  main entered
[boot] PE path = ...
[boot] image loaded base=0x...
```

If `[boot] PE path =` is missing entirely, your build predates the fix in
`v0.1.1-alpha` — rebuild from that tag first.

**2. The mode line**, from `app.log`:

```
>> mode: in-process .so (direct Surface)
>> mode: child process (...)
```

This matters more than it looks. Child-process mode streams frames over a socket
and is throttled to roughly 20 fps by design; in-process mode is a direct
`eglSwapBuffers` and runs at 60. **A performance report without this line cannot
be interpreted.** An `exec` file in the game directory forces child mode — if you
did not create it deliberately, delete it.

**3. The crash block, if it crashed.** The `[SEGV]` section with `fault_addr` and
the `[heap]` lines immediately before it.

**4. Device facts:** model, Android version, GPU, and whether the game directory
is on internal storage or `/sdcard`. `fopen` on `/sdcard` measured 61x slower
than on internal storage, so this changes conclusions.

**5. Steps:** what you did, and what you expected instead.

Logs live at:

```
Android/media/com.isaacport.launcher/files/game/inproc.log
Android/media/com.isaacport.launcher/files/game/app.log
```

### What not to report

Both of these are known, located, and unfixed. An issue saying "the HUD shows µ"
adds nothing; an issue saying *when* it starts showing µ does.

**Defect 1 — some UI atlases upload empty.** Every HUD counter renders as `µ`,
the main menu list is blank. The font atlas is created as an empty 512x128
`ImageSolid` and never filled afterwards; `glTexSubImage2D` is called zero times.
Root cause not found. Read `STARTUP_AND_ARCHIVES.md` §2.2 before investigating —
there is a concrete lead there (`.fnt` language mismatch, and the correct
512x128 PNG sitting in an encrypted stored chunk that does not decode).

**Defect 2 — dense rooms drop frames.** The game uses zero VBOs and client-side
vertex arrays, so the driver re-reads and converts vertices from CPU memory on
every draw. An offscreen draw measures 879 µs where 20-50 µs is expected. The fix
in gl4es is identified, never applied. A `LIBGL_BATCH` experiment was tried and
abandoned: one frame issued 480,000 malloc/free pairs and the game aborted within
80 frames.

### The one crash that is not your fault

If your build dies with `fault_addr=0x87e7fff0`, that is a pre-existing
out-of-bounds write, not something you introduced. It sits 16 bytes below a page
boundary the allocator reports as occupied, and it is triggered by *any change
large enough to shift the memory layout* — repacking the archives, adding
diagnostics, anything. Four configurations and their outcomes are tabulated in
`STARTUP_AND_ARCHIVES.md` §3.3. **Fix that first; everything else is waiting on
it.**

---

## Submitting a fix

**Licence: GPL-2.0.** This archive carries `GNU GENERAL PUBLIC LICENSE Version 2,
June 1991` and nothing later. Contributions are accepted under the same terms. If
you fork and redistribute, keep the licence and the notices — that is a legal
requirement, not a courtesy.

**Keep the build contract.** After linking, `libisaac.so` must satisfy:

```
readelf -sW --dyn-syms libisaac.so | awk '$5=="GLOBAL" && $7!="UND"' | sort -u | wc -l
    -> 8

readelf -d libisaac.so | grep -c NEEDED
    -> 3                       (libm, libdl, libc — no libc++)

readelf -d libisaac.so | grep -i runpath
    -> no output
```

`tools/build_and_check.sh` compiles the host layer, links, checks all four
conditions above and the PE path, and refuses to push a build that fails any of
them. It also removes the RUNPATH that Termux clang injects, since
`BUILD_SPEC_FOR_AGENTS.md` requires none.

Two build dependencies that document does **not** mention, both found by running
the script for the first time:

- `host_lua.c` includes `lua.h`, `lauxlib.h` and `lualib.h` from **Lua 5.3.3**.
  They are not in the runtime directory and not in this archive. Point
  `ISAAC_LUA` at a `lua-5.3.3/src` checkout or the build stops there.
- Termux clang writes a RUNPATH of `$PREFIX/../usr/lib` into every link. It is
  harmless here — NEEDED is only `libm`/`libdl`/`libc` and those resolve from the
  system path first — but it violates the contract and puts a path the game
  process cannot read inside the library. `patchelf --remove-rpath` after linking
  clears it, with exported symbols and NEEDED unchanged (verified: 8 and 3, before
  and after).

**Say what you measured, not what you expected.** If you claim a frame-rate
improvement, give the phase (update / render / other), the sample count, and the
median plus 95th percentile frame time. A raw "FPS" number hides the difference
between a steady 60 and a 60 with one 6-second stall.

**Say what you did not test.** One device, one GPU, no run to an ending; that is
the state of this archive and an honest contribution keeps the same standard.

---

## Taking the project over

Read in this order — each one answers a question the previous does not:

| | file | why |
|---|---|---|
| 1 | `PROJECT.md` | what this is, the architecture, what state it is in |
| 2 | `PITFALLS.md` | every trap already paid for; read before touching the build |
| 3 | `LIMITATIONS.md` | what is verified vs unverified, with the command to check each claim |
| 4 | `STARTUP_AND_ARCHIVES.md` | the archive format, and the out-of-bounds write blocking the startup fix |
| 5 | `BUILD_SPEC_FOR_AGENTS.md` | the build, stage by stage, with pass criteria |

The single highest-value action is **finding the out-of-bounds write described in
`STARTUP_AND_ARCHIVES.md` §3.3**. A repack that makes startup twice as fast is
already written, verified byte-for-byte over 13,282 entries, and measured at 8 s
on device — it is only blocked by that bug.

Things this archive does **not** know, so you do not have to rediscover them the
hard way:

- Why the font atlas is created empty and never filled
- Whether the host-layer mod fixes (written, unit-tested, never shipped) work on a
  device
- Why the game issues 22,522 archive `fopen` calls with no cache between starts
- Whether `x87` modelled as `double` (not 80-bit) changes any observable behaviour

---

## Questions

There is no support commitment. If you ask a question that `LIMITATIONS.md`
already answers, expect a pointer to the section rather than a restatement.
