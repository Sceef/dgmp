from pathlib import Path
exec(Path('dgmp/tools/read_native_environment.py').read_text().split("for name in ('GetWorldTimeOfDay'")[0])
name='Array_Remove'; target=to_rva(data.index(name.encode()+b'\0')); at=0
while True:
 at=data.find(b'\x48\x8d\x15',at)
 if at<0: break
 xref=to_rva(at)
 if xref+7+struct.unpack_from('<i',data,at+3)[0]==target and data[at-7:at-4]==b'\x4c\x8d\x05':
  fn=xref+struct.unpack_from('<i',data,at-4)[0]; print('exec',hex(fn)); raw=to_raw(fn);coff(data[raw:raw+600],out/'array-remove-exec.obj')
 at+=3
