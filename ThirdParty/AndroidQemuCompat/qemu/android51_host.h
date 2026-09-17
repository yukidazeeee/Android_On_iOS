/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ANDROID51_HOST_H
#define ANDROID51_HOST_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ANDROID51_HOST_ABI 1
/* Callbacks are synchronous on QEMU threads, never UIKit's thread. Borrowed
 * pixel/audio/log buffers are valid only for the duration of the callback. */
typedef struct Android51Event { uint16_t type, code; int32_t value; } Android51Event;
typedef struct Android51Host {
    uint32_t abi, size;
    void *opaque;
    void (*frame)(void *, const uint8_t *, size_t, uint32_t, uint32_t, uint32_t, uint32_t);
    size_t (*input)(void *, Android51Event *, size_t);
    void (*pcm)(void *, const uint8_t *, size_t);
    void (*serial)(void *, const uint8_t *, size_t);
    void (*state)(void *, int); /* 1: initialized, 2: paused, 3: resumed, 4: stopped; 5: disk flush failed */
} Android51Host;
/* Exactly one lifecycle per process. Run on an owned thread. Returns -1 for
 * invalid ABI/repeated invocation; QEMU fatal initialization errors can exit. */
/* Call on the same owned thread before run; GPU failure returns without exiting
 * the embedding app. Dimensions must match the machine options. */
int android51_host_prepare_graphics(unsigned width, unsigned height, char *error, size_t capacity);
int android51_host_run(int argc, char **argv, const Android51Host *host);
void android51_host_pause(bool paused);
void android51_host_stop(void);
/* Cached TCG metrics: 0=used bytes, 1=capacity bytes, 2=TB flush count. */
uint64_t android51_host_metric(unsigned index);
bool android51_tcg_set_region(void *rw, void *rx, size_t bytes);

/* Deep graphics pipeline diagnostics exposed by the embedded framework. */
size_t android51_host_graphics_diagnostics(char *out, size_t capacity);
void android51_host_clear_graphics_diagnostics(void);
void android51_host_graphics_mark(const char *label);
void android51_host_graphics_note_frame(const uint8_t *pixels,
                                        size_t stride,
                                        uint32_t frame_width,
                                        uint32_t frame_height,
                                        uint32_t x,
                                        uint32_t y,
                                        uint32_t width,
                                        uint32_t height);
/* Internal device hooks; callbacks remain installed until CPUs are stopped. */
void android51_host_frame(const uint8_t *, size_t, uint32_t, uint32_t, uint32_t, uint32_t);
void android51_host_pcm(const uint8_t *, size_t);
void android51_host_serial(const uint8_t *, size_t);
#ifdef __cplusplus
}
#endif
#endif
