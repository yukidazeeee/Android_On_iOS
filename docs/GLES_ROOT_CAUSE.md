# Android 6 Browser / WebViewのGPUクラッシュ調査

現状: EmuGL GLES1/2 decoder、Goldfish GPU pipe、ANGLE/Metal の iOS ビルド・同梱経路を追加済み。Linux の実 EGL/GLES による描画試験は通過。ただし iOS および Android 4/5/6 実イメージ上での Browser / WebView の正常動作は未確認であり、完全対応とはしていない。

## Metal backend の EGLImage 接続修正

固定した ANGLE の `DisplayMtl::generateExtensions` は
`EGL_ANGLE_metal_texture_client_buffer` を提供する一方、
`EGL_KHR_gl_texture_2D_image` を提供していない。
`ImageMtl::initialize` も `EGL_METAL_TEXTURE_ANGLE` のみを受け付ける。
従来の必須拡張チェックと GL texture からの image 作成は、この backend と互換でなかった。

`GPU/MetalImages.mm` は EGLDisplay が使う MTLDevice を問い合わせ、その device で
RGBA texture を作成して EGLImage に取り込む。ColorBuffer の描画用・転送用 texture を
その image に結びつけ、独立したゲスト context 間で既存の image 共有経路を使う。
ゲストに `qemu.gles=1` を通知する前に、小さな color buffer の作成・image 取り込みを確認する。
成功時は `[GPU] ANGLE Metal initialized; shared color-buffer probe passed` を記録する。
また、pbuffer だけを使う実行では未作成だった TextureDraw を GPU 初期化時に作成する。
従来は window color buffer の flush で null pointer を参照していた。
GLES1/2 の RGB/RGBA 両方で window → shared image → readback を試験する。
この変更の iOS 実機・Browser 動作はまだ未検証。

## 今回の実装と起動時の扱い

- `GPU/` は分割パケット、同期応答、GLES1/2 と renderControl を処理し、実 EGL context / pbuffer / color buffer に接続する。
- GPU の初期化に成功した場合だけ、カーネル引数と boot-properties の両方で `qemu.gles=1` を通知する。
- libEGL / libGLESv1_CM / libGLESv2 が欠ける、古いエンジンに GPU API がない、初期化に失敗する場合は、理由をログに残して `qemu.gles=0` の CPU framebuffer で起動する。ライブラリ欠落だけを理由に VM 起動を中止しない。このフォールバックは GLES2 WebView の修正にはならない。
- `scripts/build_qemu_ios.sh` は既定で `build_gpu_ios.sh` を実行し、固定 revision の ANGLE iOS frameworks をエンジンと一緒に同梱する。macOS / Xcode が必要。`ANDROIDEMU_GPU=0` は従来のソフトウェア構成を明示的に選ぶ。
- GLES1/2 clear/readback、GLES2 shader compile/link と FBO clear/readback、color buffer update/post、接続の終了処理を `Tests/GpuRendererTests.cpp` で検証する。GLES1 を無効化した Mesa では実行できない。

検証例（GLES1/2 有効の Mesa を指定）:

```sh
cmake -S GPU -B build/gpu -DCMAKE_BUILD_TYPE=Debug
cmake --build build/gpu --parallel 2
LD_LIBRARY_PATH=/path/to/mesa/lib ctest --test-dir build/gpu --output-on-failure
```

未検証事項: iOS SDK によるビルド・コード署名・Metal 実行、Android API ごとの wire protocol / 拡張機能の互換性、Browser / WebView、EGLImage 共有と複数アプリの寿命管理。旧 EmuGL の全拡張を完全実装したという意味ではない。

以下は変更前の原因調査記録。

## 現在の実装とログの対応

- `QEMUBridge/VMController.mm`のカーネル引数、および`ThirdParty/AndroidQemuCompat/qemu/pipe.c`の起動プロパティは`qemu.gles=0`を指定している。
- Android 6の[EGL Loader.cpp](https://android.googlesource.com/platform/frameworks/native/+/android-6.0.1_r1/opengl/libs/EGL/Loader.cpp)は`ro.kernel.qemu=1`かつ`ro.kernel.qemu.gles=0`なら`/system/lib/egl/libGLES_android.so`を固定で選択する。
- この[libaglのEGL設定](https://android.googlesource.com/platform/frameworks/native/+/android-6.0.1_r1/opengl/libagl/egl.cpp)は`EGL_RENDERABLE_TYPE = EGL_OPENGL_ES_BIT`であり、GLES 2用の`EGL_OPENGL_ES2_BIT`を提供していない。Loaderは取得できないGL関数を`gl_unimplemented`へ置き換える。このため「called unimplemented OpenGL ES API」と整合する。
- 提供されたログではChromiumのEGL config探索とGLSurface初期化が失敗し、続いてGpuThreadがCHECK失敗でSIGABRTしている。これはiOS上のMetalビューの表示サイズや色変換ではなく、ゲストに必要なGL実装がないことが主因と判断できる。
- `Display/MetalDisplay.mm`（表示処理）とGoldfish `display.c`はCPUフレームバッファを表示する経路であり、ゲストのGLES命令やEGLContextを実行していない。Metalで画面を表示できることはゲストのGLES 2対応を意味しない。

## 既存Android Emulatorとの比較

[AOSP Android 6 HostConnection.cpp](https://android.googlesource.com/device/generic/goldfish/+/android-6.0.1_r1/opengl/system/OpenglSystemCommon/HostConnection.cpp)は、通常QemuPipeStreamでホストに接続し、GLES 1・GLES 2・renderControlのencoderを利用する。現在の`pipe.c`はboot-properties、ADB、gsm、pingpongを実装しているが、このGPU経路を実装していない。

参照用に取得済みのAOSP QEMU `android/opengles.c`は、`libOpenglRender`をロードし、renderer初期化・post callback・GPU pipeの登録を行う。元の[EmuGL FrameBuffer.cpp](https://android.googlesource.com/platform/external/qemu/+/e6aef36e024c3265ff8103f8d2265dd235851ef4/distrib/android-emugl/host/libs/libOpenglRender/FrameBuffer.cpp)は実際のEGLDisplay・EGLContextとGLES dispatchを必要とする。これらは現在の最小iOSエンジンの依存関係に含まれていない。

`qemu.gles=1`だけを設定すると、存在しないホストrendererへの接続を選択することになり、正常な描画経路にならない。Browserの起動引数やHWUIの一括無効化でも、不足したGLES 2実装を補うことはできない。今回そのようなフラグ変更・成功応答の偽装は実施していない。

## 根本修正に必要な実装

1. Android 4〜6のGLES 1 / GLES 2 / renderControlプロトコルと互換なホストdecoderを組み込む。分割パケット、同期応答、複数ゲストプロセス・スレッドの接続を扱う。
2. iOSで実際に動くEGL/GLES backendに接続し、context・surface・texture・color buffer・EGLImage・共有context・同期の寿命を管理する。既存desktop EmuGLのライブラリを単純にiOSへコピーするだけでは成立しない。
3. grallocによるbuffer登録・更新・読み戻しと画面postを接続し、CPUフレームバッファ経路との整合を取る。
4. backendの実初期化とGLES 2 configの確認が成功したときのみGPU対応をゲストへ通知する。
5. API23の実イメージ上でEGL初期化、GLES2のshader compile/link、描画・readback、texture/FBO/EGLImage、複数context・複数アプリ、Browserと独立WebViewアプリを検証する。

本環境にはiOS SDKおよび検証用Androidシステムイメージがなく、このrendererの実装・統合・実機検証は完了していない。UI変更・ダウンロード対策の完了と、GPU修正の完了は別である。

## Actions の所要時間・配布容量

- iOS ビルドと Linux の native / Goldfish 試験は並行実行する。
- Xcode・runner architecture・固定依存・ビルドスクリプトをキーに、iOS sysroot と
  ANGLE の checkout / incremental build をキャッシュする。初回は従来どおり取得・ビルドが必要。
- アプリ artifact は IPA、entitlements、署名手順のみ。
  対応ソースは `AndroidEmu-corresponding-source` から別途取得できる。
- 対応ソース archive にはダウンロード済み LLVM / Python / Siso などのホストバイナリを入れず、
  固定 revision、DEPS、取得スクリプトと実際の engine source を残す。
- CI ログに artifact ごとの容量を記録する。変更後の時間・容量の実測値はまだない。

## AGX partial macroblock crash の調査

`com.apple.metal.agx.CompressedTexturePartialMacroblockAccess` での
EXC_BAD_ACCESS が報告されたが、完全なスタックがなく原因は未確定。
CPU 更新する表示 texture と EGLImage 用 texture は、Metal の shared storage と
`allowGPUOptimizedContents = NO` を使う互換設定へ変更した。
これは Apple の [texture 最適化からの opt-out 手順](https://developer.apple.com/documentation/metal/optimizing-texture-data)
に基づく対策で、AGX のクラッシュ解消を実機確認したという意味ではない。

併せて native image 取り込み前の不要な GL texture allocation/upload を除去し、
旧 EGL 経路の初期化でも tightly packed RGB rows に UNPACK_ALIGNMENT=1 を設定する。
幅 1 / 3 / 5、高さ 3 の RGB color buffer を初期化・更新・読み戻す回帰試験を追加。

## 画面方向と共有画像の引き渡し

EmuGL の TextureDraw は window surface を color buffer に転送するときに上下を反転し、
Android の top-down buffer 配置へ変換する。iOS の gpuFrame では行を再反転せず、
RGBA→BGRA のチャンネル変換だけを行う。CPU gralloc 更新も同じ行配置を使う。
四隅を別の色にした GLES1/2・RGB/RGBA の画像を別 pipe で post し、
表示用画素まで検証するテストを追加した。

共有画像は producer のコピー完了、および renderer の転送／CPU 更新完了を
`glFinish` で待ってから他の context に引き渡す。現時点では確実な同期を優先しており、
フレームレートへの影響は iOS 実機で測定が必要。
読み戻し失敗時は前回の画素を post しない。WindowSurface の flush 失敗も成功扱いにしない。
これらは表示側の修正であり、Android の system_server / SurfaceFlinger が再起動して
ロゴに戻る現象を解消したという確認はまだない。
