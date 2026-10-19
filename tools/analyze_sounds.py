import json, math, statistics as st
import numpy as np

D = json.load(open('samples/features.json', encoding='utf-8'))
bad = len(D)
D = [d for d in D if -70 < d['peak'] <= 6 and -80 < d['rms'] <= 0]
print('样本 %d 条，剔除解码异常 %d 条，有效 %d 条' % (bad, bad-len(D), len(D)))

def cls(f):
    c, dur = f['centroid'], f['dur']
    if c < 600 and dur >= 1.5:  return 'A 持续低频环境（车/风/飞机/流水）'
    if c < 400 and dur < 1.5:   return 'B 极低频瞬态（重物/车门/远爆）'
    if c >= 3000 and dur < 2.0: return 'C 明亮瞬态（枪声/破片/玻璃）'
    if c >= 3000:               return 'D 明亮持续（音乐/混音）'
    if dur < 1.5:               return 'E 中频瞬态（脚步/换弹/碰撞）'
    return 'F 中频持续（人声/UI/音乐）'

G = {}
for f in D: G.setdefault(cls(f), []).append(f)

def to4(b):
    B1 = b[0] + b[1] + 0.2 * b[2]
    B2 = 0.8 * b[2] + b[3] + 0.5 * b[4]
    B3 = 0.5 * b[4] + b[5] + 0.5 * b[6]
    B4 = 0.5 * b[6] + b[7] + b[8] + b[9]
    s = B1 + B2 + B3 + B4
    return [x/s if s > 0 else 0 for x in (B1, B2, B3, B4)]

# 当前默认配置
envAmt, envThr, stepAmt = 0.55, -42.0, 0.65
cutBase, cutMax = -18.0*envAmt, -12.0*envAmt
boostDb = [0.0, stepAmt*12*0.25, stepAmt*12*1.0, stepAmt*12*0.5]
def clamp(v,a,b): return a if v<a else (b if v>b else v)

print('\n【各类声音的频谱分布】（占该类别总能量的比例）')
print('%-34s %4s %8s %7s %7s %7s %7s %7s' % ('类别','n','RMS','<125Hz','B1 20-150','B2 150-600','B3 0.6-3k','B4 3-16k'))
P, L = {}, {}
for k in sorted(G):
    g = G[k]
    b4 = np.mean([to4(f['bands']) for f in g], axis=0)
    low125 = np.mean([f['bands'][0]+f['bands'][1] for f in g])
    P[k], L[k] = b4, st.median([f['rms'] for f in g])
    print('%-34s %4d %7.1f %6.1f%% %7.1f%% %8.1f%% %7.1f%% %7.1f%%' % (
        k, len(g), L[k], low125*100, b4[0]*100, b4[1]*100, b4[2]*100, b4[3]*100))

def sim(k, transient):
    b4 = P[k]
    bl = [L[k] + 10*math.log10(max(x, 1e-6)) for x in b4]
    extra = clamp((bl[0]-envThr)*0.5, cutMax, 0.0) if bl[0] < envThr else 0.0
    g = [cutBase+extra]
    for i in (1,2,3):
        guard = clamp((6.0-bl[i])/12.0, 0.0, 1.0) if transient else 0.0
        g.append(boostDb[i]*guard)
    lin = sum(b4[i]*10**(g[i]/10) for i in range(4))
    return g, bl, 10*math.log10(max(lin, 1e-9))

print('\n【处理效果】稳态素材（只有 B1 衰减） / 瞬态素材（B1 衰减 + 中高频提升）')
print('%-34s %8s %8s %8s   %s' % ('类别','稳态','瞬态','B1增益','B3增益(瞬态)'))
for k in sorted(G):
    gs, _, os_ = sim(k, False)
    gt, _, ot = sim(k, True)
    print('%-34s %+7.1f %+7.1f %+7.1f dB   %+6.1f dB' % (k, os_, ot, gt[0], gt[2]))

A = 'A 持续低频环境（车/风/飞机/流水）'
E = 'E 中频瞬态（脚步/换弹/碰撞）'
C = 'C 明亮瞬态（枪声/破片/玻璃）'
_, blA, oA = sim(A, False)
_, blE, oE = sim(E, True)
_, blC, oC = sim(C, True)
print('\n【结论指标】')
print('  环境（稳态）：B1 %.1f dBFS，整体 %+.1f dB' % (blA[0], oA))
print('  脚步（瞬态）：B3 %.1f → %.1f dBFS（%+.1f），整体 %+.1f dB' % (blE[2], blE[2]+7.8, 7.8, oE))
print('  脚步 B3 与环境 B1 的对比度： %+.1f dB → %+.1f dB   （改善 %+.1f dB）' % (
      blE[2]-blA[0], (blE[2]+7.8)-(blA[0]+cutBase), (blE[2]+7.8)-(blA[0]+cutBase)-(blE[2]-blA[0])))
print('  枪声：B3 %+.1f dB（更亮），B1 %+.1f dB（轰鸣减弱），整体 %+.1f dB' % (7.8, cutBase, oC))
print('  含 B1 份额高的爆炸类：轰鸣被压 %.1f dB，中高频被抬 %.1f dB → 听感更"干/脆"' % (-cutBase, 7.8))
