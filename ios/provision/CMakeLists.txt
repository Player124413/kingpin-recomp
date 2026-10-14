# A placeholder app with Kingpin's bundle id and entitlements: building it with Xcode
# (-allowProvisioningUpdates) makes Xcode create the development provisioning profile that
# ios/build.sh device embeds. See ios/build.sh.
cmake_minimum_required(VERSION 3.20)
project(kpprovision LANGUAGES C)
set(CMAKE_SYSTEM_NAME iOS)
add_executable(Kingpin MACOSX_BUNDLE main.c)
set_target_properties(Kingpin PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER com.blounty.kingpin
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.blounty.kingpin
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${TEAM}
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
  XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${CMAKE_CURRENT_SOURCE_DIR}/../${ENTITLEMENTS}
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 17.0)
