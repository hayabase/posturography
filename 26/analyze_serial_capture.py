#!/usr/bin/env python3
"""Re-analyze a saved check_serial_rate.py capture and generate a plot/report."""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    a=p.parse_args(); root=a.capture
    s=json.loads((root/'summary.json').read_text())
    meta=json.loads((root/'metadata.json').read_text())
    samples=[]; notes=[]
    with (root/'events.jsonl').open() as f:
        for line in f:
            d=json.loads(line)
            if d['kind']=='sample': samples.append(d)
            else: notes.append(d)
    if len(samples)<2:
        raise SystemExit('At least two samples required')
    t=[x['elapsed_s'] for x in samples]; v=[x['raw'] for x in samples]
    span=t[-1]-t[0]
    repeats=sum(x==y for x,y in zip(v,v[1:]))
    changes=[sum(x[i]!=y[i] for x,y in zip(v,v[1:])) for i in range(4)]
    runs=[]; n=1
    for x,y in zip(v,v[1:]):
        if x==y: n+=1
        else: runs.append(n); n=1
    runs.append(n)
    windows=s['windows_10s']
    init=[x.decode('ascii',errors='replace') for x in (root/'warmup.bin').read_bytes().splitlines() if x.startswith(b'#')]
    extra=dict(consecutive_identical_frames=repeats,consecutive_identical_percent=100*repeats/(len(v)-1),
        changed_value_counts=changes,changed_value_hz=[x/span for x in changes],
        identical_run_lengths=dict(sorted(Counter(runs).items())),
        ten_second_hz_min=min(x['hz'] for x in windows) if windows else None,
        ten_second_hz_max=max(x['hz'] for x in windows) if windows else None,
        ten_second_hz_stdev=statistics.pstdev(x['hz'] for x in windows) if windows else None,
        init_messages=init, captured_non_sample_lines=notes)
    (root/'analysis.json').write_text(json.dumps(extra,indent=2,ensure_ascii=False))
    report=f'''# 4ch AD7193 シリアル受信測定

測定開始（UTC）: {meta['started_utc']}  
ポート: `{meta['port']}` / {meta['baud']} baud / 8N1  
実行Python: `{meta['python']}`

## 実測結果

|項目|結果|
|---|---:|
|観測時間|{s['elapsed_s']:.6f} 秒|
|正常な4ch CSV行数|{s['frames']:,}|
|受信レート（時刻対行番号の最小二乗回帰）|{s['regression_hz']:.6f} 行/秒|
|受信レート（先頭〜末尾）|{s['endpoint_hz']:.6f} 行/秒|
|10秒窓の最小〜最大|{extra['ten_second_hz_min']}〜{extra['ten_second_hz_max']} 行/秒|
|設定100 Hzからの偏差|{s['deviation_from_expected_percent']:+.3f}%|
|不正なCSV行|{s['invalid_lines']}|
|測定中のコメント行|{s['comment_lines']}|
|受信間隔の中央値|{s['interval_ms']['p50']:.6f} ms|
|受信間隔の99パーセンタイル|{s['interval_ms']['p99']:.6f} ms|
|最大無受信時間（開始・終了区間含む）|{s['max_silence_including_start_end_ms']:.6f} ms|
|{s['stall_threshold_ms']:g} ms超の行間の途絶|{s['interframe_gaps_above_threshold']}|
|直前と4chすべて同値の行|{repeats:,}（{extra['consecutive_identical_percent']:.3f}%）|
|複数行が同時に読み出されたバッチ|{s['multiple_frame_batches']:,}|
|受信バイト数|{s['bytes_received']:,}|
|8N1・指定baudに対する受信量の比|{s['serial_8n1_capacity_fraction']*100:.3f}%|

測定完了: {s['completed']}。通信継続判定（形式エラーなし・指定時間完走・最大無受信が閾値以内）: {s['continuous_delivery_check_passed']}。
これは100 Hzで新しい測定値を送れているかの判定とは別です。

## 値の更新と原因の切り分け

各chの「直前から値が変わった回数/秒」は、{', '.join(f'CH{i+1}: {x:.4f}' for i,x in enumerate(extra['changed_value_hz']))}。
これはADCの実サンプリングレートではありません。同じ値になる別サンプルや量子化の影響があり、更新回数だけでは変換回数は確定できません。

今回の受信行数が公称100 Hzを大きく超え、同値の連続が多数見られることから、既読の変換データを再送している可能性があります。
元スケッチの `ad7193IsReady()`（共有MISOをGPIOとして読む処理）と `csvFreshMask` の更新が、次の調査対象です。
実機上のRDY信号とSTATUSレジスタを比較していないため、原因はまだ確定していません。
AD7193は同じデータレジスタを複数回読むことが可能です（[メーカー資料、p.34](https://www.analog.com/media/en/technical-documentation/data-sheets/AD7193.pdf)）。
新規変換の判定にはSTATUSレジスタのRDY（bit7）を読み、GPIO判定と比較すると切り分けできます。

## 精度と解釈

- `perf_counter_ns()`で読み出し直後を記録。長時間の回帰と先頭末尾の両方から受信レートを推定。
- ns単位の時刻表現は、ns精度を保証するものではありません。PC時計の外部校正は行っていません。
- USB・OSによるまとめ読みがあるため、同じ読み出し内の行は同時刻です。間隔0 msや約5 msの揺らぎをADCのジッタと解釈しないでください。
- 現在のCSVに機器側時刻・連番・CRCがないため、正確な欠落数、値の破損、各ADCの変換周期は確定できません。
- より正確な機器側周期測定には、新規変換判定を検証した上でADC別時刻と連番を送信し、PC受信時刻と分けて評価する必要があります。
- ウォームアップ後の最初の1行は同期のため除外。測定区間の前後の端数行は正常行数に含めません。
- 実測した期間の結果であり、長期の連続動作を保証するものではありません。

## 起動時ログ

```
{chr(10).join(init)}
```

## 保存ファイル

- `metadata.json`: 実行条件・時計情報
- `warmup.bin`: 測定前の生データ（起動メッセージ含む場合あり）
- `received.bin`: 測定中の受信生データ
- `events.jsonl`: 受信時刻・バッチ番号・値・不正行
- `summary.json`: 測定時の集計
- `analysis.json`: 重複・値変化・起動メッセージの再解析
- `rate_diagnostics.png`: 時系列と受信間隔の図
'''
    (root/'report.md').write_text(report)
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(3,1,figsize=(11,10),layout='constrained')
    axes[0].plot([x['start_s']+5 for x in windows],[x['hz'] for x in windows],marker='.')
    axes[0].axhline(meta['expected_hz'],color='red',linestyle='--',label='Expected 100 Hz')
    axes[0].set(ylabel='CSV frames/s',xlabel='Host elapsed time (s)',title='Serial delivery rate (10-second windows)'); axes[0].legend()
    delta=[(b-a)*1000 for a,b in zip(t,t[1:])]
    axes[1].hist(delta,bins=100)
    axes[1].set(xlabel='Host inter-frame interval (ms)',ylabel='Count',title='USB/OS delivery intervals; not ADC conversion jitter')
    for i in range(4):
        rates=[]
        for win in windows:
            rates.append(sum(x[i]!=y[i] and win['start_s']<=stamp<win['end_s'] for x,y,stamp in zip(v,v[1:],t[1:]))/10)
        axes[2].plot([x['start_s']+5 for x in windows],rates,label=f'CH{i+1}')
    axes[2].set(xlabel='Host elapsed time (s)',ylabel='Value changes/s',title='Observed value changes; not an independent sampling-rate measurement');axes[2].legend()
    for ax in axes: ax.grid(alpha=.2)
    fig.savefig(root/'rate_diagnostics.png',dpi=160);plt.close(fig)
    print(json.dumps(extra,indent=2,ensure_ascii=False))
    print(root/'report.md')

if __name__=='__main__':main()
