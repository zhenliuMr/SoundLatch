import os, struct, sys, json

# 游戏安装路径因人而异，可用环境变量 PUBG_PAK 覆盖；
# 默认试常见的 Steam 路径。输出目录相对脚本自身定位。
PAK = os.environ.get('PUBG_PAK', r'E:/Steam/steamapps/common/PUBG/TslGame/Content/Paks/pakchunk0-WindowsNoEditor_sound.pak')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'samples', 'wem')
os.makedirs(OUT, exist_ok=True)

if not os.path.exists(PAK):
    sys.exit('找不到 pak 文件：%s\n请设置环境变量 PUBG_PAK 指向 pakchunk0-WindowsNoEditor_sound.pak' % PAK)

f = open(PAK, 'rb', buffering=0)
f.seek(0, 2); SIZE = f.tell(); f.seek(0)

CH = 32 << 20
OVER = 4 << 20
count = 0
buf = b''
pos = 0
carved = []
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
                total = sz + 8
                start = base + i
                if start + total <= SIZE:
                    f.seek(start)
                    data = f.read(total)
                    if len(data) == total and data[-8:-4] in (b'data',) or True:
                        name = f'w{start:010d}.wem'
                        with open(os.path.join(OUT, name), 'wb') as o:
                            o.write(data)
                        carved.append({'name': name, 'offset': start, 'size': total})
                        count += 1
        i += 4
    pos = base + CH - OVER
    if pos <= base: break

json.dump(carved, open(os.path.join(ROOT, 'samples', 'carved.json'), 'w'), indent=0)
print('carved', count)
