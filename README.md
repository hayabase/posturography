# Posturography

ESP32 と 4 個の AD7193 から、重心動揺計の 4 チャンネル生データを取得するためのコード一式です。  
装置側では `ch1,ch2,ch3,ch4` 形式の CSV 行を出力し、PC 側ではサンプリング確認と校正用データ取得を行います。

## 目次

- [構成](#構成)
- [装置側コード](#装置側コード)
- [Python 環境](#python-環境)
- [サンプリング確認](#サンプリング確認)
- [校正手順](#校正手順)
- [校正スクリプトの出力](#校正スクリプトの出力)
- [校正後の確認](#校正後の確認)
- [重心位置計算について](#重心位置計算について)
- [トラブルシュート](#トラブルシュート)

## 構成

| パス | 内容 |
| --- | --- |
| `ad7193_wired/ad7193_wired.ino` | USB シリアルで 4 チャンネルの AD7193 生コードを送信する ESP32 用スケッチ |
| `ad7193_wireless/ad7193_wireless.ino` | USB シリアルに加えて Bluetooth Classic SPP でも同じ CSV 行を送信する ESP32 用スケッチ |
| `sampling_test.py` | シリアル受信のサンプル数を 1 秒ごと、10 秒ごとに表示する確認用スクリプト |
| `circuit.png` | 配線・回路の参考画像 |
| `calibration/calibration.py` | 校正用に一定数のサンプルを取得し、CSV、統計 JSON、サマリ TXT、ヒストグラム PNG を保存するスクリプト |
| `calibration/calibration.xlsx` | 各重量の `Baseline median` を入力して、各センサーの校正式を作るための Excel ファイル |
| `calibration/README.md` | 校正フォルダ単体で参照するための詳細 README |
| `calibration/calibration.png` | 校正に関する参考画像 |

## 装置側コード

基本の校正では `ad7193_wired/ad7193_wired.ino` を使用します。  
Bluetooth で受信したい場合は `ad7193_wireless/ad7193_wireless.ino` を ESP32 に書き込みます。

共通の主な設定は以下です。

- ボーレート: `115200`
- 出力形式: `ch1,ch2,ch3,ch4`
- AD7193: 4 チップ構成
- AD7193 の目標データレート: 約 `100.1 Hz`
- SPI: `1 MHz`, `MODE3`
- ESP32 側ピン:
  - `MISO`: 19
  - `MOSI`: 23
  - `SCLK`: 18
  - `SYNC`: 21
  - `CS1`: 5
  - `CS2`: 17
  - `CS3`: 27
  - `CS4`: 25

`ad7193_wireless.ino` は Bluetooth デバイス名 `ESP32-AD7193` で起動します。PC やスマートフォンから Bluetooth SPP として接続すると、USB シリアルと同じ CSV 行を受信できます。

## Python 環境

PC 側スクリプトは Python で実行します。必要な主なライブラリは以下です。

- `pyserial`
- `numpy`
- `matplotlib`

例:

```bash
cd /Users/gyobu/Documents/weight_measurement/posturography
python3 -m venv .venv
source .venv/bin/activate
pip install pyserial numpy matplotlib
```

既に使用している Python 環境がある場合は、その環境で `pyserial`, `numpy`, `matplotlib` が使えれば問題ありません。

## サンプリング確認

ESP32 に装置側コードを書き込み、PC と接続してから実行します。

```bash
cd /Users/gyobu/Documents/weight_measurement/posturography
python3 sampling_test.py
```

実行すると利用可能なシリアルポート一覧が表示されるので、対応する番号を入力します。  
受信できている場合、1 秒間のサンプル数と 10 秒平均が表示されます。終了は `Ctrl + C` です。

装置側コードでは AD7193 の目標データレートを約 `100.1 Hz` に設定していますが、PC 側で実際に受信できている周期はこのスクリプトで確認してください。

## 校正手順

校正では、既存メモに合わせて `ad7193_wired/ad7193_wired.ino` を ESP32 に書き込み、USB ケーブルで PC と接続します。

1. 重心動揺計の中心に重りを置ける状態にする。
2. まず 0 kg の状態にする。
3. `calibration.py` を実行する。
4. 出力された `*_summary.txt` の `Baseline median: [a, b, c, d]` を確認する。
5. `calibration.xlsx` の対応する重量行へ、以下のように入力する。
   - `DAT1`: `Baseline median` の 1 つ目
   - `DAT2`: `Baseline median` の 2 つ目
   - `DAT3`: `Baseline median` の 3 つ目
   - `DAT4`: `Baseline median` の 4 つ目
6. 5 kg, 10 kg, 15 kg ... 50 kg 程度まで同じ処理を繰り返す。
7. Excel 上で各センサーの値と重量の関係を確認し、校正式の係数を求める。

実行例:

```bash
cd /Users/gyobu/Documents/weight_measurement/posturography/calibration
python3 calibration.py
```

`calibration.py` は実行時に以下を対話的に確認します。

- 使用する COM ポート
- 保存する CSV ファイル名
- 計測開始確認: `y` + Enter

取得中は装置を動かさず、USB ケーブルを抜かないでください。ファイル名が既に存在する場合は、末尾に `_1`, `_2` のような番号を付けて保存されます。

## 校正スクリプトの出力

`calibration.py` で `sample.csv` を指定した場合、同じフォルダに以下が保存されます。

| 出力 | 内容 |
| --- | --- |
| `sample.csv` | `DAT1,DAT2,DAT3,DAT4` の 4 列データ |
| `sample_summary.txt` | 使用サンプル数、外れ値数、`Baseline median` などの人間が読むためのサマリ |
| `sample_stats.json` | サマリと同等の情報を機械処理しやすい JSON 形式で保存したもの |
| `sample_hist.png` | 4 チャンネル分の測定値分布ヒストグラム |

現在の `calibration.py` の主な取得条件は以下です。

- 最終的に残すサンプル数: `1600`
- 先頭ドロップ: `40`
- 末尾ドロップ: `40`
- 基準統計を作る最小サンプル数: `120`
- 外れ値判定: median ± MAD ベース
- タイムアウト計算用の目安サンプリング周波数: `80 Hz`

## 校正後の確認

校正式を反映したあと、既知の重りを中心に置き、4 つのセンサーから計算した重量の合計が実際の重りの重量に近いか確認します。  
大きくずれる場合は、以下を確認してください。

- 重りが中心に置かれているか
- 各重量の `Baseline median` を Excel の `DAT1` から `DAT4` に正しい順番で入れているか
- 計測中に装置やケーブルが動いていないか
- サンプリング確認で極端な取りこぼしが出ていないか
- AD7193 の CS ピンとセンサー位置の対応が想定通りか

## 重心位置計算について

このフォルダには、校正後の係数を使って重心位置を連続計算する完成版スクリプトは含まれていません。  
4 センサーから重量を求めたあと、重心位置は装置寸法と各センサー位置に基づいて計算します。既存資料の計算式を使って、各センサー荷重の加重平均として求める想定です。

## トラブルシュート

### COM ポートが表示されない

- ESP32 が PC に接続されているか確認する。
- Arduino IDE のシリアルモニタなど、同じポートを使うアプリを閉じる。
- Bluetooth で使う場合も、OS 側で SPP のシリアルポートとして認識されているか確認する。

### データが取得できない

- ESP32 に正しい `.ino` が書き込まれているか確認する。
- ボーレートが `115200` になっているか確認する。
- `sampling_test.py` で生データ受信の有無を先に確認する。

### 校正結果が不安定

- 計測中は重りと装置を動かさない。
- 0 kg と各重量で同じ置き方をする。
- `*_hist.png` で値の分布が極端に広がっていないか確認する。
- `*_summary.txt` の外れ値数が多すぎないか確認する。
# posturography
