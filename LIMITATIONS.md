# Known limits and unverified risks

> **Provenance: every document in this archive is AI-written**, across many
> sessions, working from device logs, the source, and commands run against it.
> That includes this file, `PITFALLS.md`, `BUILD_SPEC_FOR_AGENTS.md`,
> `DISTRIBUTION.md` and `ANNOUNCEMENT.md` — none of them is the human's prose.
>
> What is real: the logs, the build outputs, the measurements quoted below. Each
> figure is either lifted verbatim from a log or re-derived by a command you can
> re-run. What is **not** real: first-hand observation. No AI installed this on a
> device or played it (§8e explains why it could not). A human ran every test and
> directed the work; an AI wrote the record.
>
> If you would rather not rely on AI-written analysis, upstream repentogxm has
> the real toolchain and its own README/STATUS. This project is a host layer on
> top of it, and that repo is the better starting point for verification.

Every claim below is marked **measured** (there is device evidence in this
archive's incident records) or **unverified** (a real risk, but nothing here
proves or disproves it). Do not read the unverified section as "probably fine",
and do not read it as "known broken" either. It is a list of what has not been
tested, written down so nobody mistakes silence for a guarantee.

`PITFALLS.md` records traps that were **hit and solved**. This file records what
was **not** settled before the project stopped.

**On log excerpts below:** where a quote comes from a device run, the original
build logged in Chinese and the excerpt has been translated for this English
tree. The bracketed tags (`[heap]`, `[cxx]`, `[SEGV]`, `[perf]`, `[stall]`,
`[texstat]`) and every number are unchanged — only the prose between them is
English.

---

## 0. Read this before quoting numbers

A lot of numbers circulate about this port and about the Vita port it descends
from. They are not interchangeable. The two targets have different budgets:

| | Vita port (upstream) | This port (Android arm64) |
|---|---|---|
| Memory model | fixed, tight | dynamic, low-4GB address space |
| Guest heap cap | ~81 MiB class | **2640 MB** (since 2026-11-10) |
| Frame budget | heavily constrained | ~16.7 ms at 60 Hz |

"81 MiB heap", "OOM after 27 minutes" and "33-34 fps in dense rooms" are Vita
figures. They do not describe this build. If someone quotes them at you, check
which target they came from — `host_crt.c` keeps the real cap in one place:

```
grep -n 'define HEAP_END' source/host_crt.c
```

Here is how the numbers in the rest of this file were obtained, so you can
re-derive them rather than trust them:

```bash
G=/storage/emulated/0/Android/media/com.isaacport.launcher/files/game

# frame rate samples (the [perf] line prints every 60 frames)
grep -oE 'avg [0-9.]+ ms \([0-9.]+ fps\)' $G/inproc.log | sed 's/.*(\(.*\) fps)/\1/' \
  | sort -n | awk '{a[NR]=$1} END{printf "n=%d min=%.1f p10=%.1f median=%.1f max=%.1f\n",
    NR,a[1],a[int(NR*0.1)+1],a[int(NR/2)+1],a[NR]}'

# did any untranslated instruction ever fire?
grep -ciE 'guest_fault|untranslated' $G/*.log

# memory exhaustion -- matched on the "[heap] ✗" marker, which is the same in
# either language, because the library you build may log in the language of the
# source tree it came from
grep -hE '\[heap\] ✗' $G/*.log
```

---

## 1. Untranslated instructions can abort a run that started fine

**Status: unverified risk. No occurrence in this project's logs.**

The translator emits a loud stub for anything it does not implement
(`guest_fault()` in `runtime_patched/guest.h`). A loud stub is deliberate —
see `PITFALLS.md` 5.11: an empty stub returns 0 and crashes somewhere unrelated,
a loud one names the instruction.

**Why "it boots" proves little.** Starting the game exercises the startup path:
CRT init, image load, GL context, a few menus. Isaac's item interactions, special
rooms, characters and endings reach into far more of the 15,071 translated
functions. A run that is stable for an hour of ordinary rooms says nothing about
the ending cinematic.

**The instruction space is known to be incomplete — exactly 20 instructions.**
This is not an open question. `BUILD_SPEC_FOR_AGENTS.md` §7 lists them: `pabsd`,
`pcmpeqd`, `pshufb`, `vpcmpeqb`, `pmaddubsw`, `movq`, `pxor`, `movd`, `stmxcsr`
and **`popfd`**, all MMX/SSE. They sit on referenced code paths (some referenced
1-3 times) but normal gameplay does not reach them.

So the real question is not "is anything missing" — 20 things are — but "does any
particular path execute one of them". An ending, a specific item combination or a
mod can; ordinary rooms do not.

**What the evidence actually says:** across every log retained on the test
device, `guest_fault` / `untranslated` has **fired zero times**. That is consistent
with "referenced but unreached", not proof that every path stays clear.

**How to check it yourself** — this needs no code change, just a full playthrough
including an ending:

```bash
G=/storage/emulated/0/Android/media/com.isaacport.launcher/files/game
grep -nE 'guest_fault|untranslated' $G/inproc.log $G/native.log
```

Zero lines means the run stayed inside the translated set. Anything printed is
the exact instruction name and address — that is a bug report with a location
attached, not a mystery.

**If it fires:** the fix is to translate that instruction in the generator and
regenerate `gen/`; it is not a host-layer patch. Until then, treat any crash in
new content as "possibly untranslated" rather than "probably a memory bug".

---

## 2. Guest heap: raised from 1664 MB to 2640 MB, not re-validated

**Status: measured cause, fixed by arithmetic, unverified under long runs.**

This one has real device evidence. `std::bad_alloc` aborted the game twice:

```
[heap] ✗ low 4GB heap exhausted: bump used 1664.0 MB / cap 1664.0 MB, request 24 bytes
[cxx]   type[0] = ".?AVbad_alloc@std@@"
[SEGV] sig=6
```

The root cause is **address space**, not a leak, and the same cause produced the
empty textures:

```
[heap]   bump used 1664.0 MB / cap 1664.0 MB
[texstat] 940 textures, 320 of them all zero
```

When `malloc` returns NULL the PNG decoder abandons its write, the texture stays
all-zero, and the game eventually throws `bad_alloc`. A PC has 4-8 GB and never
takes this path.

**The fix already applied (2026-11-10)** — `host_crt.c`:

| | before | after |
|---|---|---|
| `HEAP_END` | `0xB0000000` | `0xF0000000` |
| span | 1664 MB | 2688 MB (minus 48 MB reserved for token space) |
| nominal | 1664 MB | 2640 MB |
| after ART's 384 MB reservation | ~1.28 GB usable | ~2.2 GB usable |

**What this does not prove.** No long run after the change has been recorded, so
"it no longer runs out" is arithmetic, not a measurement.

**The allocator converts external fragmentation into internal waste.** It is not
a plain bump pointer — since 2026-11-02 it rounds every request up to a power of
two and recycles freed blocks on 31 size-class free lists. That removes the
original failure (a pure bump allocator returned nothing, filling the cap in half
a minute) but costs up to **2x per allocation**: a 5 MB request takes an 8 MB
block, and a 100-byte request takes a 4 KB block. The 2640 MB cap is therefore
not 2640 MB of payload. Improving the size-class ladder (1.25x or 1.5x steps
instead of pure powers of two) would cut that waste substantially; it has not
been done, and would need re-validation against MSVC's aligned-free convention,
which reads the block header at `[ptr-4]` and the size at `p+0` — those two
layout facts must not change (`host_crt.c`).

**Also note the coupling:** 320 of 940 textures were all-zero, and all-zero
textures still occupy heap. At 512×512×4 that is roughly 320 MB held by images
that draw nothing. The font-atlas defect (see `README.md`) is therefore not only
a visual bug — **fixing it would relieve the exact pressure that caused the
`bad_alloc`**. That connection was not acted on before the project stopped.

**How to check:** play for a long session, then:

```bash
grep -hE '\[heap\] ✗|bad_alloc|host_fprintf.*exhaust' $G/*.log
grep -h 'live=' $G/inproc.log | tail -20     # if heap tracing is enabled
```

---

## 3. Frame rate: measured, and lower than the headline number

**Status: measured.**

`README.md` quotes 58-60 fps. That is the best-case figure from the earlier
build. Measured across 142 `[perf]` samples of the current build:

```
n=142   min 2.5   p10 43.6   median 56.9   max 60.4
```

Reading that honestly:

- **median 56.9 fps** — ordinary rooms sit just under the 60 Hz cap
- **p10 43.6 fps** — one frame window in ten is a third slower
- **min 2.5 fps** — room loads. `[stall] single frame 1016.5 ms` is recorded; a
  room transition costs about a second of wall clock
- the second-worst stall in the current log is a `free()` before the frame,
  meaning allocator work, not rendering

Earlier builds measured 58.4-59.5 fps median. The current build is **slower than
that**, and the reason is diagnostic instrumentation added on 2026-11-12 while
investigating the empty font atlas:

```
new log tags in the current build:
[alpha] [alphachk] [depthst] [drawstat] [fb] [fbatt] [fbset] [frame]
[keepres] [repair] [settexel] [stubhits] [texppm] [uidraw] [viewport] [zerotex]
```

Several sit on draw and state-query paths. `PITFALLS.md` 6.1 records this exact
failure mode: 2 `snprintf` per import × hundreds of millions = +9 s startup.
**These probes were never measured in isolation, so the 3 fps difference is
attributed, not proven.**

**If you want the faster build:** rebuild `gen/` with the profiling probes
compiled out (`kage_vita_png_profile_row_begin/end`, `host_settexel_probe`) and
re-measure. Do not decide from static instruction counts — `PITFALLS.md` 6.3: a
9.1% instruction reduction produced 0.2% wall-clock difference.

**What cannot be fixed by tuning:** the translated program still maintains x86
registers, flags and a dispatch path, then crosses a graphics compatibility
layer. Going meaningfully faster means rewriting hot paths as native code, and
each rewrite needs its own equivalence check.

**A separate defect, not a probe cost:** *dense rooms* drop frames on top of the
numbers above. `README.md` lists it as one of the two known defects and
`PITFALLS.md` §10 traces it to gl4es's `ToBuffer()` being gated behind
`vao->locked == 1` — "the fix is clear" but was never applied. Do not confuse
this with the instrumentation cost: removing the probes recovers ~3 fps; the
dense-room drop is its own thing and needs that code change.

---

## 4. Version and mod compatibility is narrow

**Status: measured constraint, by construction.**

The build is locked to **Repentance 1.7.9b (J835)**. `BUILD_SPEC_FOR_AGENTS.md`
checks size and SHA-256 of the executable before translating; a mismatch stops
the build, because all 15,071 function boundaries were derived from that exact
binary.

Consequences:

- **Repentance+ or any newer patch** requires re-running the analysis and
  re-deriving addresses. Translating a different build produces `gen/` that does
  not line up.
- **Lua mods** depend on function addresses and instruction layout, and the
  engine's Lua API surface is reachable only through the translated code.
  **No mod has been loaded on the test device at all** — `mods/` was empty in
  every recorded run, and no log contains a mod-loading sequence. Any claim of
  the form "mod X works" or "mod X crashes" therefore has no evidence behind it
  in this archive. Treat the entire mod surface as **untested**.
- **Mods requiring REPENTOGON or other native PC extensions** need those
  extensions ported as well. They are not part of this project.

### 4a. Host-layer limits that hit every mod at once

Before testing mods one by one, note that three defects in the Windows file APIs
apply to *all* mods and can be read straight out of `host_android.c`. None of
them require a mod to reproduce:

**Non-ASCII names are corrupted in both directions.** The two conversion helpers
are byte-wise, not UTF-8 aware:

```c
u8_to_u16:  st16(dst + i*2, (uint16_t)(unsigned char)src[i]);   /* byte -> code point */
u16_to_u8:  dst[k++] = (w < 0x80) ? (char)w : '?';              /* non-ASCII -> '?' */
```

Measured: a UTF-8 name of two CJK characters (6 bytes) becomes **six separate
Latin-1 code points**, not two characters:

```
UTF-8 (6 bytes) -> 6 UTF-16 code points: U+00E6 U+0088 U+0091 U+00E7 U+009A U+0084
```

**Any mod whose directory name contains non-ASCII characters cannot be found or
opened.** A UTF-8 decode/encode pass (about 20 lines each way) fixes this for
every mod simultaneously.

**Only 8 directory handles open at once.** `#define FIND_MAX 8` with static
`DIR *g_find_dir[8]`. A mod that recurses through subdirectories while the engine
is already enumerating can exhaust this and get a spurious failure.

**Wildcards are limited to `*` and `*.ext`.** Measured: `*abc*`, `a?c` and
`[ab]c` never match. The comment says that covers every use in the engine
(enumerating `mods/`, reading `*.xml`), which is true for the engine — but a mod
that calls `FindFirstFileW` with a richer pattern gets nothing back.

These three are **one-time fixes**, not per-mod work.

### 4b. What actually needs per-mod testing

After the host limits above are fixed, the remaining question is whether a given
mod touches a game function that was not translated.

**Two different counts, and they answer different questions.**
`BUILD_SPEC_FOR_AGENTS.md` records both:

- **15,071 functions total → 14,920 ok / 109 unsupported / 42 failed.** Those 151
  are *functions* that could not be translated completely.
- **20 untranslated *instructions***, all MMX/SSE: `pabsd`, `pcmpeqd`, `pshufb`,
  `vpcmpeqb`, `pmaddubsw`, `movq`, `pxor`, `movd`, `stmxcsr`, and **`popfd`** —
  the exact list is in `BUILD_SPEC_FOR_AGENTS.md` §7.

The 151 functions fail *because* they contain one of those 20 instructions, so 20
is the real currency, not 151. `BUILD_SPEC` also states those stubs **do sit on
referenced code paths** (some referenced 1-3 times) but that normal gameplay
never reaches them — which matches the zero `guest_fault` hits in every retained
log here.

So a mod is at risk only if it drives the game into code that executes one of
those 20 instructions. That is why a mod that plays normally can be fine while a
specific ending or item interaction is not: it is a *path* question, not a
*mod* question.

There is no offline shortcut. The engine's Lua API names are **not present** in
the `.so` or in `gen/` (measured — `strings` finds no `Isaac.*` or `MC_*` names),
so an API allowlist cannot be extracted from the binary. One instrumented run per
mod answers both questions at once, and it is reading a log rather than guessing:

```bash
G=/storage/emulated/0/Android/media/com.isaacport.launcher/files/game
grep -c 'guest_fault' $G/inproc.log      # untranslated instruction (names itself)
grep -c 'illegal address' $G/inproc.log  # address outside the translated image
```

A hit prints the instruction and address, so it is a located bug rather than a
mystery. Zero on both lines, plus the mod visibly working, is the only evidence
that counts.

---

## 5. Behaviour differences: the part that cannot be verified here

**Status: unverified. This is the honest gap.**

`README.md` says comments never enter the artifact and the two trees compile to
byte-identical objects. That proves the *translation of the notes* was clean. It
says nothing about whether the translated program behaves like the original.
Those are different claims and only the first one is proven.

The specific areas where this port may differ from PC, worst-first:

**x87 precision — real gap, and a verified fix exists.** The FPU stack is
currently modelled as `double`:

```c
double  st[8];          /* runtime_patched/guest.h -- 64-bit, x87 is 80-bit */
```

Real x87 registers carry 64 significant bits. Measured occurrence in the
generated code:

```
fst(    631 sites    store/address ST(0) -- MSVC's float/double return convention
st(     121 sites    same
*fst(c,n) op=       331 sites   actual x87 arithmetic (fdiv/fmul/fadd/fsub)
```

About 750 FPU-stack sites plus 331 arithmetic sites across 15,071 functions.

**The fix is small and was checked, not assumed.** `long double` on arm64
Android is **binary128 — 16 bytes, 113-bit significand**, which *exceeds* x87's
64 bits. Measured, not inferred:

```
sizeof(long double) = 16
1/3 stored as long double = 0.33333333333333333333   (20 digits)
same value as double      = 0.33333333333333331483   (17 digits)
```

All 331 arithmetic sites already read `*fst(c,n) op= (double)ldf(...)`, with an
explicit `(double)` on the operand. Because `*fst(c,n)` becomes `long double*`
and the operand is widened implicitly, **`gen/` needs no edit at all** — only
`st[8]` plus the four helpers in `guest.h` (`fpush`, `fpop`, `fcmp_set`, `fst`)
change type, then the tree is rebuilt. That was verified with a stand-alone
compilation of the exact generated-code form.

**Cost, measured:** `long double` arithmetic runs **29x slower** than `double`
(19 ns/op vs 1 ns/op on this device). But only 331 sites do arithmetic, and the
FPU-stack accesses are stores/copies rather than maths. Projected onto the 16.7 ms
frame budget:

```
1000 x87 ops/frame   -> +0.02 ms  ( 0.1%)
10000 x87 ops/frame  -> +0.18 ms  ( 1.1%)
```

So this is affordable *if* FPU arithmetic is not on a per-pixel or per-particle
path — which the call census suggests it is not, but which has **not been profiled
on device**. Do not apply it blind: measure first, then decide.

**Why the current `double` is still a risk:** precision error compounds in
*accumulation*, not in *transfer*. Most of the 750 sites only pass a return value,
where the 64-vs-80-bit difference is fixed at conversion and does not snowball.
The dangerous case is a collision or physics loop accumulating in x87, and that
pattern is not visible in the census. **Nothing here proves the current build is
wrong** — only that it is not exact, and that making it exact is a five-line
change plus a rebuild.

**Threading and timing.** The guest runs single-threaded on a serial dispatcher;
real Windows workers are not running. `PITFALLS.md` 6.2 documents one instance of
this (an unconditional `usleep` pinning frames at 30 fps). Anything that depends
on genuine parallelism — a race the original tolerated, an ordering the original
relied on — may behave differently or not at all.

**File and graphics state.** `access()` lies on FUSE, `dup2` is forbidden
in-process, symlinks do not survive into an APK (`PITFALLS.md` 2.x). These are
worked around, but the workarounds are behaviour-compatible only as far as they
were tested.

**How to verify — and what it costs.** The only sound method is **same-seed
parity**: run the same seed on PC and on device and compare state per frame.
That requires instrumenting the PC build to emit a state hash, which this project
does not include. **It was not done, and cannot be done from this archive
alone.** Anyone picking this up who wants a behavioural guarantee has to build
that harness first.

Anything short of that — "it felt the same", "it didn't crash" — is not evidence
of behavioural equivalence. Say so plainly when reporting bugs.

---

## 6. Summary table

| Risk | Status | Evidence | Action |
|---|---|---|---|
| Untranslated instruction aborts a late-game path | unverified | 0 occurrences in retained logs | static instruction census, or full playthrough + `grep guest_fault` |
| Guest heap exhaustion | cause measured, cap raised | 2 device aborts at 1664 MB | long run after the 2640 MB change |
| Internal waste from power-of-two blocks | measured design cost | up to 2x per allocation | raise the size-class ladder (1.25x/1.5x steps) |
| Frame rate below the headline | measured | 142 samples, median 56.9 | rebuild without probes, re-measure |
| Room-load stall ~1 s | measured | `[stall] 1016.5 ms` | not addressed |
| Dense rooms drop frames | known, root cause located | `PITFALLS.md` §10 (gl4es `ToBuffer` gated on `vao->locked==1`), `README.md` | not addressed; the fix was described but never applied |
| Version lock to 1.7.9b (J835) | by construction | `BUILD_SPEC` gate | re-derive for a new patch |
| Mod compatibility | **no mod ever loaded** | `mods/` empty in every recorded run | fix 4a host limits, then one instrumented run per mod |
| Non-ASCII mod names | measured defect | 2 CJK chars → 6 Latin-1 code points | UTF-8 decode/encode in both helpers |
| 8 concurrent dir handles | measured limit | `#define FIND_MAX 8` | raise it / allocate dynamically |
| Wildcards beyond `*` / `*.ext` | measured limit | `*abc*`, `a?c`, `[ab]c` never match | add `?` and `*` globbing |
| 151 functions not fully translated | measured census | 14,920 / 109 / 42 of 15,071 | `grep guest_fault` per run |
| 20 untranslated instructions (MMX/SSE, incl. `popfd`) | measured census | `BUILD_SPEC` §7 | on referenced paths, not reached in normal play |
| x87 modelled as `double` | gap known, fix verified | 331 arith sites; `long double` = binary128 | change `st[8]` + 4 helpers, rebuild, then profile |
| Thread/timing differences | unverified | single-threaded dispatcher | not verifiable from here |
| Empty font atlas | known, unfixed | `[alphachk]` logs | see `README.md` |
| Empty textures hold heap | measured coupling | 320/940 all-zero | fixing the atlas relieves memory |

**Read the "unverified" rows as work items, not as reassurance.**

Section 8 turns the actionable rows into a work list: exact file and line, what
the change costs to build, and the order that makes each step independently
verifiable. It opens with **§8.0, why the project was abandoned** — the short
version is iteration cost on phone-only hardware, not the difficulty of the work,
and that matters if you are deciding whether to pick it up.

---

## 7. Refused claims

Three things this project does **not** claim, because they were never checked:

1. **"It can be completed."** No recorded run reaches an ending.
2. **"It behaves like the PC version."** No parity test exists.
3. **"It works on other devices."** One device: Adreno 830 / Android 17.

If you need any of those, the work is described above. It has not been done.

### 7a. Reconcile with `PITFALLS.md` §10 before quoting it

`PITFALLS.md` ends with "this project reached ... 58 fps with no crashes on
device — it is a **complete, playable** thing, not a demo." That sentence was
written by the original author at a high point and is **more confident than the
evidence supports**. Read it as "the port is functional and plays", not as "every
path to an ending was verified":

- **"58 fps"** was true of a build from before 2026-11-12. The current build
  measures a median of 56.9 fps over 142 samples (§3). Neither number is wrong;
  they are different builds.
- **"no crashes"** is true of the runs recorded here, all of which are ordinary
  rooms. Two `std::bad_alloc` aborts did occur before the 2026-11-10 heap change
  (§2), and no run reaches an ending, so "no crashes" is a statement about the
  recorded paths, not about all paths.
- **"complete, playable"** means the port is a working game rather than a tech
  demo. It does **not** mean completable: 20 instructions are still untranslated
  (§1), and nothing proves an ending avoids them.

`PITFALLS.md` is left byte-for-byte as the author wrote it — it is an incident
record, and rewriting someone's conclusion after the fact would damage its value
as evidence. This section is the correction, not an edit.

---

## 8. What was left undone, and what it would cost

This section exists because the project stopped. The items above are not
"somebody is working on it" — **nobody is.** What follows is the work as it was
scoped at the point of stopping: where the change goes, how long it measured, and
why it was not done.

The maintainer will not validate any of this. Treat every number here as a
starting estimate from one device, not a promise.

### 8.0 Why the project was abandoned

**Not for lack of interest, and not because the remaining work looked hard. It
stopped because the edit-build-test loop cost too much wall-clock time on the
hardware available.**

The whole port was developed on **one Android phone running Termux**. No PC, no
workstation, no CI. That single fact explains most of the decisions in this
document, including several that otherwise look like laziness:

**Every build was a full rebuild.** There was no practical way to do a partial
link, so a one-line change to `guest.h` meant recompiling all 192 translation
units — 208 MB of generated C, largest single file 4.9 MB. Measured on this
device: **7.9 s for the largest single unit**; 16 mid-sized units in **16.4 s**
and the 4 largest in **8.7 s** with 8-way parallelism. Extrapolating by volume
gives ~3.5 minutes, `BUILD_SPEC_FOR_AGENTS.md` §7 records **6-8 minutes**, and a
fully cold run under memory pressure measured closer to **15-20 minutes**. The
spread is real: this phone has 15 GB of RAM and 8 cores, and compiling 8 units
concurrently is memory-bound, not CPU-bound. **For planning, assume 10-20
minutes** — that is what a real iteration felt like, and the rest of this
document quotes that figure.

**Every test needed a human hand.** The APK has to be installed by tapping it —
`pm install` is refused by SELinux (§8e) — and logs have to be read back off the
device. So the loop was not `edit → build → observe` but:

```
edit  →  full rebuild (10-20 min)  →  repack APK  →  hand-install by tapping
      →  play until the bug appears (minutes to tens of minutes)
      →  pull the log  →  read it  →  repeat
```

**One iteration was 20-30 minutes of mostly waiting**, and the hard bugs — the
empty font atlas, the frame-rate drop — needed many iterations because each one
only narrowed the space by one hypothesis. That is the entire reason two defects
shipped unfixed and why 51 fps was accepted instead of investigated: not that the
work was unknown, but that each experiment cost half an hour on a phone.

**Note what this does not excuse.** The measurements in this file are still
measurements, and the defects are still defects. The constraint explains the
schedule; it does not make the build verified. Everything in §8e about runtime
behaviour being unobserved still applies.

**If you have a PC, this project is very different.** The same full rebuild is a
couple of minutes on a desktop, and you can drive the device over ADB instead of
by hand. That single change would make the work items in 8b-8d routine rather
than expensive.

**What "a desktop" means concretely.** Measured on this tree: compiling 8 units
in parallel peaks at **1.48 GB of RSS** across all clang processes. So any
x86-64 machine with **8 GB of RAM** is comfortable, and 16 GB lets you run 16
compiles at once. **No GPU is involved** — nothing is rendered during a build;
gl4es is linked, not run. A 2015-era laptop is enough. The build also does not
touch the device, so the phone is only needed for the final test.

### 8a. Measured cost of the build steps

These were timed on the development device (8 cores, Android/Termux, clang 21).
They matter because they show the bottleneck is **not** the toolchain:

```
compile one host_*.c            0.24 s
link the 70.5 MB .so           0.43 s   (227 objects, reusing built .o files)
repack + zipalign + sign APK   1.78 s
```

The real cost is editing and validating, not building. A one-file host change
costs seconds to build; a change to `guest.h` costs a **full rebuild of all 192
`gen/` units** (~15-20 min).

### 8b. Work item 1 — the three mod/file-API defects (cheapest, least risk)

All of it lives in `source/host_android.c`. Line numbers are from the version in
this archive:

| What | Where | Fix |
|---|---|---|
| Non-ASCII file/mod names corrupted | `u16_to_u8()` line 848, `u8_to_u16()` line 860 | real UTF-8 ⇄ UTF-16 conversion (~20 lines each way) |
| Only 8 concurrent directory handles | `#define FIND_MAX 8` line 842 | raise to 32, or allocate the handle table dynamically |
| Wildcards limited to `*` / `*.ext` | `name_matches()` line 890 | add `?` and general `*` globbing |

Estimated: **20-30 minutes** of editing, then a compile (0.24 s), a link
(0.43 s) and a device check.

**Why this one is the best value:** it requires no `gen/` rebuild, so it cannot
change the behaviour of any translated function. The blast radius is one file.

### 8c. Work item 2 — x87 as `long double`

Change `st[8]` plus four helpers (`fpush`, `fpop`, `fcmp_set`, `fst`) in
`runtime_patched/guest.h`. `gen/` needs no edit — verified in section 5.

Estimated: **5 minutes** of editing plus a **full rebuild of all 192 `gen/`
units** (~15-20 min) and a device measurement.

**Why it was not done:** it rebuilds the entire `.so`, so every translated
function changes at once. The measured cost of `long double` arithmetic is 29x,
and no device profile exists to tell whether FPU math sits on a hot path. Making
this change without a frame-rate measurement afterwards would be gambling with
the one number this project actually has (median 56.9 fps).

### 8d. Work item 3 — remove the diagnostic probes

The 16 log tags listed in section 3 were added on 2026-11-12 while investigating
the empty font atlas. Removing them means rebuilding `gen/` with the profiling
hooks compiled out (`kage_vita_png_profile_row_begin/end`,
`host_settexel_probe`).

Estimated: **20 minutes** plus the same full rebuild.

**Why it was not done:** the 3 fps difference is attributed, not proven. Removing
the probes and measuring would settle it; nobody did that measurement.

### 8e. Why none of it was validated

Two independent reasons, and neither is fixable by trying harder:

1. **The maintainer stopped** — see §8.0 for why (iteration cost on phone-only
   hardware, not lack of interest). The project is released in this state on
   purpose.
2. **The agent that did the last pass could not test on device.** It built an
   APK locally (a throwaway, never published — see `DISTRIBUTION.md`) and could
   not install it: `pm install` is refused by SELinux from a Termux shell:

   ```
   avc: could not determine enforcing mode: Permission denied
   Unknown class fileSystem server has no access to read file context
     u:object_r:fuse:s0 (from path /storage/emulated/0/Download/...apk)
   Error: Unable to open file
   ```

   `/data/local/tmp` is also unwritable from here. So the loop was
   *edit → build → hand the file over*, never *edit → build → observe*. Code was
   verified by compilation and object comparison; **runtime behaviour was not
   verified at all.**

   This is worth stating plainly because it limits what the archive can claim.
   **The AI that wrote this file never ran the game.** Every *device* measurement
   in it came from logs the **maintainer** produced on a phone. What the AI
   measured itself was **build** cost (compile 0.24 s, link 0.43 s, repack 1.78 s
   in 8a) and what it verified was that the code compiles and that objects match.
   **No runtime behaviour was observed by the author of this file** — not frame
   rate, not crashes, not whether a mod loads.

### 8f. If you pick this up

A workable order, chosen so each step is independently verifiable. **Every step
below needs a device build first** — this archive ships neither the `.so` nor an
APK, so start by producing both yourself (S7/S8); 8g covers the installation
details.

1. **Mod/file APIs (8b).** One file, no `gen/` rebuild. Verify by loading a mod
   whose directory name is non-ASCII — before the fix it cannot be enumerated at
   all, so the result is unambiguous.
2. **Frame rate (8d).** Rebuild without probes, measure, compare against the 142
   samples recorded here. This is the cheapest way to recover known performance.
3. **x87 (8c).** Only after 2, and only with a frame-rate measurement before and
   after. If it costs more than ~1 ms/frame, revert it.
4. **Parity harness.** The only path to a behavioural guarantee (section 5). No
   shortcut exists.

Do not start with 4. It is the largest piece and it blocks on PC-side
instrumentation this archive does not contain.

### 8g. What a device check actually requires

**Nothing in this archive runs.** There is no APK and no `.so` inside it — that
is deliberate (`DISTRIBUTION.md`). Before any device check is possible you must
build one yourself: `BUILD_SPEC_FOR_AGENTS.md` S7 links the `.so` and S8 packages
the APK (with `apk/build_apk.sh`). So the chain is: build locally → install →
iterate.

**Installation has one non-obvious constraint:** the APK must be installed **by
tapping it** (file manager or package installer), not from a terminal. This was
measured — `pm install` is refused by SELinux, and `/data/local/tmp` is
unwritable from Termux. This is why 8e lists "could not install" as a real
blocker rather than laziness.

Once it *is* installed, later host-layer changes need no reinstall at all. The
external `.so` channel takes over:

```
/storage/emulated/0/Android/media/com.isaacport.launcher/files/game/libisaac.so
```

That directory is writable from Termux (measured), and `resolveSoPath` in
`MainActivity` fingerprints the file (length + mtime + head/tail hash) and copies
it into the private dir on change. So after the first install, the loop for a
host-layer change is: replace that file, relaunch the app, read the log. **No APK
rebuild, no install step.**

Logs land next to it as `app.log`, `inproc.log` and `native.log`.
