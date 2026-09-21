/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ANDROID51_GPU_H
#define ANDROID51_GPU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ANDROID51_GPU_ABI 1
typedef struct Android51GpuStream Android51GpuStream;
typedef struct Android51GpuBackend {
    uint32_t abi, size;
    void *opaque;
    /* Runs on a dedicated thread. All EGL/context work belongs here.
     * Must return after stream cancellation. No QEMU or UIKit calls. */
    void (*run)(void *, Android51GpuStream *);
} Android51GpuBackend;
/* Configure only after real backend initialization succeeds, before boot.
 * The caller owns opaque until all streams close. NULL disables the backend. */
bool android51_gpu_configure(const Android51GpuBackend *);
bool android51_gpu_ready(void);
Android51GpuStream *android51_gpu_open(void);
void android51_gpu_close(Android51GpuStream *);
/* Guest side never waits: positive byte count, -2 backpressure, -4 closed,
 * -1 invalid arguments. poll bits: readable=1, writable=2, closed=4. */
int android51_gpu_send(Android51GpuStream *, const void *, size_t);
int android51_gpu_receive(Android51GpuStream *, void *, size_t);
unsigned android51_gpu_poll(Android51GpuStream *);
/* Worker side waits for data/space; cancellation wakes both directions.
 * read returns 0 on EOF, write returns false on cancellation. */
int android51_gpu_stream_read(Android51GpuStream *, void *, size_t);
bool android51_gpu_stream_write(Android51GpuStream *, const void *, size_t);
/* Embedded engine entry points. The renderer is optional at build time;
 * an engine without it returns an actionable initialization error. */
bool android51_gpu_start(uint32_t width, uint32_t height, bool metal,
    void *(*resolve)(void *, unsigned, const char *), void *resolve_context,
    void (*post)(void *, const uint8_t *, uint32_t, uint32_t), void *post_context,
    char *error, size_t error_capacity);
bool android51_gpu_stop(void);
void gf_gpu_pipes_close(void);
#ifdef __cplusplus
}
#endif
#endif
