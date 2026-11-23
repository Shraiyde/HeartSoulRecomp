#!/bin/sh
# Builds the libzt Android archive the game links against.
# libzt is Business Source License 1.1. This script fetches that source; it is
# not stored in this repository. The archive is written to
# android/app/libs/libzt-release.aar and is not committed.
set -eu
cd "$(dirname "$0")/.."
OUT="android/app/libs/libzt-release.aar"
if [ -f "$OUT" ] && [ "${LIBZT_REBUILD:-}" != 1 ]; then
  echo "libzt archive already present"
  exit 0
fi
: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK}"
NDK_VERSION="${NDK_VERSION:-27.2.12479018}"
if [ ! -d "$ANDROID_HOME/ndk/$NDK_VERSION" ]; then
  echo "Install NDK $NDK_VERSION before building libzt"
  exit 1
fi
WORK="${LIBZT_WORK:-${TMPDIR:-/tmp}/heartsoul-libzt}"
rm -rf "$WORK"
mkdir -p "$WORK"
git clone --depth 1 --branch 1.8.10 https://github.com/zerotier/libzt.git "$WORK/src"
git -C "$WORK/src" submodule update --init --recursive --depth 1
python3 port/patch-libzt.py "$WORK/src"
rm -rf "$WORK/src/hns-aar"
cp -a port/libzt-android "$WORK/src/hns-aar"
mkdir -p "$WORK/src/hns-aar/gradle/wrapper"
cp android/gradlew "$WORK/src/hns-aar/gradlew"
cp android/gradle/wrapper/gradle-wrapper.jar "$WORK/src/hns-aar/gradle/wrapper/gradle-wrapper.jar"
cp android/gradle/wrapper/gradle-wrapper.properties "$WORK/src/hns-aar/gradle/wrapper/gradle-wrapper.properties"
chmod +x "$WORK/src/hns-aar/gradlew"
printf 'sdk.dir=%s\nndk.dir=%s\n' "$ANDROID_HOME" "$ANDROID_HOME/ndk/$NDK_VERSION" > "$WORK/src/hns-aar/local.properties"
(
  cd "$WORK/src/hns-aar"
  ./gradlew --no-daemon assembleRelease
)
mkdir -p android/app/libs
cp "$WORK/src/hns-aar/app/build/outputs/aar/"*.aar "$OUT"
echo "Built $OUT"
