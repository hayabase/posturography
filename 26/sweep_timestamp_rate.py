#!/usr/bin/env python3
"""Sweep AD7193 FS via commands; measure timestamp+4ch CSV at fixed 115200 baud.
Use only with the accompanying firmware v4. This changes the running FS.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import select
import statistics
import time
import serial


class Reader:
    def __init__(self, ser): self.ser=ser; self.buffer=b''; self.pending=[]
    def line(self, deadline):
        while time.perf_counter()<deadline:
            if self.pending: return self.pending.pop(0)
            if not select.select([self.ser.fileno()],[],[],max(0,min(.1,deadline-time.perf_counter())))[0]: continue
            chunk=os.read(self.ser.fileno(),65536); stamp=time.perf_counter_ns()
            if not chunk: raise OSError('Serial EOF')
            self.buffer+=chunk
            lines=self.buffer.split(b'\n'); self.buffer=lines.pop()
            self.pending.extend((stamp,x) for x in lines)
            if len(self.buffer)>65536: raise ValueError('No line delimiter')
        return None


def quantile(a,q):
    a=sorted(a)
    if not a:return None
    x=(len(a)-1)*q;i=int(x)
    return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(x-i)


def fit_rate(t):
    if len(t)<2:return None
    origin=t[0]; vals=[(x-origin)/1e9 for x in t]
    center=(len(t)-1)/2; avg=statistics.mean(vals)
    slope=sum((i-center)*(v-avg) for i,v in enumerate(vals))/sum((i-center)**2 for i in range(len(vals)))
    return 1/slope if slope>0 else None


def parse_sample(text):
    v=text.split(',')
    if len(v)!=5 or not all(x.isascii() and x.isdecimal() for x in v): return None
    v=list(map(int,v))
    return v if v[0] <= 0xffffffffffffffff and all(x<=0xffffff for x in v[1:]) else None


def measure(ser,reader,fs,seconds,warmup,root):
    directory=root/f'fs_{fs}_{dt.datetime.now():%H%M%S}'
    directory.mkdir()
    logs=(directory/'capture.jsonl').open('w')
    def receive(deadline):
        item=reader.line(deadline)
        if item:
            stamp,line=item
            text=line.decode('ascii',errors='replace').strip()
            logs.write(json.dumps({'host_ns':stamp,'line':text})+'\n')
            return stamp,text
        return None
    ser.write(f'FS={fs}\n'.encode())
    deadline=time.perf_counter()+8; config=None
    while time.perf_counter()<deadline:
        item=receive(deadline)
        if item and item[1].startswith(f'#CONFIG fs={fs} '):config=item[1];break
    if config is None: raise RuntimeError(f'No FS={fs} acknowledgement')
    deadline=time.perf_counter()+warmup
    while receive(deadline):pass
    ser.write(b'STATS\n');deadline=time.perf_counter()+5; initial=None
    while time.perf_counter()<deadline:
        item=receive(deadline)
        if item and item[1].startswith('#STATS '):
            initial=json.loads(item[1][7:]);break
    if initial is None: raise RuntimeError('No initial stats')
    start=time.perf_counter();deadline=start+seconds;progress=start+30
    samples=[]; stats=[]; invalid=[];metadata=[]; widths=[]
    end_stats=None; requested=False
    while time.perf_counter()<deadline+5:
        if not requested and time.perf_counter()>=deadline:
            ser.write(b'STATS\n');requested=True
        item=receive(deadline+5 if requested else deadline)
        if not item: continue
        stamp,text=item
        if text.startswith('#STATS '):
            d=json.loads(text[7:]);stats.append(d)
            if requested: end_stats=d;break
        elif text.startswith('#'):metadata.append(text)
        else:
            v=parse_sample(text)
            if v is None:invalid.append(text)
            else:samples.append((stamp,v));widths.append(len(text)+1)
        if time.perf_counter()>=progress:
            print(f'FS={fs}: {time.perf_counter()-start:.1f}s, samples={len(samples)}, invalid={len(invalid)}',flush=True)
            logs.flush();progress=time.perf_counter()+30
    elapsed=time.perf_counter()-start;logs.close()
    if end_stats is None:raise RuntimeError('No final stats')
    host=[x[0] for x in samples]; device=[x[1][0]*1000 for x in samples]
    hi=[(b-a)/1e6 for a,b in zip(host,host[1:])]; di=[(b-a)/1e6 for a,b in zip(device,device[1:])]
    nominal=4800/fs; stats_span=(end_stats['t_us']-initial['t_us'])/1e6
    read_rates=[(b-a)/stats_span for a,b in zip(initial['reads'],end_stats['reads'])]
    txdrop=end_stats['tx_drop']-initial['tx_drop']; host_rate=fit_rate(host);dev_rate=fit_rate(device)
    adcerrors=[b-a for a,b in zip(initial['adc_errors'],end_stats['adc_errors'])]
    repeated=sum(x[1][1:]==y[1][1:] for x,y in zip(samples,samples[1:]))
    sent_delta=end_stats['sent']-initial['sent']
    reasons=[]
    if len(samples)!=sent_delta:reasons.append('received rows differ from device sent counter')
    if 'init_mask=15 ' not in config: reasons.append('ADC init mask is not 15')
    if host_rate is None or abs(host_rate/nominal-1)>.02:reasons.append('host rate outside nominal +/-2%')
    if dev_rate is None or abs(dev_rate/nominal-1)>.02:reasons.append('device frame rate outside nominal +/-2%')
    if any(abs(x/nominal-1)>.02 for x in read_rates):reasons.append('ADC read rate outside nominal +/-2%')
    if end_stats['tx_drop']:reasons.append('UART whole-frame drops')
    if end_stats['stat_skip']:reasons.append('UART diagnostics skipped')
    if invalid:reasons.append('invalid CSV')
    if metadata:reasons.append('unexpected metadata/reset')
    if any(adcerrors) or any(end_stats['adc_errors']):reasons.append('ADC ERR/NOREF')
    if any(x<=0 for x in di):reasons.append('non-monotonic device timestamp')
    if any(x>2500/nominal for x in di):reasons.append('device frame gap >2.5 nominal periods')
    if hi and max(hi)>100:reasons.append('host gap >100 ms')
    summary=dict(fs=fs,nominal_hz=nominal,requested_s=seconds,observed_s=elapsed,frames=len(samples),
        passed=not reasons,failure_reasons=reasons,host_hz=host_rate,device_frame_hz=dev_rate,
        adc_read_hz=read_rates,invalid_lines=len(invalid),unexpected_metadata=metadata,
        tx_drop_measurement=txdrop,device_sent_delta=sent_delta,received_minus_sent=len(samples)-sent_delta,initial_stats=initial,final_stats=end_stats,
        superseded_measurement=[b-a for a,b in zip(initial['superseded'],end_stats['superseded'])],
        identical_frames=repeated,identical_fraction=repeated/max(1,len(samples)-1),
        device_interval_ms={'min':min(di) if di else None,'p50':quantile(di,.5),'p99':quantile(di,.99),'max':max(di) if di else None},
        host_interval_ms={'p50':quantile(hi,.5),'p99':quantile(hi,.99),'max':max(hi) if hi else None},
        line_bytes={'min':min(widths) if widths else None,'max':max(widths) if widths else None,'mean':statistics.mean(widths) if widths else None},
        device_time_us_first=device[0]//1000 if device else None,device_time_us_last=device[-1]//1000 if device else None,
        baud=115200,config=config,output=str(directory))
    (directory/'summary.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps(summary),flush=True)
    return summary


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='/dev/cu.usbserial-D30AKO0H')
    p.add_argument('--fs',nargs='+',type=int,default=[48,40,32,24,20,16])
    p.add_argument('--seconds',type=float,default=30)
    p.add_argument('--wide',action='store_true',help='Zero-pad real values to worst-case 57-byte CSV rows')
    p.add_argument('--warmup',type=float,default=3)
    p.add_argument('--output',type=Path,default=Path(__file__).resolve().parent/'timestamp_rate_results')
    a=p.parse_args()
    if a.seconds<=0 or a.warmup<0 or any(x<1 or x>1023 for x in a.fs):p.error('Invalid duration or FS')
    root=a.output/dt.datetime.now().strftime('%Y%m%d_%H%M%S');root.mkdir(parents=True)
    print('OUTPUT',root,flush=True)
    (root/'conditions.json').write_text(json.dumps({'fs':a.fs,'seconds':a.seconds,'warmup':a.warmup,'baud':115200,'port':a.port,'wide':a.wide,'criteria':'rate +/-2%, zero TX drops/stat skips/ADC errors/invalid lines; device gap <=2.5 periods; host gap <=100ms'},indent=2))
    results=[]
    with serial.Serial(a.port,115200,timeout=0,exclusive=True) as ser:
        reader=Reader(ser)
        deadline=time.perf_counter()+3
        with (root/'startup.jsonl').open('w') as log:
            while True:
                x=reader.line(deadline)
                if not x:break
                log.write(json.dumps({'host_ns':x[0],'line':x[1].decode(errors='replace')})+'\n')
        ser.write(b'WIDE=1\n' if a.wide else b'WIDE=0\n')
        for fs in a.fs:
            try:
                result=measure(ser,reader,fs,a.seconds,a.warmup,root)
            except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as exc:
                (root/'failure.json').write_text(json.dumps({'fs':fs,'error':repr(exc),'completed':False},indent=2))
                raise
            results.append(result)
            (root/'results.json').write_text(json.dumps(results,indent=2))
    print('RESULTS',root/'results.json',flush=True)

if __name__=='__main__':main()
