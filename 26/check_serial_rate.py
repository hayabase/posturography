#!/usr/bin/env python3
"""Measure host-observed 4ch CSV delivery, not ADC conversion timestamps.
Requires pyserial. No commands or firmware uploads are sent to the device.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import select
import statistics
import sys
import time
import serial
from serial.tools import list_ports


def percentile(values, fraction):
    a = sorted(values)
    if not a:
        return None
    x = (len(a) - 1) * fraction
    lo = int(x)
    return a[lo] + (a[min(lo + 1, len(a) - 1)] - a[lo]) * (x - lo)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port')
    p.add_argument('--list-ports', action='store_true')
    p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--duration', type=float, default=300)
    p.add_argument('--warmup', type=float, default=3)
    p.add_argument('--expected-hz', type=float, default=100)
    p.add_argument('--rate-tolerance-percent', type=float, default=1.0)
    p.add_argument('--stall-ms', type=float, default=100)
    p.add_argument('--output', type=Path, default=Path(__file__).resolve().parent / 'serial_rate_results')
    a = p.parse_args()
    ports = list(list_ports.comports())
    if a.list_ports:
        for port in ports:
            print(port.device, port.description, port.hwid)
        return 0
    if min(a.duration, a.expected_hz, a.stall_ms, a.baud) <= 0 or a.warmup < 0 or a.rate_tolerance_percent < 0:
        p.error('duration, rate, stall threshold and baud must be positive; warmup >= 0')
    if not a.port:
        candidates = [x.device for x in ports if 'usbserial' in x.device]
        if len(candidates) != 1:
            p.error('Specify --port; automatic selection requires exactly one usbserial port')
        a.port = candidates[0]
    folder = a.output / dt.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    folder.mkdir(parents=True, exist_ok=False)
    meta = dict(port=a.port, baud=a.baud, requested_duration_s=a.duration,
                warmup_s=a.warmup, expected_hz=a.expected_hz, python=sys.executable,
                started_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
                clock=vars(time.get_clock_info('perf_counter')),
                timestamp_method='perf_counter_ns immediately after os.read; lines in same read share timestamp')
    (folder / 'metadata.json').write_text(json.dumps(meta, indent=2))
    print('OUTPUT', folder, flush=True)
    timestamps = []
    values = []
    invalid = comments = byte_count = batches = multi_line_batches = 0
    error = None
    interrupted = False
    start = end = None
    ser = serial.Serial(port=None, baudrate=a.baud, timeout=0, exclusive=True)
    ser.dtr = False
    ser.rts = False
    ser.port = a.port
    try:
        ser.open()
        warm_end = time.perf_counter() + a.warmup
        with (folder / 'warmup.bin').open('wb') as warm:
            while time.perf_counter() < warm_end:
                if select.select([ser.fileno()], [], [], min(0.1, max(0, warm_end-time.perf_counter())))[0]:
                    warm.write(os.read(ser.fileno(), 65536))
        # Discard through first newline to avoid counting a partial warmup line.
        aligned = False
        buffer = b''
        start = time.perf_counter_ns()
        deadline = start + int(a.duration * 1e9)
        next_progress = start + 30_000_000_000
        with (folder / 'received.bin').open('wb') as raw, (folder / 'events.jsonl').open('w') as log:
            while time.perf_counter_ns() < deadline:
                remaining = (deadline - time.perf_counter_ns()) / 1e9
                if not select.select([ser.fileno()], [], [], max(0, min(0.1, remaining)))[0]:
                    continue
                data = os.read(ser.fileno(), 65536)
                stamp = time.perf_counter_ns()
                if not data:
                    raise OSError('Serial device returned EOF')
                raw.write(data)
                byte_count += len(data)
                batches += 1
                buffer += data
                lines = buffer.split(b'\n')
                buffer = lines.pop()
                valid_in_batch = 0
                for line in lines:
                    if not aligned:
                        aligned = True
                        continue
                    text = line.decode('ascii', errors='replace').strip()
                    record = {'host_ns': stamp, 'elapsed_s': (stamp-start)/1e9, 'batch': batches}
                    fields = text.split(',')
                    if text.startswith('#'):
                        comments += 1
                        record.update(kind='comment', text=text)
                    elif len(fields) == 4 and all(x.isascii() and x.isdecimal() and 0 <= int(x) <= 0xFFFFFF for x in fields):
                        v = list(map(int, fields))
                        timestamps.append(stamp)
                        values.append(v)
                        valid_in_batch += 1
                        record.update(kind='sample', raw=v)
                    else:
                        invalid += 1
                        record.update(kind='invalid', text=text)
                    log.write(json.dumps(record) + '\n')
                multi_line_batches += valid_in_batch > 1
                if len(buffer) > 65536:
                    raise ValueError('No newline within 64 KiB; check CSV mode and baud')
                if stamp >= next_progress:
                    elapsed = (stamp-start)/1e9
                    print(f'{elapsed:.1f}s: {len(timestamps)} frames, {len(timestamps)/elapsed:.4f} Hz, invalid={invalid}', flush=True)
                    log.flush()
                    next_progress = stamp + 30_000_000_000
    except KeyboardInterrupt:
        interrupted = True
    except (OSError, serial.SerialException, ValueError) as exc:
        error = str(exc)
    finally:
        end = time.perf_counter_ns()
        if ser.is_open:
            ser.close()
    elapsed = (end-start)/1e9 if start else 0
    t = [(x-start)/1e9 for x in timestamps]
    intervals = [(b-a)*1000 for a,b in zip(t,t[1:])]
    rate = (len(t)-1)/(t[-1]-t[0]) if len(t)>1 and t[-1]>t[0] else None
    # Fit elapsed time against frame index. This reduces endpoint USB timing noise.
    fit_rate = None
    if len(t)>1:
        center = (len(t)-1)/2
        mean_t = statistics.mean(t)
        slope = sum((i-center)*(v-mean_t) for i,v in enumerate(t)) / sum((i-center)**2 for i in range(len(t)))
        fit_rate = 1/slope if slope>0 else None
    windows = []
    for j in range(int(min(elapsed, a.duration)//10)):
        count = sum(j*10 <= x < (j+1)*10 for x in t)
        windows.append({'start_s':j*10, 'end_s':(j+1)*10, 'frames':count, 'hz':count/10})
    edge_gaps = ([t[0]*1000, max(0,elapsed-t[-1])*1000] if t else [elapsed*1000])
    max_silence = max(intervals + edge_gaps)
    completed = error is None and not interrupted and elapsed >= a.duration
    repeated = sum(x == y for x, y in zip(values, values[1:]))
    rate_ok = bool(fit_rate and abs(fit_rate/a.expected_hz-1)*100 <= a.rate_tolerance_percent)
    result = dict(completed=completed, error=error, interrupted=interrupted, elapsed_s=elapsed,
        frames=len(t), frames_per_observed_second=len(t)/elapsed if elapsed else None,
        endpoint_hz=rate, regression_hz=fit_rate,
        deviation_from_expected_percent=(fit_rate/a.expected_hz-1)*100 if fit_rate else None,
        rate_within_tolerance=rate_ok, rate_tolerance_percent=a.rate_tolerance_percent,
        consecutive_identical_frames=repeated,
        consecutive_identical_fraction=repeated/(len(values)-1) if len(values)>1 else None,
        channel_value_changes=[sum(x[i]!=y[i] for x,y in zip(values,values[1:])) for i in range(4)],
        invalid_lines=invalid, comment_lines=comments, bytes_received=byte_count,
        serial_8n1_capacity_fraction=byte_count*10/(elapsed*a.baud) if elapsed else None,
        read_batches=batches, multiple_frame_batches=multi_line_batches,
        interval_ms=dict(mean=statistics.mean(intervals) if intervals else None,
            stdev=statistics.pstdev(intervals) if intervals else None,
            min=min(intervals) if intervals else None, p50=percentile(intervals,.5),
            p95=percentile(intervals,.95), p99=percentile(intervals,.99), max=max(intervals) if intervals else None),
        stall_threshold_ms=a.stall_ms, interframe_gaps_above_threshold=sum(x>a.stall_ms for x in intervals),
        max_silence_including_start_end_ms=max_silence,
        continuous_delivery_check_passed=bool(completed and len(t)>1 and invalid==0 and max_silence<=a.stall_ms),
        windows_10s=windows,
        channel_ranges=[{'channel':i+1,'min':min(v[i] for v in values),'max':max(v[i] for v in values)} for i in range(4)] if values else [],
        limitations=['Host receive timestamps include USB buffering and OS scheduling.',
            'Nanosecond clock resolution is not nanosecond measurement accuracy; host clock is not externally calibrated.',
            'Identical values do not by themselves prove duplicate acquisition; steady inputs can repeat.',
            'CSV has no sequence numbers or device timestamps: exact sample loss, ADC jitter and per-ADC conversion rate cannot be established.',
            'A 4ch CSV frame is emitted only when all four ADCs have fresh data; it is not proof of simultaneous conversion.',
            'One initial line is deliberately discarded to synchronize after warmup.'])
    (folder / 'summary.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result['continuous_delivery_check_passed'] and rate_ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
