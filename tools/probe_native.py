from pathlib import Path
import sys
exec((Path(__file__).with_name('inspect.py')).read_text().split('pattern=sys.argv')[0])
o=int(sys.argv[2],16); v=q(o)
for offset in (0x678,):
 f=q(v+offset); print(hex(f-base)); code=rd(f,3000)
 header=struct.pack('<HHIIIHH',0x8664,1,0,0,0,0,0); section=struct.pack('<8sIIIIIIHHI',b'.text',0,0,len(code),60,0,0,0,0,0x60000020)
 Path('dgmp/logs/precip-slow-native.obj').write_bytes(header+section+code)
