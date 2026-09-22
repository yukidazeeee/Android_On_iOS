#!/bin/bash
# Build generators for macOS, the renderer for iPhoneOS, and ANGLE/Metal.
set -euo pipefail
cd "$(dirname "$0")/.."
repo_dir="$PWD"
[[ "$(uname -s)" == Darwin ]] || { echo 'GPU iOS build requires macOS and Xcode.' >&2; exit 1; }
ios_prefix="${1:?Pass the iPhoneOS dependency prefix}"
unset SDKROOT CC CXX OBJC CFLAGS CXXFLAGS OBJCFLAGS CPPFLAGS LDFLAGS
cmake -S GPU -B build/gpu-host-tools -DAE_GPU_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(xcrun --sdk macosx --find clang)" \
  -DCMAKE_CXX_COMPILER="$(xcrun --sdk macosx --find clang++)" \
  -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)"
cmake --build build/gpu-host-tools --target ae_emugen --parallel 2
cmake -S GPU -B build/gpu-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_BUILD_TYPE=Release -DAE_GPU_TESTS=OFF \
  -DEMUGEN_EXECUTABLE="$repo_dir/build/gpu-host-tools/ae_emugen"
cmake --build build/gpu-ios --parallel "${GPU_BUILD_JOBS:-2}"

for reference in angle depot_tools; do
  python3 scripts/fetch_references.py --only "$reference"
done
# Keep the existing Python until prepare_angle_tools.sh bootstraps depot_tools.
export DEPOT_TOOLS_UPDATE=0
cd ThirdParty/checkouts/angle
python3 "$repo_dir/scripts/prepare_angle.py" "$PWD"
bash "$repo_dir/scripts/prepare_angle_tools.sh" "$PWD" "$repo_dir/ThirdParty/checkouts/depot_tools"
# Build wrappers must use the same initialized tools as ANGLE's build hooks.
export PATH="$PWD/third_party/depot_tools:$PATH"
export DEPOT_TOOLS_DIR="$PWD/third_party/depot_tools"
# Fail if dependency synchronization moved the locked ANGLE revision.
python3 "$repo_dir/scripts/prepare_angle.py" "$PWD"
gn gen out/ios --args='target_os="ios" target_cpu="arm64" target_environment="device" ios_deployment_target="17.0" ios_enable_code_signing=false is_component_build=false is_debug=false symbol_level=0 angle_enable_metal=true angle_enable_gl=false angle_enable_vulkan=false angle_enable_null=false angle_build_tests=false use_remoteexec=false'
autoninja -C out/ios libEGL libGLESv1_CM libGLESv2 -j "${GPU_BUILD_JOBS:-2}"
mkdir -p "$ios_prefix/lib"
for name in libEGL libGLESv1_CM libGLESv2; do
  # ANGLE's iOS target is ios_framework_bundle, unlike its macOS dylib target.
  test -f "out/ios/$name.framework/$name"
  cp "out/ios/$name.framework/$name" "$ios_prefix/lib/$name.dylib"
done
