# Android 6 Browser / WebViewのGPUクラッシュ調査

現状: EmuGL GLES1/2 decoder、Goldfish GPU pipe、ANGLE/Metal の iOS ビルド・同梱経路を追加済み。Linux の実 EGL/GLES による描画試験は通過。ただし iOS および Android 4/5/6 実イメージ上での Browser / WebView の正常動作は未確認であり、完全対応とはしていない。

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
