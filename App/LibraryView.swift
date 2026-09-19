import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @ObservedObject var model: LibraryModel
    @ObservedObject var jit: JITCoordinator
    @StateObject private var runtime = RuntimeModel()
    @State private var showCreation = false
    @State private var chooseImage = false
    @State private var importAsNew = true
    @State private var showDownloads = false
    @State private var showSettings = false
    @State private var showRuntime = false
    @State private var launchWhenReady = false
    @State private var rename = false
    @State private var profileName = ""
    @State private var confirmDelete = false
    @State private var confirmReplace = false
    @Environment(\.scenePhase) private var scenePhase
    private var locked: Bool { model.importing || runtime.controller.started || runtime.preparing || jit.state == .preparing }

    var body: some View {
        NavigationSplitView {
            List(selection: Binding(get: { model.selectedID }, set: { if let id = $0 { model.select(id) } })) {
                ForEach(model.profiles) { profile in
                    NavigationLink(value: profile.id) {
                        HStack(spacing: 12) {
                            Image(systemName: "apps.iphone")
                                .font(.title2).foregroundStyle(.green)
                                .frame(width: 44, height: 48).background(.green.opacity(0.1), in: RoundedRectangle(cornerRadius: 12))
                            VStack(alignment: .leading, spacing: 4) {
                                Text(profile.name).font(.headline).lineLimit(1)
                                Text(subtitle(profile)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                            Spacer(minLength: 4)
                        }.padding(.vertical, 5)
                    }
                    .disabled(locked)
                    .contextMenu {
                        Button("起動", systemImage: "play.fill") { model.select(profile.id); beginLaunch() }
                            .disabled(locked || model.profileImages[profile.id] == nil)
                        Button("名前を変更", systemImage: "pencil") { model.select(profile.id); profileName = profile.name; rename = true }.disabled(locked)
                        Button("削除", systemImage: "trash", role: .destructive) { model.select(profile.id); confirmDelete = true }
                            .disabled(locked)
                    }
                }
            }
            .navigationTitle("ライブラリ")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("設定", systemImage: "gearshape") { showSettings = true }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button("Androidを追加", systemImage: "plus") { showCreation = true }.disabled(locked)
                }
            }
            .navigationSplitViewColumnWidth(min: 260, ideal: 320)
        } detail: {
            if let profile = model.selectedProfile {
                ScrollView {
                    VStack(spacing: 24) {
                        Image(systemName: "apps.iphone").font(.system(size: 60)).foregroundStyle(.green)
                            .frame(width: 112, height: 112).background(.green.opacity(0.1), in: RoundedRectangle(cornerRadius: 28))
                        VStack(spacing: 8) {
                            Text(profile.name).font(.largeTitle.bold()).multilineTextAlignment(.center)
                            Text(subtitle(profile)).foregroundStyle(.secondary)
                        }
                        if model.manifest != nil {
                            Button(action: beginLaunch) { Label("起動", systemImage: "play.fill").frame(maxWidth: .infinity).padding(.vertical, 6) }
                                .buttonStyle(.borderedProminent).controlSize(.large).disabled(locked)
                            if let image = model.manifest {
                                VStack(spacing: 12) {
                                    LabeledContent("Android", value: ImageProfile.label(for: image.profile))
                                    LabeledContent("イメージ容量", value: ByteCountFormatter.string(fromByteCount: image.files.reduce(0) { $0 + $1.bytes }, countStyle: .binary))
                                }.font(.subheadline).padding().background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
                            }
                        } else {
                            ContentUnavailableView("Androidを追加", systemImage: "arrow.down.app", description: Text("ダウンロードするか、お持ちのイメージフォルダを読み込んでください。"))
                            Button("ダウンロードから選ぶ") { showDownloads = true }.buttonStyle(.borderedProminent).disabled(locked)
                            Button("フォルダから読み込む") { importAsNew = false; chooseImage = true }.disabled(locked)
                        }
                        if let issue = model.profileIssues[profile.id] { Text(issue).font(.footnote).foregroundStyle(.red) }
                        if jit.state == .preparing || runtime.preparing { ProgressView("起動準備中…") }
                        if runtime.controller.started { Text("別のAndroidを起動するには、このアプリを終了して開き直してください。").font(.footnote).foregroundStyle(.secondary) }
                        Text("アプリとデータはこのAndroid専用に保存されます。実行画面では3本指を長押しすると操作メニューが開きます。")
                            .font(.footnote).foregroundStyle(.secondary)
                    }.frame(maxWidth: 520).padding(28).frame(maxWidth: .infinity)
                }
                .navigationTitle(profile.name).navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        Menu {
                            Button("名前を変更", systemImage: "pencil") { profileName = profile.name; rename = true }
                            Button("イメージを置き換える", systemImage: "arrow.triangle.2.circlepath") { confirmReplace = true }
                            Button("削除", systemImage: "trash", role: .destructive) { confirmDelete = true }
                        } label: { Label("管理", systemImage: "ellipsis.circle") }.disabled(locked)
                    }
                }
            } else {
                ContentUnavailableView("Androidを選択", systemImage: "apps.iphone", description: Text("ライブラリから選ぶか、＋から追加してください。"))
            }
        }
        .overlay {
            if model.importing && !showDownloads && !showCreation {
                Color.black.opacity(0.2).ignoresSafeArea()
                transfer.padding(24)
            }
        }
        .sheet(isPresented: $showCreation) { AndroidCreationView(model: model) }
        .sheet(isPresented: $showSettings) {
            NavigationStack {
                LibrarySettingsView(model: model, jit: jit, running: runtime.controller.started)
                    .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完了") { showSettings = false } } }
            }
        }
        .sheet(isPresented: $showDownloads) {
            NavigationStack {
                ImageDownloadsView(model: model)
                    .toolbar { ToolbarItem(placement: .confirmationAction) { Button("閉じる") { showDownloads = false } } }
            }
        }
        .sheet(isPresented: $chooseImage) {
            ImageDirectoryPicker { url in if importAsNew { model.importNewImage(url) } else { model.importImage(url) } }
        }
        .fullScreenCover(isPresented: $showRuntime) { RuntimeView(runtime: runtime) }
        .alert("名前を変更", isPresented: $rename) {
            TextField("名前", text: $profileName)
            Button("保存") { model.saveProfile(name: profileName, creating: false) }
            Button("キャンセル", role: .cancel) {}
        }
        .confirmationDialog("このAndroidと保存データを削除します。元に戻せません。", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("削除", role: .destructive) { model.deleteProfile() }
        }
        .confirmationDialog("置き換えると、このAndroidのアプリと保存データも置き換わります。", isPresented: $confirmReplace, titleVisibility: .visible) {
            Button("置き換える", role: .destructive) { importAsNew = false; chooseImage = true }
        }
        .alert("処理できませんでした", isPresented: Binding(get: { model.errorMessage != nil && !showDownloads }, set: { if !$0 { model.errorMessage = nil } })) {
            Button("OK") { model.errorMessage = nil }
        } message: { Text(model.errorMessage ?? "") }
        .task { await model.load(); jit.refresh(); jit.observe(cache: model.configuration.cache) }
        .onChange(of: model.configuration.cache) { _, cache in jit.observe(cache: cache) }
        .onChange(of: jit.state) { _, state in
            if state == .ready && launchWhenReady && scenePhase == .active { launchWhenReady = false; launch() }
            if state == .failed { launchWhenReady = false; model.errorMessage = jit.detail }
        }
        .onChange(of: scenePhase) { _, phase in if phase == .active {
            jit.refresh(); jit.observe(cache: model.configuration.cache)
            if launchWhenReady && jit.state == .ready { launchWhenReady = false; launch() }
        } }
    }
    private var transfer: some View {
        VStack(spacing: 18) {
            Image(systemName: "square.and.arrow.down").font(.largeTitle).foregroundStyle(.tint)
            Text(model.downloading ? "Androidをダウンロード中" : "Androidを準備しています").font(.headline)
            ProgressView(value: model.downloading ? model.transferProgress : nil)
            Text(model.downloading ? model.transferStatus : "イメージをコピー・確認しています。サイズによって数分かかります。")
                .font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center)
            Text("この画面を開いたままお待ちください。保存データはAndroidごとに分けて保存されます。")
                .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
            if model.downloading { Button("キャンセル", role: .cancel) { model.cancelDownload() } }
        }.padding(28).frame(maxWidth: 380)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24))
            .shadow(color: .black.opacity(0.15), radius: 24, y: 8)
    }

    private func subtitle(_ profile: AndroidProfile) -> String {
        if let image = model.profileImages[profile.id] { return ImageProfile.label(for: image.profile) }
        return model.profileIssues[profile.id] == nil ? "イメージ未設定" : "イメージの確認が必要です"
    }
    private func beginLaunch() {
        guard model.manifest != nil, !locked else { return }
        if jit.state == .ready { launch() }
        else { launchWhenReady = true; jit.enable(cache: model.configuration.cache, openStikDebug: true) }
    }
    private func launch() {
        Task {
            guard let profile = model.selectedProfile,
                  let manifest = model.manifest,
                  let apiLevel = ImageProfile.api(for: manifest.profile) else {
                model.errorMessage = "AndroidイメージのAPIレベルを確認できません。再取り込みしてください。"
                return
            }
            if await runtime.start(configuration: model.configuration,
                                   directory: profile.directory,
                                   apiLevel: apiLevel) {
                showRuntime = true
            } else {
                model.errorMessage = runtime.status
            }
        }
    }
}
struct ImageDirectoryPicker: UIViewControllerRepresentable {
    var selected: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
        picker.allowsMultipleSelection = false; picker.delegate = context.coordinator
        return picker
    }
    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}
    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let parent: ImageDirectoryPicker
        init(_ parent: ImageDirectoryPicker) { self.parent = parent }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            if let url = urls.first { parent.selected(url) }; parent.dismiss()
        }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { parent.dismiss() }
    }
}

private struct AndroidCreationView: View {
    @ObservedObject var model: LibraryModel
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var chooseFolder = false
    @State private var selectedFolder: URL?
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("例：ゲーム用、Android 6", text: $name)
                        .submitLabel(.done)
                } header: { Text("1. 名前を付ける") }
                footer: { Text("あとから変更できます。空欄の場合はイメージの名前を使います。") }
                Section {
                    NavigationLink {
                        ImageDownloadsView(model: model, profileName: name)
                    } label: {
                        Label {
                            VStack(alignment: .leading, spacing: 4) {
                                Text("ダウンロードから選ぶ")
                                Text("公開されているAndroidの一覧を表示").font(.caption).foregroundStyle(.secondary)
                            }
                        } icon: { Image(systemName: "arrow.down.circle") }
                    }
                    Button { chooseFolder = true } label: {
                        Label {
                            VStack(alignment: .leading, spacing: 4) {
                                Text("自分のイメージを読み込む")
                                Text("展開済みのイメージフォルダを選択").font(.caption).foregroundStyle(.secondary)
                            }
                        } icon: { Image(systemName: "folder") }
                    }
                } header: { Text("2. Androidを選ぶ") }
                footer: {
                    Text("同じイメージを何度選んでも、アプリ・設定・保存データはそれぞれ独立します。元のフォルダは変更されません。")
                }
            }
            .navigationTitle("Androidを追加")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("閉じる") { dismiss() } } }
            .sheet(isPresented: $chooseFolder, onDismiss: {
                if let url = selectedFolder {
                    selectedFolder = nil
                    model.importNewImage(url, name: name)
                }
            }) {
                ImageDirectoryPicker { selectedFolder = $0 }
            }
            .onChange(of: model.importing) { _, importing in if importing { dismiss() } }
        }
    }
}
