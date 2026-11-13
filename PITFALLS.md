# Pitfalls (complete list)

> **This is the most valuable document in the project.**
> Every entry was actually hit, with the evidence recorded (addresses, logs, numbers).
> The original author has stopped maintaining this — if you run into trouble,
> check here first; someone probably already paid for that lesson.
>
> Ordered by **cost**: the earlier entries consumed hours to days, the later ones
> tens of minutes.

---

## 1. Build-time: these waste an entire run

### 1.1 ★★★ The image base cannot be generated directly; it must be shifted afterwards

**Symptom**: `PE load failed: cannot allocate fixed guest image`

**Why**: `gen_all.py --image-base` accepts only `0x30000000` and Vita's
`0x98000000`. A new base requires recomputing the coverage-contract hashes in
`COVERAGE_CROSS_ROOT_CONTRACTS_BY_IMAGE_BASE`, which is expensive. Meanwhile in an
Android app process ART's `dalvik-main` heap occupies
`0x08900000..0x38900000` — **`0x30000000` sits right inside it** — so you must
generate at that base and then textually shift to `0x3C000000`.

**Critical detail**: the shift **must also cover `host_*.c` and `jni_bridge.c`**.
The host layer hard-codes image addresses too (resource-table probes, xinput's
xprobe, and others). Shifting only `gen/` leaves inconsistencies — it compiles,
but behaves strangely at runtime.

**Verify**: at runtime the `[boot]` section must print `✓ image loaded base=0x3c000000`.

### 1.2 ★★★ Forgetting `-DSTATICLIB=ON` breaks the link

**Symptom**: `cannot find -lGL`, or `libGL.a` missing

**Why**: gl4es's `src/CMakeLists.txt` branches as follows:

```cmake
elseif(STATICLIB)
    add_library(GL STATIC ${GL_SRC})
else()
    add_library(GL SHARED ${GL_SRC})
```

Without `-DSTATICLIB=ON` you get `libGL.so`, not `.a`.

### 1.3 ★★ Omitting `--version-script` pollutes the ART symbol space

**Symptom**: `.so` grows from 74 MB to 75 MB; strange behaviour after `dlopen`

**Why**: without it you export **31,104 symbols** (every `sub_XXXXXXX` from 4.66M
lines of generated code). After `dlopen` into an app process those names mix with
ART, system libraries and other `.so` files: lookups slow down, and a collision
can mean "resolving to someone else's symbol".

**Verify**:
```bash
readelf -sW --dyn-syms libisaac.so | awk '$5=="GLOBAL" && $7!="UND"' | wc -l
# must be 8
```

### 1.4 ★★ Search every directory (including prebuilt `.o`) before deleting a non-static function

**Incident**: deleted `heap_seg_count()` — no references left in that file, but
**the runtime's `guest.c` still called it**, and `guest.o` is prebuilt. Compile
passed, link passed, `dlopen` failed on device, one wasted run.

**Current defence**: step 6 of `rebuild_pic.sh` validates all strong undefined
symbols and fails at build time. **Do not loosen that gate to make a build pass.**

### 1.5 ★★ The incremental predicate must check both `.c` and `.h`

**Symptom**: changed `guest.h` but the build output still holds old logic — it
compiles, it runs, and it is silently wrong.

**Cause**: comparing only the `.c` timestamp; a header change does not trigger a
rebuild.

### 1.6 ★ `duplicate symbol` — one function translated twice

**Incident**: the same function exists in both `gen/` and `gen_extra/`;
the link reported `duplicate symbol`. `gen_extra/` holds hand-corrected versions
and those must be excluded from `gen/`.

---

## 2. ABI and calling conventions: off by one and everything is wrong

### 2.1 ★★★ The GL bridge passed float arguments as integers for 275 entry points

**Symptom**: `glClearColor(0,0,0,1)` became garbage → red blocks when changing rooms

**Root cause**: the signature parser `kind_of()` compared a **full declaration
including the variable name** (e.g. `"GLfloat r"`) against a type name
(`"GLfloat"`). They never matched, so all 16 signatures degraded to `'i'`.

**Consequence**: on x86 every argument is on the stack; on arm64 the first 8
integers go in x0-x7 and the first 8 floats in v0-v7. Stuff the bit pattern of
`glVertex3f(1.0f,2.0f,3.0f)`'s floats into x0 as integers and gl4es receives
`0x3F800000` instead of `1.0f`.

**Fix**: strip the variable name before matching; array parameters (decayed to
pointers in C) are always passed as `'i'`. **A build-time gate now exists**: if
the signature table has no `'f'`/`'d'`, the build errors out.

### 2.2 ★★★ A JNI function pointer declaring one argument too few → device SIGSEGV

**Incident**: `jstring_to_c` contained
```c
typedef const char *(*fn_t)(void *, void *);   /* only two parameters */
```
but the real signature takes three:
```c
const char* GetStringUTFChars(JNIEnv*, jstring, jboolean *isCopy);
```

Under AArch64 the third argument travels in x2. Declaring only two leaves x2
holding **whatever the previous call left there**, and ART's implementation does
`if (isCopy != NULL) *isCopy = ...` → a write to a garbage address.

**Device evidence**:
```
*** crash sig=0b code=01 addr=000000000023 stage=41 ***
```
`addr=0x23` is exactly that leftover value.

**This explains every historical "unexplainable segfault"** — it was undefined
behaviour all along; sometimes x2 happened to be 0 or a writable address and it
"did not crash", otherwise it died on the spot. **Fix: pass NULL explicitly.**

### 2.3 ★★ Measure `_beginthreadex` argument counts at the call site

**Incident**: the call site for `basic_iostream::basic_iostream(streambuf*)`
pushes **two** arguments:
```
0065c9da  push 0        ; argument 2
0065c9e6  push edi      ; argument 1 = streambuf
0065c9e7  mov ecx, ebx  ; this = stream
```
So 8 bytes must be cleared, not 4. Clearing 4 leaves the caller's stack skewed and
every later `[ebp-x]` reads a misaligned value (measured symptom: memcpy's
destination became `0xfffffffe` — a -2 return value used as a pointer).

**Lesson: measure argument byte counts by disassembly; never copy them from SDK
headers.** Measurement and headers actually disagree on `SteamAPI_RegisterCallback`.

### 2.4 ★★ 32-bit `SYSTEM_INFO` is 36 bytes, not 64

**Incident**: `memset` wrote 64 bytes while the guest had only allocated
`sub esp,0x38` (56 bytes) of local buffer. The extra 8 bytes landed on the
`[ebp]` saved slot and the return address.

**This is the root cause of the 2026-11-02 stack imbalance**: `sub_005e77d0`'s
`pop ebp` got 0 and every later `[ebp-...]` access collapsed.

### 2.5 ★★ `_set_app_type` is cdecl; never clear arguments stdcall-style

MSVC merges three calls into one `add esp,0xc`, so per-call stdcall cleanup is
wrong.

---

## 3. Allocators: the most dangerous class of bug

### 3.1 ★★★ An allocator returning a too-small block is a hundred times worse than returning NULL

**Incident**: requested 8.5 MB, received 8 MB. No error, no trace — out-of-bounds
writes disguised as "some object mysteriously changed contents". Cost **seven
rounds** of investigation.

**Current defences**:
- a size-class allocator must have a "fail when short" exit
- double-guard with `bsz < need`
- `low_malloc` returns NULL when the request exceeds the largest size class —
  **never a short block**

### 3.2 ★★★ A failure branch must not advance the pointer

**Incident**: `low_alloc`'s mmap-failure branch did not advance `g_bump`
(intending to "avoid leaving holes"), so from then on **every** `low_alloc`
failed the same way and returned 0. Callers got NULL and Lua's
`luaL_requiref` crashed outright.

Recording a hole as "accounted for but unmapped" makes a crash site point at a
completely legitimate-looking address.

**Correct approach**: on mmap failure **shift by 64 KB and retry**, rather than
deadlocking in place.

### 3.3 ★★★ `calloc` must zero explicitly

**Incident**: the code once claimed "mmap is already zero" — true only under a
bump allocator. After switching to size-class reuse the same block gets handed out
repeatedly, still holding the previous occupant's data.

Measured: a `calloc(1,28)` hash-table object received the previous
`std::string`'s buffer, with `[+0]` = `0x746e6f66` (`"font"`), which was fed into
rehash as the bucket count → prime-table overrun → **an 805 MB memset**.

### 3.4 ★★ The low-4GB constraint: guest pointers are uint32_t

Any **pointer** handed to the guest must be a low-4GB address. Only
`ALCdevice*` / `ALCcontext*` have "real pointer" shape (real OpenAL returns heap
pointers). `HDC` / `HGLRC` are kernel small-integer handles in Win32, so tagged
small integers (`0x00DCxxxx` / `0x00ACxxxx`) neither collide with image addresses
nor ever get dereferenced as pointers.

### 3.5 ★★ `realloc`'s usable size = block size minus the block header

Using the block size as the data length copies 16 bytes too many.

---

## 4. Android-specific

### 4.1 ★★★ `access()` lies on FUSE

`/storage/emulated` goes through FUSE, where `access(F_OK)` frequently returns
`EACCES` **even when `fopen` works perfectly**.

**Consequence**: the `frame_dump` and `repair` diagnostic switches sat in a state
of "file exists, code exists, never takes effect" for many rounds.

**Iron rule: every file switch must be probed with `fopen`.**

> Known leftover: the fast path in `host_path.c` **still uses `access(F_OK)`**
> (found 2026-11-12). It only affects performance (a false negative takes the slow
> path), not correctness. Switch it to `fopen` if you touch it.

### 4.2 ★★★ In-process mode forbids `dup2` / `chdir` / `abort()` / signal handling

Inside an app process, in-process mode shares everything with ART. These four
damage the runtime:

| Operation | Consequence |
|---|---|
| `dup2(fd,1/2)` | replaces the whole app's stdout/stderr, disturbs ART → ANR on device |
| `chdir` | changes the global working directory |
| `abort()` | triggers a core dump; in-process it drags the whole app down |
| taking over signals | conflicts with ART's own handler |

Child-process mode (the `game/exec` switch) may do whatever it likes.

### 4.3 ★★ Android 12+ cannot start the app (a hard limit for automated testing)

- `am start` / `cmd activity` → `SecurityException: package=com.android.shell`
- `termux-am` → `Could not connect to socket` (termux-api not running)
- `monkey` → `app_process: not found`

**The user must tap the app icon manually every time.**

### 4.4 ★★ Enumerate every Windows-vs-POSIX semantic difference; never assume "they should match"

Any one of them turns into "the file is clearly there but cannot be read":

- **Case sensitivity**: the game's paths were written on Windows
  (`gfx/ui/ScoreMenu.anm2`) while the files are lowercase
  (`gfx/ui/scoremenu.anm2`). ext4/F2FS is case-sensitive → ENOENT
- **Backslashes**: `bin\XInput1_3.dll`
- **Trailing dots**, **reserved names**

### 4.5 ★★ Symlinks do not make it into the APK

The Termux dev environment got away with a `packed -> rt/packed ->
resources/packed` chain, but Android's `/storage/emulated/0` is FUSE and
**does not support symlinks**.

On device `fopen("packed/afterbirthp.a")` necessarily returns ENOENT → no DLC
opens → the log returns to 19 `Failed to open` lines → `backdrops.xml` reads the
old version → `gridgfxroot` returns NULL → crash at `0x48d604`.

**Fix**: the prefix fallback lives in the host layer (`host_path.c`), depending on
no filesystem feature.

### 4.6 ★ Never map one basename to two paths in a single table

**Incident**: adding both root `packed/*.a` and `resources/packed/*.a` to the
table made the game **repeatedly fopen the same archive** (measured hang; the log
stalled on `packed/repentance_de.a` spamming fopen).

Cause: the same basename appeared twice pointing at two different paths, breaking
the basename → path mapping, surfacing as an infinite retry.

**Only add `resources/packed/*.a`.** The 9 `Failed to open packed/*.a` lines are
**harmless** — ignore them.

---

## 5. Diagnostic methodology (the most valuable category)

### 5.1 ★★★ Probes that actively call GL crash the process

Hit twice; both times `std::terminate`:
- `glGetProgramiv` / `glGetActiveUniform` probes → crash in `glDeleteProgram`
- adding `glBindFramebuffer` to the frame-capture path → same

**The rule**:

| Category | Example | Risk |
|---|---|---|
| read guest memory only | `[pngdec]` `[solid]` `[attrib]` | **low** |
| read `c->eax` / stack args only | `[uniform-loc]` `[clear] mask` | **low** |
| **actively call GL / change GL state** | `glGetProgramiv` `glBindFramebuffer` | **high** |

**Any probe that actively calls GL must be proven locally (child-process mode)
before being pushed to a device.**

### 5.2 ★★★ "The handler printed nothing" usually has these two causes

1. **stderr is fully buffered when redirected to a file** — `_exit` without a
   `fflush` drops the last batch. Symptom: "only the first `[SEGV]` line gets
   printed"
2. **the handler used `fopen` / `malloc`** — the heap may already be corrupted, so
   the handler crashes a second time

**Fix**: use only `write(2)` / `_exit` / `sigaction`. Hand-write the hex formatting.

**This one cost at least three rounds.**

### 5.3 ★★★ When a guard never fires, suspect the predicate first

**Incident**: the write-protect arm condition was written as
`res_table_contains(block base)`; the correct form is
`res_table_contains(block base + 16)` (object pointer = block base + 16-byte
header). One bit off and "table hit" was **always 0** — two wasted rounds.

**Rule**: whenever a guard never fires, suspect the predicate, not "there really
is nothing wrong".

### 5.4 ★★★ For scanning diagnostics, plant an audit at the write site; do not start with write protection

**Comparison**:
- write protection + vtable guard: five rounds, caught field-by-field writes, low yield
- an audit at the **external write site** (`res_write_audit`): **one hit**

**Lesson: plant the audit at the external write site, not on the victim object.**

### 5.5 ★★ Probe caps hide data

- `[uniform-loc]` was capped at 20 entries, exactly filled by
  8×Texture0 + 12×Transform, so `Texture1` was never recorded — nearly produced a
  wrong conclusion. **Dedupe by name instead.**
- `[uidraw]` was once filled by the 8×8 placeholder white texture; not a single UI
  record was captured.
- `texdump` and `texraw` shared the `txdumped` counter; large textures exhausted
  the quota so not one 32×32 HUD texture was exported.
  **Never share static counters across branches.**

### 5.6 ★★ Judging textures by CPU-side reads marks every FBO attachment as broken

The game builds offscreen RTs from a **transparent canvas** (`rep stosd` filling
`0x00000000`), then renders into them through an FBO. Sampling a GPU render target
from the CPU necessarily reads zeros.

The "14.3% all zero" reported by `[texfinal]` includes this batch of **legitimate**
offscreen canvases.

### 5.7 ★★ `fread_covers` only checks the last 512 reads → the index is unreliable

Loading performs 24 million freads, so a texture upload's record has long since
rolled out of the window. **`fread=no` is not evidence.**

### 5.8 ★★ A global ID updated at function entry goes stale

`g_last_func_id` updates only at function entry and still holds the old value after
a callee returns. Seeing "functionID=309" at a crash does **not** mean execution
is really inside `sub_0000ea90` — in this case `ebp=403fe728` (a `sub_0048d3f0`
frame) was the contradicting evidence.

### 5.9 ★★ Re-check even your own previous conclusion with a tool

A previous round concluded `[ebp-0x1036c]` was the root cause; one
`STACK_SLOT_WATCH` (revealing it is never read) overturned that.

### 5.10 ★★ "Not in the log" has two meanings

When `grep packed` returns 0, the first assumption was "fopen failed so nothing
was logged" — but **fopen was never called at all**.

To tell them apart: break at the **entry of the layer above**, not at the
speculated end point.

### 5.11 ★★ A loud stub is safer than an empty stub

An empty stub silently returns 0, producing an **empty result** rather than an
error — the caller continues down the normal path and crashes far away.

A **loud stub** (`guest_fault(c, addr, "untranslated: <instruction>")`) puts the
instruction name straight into the log.

### 5.12 ★ `pgrep -f <name>` matches the shell itself

Match on `/proc/*/comm`, or you end up sampling a `do_wait`ing subshell and
conclude "CPU usage 0".

---

## 6. Performance: these make you misjudge

### 6.1 ★★★ Diagnostic probes on hot paths destroy performance

| Incident | Cost |
|---|---|
| 2 `snprintf` per import × hundreds of millions | **+9 seconds** of startup |
| 2 `clock_gettime` per fread × 24 million | **1.7 seconds** |
| `fontDiag`, 6.8 s per round every 300 ms | **136 seconds** of main-thread time; unplayable |
| per-import logging (IMP_TRACE) | 40 million lines / **3.3 GB**; disk writes slow the game by an order of magnitude |

**Rules**:
- hot paths **store pointers**, never copy (`snprintf` → direct assignment,
  600ns → 2ns)
- diagnostics off by default, enabled via env var or file switch
- ring buffers (last N) always on; full logging off by default

### 6.2 ★★★ Unconditional `usleep` in `Sleep()` pins the frame rate at 30fps

**Symptom**: a 1017 ms frame spike = 1004 ms sleep + 17 ms actual frame

**Cause**: the game executes guest code single-threaded and serially; nothing
progresses during a sleep (the parallel worker threads of real Windows are not
running here). A long sleep is a **pure waste of wall-clock time**.

**Fix**: truncate requests `>16ms` to `1ms` (cap configurable), keep `<=16ms` as-is
(that is frame-rate throttling; truncating it makes the game spin and burn CPU).

### 6.3 ★★★ Static instruction count ≠ performance

`objdump` counted a 9.1% instruction reduction; the A/B run differed by **0.2%**
(within noise).

**Every performance claim needs runtime data.** Never make optimisation decisions
from static metrics.

### 6.4 ★★ Offscreen rendering does not mean invisible

A pbuffer is the best debugging target: one `glReadPixels` tells you whether the
image is black or white and whether it is moving — more directly than any log.

### 6.5 ★★ Local builds cannot measure device performance

- local frame rate is capped at 60fps
- font loading never happens locally (no font atlas upload in 7800 frames)
- local frame capture is always blank (gl4es's FBO chain is broken on pbuffer)
- the app cannot be started locally

**Frame rate, visuals and performance conclusions must come from a real device.**

### 6.6 ★ Distinguish "correct failure semantics" from "missing implementation"

Divide-by-zero returning 0, `OpenProcessToken` returning failure — those are
**correct behaviour**, not "unimplemented".

---

## 7. Caches and state: the silent-failure trio

### 7.1 ★★★ A cache key missing a field returns stale results

**Incident**: the window cache key omitted a field, so after changing that field
the old decode result was still returned and the text table displayed split as
`"#KEY"`.

### 7.2 ★★ A "at most once" safety check blocks every user attempt

**Incident**: a "max 1 attempt" counter on the `options.ini` rewrite blocked all
user toggles; the symptom was "none of the three language values work", with a
single log line "already attempted once".

**Lesson: with an idempotency test you do not need a global counter-style safety net.**

### 7.3 ★ A string pool can only be shortened in place

`ISMG` / `arsc` are addressed by fixed offsets; lengthening requires re-laying the
whole pool. **In-place replacement must first assert `new length <= old length`.**

---

## 8. Rejected routes (do not repeat them)

### 8.1 Switching to MobileGlues — rejected on measurement

1. **Its shader pipeline rejects Isaac's GLSL** (4/4 failed). Isaac uses GLSL 1.10
   legacy (`varying` / `attribute` / `gl_FragColor` / `texture2D`, no `#version`).
   Adding `#version 120` also fails, because of a hard glslang limitation:
   `compilation for SPIR-V does not support the compatibility profile`
2. **No client-side vertex array support**: the backend context is always GLES 3,
   which removed client-side arrays, while Isaac uses zero VBOs → no geometry can
   be drawn
3. Failure is silent: the converter returns the original GLSL and
   `ignore_error=Partial` rewrites `COMPILE_STATUS` to `GL_TRUE`

### 8.2 "Replaying" texture uploads by pointer — measured to cross-wire atlases

**Hypothesis**: the game called `glTexImage2D` before the pixels were ready, so
replay the original arguments at draw time.

**Result**: `ptr` is a **shared scratch pool**; 12 texture ids share one address.
Replaying copies the last image to every id in the pool → **the starting room
became entirely covered in the wrong textures**.

**Lesson: "same pointer was zero, then non-zero" does not imply a timing
misordering** — it may simply be two loading paths taking turns on one buffer. The
scheme is retained but **disabled by default** (the `game/repair` switch).

### 8.3 REPENTOGON — architecturally impossible

LibZHL scans x86 bytes to find hook points, but our memory contains **ARM64**.

---

## 9. Engineering discipline (paid for in blood)

### 9.1 A failure path's logging must not depend on that path succeeding first

Diagnostics placed **after** the failure path never see the failure. That is why
round 38's `.so` conversion "did nothing when tapped".

### 9.2 "Fall back on failure" requires verifying the fallback itself

Round 36: only the main path was tested. libEGL **segfaults** on an invalid
window, so the fallback was never reached.

### 9.3 Changing a wire format requires changing both native and Java sides

### 9.4 gl4es source comments are not the implementation

`h_fopen` comments claim "Windows semantics: case-insensitive", but
`host_path_resolve()` **never implemented case-insensitive lookup at all**.

**A comment is intent, not behaviour.**

### 9.5 For huge files use `grep -n` + `sed -n`; never read them whole

`gen/` holds 4.66M lines; reading it whole blows the context window.

### 9.6 Run `pkill -9 -x <name>` before testing

Do not leave an emulator running in the background (it drags Termux down).

### 9.7 Look for the pattern in the symptom before guessing the cause

The breakthrough that round did not come from disassembly but from dumping the
pool and doing **string statistics**, revealing the rule "every value ending in
`/` is lost".

**Before reading thousands of lines of disassembly, find a way to dump the data
structure and run statistics on it.**

### 9.8 Determinism is a clue

A wild address that is identical across runs is not random uninitialised garbage
but a reproducible computation or fixed data — which rules out a large class of
hypotheses immediately.

### 9.9 "Ruled out" is worth as much as "confirmed"

Three guards each returning zero closed off UAF, doubly-linked-list corruption and
VirtualAlloc in one pass, shrinking the possibility space from "infinite" to
"glyph write".

**When investigating, prefer falsifiable guards over adding more prints.**

### 9.10 Reverse "which instruction ran" from "which address was written" — faster than reading disassembly

The trace's `403fe6c4/6d4/6d8` map directly onto
`[ebp-0x64]/[ebp-0x54]/[ebp-0x50]`, pinning one code region in a single step.

---

## 10. A note for whoever takes over

This project reached 15,071 functions, 4.66M lines of C, and 58 fps with no
crashes on device — it is a **complete, playable** thing, not a demo.

Both remaining defects are fully documented here:
- **Some UI atlases upload empty**: six hypotheses ruled out; the lead points at a
  "seventh `SetTexelData` call path"
- **Frame drops in dense rooms**: root cause traced to gl4es's `ToBuffer()` being
  gated behind `vao->locked==1`; the fix is clear

Reading this list once will save you at least **two weeks** of repeating the same
mistakes.

Good luck.
