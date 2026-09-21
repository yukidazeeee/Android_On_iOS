#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo_dir="$PWD"
[[ "$(uname -s)" == Darwin ]] || { echo 'iOS engine compilation requires macOS and Xcode 26.' >&2; exit 1; }
ios_sdk_version="$(xcrun --sdk iphoneos --show-sdk-version)"
[[ "${ios_sdk_version%%.*}" -ge 26 ]] || { echo 'Select Xcode with iPhoneOS SDK 26 or newer.' >&2; exit 1; }
macos_cc="$(xcrun --sdk macosx --find clang)"
macos_cxx="$(xcrun --sdk macosx --find clang++)"
macos_sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
# Keep native generators independent of inherited device SDK/compiler flags.
unset SDKROOT CC CXX OBJC CFLAGS CXXFLAGS OBJCFLAGS CPPFLAGS LDFLAGS
export CC_FOR_BUILD="$macos_cc" CXX_FOR_BUILD="$macos_cxx"
export CFLAGS_FOR_BUILD="-isysroot $macos_sdk_path"
export CXXFLAGS_FOR_BUILD="$CFLAGS_FOR_BUILD" LDFLAGS_FOR_BUILD="$CFLAGS_FOR_BUILD"
for reference in qemu UTM; do
  if [[ ! -d "ThirdParty/checkouts/$reference/.git" ]]; then python3 scripts/fetch_references.py --only "$reference"; fi
done
python3 scripts/prepare_qemu.py ThirdParty/checkouts/qemu
python3 scripts/prepare_ios_sysroot.py
export ANDROID51_UTM_ROOT="$repo_dir/ThirdParty/checkouts/UTM"
# UTM's generated build stays inside our ignored workspace build directory.
cd build/ios-dependencies
bash build-minimal.sh -p ios -a arm64 -q "$repo_dir/ThirdParty/checkouts/qemu"
cd "$repo_dir"
ios_prefix="$repo_dir/build/ios-dependencies/sysroot-iOS-arm64"
gpu_cflags=""
gpu_ldflags=""
gpu_libraries=()
if [[ "${ANDROIDEMU_GPU:-1}" == 1 ]]; then
  bash scripts/build_gpu_ios.sh "$ios_prefix"
  gpu_cflags="-DANDROID51_GPU_RENDERER=1"
  gpu_ldflags="$repo_dir/build/gpu-ios/libandroidemu_gpu.a -lc++"
  gpu_libraries=("$ios_prefix/lib/libEGL.dylib" "$ios_prefix/lib/libGLESv1_CM.dylib" "$ios_prefix/lib/libGLESv2.dylib")
fi
ios_sdk_path="$(xcrun --sdk iphoneos --show-sdk-path)"
ios_cc="$(xcrun --sdk iphoneos --find clang)"
ios_cxx="$(xcrun --sdk iphoneos --find clang++)"
export PKG_CONFIG="$ios_prefix/host/bin/pkg-config"
export PKG_CONFIG_LIBDIR="$ios_prefix/lib/pkgconfig:$ios_prefix/share/pkgconfig"
export PKG_CONFIG_PATH=""
ios_flags="-target arm64-apple-ios17.0 -isysroot $ios_sdk_path -I$ios_prefix/include $gpu_cflags"
ios_ldflags="-target arm64-apple-ios17.0 -isysroot $ios_sdk_path -L$ios_prefix/lib -Wl,-headerpad_max_install_names $gpu_ldflags"
mkdir -p build/qemu-ios
cd build/qemu-ios
"$repo_dir/ThirdParty/checkouts/qemu/configure" --prefix="$ios_prefix" \
  --cc="$ios_cc" --cxx="$ios_cxx" --cpu=aarch64 --cross-prefix= \
  --host-cc="$macos_cc -isysroot $macos_sdk_path" \
  --extra-cflags="$ios_flags" --extra-cxxflags="$ios_flags" --extra-ldflags="$ios_ldflags" \
  --target-list=arm-softmmu --without-default-devices --enable-shared-lib -Db_staticpic=true \
  --enable-ucontext --with-coroutine=libucontext --enable-slirp --enable-pixman \
  --disable-hvf --disable-hvf-private --disable-cocoa --disable-sdl --disable-gtk \
  --disable-coreaudio --disable-pvg --disable-dbus-display --disable-slirp-smbd --disable-spice --disable-vnc --disable-opengl \
  --disable-virglrenderer --disable-guest-agent --disable-tools --disable-user --disable-docs \
  --disable-debug-info --disable-werror --disable-capstone --disable-gnutls --disable-nettle \
  --disable-gcrypt --disable-curl --disable-libssh --disable-libnfs --disable-libusb --disable-usb-redir
ninja -j "${QEMU_BUILD_JOBS:-2}" libqemu-arm-softmmu.dylib
cd "$repo_dir"
python3 scripts/package_engine_frameworks.py build/qemu-ios/libqemu-arm-softmmu.dylib "$ios_prefix" build/ios-frameworks "${gpu_libraries[@]}"
