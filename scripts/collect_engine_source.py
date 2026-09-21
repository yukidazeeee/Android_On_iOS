#!/usr/bin/env python3
"""Ship actual prepared engine sources, build scripts, dependency sources/licenses.

Never include OS images or user data. Exclude intermediate binaries and VCS
metadata. This accompanies the project git archive, not a moving upstream URL.
"""
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]

def allowed(info):
    parts = Path(info.name).parts
    if any(p in ('.git', '__pycache__', 'utm_build', 'out') for p in parts):
        return None
    if Path(info.name).suffix in ('.o', '.a', '.dylib', '.so', '.pyc', '.img', '.apk'):
        return None
    return info

if __name__ == '__main__':
    output = ROOT / 'build/artifacts/AndroidEmu-engine-source.tar.gz'
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, 'w:gz') as archive:
        for relative in ['ThirdParty/checkouts/qemu', 'ThirdParty/checkouts/UTM', 'ThirdParty/AndroidQemuCompat',
                         'ThirdParty/EmuGL', 'GPU', 'ThirdParty/dependencies.lock.json', 'scripts', 'build/ios-dependencies/build-minimal.sh',
                         'build/ios-dependencies/build-iOS-arm64']:
            path = ROOT / relative
            if not path.exists():
                raise FileNotFoundError(f'Missing corresponding build input: {relative}')
            archive.add(path, arcname=relative, filter=allowed)
        manifest = ROOT / 'build/ios-frameworks/engine-manifest.json'
        if manifest.exists() and 'libEGL' in manifest.read_text():
            for relative in ['ThirdParty/checkouts/angle', 'ThirdParty/checkouts/depot_tools']:
                path = ROOT / relative
                if not path.is_dir():
                    raise FileNotFoundError(f'Missing GPU build input: {relative}')
                archive.add(path, arcname=relative, filter=allowed)
    print(output)
