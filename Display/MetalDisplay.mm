#import "MetalDisplay.h"
#import "Performance/RuntimeMetrics.h"
#include "FrameMailbox.hpp"
#include <memory>
#include <atomic>
#include <cmath>
#include <os/signpost.h>
#include <simd/simd.h>

@interface AEMetalDisplay () <MTKViewDelegate>
- (void)renderPass:(MTLRenderPassDescriptor *)pass drawable:(id<CAMetalDrawable>)drawable size:(CGSize)size force:(BOOL)force;
@end
@implementation AEMetalDisplay {
    std::unique_ptr<emu::FrameMailbox> _mailbox;
    id<MTLCommandQueue> _commands;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLTexture> _texture;
    dispatch_semaphore_t _available;
    dispatch_queue_t _renderQueue;
    std::atomic<bool> _dirty;
    uint32_t _guestWidth, _guestHeight;
    bool _hasFrame, _needsPresent;
    os_log_t _trace;
}
- (instancetype)initWithGuestWidth:(uint32_t)width height:(uint32_t)height error:(NSError **)error {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        if (error) *error = [NSError errorWithDomain:@"AndroidEmu.Metal" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Metal device unavailable"}];
        return nil;
    }
    self = [super initWithFrame:CGRectZero device:device];
    if (!self) return nil;
    try { _mailbox = std::make_unique<emu::FrameMailbox>(width, height); }
    catch (const std::exception& e) {
        if (error) *error = [NSError errorWithDomain:@"AndroidEmu.Metal" code:2 userInfo:@{NSLocalizedDescriptionKey: @(e.what())}];
        return nil;
    }
    _guestWidth = width; _guestHeight = height;
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.autoResizeDrawable = NO;
    self.framebufferOnly = YES; self.preferredFramesPerSecond = 60;
    self.clearColor = MTLClearColorMake(0, 0, 0, 1);
    _commands = [device newCommandQueue];
    id<MTLLibrary> library = [device newDefaultLibrary];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"framebufferVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"framebufferFragment"];
    descriptor.colorAttachments[0].pixelFormat = self.colorPixelFormat;
    _pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    MTLTextureDescriptor *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
    texture.storageMode = MTLStorageModeShared;
    // replaceRegion uploads arbitrary dirty rectangles from CPU memory.
    // Keep the upload texture out of AGX's compressed partial-block path.
    texture.allowGPUOptimizedContents = NO;
    texture.usage = MTLTextureUsageShaderRead;
    _texture = [device newTextureWithDescriptor:texture];
    if (!_commands || !_pipeline || !_texture) return nil;
    _available = dispatch_semaphore_create(1);
    _renderQueue = dispatch_queue_create("org.androidemu.metal-render", DISPATCH_QUEUE_SERIAL);
    _trace = os_log_create("org.androidemu.app", "display");
    _needsPresent = true;
    self.delegate = self;
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGSize bounds = self.bounds.size;
    if (bounds.width <= 0 || bounds.height <= 0) return;
    // Upscaling the drawable to Retina resolution adds no guest detail. Limit
    // fragment work to the guest pixel budget while keeping the window ratio.
    double scale = MIN(self.contentScaleFactor, std::sqrt(double(_guestWidth) * _guestHeight / (bounds.width * bounds.height)));
    CGSize pixels = CGSizeMake(MAX(1, std::floor(bounds.width * scale)), MAX(1, std::floor(bounds.height * scale)));
    if (!CGSizeEqualToSize(pixels, self.drawableSize)) self.drawableSize = pixels;
}
- (BOOL)submitPixels:(const uint8_t *)pixels length:(size_t)length stride:(size_t)stride
                   x:(uint32_t)x y:(uint32_t)y width:(uint32_t)width height:(uint32_t)height {
    if (!pixels || !_mailbox) return NO;
    os_signpost_id_t signpost = os_signpost_id_generate(_trace);
    os_signpost_interval_begin(_trace, signpost, "FrameCopy");
    BOOL success = YES;
    try { _mailbox->update(std::span(pixels, length), stride, {x,y,width,height}); }
    catch (const std::exception&) { success = NO; }
    os_signpost_interval_end(_trace, signpost, "FrameCopy");
    if (success) _dirty.store(true, std::memory_order_release);
    return success;
}
- (void)drawInMTKView:(MTKView *)view {
    // UIKit drawable acquisition stays on main. Copy/encode work is serialized
    // off main; there is at most one render/GPU job, never an unbounded backlog.
    if (!_needsPresent && !_dirty.load(std::memory_order_acquire)) return;
    if (dispatch_semaphore_wait(_available, DISPATCH_TIME_NOW) != 0) return;
    MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!pass || !drawable) { dispatch_semaphore_signal(_available); return; }
    CGSize size = view.drawableSize;
    BOOL force = _needsPresent; _needsPresent = false;
    dispatch_async(_renderQueue, ^{ @autoreleasepool { [self renderPass:pass drawable:drawable size:size force:force]; } });
}
- (void)renderPass:(MTLRenderPassDescriptor *)pass drawable:(id<CAMetalDrawable>)drawable size:(CGSize)size force:(BOOL)force {
    _dirty.store(false, std::memory_order_release);
    bool updated = _mailbox->consume([](void *context, const uint8_t *pixels, size_t stride, emu::DirtyRect dirty) {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)context;
        [texture replaceRegion:MTLRegionMake2D(dirty.x,dirty.y,dirty.width,dirty.height) mipmapLevel:0
                     withBytes:pixels + dirty.y * stride + dirty.x * 4 bytesPerRow:stride];
    }, (__bridge void *)_texture);
    _hasFrame |= updated;
    if (!force && !updated) { dispatch_semaphore_signal(_available); return; }
    if (!_hasFrame) { dispatch_semaphore_signal(_available); return; }
    id<MTLCommandBuffer> command = [_commands commandBuffer];
    if (!command) { dispatch_async(dispatch_get_main_queue(), ^{ self->_needsPresent = true; }); dispatch_semaphore_signal(_available); return; }
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { dispatch_async(dispatch_get_main_queue(), ^{ self->_needsPresent = true; }); dispatch_semaphore_signal(_available); return; }
    const double screenRatio = size.width / MAX(size.height, 1);
    const double guestRatio = double(_guestWidth) / _guestHeight;
    vector_float2 scale = screenRatio > guestRatio ? (vector_float2){float(guestRatio / screenRatio), 1} : (vector_float2){1, float(screenRatio / guestRatio)};
    [encoder setRenderPipelineState:_pipeline];
    [encoder setVertexBytes:&scale length:sizeof(scale) atIndex:0];
    [encoder setFragmentTexture:_texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    [command presentDrawable:drawable];
    dispatch_semaphore_t semaphore = _available;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError) {
            os_log_error(OS_LOG_DEFAULT, "AndroidEmu Metal command failed");
            dispatch_async(dispatch_get_main_queue(), ^{ self->_needsPresent = true; });
        }
        if (completed.status == MTLCommandBufferStatusCompleted) [self.runtimeMetrics presentedFrame];
        dispatch_semaphore_signal(semaphore);
    }];
    [command commit];
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; _needsPresent = true; }
- (void)setPaused:(BOOL)paused { [super setPaused:paused]; if (!paused) _needsPresent = true; }
- (void)didMoveToWindow { [super didMoveToWindow]; _needsPresent = true; }
- (uint64_t)coalescedUpdates { return _mailbox->stats().replaced; }
@end
