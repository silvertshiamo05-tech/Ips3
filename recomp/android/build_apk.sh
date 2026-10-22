#!/usr/bin/env bash
# Builds Bayonetta.apk (arm64): the runtime as libmain.so with the Android NDK, SDL's Java
# activity plus ours, packaged and signed with the Android SDK's command-line tools (no Gradle).
#
#   ANDROID_NDK=... ANDROID_SDK=... build_apk.sh <out.apk> [PPC_DIR]
#
# IPS3_ANDROID_PACKAGE and IPS3_ANDROID_LABEL give the app another package name and name, so a
# second copy installs next to the first: a package name ending in .menu makes the menu version
# (settings before the game).
#
# Without PPC_DIR it builds against the recompiled game in bayonetta/private/ppc. The APK then
# contains code generated from the game: keep it to yourself, never publish it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(realpath -m "$1")"
PPC_DIR="${2:-$HERE/../bayonetta/private/ppc}"
NDK="${ANDROID_NDK:?set ANDROID_NDK}"
SDK="${ANDROID_SDK:?set ANDROID_SDK}"
API=34
TOOLS="$SDK/build-tools/35.0.0" # 34's d8 fails on anonymous classes from newer JDKs
JAR="$SDK/platforms/android-$API/android.jar"
WORK="${IPS3_ANDROID_BUILD:-$HERE/../runtime/build-android}"
mkdir -p "$WORK"

cmake -S "$HERE/../runtime" -B "$WORK/native" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
	-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
	-DCMAKE_BUILD_TYPE=Release -DPPC_DIR="$(realpath "$PPC_DIR")" > "$WORK/cmake.log"
ninja -C "$WORK/native" bayonetta

rm -rf "$WORK/apk" && mkdir -p "$WORK/apk/classes" "$WORK/apk/root/lib/arm64-v8a"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug \
	-o "$WORK/apk/root/lib/arm64-v8a/libmain.so" "$WORK/native/libmain.so"

javac -nowarn --release 11 -classpath "$JAR" -d "$WORK/apk/classes" \
	$(find "$HERE/../build/SDL/android-project/app/src/main/java" "$HERE/src" -name '*.java') 2> "$WORK/javac.log" \
	|| { cat "$WORK/javac.log"; exit 1; }
jar cf "$WORK/apk/classes.jar" -C "$WORK/apk/classes" .
"$TOOLS/d8" --min-api 30 --lib "$JAR" --output "$WORK/apk/root" "$WORK/apk/classes.jar"

MANIFEST="$WORK/apk/AndroidManifest.xml"
sed -e "s/package=\"com.ips3.bayonetta\"/package=\"${IPS3_ANDROID_PACKAGE:-com.ips3.bayonetta}\"/" \
	-e "s/android:label=\"Bayonetta\"/android:label=\"${IPS3_ANDROID_LABEL:-Bayonetta}\"/g" \
	"$HERE/AndroidManifest.xml" > "$MANIFEST"
"$TOOLS/aapt2" link -o "$WORK/apk/base.apk" --manifest "$MANIFEST" -I "$JAR" \
	--min-sdk-version 30 --target-sdk-version $API
(cd "$WORK/apk/root" && zip -qr "$WORK/apk/base.apk" classes.dex lib)
"$TOOLS/zipalign" -f -P 16 4 "$WORK/apk/base.apk" "$WORK/apk/aligned.apk"

# A local signing key, made on first use (sideloading needs one, any one).
KEY="${IPS3_ANDROID_KEY:-$HERE/../build/android-debug.keystore}"
[ -f "$KEY" ] || keytool -genkeypair -keystore "$KEY" -storepass android -keypass android \
	-alias ips3 -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Ips3" > /dev/null 2>&1
"$TOOLS/apksigner" sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android --out "$OUT" "$WORK/apk/aligned.apk"
echo "built $OUT"
