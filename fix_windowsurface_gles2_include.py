#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "62d1700cb7b41db756cfc6220bfd339cc965371b"
PATH = Path("ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp")
INCLUDE = "#include <GLES2/gl2.h>"

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
        "wrong HEAD. This fixer was prepared for "
        f"{EXPECTED_HEAD}, but current HEAD is {head}."
    )

if not PATH.is_file():
    die(f"missing source file: {PATH}")

src = PATH.read_text(encoding="utf-8")

if INCLUDE in src:
    print("OK: GLES2 core header is already included.")
    raise SystemExit(0)

required_tokens = [
    "s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);",
    "s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);",
    "AndroidEmu ColorBuffer -> PBuffer restore v3",
]
for token in required_tokens:
    if token not in src:
        die(f"expected V3 source token is missing: {token!r}")

needle = '''#include "GLESv2Dispatch.h"

#include <GLES/glext.h>
'''

replacement = '''#include "GLESv2Dispatch.h"

#include <GLES2/gl2.h>
#include <GLES/glext.h>
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
    die("git diff --check failed; original WindowSurface.cpp restored")

print("OK: added <GLES2/gl2.h> to WindowSurface.cpp")
print("This provides GL_FRAMEBUFFER_BINDING and GL_FRAMEBUFFER for the V3 ES2 path.")
print()
print("Review:")
print(f"  git diff --check -- {PATH}")
print(f"  git diff -- {PATH}")
