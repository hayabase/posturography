# BLE AD7193

`BLEDevice.h` を使って、ESP32 から AD7193 x 4 のデータを BLE Notify で送信する版です。  
Bluetooth Classic SPP ではなく BLE なので、PC 側はシリアルポートではなく Python の `bleak` で受信します。

## 構成

| パス | 内容 |
| --- | --- |
| `BLE_ad7193.ino` | ESP32 用 BLE Peripheral スケッチ |
| `ble_receive.py` | BLE Notify を受信して CSV 保存する Python スクリプト |

## 通信仕様

| 項目 | 値 |
| --- | --- |
| BLE デバイス名 | `ESP32-AD7193-BLE` |
| Service UUID | `7b7f0001-8f4c-4d52-a9f8-9c7d2b1f0001` |
| Data Characteristic UUID | `7b7f0002-8f4c-4d52-a9f8-9c7d2b1f0001` |
| BLE データ形式 | Notify, 20 byte binary frame |
| USB Serial 表示 | `ch1,ch2,ch3,ch4` |

BLE Notify の 1 フレームは little-endian の `uint32` x 5 です。

```text
sample_index, DAT1, DAT2, DAT3, DAT4
```

20 byte に収めているため、BLE の標準的な notification payload にそのまま入ります。Python 側の `ble_receive.py` がこのバイナリを CSV に変換します。

## Arduino 側

Arduino IDE で `BLE_ad7193/BLE_ad7193.ino` を ESP32 に書き込んでください。  
書き込み後、USB シリアルモニターを `115200 baud` で開くと、初期化ログと以下の形式のデータが見えます。

```text
ch1,ch2,ch3,ch4
```

BLE と同時に USB Serial にも出力しているので、BLE 接続前の動作確認は USB Serial でできます。USB Serial は既存スクリプトと同じ 4 チャンネル CSV 形式です。

## Python 側

`bleak` をインストールします。

```bash
cd /Users/gyobu/Documents/weight_measurement/posturography/BLE_ad7193
pip install bleak
```

受信を開始します。

```bash
python3 ble_receive.py
```

デフォルトでは `ESP32-AD7193-BLE` をスキャンして接続し、`ble_ad7193_YYYYmmdd_HHMMSS.csv` に保存します。最初の数サンプルと、1 秒ごとの受信レートも表示します。

主なオプション:

```bash
python3 ble_receive.py --duration 30
python3 ble_receive.py --csv test_ble.csv
python3 ble_receive.py --print-samples
python3 ble_receive.py --no-save
python3 ble_receive.py --address <BLE address or UUID>
```

CSV の列は以下です。

```text
timestamp,perf_time_sec,sample_index,DAT1,DAT2,DAT3,DAT4
```

`sample_index` が飛んでいる場合は BLE 通知の取りこぼしが起きている可能性があります。`ble_receive.py` は `index_drops` として簡易カウントします。

## 注意

- BLE は Bluetooth Classic SPP と違い、OS 上の COM ポートとして扱いません。
- VS Code や Arduino IDE のシリアルモニターでは BLE データ自体は読めません。USB Serial の確認だけに使います。
- 校正など取りこぼしを避けたい本番測定では、まず USB シリアル版の `ad7193_wired/ad7193_wired.ino` で安定性を確認してください。
- BLE 接続や通知周期は PC、OS、Bluetooth アダプタの状態に影響されます。
