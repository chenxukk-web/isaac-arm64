# Distribution

> Before using this project, read [`DISCLAIMER.md`](DISCLAIMER.md): the archive
> contains no game, carries no warranty, and using a modified client may violate
> the game's EULA -- at your own risk.

## What is published

- **Source**: this repository / archive.
- **Not published**: any build artifact. No `.so`, no APK, no generated
  `guest_####.c`, no object files.

## What is never published

The game's own material, which belongs to its publisher (Edmund McMillen /
Nicalis):

- `resources/packed/*.a` — 22 archives, ~1.0 GiB
- `isaac-ng.exe` and `isaac-ng.exe.unpacked`
- saves, keys, Workshop content

Anyone building this must supply their own legitimate PC copy of
*The Binding of Isaac: Repentance* v1.7.9b (J835). See `BUILD_SPEC_FOR_AGENTS.md`
for the exact size and SHA-256 the pipeline expects. Without those resources the
app does not start.

Also excluded from publication:

- `gen/` — 192 generated translation units, 4,669,714 lines, ~213 MB. GPL
  requires the *means* to generate it (the generator plus your own PE), not the
  generated output. Upstream `.gitignore` excludes `guest_####.c` for the same
  reason.
- `obj*/`, `luapic/`, build outputs, logs, dumps, screenshots.

## License obligations when you redistribute

This project is **GPL-2.0-or-later**. If you distribute a binary (`libisaac.so`
or an APK), you must provide the complete corresponding source.

Two third-party obligations are easy to miss:

- **OpenAL Soft 1.23.1 is LGPL-2.1.** Distributing it requires dynamic linking
  (or a documented way to relink), the full LGPL text, and a way to obtain its
  source. The simplest path is to build 1.23.1 yourself from
  https://github.com/kcat/openal-soft rather than reusing a copy found inside
  some third-party APK — the provenance of such a copy is usually unknown.
- **SteamStub unpacking** in `tools/steamstub_unpack.py` is an independent
  implementation from public documentation. Steamless
  (atom0s/Steamless, CC BY-NC-ND 4.0) was consulted for the algorithm; no code
  was copied. Keep that note if you redistribute the script.

Shipping source only — which is what this archive does — carries no additional
burden.

## Before you publish a build

1. Verify the artifact against the criteria in `BUILD_SPEC_FOR_AGENTS.md`:
   the `S7 Link` stage for **exactly 8 exported symbols** and **NEEDED without
   libc++**, and section 3 for **`base=0x3c000000`** at runtime.
2. Confirm the archive contains no file matching the excluded list above.
   `gen/` and any `.a` / `.exe` / `.so` / `.apk` should be absent.
3. Attach the SHA-256 of what you publish.

## Provenance of the code in this archive

- Host layer (`host_*.c`, `jni_bridge.c`, `libisaac.map`,
  `gl4es_bridge_gen.c`): written for this port.
- `gen_extra/*.c`: hand-corrected replacements for a few generated functions.
- `runtime_patched/guest.c`, `guest.h`, `guest_pe.h`: upstream repentogxm files
  modified for this port (English logs and added diagnostics in `guest.c`).
- `tools/*.py`: written for this port, except the SteamStub algorithm note above.
- `apk/src/*.java`, `apk/build_apk.sh`: written for this port.
