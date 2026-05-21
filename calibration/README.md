# Calibration

重心動揺計の 4 センサーについて、AD7193 の生コードと既知重量の対応を取るための校正フォルダです。  
`calibration.py` で各重量の静止データを取得し、出力された `Baseline median` を `calibration.xlsx` に入力して校正式を作成します。

## 目次

- [構成](#構成)
- [前提](#前提)
- [校正イメージ](#校正イメージ)
- [対応する Arduino ファイルと通信方式](#対応する-arduino-ファイルと通信方式)
- [シリアルデータの受信確認](#シリアルデータの受信確認)
- [Python 環境](#python-環境)
- [校正の流れ](#校正の流れ)
- [calibration.py の実行方法](#calibrationpy-の実行方法)
- [出力ファイル](#出力ファイル)
- [Excel への入力方法](#excel-への入力方法)
- [校正後の確認](#校正後の確認)
- [取得条件](#取得条件)
- [注意点](#注意点)
- [トラブルシュート](#トラブルシュート)

## 構成

| パス | 内容 |
| --- | --- |
| `calibration.py` | シリアルポートから 4 チャンネル生データを取得し、CSV、統計、ヒストグラムを保存する校正用スクリプト |
| `calibration.xlsx` | 各重量の `Baseline median` を入力し、各センサーの校正式を作るための Excel ファイル |
| `calibration.png` | 校正に関する参考画像 |
| `README.md` | このファイル |

## 前提

- ESP32 と AD7193 x 4 の装置が接続済みであること。
- PC から ESP32 の USB シリアルポートを開けること。
- 校正用の重りを、重心動揺計の中心に静置できること。
- 0 kg, 5 kg, 10 kg ... 50 kg 程度まで、同じ手順で測定すること。
- サンプリングの目安は約 `100 Hz` です。

## 校正イメージ

校正では、既知重量の重りを重心動揺計の中心に置き、各重量で 4 センサーの生データを取得します。

![校正イメージ](calibration.png)

## 対応する Arduino ファイルと通信方式

校正時は、上位フォルダの以下を ESP32 に書き込んで使用します。

```text
../ad7193_wired/ad7193_wired.ino
```

現在の校正フォルダで想定している Arduino ファイルと通信方式は以下です。

| Arduino ファイル | 通信方式 | `calibration.py` での扱い | 備考 |
| --- | --- | --- | --- |
| `../ad7193_wired/ad7193_wired.ino` | USB シリアル | 推奨、対応済み | 校正ではこのファイルを基本にする |
| `../ad7193_wireless/ad7193_wireless.ino` | USB シリアル + Bluetooth Classic SPP | USB 接続なら対応。Bluetooth は OS が SPP をシリアルポートとして認識すれば使用可能 | Bluetooth は簡易確認向け。安定した校正は USB 推奨 |
| `../BLE_ad7193/BLE_ad7193.ino` | USB シリアル + BLE Notify | BLE 入力は非対応。USB シリアル出力なら既存方式で確認可能 | BLE は `../BLE_ad7193/ble_receive.py` で受信する。校正本番は USB 推奨 |

USB シリアルと Bluetooth Classic SPP のデータ行の形式は同じです。

```text
ch1,ch2,ch3,ch4
```

例:

```text
8389210,8388750,8389021,8388601
```

起動直後は `AD7193 ID`、`AD7193 MODE`、`AD7193 CONFIG` などのデバッグ行も出ます。  
`calibration.py` は `4 つの整数がカンマ区切りになっている行` だけをデータとして採用し、それ以外の行は無視します。

今後 Arduino ファイルを追加した場合は、この表に通信方式、対応する Python スクリプト、出力形式を追記してください。出力形式が変わる場合は `calibration.py` 側の読み取り処理も確認が必要です。

BLE 版は Notify payload を 20 byte に収めるため、BLE 通知だけは `sample_index,DAT1,DAT2,DAT3,DAT4` 相当の little-endian `uint32` x 5 のバイナリフレームで送信します。`calibration.py` は BLE を直接受信しないため、BLE で確認する場合は `../BLE_ad7193/ble_receive.py` を使ってください。

## シリアルデータの受信確認

校正を始める前に、シリアルモニターでデータが見えるか確認してください。  
速度設定は `115200 bps` です。

### VS Code で確認する

1. VS Code に `Serial Monitor` 拡張機能を入れる。
2. ESP32 を USB ケーブルで PC に接続する。
3. VS Code の Serial Monitor 画面を開く。
4. `Port` で ESP32 のポートを選ぶ。
   - Windows 例: `COM3`, `COM4`
   - macOS 例: `/dev/cu.usbserial-*`, `/dev/cu.SLAB_USBtoUART`, `/dev/cu.wchusbserial*`
5. `Baud Rate` を `115200` にする。
6. `Start Monitoring` を押す。
7. 以下のような 4 つの数値が流れれば、装置からデータが出ています。

```text
8389210,8388750,8389021,8388601
8389208,8388749,8389020,8388602
```

### Arduino IDE で確認する

1. Arduino IDE を開く。
2. ESP32 を USB ケーブルで PC に接続する。
3. `ツール` から ESP32 のポートを選ぶ。
4. シリアルモニタを開く。
5. 速度を `115200 baud` にする。
6. `ch1,ch2,ch3,ch4` 形式のデータが流れることを確認する。

### Python で確認する

Python から受信できるか確認する場合は、上位フォルダの `sampling_test.py` を使います。

```bash
cd /Users/gyobu/Documents/weight_measurement/posturography
python3 sampling_test.py
```

表示されたポート一覧から ESP32 の番号を選択します。  
1 秒ごとのサンプル数が表示されれば、Python から受信できています。

### Bluetooth で確認する

Bluetooth で確認する場合は、上位フォルダの以下を書き込みます。

```text
../ad7193_wireless/ad7193_wireless.ino
```

1. ESP32 を起動する。
2. PC またはスマートフォンで `ESP32-AD7193` にペアリングする。
3. PC 側に Bluetooth SPP のシリアルポートが作られていることを確認する。
4. VS Code、Arduino IDE、または Python スクリプトでそのポートを選ぶ。
5. 速度設定が必要な場合は `115200` にする。

Bluetooth は OS によってポート名や接続状態の扱いが変わります。校正の本番データは、できるだけ USB シリアルで取得してください。

シリアルポートは基本的に同時に 1 つのアプリからしか開けません。VS Code や Arduino IDE のシリアルモニターで確認したあとに `calibration.py` を使う場合は、先にシリアルモニターを閉じてください。

## Python 環境

必要な主なライブラリは以下です。

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

既に使用している Python 環境がある場合は、その環境で上記ライブラリが使えれば問題ありません。

## 校正の流れ

1. ESP32 に `../ad7193_wired/ad7193_wired.ino` を書き込む。
2. ESP32 と PC を USB ケーブルで接続する。
3. 重心動揺計の中心に重りを置ける状態にする。
4. 0 kg の状態で `calibration.py` を実行する。
5. 出力された `*_summary.txt` の `Baseline median: [a, b, c, d]` を確認する。
6. `calibration.xlsx` の対応する重量行に `DAT1` から `DAT4` として入力する。
7. 5 kg, 10 kg, 15 kg ... 50 kg 程度まで同じ操作を繰り返す。
8. Excel 上で各センサーの直線、または校正式の係数を確認する。

## calibration.py の実行方法

```bash
cd /Users/gyobu/Documents/weight_measurement/posturography/calibration
python3 calibration.py
```

実行すると、以下を対話的に入力します。

| 入力 | 内容 |
| --- | --- |
| COM ポート番号 | 表示されたポート一覧から ESP32 の番号を選択 |
| CSV ファイル名 | 保存するデータ名。拡張子を省略すると `.csv` が付きます |
| 開始確認 | `y` + Enter で取得開始 |

ファイル名が既に存在する場合は、`sample_1.csv`, `sample_2.csv` のように自動で別名保存されます。

重量ごとのファイル名は、後から見返せるように以下のような名前にすると扱いやすいです。

```text
0kg.csv
5kg.csv
10kg.csv
```

## 出力ファイル

たとえば `5kg.csv` という名前で保存した場合、以下が生成されます。

| 出力 | 内容 |
| --- | --- |
| `5kg.csv` | `DAT1,DAT2,DAT3,DAT4` の 4 列データ |
| `5kg_summary.txt` | 使用サンプル数、外れ値数、`Baseline median` などの要約 |
| `5kg_stats.json` | 要約情報を JSON 形式で保存したもの |
| `5kg_hist.png` | 各チャンネルの測定値分布ヒストグラム |

Excel に入力する値は、基本的に `*_summary.txt` の以下の行です。

```text
Baseline median: [a, b, c, d]
```

## Excel への入力方法

`Baseline median: [a, b, c, d]` が出力された場合、`calibration.xlsx` へ以下のように入力します。

| Excel 列 | 入力する値 |
| --- | --- |
| `DAT1` | `a` |
| `DAT2` | `b` |
| `DAT3` | `c` |
| `DAT4` | `d` |

各重量で同じ入力を行います。

例:

| 重量 | DAT1 | DAT2 | DAT3 | DAT4 |
| --- | --- | --- | --- | --- |
| 0 kg | 0 kg 測定時の 1 つ目 | 0 kg 測定時の 2 つ目 | 0 kg 測定時の 3 つ目 | 0 kg 測定時の 4 つ目 |
| 5 kg | 5 kg 測定時の 1 つ目 | 5 kg 測定時の 2 つ目 | 5 kg 測定時の 3 つ目 | 5 kg 測定時の 4 つ目 |
| 10 kg | 10 kg 測定時の 1 つ目 | 10 kg 測定時の 2 つ目 | 10 kg 測定時の 3 つ目 | 10 kg 測定時の 4 つ目 |

50 kg 程度まで入力すると、各センサーの生コードと重量の対応関係が作れます。そこから、各センサー値から重量を計算するための係数を求めます。

## 校正後の確認

校正式を作成したら、既知重量の重りを重心動揺計の中心に置いて確認します。  
4 つのセンサーから計算した重量の合計が、実際に置いた重りの重量に近ければ、校正はおおむね正しくできています。

ずれが大きい場合は、以下を確認してください。

- `Baseline median` を `DAT1` から `DAT4` の順番で正しく入力しているか。
- 重りが中心に置かれているか。
- 測定中に重り、装置、USB ケーブルが動いていないか。
- `*_hist.png` の分布が極端に広がっていないか。
- `*_summary.txt` の外れ値数が多すぎないか。

## 取得条件

現在の `calibration.py` の主な設定です。

| 項目 | 値 |
| --- | --- |
| シリアルボーレート | `115200` |
| 最終的に残すサンプル数 | `1600` |
| 先頭ドロップ | `40` |
| 末尾ドロップ | `40` |
| 基準統計を作る最小サンプル数 | `120` |
| 外れ値判定 | median ± MAD ベース |
| タイムアウト計算用の目安サンプリング周波数 | `80 Hz` |

`ad7193_wired.ino` 側では AD7193 の目標データレートを約 `100.1 Hz` に設定しています。実際の PC 受信レートは、上位フォルダの `sampling_test.py` で確認してください。

## 注意点

- 測定開始後は、取得が終わるまで装置に触れないでください。
- 各重量で、重りを置く位置と置き方をできるだけ揃えてください。
- 0 kg の測定も必ず行ってください。
- 校正式を作るときは、同じ装置、同じ配線、同じセンサー対応で取得したデータを使ってください。
- センサーや AD7193 の接続順を変えた場合は、再校正してください。

## トラブルシュート

### COM ポートが表示されない

- ESP32 が USB 接続されているか確認してください。
- Arduino IDE のシリアルモニタなど、同じポートを開いているアプリを閉じてください。
- USB ケーブルが通信用か確認してください。

### `Baseline median` が出ない

- シリアルから `ch1,ch2,ch3,ch4` 形式の 4 整数が出ているか確認してください。
- 先に上位フォルダの `sampling_test.py` で受信できているか確認してください。
- ESP32 に `ad7193_wired.ino` が書き込まれているか確認してください。

### 外れ値が多い

- 測定中に重りや装置が動いていないか確認してください。
- ケーブルに触れていないか確認してください。
- 電源や配線が不安定でないか確認してください。

### Excel の結果が重量と合わない

- `DAT1` から `DAT4` の入力順が入れ替わっていないか確認してください。
- 重量行を間違えていないか確認してください。
- AD7193 の CS とセンサー位置の対応が想定通りか確認してください。
