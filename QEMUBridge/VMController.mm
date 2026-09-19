#import "VMController.h"
#import "NativeBridge.h"
#import "Display/MetalDisplay.h"
#import "Input/InputSurface.h"
#import "Audio/AudioOutput.h"
#import "Performance/RuntimeMetrics.h"
#include "ThirdParty/AndroidQemuCompat/qemu/android51_host.h"
#include "Network/GuestNetwork.hpp"
#include "Core/Text/UTF8.hpp"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <atomic>

@interface AEVMController ()
- (void)hostFrame:(const uint8_t *)pixels stride:(size_t)stride x:(uint32_t)x y:(uint32_t)y width:(uint32_t)width height:(uint32_t)height;
- (size_t)hostInput:(Android51Event *)events capacity:(size_t)capacity;
- (void)hostPCM:(const uint8_t *)data length:(size_t)length;
- (void)hostSerial:(const uint8_t *)data length:(size_t)length;
- (void)hostState:(int)state;
@end
static void frameCallback(void *ctx, const uint8_t *p, size_t s, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    [(__bridge AEVMController *)ctx hostFrame:p stride:s x:x y:y width:w height:h];
}
static size_t inputCallback(void *ctx, Android51Event *e, size_t n) { return [(__bridge AEVMController *)ctx hostInput:e capacity:n]; }
static void pcmCallback(void *ctx, const uint8_t *p, size_t n) { [(__bridge AEVMController *)ctx hostPCM:p length:n]; }
static void serialCallback(void *ctx, const uint8_t *p, size_t n) { [(__bridge AEVMController *)ctx hostSerial:p length:n]; }
static void stateCallback(void *ctx, int s) { [(__bridge AEVMController *)ctx hostState:s]; }
static std::string optionPath(NSString *path) {
    std::string result;
    for (char c : std::string(path.UTF8String)) { result += c; if (c == ',') result += ','; }
    return result;
}
static void setGraphicsDiagnostic(NSUserDefaults *defaults, NSString *key, const char *name) {
    if ([defaults boolForKey:key]) setenv(name, "1", 1);
    else unsetenv(name);
}
static void applyGraphicsDiagnosticEnvironment() {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    setGraphicsDiagnostic(defaults, @"graphics.diag.cpuReverseBlit", "AE_DIAG_CPU_REVERSE_BLIT");
    setGraphicsDiagnostic(defaults, @"graphics.diag.disableSharedImageFinish", "AE_DIAG_DISABLE_SHARED_IMAGE_FINISH");
    setGraphicsDiagnostic(defaults, @"graphics.diag.advertiseDiscard", "AE_DIAG_ADVERTISE_DISCARD");
    setGraphicsDiagnostic(defaults, @"graphics.diag.advertisePreserved", "AE_DIAG_ADVERTISE_PRESERVED");
    setGraphicsDiagnostic(defaults, @"graphics.diag.clearPbufferOnAttach", "AE_DIAG_CLEAR_PBUFFER_ON_ATTACH");
    setGraphicsDiagnostic(defaults, @"graphics.diag.disableEGLImagePreserved", "AE_DIAG_DISABLE_EGLIMAGE_PRESERVED");
    setGraphicsDiagnostic(defaults, @"graphics.diag.nearestColorBuffer", "AE_DIAG_NEAREST_COLORBUFFER_FILTER");
    setGraphicsDiagnostic(defaults, @"graphics.diag.dropEGLImageOnOrphan", "AE_DIAG_DROP_EGLIMAGE_ON_ORPHAN");
    setGraphicsDiagnostic(defaults, @"graphics.diag.forceFullHostFrame", "AE_DIAG_FORCE_FULL_HOST_FRAME");
    setGraphicsDiagnostic(defaults, @"graphics.diag.visualizeAlpha", "AE_DIAG_VISUALIZE_ALPHA");
    setGraphicsDiagnostic(defaults, @"graphics.diag.tracePipeline", "AE_DIAG_TRACE_PIPELINE");
    setGraphicsDiagnostic(defaults, @"graphics.diag.pixelFingerprints", "AE_DIAG_PIXEL_FINGERPRINTS");
    NSInteger traceEvery = [defaults integerForKey:@"graphics.diag.traceEvery"];
    if (traceEvery < 1 || traceEvery > 600) traceEvery = 30;
    char traceEveryText[16];
    snprintf(traceEveryText, sizeof(traceEveryText), "%ld", (long)traceEvery);
    setenv("AE_DIAG_TRACE_EVERY", traceEveryText, 1);
    fprintf(stderr,
            "AEGFXDIAG cpu=%d noFinish=%d discard=%d preserved=%d clearAttach=%d "
            "noImagePreserve=%d nearest=%d dropOrphan=%d fullHost=%d alpha=%d\n",
            getenv("AE_DIAG_CPU_REVERSE_BLIT") != nullptr,
            getenv("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH") != nullptr,
            getenv("AE_DIAG_ADVERTISE_DISCARD") != nullptr,
            getenv("AE_DIAG_ADVERTISE_PRESERVED") != nullptr,
            getenv("AE_DIAG_CLEAR_PBUFFER_ON_ATTACH") != nullptr,
            getenv("AE_DIAG_DISABLE_EGLIMAGE_PRESERVED") != nullptr,
            getenv("AE_DIAG_NEAREST_COLORBUFFER_FILTER") != nullptr,
            getenv("AE_DIAG_DROP_EGLIMAGE_ON_ORPHAN") != nullptr,
            getenv("AE_DIAG_FORCE_FULL_HOST_FRAME") != nullptr,
            getenv("AE_DIAG_VISUALIZE_ALPHA") != nullptr);
    fprintf(stderr, "AEGFXDIAG deep trace=%d pixels=%d every=%s\n",
            getenv("AE_DIAG_TRACE_PIPELINE") != nullptr,
            getenv("AE_DIAG_PIXEL_FINGERPRINTS") != nullptr,
            traceEveryText);
}
@implementation AEVMController {
    AEMetalDisplay *_display;
    AEInputSurface *_input;
    AEAudioOutput *_audio;
    AERuntimeMetrics *_metrics;
    AEADBClient *_adb;
    NSThread *_worker;
    NSLock *_logLock;
    NSMutableData *_log;
    NSString *_statusText;
    void *_library;
    int (*_prepareGraphics)(unsigned, unsigned, char *, size_t);
    int (*_run)(int, char **, const Android51Host *);
    void (*_pause)(bool);
    void (*_stop)(void);
    uint64_t (*_metric)(unsigned);
    bool (*_region)(void *, void *, size_t);
    size_t (*_graphicsDiagnostics)(char *, size_t);
    void (*_graphicsClear)(void);
    void (*_graphicsMark)(const char *);
    void (*_graphicsNoteFrame)(const uint8_t *, size_t, uint32_t, uint32_t,
                               uint32_t, uint32_t, uint32_t, uint32_t);
    BOOL _started, _stopped, _guestPaused;
    uint32_t _width, _height;
    UIBackgroundTaskIdentifier _saveTask;
}
- (instancetype)init {
    if ((self = [super init])) {
        _saveTask = UIBackgroundTaskInvalid;
        _metrics = [AERuntimeMetrics new]; _audio = [AEAudioOutput new];
        _logLock = [NSLock new]; _log = [NSMutableData data];
        _statusText = @"起動準備"; _width = 540; _height = 960;
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(background:) name:UIApplicationDidEnterBackgroundNotification object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(memoryWarning:) name:UIApplicationDidReceiveMemoryWarningNotification object:nil];
    }
    return self;
}
- (NSString *)enginePath {
    return [[[NSBundle mainBundle] privateFrameworksPath] stringByAppendingPathComponent:@"AndroidQEMU.framework/AndroidQEMU"];
}
- (BOOL)engineAvailable { return [[NSFileManager defaultManager] fileExistsAtPath:[self enginePath]]; }
- (AEADBClient *)adb { return _adb; }
- (BOOL)started { return _started; }
- (BOOL)stopped { return _stopped; }
- (BOOL)guestPaused { return _guestPaused; }
- (NSString *)statusText { return _statusText; }
- (NSString *)serialText {
    [_logLock lock]; NSData *copy = [_log copy]; [_logLock unlock];
    auto text = emu::logUTF8(static_cast<const uint8_t *>(copy.bytes), copy.length);
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"";
}
- (NSString *)graphicsDiagnosticsText {
    if (!_graphicsDiagnostics) return @"描画診断ログはまだ利用できません。";
    std::vector<char> buffer(512 * 1024 + 1);
    size_t bytes = _graphicsDiagnostics(buffer.data(), buffer.size());
    if (!bytes) return @"描画診断ログは空です。設定で詳細パイプライントレースを有効にして、Androidを起動し直してください。";
    bytes = MIN(bytes, buffer.size() - 1);
    return [[NSString alloc] initWithBytes:buffer.data()
                                   length:bytes
                                 encoding:NSUTF8StringEncoding] ?: @"描画診断ログのUTF-8変換に失敗しました。";
}
- (void)clearGraphicsDiagnostics { if (_graphicsClear) _graphicsClear(); }
- (void)markGraphicsDiagnostics { if (_graphicsMark) _graphicsMark("USER_SAW_NOISE"); }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (void)loadView {
    self.view = [[UIView alloc] initWithFrame:CGRectZero]; self.view.backgroundColor = UIColor.blackColor;
    NSError *error = nil;
    _display = [[AEMetalDisplay alloc] initWithGuestWidth:_width height:_height error:&error];
    if (!_display) { _statusText = error.localizedDescription ?: @"Metal初期化に失敗しました"; return; }
    _display.runtimeMetrics = _metrics;
    _input = [[AEInputSurface alloc] initWithGuestWidth:_width height:_height];
    UILongPressGestureRecognizer *menuGesture = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(openControls:)];
    menuGesture.numberOfTouchesRequired = 3;
    menuGesture.minimumPressDuration = 0.7;
    [_input addGestureRecognizer:menuGesture];
    for (UIView *view in @[_display, _input]) {
        view.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:view];
        [NSLayoutConstraint activateConstraints:@[[view.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
            [view.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
            [view.topAnchor constraintEqualToAnchor:self.view.topAnchor], [view.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]]];
    }
}
- (void)openControls:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateBegan) {
        [_input cancelTouches];
        if (self.showControls) self.showControls();
    }
}
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [_input cancelTouches];
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
}
- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    [_input cancelTouches];
}
- (BOOL)startWithImageDirectory:(NSString *)path
                            ramMiB:(uint32_t)ram
                          cacheMiB:(uint32_t)cache
                        panelWidth:(uint32_t)width
                       panelHeight:(uint32_t)height
                          apiLevel:(uint32_t)apiLevel {
    NSAssert([NSThread isMainThread], @"Launch must originate on UI thread");
    if (_started) { _statusText = @"再起動にはアプリを終了して開き直してください"; return NO; }
    if (!AEJITArenaReady()) { _statusText = @"先にJITを有効にしてください"; return NO; }
    if (ram < 512 || ram > 4096 || !(cache == 128 || cache == 192 || cache == 256)) {
        _statusText = @"非対応のメモリ設定です"; return NO;
    }
    const bool validPanel =
            (width == 360 && height == 640) ||
            (width == 480 && height == 854) ||
            (width == 540 && height == 960) ||
            (width == 720 && height == 1280);
    if (!validPanel) {
        _statusText = @"非対応のAndroid画面解像度です"; return NO;
    }
    const bool validApi =
            (apiLevel >= 14 && apiLevel <= 19) ||
            (apiLevel >= 21 && apiLevel <= 23);
    if (!validApi) {
        _statusText = @"非対応のAndroid APIレベルです"; return NO;
    }
    // Android 4.x compatibility: keep the kernel shipped with that exact
    // system image. The newer optional HIGHMEM kernel is reserved for 5/6;
    // pairing it with old 4.x ramdisks can change their expected kernel ABI.
    const bool android4Compat = apiLevel <= 19;
    ram = MIN(ram, 4080U); // ARMv7 Goldfish reserves its top 16 MiB for MMIO.
    NSString *kernelPath = [path stringByAppendingPathComponent:@"kernel"];
    if (android4Compat) {
        ram = MIN(ram, 760U);
    } else if (ram > 760) {
        kernelPath = [NSBundle.mainBundle pathForResource:@"goldfish-highmem" ofType:@"zImage"];
        if (!kernelPath) {
            _statusText = @"HIGHMEM対応カーネルが同梱されていません。最新版のIPAを使うか、メモリを760 MiB以下にしてください。";
            return NO;
        }
    }
    uint64_t available = AEAvailableMemory();
    // Leave headroom for the renderer, disk I/O and UIKit. Android 4.x is the
    // deliberate exception above: it is capped to the stock Goldfish lowmem
    // range so the image's own kernel can be retained.
    if (available && ((uint64_t)ram + 256) * (1ULL << 20) > available) {
        _statusText = @"指定したAndroidメモリと描画処理に必要な空きメモリが不足しています。設定でメモリ容量を減らしてください。";
        return NO;
    }
    // Use the arena actually prepared (possibly enlarged by the optional
    // entitlement), rather than the user's pre-preparation preference.
    cache = (uint32_t)((AEJITArenaSize() + (1U << 20) - 1) >> 20);
    // AndroidEmu workflow identity + fixed guest geometry v1
    //
    // VMConfiguration already defines a complete Android LCD resolution.
    // Do not reshape the guest LCD to the iPhone/iPad window ratio: doing so
    // can turn 360x640 into 360x480 on 4:3 iPads, clipping boot animations and
    // exposing old Android SystemUI to an unintended panel geometry.
    // MetalDisplay aspect-fits this stable guest panel to the host window.
    if (self.isViewLoaded && (height != _height || width != _width)) {
        self.view = nil; _display = nil; _input = nil;
    }
    _width = width; _height = height;
    [self loadViewIfNeeded];
    if (!_display) return NO;
    NSArray<NSString *> *disks = @[@"system", @"userdata", @"cache"];
    for (NSString *name in @[@"kernel", @"ramdisk.img", @"system.img", @"userdata.img", @"cache.img"]) {
        NSString *file = [path stringByAppendingPathComponent:name]; struct stat info;
        BOOL disk = [disks containsObject:[name stringByDeletingPathExtension]];
        uint64_t limit = disk ? 8ULL << 30 : 64ULL << 20;
        if (lstat(file.fileSystemRepresentation, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
            (uint64_t)info.st_size > limit || (disk && info.st_size % 4096)) {
            _statusText = [NSString stringWithFormat:@"保存イメージが不正です: %@", name]; return NO;
        }
    }
    applyGraphicsDiagnosticEnvironment();
    if (!_library) _library = dlopen([self enginePath].fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
    if (!_library) { const char *reason = dlerror(); _statusText = [NSString stringWithFormat:@"QEMU frameworkを読み込めません: %s", reason ?: "unknown loader error"]; return NO; }
    NSMutableArray<NSString *> *missing = [NSMutableArray array];
    auto resolve = [&](const char *name) -> void * {
        void *symbol = dlsym(_library, name);
        if (!symbol) [missing addObject:[NSString stringWithUTF8String:name]];
        return symbol;
    };
    _prepareGraphics = reinterpret_cast<decltype(_prepareGraphics)>(resolve("android51_host_prepare_graphics"));
    _run = reinterpret_cast<decltype(_run)>(resolve("android51_host_run"));
    _pause = reinterpret_cast<decltype(_pause)>(resolve("android51_host_pause"));
    _stop = reinterpret_cast<decltype(_stop)>(resolve("android51_host_stop"));
    _metric = reinterpret_cast<decltype(_metric)>(resolve("android51_host_metric"));
    _region = reinterpret_cast<decltype(_region)>(resolve("android51_tcg_set_region"));
    _graphicsDiagnostics = reinterpret_cast<decltype(_graphicsDiagnostics)>(
            resolve("android51_host_graphics_diagnostics"));
    _graphicsClear = reinterpret_cast<decltype(_graphicsClear)>(
            resolve("android51_host_clear_graphics_diagnostics"));
    _graphicsMark = reinterpret_cast<decltype(_graphicsMark)>(
            resolve("android51_host_graphics_mark"));
    _graphicsNoteFrame = reinterpret_cast<decltype(_graphicsNoteFrame)>(
            resolve("android51_host_graphics_note_frame"));
    for (const char *name : {"android51_adb_connected", "android51_adb_disconnect",
                            "android51_adb_read", "android51_adb_write"}) {
        resolve(name);
    }
    if (missing.count) {
        _statusText = [NSString stringWithFormat:@"QEMU frameworkに必要な関数がありません: %@。更新したIPAを再インストールしてください。",
                      [missing componentsJoinedByString:@", "]];
        return NO;
    }
    NSError *error = nil;
    if (![_audio startWithSampleRate:44100 error:&error]) { _statusText = error.localizedDescription ?: @"音声の初期化に失敗しました"; return NO; }
    if (!_region(AEJITWritableBase(), const_cast<void *>(AEJITExecutableBase()), AEJITArenaSize())) {
        [_audio stop]; _statusText = @"TCG領域の登録に失敗しました。アプリの再起動が必要です"; return NO;
    }
    std::string kernelArgs =
            "qemu=1 console=ttyS0 androidboot.console=ttyS0 "
            "androidboot.hardware=goldfish android.qemud=1";
    // Android 4.x used this explicit software-composition mode before the
    // GLES renderer work. Keep 5/6 on the current GPU path, but restore the
    // old Goldfish contract for API 14-19 to avoid SystemUI/HWUI regressions.
    if (apiLevel <= 19) {
        kernelArgs += " qemu.gles=0";
    }
    std::vector<std::string> args = {"AndroidEmu", "-machine", "android51,audiodev=audio,width=" + std::to_string(_width) + ",height=" + std::to_string(_height), "-cpu", "cortex-a8",
        "-m", std::to_string(ram), "-smp", "1", "-accel", "tcg,tb-size=" + std::to_string(cache) + ",split-wx=on",
        "-nodefaults", "-no-reboot", "-display", "none", "-serial", "null", "-monitor", "none",
        "-audiodev", "none,id=audio", "-nic", emu::guestNICOption(),
        "-kernel", kernelPath.UTF8String,
        "-initrd", [path stringByAppendingPathComponent:@"ramdisk.img"].UTF8String,
        "-append", kernelArgs};
    for (NSString *name in disks) {
        args.emplace_back("-drive");
        args.push_back("if=none,id=" + std::string(name.UTF8String) + ",format=raw,file=" +
            optionPath([path stringByAppendingPathComponent:[name stringByAppendingString:@".img"]]) +
            ",readonly=" + ([name isEqualToString:@"system"] ? "on" : "off"));
    }
    _adb = [[AEADBClient alloc] initWithEngineHandle:_library];
    _started = YES; _statusText = @"Androidを起動中";
    [UIApplication sharedApplication].idleTimerDisabled = YES;
    _worker = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            // Block retains controller and its views until every QEMU callback stops.
            std::vector<std::string> owned = args;
            std::vector<char *> argv;
            for (auto &arg : owned) argv.push_back(arg.data());
            argv.push_back(nullptr);
            Android51Host host = {ANDROID51_HOST_ABI, sizeof(Android51Host), (__bridge void *)self,
                frameCallback, inputCallback, pcmCallback, serialCallback, stateCallback};
            char graphicsError[256] = {};
            int result = self->_prepareGraphics(self->_width, self->_height, graphicsError, sizeof(graphicsError));
            NSString *startupError = nil;
            if (result != 0) {
                startupError = [NSString stringWithFormat:@"GPUの初期化に失敗しました: %s", graphicsError];
                NSData *message = [startupError dataUsingEncoding:NSUTF8StringEncoding];
                [self hostSerial:static_cast<const uint8_t *>(message.bytes) length:message.length];
            } else {
                result = self->_run((int)owned.size(), argv.data(), &host);
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_worker = nil;
                [self->_adb cancel];
                self->_stopped = YES; self->_display.paused = YES; self->_input.userInteractionEnabled = NO; [self->_audio stop];
                [UIApplication sharedApplication].idleTimerDisabled = NO;
                self->_statusText = startupError ?: (result == 0 ? @"停止しました。再起動にはアプリを開き直してください" : @"QEMUがエラーで停止しました");
            });
        }
    }];
    _worker.name = @"AndroidEmu.QEMU";
    _worker.qualityOfService = NSQualityOfServiceUserInitiated;
    _worker.stackSize = 2 * 1024 * 1024;
    [_worker start];
    return YES;
}
- (void)setGuestPaused:(BOOL)paused {
    if (!_started || _stopped || paused == _guestPaused) return;
    if (paused && _saveTask == UIBackgroundTaskInvalid) {
        __weak AEVMController *weakSelf = self;
        _saveTask = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Save Android disks" expirationHandler:^{ [weakSelf finishSaveTask]; }];
    }
    if (!paused) [self finishSaveTask];
    _guestPaused = paused;
    [_input cancelTouches]; _pause(paused); _display.paused = paused;
    if (paused) [_audio stop];
    else { NSError *error = nil; if (![_audio startWithSampleRate:44100 error:&error]) _statusText = error.localizedDescription ?: @"音声の初期化に失敗しました"; }
    [UIApplication sharedApplication].idleTimerDisabled = !paused;
}
- (void)stopGuest { if (_started && !_stopped) { [_input cancelTouches]; [_adb cancel]; _stop(); _statusText = @"停止中"; } }
- (void)sendGuestKey:(uint16_t)code pressed:(BOOL)pressed { if (_started && !_stopped) [_input sendHardwareKey:code == 172 ? 102 : code value:pressed ? 1 : 0]; }
- (NSDictionary<NSString *, NSNumber *> *)statistics {
    NSMutableDictionary *result = [[_metrics snapshot] mutableCopy];
    result[@"panelWidth"] = @(_width); result[@"panelHeight"] = @(_height);
    if (_metric) {
        result[@"tcgUsedMiB"] = @(_metric(0) / 1048576.0);
        result[@"tcgCapacityMiB"] = @(_metric(1) / 1048576.0);
        result[@"tbFlushCount"] = @(_metric(2));
    }
    result[@"coalescedUpdates"] = @([_display coalescedUpdates]); result[@"audioUnderruns"] = @([_audio underruns]); return result;
}
- (void)background:(NSNotification *)note { (void)note; [self setGuestPaused:YES]; }
- (void)memoryWarning:(NSNotification *)note { (void)note; [self setGuestPaused:YES]; _statusText = @"メモリ不足のため一時停止しました"; }
- (void)hostFrame:(const uint8_t *)p stride:(size_t)s x:(uint32_t)x y:(uint32_t)y width:(uint32_t)w height:(uint32_t)h {
    if (_graphicsNoteFrame) {
        _graphicsNoteFrame(p, s, _width, _height, x, y, w, h);
    }
    if ([_display submitPixels:p length:s * _height stride:s x:x y:y width:w height:h]) [_metrics receivedFrameBytes:(uint64_t)w * h * 4];
}
- (size_t)hostInput:(Android51Event *)events capacity:(size_t)capacity {
    AEInputEvent buffer[128]; size_t count = [_input readEvents:buffer capacity:MIN(capacity, 128)];
    for (size_t i = 0; i < count; ++i) events[i] = {buffer[i].type, buffer[i].code, buffer[i].value};
    return count;
}
- (void)hostPCM:(const uint8_t *)data length:(size_t)length {
    // Goldfish emits S16LE complete stereo frames. Keep each ring commit bounded.
    while (length >= 4) {
        size_t frames = MIN(length / 4, 4096); int16_t samples[8192];
        for (size_t i = 0; i < frames * 2; ++i) samples[i] = (int16_t)(data[i * 2] | (uint16_t)data[i * 2 + 1] << 8);
        if (![_audio writeStereoPCM:samples frames:frames]) [_metrics droppedAudio];
        data += frames * 4; length -= frames * 4;
    }
}
- (void)hostSerial:(const uint8_t *)data length:(size_t)length {
    [_logLock lock];
    const size_t limit = 256 * 1024;
    if (length > limit) { data += length - limit; length = limit; }
    if (_log.length + length > limit) [_log replaceBytesInRange:NSMakeRange(0, _log.length + length - limit) withBytes:nullptr length:0];
    [_log appendBytes:data length:length]; [_logLock unlock];
}
- (void)finishSaveTask {
    if (_saveTask != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:_saveTask];
        _saveTask = UIBackgroundTaskInvalid;
    }
}
- (void)hostState:(int)state {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state == 2 || state == 4 || state == 5) [self finishSaveTask];
        if (state == 1 || state == 3) self->_statusText = @"Androidを起動中";
        else if (state == 2) self->_statusText = @"一時停止";
        else if (state == 5) self->_statusText = @"データの書き込みに失敗しました。端末の空き容量を確認してください";
    });
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; /* Never dlclose a QEMU lifecycle. */ }
@end
