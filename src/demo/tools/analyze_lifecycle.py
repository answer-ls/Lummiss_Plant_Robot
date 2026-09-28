"""同窗长频谱和寄存器差异；不将跨状态窗口自动归因为某一个状态。"""
import json,sys,wave
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy.signal import welch
p=Path(sys.argv[1]);meta=json.loads((p/'lifecycle.json').read_text());result={}
hw_names=['rx_conf','rx_conf1','rx_tdm','tx_conf','tx_conf1','tx_tdm','clk11','clk12','clk13','clk14']
base=meta['records']['0']['snapshots'].get('BEFORE',{})
for key,item in meta['records'].items():
    r=dict(samples=item['size']//8,crossed=item['crossed'],error=item['error'],valid_full_window=item['size']==192000 and item['error']==0,slots={},register_diffs={})
    for edge,snap in item['snapshots'].items():
        r['register_diffs'][edge]=dict(es7210={k:[base.get('regs',{}).get(k),v] for k,v in snap['regs'].items() if v!=base.get('regs',{}).get(k)},i2s={n:[a,b] for n,a,b in zip(hw_names,base.get('hw',[]),snap['hw']) if a!=b},snapshot_delay_from_mark_us=snap['us']-item['mark_us'])
    before=item['snapshots'].get('BEFORE',{});after=item['snapshots'].get('AFTER',{})
    r['format_matches_4x16']={edge:((snap['hw'][1]>>14)&31)==15 and ((snap['hw'][1]>>27)&31)==15 and ((snap['hw'][2]>>16)&15)==3 for edge,snap in item['snapshots'].items()}
    r['within_window_diff']=dict(es7210={k:[before.get('regs',{}).get(k),v] for k,v in after.get('regs',{}).items() if v!=before.get('regs',{}).get(k)},i2s={n:[a,b] for n,a,b in zip(hw_names,before.get('hw',[]),after.get('hw',[])) if a!=b})
    r['first_to_last_read_us']=item['last_us']-item['first_us']
    r['pure_state_comparison']=r['valid_full_window'] and item['crossed']==0
    if not r['pure_state_comparison']:
        r['excluded_reason']='crossed_state' if item['crossed'] else 'short_or_failed'
        result[item['name']]=r
        continue
    fig,axes=plt.subplots(4,2,figsize=(12,9));fig.suptitle(item['name']+' crossed='+str(item['crossed']))
    for slot in range(4):
        with wave.open(str(p/item['name']/f'slot{slot}.wav'),'rb') as w:x=np.frombuffer(w.readframes(w.getnframes()),'<i2').astype(float)
        if len(x)!=24000:continue
        axes[slot,0].plot(np.arange(len(x))/24000,x,linewidth=.4);axes[slot,0].set_ylabel('SLOT'+str(slot))
        f,power=welch(x,24000,nperseg=24000,detrend='constant');total=power[f>0].sum()
        axes[slot,1].semilogy(f,power+1e-15);axes[slot,1].set_xlim(0,300)
        for hz in (37,47,53,50,60):axes[slot,1].axvline(hz,color='r',alpha=.25)
        r['slots'][slot]=dict(rms=float(np.sqrt(np.mean(x*x))),peak=float(abs(x).max()),dc=float(x.mean()),low300_percent=float(100*power[(f>0)&(f<300)].sum()/total) if total else 0,
            peak_bands={str(hz):dict(max_psd=float(power[abs(f-hz)<=1].max()),band_rms_counts=float(np.sqrt(power[abs(f-hz)<=1].sum()*(f[1]-f[0]))),power_percent=float(100*power[abs(f-hz)<=1].sum()/total) if total else 0) for hz in (37,47,53,50,60)})
        # 幅值为目标频率±1Hz积分的RMS，单位PCM counts；不是单根FFT峰高。
        r['slots'][slot]['energy_bands']={}
        for label,low,high in [('below100',0,100),('100to300',100,300),('300to3400',300,3400)]:
            mask=(f>=low)&(f<high)&(f>0)
            energy=float(power[mask].sum()*(f[1]-f[0]))
            r['slots'][slot]['energy_bands'][label]=dict(mean_square=energy,percent=float(100*power[mask].sum()/total) if total else 0)
    r['mic2_mic1_rms_ratio']=r['slots'][2]['rms']/r['slots'][0]['rms'] if r['slots'][0]['rms'] else None
    fig.tight_layout();fig.savefig(p/item['name']/'waveform_spectrum.png',dpi=130);plt.close(fig)
    result[item['name']]=r
(p/'comparison.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
for name,r in result.items():print(name,'valid=',r['valid_full_window'],'crossed=',r['crossed'],'RMS=',[round(v['rms'],2) for v in r['slots'].values()])

# 纯状态表便于横向比较；跨状态/短读仅保留元数据，不生成频谱行。
import csv
with (p/'pure_state_metrics.csv').open('w',newline='',encoding='utf-8-sig') as out:
    columns=['stage','slot','rms','dc','mic2_mic1_ratio']+[f'{hz}Hz_band_rms' for hz in (37,47,53,50,60)]+[f'{b}_percent' for b in ('below100','100to300','300to3400')]
    writer=csv.DictWriter(out,fieldnames=columns);writer.writeheader()
    for name,r in result.items():
        if not r['pure_state_comparison']:continue
        for slot in (0,2):
            v=r['slots'][slot]
            row=dict(stage=name,slot=slot,rms=v['rms'],dc=v['dc'],mic2_mic1_ratio=r['mic2_mic1_rms_ratio'])
            row.update({f'{hz}Hz_band_rms':v['peak_bands'][str(hz)]['band_rms_counts'] for hz in (37,47,53,50,60)})
            row.update({f'{b}_percent':v['energy_bands'][b]['percent'] for b in ('below100','100to300','300to3400')})
            writer.writerow(row)
