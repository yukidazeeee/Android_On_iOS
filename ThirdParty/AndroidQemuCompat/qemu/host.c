/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "android51.h"
#include "android51_host.h"
#include "adb.h"
#include "gpu.h"
#include "tcg/tcg.h"
#include "system/tcg.h"
#include "accel/tcg/tb-context.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/replay.h"
#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "qemu/rcu.h"
#include "block/block-global-state.h"

static Android51Host callbacks;
static int started, stop_requested, pause_requested;
static bool paused;
static uint64_t metrics[3];
static unsigned polls;
uint64_t android51_host_metric(unsigned index) { return index < 3 ? qatomic_read(&metrics[index]) : 0; }
size_t android51_host_graphics_diagnostics(char *out, size_t capacity)
{
    return ae_gpu_diagnostics_copy(out, capacity);
}
void android51_host_clear_graphics_diagnostics(void)
{
    ae_gpu_diagnostics_clear();
}
void android51_host_graphics_mark(const char *label)
{
    ae_gpu_diagnostics_mark(label);
}
void android51_host_graphics_note_frame(const uint8_t *pixels,
                                        size_t stride,
                                        uint32_t frame_width,
                                        uint32_t frame_height,
                                        uint32_t x,
                                        uint32_t y,
                                        uint32_t w,
                                        uint32_t h)
{
    ae_gpu_diagnostics_note_bgra_frame(
            pixels, stride, frame_width, frame_height, x, y, w, h);
}
static QEMUTimer *poll_timer;
static Android51Event pending[128];
static size_t pending_index, pending_count;

void android51_host_frame(const uint8_t *pixels, size_t stride, uint32_t x,
                          uint32_t y, uint32_t width, uint32_t height)
{
    if (callbacks.frame) { callbacks.frame(callbacks.opaque, pixels, stride, x, y, width, height); }
}
void android51_host_pcm(const uint8_t *data, size_t bytes)
{
    if (callbacks.pcm) { callbacks.pcm(callbacks.opaque, data, bytes); }
}
void android51_host_serial(const uint8_t *data, size_t bytes)
{
    if (callbacks.serial) { callbacks.serial(callbacks.opaque, data, bytes); }
}
void android51_host_pause(bool value) { qatomic_set(&pause_requested, value); }
void android51_host_stop(void) { qatomic_set(&stop_requested, 1); }
static void host_poll(void *opaque)
{
    if (qatomic_read(&stop_requested)) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
        return;
    }
    if ((polls++ % 200) == 0 && tcg_enabled()) {
        qatomic_set(&metrics[0], tcg_code_size());
        qatomic_set(&metrics[1], tcg_code_capacity());
        qatomic_set(&metrics[2], qatomic_read(&tb_ctx.tb_flush_count));
    }
    bool wanted = qatomic_read(&pause_requested);
    if (wanted != paused) {
        if (wanted) { vm_stop(RUN_STATE_PAUSED); } else { vm_start(); }
        paused = wanted;
        bool saved = !paused || bdrv_flush_all() == 0;
        if (callbacks.state) { callbacks.state(callbacks.opaque, !saved ? 5 : paused ? 2 : 3); }
    }
    if (pending_index == pending_count && callbacks.input) {
        pending_index = 0;
        pending_count = MIN(callbacks.input(callbacks.opaque, pending, ARRAY_SIZE(pending)), ARRAY_SIZE(pending));
    }
    while (pending_index < pending_count) {
        Android51Event *event = &pending[pending_index];
        if (!android51_input_event(event->type, event->code, event->value)) { break; }
        pending_index++;
    }
    /* Realtime timer remains live while the virtual clock is paused. */
    timer_mod(poll_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 5);
}
int android51_host_prepare_graphics(unsigned width, unsigned height, char *error, size_t capacity)
{
#ifdef AE_HAS_EMUGL
    if (ae_gpu_init(width, height)) { return 0; }
    if (error && capacity) { g_strlcpy(error, ae_gpu_last_error(), capacity); }
#else
    if (error && capacity) { g_strlcpy(error, "Engine was built without the GPU renderer", capacity); }
#endif
    return -2;
}
int android51_host_run(int argc, char **argv, const Android51Host *host)
{
    if (!host || host->abi != ANDROID51_HOST_ABI || host->size != sizeof(*host) ||
        argc < 1 || !argv || qatomic_cmpxchg(&started, 0, 1)) { return -1; }
    callbacks = *host;
    qemu_init(argc, argv); /* Returns holding BQL and replay mutex, as main.c expects. */
    poll_timer = timer_new_ms(QEMU_CLOCK_REALTIME, host_poll, NULL);
    timer_mod(poll_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    if (callbacks.state) { callbacks.state(callbacks.opaque, 1); }
    int result = qemu_main_loop();
    timer_del(poll_timer);
    timer_free(poll_timer);
    gf_adb_connect(false);
    vm_stop(RUN_STATE_SHUTDOWN);
    if (bdrv_flush_all() < 0) { result = -1; }
    qemu_cleanup(result);
    bql_unlock();
    replay_mutex_unlock();
#ifdef CONFIG_SHARED_LIBRARY_BUILD
    rcu_unregister_thread();
#endif
    if (callbacks.state) { callbacks.state(callbacks.opaque, 4); }
    memset(&callbacks, 0, sizeof(callbacks));
    return result;
}
