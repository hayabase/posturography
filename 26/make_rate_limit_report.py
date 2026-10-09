#!/usr/bin/env python3
"""Summarize completed hardware sweeps and keep interrupted trials visible."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,default=Path(__file__).resolve().parent)
    a=ap.parse_args();root=a.root
    entries=[]
    for p in sorted((root/'timestamp_rate_results').glob('*/results.json')):
        condition=json.loads((p.parent/'conditions.json').read_text())
        for s in json.loads(p.read_text()):
            s['wide']=condition.get('wide',False)
            s['firmware']='v4' if ' wide=' in s['config'] else ('v3' if p.parent.name>='20260917_152914' else 'v2')
            entries.append(s)
    final=[x for x in entries if x['firmware']=='v4']
    if not final:raise SystemExit('No completed v4 results')
    normal=[x for x in final if not x['wide']]
    wide=[x for x in final if x['wide']]
    passed=[x for x in normal if x['passed']]
    best=max(passed,key=lambda x:(x['nominal_hz'],x['requested_s'])) if passed else None
    long=max(passed,key=lambda x:x['requested_s']) if passed else None
    safe=[x for x in wide if x['passed']]
    safest=max(safe,key=lambda x:x['nominal_hz']) if safe else None
    def table(items):
        lines=['|版/形式|FS|公称Hz|受信Hz|秒|TX破棄（測定区間）|受信−送信行数|判定|','|---|---:|---:|---:|---:|---:|---:|---|']
        for s in items:
            diff=s.get('received_minus_sent',s['frames']-(s['final_stats']['sent']-s['initial_stats']['sent']))
            label=s['firmware']+('/57 byte' if s['wide'] else '/通常')
            lines.append(f"|{label}|{s['fs']}|{s['nominal_hz']:.3f}|{s['host_hz']:.4f}|{s['requested_s']:g}|{s['tx_drop_measurement']}|{diff}|{'PASS' if s['passed'] else 'FAIL'}|")
        return '\n'.join(lines)
    code=root/'posturography_ad7193x4_wired/posturography_ad7193x4_wired.ino'
    digest=hashlib.sha256(code.read_bytes()).hexdigest()
    report=['# AD7193 x4 / 115200 baud サンプリングレート上限調査','',
        '測定日: 2026-09-17。実機: ESP32-D0WDQ6 + AD7193 x4、FT231X USB UART。Arduino-ESP32 3.0.7。',
        '', '## 結論','']
    if best:
        report.append(f"通常CSVで実測した最高の合格設定は **FS={best['fs']}（公称{best['nominal_hz']:.6f} Hz）**。{best['requested_s']:g}秒間、受信は **{best['host_hz']:.6f} 行/秒**、TX破棄は{best['tx_drop_measurement']}行でした。この時間での通常CSVの最高合格レートです。")
    if best:
        report.append(f"FS={best['fs']} の5分間は {best['frames']:,}行を受信し、送受信カウンタ差{best.get('received_minus_sent',0)}、最大PC受信間隔{best['host_interval_ms']['max']:.3f} msでした。[集計]({best['output']}/summary.json)、[受信ログ]({best['output']}/capture.jsonl)。")
    if long:
        report.append(f"常用設定について完走した試験は **FS={long['fs']}（公称{long['nominal_hz']:.3f} Hz）を{long['requested_s']:g}秒**。受信 **{long['frames']:,}行**、平均 **{long['host_hz']:.6f} 行/秒**、TX破棄・欠落・ADCエラー0、PC最大受信間隔 **{long['host_interval_ms']['max']:.3f} ms**でした。")
    if safest:
        report.append(f"最大長57 byte/行で60秒間の全判定を通過した最高の設定は **FS={safest['fs']}（公称{safest['nominal_hz']:.3f} Hz）**。{safest['requested_s']:g}秒間の受信は **{safest['host_hz']:.6f} 行/秒**でした。")
    report.extend(['','**最終スケッチの起動設定はFS=24（200 Hz）、通常5列CSV、SERIAL_BAUD=115200です。**',
        '通常形式の境界値は今回のデータ桁数とタイムスタンプ桁数での結果です。起動後時間・荷重値に依存しない常用設定とは区別します。',
        '最大長CSVでは200 Hzと184.6 Hzでも送受信行数が一致しましたが、PC側の受信間隔が一度100 msを超えて全条件は不合格でした。192 Hzの60秒試行は全条件を満たしましたが、低レート側にもPC遅延が出るため「192 Hz以下なら常に100 ms以内」とは保証できません。',
        '', '## 最終版の全試験', '',table(final),'',
        'PASS条件: ホストHz・デバイスフレームHz・ADC別読み出しHzが公称±2%、TX破棄・統計スキップ・ADCエラー・不正CSVがゼロ、送信カウンタ増分と受信行数が一致、機器側フレーム間隔が公称周期の2.5倍以下、ホスト無受信が100 ms以下。',
        'TX破棄はウォームアップ中も含めてゼロを要求します。表の破棄数は測定区間のみです。',
        '', '## 常用設定FS24の5分試験の詳細',''])
    if long:
        b=long
        report.extend([f"- フレーム数: {b['frames']:,}",f"- 設定: `{b['config']}`",
            f"- PC受信Hz: {b['host_hz']:.6f} / 機器時刻によるフレームHz: {b['device_frame_hz']:.6f}",
            f"- ADC別読み出しHz: {', '.join(f'{x:.6f}' for x in b['adc_read_hz'])}",
            f"- 機器側の行間隔: {json.dumps(b['device_interval_ms'])} ms",
            f"- ホスト側の行間隔: {json.dumps(b['host_interval_ms'])} ms",
            f"- 1行の長さ: {json.dumps(b['line_bytes'])} byte",
            f"- 4ch同値連続: {b['identical_frames']} / 不正CSV: {b['invalid_lines']}",
            f"- 速いADCの値の置き換え（CH1〜4）: {b['superseded_measurement']}",
            f"- デバイスの送信カウンタ増分: {b.get('device_sent_delta',b['final_stats']['sent']-b['initial_stats']['sent'])}",
            f"- [測定ログ]({b['output']}/capture.jsonl) / [集計]({b['output']}/summary.json)"])
    report.extend(['','## 帯域と常用設定の理由','',
        '8N1での公称容量は11520 byte/sです。通常CSVでは、4chがすべて8桁なら「タイムスタンプの桁数+37」byte/行になります。さらに約5秒ごとの統計行が加わります。',
        'たとえばFS=19の公称252.63 Hzは46 byte/行だけでも11621 byte/sとなり、公称帯域を超えます。今回短い行で合格しても、値や時刻の桁が増えると不適になります。',
        'FS=20の240 Hzも48 byte/行で11520 byte/sに達し、統計行と発振器の偏差の余裕がなくなります。タイムスタンプは起動後約2.78時間で11桁になります。',
        'WIDE試験では20桁のtimestampと8桁×4chに先頭ゼロを付け、値を変えずに57 byte/行に固定しました。これは通常運用の桁数より厳しい、uint64 timestampを含む最大幅の通信負荷です。',
        '200 Hzなら最大長データの公称帯域は11400 byte/sです。通常CSVの実運用ではこれより行が短く、通信余裕が増えます。最大長CSVの200 Hzは90秒および60秒の実測でTX破棄0、送受信行数一致でした。一方、PC側で最大313 ms/130 msのまとめ受信がありました。100 ms以内の到着遅延まで必要な用途では、今回の最大長CSV・200 Hz試験は合格していません。PC側の受信処理やUSB接続を別途評価してください。',
        '', '## 修正した取得・送信処理','',
        '1. 新規変換の確認を共有MISOのdigitalReadからSTATUS.RDY（bit7=0）へ変更。旧版の約322行/秒・多数の同値送信は解消しました。GPIO判定の電気的な誤判定理由までは確定していません。',
        '2. UART送信を独自4096 byteキュー＋uart_tx_charsによるFIFO非ブロッキング転送へ変更。ArduinoのSerial.writeが帯域超過時にADC読み出しを遅らせる状態を解消しました。',
        '3. timestamp_usと4ch rawを出力。送信できなかった行、ADCエラー、速いchの値の置き換えを別々に記録します。',
        '', '## 測定の限界・未完了試行','',
        '- timestamp_usは4chの値が揃った時点のESP32時刻です。ADCの厳密な同時変換時刻ではありません。',
        '- ADC内部クロックは独立しており、速いchは4ch行にまとめるまでに値が置き換わります。supersededを記録しており、UART破棄ゼロでも全ADCの全変換を保存する形式ではありません。',
        '- AD7193の内部上書きには通番がないため、未読変換の欠落をすべて直接数えられるわけではありません。',
        '- ホスト時刻はperf_counter_ns、機器時刻はesp_timer_get_time。いずれも外部基準で校正していません。Hzの小数表示桁数は絶対精度の保証ではありません。',
        '- 途中のv3試験（20260917_152914、240 Hz測定中）とv4のFS19・5分試験（20260917_153908）はOSのDevice not configuredで中断しました。macOSログではFT231XのUSB再列挙が確認できました。ケーブル・ハブ・給電など物理的な原因は未確定です。未完了試行を合格扱いにはしていません。',
        '- FS19は通常CSVの現在の桁数で300秒合格しました。タイムスタンプや生データの桁が増えると通信用バイト数が増え、同じ設定で無期限に維持できることは示していません。',
        '', '## 初期調査（旧送信処理を含む）','',table([x for x in entries if x['firmware']!='v4']),
        '', '## 再測定','',
        '```sh\nconda activate wm\ncd /Users/gyobu/Documents/weight_measurement/posturography/26\npython sweep_timestamp_rate.py --fs 19 --seconds 300\npython sweep_timestamp_rate.py --fs 20 --wide --seconds 30\npython sweep_timestamp_rate.py --fs 24 --wide --seconds 120\npython sweep_timestamp_rate.py --fs 24 --seconds 30\n```',
        '', '先にArduino IDEのシリアルモニタを閉じてください。最終確認として通常CSV・FS24で30秒合格し、Arduinoを200 Hzへ戻しました。再測定時も最後のコマンドで通常CSV・200 Hzに戻ります。',
        '', 'メーカー仕様: [AD7193データシート（STATUS、出力データレート式）](https://www.analog.com/media/en/technical-documentation/data-sheets/AD7193.pdf)',
        '',f'最終スケッチSHA-256: `{digest}`'])
    (root/'sampling_rate_limit_report.md').write_text('\n'.join(report)+'\n')
    (root/'sampling_rate_limit_results.json').write_text(json.dumps(entries,indent=2))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(1,2,figsize=(12,4.8),layout='constrained')
    for ax,items,title in zip(axes,[normal,wide],['Normal timestamp + 4ch CSV','Worst-case 57-byte CSV rows']):
        label_count={}
        for s in items:
            color='#238b45' if s['passed'] else '#cb181d'
            ax.scatter(s['nominal_hz'],s['host_hz'],c=color,s=65)
            n=label_count.get(s['fs'],0);label_count[s['fs']]=n+1
            ax.annotate(f"FS{s['fs']} / {s['requested_s']:g}s",(s['nominal_hz'],s['host_hz']),xytext=(4,8-17*n),textcoords='offset points',fontsize=9)
        ax.plot([180,310],[180,310],'--',color='gray',linewidth=1)
        ax.set(xlim=(175,310),ylim=(175,315),xlabel='Nominal sample rate (Hz)',ylabel='Measured CSV delivery (frames/s)',title=title)
        ax.grid(alpha=.2)
    fig.suptitle('115200 baud: green = pass; red = fail (including TX drops)')
    fig.savefig(root/'sampling_rate_limit.png',dpi=180);plt.close(fig)
    print(root/'sampling_rate_limit_report.md')

if __name__=='__main__':main()
