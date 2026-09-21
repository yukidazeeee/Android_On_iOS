# Android 4–6 Goldfish GPU / iOS 描画設計

状態: 方針承認済み。本文は実装前レビュー用。実装完了を示すものではない。

## 目的と対象

ARMv7a の Android 4.x、5.x、6.0 の既存 Goldfish ゲストが要求する
EGL、OpenGL ES 1.x/2.0、renderControl を iOS 上で実行し、SurfaceFlinger、
Browser、WebView の初期化、描画、スクリーンショットを成立させる。
ゲスト ABI は 32 bit ARM、ホストは arm64 iPhoneOS、最低 OS は既存の iOS 17.0。
Xcode/iPhoneOS SDK の条件は既存ビルドスクリプトに従う。
Android 4.x の細かな版によるプロトコル差は互換表と実イメージ試験で明示する。
任意の Android 派生イメージや全アプリの無欠陥動作まで保証する意味ではない。

## 確認済みの現状

- `VMController.mm` とホストランチャーは `qemu.gles=0` を指定する。
- `pipe.c` の boot-properties も同じ値を返し、GPU サービスを登録しない。
- `MetalDisplay.mm` は CPU の BGRA フレームを表示するもので、GLES 実行器ではない。
- QEMU ビルドは OpenGL と virglrenderer を無効にしている。
- GLES デコーダ、EGL 実装、ANGLE は現在の依存ソースに含まれない。
- 提示された Android 6 のログは GL 実装初期化失敗と整合する。
  そのログだけで全クラッシュの原因や修正後の動作は証明できない。

## 採用する構成

ゲスト EGL/GLES encoder → Goldfish `opengles` pipe → パケット組立て →
AOSP 互換 GLES1/GLES2/renderControl decoder → ホスト EGL/GLES → ANGLE Metal →
color buffer の post → 既存表示経路。

既存の AOSP プロトコル定義と生成器を固定リビジョンで使用し、opcode を
推測した手書きの一部実装で代用しない。Android 4、5、6 の定義差を比較し、
受理する opcode、引数形式、戻り値、拡張を互換表に記録する。
ANGLE の GLES1 対応も採用リビジョンと Metal バックエンドで実行検証する。
その経路で必要機能が成立しない場合は、AOSP の GLES1 変換層を評価し、
対応済みと通知する前に実動作を確認する。

代替案はゲスト内のソフトウェア GLES 実装の差し替え、または独自の Metal
GLES 実装である。前者はゲストイメージ変更と CPU 負荷、後者は API 全体の
実装・検証負担が大きいため、既存デコーダと ANGLE の統合を優先する。

## コンポーネント境界

1. QEMU pipe アダプター: 接続、短い read/write、poll、wake、close を担当。
   guest DMA 検証を保持し、C ABI でレンダラーと接続する。
2. プロトコル層: 長さ付きパケット、分割入力、同期応答、opcode dispatch。
   各命令について実処理または規定のエラーを返す。
3. renderControl 資源管理: context、surface、color buffer、共有グループと
   ゲストハンドルを管理する。ホストのポインタをゲスト ID に流用しない。
4. EGL/GLES バックエンド: ANGLE の初期化、config 列挙、context の current、
   shader、texture、FBO、EGLImage、同期と読み戻しを実行する。
5. iOS 統合: 初期化、画面 post、メモリ制約、バックグラウンド移行、停止処理。
   UIKit の操作はメインスレッドに限定する。

## 通信・並行実行

pipe 接続ごとに入力組立てと応答キュー、current context を分離する。
一つのゲスト書き込みが一つの命令であると仮定しない。ヘッダーの途中、
payload の途中、複数命令の連結、短い読み戻しを扱う。
長さの整数オーバーフローと上限を割当て前に検証する。
キュー満杯では既存 pipe の短い転送／再試行規約を使用する。
unknown opcode や不正パケットは接続単位で処理し、成功を偽装しない。

GPU の実行待ちは QEMU の MMIO 処理や主ロックを保持した状態で行わない。
レンダースレッドからの通知は QEMU の安全なイベント経路で IRQ に反映する。
close、reset、stop と処理中コールバックの競合を防止する。
共有資源の参照数と GPU 完了状態を確認してから破棄する。

## gralloc・表示・同期

color buffer の作成、登録、CPU 更新、GL 描画、readback、post を一貫した
資源へ接続する。pixel format、stride、pack/unpack alignment、上下方向、
RGB565 と RGBA/BGRA の変換を画素比較で検証する。
EGLImage と renderbuffer/texture への binding は実際の共有画像を参照させる。
FBO 完全性は実際の attachment に基づいて返す。

最初の正確性検証では GPU post を BGRA フレームへ読み戻して既存 mailbox
へ送る。GPU 完了前にバッファを再利用しない。CPU framebuffer と GPU post
の表示所有権を明示し、古い CPU フレームによる上書きを防ぐ。
ゼロコピー最適化はこの正確性が確認できた後の独立した変更とする。

## 起動と失敗

ゲスト起動前に EGL 初期化、GLES1/2 config、context 作成、最小描画と readback
を確認する。その成功後に限り、カーネル引数と boot-properties の両方で
GPU 対応を通知する。同じ初期化結果を参照し、値を別々に固定しない。
GPU モードを要求した起動で失敗した場合は具体的なエラーを表示する。
software framebuffer モードを残す場合は GLES2 非対応であることを明示する。
広告する extension は decoder と backend の両方が実装するものに限定する。

## ビルドと配布

ANGLE、AOSP decoder、生成器の取得元と commit を lock に記録する。
生成器はビルドホストで実行し、成果物は arm64 iPhoneOS 向けにコンパイルする。
macOS 用 dylib や simulator バイナリを IPA に混入させない。
静的リンクまたは同梱 framework を使用し、実行時の外部ライブラリ取得を要求しない。
既存の framework パッケージ検証とソース配布・ライセンス収集に追加する。

## 完了条件

- パケット分割、連結、不正長、背圧、切断、並行接続をホスト試験で検証。
- ASan/UBSan と既存 C++/Python 試験が成功する。
- opcode/API 互換表で対象 GLES1/2 と renderControl の未実装をなくす。
- EGL config/context、shader compile/link、draw/readback、texture/FBO、
  EGLImage、共有 context、同期、資源解放の実バックエンド試験が成功する。
- iPhoneOS SDK による C/C++/Objective-C++/Swift のコンパイルとリンクが成功する。
  Linux の構文チェックを iOS ビルド成功の代用にしない。
- Android 4.x、5.x、6.0 の各使用イメージの識別情報と iOS 実機情報を記録し、
  起動、Launcher、Browser、独立 WebView、スクリーンショットを検証する。
- アプリ切替、回転、バックグラウンド復帰、メモリ圧迫、VM 停止で破綻しない。
- 提示ログの再現手順を実施し、GL 初期化と実描画の両方を確認する。

## 現環境の制約と報告

現在の作業環境は Linux で Xcode/iPhoneOS SDK と検証用 Android イメージがない。
移植可能部分の試験とビルド入力整備はここで実行できるが、iOS ビルドおよび
実機試験の結果は別途取得が必要である。実装済み、試験成功、未検証を別々に
報告し、スタブ、未接続経路、未実行試験を含む状態を「完全実装」と呼ばない。
