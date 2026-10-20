"""carve_check.py — 复核 carve.py 的 offset/size 抽取质量

carve.py 只校验了 RIFF/WAVE/fmt(0x42)/codec(0xFFFF) 四件事，没有校验**内部 chunk 链**，
于是把「头部明文、其后密文」的 Wwise AES 加密 wem 也当成正常文件抽了出来 —— 这批文件
vgmstream 会报 no 'data' tag found。

本脚本重新扫一遍 pak，对每个 Wwise 头做完整 chunk 链校验，分类统计：
    ok        能找到 data 块，长度自洽
    nocipher  帧头之后即密文（加密 wem）
    torn      chunk 链越界/断裂
同时报告哪个 offset 在 samples/wem 里缺失。

用法: python tools/carve_check.py [pak路径]
"""
import os, sys, struct, json

PAK = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
    'PUBG_PAK',
    r'E:/Steam/steamapps/common/PUBG/TslGame/Content/Paks/pakchunk0-WindowsNoEditor_sound.pak')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'samples', 'wem')

if not os.path.exists(PAK):
    sys.exit('找不到 pak：%s' % PAK)

f = open(PAK, 'rb', buffering=0)
f.seek(0, 2); SIZE = f.tell(); f.seek(0)

CH = 32 << 20
OVER = 4 << 20
found = []
pos = 0
while pos < SIZE:
    f.seek(pos)
    buf = f.read(CH)
    if not buf: break
    base = pos
    i = 0
    while True:
        i = buf.find(b'RIFF', i)
        if i < 0: break
        if i + 40 <= len(buf) and buf[i+8:i+12] == b'WAVE':
            sz = struct.unpack('<I', buf[i+4:i+8])[0]
            fmtsz = struct.unpack('<I', buf[i+16:i+20])[0]
            codec = struct.unpack('<H', buf[i+20:i+22])[0]
            if fmtsz == 0x42 and codec == 0xFFFF and 1000 < sz < 40_000_000:
                found.append((base + i, sz))
        i += 4
    nxt = base + CH - OVER
    if nxt <= base: break
    pos = nxt

# 32 MB 窗口有 4 MB 重叠 -> 边界附近的头会被扫到两次，必须按绝对 offset 去重
found = sorted(set(found))
print('pak %s  %.2f GB' % (os.path.basename(PAK), SIZE / 1e9))
print('去重后 Wwise 头 %d 个' % len(found))

ok, nocipher, torn = [], [], []
for off, sz in found:
    total = sz + 8
    f.seek(off)
    b = f.read(total)
    if len(b) != total:
        torn.append((off, sz, 'pak 末尾截断'))
        continue
    p = 12
    have_data = False
    while p + 8 <= total:
        cid = b[p:p+4]; csz = struct.unpack('<I', b[p+4:p+8])[0]
        if csz > total - p - 8:
            break
        if cid == b'data':
            have_data = True
            break
        p += 8 + csz + (csz & 1)
    (ok if have_data else nocipher).append((off, sz))

print('  ok（含 data 块）      %5d' % len(ok))
print('  nocipher（帧头后密文）%5d' % len(nocipher))
print('  torn（链断裂/越界）   %5d' % len(torn))

exist = set(os.listdir(OUT))
missing = [(o, s) for o, s in ok if ('w%010d.wem' % o) not in exist]
print('\nsamples/wem 已存在 %d 个；ok 但缺失 %d 个' % (len(exist), len(missing)))
for o, s in missing[:10]:
    print('   缺 w%010d.wem  (%d B)' % (o, s + 8))

if len(sys.argv) > 2 and sys.argv[2] == '--fix':
    for o, s in missing:
        f.seek(o)
        data = f.read(s + 8)
        with open(os.path.join(OUT, 'w%010d.wem' % o), 'wb') as w:
            w.write(data)
    print('已补回 %d 个' % len(missing))
    with open(os.path.join(ROOT, 'samples', 'encrypted.txt'), 'w') as w:
        for o, s in nocipher:
            w.write('w%010d.wem\n' % o)
    print('加密清单写入 samples/encrypted.txt（%d 条）' % len(nocipher))
