#!/usr/bin/env python3
"""Build an unsigned iOS framework dependency closure; reject host dylibs."""
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

REQUIRED_ENGINE_SYMBOLS = frozenset({
    'android51_host_run', 'android51_host_pause', 'android51_host_stop',
    'android51_host_metric', 'android51_tcg_set_region',
    'android51_adb_connected', 'android51_adb_disconnect',
    'android51_adb_read', 'android51_adb_write',
})


def verify_engine_exports(path):
    # Mach-O C symbols have a leading underscore. Only defined external
    # symbols count: undefined imports cannot satisfy the application's ABI.
    exported = set(run('xcrun', 'nm', '-gUj', str(path)).splitlines())
    missing = sorted(name for name in REQUIRED_ENGINE_SYMBOLS if '_' + name not in exported)
    if missing:
        raise ValueError('QEMU framework is missing application exports: ' + ', '.join(missing))


def run(*args):
    return subprocess.check_output(args, text=True)

def package(engine, prefix, destination, runtime_libraries=()):
    engine, prefix, destination = (Path(p).resolve() for p in (engine, prefix, destination))
    closure, imports = {}, {}
    def visit(path, main=False):
        path = path.resolve(strict=True)
        if path in closure:
            return
        if not main and not path.is_relative_to(prefix):
            raise ValueError(f'Non-iOS dependency outside sysroot: {path}')
        name = 'AndroidQEMU' if main else re.sub(r'[^A-Za-z0-9_]', '_', path.name.removesuffix('.dylib'))
        if name in closure.values():
            raise ValueError('Framework name collision')
        if run('xcrun', 'lipo', '-archs', str(path)).strip() != 'arm64':
            raise ValueError(f'Expected thin arm64: {path}')
        commands = run('xcrun', 'vtool', '-show-build', str(path))
        if not re.search(r'platform\s+(IOS|2)\b', commands):
            raise ValueError(f'Expected iPhoneOS Mach-O: {path}')
        minimum = re.search(r'minos\s+(\d+)\.(\d+)', commands)
        if not minimum or tuple(map(int, minimum.groups())) > (17, 0):
            raise ValueError(f'Framework requires a newer iOS than 17.0: {path}')
        if main:
            verify_engine_exports(path)
        closure[path] = name
        deps = []
        for line in run('otool', '-L', str(path)).splitlines()[2:]:
            dep = line.strip().split(' (compatibility version', 1)[0]
            if dep.startswith(('/usr/lib/', '/System/Library/')):
                continue
            if dep.startswith('@rpath/') or dep.startswith('@loader_path/'):
                target = prefix / 'lib' / Path(dep).name
                if '.framework/' in dep:
                    target = target.with_name(target.name + '.dylib')
            else:
                target = Path(dep)
            target = target.resolve(strict=True)
            deps.append((dep, target))
            visit(target)
        imports[path] = deps
    visit(engine, True)
    # dlopen dependencies are absent from the engine's Mach-O load commands.
    # They still need platform checks, dependency relocation and IPA embedding.
    for library in runtime_libraries:
        visit(Path(library))
    destination.mkdir(parents=True, exist_ok=True)
    # Only replace framework names owned by this generated closure.
    for path, name in closure.items():
        folder = destination / (name + '.framework')
        if folder.exists(): shutil.rmtree(folder)
        folder.mkdir()
        binary = folder / name
        shutil.copy2(path, binary)
        subprocess.run(['codesign', '--remove-signature', str(binary)], capture_output=True)
        if subprocess.run(['codesign', '--verify', str(binary)], capture_output=True).returncode == 0:
            raise ValueError('Framework signature was not removed')
        subprocess.run(['install_name_tool', '-id', f'@rpath/{name}.framework/{name}', str(binary)], check=True)
        for original, target in imports[path]:
            other = closure[target]
            subprocess.run(['install_name_tool', '-change', original, f'@rpath/{other}.framework/{other}', str(binary)], check=True)
        if path == engine:
            verify_engine_exports(binary)
        (folder / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleExecutable': name, 'CFBundleIdentifier': 'org.androidemu.engine.' + name.replace('_', '-'),
            'CFBundlePackageType': 'FMWK', 'CFBundleShortVersionString': '1.0', 'CFBundleVersion': '1',
            'MinimumOSVersion': '17.0', 'CFBundleSupportedPlatforms': ['iPhoneOS']}))
    (destination / 'engine-manifest.json').write_text(json.dumps({'frameworks': sorted(closure.values())}, indent=2) + '\n')

if __name__ == '__main__':
    package(*sys.argv[1:4], runtime_libraries=sys.argv[4:])
