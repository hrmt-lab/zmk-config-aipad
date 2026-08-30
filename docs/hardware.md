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
| 22 | LED | P0.05 | D5 | WS2812B-V6 ×4 DIN、`spi3`のSPIM MOSIで駆動 |
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
| `spi3`の既定（`spi3_default`）がP0.20／P0.21／P0.24 = QSPIフラッシュと同じピン | 専用の`aipad_spi3_default`でMOSIのみP0.05へ張り替え |
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

## WS2812B-V6 ×4 ステータスLED

P0.05のWS2812B-V6 ×4を`spi3`のSPIM MOSI（MOSI-only、クロック・MISO無し）で駆動します。
4個は`aipad_leds`（`chain-length = 4`）としてまとめて1本のチェーンになっていて、
Renderer側（`src/status_led.c` / `src/screenkey_renderer_model.c`）も4個を常に同じ色・
同じ明滅で書きます。表示の意味は[README.md](../README.md)を参照してください。

### なぜ`spi3`か

このボードで空いているSPI/SPIMペリフェラルは`spi3`だけです。

- `spi2`は4枚のScreenKey（ST7735S）が既に使っている
- `spi0`・`spi1`はそれぞれ`i2c0`・`i2c1`と排他（nRF52840はSPIMとTWIMでインスタンスを共有する）

`spi3`の board 既定pinctrl（`spi3_default`）はP0.21（SCK）／P0.20（MOSI）／P0.24（MISO）を使い、
これはQSPIフラッシュ（p25q16h）と同じピンです。そのままではSPI初期化がQSPIより先に
そのピンを取ってしまうため、専用の`aipad_spi3_default` / `aipad_spi3_sleep`をoverlayへ定義し、
MOSIだけをP0.05へ張り替えています（WS2812はクロックもMISOも不要）。

### SPIフレームとWS2812B-V6のビットタイミング

`worldsemi,ws2812-spi`は、WS2812の1ビットを1個の8bit SPIフレームへエンコードし、MSB firstで
送出します。フレーム内のHighビットの連続長がWS2812のHigh区間（T0H / T1H）に、その前後のLow
（フレーム末尾のLowビット＋次フレーム先頭のLowビット）がLow区間（T0L / T1L）になります。

**Highの並びがbit 7から始まるとは限りません。** 下記の`0x70` / `0x40`はどちらも先頭が
Lowビットです。フレーム値を検討するときは、位置ではなくビットを数えてください。

```dts
&spi3 {
    ...
    aipad_leds: ws2812@0 {
        compatible = "worldsemi,ws2812-spi";
        spi-max-frequency = <4000000>;
        chain-length = <4>;
        spi-one-frame  = <0x70>;
        spi-zero-frame = <0x40>;
        reset-delay = <300>;
        color-mapping = <LED_COLOR_ID_GREEN LED_COLOR_ID_RED LED_COLOR_ID_BLUE>;
    };
};
```

- `spi-max-frequency = <4000000>`: nRFのSPIMは1/2/4/8/16/32MHzしかロックできず、
  `get_nrf_spim_frequency()`（`zephyr/drivers/spi/spi_nrfx_spim.c`）は要求値をこの並びへ
  **黙って切り下げます**。4〜8MHzの間の値を書いても実際には4MHzへ切り下がるので、
  最初から4MHzを明示しています。1bit=250nsです。
- `spi-one-frame = <0x70>`（`0111_0000`）: Low 1bit → High 3bit → Low 4bit。
  T1H = 3 × 250ns = **750ns**。T1L = 末尾4bit + 次フレーム先頭の1bit = 5 × 250ns =
  **1250ns**。T1H（580ns〜1µs）・T1L（580ns〜1.6µs）の両方がV6の規格内に収まります。
- `spi-zero-frame = <0x40>`（`0100_0000`）: Low 1bit → High 1bit → Low 6bit。
  T0H = **250ns**で220〜380nsの規格内。T0L = 6 + 1 = 7 × 250ns = **1750ns**で、
  V6の公称上限1.6µsは超えますが、これはLowが長すぎる側なので無害です（理由は後述）。
- `reset-delay = <300>`: WS2812B-V6はRES（ラッチ／リセット）に**280µs超**を要求します。
  このワークスペースの他の構成が使っている70（プレーンなWS2812B向け）はV6には短すぎます。
- フレーム値を変える場合は**bit 0（LSB）を0のままにしてください**。SPIは転送の合間Lowに
  アイドルするため、フレームのLSBがLowであることが、そのアイドル電位とWS2812のLowを
  一致させる条件になっています。
- `color-mapping`はGRB（WS2812Bのバイト順どおり）。

#### 8MHz構成で起きていた不具合と、なぜ4MHzが正しいか

当初はこのボードだけ`spi-max-frequency = <8000000>` / `spi-one-frame = <0xFC>`（6bit High）/
`spi-zero-frame = <0xC0>`（2bit High）にしていました。1bit=125nsとしてT0H=250ns・T1H=750ns
はどちらも一見規格内でしたが、実際には**T1L（'1'ビット直後のLow）が250nsしかなく、
V6のT1L規格580ns〜1.6µsを大きく下回っていました**。

WS2812のデコーダはDINの立ち上がりエッジでワンショットを起動し、そのHigh幅をT0H/T1Hと
比べて0/1を判定してから、次の立ち上がりエッジまでを1ビットとして扱います。Low区間が
短すぎると、次のエッジが来る前に信号がLowへ落ち着けず、連続する'1'ビットが1本の長い
Highパルスとして読まれてビットが失われます。各LEDは自分がデコードした結果を下流へ
出し直すため、誤りはチェーンを下るほど積み上がります。実機では4個のLEDがそれぞれ
無関係な色に化け、しかもフレームを送るたびに化け方が変わる（そのフレームのビット並びに
依存する）という症状になって現れました。

このLow区間短縮は「T1Lは0/1判定に関与しないので問題ない」という理屈では正当化できません。
判定に使うのはT0H/T1Hだけというのは正しいのですが、T1Lが短すぎるとその判定の前提である
「次の立ち上がりエッジまでが1ビット」という区切り自体が壊れるため、無関係ではないのです。
また「SPIMは離散クロックしか取れないので、どの周波数でもT0H/T1HとT1L/T0Lの両立はできない」
というのも誤りです。そもそも8MHzでは**どんなフレーム値を選んでも解がありません**。8MHzでは
1フレーム全体が1000nsしかないのに対し、V6の'1'ビットはT1H ≥ 580ns・T1L ≥ 580nsで合計
1160ns以上を要求するためです。4MHzなら1フレーム2000nsあるので、`0x70`でT1H=750ns・
T1L=1250nsが両方同時に規格内へ収まります。Low側を長くする方向のトレードオフ（後述）は
無害ですが、短くする方向はデータ破壊に直結するため、詰めるべきはHigh側ではなくLow側でした。

4MHzでは1ビット周期がT0/T1とも2000nsになり、WS2812B-V6の公称ビット周期1.25µs±600nsの
上限を超えます。ただしこれが問題になるのはRES（ラッチ）の閾値である約280µsに近づいた
場合だけで、2000nsはそれよりはるかに短いため実害はありません。Lowが「長すぎる」側は
単なるアイドルに見えるだけですが、Lowが「短すぎる」側はビット境界を失ってデータそのものが
壊れる、という非対称性がこの選択の根拠です。

副次的な効果として、4MHzは8MHzに比べてEasyDMAがバイトを供給できる時間的余裕を2倍にします。
`spi3`（WS2812）は`spi2`（ST7735×4のフル転送）とRAM/AHB帯域を共有しているため、この余裕は
バイト間ギャップによる追加の化けを起きにくくします（nRF52840 anomaly 198のワークアラウンドは
`CONFIG_NRF52_ANOMALY_198_WORKAROUND=y`で別途有効になっており、これとは無関係です）。

この4MHz / `0x70` / `0x40`という値は、同じworkspaceのhitsuki46ボード
（`zmk-config-hitsuki46/boards/shields/hitsuki46/hitsuki46.dtsi`の`&spi1` / `ws2812@0`）
で既に実績のある構成と同じです。

なお、この基板の電源は3.3Vですが、WS2812B-V6のデータシート（V1.4）ではVDD 3.3〜5.3Vが
規格内、VIHは0.55×VDDです。3.3V駆動時のVIHは1.82Vとなり、3.3VのDINでも十分マージンが
あるため、レベルシフタは不要です（V6特性表はVDD=5V条件のため、3.3V駆動では光量が下がります）。

### 静的な表示の消灯・確認書き込み

WS2812はソフトリセットでは消えず、最後にラッチしたフレームを電源断まで保持し続けます。
そのため`SCREENKEY_LED_OFF`・`SCREENKEY_LED_COMPLETED`のような静的な色は、`src/status_led.c`
の`led_tick_cb()`が一度書いたきりで再送しないと、その1回のSPI転送が化けたり失敗したりした
場合にLEDが点いたまま（あるいは化けたまま）戻らなくなります。これを防ぐため、`status_led.c`は
次の2点を実装しています。

- `led_strip_update_rgb()`が失敗を返した場合、`last_written`（チェーンが表示していると
  信じている色）を更新しません。次のtickで「今出したい色 != last_written」が再び真になり、
  自動的に書き直しが起こります。
- 静的な色へ新しく落ち着いたときは、同じ色を合計3回、200ms間隔で強制的に書き直します
  （「色が変わっていなければ送らない」という通常の最適化は、この確認書き込みの間だけ
  バイパスされます）。書き込みが失敗した場合は、表示中の状態に関係なく200ms後に無条件で
  再試行します。

### 画面の色をそのままLEDへ渡さない

ScreenKeyのパネルはsRGBのトランスファーカーブがかかりますが、**WS2812の各チャンネルは線形PWM**です。
そのため同じ16進数でも見え方が一致しません。

実例として、完了表示は当初、画面の緑枠と同じ`#22C55E`をそのままLEDへ渡していました。この値は
青が`0x5E`（緑`0xC5`の48%）あり、パネル上では緑に見えますが、線形のLEDでは青が相対的にずっと
明るく出るため、**はっきりシアン寄りの緑**になります。実機で「完了の緑が青っぽい」と分かったため、
LED側だけ純緑`(0x00, 0xC5, 0x00)`に変更しました。緑チャンネルは`0xC5`のまま据え置いているので、
明るさは変わらず色味だけが直ります。画面側の枠は`#22C55E`のままです。

**画面とLEDを「同じ表示」に見せたいときに揃えるべきなのは、16進数ではなく見た目です。**
新しい色を足すときは、青と赤の成分が緑や主色に対して何%あるかを見てください。
目安として、副成分が主成分の20%を超えるとLEDでは色が濁って見えます。

同じ理屈で、エラーの赤`#EF4444`は緑と青がそれぞれ`0x44`（主成分の28%）あるため、LEDでは
やや淡いピンク寄りに見えます。気になる場合は同様に副成分を落としてください。

### hogノードは削除しない

`&gpio0`の`ws2812_din_idle` GPIO hogは、`&spi3` / `aipad_leds`が入った後も**残しています**。

```dts
&gpio0 {
    ws2812_din_idle {
        gpio-hog;
        gpios = <5 GPIO_ACTIVE_HIGH>;
        output-low;
    };
};
```

理由は次のとおりです。

- `gpio_hogs_init()`（`zephyr/drivers/gpio/gpio_hogs.c`）は`gpio_pin_configure()`を呼ぶだけで、
  ピンの予約機構を持ちません。そのため後から来る`pinctrl`がP0.05のPIN_CNFを書き換えても
  衝突エラーにはなりません。
- 初期化の順序はhog（`POST_KERNEL 41`、GPIOドライバ40の直後）→
  `AIPAD_ENCODER_PROBE`のピンウォーク（`POST_KERNEL 45`、約8秒間FPCの他の信号ピンを順に
  駆動する）→ `spi3`のpinctrl（`POST_KERNEL 50`）です。hogが無いと、P0.05はhogとspi3の間の
  区間、ピンウォークが0.5mmピッチの隣接トレースを叩いている間ずっと「浮いた入力」のままになり、
  何を拾うか分かりません。hogはこの間ずっとP0.05をLowへ駆動し続けることで、その窓を塞いでいます。
- `aipad_spi3_sleep`の`low-power-enable`は、spi3がidleになるとP0.05を完全にハイインピーダンスの
  入力へ戻します。今はCONFIG_PMもCONFIG_ZMK_SLEEPも無効なので実際には使われませんが、
  将来どちらかを有効にしたときに、その時点で（もう動いていない）SPIのpinctrlではなく
  このhogがP0.05をLow固定に保つ役目を引き継ぎます。

`CONFIG_AIPAD_STATUS_LED=n`にしてもこのdevicetree自体（`&spi3` / `aipad_leds` / hog）は
そのまま残ります。Kconfigが切るのはRenderer側のロジック（`status_led.c`のコンパイル）だけで、
SPIMのpinctrlはP0.05を引き続きLowで保持するため、チェーンは浮くことなく消灯を保ちます。

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
