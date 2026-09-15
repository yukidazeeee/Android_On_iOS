#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo_dir="$PWD"
[[ "$(uname -s)" == Darwin ]] || { echo 'iOS engine compilation requires macOS and Xcode 26.' >&2; exit 1; }
ios_sdk_version="$(xcrun --sdk iphoneos --show-sdk-version)"
[[ "${ios_sdk_version%%.*}" -ge 26 ]] || { echo 'Select Xcode with iPhoneOS SDK 26 or newer.' >&2; exit 1; }
# Use the runner's CPUs, capped to avoid memory pressure during C++ builds.
build_jobs="${QEMU_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'QEMU_BUILD_JOBS must be positive.' >&2; exit 1; }
if [[ -z "${QEMU_BUILD_JOBS:-}" && "$build_jobs" -gt 4 ]]; then build_jobs=4; fi
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
ios_prefix="$repo_dir/build/ios-dependencies/sysroot-iOS-arm64"
deps_key="$(python3 scripts/ios_dependency_key.py)"
deps_stamp="$repo_dir/build/ios-dependencies/dependency-key"
if [[ -f "$deps_stamp" && "$(cat "$deps_stamp")" == "$deps_key" &&
      -f "$ios_prefix/lib/libEGL.dylib" && -f "$ios_prefix/lib/libGLESv2.dylib" &&
      -x "$ios_prefix/host/bin/pkg-config" &&
      -f build/ios-dependencies/build-iOS-arm64/BUILD_SUCCESS ]]; then
  echo "Reusing matching iOS dependency sysroot (including ANGLE)"
else
  rm -f "$deps_stamp"
  cd build/ios-dependencies
  bash build-minimal.sh -p ios -a arm64 -q "$repo_dir/ThirdParty/checkouts/qemu"
  cd "$repo_dir"
  python3 scripts/normalize_angle.py "$ios_prefix"
  printf '%s\n' "$deps_key" > "$deps_stamp"
fi
ios_sdk_path="$(xcrun --sdk iphoneos --show-sdk-path)"
ios_cc="$(xcrun --sdk iphoneos --find clang)"
ios_cxx="$(xcrun --sdk iphoneos --find clang++)"
python3 scripts/normalize_angle.py "$ios_prefix"
# Build wire-code generators with the macOS SDK, then compile the renderer for iOS.
cmake -S ThirdParty/EmuGL -B build/emugl-generator -G Ninja \
  -DCMAKE_C_COMPILER="$macos_cc" -DCMAKE_CXX_COMPILER="$macos_cxx" \
  -DCMAKE_OSX_SYSROOT="$macos_sdk_path" -DEMUGL_GENERATOR_ONLY=ON
cmake --build build/emugl-generator --target emugen --parallel "$build_jobs"
cmake -S ThirdParty/EmuGL -B build/emugl-ios -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$ios_sdk_path" -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_C_COMPILER="$ios_cc" -DCMAKE_CXX_COMPILER="$ios_cxx" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ios_prefix" \
  -DANGLE_GLES_LIBRARY:FILEPATH="$ios_prefix/lib/libGLESv2.dylib" \
  -DCMAKE_PREFIX_PATH="$ios_prefix" -DEMUGEN="$repo_dir/build/emugl-generator/emugen"
cmake --build build/emugl-ios --parallel "$build_jobs"
cmake --install build/emugl-ios
export PKG_CONFIG="$ios_prefix/host/bin/pkg-config"
export PKG_CONFIG_LIBDIR="$ios_prefix/lib/pkgconfig:$ios_prefix/share/pkgconfig"
export PKG_CONFIG_PATH=""
"$PKG_CONFIG" --exists androidemugl || { echo "Missing GLES renderer dependency" >&2; exit 1; }
ios_flags="-target arm64-apple-ios17.0 -isysroot $ios_sdk_path -I$ios_prefix/include"
ios_ldflags="-target arm64-apple-ios17.0 -isysroot $ios_sdk_path -L$ios_prefix/lib -Wl,-headerpad_max_install_names"
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
ninja -j "$build_jobs" libqemu-arm-softmmu.dylib
cd "$repo_dir"
python3 scripts/package_engine_frameworks.py build/qemu-ios/libqemu-arm-softmmu.dylib "$ios_prefix" build/ios-frameworks
