import os, struct, sys, json

PAK = r'E:/Steam/steamapps/common/PUBG/TslGame/Content/Paks/pakchunk0-WindowsNoEditor_sound.pak'
OUT = r'D:/project/PUBG/samples/wem'
os.makedirs(OUT, exist_ok=True)

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

json.dump(carved, open(r'D:/project/PUBG/samples/carved.json', 'w'), indent=0)
print('carved', count)
