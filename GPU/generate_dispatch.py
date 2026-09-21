"""Generate typed EGL dispatch assignments from the pinned EmuGL declaration."""
import re
import sys
from pathlib import Path

fields = re.findall(r'^\s+(egl\w+)_t\s+(egl\w+);', Path(sys.argv[1]).read_text(), re.M)
if not fields or any(type_name != name for type_name, name in fields):
    raise ValueError('Unexpected EGL dispatch declarations')
Path(sys.argv[2]).write_text(''.join(
    f'    s_egl.{name} = reinterpret_cast<{name}_t>(resolve(context, 0, "{name}"));\n'
    for _, name in fields))
