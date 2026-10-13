from pathlib import Path
exec(Path('dgmp/tools/read_native_environment.py').read_text().split("for name in ('GetWorldTimeOfDay'")[0])
start=0x2e29260; raw=to_raw(start); fn=start+0xc8+struct.unpack_from('<i',data,raw+0xc4)[0]
print(hex(fn)); raw=to_raw(fn);coff(data[raw:raw+700],out/'array-remove-native.obj')
