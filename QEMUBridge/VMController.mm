#import "VMController.h"
#import "NativeBridge.h"
#import "Display/MetalDisplay.h"
#import "Input/InputSurface.h"
#import "Audio/AudioOutput.h"
#import "Performance/RuntimeMetrics.h"
#include "ThirdParty/AndroidQemuCompat/qemu/android51_host.h"
#include "ThirdParty/AndroidQemuCompat/qemu/gpu.h"
#include "Network/GuestNetwork.hpp"
#include <dlfcn.h>
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
- (void)gpuFrame:(const uint8_t *)rgba width:(uint32_t)width height:(uint32_t)height;
@end
static void frameCallback(void *ctx, const uint8_t *p, size_t s, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    [(__bridge AEVMController *)ctx hostFrame:p stride:s x:x y:y width:w height:h];
}
static size_t inputCallback(void *ctx, Android51Event *e, size_t n) { return [(__bridge AEVMController *)ctx hostInput:e capacity:n]; }
static void pcmCallback(void *ctx, const uint8_t *p, size_t n) { [(__bridge AEVMController *)ctx hostPCM:p length:n]; }
static void serialCallback(void *ctx, const uint8_t *p, size_t n) { [(__bridge AEVMController *)ctx hostSerial:p length:n]; }
static void stateCallback(void *ctx, int s) { [(__bridge AEVMController *)ctx hostState:s]; }
static void gpuPostCallback(void *ctx, const uint8_t *rgba, uint32_t w, uint32_t h) {
    [(__bridge AEVMController *)ctx gpuFrame:rgba width:w height:h];
}
static void *gpuResolve(void *ctx, unsigned api, const char *name) {
    void **libraries = static_cast<void **>(ctx);
    if (api > 2 || !libraries[api]) return nullptr;
    void *result = dlsym(libraries[api], name);
    if (!result) {
        auto getProc = reinterpret_cast<void *(*)(const char *)>(dlsym(libraries[0], "eglGetProcAddress"));
        if (getProc) result = getProc(name);
    }
    return result;
}
static std::string optionPath(NSString *path) {
    std::string result;
    for (char c : std::string(path.UTF8String)) { result += c; if (c == ',') result += ','; }
    return result;
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
    int (*_run)(int, char **, const Android51Host *);
    void (*_pause)(bool);
    void (*_stop)(void);
    uint64_t (*_metric)(unsigned);
    bool (*_region)(void *, void *, size_t);
    decltype(&android51_gpu_start) _gpuStart;
    decltype(&android51_gpu_stop) _gpuStop;
    void *_angleLibraries[3];
    std::atomic<bool> _gpuPosted;
    NSLock *_frameLock;
    BOOL _started, _stopped, _guestPaused;
    uint32_t _width, _height;
    UIBackgroundTaskIdentifier _saveTask;
}
- (instancetype)init {
    if ((self = [super init])) {
        _saveTask = UIBackgroundTaskInvalid;
        _metrics = [AERuntimeMetrics new]; _audio = [AEAudioOutput new];
        _logLock = [NSLock new]; _log = [NSMutableData data];
        _frameLock = [NSLock new];
        _gpuPosted.store(false);
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
    // Serial may end mid UTF-8 sequence. Latin-1 preserves every diagnostic byte.
    return [[NSString alloc] initWithData:copy encoding:NSISOLatin1StringEncoding] ?: @"";
}
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
        [NSLayoutConstraint activateConstraints:@[[view.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
            [view.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
            [view.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor], [view.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor]]];
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
- (BOOL)startWithImageDirectory:(NSString *)path ramMiB:(uint32_t)ram cacheMiB:(uint32_t)cache panelWidth:(uint32_t)width {
    NSAssert([NSThread isMainThread], @"Launch must originate on UI thread");
    if (_started) { _statusText = @"再起動にはアプリを終了して開き直してください"; return NO; }
    if (!AEJITArenaReady()) { _statusText = @"先にJITを有効にしてください"; return NO; }
    if (!(ram == 512 || ram == 640 || ram == 768 || ram == 1024) || !(cache == 128 || cache == 192 || cache == 256)) {
        _statusText = @"非対応のメモリ設定です"; return NO;
    }
    if (!(width == 360 || width == 480 || width == 540 || width == 720)) {
        _statusText = @"非対応の画面幅です"; return NO;
    }
    // The stock Goldfish 3.4 kernel has a 760 MiB lowmem ceiling.
    ram = MIN(ram, 760U);
    // Use the arena actually prepared (possibly enlarged by the optional
    // entitlement), rather than the user's pre-preparation preference.
    cache = (uint32_t)((AEJITArenaSize() + (1U << 20) - 1) >> 20);
    // Use the active iPhone/iPad window ratio, with bounded guest pixels.
    UIWindowScene *scene = nil;
    for (UIScene *candidate in [UIApplication sharedApplication].connectedScenes) {
        if ([candidate isKindOfClass:UIWindowScene.class] && candidate.activationState == UISceneActivationStateForegroundActive) {
            scene = (UIWindowScene *)candidate; break;
        }
    }
    CGSize panel = CGSizeMake(540, 960);
    UIWindow *window = self.viewIfLoaded.window ?: scene.keyWindow;
    if (window) {
        panel = UIEdgeInsetsInsetRect(window.bounds, window.safeAreaInsets).size;
    } else if (scene) {
        if (@available(iOS 26.0, *)) { panel = scene.effectiveGeometry.coordinateSpace.bounds.size; }
        else { panel = scene.coordinateSpace.bounds.size; }
    }
    // Goldfish cannot hotplug panel geometry. Keep a portrait virtual panel;
    // window rotation/resizing aspect-fits it without cropping or coordinate drift.
    if (panel.width > panel.height) { panel = CGSizeMake(panel.height, panel.width); }
    uint32_t height = (uint32_t)(MIN(1600, MAX(480, width * panel.height / MAX(panel.width, 1))) / 2) * 2;
    if (self.isViewLoaded && (height != _height || width != _width)) { self.view = nil; _display = nil; _input = nil; }
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
    if (!_library) _library = dlopen([self enginePath].fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
    if (!_library) { const char *reason = dlerror(); _statusText = [NSString stringWithFormat:@"QEMU frameworkを読み込めません: %s", reason ?: "unknown loader error"]; return NO; }
    NSMutableArray<NSString *> *missing = [NSMutableArray array];
    auto resolve = [&](const char *name) -> void * {
        void *symbol = dlsym(_library, name);
        if (!symbol) [missing addObject:[NSString stringWithUTF8String:name]];
        return symbol;
    };
    _run = reinterpret_cast<decltype(_run)>(resolve("android51_host_run"));
    _pause = reinterpret_cast<decltype(_pause)>(resolve("android51_host_pause"));
    _stop = reinterpret_cast<decltype(_stop)>(resolve("android51_host_stop"));
    _metric = reinterpret_cast<decltype(_metric)>(resolve("android51_host_metric"));
    _region = reinterpret_cast<decltype(_region)>(resolve("android51_tcg_set_region"));
    _gpuStart = reinterpret_cast<decltype(_gpuStart)>(resolve("android51_gpu_start"));
    _gpuStop = reinterpret_cast<decltype(_gpuStop)>(resolve("android51_gpu_stop"));
    for (const char *name : {"android51_adb_connected", "android51_adb_disconnect",
                            "android51_adb_read", "android51_adb_write"}) {
        resolve(name);
    }
    if (missing.count) {
        _statusText = [NSString stringWithFormat:@"QEMU frameworkに必要な関数がありません: %@。更新したIPAを再インストールしてください。",
                      [missing componentsJoinedByString:@", "]];
        return NO;
    }
    NSArray<NSString *> *angleNames = @[@"libEGL", @"libGLESv1_CM", @"libGLESv2"];
    for (NSUInteger index = 0; index < angleNames.count; ++index) {
        NSString *name = angleNames[index];
        NSString *binary = [[[NSBundle mainBundle] privateFrameworksPath]
            stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.framework/%@", name, name]];
        if (!_angleLibraries[index]) _angleLibraries[index] = dlopen(binary.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!_angleLibraries[index]) {
            _statusText = [NSString stringWithFormat:@"描画ライブラリ %@ を読み込めません。GPU対応のIPAを再ビルドしてください。", name];
            return NO;
        }
    }
    NSError *error = nil;
    if (![_audio startWithSampleRate:44100 error:&error]) { _statusText = error.localizedDescription ?: @"音声の初期化に失敗しました"; return NO; }
    if (!_region(AEJITWritableBase(), const_cast<void *>(AEJITExecutableBase()), AEJITArenaSize())) {
        [_audio stop]; _statusText = @"TCG領域の登録に失敗しました。アプリの再起動が必要です"; return NO;
    }
    std::vector<std::string> args = {"AndroidEmu", "-machine", "android51,audiodev=audio,width=" + std::to_string(_width) + ",height=" + std::to_string(_height), "-cpu", "cortex-a8",
        "-m", std::to_string(ram), "-smp", "1", "-accel", "tcg,tb-size=" + std::to_string(cache) + ",split-wx=on",
        "-nodefaults", "-no-reboot", "-display", "none", "-serial", "null", "-monitor", "none",
        "-audiodev", "none,id=audio", "-nic", emu::guestNICOption(),
        "-kernel", [path stringByAppendingPathComponent:@"kernel"].UTF8String,
        "-initrd", [path stringByAppendingPathComponent:@"ramdisk.img"].UTF8String,
        "-append", "qemu=1 console=ttyS0 androidboot.console=ttyS0 androidboot.hardware=goldfish qemu.gles=1 android.qemud=1"};
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
            char gpuError[512] = {};
            bool gpuReady = self->_gpuStart(self->_width, self->_height, true, gpuResolve,
                self->_angleLibraries, gpuPostCallback, (__bridge void *)self, gpuError, sizeof(gpuError));
            int result = gpuReady ? self->_run((int)owned.size(), argv.data(), &host) : -1;
            if (gpuReady) self->_gpuStop();
            NSString *gpuFailure = gpuReady ? nil : [NSString stringWithFormat:@"GPU初期化失敗: %s", gpuError];
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_worker = nil;
                [self->_adb cancel];
                self->_stopped = YES; self->_display.paused = YES; self->_input.userInteractionEnabled = NO; [self->_audio stop];
                [UIApplication sharedApplication].idleTimerDisabled = NO;
                self->_statusText = gpuFailure ?: (result == 0 ? @"停止しました。再起動にはアプリを開き直してください" : @"QEMUがエラーで停止しました");
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
    [_frameLock lock];
    if (!_gpuPosted.load() && [_display submitPixels:p length:s * _height stride:s x:x y:y width:w height:h]) [_metrics receivedFrameBytes:(uint64_t)w * h * 4];
    [_frameLock unlock];
}
- (void)gpuFrame:(const uint8_t *)rgba width:(uint32_t)width height:(uint32_t)height {
    if (!rgba || width != _width || height != _height) return;
    std::vector<uint8_t> bgra(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *source = rgba + size_t(height - 1 - y) * width * 4;
        uint8_t *destination = bgra.data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            destination[x*4] = source[x*4+2]; destination[x*4+1] = source[x*4+1];
            destination[x*4+2] = source[x*4]; destination[x*4+3] = source[x*4+3];
        }
    }
    [_frameLock lock];
    if ([_display submitPixels:bgra.data() length:bgra.size() stride:size_t(width)*4 x:0 y:0 width:width height:height]) {
        _gpuPosted.store(true);
        [_metrics receivedFrameBytes:bgra.size()];
    }
    [_frameLock unlock];
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
