#!/bin/bash
# Builds Kingpin.app for the iOS simulator (default) or a device:
#   ios/build.sh [sim|device]
# Device builds need a development provisioning profile for com.blounty.kingpin; make one with
#   cmake -S ios/provision -B build-ios-provision -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DTEAM=<team id> \
#     -DENTITLEMENTS=entitlements-personal.plist      (entitlements.plist with a paid team)
#   xcodebuild -project build-ios-provision/kpprovision.xcodeproj -scheme Kingpin -destination id=<device udid> \
#     -allowProvisioningUpdates -allowProvisioningDeviceRegistration -derivedDataPath build-ios-provision/dd build
# (KP_PROFILE overrides the profile used.)
set -e
cd "$(dirname "$0")/.."
target=${1:-sim}
if [ "$target" = sim ]; then sdk=iphonesimulator; else sdk=iphoneos; fi
build=build-ios-$target
cmake -S . -B $build -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
ninja -C $build kingpin
app=$build/out/Kingpin.app
rm -rf $app
mkdir -p $app
cp $build/kingpin.app/kingpin $app/
cp ios/Info.plist $app/
if [ "$target" = sim ]; then
  codesign --force --sign - --timestamp=none $app
else
  profile=${KP_PROFILE:-build-ios-provision/Debug-iphoneos/Kingpin.app/embedded.mobileprovision}
  cp "$profile" $app/embedded.mobileprovision
  security cms -D -i "$profile" > $build/profile.plist
  plutil -extract Entitlements xml1 -o $build/entitlements.plist $build/profile.plist
  cert=$(plutil -extract DeveloperCertificates.0 raw -o - $build/profile.plist | base64 -d | shasum | awk '{print toupper($1)}')
  codesign --force --sign "$cert" --entitlements $build/entitlements.plist --timestamp=none $app
fi
echo "built $app"
