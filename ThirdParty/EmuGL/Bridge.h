#ifndef ANDROID_EMUGL_BRIDGE_H
#define ANDROID_EMUGL_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All entry points except the renderer's post callback run on the QEMU thread.
 * Streams return bytes transferred, -2 for backpressure and -4 for closure. */
int ae_gpu_init(unsigned width, unsigned height);
const char *ae_gpu_last_error(void);
void ae_gpu_set_error(const char *message);
void *ae_gpu_open(void);
void ae_gpu_close(void *stream);
int ae_gpu_fd(void *stream);
int ae_gpu_send(void *stream, const void *data, size_t size);
int ae_gpu_receive(void *stream, void *data, size_t size);
unsigned ae_gpu_poll(void *stream); /* 1 readable, 2 writable, 4 closed */
void ae_gpu_invalidate_frame(void);
/* Update a persistent BGRA backing store; untouched rows retain their pixels. */
int ae_gpu_frame_region(uint8_t *bgra, size_t size, unsigned *first, unsigned *rows);
int ae_gpu_frame(uint8_t *bgra, size_t size); /* latest frame, top-left origin */

/* Deep graphics diagnostics. The log is a bounded in-memory ring owned by
 * EmuGL, so the iOS app can retrieve it without depending on stderr capture. */
size_t ae_gpu_diagnostics_copy(char *out, size_t capacity);
void ae_gpu_diagnostics_clear(void);
void ae_gpu_diagnostics_mark(const char *label);
void ae_gpu_diagnostics_note_bgra_frame(const uint8_t *pixels,
                                        size_t stride,
                                        unsigned frame_width,
                                        unsigned frame_height,
                                        unsigned x,
                                        unsigned y,
                                        unsigned width,
                                        unsigned height);
#ifdef __cplusplus
}
// Internal C++ tracing helpers. These stay outside the C ABI block.
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
#endif
#endif
