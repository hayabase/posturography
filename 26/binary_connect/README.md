# AD7193 x4 binary serial, 115200 baud

CSV版は隣の `posturography_ad7193x4_wired` に残しています。このディレクトリは独立したArduinoスケッチです。
接続するESP32にこの版を書き込むと、シリアル出力は**バイナリ専用**になります。Arduino IDEのシリアルモニタでは文字として読めません。

## データフレーム（26 byte）

|offset|byte数|意味|
|---:|---:|---|
|0|2|同期語 `A5 5A`|
|2|2|フレーム連番 uint16、little-endian。65,536で周回|
|4|8|ESP32起動後のµs時刻 uint64、little-endian|
|12|3|CH1 raw uint24、little-endian|
|15|3|CH2 raw uint24、little-endian|
|18|3|CH3 raw uint24、little-endian|
|21|3|CH4 raw uint24、little-endian|
|24|2|CRC-16/CCITT-FALSE、little-endian。対象はoffset 0〜23|

CRCは多項式0x1021、初期値0xFFFF、最終XORなしです。rawはAD7193の24 bit offset-binary値で、既存のbit0クリア補正を維持します。
µs時刻は4chの新しい値が揃った時刻であり、4台のADCが同時に変換した時刻ではありません。

## 統計フレーム（84 byte）

同期語 `A5 5B`、バージョン1、ADC初期化mask、FS uint16、機器時刻 uint64、
`frames`・`sent`・`tx_drop`・`stat_skip` uint32、
`reads[4]`・`superseded[4]`・`adc_errors[4]` uint32、`max_loop_gap_us` uint32、
CRC uint16です。すべてlittle-endianで、CRCは先頭82 byteが対象です。
約5秒ごと、起動時、FS変更時、`STATS` 指示時に送ります。
`tx_drop` はUARTキュー満杯で捨てたデータフレーム数、`superseded` は4chの値を揃える間に速いADCの値が置き換わった回数です。

## 設定・再測定

スケッチの `DEFAULT_FS=12`（公称400 Hz）が再起動時の設定です。115200 baudは固定です。実機評価は [RESULTS.md](RESULTS.md) に記録しています。
次のテキストコマンドをシリアルポートへ改行付きで送ると、書き込み直さずにFSを変えられます。出力はすべてバイナリのままです。

```
FS=12
STATS
```

`wm`環境のpyserialで測定します。Arduino IDEのシリアルモニタを閉じて実行してください。

```sh
conda activate wm
cd /Users/gyobu/Documents/weight_measurement/posturography/26/binary_connect
python sweep_binary_rate.py --fs 24 20 16 12 11 10 --seconds 30
python sweep_binary_rate.py --fs 11 --seconds 300
```

`--port`、`--warmup`、`--output`も指定できます。ログは `results/日時/` に保存されます。
`received.bin` はバイナリ生データ、`events.jsonl` はPC受信時刻と復号データ、`summary.json` は判定結果です。
連番周回は受信側で扱います。FS変更時は連番と統計をリセットし、統計フレームで区切ります。

合格条件: PC/ESP32のフレームHzとADC別読み出しHzが公称±2%、
送信破棄・連番欠落・CRC/フレーム異常・ADCエラー0、
送信カウンタ増分と受信フレーム数が一致、
機器側の最大フレーム間隔が公称周期の2.5倍以下、PC側の最大受信間隔が100 ms以下。
PC/USBはデータをまとめて渡す場合があるため、ホスト間隔はADC変換周期ではありません。

8N1での公称上限は11520 byte/s。26 byte/フレームならデータだけで理論上約443 Hzです。
実際には約5秒ごとの84 byte統計フレーム、内部発振器の偏差、OS/USBの挙動が加わるので実機測定で判定してください。
データ行の値や時刻の桁数はバイナリのフレーム長に影響しません。

## 注意

- `tx_drop=0`でも、独立したADCクロックの差で速いchの値が4chフレームへまとめる前に置き換わることがあります。`superseded`で別計上します。
- ADC内部の未読上書きには通番がないため、全ADC変換の完全性までは証明できません。
- 実測は有限の期間についての結果です。PC時計は外部基準で校正していません。
- FS変更はRAM上のみです。再起動すると`DEFAULT_FS`に戻ります。
