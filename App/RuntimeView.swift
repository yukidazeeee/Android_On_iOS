import SwiftUI
import Combine

@MainActor final class RuntimeModel: ObservableObject {
    let controller = AEVMController()
    @Published private(set) var status = "起動準備"
    @Published private(set) var stopped = false
    @Published private(set) var hasFrame = false
    @Published private(set) var metrics: [String: NSNumber] = [:]
    @Published var paused = false
    @Published var showPerformance = false
    private var sampler: AnyCancellable?
    private var samples = 0
    private var startedAt: Date?
    @Published private(set) var elapsedSeconds = 0
    @Published private(set) var preparing = false
    func start(configuration: VMConfiguration, directory root: URL) async -> Bool {
        guard !preparing else { return false }
        preparing = true
        defer { preparing = false }
        status = "cache領域を準備中"
        do { try await ImageStore(root: root).ensureCache() }
        catch { status = error.localizedDescription; return false }
        let success = controller.start(imageDirectory: root.path,
            ramMiB: UInt32(configuration.ram.rawValue), cacheMiB: UInt32(configuration.cache.rawValue), panelWidth: UInt32(configuration.resolution.width))
        status = controller.statusText
        guard success else { return false }
        startedAt = Date()
        showPerformance = configuration.performanceOverlay
        sampler = Timer.publish(every: 1, on: .main, in: .common).autoconnect().sink { [weak self] _ in
            guard let self else { return }
            self.paused = self.controller.guestPaused
            self.samples += 1
            self.elapsedSeconds = Int(Date().timeIntervalSince(self.startedAt ?? Date()))
            if let adb = self.controller.adb, !adb.bootCompleted, !adb.busy, !self.paused, self.samples % 10 == 1 {
                adb.checkBoot()
            }
            self.status = self.controller.statusText
            self.stopped = self.controller.stopped
            if self.controller.adb?.bootCompleted == true && !self.paused && !self.stopped { self.status = "Android実行中" }
            self.metrics = self.controller.statistics()
            self.hasFrame = (self.metrics["totalGuestUpdates"]?.uint64Value ?? 0) > 0
            if self.stopped { self.sampler = nil }
        }
        return true
    }
    func pause(_ value: Bool) { paused = value; controller.setGuestPaused(value) }
    func stop() { controller.stopGuest() }
}

private struct GuestSurface: UIViewControllerRepresentable {
    let controller: AEVMController
    func makeUIViewController(context: Context) -> AEVMController { controller }
    func updateUIViewController(_ controller: AEVMController, context: Context) {}
}

struct RuntimeView: View {
    @ObservedObject var runtime: RuntimeModel
    @StateObject private var network = NetworkStatus()
    @State private var menu = false
    @AppStorage("runtime.immersive") private var immersive = true
    @State private var log = false
    @State private var confirmStop = false
    @Environment(\.scenePhase) private var phase
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        ZStack(alignment: .topTrailing) {
            Color.black.ignoresSafeArea()
            GuestSurface(controller: runtime.controller).ignoresSafeArea()
            if !runtime.hasFrame || runtime.stopped {
                VStack(spacing: 16) {
                    if !runtime.stopped { ProgressView().tint(.white) }
                    Text(runtime.status).foregroundStyle(.white)
                    if runtime.stopped { Button("詳細を確認") { log = true }.tint(.white) }
                    if runtime.stopped { Button("ライブラリへ戻る") { dismiss() }.tint(.white) }
                }.frame(maxWidth: .infinity, maxHeight: .infinity).allowsHitTesting(true)
            }
            if runtime.showPerformance && !immersive {
                VStack(alignment: .leading, spacing: 3) {
                    Text(String(format: "更新 %.0f/s · 表示 %.0f/s", value("guestUpdatesPerSecond"), value("presentationsPerSecond")))
                    Text(String(format: "メモリ %.0f MiB · コピー %.1f MiB/s", value("footprintMiB"), value("copyMiBPerSecond")))
                    Text(String(format: "TCG %.1f/%.0f MiB · flush %.0f", value("tcgUsedMiB"), value("tcgCapacityMiB"), value("tbFlushCount")))
                }
                .font(.caption.monospacedDigit()).foregroundStyle(.white)
                .padding(8).background(.black.opacity(0.7), in: RoundedRectangle(cornerRadius: 8))
                .frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 12).padding(.top, 8)
                .allowsHitTesting(false)
            }
            if !immersive { Button { menu = true } label: {
                Image(systemName: "ellipsis").font(.body.bold()).foregroundStyle(.white)
                    .frame(width: 44, height: 36).background(.black.opacity(0.55), in: Capsule())
            }.accessibilityLabel("エミュレータの操作").padding(.trailing, 12).padding(.top, 8) }
        }
        .onAppear { runtime.controller.showControls = { menu = true } }
        .onDisappear { runtime.controller.showControls = nil }
        .accessibilityAction(named: Text("操作メニューを開く")) { menu = true }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .interactiveDismissDisabled()
        .onChange(of: phase) { _, phase in
            if phase == .background { runtime.pause(true) }
            if phase == .active && runtime.paused { menu = true }
        }
        .sheet(isPresented: $menu) {
            NavigationStack {
                List {
                    Section("ツール") {
                        if let adb = runtime.controller.adb {
                            NavigationLink("APK・ADB") { ADBToolsView(client: adb, paused: runtime.paused, resume: { runtime.pause(false) }) }
                        }
                        NavigationLink("シリアルログ") { RuntimeLogView(text: runtime.controller.serialText) }
                        NavigationLink("描画パイプラインログ") {
                            RuntimeGraphicsDiagnosticsView(controller: runtime.controller)
                        }
                    }
                    Section {
                        Text(runtime.status)
                        Text("起動から \(runtime.elapsedSeconds / 60)分\(runtime.elapsedSeconds % 60)秒")
                        if runtime.controller.adb?.bootCompleted != true {
                            Text("ロゴの表示だけでは起動完了を判断できません。進まない場合は「APK・ADB」の起動診断を取得してください。")
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                        Button(runtime.paused ? "再開" : "一時停止") { runtime.pause(!runtime.paused); menu = false }
                        Toggle("完全全画面（メニューボタンを隠す）", isOn: $immersive)
                        Text("3本指の長押しで操作メニューを開けます。")
                            .font(.footnote).foregroundStyle(.secondary)
                        Toggle("パフォーマンスを表示", isOn: $runtime.showPerformance)
                    }
                    Section {
                        NavigationLink("Androidの操作ボタン") {
                            List {
                                Section("Androidの操作") {
                                    HStack {
                                        GuestKey(label: "戻る", code: 158, runtime: runtime)
                                        GuestKey(label: "ホーム", code: 172, runtime: runtime)
                                    }.buttonStyle(.bordered)
                                    HStack {
                                        GuestKey(label: "電源", code: 116, runtime: runtime)
                                        GuestKey(label: "音量−", code: 114, runtime: runtime)
                                        GuestKey(label: "音量＋", code: 115, runtime: runtime)
                                    }.buttonStyle(.bordered)
                                }
                                Section("旧アプリ向け") {
                                    HStack {
                                        GuestKey(label: "メニュー", code: 139, runtime: runtime)
                                        GuestKey(label: "検索", code: 217, runtime: runtime)
                                    }
                                }
                            }.navigationTitle("Androidの操作")
                        }
                    }
                    DisclosureGroup("詳細ツール・診断") {
                        Text(network.description)
                        Text("停止すると、次回の起動にはアプリを開き直す必要があります。")
                            .font(.footnote).foregroundStyle(.secondary)
                        Button("Androidを停止", role: .destructive) { confirmStop = true }
                    }
                }.navigationTitle("Android")
                    .toolbar { Button("閉じる") { menu = false } }
            }.presentationDetents([.medium, .large])
        }
        .sheet(isPresented: $log) {
            NavigationStack {
                RuntimeLogView(text: runtime.controller.serialText)
                    .toolbar { Button("閉じる") { log = false } }
            }
        }
        .confirmationDialog("Androidを停止しますか？未保存のデータは失われる場合があります。", isPresented: $confirmStop, titleVisibility: .visible) {
            Button("停止", role: .destructive) { runtime.stop(); menu = false }
        }
    }
    private func value(_ key: String) -> Double { runtime.metrics[key]?.doubleValue ?? 0 }
}

private struct GuestKey: View {
    let label: String
    let code: UInt16
    let runtime: RuntimeModel
    var body: some View {
        Button {
            runtime.controller.sendGuestKey(code, pressed: true)
            runtime.controller.sendGuestKey(code, pressed: false)
        } label: {
            Text(label).frame(maxWidth: .infinity, minHeight: 44)
        }
        .buttonStyle(.bordered)
        .contextMenu {
            Button("長押し（1秒）") {
                runtime.controller.sendGuestKey(code, pressed: true)
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    runtime.controller.sendGuestKey(code, pressed: false)
                }
            }
        }
        .disabled(runtime.stopped)
    }
}

private struct RuntimeGraphicsDiagnosticsView: View {
    let controller: AEVMController
    @State private var text = ""

    var body: some View {
        ScrollView {
            Text(text)
                .font(.caption2.monospaced())
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
        }
        .navigationTitle("描画パイプライン")
        .toolbar {
            Button("ノイズ地点をマーク") {
                controller.markGraphicsDiagnostics()
                refresh()
            }
            Button("消去") {
                controller.clearGraphicsDiagnostics()
                refresh()
            }
            ShareLink(item: text)
        }
        .task {
            refresh()
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(1))
                refresh()
            }
        }
    }

    private func refresh() {
        text = controller.graphicsDiagnosticsText
    }
}

private struct RuntimeLogView: View {
    let text: String
    var body: some View {
        ScrollView {
            Text(text).font(.caption.monospaced()).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading).padding()
        }
        .navigationTitle("起動ログ")
        .toolbar { ShareLink(item: text) }
    }
}
