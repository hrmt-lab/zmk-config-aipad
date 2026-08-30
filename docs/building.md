# ビルド

## GitHub Actions

このリポジトリをforkしてpushすると、`.github/workflows/build.yml`がUF2を生成します。
Actionsの成果物から`aipad`をダウンロードしてください。

## ローカルビルド

west workspaceを用意して、次を実行します。

```bash
cd <zmk-workspace>
R=$(pwd)/config/zmk-config-aipad
west build -s zmk/app -d /tmp/aipad -p always \
  -b 'xiao_ble//zmk' -S studio-rpc-usb-uart -- \
  -DZMK_CONFIG=$R/config \
  "-DZMK_EXTRA_MODULES=$R" \
  '-DSHIELD=aipad raw_hid_adapter'
```

`ZMK_CONFIG`がリポジトリrootではなく`config/`を指している点に注意してください。理由は下記。

モジュールをworkspace内の別の場所で管理している場合は、`ZMK_EXTRA_MODULES`に
セミコロン区切りでそのパスも並べます。west manifestはworkspace root側のcloneへ
解決するため、正本が別にあるならここで明示的に向ける必要があります。

```bash
  "-DZMK_EXTRA_MODULES=$R;$(pwd)/config/zmk-rawhid-app;$(pwd)/config/zmk-raw-hid"
```

## 書き込み

XIAOのリセットボタンを素早く2回押すとブートローダーに入り、USBマスストレージとして
現れます。そこへUF2をコピーしてください。

既定のキーマップなら、3行目3列目のキーが`&bootloader`です。

## keymapは `config/` にしか置かない

keymapは`config/aipad.keymap`に置きます。

ZMKの`app/boards/post_boards_shields.cmake`は`KEYMAP_DIRS`へ`ZMK_CONFIG`を**先頭に挿入**し、
最初に見つかった1つだけを使います。したがって`ZMK_CONFIG`配下が最優先です。

shield配下（`boards/shields/aipad/`）には置かないでください。両方に置くと、
片方が気づかないうちに古くなります。

この規約の結果、**`ZMK_CONFIG`はリポジトリrootではなく`config/`を指す必要があります**。
shieldは`ZMK_EXTRA_MODULES`と`zephyr/module.yml`の`board_root: .`経由で見つかります。

ビルドログに次が出ることを確認してください。

```
-- Using keymap file: .../config/aipad.keymap
```

## `.conf` は追加であって上書きではない

shield配下の`.conf`（`boards/shields/aipad/aipad.conf`）と`ZMK_CONFIG`側の`.conf`は
**両方が適用されます**。後者が前者を上書きするわけではありません。
shieldの既定値とユーザー設定を両立できます。

## 診断用ビルド

いずれも通常ビルドには含まれません（Kconfigの既定は`n`）。

### セルフテスト版

Hostと無関係に起動直後から4画面へ描画し、パネル信号のピンウォークとショートチェックを行います。

```bash
  -DCONFIG_AIPAD_DISPLAY_SELFTEST=y -DCONFIG_ZMK_USB_LOGGING=y \
  -DCONFIG_ZMK_LOGGING_MINIMAL=y -DCONFIG_ZMK_LOG_LEVEL_INF=y \
  -DCONFIG_LOG_BUFFER_SIZE=16384
```

### エンコーダプローブ版

エンコーダ候補ピンのレベル・エッジ数・プルアップ有無を継続的にログへ出します。

```bash
  -DCONFIG_AIPAD_ENCODER_PROBE=y -DCONFIG_GPIO_HOGS=n \
  -DCONFIG_ZMK_USB_LOGGING=y \
  -DCONFIG_ZMK_LOGGING_MINIMAL=y -DCONFIG_ZMK_LOG_LEVEL_INF=y \
  -DCONFIG_LOG_BUFFER_SIZE=16384
```

`CONFIG_GPIO_HOGS=n`を忘れないでください。hogがP0.05をLowに固定したままだと、
そこにエンコーダのチャンネルがあっても見えません。

### LEDセルフテスト版

起動直後にWS2812B-V6チェーンの先頭から赤→緑→青を流します（詳細は
[bring-up.md](bring-up.md)参照）。

```bash
  -DCONFIG_AIPAD_STATUS_LED_SELFTEST=y \
  -DCONFIG_ZMK_USB_LOGGING=y \
  -DCONFIG_ZMK_LOGGING_MINIMAL=y -DCONFIG_ZMK_LOG_LEVEL_INF=y \
  -DCONFIG_LOG_BUFFER_SIZE=16384
```

`CONFIG_AIPAD_DISPLAY_SELFTEST`とは独立したKconfigなので、12秒のバックライトbeaconや
8秒のピンウォークを待たずにLEDだけを確認できます。

### LEDチェーン診断版

セルフテストで「1個目だけ正しく光り、2〜4個目が光らない」ことまでは分かっても、
「一度も正しく光らないのか」「たまに正しく光るのか」「化けた値を掴んだまま
更新されないのか」までは、起動1回・各ステップ200msのセルフテストでは切り分けられません。
このビルドはそれを見るための診断版で、10個の固定フレームを**無限に**繰り返します。
1フェーズ2秒保持し、その間500msごとに同じフレームを再送します（1フェーズ4回書き込み、
「色が変わっていなければ書かない」通常の最適化は通しません）。1周は20秒です。

```bash
  -DCONFIG_AIPAD_STATUS_LED_CHAIN_PROBE=y \
  -DCONFIG_ZMK_USB_LOGGING=y \
  -DCONFIG_ZMK_LOGGING_MINIMAL=y -DCONFIG_ZMK_LOG_LEVEL_INF=y \
  -DCONFIG_LOG_BUFFER_SIZE=16384
```

フェーズは次の順で、10まで行ったら1へ戻ります。

| # | フェーズ | 内容 |
|---|---|---|
| 1 | ALL RED | 4画素すべて赤 (128,0,0) |
| 2 | ALL GREEN | 4画素すべて緑 (0,128,0) |
| 3 | ALL BLUE | 4画素すべて青 (0,0,128) |
| 4 | ALL WHITE | 4画素すべて白 (128,128,128) |
| 5 | WALK 0 | 画素0だけ白 (255,255,255)、他は消灯 |
| 6 | WALK 1 | 画素1だけ白 |
| 7 | WALK 2 | 画素2だけ白 |
| 8 | WALK 3 | 画素3だけ白 |
| 9 | DISTINCT | 画素0=赤 / 1=緑 / 2=青 / 3=白（各128） |
| 10 | ALL OFF | 4画素すべて消灯 |

4画素同時に光るフェーズは明るさ128（半分）に抑えています。VCC_3V3がMCUとチェーンの
両方を賄っているためで、`AIPAD_STATUS_LED_SELFTEST`のALL WHITEフェーズと同じ理由です。
単画素のWALKフェーズだけは255（1画素なら約20mA）です。

各フェーズの先頭で`LOG_INF`が1行出ます（フェーズ名、`write_count`、直近の`last_err`）。
USB CDCのログバックエンドが立つのは起動20秒前後なので、1周目の早いフェーズはログに
乗らないことがありますが、無限ループなので2周目以降は必ず出ます。見え方の判定は
[bring-up.md](bring-up.md)を参照してください。

`AIPAD_STATUS_LED_SELFTEST`と同時には有効化できません（Kconfigで
`depends on !AIPAD_STATUS_LED_SELFTEST`としています）。両方ともストリップを
排他的に握るためです。またこの診断モードでは、ホストのAI client state更新を
受け取るリスナー自体をビルドから外しています。フェーズの2秒/500ms周期を
`k_work_reschedule(..., K_NO_WAIT)`で前倒しされると崩れてしまうためです。

## ログの設定

`CONFIG_ZMK_LOGGING_MINIMAL=y`と`CONFIG_ZMK_LOG_LEVEL_INF=y`はセットで指定します。
ZMKは`ZMK_LOG_LEVEL`に`default 4`（DEBUG）を与えており、`ZMK_LOGGING_MINIMAL`を立てないと
choiceで指定したINFが効きません。DEBUGのままだとRawHIDのhex dumpでログが埋まります。

### 起動直後のログは失われる

USB CDCのログバックエンドができるのは起動から20秒前後です。それ以前のログは
バッファに溜まりますが、溢れると失われます。

そのため、起動時に判明した内容（各パネルのready状態、ショートチェックの結果、
エンコーダピンの特性）は保持しておき、**USB列挙後に繰り返しログへ出し直す**構造にしています。
`status_screen.c`の`walk_report_cb()`と`encoder_probe.c`の集計行がそれです。

## `zmk-rawhid-app` の規約

`CONFIG_RAWHID_APP_COMBO_RUNTIME=y`のとき、Combo定義は`.keymap`に置き、
`/combos`の`status = "disabled"`は`.overlay`側に書きます。
これによりZMK標準のCombo listenerは生成されず、Keylink runtime Combo engineが
`.keymap`の定義を既定値として実行します。

ビルド時に`verify_combo_listener_order.cmake`がlistener順と標準listenerの不在を検証し、
成功すると次が出ます。

```
-- Keylink Combo listener order verified: Keylink -> activity -> hold-tap -> keymap; stock Combo listener absent
```

## モデルテスト

Rendererの状態機械とアニメーション計算はdevicetreeにもZephyrにも依存しない純関数に
分けてあるので、ホスト側でテストできます。

```bash
bash tools/test-renderer-model.sh
```

## ロゴの再生成

いずれのロゴも96×96のLVGL向けRGB565配列（little-endian、`stride=192`、
`data_size` 18,432 byte）で、元画像を黒背景へ合成し、ScreenKeyの直立方向へ向けて
90度反時計回りに配置します。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\generate-codex-logo.ps1
```

```bash
python3 tools/generate-claude-code-logo.py
```

生成済みの`codex_logo.c`と`claude_code_logo.c`はリポジトリへ含めるため、
firmware build時に画像変換ツールは不要です。4画面とも同じロゴassetを共有します。

## 画面数を変えるとき

次の3つを**同じ値**に保ってください。

| 場所 | 内容 |
|---|---|
| `src/screenkey_renderer_model.h` | `SCREENKEY_RENDERER_SCREEN_COUNT` |
| `boards/shields/aipad/aipad.conf` | `CONFIG_RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT` |
| `boards/shields/aipad/aipad.overlay` | `zephyr,mipi-dbi-spi`ノードの数と`cs-gpios`の本数 |

加えて`status_screen.c`の`extra_lcds[]`にパネルを足します。

## LEDの色・明るさを変えるとき

上限の明るさ（既定40%）は`src/screenkey_renderer_model.h`の`SCREENKEY_LED_MAX_LEVEL`
1定数で決まります。呼吸表示（入力待ち）はこの値を上限に10%〜40%で明滅するので、
ここを変えるとブレークもピークも一緒に動きます。

各状態の色そのものは`src/screenkey_renderer_model.c`の`screenkey_led_color_for()`内、
`SCREENKEY_LED_WAITING_APPROVAL` / `SCREENKEY_LED_WAITING_INPUT` / `SCREENKEY_LED_ERROR` /
`SCREENKEY_LED_COMPLETED`の各caseにあります。完了だけは点滅も呼吸もせず、上限の明るさで
点灯し続けます。

完了の点灯時間は`src/screenkey_renderer_model.h`の`SCREENKEY_COMPLETED_HOLD_MS`（既定15秒）で、
ScreenKeyの緑枠とWS2812Bが同じ値を読みます。ここを変えると両方が一緒に動くので、
片方だけ変えたい場合以外は定数を書き換えるだけで済みます。
