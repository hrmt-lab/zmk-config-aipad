# ScreenKey AI Client State Renderer仕様

## 入力と責務

Rendererは`zmk-rawhid-app`の`rawhid_app_ai_client_state_changed` eventを購読し、
`session_active`と`activity_state`から表示modeを選ぶ。packet validation、
revision判定、heartbeat抑制、15秒Host timeoutはCoreの責務であり、Rendererへ複製しない。

`WORKING`の表示は`work_phase`によらず常に青い移動線ひとつである。`work_phase`はHost側の
診断用に受信を続けるが、表示modeの選択には使わない。したがって`THINKING`と
`EXECUTING`／`SEARCHING`／`UNSPECIFIED`の間で表示は変わらず、同じanimationが継続する。

## 複数ScreenKeyと論理表示slot

本shieldは物理ScreenKeyを4枚持つ。eventの`display_slot`で描画先を選び、
`display_slot n`を物理ScreenKey `n`へ写像する。`screenkey_renderer_screen_for_slot()`が
この写像を担い、対応する画面が無いslotには`SCREENKEY_RENDERER_NO_SCREEN`を返す。
そのeventは何も描画せずに捨てる。slotを画面0へ折り返さない。

表示state（mode、animation frame、点滅位相、COMPLETEDの15秒timer、backlight要求）は
画面ごとに独立して持つ。あるslotの更新が他のslotのanimationを再開始させることはない。

ただしbacklightは実機で4枚がPWM 1本（P0.15、PAM2804のEN×4）を共有するため、
出力だけは共有資源として扱う。
各画面は自分のbacklight要求を保持し、いずれかの画面が点灯を要求している間は点灯を維持し、
すべての画面がOFFになったときだけ消灯する。あるslotが`NONE`になっても、
他のどれかが表示中なら消灯しない。

`ZMK_DISPLAY_WIDGET_LISTENER`は単一のsnapshotへcoalesceするため使わない。
異なるslotのeventが連続して届いた場合に先のものが失われるからである。代わりに
画面ごとのpending entryを持つlistenerを`status_screen.c`に直接書き、display work queueで
dirtyな画面だけを描画する。ZMK eventのcallbackが状態を記録するだけで、LVGL操作は
display work queue上で行うというthreading契約は同じである。

画面1〜3はZMKのdisplay moduleが扱わないため、`src/screenkey_display.c`が
`lv_display_create()`とflush callbackを自前で登録する。登録に失敗した画面は
LOG_ERRを残してskipし、残りの画面はそのまま動作を続ける。

wire側のslot契約は
[`zmk-rawhid-app`の`docs/ai-client-display-slot.md`](https://github.com/hrmt-lab/zmk-rawhid-app/blob/develop/docs/ai-client-display-slot.md)
を参照する。

## AI client typeとロゴ

`client_type`はロゴの選択にだけ使い、状態表現には使わない。

| `client_type` | ロゴ |
|---:|---|
| `0x01` Codex | `screenkey_codex_logo` |
| `0x02` Claude Code | `screenkey_claude_code_logo` |
| 上記以外 | `screenkey_codex_logo`（fallback。Coreが未知typeをrejectするため通常は届かない） |

Claude Codeでも、以下の状態表現・色・animation・timerは Codex と完全に同じものを適用する。
ロゴ以外にclient type固有の表示分岐を追加しない。

ScreenKeyは`CONFIG_RAWHID_APP_AI_CLIENT_CLAUDE_CODE_RENDERER=y`を設定し、
DEVICE_HELLOで`CAP_AI_CLIENT_CLAUDE_CODE`（bit 12）を広告する。

| state | 表示 |
|---|---|
| `WORKING + THINKING` | 既存の青い移動線 |
| `WORKING + EXECUTING` | 既存の青い移動線 |
| `WORKING + SEARCHING` | 既存の青い移動線 |
| `WORKING + UNSPECIFIED` | 既存の青い移動線（legacy fallback） |
| `WAITING_APPROVAL` | 黄`#FACC15`の点滅枠 |
| `WAITING_INPUT` | オレンジ`#F97316`の呼吸枠 |
| `AVAILABLE` | ロゴ表示、枠なし |
| `COMPLETED` | 既存の緑枠（`#22C55E`）を15秒表示。共有LEDは同じ15秒だけ純緑`(0, 0xC5, 0)`で点灯（枠と同じ数値ではない。理由は[hardware.md](hardware.md)参照） |
| `ERROR` | 既存の赤点滅枠 |
| sessionなし／`NONE`／未知activity | 画面とbacklightをOFF |

## Animation

移動線と呼吸枠は100 msごとの20 frame、2秒周期で進める。呼吸opacityはRenderer modelの
純粋関数で計算し、mode開始時のframe 0を64、frame 10を255とする。

```text
normalized = frame % 20
distance = normalized <= 10 ? normalized : 20 - normalized
opacity = 64 + ((255 - 64) * distance) / 10
```

frame 20はframe 0へ折り返す。Coreが同一heartbeatのeventを発行しないため、同一state、
work phase、revisionの再送ではanimationをframe 0へ戻さない。

## ロゴasset

96×96 RGB565、little-endian byte order、`lv_image_dsc_t`（`w=96 h=96 stride=192`、
`data_size` 18,432 byte）で、ScreenKeyの直立方向へ向けて元画像から90度反時計回りに配置する。

| ロゴ | 元画像 | 生成 |
|---|---|---|
| Codex | `assets/codex_icon_transparent.png`（640×640） | `tools/generate-codex-logo.ps1`（Windows／System.Drawingで96×96へ縮小） |
| Claude Code | `assets/claude_code_screenkey_96.png`（96×96） | `tools/generate-claude-code-logo.py`（Python標準ライブラリのみ） |

Claude用assetは既に96×96のため縮小しない。`generate-claude-code-logo.py`は96×96以外を
拒否し、リサンプラの実装差をrepositoryへ持ち込まない。生成物は
`boards/shields/aipad/src/{codex,claude_code}_logo.c`で、同じ入力からは常に同じ内容になる。

## 変更しない表示契約

- 青い移動線の長さ、形状、開始方向を変更しない。
- 承認待ちの黄色点滅、ERRORの赤点滅、COMPLETEDの保持時間を変更しない。
  保持時間は`SCREENKEY_COMPLETED_HOLD_MS`（15000 ms）1定数で、緑枠と共有LEDの両方が読む。
  片方だけ別の値にしない。
- Host Linkへ色、frame、周期を追加しない。表示定数はRenderer内で管理する。
- Host Linkへclient type固有の会話本文、session ID、hook event名を追加しない。
- 1画面へ複数sessionを同時描画しない。1画面が表示するのは常に1 slot分だけである。

この節はHost Linkへ`display_slot`（8 byte payloadのoffset 7、`0..=7`）が加わったことに
合わせて改訂した。`display_slot`は**描画先の選択**にだけ使う論理slot番号であり、
screen ID、thread ID、session IDではない。会話本文やsession IDをwireへ載せない制約は
これまでどおり維持する。

## テスト

リポジトリrootで次を実行する。

```bash
bash tools/test-renderer-model.sh
```

ホストCテストは全表示mode、未知work phaseのlegacy fallback、移動線の全20 frameと開始方向、
呼吸opacityの全20 frame、最小／最大、単調増減、周期折り返し、client typeごとのロゴ選択、
`display_slot`から物理画面indexへの写像と、対応する画面が無いslot（`2`〜`7`と範囲外）が
`SCREENKEY_RENDERER_NO_SCREEN`になることを検証する。
変更後は`git diff --check`と`aipad`のfresh UF2 buildも行う。
