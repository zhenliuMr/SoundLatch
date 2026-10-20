import csv, statistics as st, sys, math
from collections import Counter, defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\Administrator\AppData\Local\Temp\final.csv'
rows = []
with open(path, encoding='utf-8', errors='replace') as fh:
    for r in csv.DictReader(fh):
        rows.append(r)

ok  = [r for r in rows if r['ok'] == '1']
print('总条目 %d    解码成功 %d    失败 %d' % (len(rows), len(ok), len(rows)-len(ok)))

f = lambda r, k: float(r[k])
dur = [f(r,'dur') for r in ok]
print('时长：合计 %.2f h  中位 %.2f s  均值 %.2f s  min %.2f  max %.2f'
      % (sum(dur)/3600, st.median(dur), sum(dur)/len(dur), min(dur), max(dur)))

# ---- 声学形态分桶（与 duck 门控无关，只看素材自身特征）----
def bucket(r):
    f3, tr, tilt = f(r,'f3Max'), f(r,'trSmMax'), f(r,'tiltMax')
    if f3 >= 0.40:      return 'A 明亮宽带（枪声/破片/玻璃/弹壳）'
    if f3 >= 0.20:      return 'A- 偏亮宽带'
    if tr >= 0.50:      return 'B 中频瞬态（脚步/换弹/碰撞）'
    if tilt >= 11:      return 'C 低频独大（环境/载具/风）'
    return 'D 其它（持续/人声/UI/音乐）'

G = defaultdict(list)
for r in ok: G[bucket(r)].append(r)
print('\n=== 形态分布（全量 %d 条）===' % len(ok))
print('%-36s %6s %7s %9s' % ('形态', '条数', '占比', '时长h'))
for k in sorted(G):
    g = G[k]
    print('%-36s %6d %6.1f%% %9.2f' % (k, len(g), 100*len(g)/len(ok), sum(f(x,'dur') for x in g)/3600))

# ---- duck 触发（受峰值门控影响：ceil = -3 dBFS）----
print('\n=== duck 触发强度（ceil=-3 dBFS，只统计真正顶到天花板的块）===')
dk = [f(r,'duckMax') for r in ok]
for lo, hi in ((0,0.05),(0.05,0.2),(0.2,0.5),(0.5,0.8),(0.8,1.01)):
    n = sum(1 for x in dk if lo <= x < hi)
    print('  [%.2f,%.2f)  %6d  %5.1f%%' % (lo,hi,n,100*n/len(ok)))
trig = [r for r in ok if f(r,'duckMax') >= 0.2]
print('  有实际动作 %d 条 (%.1f%%)，合计 %.1f h'
      % (len(trig), 100*len(trig)/len(ok), sum(f(r,'dur') for r in trig)/3600))
trig2 = [r for r in ok if f(r,'duckMax') >= 0.5]
print('  接近满档 %d 条 (%.1f%%)，合计 %.1f h'
      % (len(trig2), 100*len(trig2)/len(ok), sum(f(r,'dur') for r in trig2)/3600))

# ---- 峰值分布 ----
print('\n=== 素材峰值分布（母带电平）===')
for thr in (1.0,0.891,0.708,0.501):
    n = sum(1 for r in ok if f(r,'peakIn') >= thr)
    print('  peakIn >= %.3f (%6.1f dBFS) : %5.1f%%' % (thr, 20*math.log10(thr), 100*n/len(ok)))

print('\n处理后削顶/非有限: %d / %d'
      % (sum(1 for r in ok if int(r['clip'])>0), sum(1 for r in ok if r['nan']=='1')))

# ---- 旧抽样 632 对比 ----
try:
    sub = set(x.strip().split('/')[-1] for x in open(r'C:\Users\Administrator\AppData\Local\Temp\f632.txt', encoding='utf-8') if x.strip())
    r632 = [r for r in ok if r['name'].split('/')[-1] in sub]
    print('\n=== features.json 那 632 条抽样 vs 全量 ===')
    G2 = Counter(bucket(r) for r in r632)
    for k in sorted(G):
        print('  %-36s 全量 %5.1f%%   632 子集 %5.1f%%'
              % (k, 100*len(G[k])/len(ok), 100*G2.get(k,0)/max(1,len(r632))))
    print('  有 duck 动作: 全量 %.1f%%   632 子集 %.1f%%'
          % (100*len(trig)/len(ok), 100*sum(1 for r in r632 if f(r,'duckMax')>=0.2)/len(r632)))
except FileNotFoundError:
    pass
