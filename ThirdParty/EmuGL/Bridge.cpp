#include "Bridge.h"
#include "FrameBuffer.h"
#include "GraphicsDiagnostics.h"
#include "RenderThread.h"
#include "IOStream.h"
#include "emugl/common/mutex.h"
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>
#include <new>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <string>
extern bool ae_backend_init();
namespace {
std::mutex frameLock;
emugl::Mutex renderLock;
std::vector<uint8_t> frame;
bool havePost = false, pending = false, initialized = false, attempted = false;
char initializationError[256] = "GPU initialization has not completed";
unsigned width, height, dirtyFirst, dirtyLast;
std::vector<uint8_t> rgba;

std::mutex diagnosticsLock;
std::string diagnosticsText;
std::atomic<uint64_t> diagnosticsSequence(0);
const std::chrono::steady_clock::time_point diagnosticsStarted =
        std::chrono::steady_clock::now();
const size_t kDiagnosticsLimit = 512 * 1024;

void appendDiagnosticsLine(const char *stage, const char *body) {
    if (!aeGraphicsDiagTraceEnabled()) return;
    const uint64_t sequence = diagnosticsSequence.fetch_add(1) + 1;
    const uint64_t ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - diagnosticsStarted).count());
    char line[1024];
    const int written = std::snprintf(
            line, sizeof(line), "#%06llu +%llums %-22s %s\n",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(ms),
            stage ? stage : "?", body ? body : "");
    if (written <= 0) return;
    const size_t bytes = std::min<size_t>(
            static_cast<size_t>(written), sizeof(line) - 1);
    std::lock_guard<std::mutex> lock(diagnosticsLock);
    diagnosticsText.append(line, bytes);
    if (diagnosticsText.size() > kDiagnosticsLimit) {
        size_t drop = diagnosticsText.size() - kDiagnosticsLimit;
        size_t newline = diagnosticsText.find('\n', drop);
        diagnosticsText.erase(0, newline == std::string::npos ? drop : newline + 1);
    }
}

inline void hashByte(uint64_t *hash, uint8_t value) {
    *hash ^= value;
    *hash *= 1099511628211ULL;
}

void post(void *, int w, int h, int direction, int format, int type, unsigned char *pixels) {
    (void)direction; // ColorBuffer blit already converts GL coordinates to gralloc top-left rows.
    if (w != int(width) || h != int(height) || format != 0x1908 || type != 0x1401 || !pixels) return;
    const bool visualizeAlpha = aeGraphicsDiagEnabled("AE_DIAG_VISUALIZE_ALPHA");
    const bool forceFullFrame =
            visualizeAlpha || aeGraphicsDiagEnabled("AE_DIAG_FORCE_FULL_HOST_FRAME");
    static std::atomic<uint64_t> postOrdinal(0);
    const uint64_t ordinal = postOrdinal.fetch_add(1) + 1;
    const bool traceSample = aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal);
    if (traceSample) {
        aeGraphicsDiagLog("BRIDGE_POST",
                          "frame=%llu size=%dx%d visualizeAlpha=%d forceFull=%d",
                          static_cast<unsigned long long>(ordinal), w, h,
                          visualizeAlpha, forceFullFrame);
        aeGraphicsDiagPixels("BRIDGE_INPUT_RGBA", pixels, width, height,
                             static_cast<size_t>(width) * 4, false);
    }
    std::lock_guard<std::mutex> lock(frameLock);
    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *src = pixels + size_t(y) * width * 4;
        uint8_t *previous = rgba.data() + size_t(y) * width * 4;
        if (!forceFullFrame && havePost &&
            !memcmp(previous, src, size_t(width) * 4)) continue;
        memcpy(previous, src, size_t(width) * 4);
        uint8_t *dst = frame.data() + size_t(y) * width * 4;
        for (unsigned x = 0; x < width; ++x) {
            if (visualizeAlpha) {
                uint8_t alpha = src[4*x+3];
                dst[4*x] = alpha; dst[4*x+1] = alpha;
                dst[4*x+2] = alpha; dst[4*x+3] = 255;
            } else {
                dst[4*x] = src[4*x+2]; dst[4*x+1] = src[4*x+1];
                dst[4*x+2] = src[4*x]; dst[4*x+3] = 255;
            }
        }
        if (!pending) { dirtyFirst = dirtyLast = y; pending = true; }
        else { dirtyFirst = std::min(dirtyFirst, y); dirtyLast = std::max(dirtyLast, y); }
    }
    if (forceFullFrame) {
        pending = true; dirtyFirst = 0; dirtyLast = height - 1;
    }
    if (traceSample) {
        aeGraphicsDiagPixels("BRIDGE_OUTPUT_BGRA", frame.data(), width, height,
                             static_cast<size_t>(width) * 4, true);
    }
    havePost = true;
}
class Stream final : public IOStream {
    int fd;
    bool handshake = false;
    std::vector<unsigned char> storage;
public:
    explicit Stream(int socket) : IOStream(4096), fd(socket) {}
    ~Stream() override { close(fd); }
    void forceStop() override { shutdown(fd, SHUT_RDWR); }
    void *allocBuffer(size_t size) override {
        if (size > 64u * 1024 * 1024) return nullptr;
        storage.resize(size); return storage.data();
    }
    int commitBuffer(size_t size) override {
        int result = writeFully(storage.data(), size);
        if (storage.capacity() > 1024 * 1024) { std::vector<unsigned char>().swap(storage); }
        return result;
    }
    int writeFully(const void *data, size_t size) override {
        auto p = static_cast<const unsigned char *>(data);
        while (size) {
#ifdef MSG_NOSIGNAL
            ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
#else
            ssize_t n = send(fd, p, size, 0);
#endif
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -1;
            p += n; size -= n;
        }
        return 0;
    }
    const unsigned char *readFully(void *data, size_t size) override {
        auto p = static_cast<unsigned char *>(data);
        while (size) {
            ssize_t n = recv(fd, p, size, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return nullptr;
            p += n; size -= n;
        }
        return static_cast<unsigned char *>(data);
    }
    const unsigned char *read(void *data, size_t *size) override {
        if (!handshake) {
            uint32_t flags;
            if (!readFully(&flags, sizeof(flags)) || flags != 0) return nullptr;
            handshake = true;
        }
        ssize_t n;
        do { n = recv(fd, data, *size, 0); } while (n < 0 && errno == EINTR);
        if (n <= 0) return nullptr;
        *size = n; return static_cast<unsigned char *>(data);
    }
};
struct Connection { int fd; RenderThread *thread; };
int transfer(void *opaque, void *data, size_t size, bool sending) {
    if (!opaque || size > 8192) return -4;
    int fd = static_cast<Connection *>(opaque)->fd;
    ssize_t n;
    do {
#ifdef MSG_NOSIGNAL
        n = sending ? send(fd, data, size, MSG_NOSIGNAL) : recv(fd, data, size, 0);
#else
        n = sending ? send(fd, data, size, 0) : recv(fd, data, size, 0);
#endif
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
    return n <= 0 && size ? -4 : int(n);
}
}

bool aeGraphicsDiagTraceEnabled() {
    return aeGraphicsDiagEnabled("AE_DIAG_TRACE_PIPELINE") ||
           aeGraphicsDiagEnabled("AE_DIAG_PIXEL_FINGERPRINTS");
}

bool aeGraphicsDiagPixelFingerprints() {
    return aeGraphicsDiagEnabled("AE_DIAG_PIXEL_FINGERPRINTS");
}

unsigned aeGraphicsDiagTraceEvery() {
    const char *value = getenv("AE_DIAG_TRACE_EVERY");
    if (!value || !*value) return 30;
    char *end = nullptr;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end || parsed < 1) return 30;
    return static_cast<unsigned>(std::min<unsigned long>(parsed, 600));
}

bool aeGraphicsDiagSample(uint64_t ordinal) {
    const unsigned every = aeGraphicsDiagTraceEvery();
    return ordinal == 1 || every == 1 || (ordinal % every) == 0;
}

void aeGraphicsDiagLog(const char *stage, const char *format, ...) {
    if (!aeGraphicsDiagTraceEnabled()) return;
    char body[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format ? format : "", args);
    va_end(args);
    appendDiagnosticsLine(stage, body);
}

void aeGraphicsDiagPixels(const char *stage,
                          const unsigned char *pixels,
                          unsigned w,
                          unsigned h,
                          size_t stride,
                          bool bgra) {
    if (!aeGraphicsDiagPixelFingerprints() || !pixels || !w || !h ||
        stride < static_cast<size_t>(w) * 4) return;

    uint64_t rgbaHash = 1469598103934665603ULL;
    uint64_t rgbHash = 1469598103934665603ULL;
    uint64_t alphaHash = 1469598103934665603ULL;
    uint64_t tileHashes[16];
    for (uint64_t &tile : tileHashes) tile = 1469598103934665603ULL;
    uint64_t alphaZero = 0, alphaPartial = 0, alphaFull = 0;
    unsigned alphaMin = 255, alphaMax = 0;

    for (unsigned y = 0; y < h; ++y) {
        const unsigned char *row = pixels + static_cast<size_t>(y) * stride;
        for (unsigned x = 0; x < w; ++x) {
            const unsigned char *p = row + static_cast<size_t>(x) * 4;
            const uint8_t r = bgra ? p[2] : p[0];
            const uint8_t g = p[1];
            const uint8_t b = bgra ? p[0] : p[2];
            const uint8_t a = p[3];
            hashByte(&rgbaHash, r); hashByte(&rgbaHash, g);
            hashByte(&rgbaHash, b); hashByte(&rgbaHash, a);
            hashByte(&rgbHash, r); hashByte(&rgbHash, g); hashByte(&rgbHash, b);
            hashByte(&alphaHash, a);
            const unsigned tx = std::min(3u, static_cast<unsigned>(
                    (static_cast<uint64_t>(x) * 4) / w));
            const unsigned ty = std::min(3u, static_cast<unsigned>(
                    (static_cast<uint64_t>(y) * 4) / h));
            uint64_t *tile = &tileHashes[ty * 4 + tx];
            hashByte(tile, r); hashByte(tile, g); hashByte(tile, b); hashByte(tile, a);
            alphaMin = std::min(alphaMin, static_cast<unsigned>(a));
            alphaMax = std::max(alphaMax, static_cast<unsigned>(a));
            if (a == 0) ++alphaZero;
            else if (a == 255) ++alphaFull;
            else ++alphaPartial;
        }
    }

    char tiles[16 * 5 + 1];
    size_t offset = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const int n = std::snprintf(tiles + offset, sizeof(tiles) - offset,
                                    "%04x%s",
                                    static_cast<unsigned>(tileHashes[i] & 0xffff),
                                    i == 15 ? "" : ".");
        if (n <= 0) break;
        offset += std::min<size_t>(static_cast<size_t>(n),
                                   sizeof(tiles) - offset - 1);
    }
    tiles[sizeof(tiles) - 1] = '\0';

    aeGraphicsDiagLog(
            stage,
            "size=%ux%u hashRGBA=%016llx hashRGB=%016llx hashA=%016llx "
            "alpha[min=%u max=%u zero=%llu partial=%llu full=%llu] tiles4x4=%s",
            w, h,
            static_cast<unsigned long long>(rgbaHash),
            static_cast<unsigned long long>(rgbHash),
            static_cast<unsigned long long>(alphaHash),
            alphaMin, alphaMax,
            static_cast<unsigned long long>(alphaZero),
            static_cast<unsigned long long>(alphaPartial),
            static_cast<unsigned long long>(alphaFull),
            tiles);
}

void aeGraphicsDiagComparePixels(const char *stage,
                                 const unsigned char *first,
                                 size_t firstStride,
                                 bool firstBgra,
                                 const unsigned char *second,
                                 size_t secondStride,
                                 bool secondBgra,
                                 unsigned w,
                                 unsigned h,
                                 bool flipSecondY) {
    if (!aeGraphicsDiagPixelFingerprints() || !first || !second || !w || !h)
        return;
    uint64_t bad = 0;
    unsigned minX = w, minY = h, maxX = 0, maxY = 0, maxDelta = 0;
    for (unsigned y = 0; y < h; ++y) {
        const unsigned char *aRow = first + static_cast<size_t>(y) * firstStride;
        const unsigned by = flipSecondY ? h - 1 - y : y;
        const unsigned char *bRow = second + static_cast<size_t>(by) * secondStride;
        for (unsigned x = 0; x < w; ++x) {
            const unsigned char *ap = aRow + static_cast<size_t>(x) * 4;
            const unsigned char *bp = bRow + static_cast<size_t>(x) * 4;
            const uint8_t av[4] = {
                firstBgra ? ap[2] : ap[0], ap[1],
                firstBgra ? ap[0] : ap[2], ap[3]
            };
            const uint8_t bv[4] = {
                secondBgra ? bp[2] : bp[0], bp[1],
                secondBgra ? bp[0] : bp[2], bp[3]
            };
            bool different = false;
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned delta = av[c] > bv[c] ? av[c] - bv[c] : bv[c] - av[c];
                maxDelta = std::max(maxDelta, delta);
                different |= delta != 0;
            }
            if (different) {
                ++bad;
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
        }
    }
    if (!bad) {
        aeGraphicsDiagLog(stage, "PASS pixels=%ux%u flipSecondY=%d", w, h, flipSecondY);
    } else {
        aeGraphicsDiagLog(stage,
                          "FAIL bad=%llu/%llu box=(%u,%u)-(%u,%u) maxDelta=%u flipSecondY=%d",
                          static_cast<unsigned long long>(bad),
                          static_cast<unsigned long long>(
                                  static_cast<uint64_t>(w) * h),
                          minX, minY, maxX, maxY, maxDelta, flipSecondY);
    }
}

extern "C" size_t ae_gpu_diagnostics_copy(char *out, size_t capacity) {
    std::lock_guard<std::mutex> lock(diagnosticsLock);
    if (!out || !capacity) return diagnosticsText.size();
    const size_t bytes = std::min(diagnosticsText.size(), capacity - 1);
    if (bytes) std::memcpy(out, diagnosticsText.data(), bytes);
    out[bytes] = '\0';
    return bytes;
}

extern "C" void ae_gpu_diagnostics_clear(void) {
    std::lock_guard<std::mutex> lock(diagnosticsLock);
    diagnosticsText.clear();
}

extern "C" void ae_gpu_diagnostics_mark(const char *label) {
    aeGraphicsDiagLog("USER_MARK", "%s", label ? label : "mark");
}

extern "C" void ae_gpu_diagnostics_note_bgra_frame(
        const uint8_t *pixels, size_t stride,
        unsigned frameWidth, unsigned frameHeight,
        unsigned x, unsigned y, unsigned w, unsigned h) {
    static std::atomic<uint64_t> hostOrdinal(0);
    const uint64_t ordinal = hostOrdinal.fetch_add(1) + 1;
    if (!aeGraphicsDiagTraceEnabled() || !aeGraphicsDiagSample(ordinal)) return;
    aeGraphicsDiagLog("IOS_CALLBACK",
                      "frame=%llu region=(%u,%u %ux%u) full=%ux%u stride=%zu",
                      static_cast<unsigned long long>(ordinal),
                      x, y, w, h, frameWidth, frameHeight, stride);
    aeGraphicsDiagPixels("IOS_CALLBACK_BGRA", pixels, frameWidth, frameHeight,
                         stride, true);
}

extern "C" const char *ae_gpu_last_error() { return initializationError; }
extern "C" void ae_gpu_set_error(const char *message) {
    std::snprintf(initializationError, sizeof(initializationError), "%s", message);
}
extern "C" int ae_gpu_init(unsigned w, unsigned h) {
    if (attempted) return initialized;
    attempted = true;
    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog(
                "GPU_INIT",
                "size=%ux%u every=%u pixels=%d cpuReverse=%d noFinish=%d "
                "discard=%d preserved=%d clearAttach=%d noImagePreserve=%d "
                "nearest=%d dropOrphan=%d fullHost=%d visualizeAlpha=%d",
                w, h, aeGraphicsDiagTraceEvery(),
                aeGraphicsDiagPixelFingerprints(),
                aeGraphicsDiagEnabled("AE_DIAG_CPU_REVERSE_BLIT"),
                aeGraphicsDiagEnabled("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH"),
                aeGraphicsDiagEnabled("AE_DIAG_ADVERTISE_DISCARD"),
                aeGraphicsDiagEnabled("AE_DIAG_ADVERTISE_PRESERVED"),
                aeGraphicsDiagEnabled("AE_DIAG_CLEAR_PBUFFER_ON_ATTACH"),
                aeGraphicsDiagEnabled("AE_DIAG_DISABLE_EGLIMAGE_PRESERVED"),
                aeGraphicsDiagEnabled("AE_DIAG_NEAREST_COLORBUFFER_FILTER"),
                aeGraphicsDiagEnabled("AE_DIAG_DROP_EGLIMAGE_ON_ORPHAN"),
                aeGraphicsDiagEnabled("AE_DIAG_FORCE_FULL_HOST_FRAME"),
                aeGraphicsDiagEnabled("AE_DIAG_VISUALIZE_ALPHA"));
    }
    if (!w || !h || w > 2048 || h > 2048) {
        ae_gpu_set_error("Invalid GPU framebuffer dimensions"); return 0;
    }
    if (!ae_backend_init()) return 0;
    try {
        width = w; height = h;
        frame.resize(size_t(w) * h * 4);
        rgba.resize(frame.size(), 0xff);
        // A first all-white frame still needs a full presentation.
        pending = true; dirtyFirst = 0; dirtyLast = h - 1;
        for (size_t i = 3; i < frame.size(); i += 4) frame[i] = 255;
        if (!FrameBuffer::initialize(w, h)) return 0;
        FrameBuffer::getFB()->setPostCallback(post, nullptr);
        initialized = true;
        initializationError[0] = '\0';
        return 1;
    } catch (const std::bad_alloc&) {
        ae_gpu_set_error("Insufficient memory while initializing GPU");
        return 0;
    }
}
extern "C" void *ae_gpu_open() {
    if (!initialized) return nullptr;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return nullptr;
    int bufferBytes = 64 * 1024;
    for (int fd : sockets) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    }
    fcntl(sockets[0], F_SETFD, FD_CLOEXEC); fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
    fcntl(sockets[0], F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    setsockopt(sockets[1], SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    Stream *stream = new Stream(sockets[1]);
    RenderThread *thread = RenderThread::create(stream, &renderLock);
    if (!thread || !thread->start()) {
        if (thread) delete thread; else delete stream;
        close(sockets[0]); return nullptr;
    }
    return new Connection{sockets[0], thread};
}
extern "C" void ae_gpu_close(void *opaque) {
    auto c = static_cast<Connection *>(opaque);
    if (!c) return;
    shutdown(c->fd, SHUT_RDWR); close(c->fd);
    c->thread->forceStop(); c->thread->wait(nullptr);
    delete c->thread; delete c;
}
extern "C" int ae_gpu_fd(void *stream) { return stream ? static_cast<Connection *>(stream)->fd : -1; }
extern "C" int ae_gpu_send(void *s, const void *p, size_t n) { return transfer(s, const_cast<void *>(p), n, true); }
extern "C" int ae_gpu_receive(void *s, void *p, size_t n) { return transfer(s, p, n, false); }
extern "C" unsigned ae_gpu_poll(void *opaque) {
    if (!opaque) return 4;
    pollfd p = {static_cast<Connection *>(opaque)->fd, POLLIN | POLLOUT, 0};
    if (poll(&p, 1, 0) < 0) return errno == EINTR ? 0 : 4;
    return ((p.revents & POLLIN) ? 1 : 0) | ((p.revents & POLLOUT) ? 2 : 0) |
           ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) ? 4 : 0);
}
extern "C" void ae_gpu_invalidate_frame() {
    std::lock_guard<std::mutex> lock(frameLock);
    if (havePost) { pending = true; dirtyFirst = 0; dirtyLast = height - 1; }
}
extern "C" int ae_gpu_frame_region(uint8_t *pixels, size_t size, unsigned *first, unsigned *rows) {
    std::lock_guard<std::mutex> lock(frameLock);
    if (!havePost || !pending || size != frame.size() || !pixels || !first || !rows) return 0;
    *first = dirtyFirst; *rows = dirtyLast - dirtyFirst + 1;
    size_t offset = size_t(dirtyFirst) * width * 4;
    memcpy(pixels + offset, frame.data() + offset, size_t(*rows) * width * 4);
    static std::atomic<uint64_t> regionOrdinal(0);
    const uint64_t ordinal = regionOrdinal.fetch_add(1) + 1;
    if (aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal)) {
        aeGraphicsDiagLog("QEMU_FRAME_REGION",
                          "frame=%llu first=%u rows=%u full=%ux%u",
                          static_cast<unsigned long long>(ordinal),
                          *first, *rows, width, height);
        aeGraphicsDiagPixels("QEMU_FRAME_BGRA", frame.data(), width, height,
                             static_cast<size_t>(width) * 4, true);
    }
    pending = false; return 1;
}
extern "C" int ae_gpu_frame(uint8_t *pixels, size_t size) {
    unsigned first, rows;
    return ae_gpu_frame_region(pixels, size, &first, &rows);
}
