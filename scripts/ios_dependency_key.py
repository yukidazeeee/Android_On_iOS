#!/usr/bin/env python3
"""Fingerprint the toolchain and inputs for the iOS dependency sysroot."""
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def fingerprint(root, toolchain):
    digest = hashlib.sha256(toolchain.encode())
    # Installed dylib IDs and pkg-config files contain absolute workspace paths.
    digest.update(str(root.resolve()).encode())
    for name in ('ThirdParty/dependencies.lock.json', 'scripts/prepare_ios_sysroot.py',
                 'scripts/prepare_angle.py', 'scripts/angle_metal_image.py', 'scripts/normalize_angle.py',
                 'ThirdParty/EmuGL/Bridge.cpp', 'ThirdParty/EmuGL/Bridge.h',
                 'ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/GraphicsDiagnostics.h',
                 'ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/ColorBuffer.cpp',
                 'ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp',
                 'ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp',
                 'scripts/build_qemu_ios.sh', 'scripts/ios_dependency_key.py'):
        digest.update(name.encode())
        digest.update((root / name).read_bytes())
    return digest.hexdigest()


if __name__ == '__main__':
    commands = [['xcodebuild', '-version'], ['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'],
                ['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'], ['uname', '-m'],
                ['brew', 'list', '--versions']]
    toolchain = '\n'.join(subprocess.check_output(command, text=True) for command in commands)
    print(fingerprint(ROOT, toolchain))
