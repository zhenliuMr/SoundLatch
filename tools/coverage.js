// 覆盖度分析：86 条探针样本 vs 全库 12022 条 wem
// 目的：回答「样本是否覆盖了游戏主要战斗音效」
//
// 现实约束：pak 索引 AES 加密 → 素材名是哈希（w0000612405.wem），无法按名字判断
// 是枪声/脚步/载具。只能用可测的代理量：时长、峰值、质心、动态、10 段倍频程能量。
//
// 输出：
//   1) 全库抽样 632 条的形态分布（这 632 本身是 files[::17] 均匀抽样）
//   2) 探针样本 86 条在时长/质心/动态上的分布 vs 632 条全体
//   3) 缺失区间：哪些区间探针没覆盖
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');
const feats = JSON.parse(fs.readFileSync(path.join(ROOT, 'samples/features.json'), 'utf8'));

// 复现 pick_samples.js 的三维分层筛选
const C = [[0,250],[250,800],[800,2000],[2000,5000],[5000,24000]];
const D = [[0,10],[10,20],[20,50],[50,1e9]];
const T = [[0,1.5],[1.5,6],[6,20],[20,1e9]];
function pick(list) {
  const cells = new Map();
  for (const s of list) {
    if (!(s.peak > -40)) continue;
    if (!fs.existsSync(path.join(ROOT, 'samples/wem', s.f))) continue;
    const ci = C.findIndex(b => s.centroid >= b[0] && s.centroid < b[1]);
    const di = D.findIndex(b => s.dyn >= b[0] && s.dyn < b[1]);
    const ti = T.findIndex(b => s.dur >= b[0] && s.dur < b[1]);
    if (ci < 0 || di < 0 || ti < 0) continue;
    const k = `${ci}/${di}/${ti}`;
    if (!cells.has(k)) cells.set(k, []);
    cells.get(k).push(s);
  }
  const seen = new Set(); const out = [];
  for (const [k, arr] of cells) {
    const n = arr.length <= 8 ? arr.length : 2;
    for (let i = 0; i < n; i++) {
      const s = arr[Math.floor(i * (arr.length - 1) / Math.max(1, n - 1))];
      if (seen.has(s.f)) continue;
      seen.add(s.f); out.push(s);
    }
  }
  return out;
}

function stat(v) {
  if (!v.length) return { n: 0 };
  const s = [...v].sort((a, b) => a - b);
  const q = p => s[Math.floor(p * (s.length - 1))];
  return { n: s.length, p10: q(.1), p50: q(.5), p90: q(.9), max: s[s.length - 1] };
}
function row(label, o) {
  if (!o.n) return `  ${label.padEnd(18)} (无)`;
  return `  ${label.padEnd(18)} n=${String(o.n).padStart(4)}  p10=${o.p10.toFixed(2).padStart(8)}  p50=${o.p50.toFixed(2).padStart(8)}  p90=${o.p90.toFixed(2).padStart(8)}  max=${o.max.toFixed(2)}`;
}

const probe = pick(feats);
const probeSet = new Set(probe.map(s => s.f));

console.log('=== 素材库实况 ===');
const wemAll = fs.readdirSync(path.join(ROOT, 'samples/wem'));
console.log(`  samples/wem 总数: ${wemAll.length}`);
console.log(`  做过频谱特征的:    ${feats.length}  (覆盖率 ${(feats.length / wemAll.length * 100).toFixed(1)}%)`);
console.log(`  探针样本:          ${probe.length}`);

console.log('\n=== 时长分布 (s) ===');
console.log(row('全库抽样 632', stat(feats.map(s => s.dur))));
console.log(row('探针样本', stat(probe.map(s => s.dur))));
console.log('\n  被 dur>12 过滤掉的素材（未进特征库筛选范围）:');
const dropped = feats.filter(s => s.dur > 12);
console.log(`    ${dropped.length} 条，其中 >30s: ${dropped.filter(s=>s.dur>30).length} 条，>60s: ${dropped.filter(s=>s.dur>60).length} 条`);
const droppedPeak = stat(dropped.map(s => s.peak));
console.log(`    这些长素材峰值 p50=${droppedPeak.p50?.toFixed(1)} dBFS，长时段落/循环/载具音效的主要载体`);

console.log('\n=== 频谱质心 (Hz) ===');
console.log(row('全库抽样 632', stat(feats.map(s => s.centroid))));
console.log(row('探针样本', stat(probe.map(s => s.centroid))));

console.log('\n=== 动态 dyn (dB, p95-p30) ===');
console.log(row('全库抽样 632', stat(feats.map(s => s.dyn))));
console.log(row('探针样本', stat(probe.map(s => s.dyn))));

console.log('\n=== 覆盖缺口检查 ===');
// 三维网格（质心 × 动态 × 时长）覆盖
const cBins = C, dBins = D, tBins = T;
const cName = ['<250','250-800','800-2k','2k-5k','>5k'];
const dName = ['d0-10','d10-20','d20-50','d50+'];
const tName = ['t0-1.5','t1.5-6','t6-20','t20+'];
const allGrid = {}, probeGrid = {};
for (const s of feats) {
  const ci = cBins.findIndex(b => s.centroid >= b[0] && s.centroid < b[1]);
  const di = dBins.findIndex(b => s.dyn >= b[0] && s.dyn < b[1]);
  const ti = tBins.findIndex(b => s.dur >= b[0] && s.dur < b[1]);
  if (ci < 0 || di < 0 || ti < 0) continue;
  const k = `${ci}_${di}_${ti}`;
  allGrid[k] = (allGrid[k] || 0) + 1;
  if (probeSet.has(s.f)) probeGrid[k] = (probeGrid[k] || 0) + 1;
}
console.log('  质心 × 动态 × 时长 网格（all=全库抽样数, probe=探针数）');
let gaps = 0, cells = 0;
for (let ci = 0; ci < cBins.length; ci++)
  for (let di = 0; di < dBins.length; di++)
    for (let ti = 0; ti < tBins.length; ti++) {
      const k = `${ci}_${di}_${ti}`;
      const a = allGrid[k] || 0, p = probeGrid[k] || 0;
      if (a === 0) continue;
      cells++;
      const flag = p === 0 ? '  ← 未覆盖' : '';
      if (p === 0) gaps++;
      console.log(`    ${cName[ci].padStart(9)} ${dName[di].padStart(6)} ${tName[ti].padStart(8)}  all=${String(a).padStart(3)}  probe=${String(p).padStart(3)}${flag}`);
    }
console.log(`\n  ${cells} 个非空格中，${gaps} 个未覆盖`);