"""Static executable research; never opens or changes a running game."""
from pathlib import Path
import struct

root=Path(__file__).resolve().parents[2]
data=(root/'BendGame/Binaries/Win64/DaysGone.exe').read_bytes()
nt=struct.unpack_from('<I',data,0x3c)[0]
count=struct.unpack_from('<H',data,nt+6)[0]
optional=struct.unpack_from('<H',data,nt+20)[0]
base=struct.unpack_from('<Q',data,nt+24+24)[0]
sections=[]
for i in range(count):
    off=nt+24+optional+i*40
    size,rva,rawsize,raw=struct.unpack_from('<IIII',data,off+8)
    sections.append((rva,max(size,rawsize),raw))
def to_raw(rva):
    for start,size,raw in sections:
        if start<=rva<start+size: return raw+rva-start
    raise ValueError(hex(rva))
def to_rva(raw):
    for rva,size,start in sections:
        if start<=raw<start+size: return rva+raw-start
    raise ValueError(hex(raw))
def coff(code,path):
    header=struct.pack('<HHIIIHH',0x8664,1,0,0,0,0,0)
    section=struct.pack('<8sIIIIIIHHI',b'.text',0,0,len(code),60,0,0,0,0,0x60000020)
    path.write_bytes(header+section+code)
out=root/'dgmp/logs'; out.mkdir(exist_ok=True)
stamp=struct.unpack_from('<I',data,nt+8)[0]
if stamp!=1752517958: raise SystemExit('Unsupported executable')
for name in ('GetWorldTimeOfDay','SetWorldTimeOfDay','SetWorldTimeRate','GetWorldTimeHour','SelectWorldWeather'):
    pos=data.find(name.encode()+b'\0')
    if pos<0: continue
    target=to_rva(pos); at=0
    # RegisterNativeFunctions loads the ANSI name into RDX and the exec
    # wrapper into R8 with adjacent RIP-relative LEA instructions.
    while True:
        at=data.find(b'\x48\x8d\x15',at)
        if at<0: break
        xref=to_rva(at)
        if xref+7+struct.unpack_from('<i',data,at+3)[0]==target and data[at-7:at-4]==b'\x4c\x8d\x05':
            function=xref+struct.unpack_from('<i',data,at-4)[0]
            raw=to_raw(function)
            print(name,'exec RVA',hex(function))
            coff(data[raw:raw+320],out/(name+'-exec.obj')); break
        at+=3

# The supported build's native getter takes the ticks within a day and
# multiplies by ticks-to-hours. Record the actual constant for API units.
getter=0x8e6820; raw=to_raw(getter)
if data[raw:raw+7]!=b'\x48\x8b\x81\xd0\x37\x00\x00': raise SystemExit('Unexpected getter')
constant=getter+0x26+struct.unpack_from('<i',data,raw+0x22)[0]
multiplier=struct.unpack_from('<d',data,to_raw(constant))[0]
print('GetWorldTimeOfDay units: one hour =',36000000000*multiplier)
coff(data[raw:raw+43],out/'GetWorldTimeOfDay-native.obj')
