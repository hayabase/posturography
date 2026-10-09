# 4ch CSV受信レート・連続動作の確認

## 実行

Arduino IDEのシリアルモニタ/プロッタを閉じてください。同じポートを同時に開くと `Resource busy` になります。

```sh
conda activate wm
cd /Users/gyobu/Documents/weight_measurement/posturography/26
python check_serial_rate.py --list-ports
python check_serial_rate.py --port /dev/cu.usbserial-D30AKO0H --duration 300
```

30分の場合は `--duration 1800`。既定は115200 baud、期待100 Hz、許容偏差±1%、途絶閾値100 ms、ウォームアップ3秒です。
`--expected-hz`、`--rate-tolerance-percent`、`--stall-ms`、`--warmup`で変更できます。
現在の **4列CSV** 専用です。バイナリ出力は対象外です。macOS/Linuxで動作します。

`serial_rate_results/日時/` に生データ、ns単位のホスト時刻を付けたJSONL、集計JSONを保存します。
測定中は30秒ごとに状況を表示します。Ctrl+Cでもそれまでの集計を保存し、未完了と記録します。
デバイスへデータ送信やファームウェア書き込みは行いません。ただしシリアルポート開放・接続時に、基板の自動リセット回路によって再起動する場合があります。

正常終了コード0は「規定時間の通信継続条件」と「平均受信レートの許容範囲」の両方を満たした場合です。
コード1はレート逸脱、受信エラー、長い途絶、途中停止などです。詳しい理由は `summary.json` を参照してください。
**通信が止まらないことと、100 Hzの新しいADC変換を受け取れていることは別です。**

## 保存データの再解析・図とレポート

```sh
python analyze_serial_capture.py serial_rate_results/対象の日時フォルダ
```

`report.md`、`analysis.json`、`rate_diagnostics.png` を追加します。依存は測定がpyserial、図の生成がmatplotlibです。
再解析には2行以上の正常データが必要です。測定中ではなく測定終了後に実行してください。

## 測定方法と限界

- 単調時計 `time.perf_counter_ns()` を読み出し直後に記録します。PCの時計は外部校正していません。
- `(受信行数−1)/(末尾時刻−先頭時刻)` と、全行の行番号対時刻の回帰から平均Hzを計算します。
- 10秒ごとの行数、周期分布、長い無受信時間、同一値の連続、各chの値変化も確認します。
- USB/OSのまとめ読みでは複数行に同じ時刻が付きます。この受信間隔はADCの周期ジッタではありません。
- CSVにはデバイス時刻、連番、CRCがないため、欠落・新規変換・値の破損は厳密には識別できません。同値だけで重複測定とは断定できません。
- 最初の行は行境界を合わせるため破棄します。ウォームアップデータは別保存します。

## 今回の実行環境

2026-09-17の確認時点では、`wm`環境内にPython実行ファイルがなく、`conda run -n wm python` は
`/Users/gyobu/.pyenv/versions/3.10.9/bin/python` を使用しました。pyserial 3.5、matplotlib 3.8.0を利用できました。
環境のインストールや変更は行っていません。

## 検証

パイプで模擬した100 Hz入力、200 Hzへのレート逸脱、不正CSV、約250 msの停止を使い、それぞれ正しく判定することを確認しました。
実機の結果は `serial_rate_results` 内のレポートを参照してください。
