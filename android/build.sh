#!/bin/bash
# Builds Kingpin.apk (arm64) without Gradle: NDK CMake build, aapt2, zipalign, apksigner.
#   android/build.sh            -> build-android/Kingpin.apk
# Install: adb install -r build-android/Kingpin.apk
# Game data goes to /sdcard/Android/data/com.blounty.kingpin/files (see android/push_data.sh).
set -e
cd "$(dirname "$0")/.."
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=$SDK/ndk/$(ls $SDK/ndk | sort -V | tail -1)
BT=$SDK/build-tools/$(ls $SDK/build-tools | sort -V | tail -1)
PLATFORM=$SDK/platforms/$(ls $SDK/platforms | sort -V | tail -1)
case "$(uname -s)" in
  Darwin) NDK_HOST=darwin-x86_64 ;;
  Linux) NDK_HOST=linux-x86_64 ;;
  *) echo "Unsupported build host" >&2; exit 1 ;;
esac
build=build-android
cmake -S . -B $build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$build" --target kingpin
rm -rf $build/apk && mkdir -p $build/apk/lib/arm64-v8a
$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin/llvm-strip --strip-debug -o $build/apk/lib/arm64-v8a/libkingpin.so $build/libkingpin.so
mkdir -p "$build/assets"
cp extras/port.cfg extras/gamepad.cfg "$build/assets/"
$BT/aapt2 link -A "$build/assets" -o $build/unaligned.apk --manifest android/AndroidManifest.xml -I $PLATFORM/android.jar
# The activity class (immersive full screen) -> classes.dex
rm -rf $build/java && mkdir -p $build/java
javac -nowarn --release 11 -classpath $PLATFORM/android.jar -d $build/java $(find android/java -name '*.java')
$BT/d8 --release --min-api 30 --lib $PLATFORM/android.jar --output $build/apk $(find $build/java -name '*.class')
(cd $build/apk && zip -q -0 -r ../unaligned.apk lib && zip -q ../unaligned.apk classes.dex)
$BT/zipalign -f -P 16 4 $build/unaligned.apk $build/aligned.apk
KS=$HOME/.android/debug.keystore
if [ ! -f $KS ]; then
  mkdir -p $HOME/.android
  keytool -genkeypair -keystore $KS -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
$BT/apksigner sign --ks $KS --ks-pass pass:android --key-pass pass:android --out $build/Kingpin.apk $build/aligned.apk
echo "built $build/Kingpin.apk"
