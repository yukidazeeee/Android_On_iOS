import SwiftUI

struct LibrarySettingsView: View {
    @ObservedObject var model: LibraryModel
    @ObservedObject var jit: JITCoordinator
    let running: Bool
    @State private var ramInput = ""
    @State private var ramError: String?
    @AppStorage("graphics.diag.cpuReverseBlit") private var diagCPUReverseBlit = false
    @AppStorage("graphics.diag.disableSharedImageFinish") private var diagDisableSharedImageFinish = false
    @AppStorage("graphics.diag.advertiseDiscard") private var diagAdvertiseDiscard = false
    @AppStorage("graphics.diag.advertisePreserved") private var diagAdvertisePreserved = false
    @AppStorage("graphics.diag.clearPbufferOnAttach") private var diagClearPbufferOnAttach = false
    @AppStorage("graphics.diag.disableEGLImagePreserved") private var diagDisableEGLImagePreserved = false
    @AppStorage("graphics.diag.nearestColorBuffer") private var diagNearestColorBuffer = false
    @AppStorage("graphics.diag.dropEGLImageOnOrphan") private var diagDropEGLImageOnOrphan = false
    @AppStorage("graphics.diag.forceFullHostFrame") private var diagForceFullHostFrame = false
    @AppStorage("graphics.diag.visualizeAlpha") private var diagVisualizeAlpha = false
    @AppStorage("graphics.diag.tracePipeline") private var diagTracePipeline = false
    @AppStorage("graphics.diag.pixelFingerprints") private var diagPixelFingerprints = false
    @AppStorage("graphics.diag.traceEvery") private var diagTraceEvery = 30
    var body: some View {
        List {
            Section("VM Settings") {
                LabeledContent("Androidのメモリ", value: "\(model.configuration.ram.rawValue) MiB")
                Slider(value: Binding(get: { Double(model.configuration.ram.rawValue) }, set: {
                    if let ram = VMConfiguration.RAM(rawValue: Int($0)) { model.configuration.ram = ram }
                }), in: 512...4096, step: 1).disabled(running)
                HStack {
                    TextField("512〜4096", text: $ramInput).keyboardType(.numberPad)
                        .accessibilityLabel("メモリ容量（MiB）")
                    Text("MiB").foregroundStyle(.secondary)
                    Button("適用") {
                        guard let value = Int(ramInput), let ram = VMConfiguration.RAM(rawValue: value) else {
                            ramError = "512〜4096の整数を入力してください。"; return
                        }
                        model.configuration.ram = ram; ramError = nil
                    }.buttonStyle(.bordered)
                }.disabled(running)
                if let ramError { Text(ramError).foregroundStyle(.red).font(.footnote) }
                Text("次回起動時に使用: \(model.configuration.ram.guestMiB) MiB" +
                     (model.configuration.ram.needsHighmemKernel ? "（HIGHMEM対応カーネル）" : "（イメージ付属カーネル）"))
                    .font(.footnote).foregroundStyle(.secondary)
                Picker("TCG cache", selection: $model.configuration.cache) {
                    ForEach(VMConfiguration.Cache.allCases, id: \.self) { Text("\($0.rawValue) MiB").tag($0) }
                }.disabled(jit.state == .preparing || jit.state == .ready)
                LabeledContent("vCPU", value: "1")
                Text("少ないメモリの端末では640 MiBを推奨します。761 MiB以上では同梱の専用カーネルを使います。32bitボードの機器用領域を除き、Androidが使える上限は4080 MiBです。端末・署名のメモリ上限を超える場合は起動できません。拡張メモリの資格と空きメモリがある場合、変換キャッシュは自動で最大512 MiBになります。")
                    .font(.footnote).foregroundStyle(.secondary)
                Picker("描画する画面幅", selection: $model.configuration.resolution) {
                    ForEach(VMConfiguration.Resolution.allCases, id: \.self) { resolution in
                        Text("\(resolution.width) px" + (resolution == .performance ? "（速度優先）" : "")).tag(resolution)
                    }
                }.disabled(running)
                Text("高さは端末の比率に合わせ、全画面に拡大します。360 pxは540 pxに比べ描画画素数を約56%削減します。")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            Section {
                Toggle("詳細パイプライントレース", isOn: $diagTracePipeline)
                Toggle("画素ハッシュ・α統計を採取（重い）", isOn: $diagPixelFingerprints)
                Picker("ログ採取間隔", selection: $diagTraceEvery) {
                    Text("毎フレーム").tag(1)
                    Text("5フレームごと").tag(5)
                    Text("30フレームごと").tag(30)
                    Text("120フレームごと").tag(120)
                }
                .disabled(!diagTracePipeline && !diagPixelFingerprints)
                Text("画素ハッシュONではreverse EGLImageの前後、forward EGLImageのsource/import直後、最終post、QEMU→iOS callbackのRGBA/RGB/αハッシュと4×4領域fingerprintを記録します。短時間だけ「毎フレーム」にすると最も詳しく追跡できます。")
                    .font(.footnote).foregroundStyle(.secondary)
                Toggle("reverse EGLImageをCPUコピーに置換", isOn: $diagCPUReverseBlit)
                Toggle("共有EGLImageのglFinish同期を無効化", isOn: $diagDisableSharedImageFinish)
                Toggle("GL_EXT_discard_framebufferを広告", isOn: $diagAdvertiseDiscard)
                Toggle("preserved swapを対応扱いにする", isOn: $diagAdvertisePreserved)
                Toggle("ColorBuffer切替時にマゼンタクリア", isOn: $diagClearPbufferOnAttach)
                Toggle("EGL_IMAGE_PRESERVEDを付けない", isOn: $diagDisableEGLImagePreserved)
                Toggle("ColorBufferの拡大縮小をNEARESTにする", isOn: $diagNearestColorBuffer)
                Toggle("EGLImage orphan時に参照を破棄", isOn: $diagDropEGLImageOnOrphan)
                Toggle("host出力を毎回全画面更新", isOn: $diagForceFullHostFrame)
                Toggle("最終ColorBufferのαを白黒表示", isOn: $diagVisualizeAlpha)
                if diagClearPbufferOnAttach {
                    Text("マゼンタが残る場所は、そのColorBuffer切替後にAndroidが再描画していない領域です。半透明部分がマゼンタと混ざる場合はdirty-region / preserved-buffer経路が強く疑われます。")
                        .font(.footnote).foregroundStyle(.pink)
                }
                Button("描画診断をすべてOFF") {
                    diagCPUReverseBlit = false
                    diagDisableSharedImageFinish = false
                    diagAdvertiseDiscard = false
                    diagAdvertisePreserved = false
                    diagClearPbufferOnAttach = false
                    diagDisableEGLImagePreserved = false
                    diagNearestColorBuffer = false
                    diagDropEGLImageOnOrphan = false
                    diagForceFullHostFrame = false
                    diagVisualizeAlpha = false
                    diagTracePipeline = false
                    diagPixelFingerprints = false
                    diagTraceEvery = 30
                }
            } header: {
                Text("描画診断")
            } footer: {
                Text("各項目は独立した切り分け用です。原則1項目ずつONにしてください。設定は次回のAndroid起動時に反映されます。α白黒表示は画面を診断画像へ置き換えます。")
            }
            .disabled(running)
            Section("JIT") {
                LabeledContent("Status", value: jit.state.rawValue)
                LabeledContent("TXM", value: jit.txm.label)
                LabeledContent("SPTM", value: jit.sptm.label)
                LabeledContent("get-task-allow", value: jit.entitlement ? "Available" : "Missing")
                Text(jit.detail).font(.footnote).textSelection(.enabled)
                Button("Wait for Compatible Debugger") { jit.enable(cache: model.configuration.cache, openStikDebug: false) }
                    .disabled(jit.state == .preparing || jit.state == .ready)
            }
            NavigationLink("診断・ログ") { DiagnosticsView(model: model, jit: jit) }
        }.navigationTitle("詳細設定")
            .onAppear { ramInput = String(model.configuration.ram.rawValue) }
            .onChange(of: model.configuration.ram) { _, ram in ramInput = String(ram.rawValue); ramError = nil }
    }
}
