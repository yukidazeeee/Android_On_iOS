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
extern bool ae_backend_init();
namespace {
std::mutex frameLock;
emugl::Mutex renderLock;
std::vector<uint8_t> frame;
bool havePost = false, pending = false, initialized = false, attempted = false;
char initializationError[256] = "GPU initialization has not completed";
unsigned width, height, dirtyFirst, dirtyLast;
std::vector<uint8_t> rgba;
void post(void *, int w, int h, int direction, int format, int type, unsigned char *pixels) {
    (void)direction; // ColorBuffer blit already converts GL coordinates to gralloc top-left rows.
    if (w != int(width) || h != int(height) || format != 0x1908 || type != 0x1401 || !pixels) return;
    const bool visualizeAlpha = aeGraphicsDiagEnabled("AE_DIAG_VISUALIZE_ALPHA");
    const bool forceFullFrame =
            visualizeAlpha || aeGraphicsDiagEnabled("AE_DIAG_FORCE_FULL_HOST_FRAME");
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
extern "C" const char *ae_gpu_last_error() { return initializationError; }
extern "C" void ae_gpu_set_error(const char *message) {
    std::snprintf(initializationError, sizeof(initializationError), "%s", message);
}
extern "C" int ae_gpu_init(unsigned w, unsigned h) {
    if (attempted) return initialized;
    attempted = true;
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
    pending = false; return 1;
}
extern "C" int ae_gpu_frame(uint8_t *pixels, size_t size) {
    unsigned first, rows;
    return ae_gpu_frame_region(pixels, size, &first, &rows);
}
