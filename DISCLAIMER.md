# Disclaimer

> This section is part of the project documentation. It is not legal advice --
> if you intend to redistribute or use this commercially, consult a practising
> lawyer. The law differs substantially between jurisdictions.

---

## 1. This archive does not contain the game

This archive contains **only** the host-layer code, build tooling and
documentation written to run *The Binding of Isaac: Repentance*. It does **not**
contain:

- the game executable (`isaac-ng.exe` or its unpacked form)
- game resources (`resources/packed/*.a`, ~1 GB)
- any art, audio, fonts or level data
- saves, keys or Workshop content

You **must** supply your own legitimately obtained copy of the game (Steam or
equivalent). This project is not the game and is not a substitute for buying it.

## 2. No warranty

This project is provided **"as is"**, without warranty of any kind, express or
implied, including but not limited to the warranties of merchantability, fitness
for a particular purpose and non-infringement.

The original author has **stopped maintaining** this project and provides no
support, no updates and no guarantee of fixes. The two known defects (some UI
atlases uploading empty; frame drops in dense rooms) are documented as
**unresolved**. Do not read them as promised fixes.

## 3. Use at your own risk

To the maximum extent permitted by applicable law, the original author and
contributors are **not liable** for any direct, indirect, incidental, special,
punitive or consequential damages arising from the use of, or inability to use,
this project, including but not limited to:

- device damage, data loss, corrupted saves
- game account anomalies or bans
- system instability, crashes, abnormal battery drain
- any financial loss or loss of goodwill

This project is designed to run a game **you own** on a device **you own**.
Whether and how you use it is entirely your decision and your responsibility.

## 4. Licensing and trademarks

- This project's code is released under **GPL-2.0-or-later**. The full text is
  in `LICENSE` at the root of this archive. The GPL is a **copyright licence**: it
  grants rights to **this project's code**, not to the game.
- *The Binding of Isaac* and all related content are trademarks and/or
  copyrighted works of Edmund McMillen / Nicalis, Inc. This project is
  **not affiliated with, authorised by, or endorsed by** them.
- Third-party components (gl4es, Lua, OpenAL Soft, etc.) remain under their own
  licences; see the list in `DISTRIBUTION.md`.

## 5. Interoperability purpose

The purpose of this project is to study **static recompilation** as a technique
(translating an x86 binary into native code for another instruction set) and to
let people who already bought the game run it on another device they own.

This project provides **no** means of circumventing DRM, bypassing licence
checks, or copying game content. The unpacking tool
(`tools/steamstub_unpack.py`) only produces translation input from an executable
you already legitimately own; it does not change the game's licence status.

## 6. May violate terms of service

Using a modified client **may** violate the game's End User Licence Agreement or
Steam's Subscriber Agreement, and could invalidate achievements, cause cloud-save
conflicts, or restrict your account.

**You bear this risk.** The original author is not responsible for any account
consequences. If your account matters to you, test with a separate account or in
offline mode.

## 7. Unmaintained

The original author no longer follows this project. The documentation, code and
criteria here are a **snapshot in time**. Upstream dependencies (gl4es, Lua,
OpenAL) may have moved on and may no longer be compatible.

Anyone taking over may change things freely, but owns the verification burden:
none of the "measured results" recorded here are a guarantee for later versions.
