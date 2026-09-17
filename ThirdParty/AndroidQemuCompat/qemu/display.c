/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
*/
/* AndroidEmu: QEMU 10 port of goldfish/fb.c. RGB565 guest scanout to BGRA32 host surface,
 * guest-RAM dirty tracking, virtual-clock vsync independent of host UI.
 */
#include "qemu/osdep.h"
#include "android51.h"
#include "android51_host.h"
#include "gpu.h"
#include "ui/console.h"
#include "exec/ram_addr.h"
#include "system/reset.h"
#include "qemu/bswap.h"

#define GF_WIDTH (s->board->width)
#define GF_HEIGHT (s->board->height)
typedef struct GFDisplay {
    Android51State *board;
    MemoryRegion io;
    QemuConsole *console;
    QEMUTimer *vsync;
    uint32_t base, status, enabled;
    bool valid, blank, invalidate, base_pending, presented, gpu_presented;
    uint8_t *scanout;
    uint8_t *gpu_scanout; /* AndroidEmu sync/lifetime hardening v1 */
} GFDisplay;
static void display_irq(GFDisplay *s)
{
    qemu_set_irq(s->board->irqs[12], (s->status & s->enabled) != 0);
}
static void display_update(void *opaque)
{
    GFDisplay *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->console);
    if (s->board->gpu_ready) {
        if (s->blank && s->gpu_presented) {
            if (s->invalidate) {
                for (unsigned y = 0; y < GF_HEIGHT; ++y) {
                    uint8_t *row = surface_data(surface) + y * surface_stride(surface);
                    for (unsigned x = 0; x < GF_WIDTH; ++x) { stl_le_p(row + x * 4, 0xff000000); }
                }
                dpy_gfx_update(s->console, 0, 0, GF_WIDTH, GF_HEIGHT);
                android51_host_frame(surface_data(surface), surface_stride(surface), 0, 0, GF_WIDTH, GF_HEIGHT);
                s->invalidate = false;
            }
            return;
        }
        unsigned first, rows;
        const size_t tight_stride = (size_t)GF_WIDTH * 4;
        const size_t tight_size = tight_stride * GF_HEIGHT;
        if (ae_gpu_frame_region(s->gpu_scanout, tight_size, &first, &rows)) {
            for (unsigned y = first; y < first + rows; ++y) {
                memcpy(surface_data(surface) + (size_t)y * surface_stride(surface),
                       s->gpu_scanout + (size_t)y * tight_stride,
                       tight_stride);
            }
            s->gpu_presented = true;
            dpy_gfx_update(s->console, 0, first, GF_WIDTH, rows);
            android51_host_frame(surface_data(surface), surface_stride(surface), 0, first, GF_WIDTH, rows);
        }
        if (s->gpu_presented) { return; }
    }
    MemoryRegion *ram = MACHINE(s->board)->ram;
    uint64_t bytes = (uint64_t)GF_WIDTH * GF_HEIGHT * 2;
    int first = -1, last = -1;
    DirtyBitmapSnapshot *snapshot;
    uint8_t *source;
    if (!s->valid || s->base > memory_region_size(ram) || bytes > memory_region_size(ram) - s->base) { return; }
    if (!s->invalidate && !cpu_physical_memory_get_dirty(memory_region_get_ram_addr(ram) + s->base, bytes, DIRTY_MEMORY_VGA)) { return; }
    source = memory_region_get_ram_ptr(ram) + s->base;
    snapshot = memory_region_snapshot_and_clear_dirty(ram, s->base, bytes, DIRTY_MEMORY_VGA);
    for (uint32_t y = 0; y < GF_HEIGHT; ++y) {
        uint64_t offset = (uint64_t)y * GF_WIDTH * 2;
        if (s->invalidate || memory_region_snapshot_get_dirty(ram, snapshot, s->base + offset, GF_WIDTH * 2)) {
            uint8_t *previous = s->scanout + offset;
            const uint8_t *row = source + offset;
            /* Page flips invalidate addresses, not necessarily pixels. Compare
             * RGB565 rows before conversion/callback copies and Metal uploads. */
            if (s->presented && !memcmp(previous, row, GF_WIDTH * 2)) { continue; }
            memcpy(previous, row, GF_WIDTH * 2);
            uint8_t *dest = surface_data(surface) + y * surface_stride(surface);
            for (uint32_t x = 0; x < GF_WIDTH; ++x) {
                uint16_t pixel = s->blank ? 0 : lduw_le_p(row + x * 2);
                unsigned r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
                dest[x * 4] = (b << 3) | (b >> 2);
                dest[x * 4 + 1] = (g << 2) | (g >> 4);
                dest[x * 4 + 2] = (r << 3) | (r >> 2);
                dest[x * 4 + 3] = 255;
            }
            if (first < 0) { first = y; }
            last = y;
        }
    }
    g_free(snapshot);
    s->invalidate = false;
    s->presented = true;
    if (first >= 0) {
        dpy_gfx_update(s->console, 0, first, GF_WIDTH, last - first + 1);
        android51_host_frame(surface_data(surface), surface_stride(surface), 0, first, GF_WIDTH, last - first + 1);
    }
}
static void display_invalidate(void *opaque) {
    GFDisplay *s = opaque;
    s->invalidate = true;
    if (s->board->gpu_ready) { ae_gpu_invalidate_frame(); }
}
static const GraphicHwOps graphic_ops = { .gfx_update = display_update, .invalidate = display_invalidate };
static void display_tick(void *opaque)
{
    GFDisplay *s = opaque;
    display_update(s);
    s->status |= 1;
    if (s->base_pending) { s->status |= 2; s->base_pending = false; }
    display_irq(s);
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667);
}
static uint64_t display_read(void *opaque, hwaddr offset, unsigned size)
{
    GFDisplay *s = opaque;
    switch (offset) {
    case 0: return GF_WIDTH;
    case 4: return GF_HEIGHT;
    case 8: {
        uint32_t status = s->status & s->enabled;
        s->status &= ~status;
        display_irq(s);
        return status;
    }
    case 0x1c: return GF_WIDTH * 254 / 1600;
    case 0x20: return GF_HEIGHT * 254 / 1600;
    case 0x24: return 4; /* HAL_PIXEL_FORMAT_RGB_565; goldfish kernel uses 16 bpp. */
    default: return 0;
    }
}
static void display_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    GFDisplay *s = opaque;
    switch (offset) {
    case 12: s->enabled = value & 3; break;
    case 16:
        s->base = value;
        s->valid = true;
        s->invalidate = true;
        s->base_pending = true;
        s->status &= ~2U;
        break;
    case 20: /* SurfaceFlinger performs rotation; physical panel stays portrait. */ break;
    case 24:
        if (s->blank != (value != 0)) { s->presented = false; s->invalidate = true; }
        s->blank = value != 0;
        if (!s->blank && s->board->gpu_ready) { ae_gpu_invalidate_frame(); }
        break;
    }
    display_irq(s);
}
static const MemoryRegionOps display_ops = {
    .read = display_read, .write = display_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};
static void display_reset(void *opaque)
{
    GFDisplay *s = opaque;
    s->base = s->status = s->enabled = 0;
    s->valid = s->blank = s->base_pending = s->presented = false;
    s->gpu_presented = false;
    s->invalidate = true;
    if (s->board->gpu_ready) { ae_gpu_invalidate_frame(); }
    display_irq(s);
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667);
}
void gf_display_init(Android51State *board)
{
    GFDisplay *s = g_new0(GFDisplay, 1);
    s->board = board;
    s->scanout = g_malloc0((size_t)GF_WIDTH * GF_HEIGHT * 2);
    s->gpu_scanout = g_malloc0((size_t)GF_WIDTH * GF_HEIGHT * 4);
    s->console = graphic_console_init(NULL, 0, &graphic_ops, s);
    qemu_console_resize(s->console, GF_WIDTH, GF_HEIGHT);
    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, display_tick, s);
    memory_region_set_log(MACHINE(board)->ram, true, DIRTY_MEMORY_VGA);
    gf_map(board, &s->io, "android51.fb", 0xff040000, &display_ops, s);
    gf_register(board, "goldfish_fb", 0, 0xff040000, 0x1000, 12, 1);
    board->display_state = s;
    qemu_register_reset(display_reset, s);
}
