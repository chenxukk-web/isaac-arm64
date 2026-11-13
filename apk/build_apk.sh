#!/data/data/com.termux/files/usr/bin/bash
# =============================================================================
# build_apk.sh — minimal APK scaffold for the arm64-native TBOI:R port
# =============================================================================
# Builds an installable, signed APK whose only native payload is the game ELF,
# shipped as lib/arm64-v8a/libisaac.so and started by a tiny Java Activity.
# Nothing under tmp/rgxm_build/gen, gen_extra or any host_*.c is touched here:
# the script only READS tmp/rgxm_build/isaac_android_test and writes to
# work/apk/.
#
# Usage:
#   bash build_apk.sh              # build + verify
#   bash build_apk.sh --no-patch   # keep the ELF byte-identical (see step 4)
#   bash build_apk.sh --install    # build, then hand the APK to the installer
#   ELF=/path/to/game bash build_apk.sh
#
# Exit code 0 means: APK produced AND all structure checks passed.
# =============================================================================
set -uo pipefail

ABI=arm64-v8a
PAYLOAD_NAME=libisaac.so
APK_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD=$APK_DIR/build
OUT=$APK_DIR/out
ANDROID_JAR=${ANDROID_JAR:-/data/data/com.termux/files/home/work/luaport/tools/android/android-35/android.jar}
R8_JAR=${R8_JAR:-/data/data/com.termux/files/home/work/luaport/tools/android/r8.jar}
ELF=${ELF:-/data/data/com.termux/files/home/tmp/rgxm_build/isaac_android_test}
PKG=com.isaacport.launcher
MIN_SDK=26
TARGET_SDK=28
VERSION_CODE=1
VERSION_NAME=0.1.0
KEYSTORE=$APK_DIR/keystore.jks
KS_ALIAS=isaacport
KS_PASS=android

# The one hardcoded Termux path in the host code (host_main.c:177). See SETUP.md
# section 4.2: replaced inside the *packaged copy* only, never in the source.
OLD_PE_PATH='/data/data/com.termux/files/home/work/analysis/isaac-ng.exe.unpacked'
NEW_PE_PATH='./isaac-ng.exe.unpacked'

DO_PATCH=1
DO_INSTALL=0
for arg in "$@"; do
    case "$arg" in
        --no-patch) DO_PATCH=0 ;;
        --install)  DO_INSTALL=1 ;;
        -h|--help)  sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

say()  { printf '%s\n' "$*"; }
step() { printf '\n=== %s ===\n' "$*"; }
die()  { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

mkdir -p "$BUILD" "$OUT"
LOG=$OUT/build.log
exec > >(tee "$LOG") 2>&1

step "0. toolchain (verified with command -v, not assumed)"
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }
for t in aapt2 zip unzip zipalign apksigner keytool javac java python3; do need "$t"; done
AAPT2=$(command -v aapt2)
ZIP=$(command -v zip)
UNZIP=$(command -v unzip)
ZIPALIGN=$(command -v zipalign)
APKSIGNER=$(command -v apksigner)
KEYTOOL=$(command -v keytool)
JAVAC=$(command -v javac)
JAVA=$(command -v java)
PY=$(command -v python3)
printf '  %-10s %s\n' aapt2 "$AAPT2"
printf '  %-10s %s\n' zip "$ZIP"
printf '  %-10s %s\n' unzip "$UNZIP"
printf '  %-10s %s\n' zipalign "$ZIPALIGN"
printf '  %-10s %s\n' apksigner "$APKSIGNER"
printf '  %-10s %s\n' keytool "$KEYTOOL"
printf '  %-10s %s\n' javac "$JAVAC"
printf '  %-10s %s\n' java "$JAVA"
printf '  %-10s %s\n' python3 "$PY"
say "  build-tools: $(aapt version 2>&1 | head -1)"
say "  apksigner  : $(apksigner --version 2>&1 | head -1)"
[ -f "$ANDROID_JAR" ] || die "android.jar not found: $ANDROID_JAR"
[ -f "$R8_JAR" ]      || die "r8.jar not found: $R8_JAR"
D8_VER=$("$JAVA" -cp "$R8_JAR" com.android.tools.r8.D8 --version 2>&1 | head -1)
say "  d8         $D8_VER   (via $R8_JAR)"
say "  java       $("$JAVA" -version 2>&1 | head -1)"
say "  android.jar $ANDROID_JAR"


step "1. payload"
[ -f "$ELF" ] || die "ELF not found: $ELF"
say "  source ELF : $ELF"
say "  size       : $(stat -c %s "$ELF") bytes"
say "  sha256     : $(sha256sum "$ELF" | cut -d' ' -f1)"
file "$ELF" | sed 's/^/  /'
mkdir -p "$BUILD/payload/lib/$ABI"
PAYLOAD=$BUILD/payload/lib/$ABI/$PAYLOAD_NAME
cp -f "$ELF" "$PAYLOAD"

# * 2026-11-07: extra libraries.
# libopenal.so = OpenAL Soft 1.23.1 (OpenSL ES backend), dlopen'd at runtime
# by host_openal.c. Our OpenAL layer used to be a pure bookkeeping stub with no
# real audio output; this is what makes sound possible.
# Placed under lib/<abi>/ Android extracts it into nativeLibraryDir,
# where dlopen("libopenal.so") finds it.
EXTRA_LIBS="${EXTRA_LIBS:-}"
for x in $EXTRA_LIBS; do
    [ -f "$x" ] || { echo "missing extra library: $x" >&2; exit 1; }
    cp -f "$x" "$BUILD/payload/lib/$ABI/$(basename "$x")"
    say "  + extra library $(basename "$x")  $(stat -c %s "$x") bytes"
done

# * 2026-11-06: PAYLOAD can now be one of two things
#   1) an executable ELF (old child-process mode) -> needs chmod 0755
#   2) a real shared library .so (in-process mode) -> 755 is harmless; System.load only looks at content
# The discriminator is e_type in the ELF header:
#   ET_EXEC(2) / ET_DYN(3) with INTERP  -> executable
#   plain .so                           -> ET_DYN without PT_INTERP
if readelf -lW "$PAYLOAD" 2>/dev/null | grep -q 'INTERP'; then
    say "  payload type: executable ELF (child-process mode)"
else
    say "  payload type: shared library .so (in-process mode)"
fi
chmod 0755 "$PAYLOAD"

step "2. manifest + resources (aapt2 link, no res/ dir needed)"
"$AAPT2" link \
    -o "$BUILD/base.apk" \
    --manifest "$APK_DIR/AndroidManifest.xml" \
    -I "$ANDROID_JAR" \
    --min-sdk-version "$MIN_SDK" \
    --target-sdk-version "$TARGET_SDK" \
    --version-code "$VERSION_CODE" \
    --version-name "$VERSION_NAME" \
    || die "aapt2 link failed"
say "  base.apk: $(stat -c %s "$BUILD/base.apk") bytes"

step "3. java -> class -> dex"
rm -rf "$BUILD/classes" "$BUILD/dex"
mkdir -p "$BUILD/classes" "$BUILD/dex"
find "$APK_DIR/src" -name '*.java' > "$BUILD/sources.txt"
"$JAVAC" --release 8 -nowarn -classpath "$ANDROID_JAR" -d "$BUILD/classes" \
    @"$BUILD/sources.txt" || die "javac failed"
"$JAVA" -cp "$R8_JAR" com.android.tools.r8.D8 \
    --min-api "$MIN_SDK" --lib "$ANDROID_JAR" \
    --output "$BUILD/dex" $(find "$BUILD/classes" -name '*.class') \
    || die "d8 failed"
[ -f "$BUILD/dex/classes.dex" ] || die "d8 produced no classes.dex"
say "  classes.dex: $(stat -c %s "$BUILD/dex/classes.dex") bytes"

step "4. optional build-time string relocation (--no-patch to skip)"
if [ "$DO_PATCH" = 1 ]; then
    "$PY" - "$PAYLOAD" "$OLD_PE_PATH" "$NEW_PE_PATH" <<'PYEOF' || die "PE path patch failed"
import sys
pay, old, new = sys.argv[1], sys.argv[2].encode(), sys.argv[3].encode()
data = bytearray(open(pay, 'rb').read())
n = data.count(old)
print("  occurrences of the hardcoded PE path: %d" % n)
if n != 1:
    sys.exit("expected exactly 1 occurrence, got %d" % n)
if len(new) > len(old):
    sys.exit("replacement is longer than the original")
i = data.find(old)
data[i:i + len(old)] = new + b'\x00' * (len(old) - len(new))
open(pay, 'wb').write(bytes(data))
print("  patched %r -> %r (+%d NUL pad)" % (old, new, len(old) - len(new)))
PYEOF
    chmod 0755 "$PAYLOAD"
    say "  patched sha256: $(sha256sum "$PAYLOAD" | cut -d' ' -f1)"
else
    say "  skipped (--no-patch): ELF stays byte-identical; the game will look for"
    say "  $OLD_PE_PATH and fail at guest_image_load inside the sandbox."
fi

step "5. assemble (classes.dex deflated, .so stored for page alignment)"
cp -f "$BUILD/base.apk" "$BUILD/unsigned.apk"
( cd "$BUILD/dex" && "$ZIP" -q -X "$BUILD/unsigned.apk" classes.dex ) || die "zip classes.dex"
# * 2026-11-07: pack **every** library under lib/$ABI/ (previously only PAYLOAD_NAME).
# This is what gets extra libraries (libopenal.so) into the package.
( cd "$BUILD/payload" && "$ZIP" -q -X -0 "$BUILD/unsigned.apk" lib/$ABI/* ) \
    || die "zip payload"
"$ZIPALIGN" -p -f 4 "$BUILD/unsigned.apk" "$BUILD/aligned.apk" || die "zipalign failed"
"$ZIPALIGN" -c -p 4 "$BUILD/aligned.apk" || die "zipalign check failed"

step "6. sign (debug keystore, generated once)"
if [ ! -f "$KEYSTORE" ]; then
    "$KEYTOOL" -genkeypair -keystore "$KEYSTORE" -alias "$KS_ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass "$KS_PASS" -keypass "$KS_PASS" \
        -dname "CN=IsaacPort Debug,O=Termux,C=CN" >/dev/null 2>&1 \
        || die "keytool -genkeypair failed"
    say "  created $KEYSTORE"
else
    say "  reusing $KEYSTORE"
fi
FINAL=$OUT/$PKG-$VERSION_NAME.apk
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-key-alias "$KS_ALIAS" \
    --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
    --min-sdk-version "$MIN_SDK" --out "$FINAL" "$BUILD/aligned.apk" \
    || die "apksigner sign failed"

step "7. verify"
say "-- apksigner verify --"
"$APKSIGNER" verify --verbose --min-sdk-version "$MIN_SDK" "$FINAL" || die "apksigner verify failed"
say "-- aapt2 dump badging --"
"$AAPT2" dump badging "$FINAL" | grep -E "^(package|minSdk|targetSdk|launchable|uses-perm)" \
    | sed 's/^/  /'
say "-- zipalign -c --"
"$ZIPALIGN" -c -p 4 "$FINAL" && say "  alignment OK"
say "-- zip contents (unzip -l) --"
"$UNZIP" -l "$FINAL"

say ""
say "APK      : $FINAL"
say "size     : $(stat -c %s "$FINAL") bytes"
say "sha256   : $(sha256sum "$FINAL" | cut -d' ' -f1)"
if [ "$DO_INSTALL" = 1 ]; then
    step "8. install (hand-off to the package installer; needs one tap)"
    termux-open --content-type application/vnd.android.package-archive "$FINAL" \
        && say "  installer launched" \
        || say "  could not launch installer (grant 'Install unknown apps' to Termux)"
fi
say "DONE"
exit 0
