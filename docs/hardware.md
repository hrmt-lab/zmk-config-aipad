# ハードウェアとピン割り当て

aipadの回路構成と、devicetreeがそれをどう表現しているかをまとめたノートです。
基板を改版するとき、あるいは配線を疑うときに読んでください。

## 全体構成

MCUは別基板にあり、24ピンFPC（回路図の`J1`）でキーボード基板と繋がります。
キーボード基板側の電源はFPC pin 3の`VCC_3V3`だけで、`VBUS`（pin 1）は配線していません。
**XIAOの3.3Vレギュレータ出力がMCU自身の電源と共通**なので、キーボード基板側の過負荷は
そのままMCUの電源品質に影響します。

XIAO nRF52840 **Plus**は通常のXIAOがブレイクアウトしないピンも引き出しています。
`seeed_xiao`コネクタ（`&xiao_d`）はD0〜D10しか定義していないため、
overlayではすべて`&gpio0` / `&gpio1`の直接参照で統一しています。

## ピン割り当て

| J1 | ネット | nRF52840 | XIAO | 用途 |
|---:|---|---|---|---|
| 4 | CS-4 | P1.15 | D10 | ScreenKey 4 CS → `aipad_lcd3` → slot 3 |
| 5 | SCLK | P1.03 | — | SPI SCK（4枚で共有） |
| 6 | CS-2 | P1.14 | D9 | ScreenKey 2 CS → `aipad_lcd1` → slot 1 |
| 7 | DC | P1.05 | — | データ／コマンド（4枚で共有） |
| 8 | CS-1 | P1.13 | D8 | ScreenKey 1 CS → `aipad_lcd0` → slot 0 |
| 9 | CS-3 | P1.07 | — | ScreenKey 3 CS → `aipad_lcd2` → slot 2 |
| 10 | MOSI | P1.12 | D7 | SPI MOSI（4枚で共有） |
| 12 | RST | P0.02 | D0 | パネルRESET（4枚で共有） |
| 13 | PWM | P0.15 | — | バックライト（PAM2804のEN ×4を共有） |
| 14 | ROW0 | P0.03 | D1 | matrix row 0（ScreenKeyの行） |
| 15 | COL0 | P0.19 | — | matrix column 0 |
| 16 | COL1 | P0.28 | D2 | matrix column 1 |
| 17 | ROW1 | P1.01 | — | matrix row 1 |
| 18 | ROW2 | P0.29 | D3 | matrix row 2 |
| 19 | RE-A | P0.09 | — | エンコーダA（NFC1パッド） |
| 20 | RE-B | P0.04 | D4 | エンコーダB |
| 21 | COL2 | P0.10 | — | matrix column 2（NFC2パッド） |
| 22 | LED | P0.05 | D5 | WS2812B ×4 DIN（未実装、GPIO hogでLow固定） |
| 24 | COL3 | P1.11 | D6 | matrix column 3 |

ScreenKeyの物理配置は**左からCS-1、CS-2、CS-3、CS-4**（＝slot 0〜3）です。

## SCLKとCS-3は隣り合っていて紛らわしい

`P1.03`がSCLK、`P1.07`がCS-3です。**この2本を逆にすると、パネルにクロックが届かなくなります。**

厄介なのは、パネルがwrite-onlyで読み返しが無いため`st7735r_init()`が成功を返してしまうことです。
ログには`lcd0: ready`と出るのに何も映らない、という紛らわしい状態になります。

判定方法は[bring-up.md](bring-up.md)のベースライン測定にあります。CS 4本には10Kプルアップが
付いているので、「内部プルダウンをかけてもHighのままの線」がCSです。SCLKにプルアップはありません。

## 周辺機能の競合

| 競合 | 対処 |
|---|---|
| `i2c1`の既定がP0.04／P0.05 = エンコーダBとWS2812Bデータ線 | overlayで`&i2c1`を`disabled` |
| `spi2`の既定がP1.13／P1.14／P1.15 = CS 3本 | pinctrlでSCKをP1.03、MOSIをP1.12へ張り替え |
| `uart0`の既定がP1.11（TX）／P1.12（RX）= COL3とMOSI | `xiao_ble_zmk.dts`が`&xiao_serial`を既に`disabled`にしている。ZMK StudioのRPCは`studio-rpc-usb-uart` snippet経由でUSB CDCを使う |
| P0.09／P0.10がNFCアンテナパッド | overlayの`&uicr { nfct-pins-as-gpios; };`でGPIO化 |
| QSPI（P0.20〜P0.25）、電池計測（ADC ch7 = P0.31 / `power-gpios` P0.14） | 競合なし |

### NFCピンのGPIO化

**`&uicr { nfct-pins-as-gpios; };`が無いとCOL2とエンコーダAが死にます。**
Kconfigの`CONFIG_NFCT_PINS_AS_GPIOS`は現行Zephyrでdeprecatedなので使いません。

`zephyr/modules/hal_nordic/nrfx/CMakeLists.txt`がこのDTプロパティを読んで
`system_nrf52840.c`に`NRF_CONFIG_NFCT_PINS_AS_GPIOS`を定義し、起動時にUICRを書いて
自分でリセットします。`UICR.NFCPINS`はP0.09とP0.10を1つのビットで制御するので、
片方だけGPIOにすることはできません。戻すにはUICR eraseが必要です。

生成`.config`側は`# CONFIG_NFCT_PINS_AS_GPIOS is not set`のままで正常です（経路が違います）。

## ScreenKey ×4

4枚のST7735S（128×128）が同じSPIバスとDC／RESETを共有し、chip selectだけを分けます。
`zephyr,mipi-dbi-spi`の子ノードの`reg`が親の`cs-gpios`のindexに解決されます。

MISOは配線しないため`write-only`を指定します。

### RESETは最初の1ノードにだけ書く

RSTが4枚で物理的に共有なので、`zephyr,mipi-dbi-spi`を1ノードにまとめたり、
全ノードに`reset-gpios`を書いたりすると、後発パネルの初期化時のハードウェアリセットが
**設定済みのパネルまで巻き込んでリセット**し、先に初期化されたパネルが無表示になります。

対策としてコントローラを4ノードに分け、`reset-gpios`は`aipad_mipi_dbi0`にだけ持たせています。
`reset-gpios`が無いノードはst7735rドライバがソフトウェアリセット（自分のCS経由）へ
fallbackするため、他を壊しません。

**devicetreeの並び順がそのままinit順になる**ので、`aipad_mipi_dbi0`は先頭に置いたままにしてください。

### バックライト

PWM 1本（P0.15）を4枚で共有します。Rendererは「どれかの画面が表示中なら点灯、
全部OFFになって初めて消灯」という扱いにしています。

明るさは`led_set_brightness()`の100が上限で、そのとき`pwm_nrfx`はPWMを動かさず
**P0.15を静的Highに固定**します。実際の光量はPAM2804の電流設定抵抗（5R6）で決まるので、
ソフトウェア側でこれ以上明るくする余地はありません。

### 追加画面のLVGL登録

ZMKのdisplay moduleとZephyrのLVGL glueはどちらも`chosen zephyr,display`の1枚しか扱いません。
画面1〜3は`src/screenkey_display.c`が自前で`lv_display_create()`とflush callbackを登録します。
`lv_task_handler()`はZMKのdisplay tickが全displayを回すので、追加のtimerは不要です。

登録に失敗した画面はskipされ、残りはそのまま動きます。

## エンコーダ回路

基板はAlps推奨のフィルタ回路です。

```
VCC ──[10K プルアップ]──┬── エンコーダ端子 ──接点── C端子 ── GND
                        └──[10K 直列]──┬── MCU ピン
                                       └──[0.01µF]── GND
```

### 内部プルアップを付けてはいけない

この回路は**MCU入力がハイインピーダンスであること**が前提です。
`GPIO_PULL_UP`でnRF52840の内部プルアップ（約13K）を有効にすると直列10Kと分圧になり、
接点が閉じても

```
3.3V × 10 / (10 + 13) = 1.43V
```

までしか下がりません。`V_IL`は`0.3 × VDD = 0.99V`なので**Lowとして認識されず、
エンコーダは一切反応しません**。`a-gpios` / `b-gpios`に`GPIO_PULL_UP`を付けないこと。

エンコーダをピンへ直結する（直列抵抗の無い）基板なら内部プルアップは無害ですが、
**この基板は違います**。

### steps の根拠

Alps **EC12E1220301** = 12ディテント／12パルス毎回転。

ZMKのEC11ドライバは直交信号の**遷移ごと**に±1を数えるため、12パルス／回転なら
1回転は`12 × 4 = 48`です。`triggers-per-rotation = 12`で30°ごと、
すなわち1ディテントにつき1回の入力になります。

ドライバ自体は`alps,ec11`ノードの存在で自動的に有効になりますが、trigger modeの既定は
`none`でサンプリングされないため、`.conf`に`CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y`が必要です。

### RC(2,3)

実装したEC12E1220301にプッシュスイッチが無いため、キーマップでは`&none`です。
ただし基板側は`D12`でエンコーダのS1/S2パッドをマトリクスへ引き込んであるので、
プッシュ付きエンコーダに載せ替えればdevicetreeを変えずに`RC(2,3)`が生きます。
そのため`matrix-transform`は12ポジションのまま残してあります。

## WS2812B（未実装）

P0.05にWS2812Bが4個デイジーチェーンで載っていますが、LEDドライバはまだ入れていません。
**ピンはGPIO hogでLowに固定**してあります。

```dts
&gpio0 {
    ws2812_din_idle {
        gpio-hog;
        gpios = <5 GPIO_ACTIVE_HIGH>;
        output-low;
    };
};
```

放置するとリセット後のP0.05は「切り離された入力」のままなので、1個目のDINが浮いて
ノイズをデータとして拾い、4個が任意の色（最悪フルホワイト）で点灯し得ます。
4×60mA = 240mAがXIAOの3.3Vレギュレータを通ると`(5−3.3)×0.24 ≈ 0.4W`が
レギュレータで熱になります。Lowに固定すればチェーンは静止したidleを見て消灯を保ちます。

hogは`POST_KERNEL 41`で走ります。GPIOドライバ（40）の直後で、他の何かがこのピンに触るより前です。
`CONFIG_GPIO_HOGS`はdevicetreeにhogノードがあれば自動で`y`になります。

将来、AI clientの状態に応じて色を変える用途に使う予定です。実装する際は:

- 状態→色の写像は`src/screenkey_renderer_model.c`側へ純関数として足すと、
  `tools/test-renderer-model.sh`でホスト側テストできる
- **`ws2812_din_idle`のhogノードを削除する**こと。残したままだとhogがピンを保持してLEDドライバと衝突する

## 電池

XIAOオンボードの分圧（ADC ch7 = P0.31、`power-gpios` P0.14）を使います。
FPCのP0.31は未接続ですが、XIAO側のBATパッドにセルを繋げばそのまま動きます。

## 実装上のハマりどころ

### FPCの裏表

12ピンFPC（ScreenKey側）は**pin 1とpin 12がどちらもGND**なので、逆向きに挿しても
導通確認では区別がつきません。実際には次のように入れ替わり、パネルへ電源が届きません。

| 逆挿し時 | 本来 | 実際に繋がる |
|---|---|---|
| pin 6 | VDD | DC |
| pin 7 | DC | VDD |
| pin 5 | LEDA | CS |
| pin 2 | LEDK | RST |

この状態ではバックライトも点かず、SPIも通らず、MCU側の測定はすべて正常に見えます。
症状が「パネルが完全に無反応」のときは、テスターを当てる前にFPCの向きを疑うのが早いです。

なお10Kと5R6の抵抗はpin 5とpin 2を通る電流経路に直列に入っているため、
0.5mmピッチに触らずに両端電圧で接触を判定できます。

24ピンFPC（本体側）にも同じ罠があります。全ピンが左右反転すると`N ↔ 25−N`になり、
GNDが繋がらないまま信号ピンだけが繋がるため、各信号のESDダイオード経由で電流が流れて
MCUが発熱します。

### WS2812Bの向き

180°回るとVDDとVSSが入れ替わり、通電した瞬間に内部の基板ダイオードが順方向になって
電源レールを短絡します。

テスターの試験電圧は順方向電圧に届かないため、**無通電では正常に見えて通電で壊れます**。
「無通電の測定は全部通るのに、電源を入れるとMCUが発熱してUSBが列挙されない」ときは、
極性部品の向きを疑ってください。
