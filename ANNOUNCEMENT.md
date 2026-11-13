# Announcement copy

> **Provenance: every document in this archive is AI-written**, including this
> one and `LIMITATIONS.md`, `PITFALLS.md`, `BUILD_SPEC_FOR_AGENTS.md` and
> `DISTRIBUTION.md`. They were produced across many sessions from device logs,
> the source, and commands run against it. Figures are traceable to a log or a
> re-runnable command; the interpretation is not first-hand — no AI installed
> this on a device. If you would rather write your own post, upstream
> [`repentogxm`](https://github.com/0xl0cal/repentogxm) plus
> `BUILD_SPEC_FOR_AGENTS.md` are the factual basis for everything asserted here.

Ready-to-paste text for announcing this release. Nothing here is a build input;
it exists so whoever publishes the archive does not have to rewrite the framing
from scratch — and so the framing stays honest about what this is and is not.

In this tree the file is English throughout; the Chinese counterpart lives in
`isaac-arm64-zh/ANNOUNCEMENT.md`. Keep the two in step if you edit either: the
facts in them must not drift apart, only the language should differ.

Three things every version says, on purpose, because they are the three
questions that come first:

1. **There is no APK and no `.so`.** Source only, by design.
2. **One device was tested.** The frame numbers are not a general claim.
3. **Two defects are known and unfixed.** Say so before anyone finds out.

A fourth was added later, at the maintainer's request: **say which documents are
AI-written**, and point anyone who distrusts that at the upstream spec instead.

---

## 0. Title

Do not write "How to port" — this archive is a finished-and-abandoned port plus
its records, not a tutorial, and a tutorial-shaped title guarantees the first
reply is "where's the tutorial". Keep the docs tag if the forum uses one, and
spell out the game name: nobody searches for "Tboi".

```
[Development Documents] The Binding of Isaac Repentance — x86 PC to arm64 Android port
```

Shorter variant if the forum caps titles near 60 characters:

```
[Docs] The Binding of Isaac: Repentance — x86 PC to arm64 Android
```

---

## 1. Forum post

```
The Binding of Isaac: Repentance — Android arm64 static recompilation

NOT AN EMULATOR. The PC x86 engine is translated to C at build time and
cross-compiled into a native arm64 .so — 15,071 guest functions, no interpreter
in the hot path.

Source release only. There is no APK and no .so in this archive, and there
never will be: no game code, no resources, no generated translation units.
You build it yourself, and you must own a legitimate PC copy of Repentance
v1.7.9b (J835). See DISTRIBUTION.md for exactly what you have to supply.

  DOWNLOAD (both trees, 1.04 MB):
  https://mega.nz/file/TO5QFQjS#5lYOGYOoCduRWWZH1z4daEUQWKrZVeDlsJAOfoRnzqE

Upstream project this is built on — the PS Vita port, GPL-2.0, still the best
reference for how the recompilation works:
  https://github.com/0xl0cal/repentogxm

-- Measured state (one device) --
  Playable, 58-60 fps best case (17.10 ms/frame) on the pre-2026-11-12 build;
    the current build measures 56.9 fps median (the difference is diagnostic
    probes added while chasing the font-atlas bug -- see LIMITATIONS.md)
  Zero crashes over a 7800-frame run
  ~18.7 s cold start
  gl4es 1.1.7, GL 2.1 -> GLES 2.0

Verified on exactly one device: Adreno 830 / Android 17 (Snapdragon 8 Elite
class). Other GPUs are completely untested — do not read the numbers above as
a general claim.

-- Known defects, root causes found, NOT fixed --
  1. Some UI atlases upload empty. 105 of 538 textures are all-zero; the
     confirmed real one is the font atlas, so every HUD digit renders as the
     same µ and the main menu list is blank.
  2. Dense rooms drop frames. Located, not solved.

Both are documented with logs and addresses. I am not shipping a fix for
either, and I would rather say that up front than let people discover it.

-- What is NOT proven --
  No recorded run reaches an ending, so "completable" is unproven. The
  translator emits a loud stub for any instruction it does not implement;
  ordinary rooms never exercise the whole translated set, endings do.
  There is also no behaviour-parity test against the PC build: byte-identical
  objects prove the comment translation was clean, not that the program behaves
  the same. x87 is modelled as double, not 80-bit.

  LIMITATIONS.md labels every one of these "measured" or "unverified", with
  the exact command to check it. Read the unverified rows as work items.

-- What is in the archive --
  source/          the host layer + patched runtime, GPL-2.0-or-later
  PITFALLS.md      what was hit AND solved — every pitfall, ordered by cost,
                   with the evidence recorded at the time
  LIMITATIONS.md   what was NOT settled — with commands to verify each
  BUILD_SPEC_FOR_AGENTS.md   the build pipeline, written so an agent or a
                   human can follow it without guessing
  Two trees, isaac-arm64-en/ and isaac-arm64-zh/. The code is identical;
                   only comment and document language differs. Verified by
                   compiling all 20 host files from both trees and comparing
                   objects — byte-identical, 20/20. Comments never enter the
                   artifact.

-- This is NOT a standalone buildable tree --
  The archive ships the host layer, the docs and the build spec. It does not
  ship everything you need to compile. You must also supply:
    · upstream repentogxm's recomp/runtime — gl_bridge.c, kage_pc_backend.h
      and the windows.h shim live there
    · Lua 5.3.3 headers (third-party)
    · gl4es's libGL.a
    · your own Repentance 1.7.9b (J835) executable and resources

  Check it in one command. Unpack, then:

    clang -fsyntax-only -I source source/minimal_kage.c
    -> fatal error: 'kage_pc_backend.h' file not found

  That failure is EXPECTED, not a broken upload. Same for
  runtime_patched/guest.c (wants windows.h) and host_lua.c (wants lua.h).
  BUILD_SPEC_FOR_AGENTS.md S6 lists every -I you need.

-- Who this is for --
The original author has stopped maintaining this. It is released in this state
for others to pick up. If you want a turnkey APK, this is not it and I would
rather you know now. If you want a working, heavily documented starting point
for static recompilation on Android — including the routes that do not work
and why — this is a good one.

-- Why it stopped, honestly --
Not lack of interest, and not because the remaining work looked hard. The whole
port was built on ONE Android phone running Termux — no PC, no CI. Every change
meant a full rebuild of all 192 translation units (10-20 min, 208 MB of
generated C), then a hand-installed APK and a manual test. One iteration was
20-30 minutes of mostly waiting, and the two remaining defects each needed many
iterations.

That is why two known bugs shipped unfixed. If you have a desktop, the same
rebuild takes a couple of minutes and ADB replaces the hand-install — the work
listed in LIMITATIONS.md §8 stops being expensive. Concretely: any x86-64
machine with 8 GB of RAM. No GPU, because nothing is rendered during a build
(measured: 8 parallel compiles peak at 1.48 GB of RSS).

-- Provenance: all of these documents are AI-written --
Every document in the archive — PITFALLS.md, BUILD_SPEC_FOR_AGENTS.md,
DISTRIBUTION.md, LIMITATIONS.md, ANNOUNCEMENT.md — was written by an AI, across
many sessions, working from device logs, the source, and commands run against
that source. The logs are real and each number is traceable to one or to a
command you can re-run.

What is *not* real is first-hand observation: no AI ever installed this on a
device or played it. A human ran the tests; the AI wrote them up. So the numbers
are checkable but the judgement is second-hand — if a claim conflicts with what
you measure, trust your measurement. LIMITATIONS.md is explicit about which
claims are measured, which are inferred, and which are unverified.

If AI-written docs are a blocker for you, the fallback is clean: upstream
repentogxm has the real toolchain, its own README and STATUS, and the same game
target. This project is a host layer added on top of it.

LIMITATIONS.md section 8 lists what was left undone: three host-layer mod/file
API defects (one file, no full rebuild), the x87 precision change (verified
fix, but rebuilds all 192 translation units), and removing the diagnostic
probes that cost a few fps. Each entry has the file and line, the measured
build cost, and a suggested order. Nobody is working on any of it.

-- Before you ask --
  · No APK. Source only, deliberately.
  · No game files. Ever.
  · Will it run on my Snapdragon 8 Gen 2 / Mali / Tensor? Unknown. One device
    was tested.
  · Using a modified client may violate the EULA. Your risk, read
    DISCLAIMER.md.
  · GPL-2.0-or-later: if you distribute a binary, you owe the full
    corresponding source.

Read PITFALLS.md once. It saves two weeks.
```

---

## 2. Discord — main message

Discord caps a message at 2000 characters; this block is 1734, so it fits with
room to spare. Send this on its own and wait for reactions before posting
anything else — three messages in a row reads as spam.

```
**The Binding of Isaac: Repentance — x86 PC to arm64 Android (source + docs)**

Not an emulator. The PC x86 engine is statically recompiled to C at build time
and cross-compiled into a native arm64 `.so` — 15,071 functions, no interpreter.

**Source only** — no APK, no `.so`. You build it, and you need your own legit
copy of Repentance v1.7.9b (J835).

**Upstream:** built on repentogxm, the PS Vita port (GPL-2.0):
https://github.com/0xl0cal/repentogxm

**Download (both trees, 1.04 MB):**
https://mega.nz/file/TO5QFQjS#5lYOGYOoCduRWWZH1z4daEUQWKrZVeDlsJAOfoRnzqE

**Measured on one device** (Adreno 830 / Android 17): 56.9 fps median over 142
samples, ~18.7 s cold start, no crashes. Other GPUs unknown.

**Known defects, root causes found, not fixed:** font atlas uploads empty (HUD
digits all render as `µ`, menu list blank) and dense rooms drop frames. Logs and
addresses in `LIMITATIONS.md`.

**Not proven:** no recorded run reaches an ending; no parity test vs the PC build.

**I wrote none of this.** Every document here was written by an AI across many
sessions, from device logs and source I pointed it at. The numbers are traceable
to those logs or to commands you can re-run, but no AI ever ran the game — it
could not install an APK. If a claim conflicts with what you measure, trust your
measurement.

**Abandoned over iteration cost:** on a phone each change was a 10-20 min
rebuild plus a manual APK install, 20-30 min per round. A desktop makes that
~2 min (any x86-64 box, 8 GB RAM, no GPU) and ADB replaces hand-installing.

**Not a buildable tree.** Also needed: upstream `recomp/runtime`, Lua 5.3.3
headers, gl4es `libGL.a`. Every `-I` is in `BUILD_SPEC_FOR_AGENTS.md`; the short
version is in my next message.
```

---

## 3. Discord — follow-up detail

Post only after someone asks a real question, or immediately after the main
message if the channel expects detail up front. Never as a third unsolicited
message.

```
**More detail**

Identical code in `isaac-arm64-en/` and `isaac-arm64-zh/` — only comment and doc
language differ (20/20 host files compile to byte-identical objects).

**Not an emulator wrapper,** not MobileGlues/Box64 — tried and rejected,
`PITFALLS.md` explains why.

**Not a buildable tree.** Besides the archive you need upstream repentogxm's
`recomp/runtime` (`gl_bridge.c`, `kage_pc_backend.h`, the `windows.h` shim),
Lua 5.3.3 headers, and gl4es's `libGL.a`. On a fresh unpack:

    clang -fsyntax-only -I source source/minimal_kage.c
    -> 'kage_pc_backend.h' file not found        (expected)

`runtime_patched/guest.c` wants `windows.h`; `host_lua.c` wants `lua.h`.
`BUILD_SPEC_FOR_AGENTS.md` S6 lists every `-I`.

**You must also supply** `resources/packed/*.a` (22 archives, ~1.0 GiB) and
`isaac-ng.exe`, or it won't start. See `DISTRIBUTION.md`.

**Download (both trees, 1.04 MB):**
https://mega.nz/file/TO5QFQjS#5lYOGYOoCduRWWZH1z4daEUQWKrZVeDlsJAOfoRnzqE

**Provenance, plainly.** All the documents are AI-written, across many sessions,
from this project's device logs and source. The logs are real; the numbers trace
to them or to re-runnable commands. But no AI installed or played this — the
human ran every test. Where a claim and your measurement disagree, trust yours.

**Fallback if you distrust AI-written docs:** upstream repentogxm's repo has the
real toolchain and its own STATUS/README; this project is a host layer on top of
it. https://github.com/0xl0cal/repentogxm

**FAQ:** 8 Gen 2 / Mali / Tensor? No idea — one device. Upload the .so? No.
EULA? Possibly — your risk, read `DISCLAIMER.md`.
```

---

## 4. Posting notes

- **Do not paste tables.** README's comparison tables render as walls of pipes
  in Discord and on most forum themes. That is why the blocks above lead with
  bold labels and `·` separators instead.
- **"No APK" goes in the first three lines.** People skim the opening; if the
  first thing they read is a frame rate they will reply "where's the APK".
- **Link, do not inline.** Append the archive or repository URL to the end of
  the main message. Do not paste `PITFALLS.md` into the thread — the whole
  point of that document is that people read it themselves.
- **In Discord, keep the thread organised.** Main message in the showcase
  channel; technical follow-up in a help channel, not scrolled out underneath
  the announcement.
- **Expect the EULA question and answer it plainly.** "Might violate it, your
  risk, read `DISCLAIMER.md`" is the honest answer and it ends the thread
  quickly. Do not soften it.
- **Do not promise a fix.** The two defects are documented as unfixed; saying
  "working on it" would be inventing a commitment that does not exist.
