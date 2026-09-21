#!/usr/bin/env python3
"""Apply the isolated Android51 board overlay to precisely the pinned UTM tree."""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMIT = 'b44153a4b6aabf86edebf92199b14aec26e15d59'

def prepare(destination):
    destination = Path(destination).resolve()
    revision = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != COMMIT:
        raise ValueError(f'Expected UTM QEMU {COMMIT}, got {revision}')
    overlay = destination / 'hw/arm/android51'
    overlay.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / 'GPU/Renderer.h', overlay / 'gpu_renderer.h')
    for source in (ROOT / 'ThirdParty/AndroidQemuCompat/qemu').glob('*'):
        if source.suffix in ('.h', '.c', '.inc'):
            target = overlay / source.name
            if not target.exists() or source.read_bytes() != target.read_bytes():
                shutil.copyfile(source, target)
    additions = {
        'hw/arm/Kconfig': '\nconfig ANDROID51\n    bool\n    default y\n    depends on TCG && ARM\n    select SMC91C111\n',
        'hw/arm/meson.build': "\narm_ss.add(when: 'CONFIG_ANDROID51', if_true: files(\n  'android51/android51.c', 'android51/platform.c', 'android51/nand.c',\n  'android51/display.c', 'android51/events.c', 'android51/battery.c',\n  'android51/pipe.c', 'android51/audio.c', 'android51/host.c', 'android51/adb.c', 'android51/gpu.c'))\n",
    }
    for name, addition in additions.items():
        path = destination / name
        text = path.read_text()
        marker = '# AndroidEmu generated integration\n'
        if marker in text:
            text = text.split(marker)[0]
        updated = text + marker + addition
        if path.read_text() != updated:
            path.write_text(updated)
    # --without-default-devices excludes unrelated boards and virtual peripherals.
    config = destination / 'configs/devices/arm-softmmu/default.mak'
    if config.read_text() != 'CONFIG_ANDROID51=y\n':
        config.write_text('CONFIG_ANDROID51=y\n')
    # Migrate the early GPU export patch, which overlapped patch 0005 and made
    # repeated preparation fail. Keep each patch's reverse-check independent.
    symbols = destination / 'system/qemu.symbols'
    early_gpu_exports = '{\n  android51_gpu_start;\n  android51_gpu_stop;\n  android51_gpu_ready;\n'
    if symbols.read_text().startswith(early_gpu_exports):
        symbols.write_text(symbols.read_text().replace(early_gpu_exports, '{\n', 1))
    for patch in sorted((ROOT / 'ThirdParty/AndroidQemuCompat/patches').glob('*.patch')):
        # git apply is also usable on tar-extracted sources, but the revision check
        # above intentionally requires an auditable Git checkout.
        reverse = subprocess.run(['git', '-C', str(destination), 'apply', '--reverse', '--check', str(patch)], capture_output=True)
        if reverse.returncode == 0:
            continue
        subprocess.run(['git', '-C', str(destination), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(destination), 'apply', str(patch)], check=True)

if __name__ == '__main__':
    prepare(sys.argv[1])
