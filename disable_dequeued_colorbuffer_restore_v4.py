#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "d154e851406e4ec99e4b6b1966155a476bd5f682"
PATH = Path("ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp")
MARKER = "AndroidEmu: dequeued ColorBuffer restore is diagnostic-only"

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
        "wrong HEAD. This modifier was prepared only for "
        f"{EXPECTED_HEAD}, but current HEAD is {head}."
    )

if not PATH.is_file():
    die(f"missing source file: {PATH}")

src = PATH.read_text(encoding="utf-8")

if MARKER in src:
    print("OK: dequeued-ColorBuffer restore is already diagnostic-only.")
    raise SystemExit(0)

old = '''    (*w).second->setColorBuffer((*c).second.cb);

    // Restore this particular BufferQueue slot's previous pixels before the
    // guest starts its next partial redraw. Without this, one host PBuffer is
    // reused across A/B/C ColorBuffers and untouched regions inherit pixels
    // from the wrong slot (or the magenta diagnostic poison).
    const bool restored = (*w).second->restoreColorBuffer();
    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("FB_RESTORE_WINDOW_CB",
                          "surface=%#x colorbuffer=%#x restored=%d",
                          p_surface, p_colorbuffer, restored);
    }
    return restored;
}
'''

new = '''    (*w).second->setColorBuffer((*c).second.cb);

    // AndroidEmu: dequeued ColorBuffer restore is diagnostic-only.
    //
    // Goldfish keeps one persistent host PBuffer across BufferQueue slot
    // changes. After swap/flush that PBuffer already contains the immediately
    // previous completed frame. Restoring the newly dequeued A/B/C slot here
    // can replace untouched regions with that slot's older contents and make
    // partial redraws oscillate between stale frames.
    //
    // Keep the original Goldfish behavior by default. The old V3 restore can
    // still be enabled explicitly for A/B testing.
    if (aeGraphicsDiagEnabled("AE_DIAG_RESTORE_DEQUEUED_COLORBUFFER")) {
        const bool restored = (*w).second->restoreColorBuffer();
        if (aeGraphicsDiagTraceEnabled()) {
            aeGraphicsDiagLog("FB_RESTORE_WINDOW_CB",
                              "surface=%#x colorbuffer=%#x restored=%d",
                              p_surface, p_colorbuffer, restored);
        }
        return restored;
    }

    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("FB_RESTORE_WINDOW_CB",
                          "surface=%#x colorbuffer=%#x skipped=previous-frame-pbuffer",
                          p_surface, p_colorbuffer);
    }
    return true;
}
'''

count = src.count(old)
if count != 1:
    die(f"expected current V3 restore block exactly once, found {count}; no file changed")

PATH.write_text(src.replace(old, new, 1), encoding="utf-8")

try:
    subprocess.run(
        ["git", "diff", "--check", "--", str(PATH)],
        check=True,
    )
except subprocess.CalledProcessError:
    PATH.write_text(src, encoding="utf-8")
    die("git diff --check failed; original FrameBuffer.cpp restored")

print("OK: default dequeued-slot restore disabled.")
print("Set AE_DIAG_RESTORE_DEQUEUED_COLORBUFFER=1 only to re-enable old V3 behavior.")
