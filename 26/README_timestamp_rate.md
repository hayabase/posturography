# AD7193 x4 タイムスタンプ付きCSV / 115200 baud

2026-09-17実機結果: 通常CSVではFS19（公称252.63 Hz、実測253.83 Hz）が現在の値の桁数で5分間合格。
FS18（266.67 Hz）は送信行の破棄が発生。FS24（200 Hz、実測200.95 Hz）も通常CSVで5分間合格しました。
FS19は数字の桁が増えると帯域を超えるため、再起動時の設定はFS24です。
全試験と判定の詳細は `sampling_rate_limit_report.md` を参照してください。

## 出力

```
timestamp_us,raw1,raw2,raw3,raw4
```

実際のデータ行はヘッダを含まない5個の10進整数です（LF終端）。
`timestamp_us` はESP32の起動後経過時間（uint64、µs）で、4ch分の新しい値が揃って行を組み立てる時刻です。
4台のADCは内部クロックが独立しているので、4ch同時変換の時刻ではありません。
raw値は24bit offset-binaryで、従来のSPI bit0補正（最下位bitをクリア）を維持しています。
`#` で始まる行は起動情報や約5秒おきの統計です。データとして扱わないでください。

## 設定と書き込み

スケッチ先頭の `DEFAULT_FS=24`（公称200 Hz）が電源投入後のFS設定です。`SERIAL_BAUD = 115200` は固定です。
サンプリングレートは公称 `4800 / FS` Hz。内部発振器の個体差があり、公称値と実測は一致しません。

シリアルモニタから改行付きで次を送信すると、再書き込みせずに全ADCのFSを変更できます。

```
FS=48
FS=24
FS=20
STATS
```

順に公称100、200、240 Hz、統計取得です。FSは1〜1023で、変更時に統計カウンタをリセットします。
FS変更はRAM上のみで、再起動すると `DEFAULT_FS` に戻ります。

帯域負荷試験用の `WIDE=1` は同じ実測値を先頭ゼロ付きの固定幅で送ります（timestamp20桁・各raw8桁、57 byte/行）。
値や時刻は変更しません。`WIDE=0` で通常の可変長CSVに戻り、再起動時も通常形式になります。
WIDE変更でも統計をリセットします。

## 安定性の測定

```sh
conda activate wm
cd /Users/gyobu/Documents/weight_measurement/posturography/26
python sweep_timestamp_rate.py --fs 48 40 32 24 20 16 --seconds 30
python sweep_timestamp_rate.py --fs 19 --seconds 300
python sweep_timestamp_rate.py --fs 24 --wide --seconds 120
python sweep_timestamp_rate.py --fs 24 --seconds 30
```

Arduino IDEのシリアルモニタを閉じて実行します。測定後は最後に指定したFSとCSV形式で動作します。`--wide` なしの実行は通常形式を指定します。
`--port`、`--seconds`、`--warmup`、`--output`を指定できます。baud変更オプションはありません。
測定にはpyserialを使用。ログと判定は `timestamp_rate_results/日時/` に保存されます。

以前の `check_serial_rate.py` / `analyze_serial_capture.py` は旧4列CSV用です。新5列CSVはこの測定コードを使用してください。

## 判定内容

- PC受信時刻とESP32タイムスタンプをそれぞれ回帰し、フレームレートを推定。
- 全ADCの読み出し回数から各chの読み出しHzを確認。
- 公称値から±2%以内（厳密な校正基準ではなく、今回の通信試験の判定閾値）。
- UART送信フレーム破棄ゼロ、統計出力スキップゼロ、ADCエラーゼロ、不正CSVゼロ。
- 送信カウンタ増分と受信した行数が一致すること。
- デバイス側のフレーム間隔が公称周期の2.5倍以下、PC受信間隔が100 ms以下。

送信はアプリ側4096 byteキューから `uart_tx_chars()` でUART FIFOへ非ブロッキング転送します。
Arduino-ESP32 3.0.7の `availableForWrite()` と `Serial.write()` の組み合わせは負荷時に待ちが発生したため、取得ループ中には使っていません。
`tx_drop` は送信バッファ不足で捨てた行数です。UARTが詰まってもADC読み出しを止めず、行単位で破棄して明示的にカウントします。
`superseded` は4chが揃うまでに速いADCの値がもう一度更新された回数です。独立したADCクロックによる集約時の間引きで、UARTの取りこぼしとは別に記録します。
4ch CSVは速いchの全変換を保存する形式ではありません。すべてのchのすべての変換が必要ならADC別イベント形式または共通ADCクロックが必要です。
AD7193には未読のまま上書きされた変換の通番がないため、`reads` だけではADC内部の全欠落は数えられません。

## 通信帯域

8N1の公称容量は `115200 / 10 = 11520 byte/s` です。
数字の桁数で1行の長さが変わるため、荷重値や起動後時間によって上限も変わります。
4chがすべて8桁の場合、1行は「タイムスタンプの桁数 + 37」byteです。
例: タイムスタンプ9桁なら46 byte、10桁なら47 byte、11桁なら48 byte。
統計行にも帯域を使用するため、データ行だけで容量を使い切る設定は継続運用に適しません。
短時間の最大合格レートと、桁数増加に余裕を持たせる常用設定を分けてレポートします。

## 新規変換判定の修正

旧版は共有MISOを `digitalRead()` で読み、過剰な同値送信が観測されました。
新版はSTATUSレジスタのRDY（bit7=0）を確認してからDATAを読みます。STATUSのERR/NOREFも記録します。
実機100 Hz試験で過剰出力の解消を確認しました。旧GPIO判定が誤った電気的原因までは確定していません。
メーカー資料: https://www.analog.com/media/en/technical-documentation/data-sheets/AD7193.pdf

判定で使うPC側の無受信閾値100 msは実時間用途向けの厳しい基準です。
57 byte/行・200 Hzでは全行を受信しても、PC側だけで100 msを超す一時的なまとめ受信が観測されました。
`tx_drop=0`、送受信カウンタの一致、機器側タイムスタンプの周期、PC側の最大遅延を分けて読んでください。
また、試験中にmacOSがFT231XをUSB上で再列挙したことがあり、5分試験が中断されました。
この中断をファームウェアのサンプルレート失敗として数えません。長時間評価にはUSB接続の安定化が必要です。

元のスケッチは `backups/before_timestamp_limit.ino` に保存しています。
