// 从 features.json 按 (质心 × 动态 × 时长) 三维分层挑样本
//
// 为什么要改：旧版按 centroid/dyn 四分法分组 + dur<=12 过滤，存在两个系统性缺口
//   1) dur>12 的 55 条被全丢掉 —— 而长时段落/载具循环正是「环境抑制」要处理的对象
//   2) 600-1500Hz dyn8+ 这一格覆盖率仅 3%，而脚步主体恰好在 250Hz-2kHz
//   → 之前「实测出的最优参数」其实是偏置样本上的最优，不能直接固化。
//
// 无法按名字分类：pak 索引 AES 加密，素材名是哈希（w0000612405.wem）。
// 只能用可测代理量分层，保证频谱形态上覆盖全库。
//
// 输出 TSV: name kind centroid dyn dur
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');
const d = JSON.parse(fs.readFileSync(path.join(ROOT, 'samples/features.json'), 'utf8'));
const WEM = path.join(ROOT, 'samples/wem');

// ---- 三维分层定义 ----
// 质心：脚步主体 250-2k，枪声 2-8k，环境 <250
const C = [[0,250],[250,800],[800,2000],[2000,5000],[5000,24000]];
// 动态：dB，区分稳态持续 vs 瞬态
const D = [[0,10],[10,20],[20,50],[50,1e9]];
// 时长：区分单发音效 vs 长循环
const T = [[0,1.5],[1.5,6],[6,20],[20,1e9]];
// 层名用 ASCII：素材会按层名建目录，中文目录名在 MinGW 的 fopen 下编码不稳
const CLname = ['L1_low','L2_stepband','L3_stepband','L4_gunband','L5_bright'];
const DLname = ['d0_steady','d1_slow','d2_mid','d3_sharp'];
const TLname = ['t0_short','t1_mid','t2_long','t3_loop'];

const cells = new Map();
for (const s of d) {
  // 注意：features.json 里 peak/rms 是 dBFS（负数），不是线性幅度。
  // 旧版误写成 s.peak > 0.15，把 peak=-3.5dBFS 的正常素材全滤掉了 → 只剩极安静的样本。
  if (!(s.peak > -40)) continue;                 // 峰值低于 -40dBFS 的基本听不见
  if (!(s.dur >= 0.05)) continue;               // 短于 20ms 的多半是残片
  if (!fs.existsSync(path.join(WEM, s.f))) continue;
  const ci = C.findIndex(b => s.centroid >= b[0] && s.centroid < b[1]);
  const di = D.findIndex(b => s.dyn >= b[0] && s.dyn < b[1]);
  const ti = T.findIndex(b => s.dur >= b[0] && s.dur < b[1]);
  if (ci < 0 || di < 0 || ti < 0) continue;
  const k = `${ci}/${di}/${ti}`;
  if (!cells.has(k)) cells.set(k, []);
  cells.get(k).push(s);
}

// 每格取样：多数格 2 条（覆盖形态即可）；样本少的格全取（<=8 条的格全要，避免留空）
const ciOf = k => +k.split('/')[0];
const diOf = k => +k.split('/')[1];
const tiOf = k => +k.split('/')[2];
const out = [];
for (const [k, arr] of cells) {
  const n = arr.length <= 8 ? arr.length : 2;
  for (let i = 0; i < n; i++) {
    const s = arr[Math.floor(i * (arr.length - 1) / Math.max(1, n - 1))];
    out.push({ s, kind: `${CLname[ciOf(k)]}_${DLname[diOf(k)]}_${TLname[tiOf(k)]}` });
  }
}
// 每条 wav 只解码一次 → 同名去重
const seen = new Set();
for (const o of out) {
  if (seen.has(o.s.f)) continue;
  seen.add(o.s.f);
  console.log(`${o.s.f}\t${o.kind}\tcentroid=${o.s.centroid.toFixed(0)}\tdyn=${o.s.dyn}\tdur=${o.s.dur.toFixed(2)}`);
}