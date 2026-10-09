#!/usr/bin/env python3
"""Receive and assess the accompanying AD7193 x4 26-byte binary protocol."""
import argparse
import binascii
import datetime as dt
import json
import os
from pathlib import Path
import select
import statistics
import time
import serial

DATA_MAGIC=b'\xa5\x5a'
STATUS_MAGIC=b'\xa5\x5b'
DATA_LEN=26
STATUS_LEN=84


def u24(b): return int.from_bytes(b,'little')


def decode_packet(packet):
    if len(packet) not in (DATA_LEN,STATUS_LEN) or packet[:2] not in (DATA_MAGIC,STATUS_MAGIC):
        raise ValueError('framing')
    if binascii.crc_hqx(packet[:-2],0xffff)!=int.from_bytes(packet[-2:],'little'):
        raise ValueError('crc')
    if packet[:2]==DATA_MAGIC:
        if len(packet)!=DATA_LEN: raise ValueError('data length')
        return dict(kind='data',seq=int.from_bytes(packet[2:4],'little'),
            t_us=int.from_bytes(packet[4:12],'little'),raw=[u24(packet[i:i+3]) for i in (12,15,18,21)])
    if len(packet)!=STATUS_LEN or packet[2]!=1:raise ValueError('status version')
    p=dict(kind='status',version=packet[2],init_mask=packet[3],fs=int.from_bytes(packet[4:6],'little'),
        t_us=int.from_bytes(packet[6:14],'little'))
    names=('frames','sent','tx_drop','stat_skip')
    p.update({name:int.from_bytes(packet[14+4*i:18+4*i],'little') for i,name in enumerate(names)})
    p['reads']=[int.from_bytes(packet[i:i+4],'little') for i in (30,34,38,42)]
    p['superseded']=[int.from_bytes(packet[i:i+4],'little') for i in (46,50,54,58)]
    p['adc_errors']=[int.from_bytes(packet[i:i+4],'little') for i in (62,66,70,74)]
    p['max_loop_gap_us']=int.from_bytes(packet[78:82],'little')
    return p


class FrameReader:
    def __init__(self,ser):
        self.ser=ser;self.buffer=bytearray();self.pending=[]
        self.discarded=0;self.crc_errors=0;self.version_errors=0
    def next(self,deadline,rawfile=None):
        while time.perf_counter()<deadline:
            if self.pending:return self.pending.pop(0)
            if self.buffer:
                at=self.buffer.find(b'\xa5')
                if at<0:
                    self.discarded+=len(self.buffer);self.buffer.clear()
                elif at>0:
                    self.discarded+=at;del self.buffer[:at]
                if len(self.buffer)>=2 and self.buffer[:2] not in (DATA_MAGIC,STATUS_MAGIC):
                    self.discarded+=1;del self.buffer[0]
                    continue
                if len(self.buffer)>=2:
                    length=DATA_LEN if self.buffer[:2]==DATA_MAGIC else STATUS_LEN
                    if len(self.buffer)>=length:
                        packet=bytes(self.buffer[:length])
                        try:decoded=decode_packet(packet)
                        except ValueError as exc:
                            if str(exc)=='crc':self.crc_errors+=1
                            else:self.version_errors+=1
                            self.discarded+=1;del self.buffer[0]
                            continue
                        del self.buffer[:length]
                        return time.perf_counter_ns(),decoded
            if not select.select([self.ser.fileno()],[],[],max(0,min(.1,deadline-time.perf_counter())))[0]:continue
            data=os.read(self.ser.fileno(),65536)
            if not data:raise OSError('Serial EOF')
            if rawfile:rawfile.write(data)
            self.buffer.extend(data)
        return None


def quantile(a,q):
    if not a:return None
    a=sorted(a);x=(len(a)-1)*q;i=int(x)
    return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(x-i)


def rate(t):
    if len(t)<2:return None
    origin=t[0];y=[(v-origin)/1e9 for v in t]
    ci=(len(t)-1)/2;cy=statistics.mean(y)
    slope=sum((i-ci)*(v-cy) for i,v in enumerate(y))/sum((i-ci)**2 for i in range(len(t)))
    return 1/slope if slope>0 else None


def measure(ser,reader,fs,seconds,warmup,root):
    folder=root/f'fs_{fs}_{dt.datetime.now():%H%M%S}';folder.mkdir()
    with (folder/'received.bin').open('wb') as raw, (folder/'events.jsonl').open('w') as events:
        def receive(deadline):
            v=reader.next(deadline,raw)
            if v: events.write(json.dumps(dict(host_ns=v[0],**v[1]))+'\n')
            return v
        ser.write(f'FS={fs}\n'.encode('ascii'))
        end=time.perf_counter()+8;config=None
        while time.perf_counter()<end:
            item=receive(end)
            if item and item[1]['kind']=='status' and item[1]['fs']==fs and item[1]['frames']<100:
                config=item[1];break
        if config is None:raise RuntimeError(f'No FS={fs} acknowledgement')
        end=time.perf_counter()+warmup
        while receive(end):pass
        ser.write(b'STATS\n');end=time.perf_counter()+5;initial=None
        while time.perf_counter()<end:
            item=receive(end)
            if item and item[1]['kind']=='status' and item[1]['fs']==fs:
                initial=item[1];break
        if initial is None:raise RuntimeError('No initial status')
        baseline=(reader.discarded,reader.crc_errors,reader.version_errors)
        samples=[];status=[]
        start=time.perf_counter();end=start+seconds;next_progress=start+30;requested=False;final=None
        while time.perf_counter()<end+5:
            if not requested and time.perf_counter()>=end:
                ser.write(b'STATS\n');requested=True
            item=receive(end+5 if requested else end)
            if not item:continue
            if item[1]['kind']=='data':samples.append(item)
            else:
                status.append(item[1])
                if requested and item[1]['fs']==fs:final=item[1];break
            if time.perf_counter()>=next_progress:
                print(f'FS={fs} {time.perf_counter()-start:.1f}s frames={len(samples)} crc={reader.crc_errors-baseline[1]}',flush=True)
                events.flush();next_progress=time.perf_counter()+30
    if final is None:raise RuntimeError('No final status')
    host=[stamp for stamp,_ in samples]
    device=[data['t_us']*1000 for _,data in samples]
    hi=[(b-a)/1e6 for a,b in zip(host,host[1:])]
    di=[(b-a)/1e6 for a,b in zip(device,device[1:])]
    gaps=[(b[1]['seq']-a[1]['seq']-1)&0xffff for a,b in zip(samples,samples[1:])]
    nominal=4800/fs
    statspan=(final['t_us']-initial['t_us'])/1e6
    read_rates=[(b-a)/statspan for a,b in zip(initial['reads'],final['reads'])]
    host_hz=rate(host);device_hz=rate(device)
    sent_delta=final['sent']-initial['sent']
    drop_delta=final['tx_drop']-initial['tx_drop']
    errors_delta=[b-a for a,b in zip(initial['adc_errors'],final['adc_errors'])]
    missing=sum(gaps)
    reasons=[]
    if config['init_mask']!=15:reasons.append('ADC init mask is not 15')
    if host_hz is None or abs(host_hz/nominal-1)>.02:reasons.append('host rate outside nominal +/-2%')
    if device_hz is None or abs(device_hz/nominal-1)>.02:reasons.append('device rate outside nominal +/-2%')
    if any(abs(x/nominal-1)>.02 for x in read_rates):reasons.append('ADC read rate outside nominal +/-2%')
    if final['tx_drop'] or drop_delta:reasons.append('UART frame drops')
    if final['stat_skip']:reasons.append('status frames skipped')
    if final['adc_errors']!=[0]*4 or any(errors_delta):reasons.append('ADC error/no reference')
    if len(samples)!=sent_delta:reasons.append('received frame count differs from device sent counter')
    if missing:reasons.append('sequence gap')
    if reader.discarded>baseline[0] or reader.crc_errors>baseline[1] or reader.version_errors>baseline[2]:reasons.append('framing or CRC error')
    if any(x<=0 for x in di):reasons.append('non-monotonic device timestamp')
    if any(x>2500/nominal for x in di):reasons.append('device frame gap >2.5 periods')
    if hi and max(hi)>100:reasons.append('host gap >100ms')
    result=dict(fs=fs,nominal_hz=nominal,seconds_requested=seconds,frames_received=len(samples),
        passed=not reasons,failure_reasons=reasons,host_hz=host_hz,device_hz=device_hz,adc_read_hz=read_rates,
        device_sent_delta=sent_delta,received_minus_sent=len(samples)-sent_delta,tx_drop_delta=drop_delta,
        sequence_missing=missing,crc_errors=reader.crc_errors-baseline[1],framing_discard_bytes=reader.discarded-baseline[0],
        status_version_errors=reader.version_errors-baseline[2],host_gap_ms_max=max(hi) if hi else None,
        host_gap_ms_p99=quantile(hi,.99),device_interval_ms_max=max(di) if di else None,
        device_interval_ms_p99=quantile(di,.99),initial_status=initial,final_status=final,
        superseded_delta=[b-a for a,b in zip(initial['superseded'],final['superseded'])],
        output=str(folder))
    (folder/'summary.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result),flush=True)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='/dev/cu.usbserial-D30AKO0H')
    p.add_argument('--fs',nargs='+',type=int,default=[24,20,16,12,11,10])
    p.add_argument('--seconds',type=float,default=30)
    p.add_argument('--warmup',type=float,default=3)
    p.add_argument('--output',type=Path,default=Path(__file__).resolve().parent/'results')
    a=p.parse_args()
    if a.seconds<=0 or a.warmup<0 or any(x<1 or x>1023 for x in a.fs):p.error('Invalid duration or FS')
    root=a.output/dt.datetime.now().strftime('%Y%m%d_%H%M%S');root.mkdir(parents=True)
    (root/'conditions.json').write_text(json.dumps(dict(port=a.port,baud=115200,fs=a.fs,seconds=a.seconds,warmup=a.warmup,
        criteria='rate +/-2%, zero UART loss/CRC/ADC errors, device interval <=2.5 periods, host gap <=100ms'),indent=2))
    print('OUTPUT',root,flush=True)
    results=[]
    try:
        with serial.Serial(a.port,115200,timeout=0,exclusive=True) as ser:
            reader=FrameReader(ser)
            end=time.perf_counter()+2
            with (root/'startup.bin').open('wb') as raw:
                while reader.next(end,raw):pass
            for fs in a.fs:
                results.append(measure(ser,reader,fs,a.seconds,a.warmup,root))
                (root/'results.json').write_text(json.dumps(results,indent=2))
    except (OSError,RuntimeError,ValueError,KeyboardInterrupt) as exc:
        (root/'failure.json').write_text(json.dumps(dict(fs=fs if 'fs' in locals() else None,error=repr(exc)),indent=2))
        raise
    print('RESULTS',root/'results.json',flush=True)

if __name__=='__main__':main()
