#ifndef ANDROIDEMU_GRAPHICS_DIAGNOSTICS_H
#define ANDROIDEMU_GRAPHICS_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

inline bool aeGraphicsDiagEnabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
}

// Deep renderer tracing is intentionally runtime-controlled. Event tracing is
// lightweight; pixel fingerprints perform synchronous readbacks and can change
// timing, so they are a separate switch.
bool aeGraphicsDiagTraceEnabled();
bool aeGraphicsDiagPixelFingerprints();
unsigned aeGraphicsDiagTraceEvery();
bool aeGraphicsDiagSample(uint64_t ordinal);
void aeGraphicsDiagLog(const char *stage, const char *format, ...);
void aeGraphicsDiagPixels(const char *stage,
                          const unsigned char *pixels,
                          unsigned width,
                          unsigned height,
                          size_t stride,
                          bool bgra);
void aeGraphicsDiagComparePixels(const char *stage,
                                 const unsigned char *first,
                                 size_t firstStride,
                                 bool firstBgra,
                                 const unsigned char *second,
                                 size_t secondStride,
                                 bool secondBgra,
                                 unsigned width,
                                 unsigned height,
                                 bool flipSecondY);

#endif
