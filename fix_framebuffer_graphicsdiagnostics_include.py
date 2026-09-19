#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "381f60f55929ea9099a8c77c843c3c34ceed3b0c"
PATH = Path("ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp")
INCLUDE = '#include "GraphicsDiagnostics.h"'

def die(msg: str) -> None:
    print("ERROR: " + msg, file=sys.stderr)
    raise SystemExit(1)

try:
    head = subprocess.check_output(
        ["git", "rev-parse", "HEAD"],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()
except Exception as exc:
    die(f"not a git repository: {exc}")

print("HEAD=" + head)

if head != EXPECTED_HEAD:
    die(
        "wrong HEAD. This fixer was prepared only for "
        f"{EXPECTED_HEAD}, but current HEAD is {head}."
    )

if not PATH.is_file():
    die(f"missing source file: {PATH}")

src = PATH.read_text(encoding="utf-8")

if INCLUDE in src:
    print("OK: GraphicsDiagnostics.h is already included.")
    raise SystemExit(0)

required_tokens = [
    'aeGraphicsDiagEnabled("AE_DIAG_RESTORE_DEQUEUED_COLORBUFFER")',
    "aeGraphicsDiagTraceEnabled()",
    "AndroidEmu: dequeued ColorBuffer restore is diagnostic-only",
]
for token in required_tokens:
    if token not in src:
        die(f"expected V4 source token is missing: {token!r}")

needle = '''#include "EGLDispatch.h"
#include "GLESv1Dispatch.h"
#include "GLESv2Dispatch.h"
#include "NativeSubWindow.h"
'''

replacement = '''#include "EGLDispatch.h"
#include "GLESv1Dispatch.h"
#include "GLESv2Dispatch.h"
#include "GraphicsDiagnostics.h"
#include "NativeSubWindow.h"
'''

count = src.count(needle)
if count != 1:
    die(f"include anchor expected exactly 1 match, found {count}; no file changed")

new_src = src.replace(needle, replacement, 1)
PATH.write_text(new_src, encoding="utf-8")

try:
    subprocess.run(
        ["git", "diff", "--check", "--", str(PATH)],
        check=True,
    )
except subprocess.CalledProcessError:
    PATH.write_text(src, encoding="utf-8")
    die("git diff --check failed; original FrameBuffer.cpp restored")

print("OK: added GraphicsDiagnostics.h to FrameBuffer.cpp")
print("aeGraphicsDiagEnabled() is now declared through its defining header.")
