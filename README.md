# aipad

AIエージェントの状態を、キーの上に表示するマクロパッドです。

[Keylink Studio](https://github.com/hrmt-lab/Keylink-Studio)と組み合わせると、
CodexやClaude Codeのセッションの状態(「作業中」「承認待ち」「入力待ち」「完了」「エラー」)を4つのScreenKeyに表示します。


## 構成



| | |
|---|---|
| MCU | Seeed XIAO nRF52840 **Plus**（別基板、24ピンFPCで接続） |
| ディスプレイ | [Screenkey](https://docs.waveshare.com/0.85inch-ScreenKey-Module-W?variant=0.85inch+ScreenKey+LCD+W) ×4（SPI共有、CSのみ個別） |
| キー | 3行4列マトリクス、実キー11個 |
| エンコーダ | Alps EC12（プッシュ無し） |
| LED | WS2812B-V6 ×4（4個まとめて1つの表示） |
| 接続 | USBのみ |


## 表示

| セッションの状態 | 見た目 |
|---|---|
| 作業中 | 青（`#3B82F6`）の線が枠を回る |
| 承認待ち | 黄（`#FACC15`）の枠が点滅 |
| 入力待ち | オレンジ（`#F97316`）の枠が呼吸するように明滅 |
| 完了 | 緑（`#22C55E`）の枠とLED。15秒で消灯 |
| エラー | 赤（`#EF4444`）の枠が点滅 |
| 待機中 | ロゴのみ |
| セッション無し | 消灯 |

ロゴはクライアントの種類で変わります（Codex / Claude Code）。

最大4セッションまで同時表示可能。

### LEDインジケータ

WS2812B ×4は、4個まとめて1つのインジケータとして動きます。
**ユーザーが対応しないといけない状態**と**完了**のときだけ光り、それ以外（作業中・待機中・
セッション無し）は消灯します。4画面のうち1つでも対象状態になれば点灯します。

| 状態 | 見た目 |
|---|---|
| 承認待ち | 黄が点滅 |
| 入力待ち | オレンジが呼吸するように明滅 |
| エラー | 赤が点滅 |
| 完了 | 画面と同じ緑（`#22C55E`）が15秒点灯（点滅なし） |
| それ以外 | 消灯 |

完了の緑は、その画面の緑枠と同時に15秒で消えます。保持中に別のセッションが完了すると
15秒を取り直します。

複数の画面が同時に対象状態のときは、承認待ち＞入力待ち＞エラー＞完了の優先順位で1色に
なります。完了は最下位なので、対応が必要な状態を隠しません。
技術的な詳細は[docs/hardware.md](docs/hardware.md)を参照してください。

## 必要なもの

- [Keylink Studio](https://github.com/hrmt-lab/Keylink-Studio) — ホスト側アプリ。
  AIクライアントの状態を検出してキーボードへ送ります
- [zmk-raw-hid](https://github.com/hrmt-lab/zmk-raw-hid) — RawHIDトランスポート
- [zmk-rawhid-app](https://github.com/hrmt-lab/zmk-rawhid-app) — AI Client State、
  Config RPC、Comboランタイムなどのアプリ層

モジュールは`config/west.yml`で参照しているので、個別に用意する必要はありません。


## キーマップ

[Keylink Studio](https://github.com/hrmt-lab/Keylink-Studio)またはZMK Studioで編集できます。
ロックは無効なので`&studio_unlock`キーは不要です。

エンコーダはKeylink Studioから編集できます。

## ドキュメント

| | |
|---|---|
| [docs/hardware.md](docs/hardware.md) | ピン割り当て、回路、周辺機能の競合、実装上のハマりどころ |
| [docs/building.md](docs/building.md) | ビルドの詳細、診断用ビルド、ログ設定 |
| [docs/bring-up.md](docs/bring-up.md) | 新規基板の立ち上げとトラブルシュート |
| [docs/ai-client-state-renderer.md](docs/ai-client-state-renderer.md) | Rendererの設計と契約 |

## 状態

実機で動作確認済みです。

| | |
|---|---|
| キーマトリクス 11キー | ✅ |
| ScreenKey 4枚 | ✅ |
| エンコーダ | ✅ |
| Keylink Studioとの通信 | ✅ |
