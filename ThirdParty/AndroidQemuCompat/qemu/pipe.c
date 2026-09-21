/* Copyright (C) 2011 The Android Open Source Project
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
/* AndroidEmu: bounded v1 pipe MMIO port based on goldfish/pipe.c and
 * android/{hw-qemud,boot-properties}.c. Implements boot-properties and
 * pingpong, ADB and the SDK data modem; unknown services return errors.
 */
#include "qemu/osdep.h"
#include "android51.h"
#include "adb.h"
#include "gpu.h"
#include "gsm.h"
#include "system/reset.h"
#include "qemu/bswap.h"

#define GF_PIPE_LIMIT 64
#define GF_PIPE_BYTES 8192
typedef enum GFService { GF_CONNECT, GF_BOOT_PROPERTIES, GF_PINGPONG, GF_GSM, GF_ADB_ACCEPT, GF_ADB_START, GF_ADB_DATA, GF_GPU, GF_CLOSED } GFService;
typedef struct GFPipe {
    bool used;
    uint32_t channel, wanted, wakes;
    GFService service;
    uint8_t incoming[GF_PIPE_BYTES], outgoing[GF_PIPE_BYTES];
    unsigned received, queued;
    GFGSM gsm;
    Android51GpuStream *gpu;
} GFPipe;
typedef struct GFPipes {
    Android51State *board;
    MemoryRegion io;
    GFPipe pipes[GF_PIPE_LIMIT];
    uint32_t channel, address, length, wakes;
    uint64_t params;
    int32_t result;
    QEMUTimer *adb_timer;
    GFPipe *adb;
} GFPipes;
static GFPipes *active_pipes;
void gf_gpu_pipes_close(void)
{
    if (!active_pipes) { return; }
    for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) {
        GFPipe *p = &active_pipes->pipes[i];
        if (p->gpu) {
            android51_gpu_close(p->gpu);
            p->gpu = NULL;
            p->service = GF_CLOSED;
        }
    }
}
static bool pipe_writable(GFPipe *p)
{
    if (p->service == GF_CLOSED) { return false; }
    if (p->service == GF_GPU) { return (android51_gpu_poll(p->gpu) & 2) != 0; }
    if (p->service == GF_ADB_DATA) { return gf_adb_guest_writable() != 0; }
    if (p->service == GF_GSM) { return GF_PIPE_BYTES - p->queued >= 1024; }
    return p->queued < GF_PIPE_BYTES;
}
static void pipes_irq(GFPipes *s)
{
    bool wake = false;
    for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) { wake |= s->pipes[i].used && s->pipes[i].wakes != 0; }
    qemu_set_irq(s->board->irqs[15], wake);
}
static void pipe_wake(GFPipes *s, GFPipe *p)
{
    uint32_t ready = (p->queued ? 2 : 0) | (pipe_writable(p) ? 4 : 0);
    if (p->service == GF_CLOSED) { ready |= 1; }
    if (p->service == GF_GPU) {
        unsigned gpu = android51_gpu_poll(p->gpu);
        ready |= (gpu & 1 ? 2 : 0) | (gpu & 4 ? 1 : 0);
    }
    p->wakes |= (ready & p->wanted) | (ready & 1);
    p->wanted &= ~p->wakes;
    pipes_irq(s);
}
static GFPipe *pipe_find(GFPipes *s)
{
    for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) {
        if (s->pipes[i].used && s->pipes[i].channel == s->channel) { return &s->pipes[i]; }
    }
    return NULL;
}
static bool framed_reply(GFPipe *p, const char *data, size_t length)
{
    char header[5];
    if (length > 0xffff || length + 4 > sizeof(p->outgoing) - p->queued) { return false; }
    snprintf(header, sizeof(header), "%04x", (unsigned)length);
    memcpy(p->outgoing + p->queued, header, 4);
    memcpy(p->outgoing + p->queued + 4, data, length);
    p->queued += 4 + length;
    return true;
}
static bool boot_properties(GFPipes *s, GFPipe *p)
{
    static const char *properties[] = {
        "qemu.sf.lcd_density=160", "qemu.hw.mainkeys=0", "qemu.sf.fake_camera=none",
    };
    while (p->received >= 4) {
        unsigned length = 0;
        for (unsigned i = 0; i < 4; ++i) {
            int digit = g_ascii_xdigit_value(p->incoming[i]);
            if (digit < 0) { return false; }
            length = length * 16 + digit;
        }
        if (length > GF_PIPE_BYTES - 4) { return false; }
        if (p->received < length + 4) { break; }
        if (length != 4 || memcmp(p->incoming + 4, "list", 4)) { return false; }
        /* Build into a fresh queue only. Prevent repeated requests from overflowing it. */
        if (p->queued) { return false; }
        for (unsigned i = 0; i < ARRAY_SIZE(properties); ++i) {
            if (!framed_reply(p, properties[i], strlen(properties[i]))) { return false; }
        }
        const char *gles = android51_gpu_ready() ? "qemu.gles=1" : "qemu.gles=0";
        if (!framed_reply(p, gles, strlen(gles))) { return false; }
        char dimension[64];
        snprintf(dimension, sizeof(dimension), "qemu.sf.lcd_width=%u", s->board->width);
        if (!framed_reply(p, dimension, strlen(dimension))) { return false; }
        snprintf(dimension, sizeof(dimension), "qemu.sf.lcd_height=%u", s->board->height);
        if (!framed_reply(p, dimension, strlen(dimension))) { return false; }
        if (!framed_reply(p, "", 1)) { return false; }
        p->received -= length + 4;
        memmove(p->incoming, p->incoming + length + 4, p->received);
    }
    return true;
}
static int32_t pipe_send(GFPipes *s, GFPipe *p)
{
    uint8_t buffer[GF_PIPE_BYTES];
    unsigned offset = 0;
    if (s->length > sizeof(buffer) || !gf_guest_virtual(s->board, s->address, buffer, s->length, false)) { return -1; }
    if (p->service == GF_CONNECT) {
        while (offset < s->length) {
            if (p->received >= 255) { return -1; }
            uint8_t ch = buffer[offset++];
            p->incoming[p->received++] = ch;
            if (!ch) {
                if (!strcmp((char *)p->incoming, "pipe:qemud:boot-properties")) { p->service = GF_BOOT_PROPERTIES; }
                else if (!strcmp((char *)p->incoming, "pipe:qemud:gsm")) { p->service = GF_GSM; p->gsm.radio = true; }
                else if ((!strcmp((char *)p->incoming, "pipe:qemud:adb") ||
                          !strcmp((char *)p->incoming, "pipe:qemud:adb:5555")) && !s->adb) {
                    p->service = GF_ADB_ACCEPT; s->adb = p;
                }
                else if (!strcmp((char *)p->incoming, "pipe:pingpong")) { p->service = GF_PINGPONG; }
                else if (!strcmp((char *)p->incoming, "pipe:opengles") &&
                         (p->gpu = android51_gpu_open())) { p->service = GF_GPU; }
                else { p->service = GF_CLOSED; return -1; }
                p->received = 0;
                break;
            }
        }
    }
    if (p->service == GF_GPU) {
        int sent = android51_gpu_send(p->gpu, buffer + offset, s->length - offset);
        return sent >= 0 ? (int32_t)offset + sent : offset ? (int32_t)offset : sent;
    }
    if (p->service == GF_GSM) {
        while (offset < s->length) {
            if (sizeof(p->outgoing) - p->queued < 1024) { return offset ? (int32_t)offset : -2; }
            uint8_t ch = buffer[offset++];
            if (ch == '\r' || ch == '\n') {
                if (p->received) {
                    char response[1024];
                    p->incoming[p->received] = 0;
                    gf_gsm_command(&p->gsm, (char *)p->incoming, response);
                    size_t n = strlen(response);
                    memcpy(p->outgoing + p->queued, response, n); p->queued += n;
                    p->received = 0;
                }
            } else if (ch < 32 || ch > 126 || p->received >= 1023) {
                p->service = GF_CLOSED; return -4;
            } else { p->incoming[p->received++] = ch; }
        }
        return s->length;
    }
    if (p->service == GF_ADB_ACCEPT || p->service == GF_ADB_START) {
        const char *expected = p->service == GF_ADB_ACCEPT ? "accept" : "start";
        size_t needed = strlen(expected);
        while (offset < s->length && p->received < needed) {
            if (buffer[offset] != expected[p->received]) { p->service = GF_CLOSED; return -4; }
            p->incoming[p->received++] = buffer[offset++];
        }
        if (p->received == needed) {
            p->received = 0;
            if (p->service == GF_ADB_ACCEPT) {
                memcpy(p->outgoing, "ok", 2); p->queued = 2; p->service = GF_ADB_START;
            } else { p->service = GF_ADB_DATA; gf_adb_connect(true); }
        }
        if (offset < s->length && p->service != GF_ADB_DATA) { return -4; }
    }
    if (p->service == GF_ADB_DATA) {
        size_t written = gf_adb_guest_send(buffer + offset, s->length - offset);
        if (!written && s->length != offset) { return -2; }
        return offset + written;
    }
    if (p->service == GF_BOOT_PROPERTIES) {
        if (s->length - offset > sizeof(p->incoming) - p->received) { return -2; }
        memcpy(p->incoming + p->received, buffer + offset, s->length - offset);
        p->received += s->length - offset;
        if (!boot_properties(s, p)) { p->service = GF_CLOSED; return -4; }
    } else if (p->service == GF_PINGPONG) {
        if (s->length - offset > sizeof(p->outgoing) - p->queued) { return -2; }
        memcpy(p->outgoing + p->queued, buffer + offset, s->length - offset);
        p->queued += s->length - offset;
    }
    return s->length;
}
static void pipe_command(GFPipes *s, uint32_t command)
{
    GFPipe *p = pipe_find(s);
    s->result = -1;
    if (command == 1) {
        if (p || !s->channel) { return; }
        for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) {
            if (!s->pipes[i].used) {
                p = &s->pipes[i];
                memset(p, 0, sizeof(*p)); p->used = true; p->channel = s->channel;
                s->result = 0; return;
            }
        }
        s->result = -3; return;
    }
    if (!p) { return; }
    if (command == 2) { if (s->adb == p) { gf_adb_connect(false); s->adb = NULL; } android51_gpu_close(p->gpu); memset(p, 0, sizeof(*p)); s->result = 0; pipes_irq(s); return; }
    if (p->service == GF_CLOSED) { s->result = -4; return; }
    switch (command) {
    case 3: s->result = (p->queued ? 1 : 0) | (pipe_writable(p) ? 2 : 0);
        if (p->service == GF_GPU) { s->result |= android51_gpu_poll(p->gpu) & 1; }
        break;
    case 4: s->result = pipe_send(s, p); break;
    case 5: p->wanted |= 4; s->result = 0; break;
    case 6: {
        unsigned count = MIN(s->length, p->queued);
        if (!count) { s->result = s->length ? -2 : 0; break; }
        if (!gf_guest_virtual(s->board, s->address, p->outgoing, count, true)) { break; }
        p->queued -= count;
        memmove(p->outgoing, p->outgoing + count, p->queued);
        s->result = count;
        break;
    }
    case 7: p->wanted |= 2; s->result = 0; break;
    }
    pipe_wake(s, p);
}
static uint64_t pipes_read(void *opaque, hwaddr offset, unsigned size)
{
    GFPipes *s = opaque;
    switch (offset) {
    case 4: return (uint32_t)s->result;
    case 8:
        for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) {
            GFPipe *p = &s->pipes[i];
            if (p->used && p->wakes) {
                s->wakes = p->wakes; p->wakes = 0; pipes_irq(s); return p->channel;
            }
        }
        s->wakes = 0; return 0;
    case 0x14: return s->wakes;
    case 0x18: return (uint32_t)s->params;
    case 0x1c: return s->params >> 32;
    default: return 0;
    }
}
static void pipes_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    GFPipes *s = opaque;
    switch (offset) {
    case 0: pipe_command(s, value); break;
    case 8: s->channel = value; break;
    case 12: s->length = value; break;
    case 16: s->address = value; break;
    case 0x18: s->params = (s->params & 0xffffffff00000000ULL) | (uint32_t)value; break;
    case 0x1c: s->params = (s->params & UINT32_MAX) | (value << 32); break;
    case 0x20: {
        uint8_t params[24];
        if (!s->params || s->params > MACHINE(s->board)->ram_size - sizeof(params) ||
            address_space_read(&address_space_memory, s->params, MEMTXATTRS_UNSPECIFIED, params, sizeof(params)) != MEMTX_OK) { break; }
        uint32_t cmd = ldl_le_p(params + 12);
        if (cmd != 4 && cmd != 6) { break; }
        s->channel = ldl_le_p(params); s->length = ldl_le_p(params + 4); s->address = ldl_le_p(params + 8);
        pipe_command(s, cmd);
        stl_le_p(params + 16, s->result);
        address_space_write(&address_space_memory, s->params + 16, MEMTXATTRS_UNSPECIFIED, params + 16, 4);
        break;
    }
    }
}
static const MemoryRegionOps pipes_ops = {
    .read = pipes_read, .write = pipes_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};
static void adb_tick(void *opaque)
{
    GFPipes *s = opaque;
    for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) {
        GFPipe *gpu = &s->pipes[i];
        if (gpu->used && gpu->service == GF_GPU) {
            int count = android51_gpu_receive(gpu->gpu, gpu->outgoing + gpu->queued,
                                              sizeof(gpu->outgoing) - gpu->queued);
            if (count > 0) { gpu->queued += count; }
            pipe_wake(s, gpu);
        }
    }
    GFPipe *p = s->adb;
    if (gf_adb_take_disconnect() && p) {
        gf_adb_connect(false); p->service = GF_CLOSED; p->queued = 0;
        pipe_wake(s, p); s->adb = NULL; p = NULL;
    }
    if (p && p->service == GF_ADB_DATA) {
        p->queued += gf_adb_guest_receive(p->outgoing + p->queued, sizeof(p->outgoing) - p->queued);
        pipe_wake(s, p);
    }
    timer_mod(s->adb_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 5);
}
static void pipes_reset(void *opaque)
{
    GFPipes *s = opaque;
    gf_adb_connect(false); s->adb = NULL;
    for (unsigned i = 0; i < GF_PIPE_LIMIT; ++i) { android51_gpu_close(s->pipes[i].gpu); }
    memset(s->pipes, 0, sizeof(s->pipes));
    s->channel = s->address = s->length = s->wakes = s->result = 0;
    s->params = 0; pipes_irq(s);
}
void gf_pipes_init(Android51State *board)
{
    GFPipes *s = g_new0(GFPipes, 1);
    active_pipes = s;
    s->board = board;
    gf_adb_init();
    s->adb_timer = timer_new_ms(QEMU_CLOCK_REALTIME, adb_tick, s);
    timer_mod(s->adb_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 5);
    gf_map(board, &s->io, "android51.pipe", 0xff070000, &pipes_ops, s);
    gf_register(board, "qemu_pipe", -1, 0xff070000, 0x1000, 15, 1);
    qemu_register_reset(pipes_reset, s);
}
