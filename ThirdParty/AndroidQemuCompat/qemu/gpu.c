/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Portable bounded transport. No GL, QEMU locks, sockets or guest pointers
 * cross this boundary. Compile independently for sanitizer tests. */
#include "gpu.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define GPU_QUEUE_BYTES 65536u
#define GPU_STREAM_LIMIT 64u
typedef struct GpuQueue {
    unsigned char bytes[GPU_QUEUE_BYTES];
    size_t head, count;
} GpuQueue;
struct Android51GpuStream {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    GpuQueue input, output;
    bool cancelled, finished;
    Android51GpuBackend backend;
};
static pthread_mutex_t backend_mutex = PTHREAD_MUTEX_INITIALIZER;
static Android51GpuBackend backend;
static unsigned streams;

static size_t minimum(size_t a, size_t b) { return a < b ? a : b; }
static size_t put(GpuQueue *q, const unsigned char *bytes, size_t size) {
    size = minimum(size, GPU_QUEUE_BYTES - q->count);
    size_t tail = (q->head + q->count) % GPU_QUEUE_BYTES;
    size_t first = minimum(size, GPU_QUEUE_BYTES - tail);
    if (size) {
        memcpy(q->bytes + tail, bytes, first);
        memcpy(q->bytes, bytes + first, size - first);
    }
    q->count += size;
    return size;
}
static size_t take(GpuQueue *q, unsigned char *bytes, size_t size) {
    size = minimum(size, q->count);
    size_t first = minimum(size, GPU_QUEUE_BYTES - q->head);
    if (size) {
        memcpy(bytes, q->bytes + q->head, first);
        memcpy(bytes + first, q->bytes, size - first);
    }
    q->head = (q->head + size) % GPU_QUEUE_BYTES;
    q->count -= size;
    return size;
}
bool android51_gpu_configure(const Android51GpuBackend *candidate) {
    pthread_mutex_lock(&backend_mutex);
    bool valid = !streams && (!candidate || (candidate->abi == ANDROID51_GPU_ABI &&
        candidate->size == sizeof(*candidate) && candidate->run));
    if (valid) {
        if (candidate) backend = *candidate;
        else memset(&backend, 0, sizeof(backend));
    }
    pthread_mutex_unlock(&backend_mutex);
    return valid;
}
bool android51_gpu_ready(void) {
    pthread_mutex_lock(&backend_mutex);
    bool ready = backend.run != NULL;
    pthread_mutex_unlock(&backend_mutex);
    return ready;
}
static void *run_stream(void *opaque) {
    Android51GpuStream *s = opaque;
    s->backend.run(s->backend.opaque, s);
    pthread_mutex_lock(&s->mutex);
    s->finished = true;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
Android51GpuStream *android51_gpu_open(void) {
    pthread_mutex_lock(&backend_mutex);
    if (!backend.run || streams == GPU_STREAM_LIMIT) {
        pthread_mutex_unlock(&backend_mutex);
        return NULL;
    }
    Android51GpuStream *s = calloc(1, sizeof(*s));
    if (!s) { pthread_mutex_unlock(&backend_mutex); return NULL; }
    s->backend = backend;
    if (pthread_mutex_init(&s->mutex, NULL)) goto fail_alloc;
    if (pthread_cond_init(&s->changed, NULL)) goto fail_mutex;
    if (pthread_create(&s->thread, NULL, run_stream, s)) goto fail_cond;
    ++streams;
    pthread_mutex_unlock(&backend_mutex);
    return s;
fail_cond:
    pthread_cond_destroy(&s->changed);
fail_mutex:
    pthread_mutex_destroy(&s->mutex);
fail_alloc:
    free(s);
    pthread_mutex_unlock(&backend_mutex);
    return NULL;
}
void android51_gpu_close(Android51GpuStream *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mutex);
    s->cancelled = true;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->mutex);
    free(s);
    pthread_mutex_lock(&backend_mutex);
    --streams;
    pthread_mutex_unlock(&backend_mutex);
}
int android51_gpu_send(Android51GpuStream *s, const void *bytes, size_t size) {
    if (!s || (!bytes && size)) return -1;
    pthread_mutex_lock(&s->mutex);
    int result = -4;
    if (!s->cancelled && !s->finished) {
        result = (int)put(&s->input, bytes, size);
        if (!result && size) result = -2;
        pthread_cond_broadcast(&s->changed);
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
}
int android51_gpu_receive(Android51GpuStream *s, void *bytes, size_t size) {
    if (!s || (!bytes && size)) return -1;
    pthread_mutex_lock(&s->mutex);
    int result = (int)take(&s->output, bytes, size);
    if (!result && size) result = (s->cancelled || s->finished) ? -4 : -2;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return result;
}
unsigned android51_gpu_poll(Android51GpuStream *s) {
    if (!s) return 4;
    pthread_mutex_lock(&s->mutex);
    unsigned result = s->output.count ? 1 : 0;
    if (s->cancelled || s->finished) result |= 4;
    else if (s->input.count < GPU_QUEUE_BYTES) result |= 2;
    pthread_mutex_unlock(&s->mutex);
    return result;
}
int android51_gpu_stream_read(Android51GpuStream *s, void *bytes, size_t size) {
    if (!s || (!bytes && size)) return -1;
    if (!size) return 0;
    pthread_mutex_lock(&s->mutex);
    while (!s->input.count && !s->cancelled) pthread_cond_wait(&s->changed, &s->mutex);
    int result = s->cancelled ? 0 : (int)take(&s->input, bytes, size);
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return result;
}
bool android51_gpu_stream_write(Android51GpuStream *s, const void *bytes, size_t size) {
    if (!s || (!bytes && size)) return false;
    const unsigned char *cursor = bytes;
    pthread_mutex_lock(&s->mutex);
    while (size && !s->cancelled) {
        size_t written = put(&s->output, cursor, size);
        cursor += written;
        size -= written;
        pthread_cond_broadcast(&s->changed);
        if (size && !s->cancelled) pthread_cond_wait(&s->changed, &s->mutex);
    }
    bool result = !s->cancelled;
    pthread_mutex_unlock(&s->mutex);
    return result;
}
