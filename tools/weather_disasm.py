from pathlib import Path
exec(Path('dgmp/tools/read_native_environment.py').read_text().split("for name in ('GetWorldTimeOfDay'")[0])
for name in ('SlowTick','PerFrameCameraUpdate'):
 pos=data.find(name.encode()+b'\0'); target=to_rva(pos); at=0
 while True:
  at=data.find(b'\x48\x8d\x15',at)
  if at<0: break
  xref=to_rva(at)
  if xref+7+struct.unpack_from('<i',data,at+3)[0]==target and data[at-7:at-4]==b'\x4c\x8d\x05':
   fn=xref+struct.unpack_from('<i',data,at-4)[0]; print(name,hex(fn)); raw=to_raw(fn);coff(data[raw:raw+512],out/(name+'-exec.obj'))
  at+=3
