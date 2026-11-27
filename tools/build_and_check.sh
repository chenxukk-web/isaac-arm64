#!/bin/sh
# build_and_check.sh — compile the host layer, link libisaac.so, and verify the
# build contract before it can reach a device.
#
# Works on a normal Linux/macOS box and under Termux, where /usr/bin/env does not
# exist. The block below re-executes this file under bash, which the rest of the
# script needs for arrays and local variables.
if [ -z "${BASH_VERSION:-}" ]; then
  for b in /bin/bash /usr/bin/bash /usr/local/bin/bash \
           /data/data/com.termux/files/usr/bin/bash "$HOME/../usr/bin/bash"; do
    [ -x "$b" ] && exec "$b" "$0" "$@"
  done
  echo "this script needs bash; none found" >&2
  exit 1
fi
#
# Configure via environment, or edit the defaults below:
#   ISAAC_CC        C compiler           (default: clang)
#   ISAAC_RT        guest runtime headers (upstream recomp/runtime)
#   ISAAC_GEN       directory holding gen/ and obj_pic/ and luapic/
#   ISAAC_MAP       libisaac.map version script
#   ISAAC_LUA       Lua 5.3.3 headers (host_lua.c includes lua.h/lauxlib.h/lualib.h)
#   ISAAC_GL        path to gl4es' libGL.a
#   ISAAC_GAME      on-device game directory (only used with -push)
#
# WHAT THIS CHECKS, AND WHY
#
# The link can succeed and still produce a .so that cannot start. That happened
# once: host_main.c held an absolute Termux path for the PE image, and only a
# post-link binary replace in the packaging scripts hid it. A build that linked
# obj_pic/ directly shipped a library that failed to load the image and exited
# with a nearly empty log. The checks below exist so that cannot recur silently.
#
#   1. exactly 8 exported symbols          — the JNI surface, nothing more
#   2. exactly 3 NEEDED entries            — libm, libdl, libc; no libc++
#   3. no undefined host_* symbols         — those mean a missing object file
#   4. PE path is relative                 — the app sandbox cannot read $HOME
#   5. no RUNPATH                          — Termux clang injects one; it breaks
#                                            the game process at load time
#
# A build that fails any check exits non-zero and is not pushed.

set -u

CC=${ISAAC_CC:-clang}
RT=${ISAAC_RT:-$HOME/rgxm/recomp/runtime}
GEN=${ISAAC_GEN:-$HOME/rgxm/build}
MAP=${ISAAC_MAP:-source/libisaac.map}
GL=${ISAAC_GL:-$HOME/gl4es/lib/libGL.a}
LUA=${ISAAC_LUA:-$HOME/lua-5.3.3/src}
GAME=${ISAAC_GAME:-/sdcard/Android/media/com.isaacport.launcher/files/game}
SRC=${ISAAC_SRC:-source}
OUT=${ISAAC_OUT:-out}
JOBS=${ISAAC_JOBS:-4}

PUSH=0; FULL=0; RELEASE=0
for a in "$@"; do
  case "$a" in
    -push)    PUSH=1 ;;
    -full)    FULL=1 ;;
    -release) RELEASE=1 ;;
    -j*)      JOBS="${a#-j}" ;;
    -h|--help) sed -n '2,40p' "$0" | sed 's/^# \?//'; exit 0 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

for p in "$RT" "$GEN/gen" "$GEN/obj_pic" "$GEN/luapic" "$MAP" "$GL"; do
  [ -e "$p" ] || { echo "missing: $p" >&2; exit 1; }
done
# host_lua.c needs the Lua 5.3.3 headers. BUILD_SPEC_FOR_AGENTS.md does not
# mention this dependency; a build following that document alone fails here.
[ -f "$LUA/lua.h" ] || {
  echo "missing: $LUA/lua.h" >&2
  echo "  host_lua.c includes lua.h, lauxlib.h and lualib.h from Lua 5.3.3." >&2
  echo "  Point ISAAC_LUA at a lua-5.3.3/src checkout." >&2
  exit 1
}

mkdir -p "$OUT"

# Diagnostic macros cost 40% build time and 35% object size. They also change
# what the logs contain, so a timing comparison must hold this constant.
if [ "$RELEASE" = "1" ]; then
  DIAG=""
else
  DIAG="-DGUEST_TRACE_FUNCTIONS=1 -DGUEST_CHECKED_MEMORY=1"
fi

T0=$(date +%s)

if [ "$FULL" = "1" ]; then
  # Only units whose .o is older than the source or than guest.h. Recompiling
  # all 192 unconditionally takes 10-20 minutes on a phone.
  LIST=$(mktemp)
  skip=0
  for f in "$GEN"/gen/guest_*.c; do
    o="$GEN/obj_pic/$(basename "$f" .c).o"
    if [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt "$RT/guest.h" ]; then
      skip=$((skip + 1))
    else
      printf '%s %s\n' "$(stat -c %s "$f")" "$f" >> "$LIST"
    fi
  done
  todo=$(wc -l < "$LIST" | tr -d ' ')
  echo "[1/3] guest units: $skip up to date, $todo to build (parallel -P$JOBS)"
  if [ "$todo" -gt 0 ]; then
    # Largest first, so the longest single compile does not become the tail.
    export CC RT GEN DIAG
    sort -rn "$LIST" | cut -d' ' -f2- | xargs -P "$JOBS" -I{} \
      sh -c '"$CC" --target=aarch64-linux-android24 -O2 -fPIC $DIAG \
             -I "$RT" -c "$1" -o "$GEN/obj_pic/$(basename "$1" .c).o"' _ {} 2>/dev/null
  fi
  rm -f "$LIST"
fi

echo "[2/3] compiling host layer"
for f in "$SRC"/host_*.c "$SRC"/minimal_kage.c "$SRC"/cpu_seam.c \
         "$SRC"/missing_stubs.c "$SRC"/gl4es_bridge_gen.c; do
  [ -f "$f" ] || continue
  "$CC" --target=aarch64-linux-android24 -O2 -fPIC -Dfprintf=host_fprintf \
    -I "$RT" -I "$LUA" -c "$f" -o "$OUT/$(basename "$f" .c).o" \
    || { echo "compile failed: $f" >&2; exit 1; }
done
# jni_bridge.c is compiled separately: it must not see the fprintf macro.
if [ -f "$SRC/jni_bridge.c" ]; then
  "$CC" --target=aarch64-linux-android24 -O2 -fPIC -I "$RT" -I "$LUA" \
    -c "$SRC/jni_bridge.c" -o "$OUT/jni_bridge.o" \
    || { echo "compile failed: jni_bridge.c" >&2; exit 1; }
fi

echo "[3/3] linking"
# obj_pic holds everything built once: the 192 guest units, the runtime objects
# (guest.o, gl_bridge.o, sub_*.o) and the old host layer. Everything this script
# just rebuilt must come from $OUT instead, or the linker sees duplicates. The
# runtime objects have no source here and must stay.
REBUILT='obj_pic/(host_[^/]*|jni_bridge|minimal_kage|cpu_seam|missing_stubs|gl4es_bridge_gen)\.o$'
"$CC" --target=aarch64-linux-android24 -shared \
  $(ls "$GEN"/obj_pic/*.o | grep -vE "$REBUILT") \
  "$GEN"/luapic/*.o "$OUT"/*.o \
  -o "$OUT/libisaac.so" -Wl,-soname,libisaac.so -Wl,--version-script="$MAP" \
  -lm -ldl -lpthread "$GL" \
  || { echo "link failed" >&2; exit 1; }

# ---- strip the injected RUNPATH ----
# Termux clang rewrites the rpath of everything it links to point at
# $PREFIX/lib. That is harmless for this library (NEEDED is only libm, libdl,
# libc, all resolved from the system path first) but BUILD_SPEC_FOR_AGENTS.md
# requires no RUNPATH, and leaving it means a copy of the .so carries a path into
# a directory the game process cannot read. Remove it after linking.
if readelf -d "$OUT/libisaac.so" | grep -qiE "runpath|rpath"; then
  if command -v patchelf >/dev/null 2>&1; then
    patchelf --remove-rpath "$OUT/libisaac.so" \
      && echo "  removed the RUNPATH injected by the toolchain"
  else
    echo "  WARN: RUNPATH present and patchelf not installed; it stays" >&2
    echo "        (harmless here, but it violates the build contract)" >&2
  fi
fi

# ---- contract ----
fail=0
syms=$(readelf -sW --dyn-syms "$OUT/libisaac.so" \
       | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | sort -u | wc -l)
need=$(readelf -d "$OUT/libisaac.so" | grep -c NEEDED)
und=$(readelf -sW "$OUT/libisaac.so" \
      | awk '$7=="UND" && $8 ~ /^host_/ {print $8}' | sort -u)
nund=$(printf '%s' "$und" | grep -c . || true)
pepath=$(strings "$OUT/libisaac.so" | grep -m1 -E 'isaac-ng\.exe')
runpath=$(readelf -d "$OUT/libisaac.so" | grep -ci runpath || true)

printf '  %-14s %s\n' "exports"  "$syms (want 8)"
printf '  %-14s %s\n' "NEEDED"   "$need (want 3)"
printf '  %-14s %s\n' "undef host_*" "$nund (want 0)"
printf '  %-14s %s\n' "PE path"  "$pepath"
printf '  %-14s %s\n' "RUNPATH"  "$runpath (want 0)"

[ "$syms" = "8" ] || { echo "  FAIL: exported symbol count" >&2; fail=1; }
[ "$need" = "3" ] || { echo "  FAIL: NEEDED count" >&2; fail=1; }
[ "$nund" = "0" ] || { echo "  FAIL: undefined host_* symbols:" >&2
                       printf '%s\n' "$und" | sed 's/^/    /' >&2; fail=1; }
case "$pepath" in
  ./*) ;;
  *)   echo "  FAIL: PE path is not relative (${pepath:-missing})" >&2
       echo "        the app sandbox cannot read an absolute host path;" >&2
       echo "        see the note about host_main.c in CONTRIBUTING.md" >&2
       fail=1 ;;
esac
[ "$runpath" = "0" ] || { echo "  FAIL: RUNPATH present; Termux clang injects one" >&2
                          echo "        and the game process cannot resolve it" >&2
                          fail=1; }

[ "$fail" = "0" ] || { echo "build rejected" >&2; exit 1; }

if [ "$PUSH" = "1" ]; then
  cp "$OUT/libisaac.so" "$GAME/libisaac.so" \
    || { echo "copy to device directory failed" >&2; exit 1; }
  echo "  pushed to $GAME/libisaac.so"
  echo "  launch by hand; the app has no shell permission to start itself"
  echo "  then read $GAME/inproc.log"
else
  echo "  (not pushed; add -push to copy to the game directory)"
fi

echo "done in $(( $(date +%s) - T0 ))s"
