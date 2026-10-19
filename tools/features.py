import os, json, subprocess, sys, math
import numpy as np
from scipy.io import wavfile
from scipy import signal

# 路径相对脚本自身定位，克隆到任何位置都能跑（原先硬编码 D:/project/PUBG）
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEM = os.path.join(ROOT, 'samples', 'wem')
CLI = os.path.join(ROOT, 'tools', 'vgm', 'vgmstream-cli.exe')
TMP = os.path.join(ROOT, 'samples', 'tmp.wav')
OUTCSV = os.path.join(ROOT, 'samples', 'features.json')

files = sorted(os.listdir(WEM))
step = max(1, len(files) // 700)
sel = files[::step][:700]

BANDS = [(20,63),(63,125),(125,250),(250,500),(500,1000),(1000,2000),
         (2000,4000),(4000,8000),(8000,16000),(16000,22000)]
labels = ['<63','63-125','125-250','250-500','500-1k','1-2k','2-4k','4-8k','8-16k','16k+']

rows = []
for n, fn in enumerate(sel):
    p = os.path.join(WEM, fn)
    try:
        subprocess.run([CLI, '-o', TMP, p], capture_output=True, timeout=60)
        if not os.path.exists(TMP): continue
        sr, x = wavfile.read(TMP)
    except Exception as e:
        continue
    finally:
        try: os.remove(TMP)
        except: pass
    if x.ndim > 1: x = x.mean(axis=1)
    if x.dtype == np.int16: x = x.astype(np.float32)/32768.0
    else: x = x.astype(np.float32)
    if x.size < 2048: continue
    dur = x.size / sr
    peak = float(np.max(np.abs(x))) + 1e-12
    rms = float(np.sqrt(np.mean(x**2))) + 1e-12
    # overall long-term spectrum (welch) up to 120s
    seg = x[:int(min(len(x), sr*120))]
    fq, P = signal.welch(seg, sr, nperseg=4096)
    P = P + 1e-16
    tot = P.sum()
    be = []
    for lo, hi in BANDS:
        m = (fq >= lo) & (fq < hi)
        be.append(float(P[m].sum()/tot) if m.any() else 0.0)
    centroid = float((fq*P).sum()/tot)
    # short-term (10ms) rms envelope -> transientness
    hop = int(sr*0.01)
    fr = x[: (len(x)//hop)*hop].reshape(-1, hop)
    env = np.sqrt((fr**2).mean(axis=1)) + 1e-9
    edb = 20*np.log10(env)
    dyn = float(np.percentile(edb, 95) - np.percentile(edb, 30))
    # onset rate: positive jumps > 6 dB per 10ms
    d = np.diff(edb)
    onset = float((d > 6).sum() / max(dur, 0.01))
    # spectral flux / stationarity: std of band energies over time
    f2, t2, S = signal.spectrogram(seg, sr, nperseg=1024, noverlap=512)
    S = S + 1e-16
    SdB = 10*np.log10(S)
    flux = float(np.mean(np.abs(np.diff(SdB, axis=1))))
    midE = sum(be[3:7]); lowE = sum(be[0:3]); highE = sum(be[7:])
    rows.append(dict(f=fn, sr=int(sr), dur=round(dur,3), peak=round(float(20*math.log10(peak)),1),
                     rms=round(float(20*math.log10(rms)),1), centroid=round(centroid,1),
                     dyn=round(dyn,1), onset=round(onset,3), flux=round(flux,2),
                     low=round(lowE,3), mid=round(midE,3), high=round(highE,3),
                     bands=[round(b,4) for b in be]))
    if n % 50 == 0: print(n, fn, round(dur,2), flush=True)

json.dump(rows, open(OUTCSV, 'w'), indent=0)
print('done', len(rows))
