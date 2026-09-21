# EmuGL source provenance

Subset of AOSP `platform/external/qemu`, revision
`e6aef36e024c3265ff8103f8d2265dd235851ef4`, directory
`distrib/android-emugl`.

Upstream: https://android.googlesource.com/platform/external/qemu/+/e6aef36e024c3265ff8103f8d2265dd235851ef4/distrib/android-emugl/

Original copyright and per-file license notices are retained. Apache 2.0
license text is in LICENSE; consult individual files for other notices.

Local integration uses GPU/CMakeLists.txt, generated decoder bounds/alignment
checks, headless EGL pbuffers, bounded color buffers and callback presentation.
Apple EGL native display types use pointer width. The desktop window/server
sources are retained as reference and are not built by the iOS target.

ANGLE is fetched separately at the revision in dependencies.lock.json; this
source subset is not ANGLE and is not a complete modern emulator GPU stack.
