#!/usr/bin/env python3
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]

def embed(app):
    app = Path(app)
    # Empty cache filesystem metadata only; no kernel/system/userdata content.
    shutil.copy2(ROOT / 'build/cache-template.sparse', app / 'cache-template.sparse')
    source = ROOT / 'build/ios-frameworks'
    names = json.loads((source / 'engine-manifest.json').read_text())['frameworks']
    if 'AndroidQEMU' not in names:
        raise ValueError('Engine framework missing')
    destination = app / 'Frameworks'
    destination.mkdir(exist_ok=True)
    for name in names:
        if not name.replace('_', '').isalnum():
            raise ValueError('Invalid framework name')
        folder = source / (name + '.framework')
        if not (folder / name).is_file():
            raise ValueError('Incomplete engine dependency')
        shutil.copytree(folder, destination / folder.name, dirs_exist_ok=True)
    notices = app / 'EngineLicenses'
    notices.mkdir(exist_ok=True)
    for name in ['LICENSE', 'TCG-MIT.txt', 'LGPL-2.1.txt']:
        shutil.copy2(ROOT / 'ThirdParty/AndroidQemuCompat' / name, notices / name)
    shutil.copy2(ROOT / 'ThirdParty/EmuGL/LICENSE', notices / 'EmuGL-Apache-2.0.txt')
    if 'libEGL' in names:
        angle = ROOT / 'ThirdParty/checkouts/angle'
        if not (angle / 'LICENSE').is_file():
            raise FileNotFoundError('ANGLE license sources missing')
        for path in angle.rglob('*'):
            if path.is_file() and not path.is_symlink() and not {'.git', 'out'}.intersection(path.relative_to(angle).parts) and path.name.upper().startswith(('COPYING', 'LICENSE', 'LICENCE')):
                output = notices / 'ANGLE' / path.relative_to(angle)
                output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, output)
    # Include all installed dependency license texts and exact input revisions.
    shutil.copy2(ROOT / 'ThirdParty/dependencies.lock.json', notices)
    shutil.copy2(source / 'engine-manifest.json', notices)
    prefix = ROOT / 'build/ios-dependencies'
    for path in prefix.rglob('*'):
        if path.is_file() and not path.is_symlink() and path.name.upper().startswith(('COPYING', 'LICENSE', 'LICENCE')):
            relative = path.relative_to(prefix)
            output = notices / 'dependencies' / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, output)

if __name__ == '__main__':
    embed(sys.argv[1])
