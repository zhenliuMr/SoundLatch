import os, struct, sys, json

# 游戏安装路径因人而异，可用环境变量 PUBG_PAK 覆盖；
# 默认试常见的 Steam 路径。输出目录相对脚本自身定位（可用 PUBG_OUT 覆盖，便于试跑）。
PAK = os.environ.get('PUBG_PAK', r'E:/Steam/steamapps/common/PUBG/TslGame/Content/Paks/pakchunk0-WindowsNoEditor_sound.pak')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get('PUBG_OUT', os.path.join(ROOT, 'samples', 'wem'))
os.makedirs(OUT, exist_ok=True)

if not os.path.exists(PAK):
    sys.exit('找不到 pak 文件：%s\n请设置环境变量 PUBG_PAK 指向 pakchunk0-WindowsNoEditor_sound.pak' % PAK)

f = open(PAK, 'rb', buffering=0)
f.seek(0, 2); SIZE = f.tell(); f.seek(0)

CH = 32 << 20
OVER = 4 << 20
count = 0
skipped_cipher = 0
seen = set()
buf = b''
pos = 0
carved = []
encrypted = []


def has_data_chunk(blob, total):
    """校验内部 chunk 链能走到 data 块。

    Wwise 的 AES 加密 wem 只保留 RIFF/fmt/采样率明文，之后的字节是密文，
    于是 'data' 块根本读不到（vgmstream 会报 no 'data' tag found）。
    只按 RIFF/WAVE/fmt(0x42)/codec(0xFFFF) 四个字段判断是分不出来的。
    """
    p = 12
    while p + 8 <= total:
        cid = blob[p:p+4]
        csz = struct.unpack('<I', blob[p+4:p+8])[0]
        if csz > total - p - 8:
            return False
        if cid == b'data':
            return True
        p += 8 + csz + (csz & 1)
    return False


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
                # 32 MB 窗口有 4 MB 重叠，边界附近的头会被扫到两次；按绝对 offset 去重
                if start in seen:
                    i += 4
                    continue
                if start + total > SIZE:
                    # pak 末尾：能读多少读多少，头部声明可能超出 pak（第 12 023 条就是这样丢的）
                    total = SIZE - start
                f.seek(start)
                data = f.read(total)
                if len(data) == total and has_data_chunk(data, total):
                    seen.add(start)
                    name = f'w{start:010d}.wem'
                    with open(os.path.join(OUT, name), 'wb') as o:
                        o.write(data)
                    carved.append({'name': name, 'offset': start, 'size': total})
                    count += 1
                else:
                    seen.add(start)
                    encrypted.append(f'w{start:010d}.wem')
                    skipped_cipher += 1
        i += 4
    pos = base + CH - OVER
    if pos <= base: break

json.dump(carved, open(os.path.join(ROOT, 'samples', 'carved.json'), 'w'), indent=0)
with open(os.path.join(ROOT, 'samples', 'encrypted.txt'), 'w') as w:
    w.write('\n'.join(encrypted))
print('carved %d   跳过(加密/链表不可读) %d' % (count, skipped_cipher))
