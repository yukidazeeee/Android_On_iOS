#!/usr/bin/env python3
from pathlib import Path
import sys

path = Path("ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp")

if not path.is_file():
    print(f"ERROR: not found: {path}", file=sys.stderr)
    sys.exit(2)

data = path.read_bytes()

# Idempotent: do nothing when <atomic> is already present.
if b"#include <atomic>" in data:
    print(f"OK: {path} already includes <atomic>")
    sys.exit(0)

candidates = (
    (b"#include <stdio.h>\r\n", b"#include <stdio.h>\r\n#include <atomic>\r\n"),
    (b"#include <stdio.h>\n", b"#include <stdio.h>\n#include <atomic>\n"),
)

for needle, replacement in candidates:
    if needle in data:
        data = data.replace(needle, replacement, 1)
        path.write_bytes(data)
        break
else:
    print("ERROR: could not find '#include <stdio.h>' in FrameBuffer.cpp", file=sys.stderr)
    print("First include lines:", file=sys.stderr)
    for line in data.decode("utf-8", errors="replace").splitlines():
        if line.startswith("#include"):
            print(line, file=sys.stderr)
    sys.exit(3)

check = path.read_bytes()
if b"#include <atomic>" not in check:
    print("ERROR: insertion verification failed", file=sys.stderr)
    sys.exit(4)

print(f"OK: inserted #include <atomic> into {path}")
