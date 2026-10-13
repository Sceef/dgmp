"""Read-only reflection diagnostics for the running Days Gone process."""
import ctypes as c
from ctypes import wintypes as w
import struct, sys, json

k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [w.DWORD,w.BOOL,w.DWORD]; k.OpenProcess.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]; k.ReadProcessMemory.restype=w.BOOL
k.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD]; k.CreateToolhelp32Snapshot.restype=w.HANDLE
class Module(c.Structure):
    _fields_=[('size',w.DWORD),('id',w.DWORD),('pid',w.DWORD),('globalusage',w.DWORD),('usage',w.DWORD),('base',c.c_void_p),('basesize',w.DWORD),('handle',w.HMODULE),('name',w.WCHAR*256),('path',w.WCHAR*260)]
k.Module32FirstW.argtypes=[w.HANDLE,c.POINTER(Module)]; k.Module32NextW.argtypes=[w.HANDLE,c.POINTER(Module)]
pid=int(sys.argv[1]); snapshot=k.CreateToolhelp32Snapshot(8|16,pid); mod=Module(); mod.size=c.sizeof(mod)
base=0; ok=k.Module32FirstW(snapshot,c.byref(mod))
while ok:
    if mod.name.lower()=='daysgone.exe': base=mod.base
    if mod.name.lower() in ('daysgone.exe','version.dll'): print('module',mod.name,mod.path)
    ok=k.Module32NextW(snapshot,c.byref(mod))
k.CloseHandle.argtypes=[w.HANDLE]; k.CloseHandle(snapshot)
handle=k.OpenProcess(0x410,False,pid)
def rd(address,size):
    if not address: return b'\0'*size
    buf=c.create_string_buffer(size); got=c.c_size_t()
    if not k.ReadProcessMemory(handle,address,buf,size,c.byref(got)): raise OSError(f'read {address:x} ({size})')
    return buf.raw[:got.value]
def q(a): return struct.unpack('<Q',rd(a,8))[0]
def i(a): return struct.unpack('<i',rd(a,4))[0]
names=q(base+0x4c0dc48); objs=q(base+0x4c12290+0x10); count=i(base+0x4c12290+0x1c)
cache={}
def name(index):
    if index in cache: return cache[index]
    entry=q(q(names+(index>>14)*8)+(index&0x3fff)*8)
    raw=rd(entry+0x12,256); value=raw.decode('utf-16le','ignore').split('\0')[0] if i(entry)&1 else raw.split(b'\0')[0].decode('latin1')
    cache[index]=value; return value
def fname(a):
    idx,num=struct.unpack('<ii',rd(a,8)); value=name(idx); return value if not num else f'{value}_{num-1}'
object_cache={}
def object_name(a):
    if not a: return 'None'
    if a not in object_cache: object_cache[a]=fname(a+0x18)
    return object_cache[a]
def full(a):
    parts=[]
    for _ in range(15):
        if not a: break
        parts.append(object_name(a)); a=q(a+0x20)
    return '.'.join(reversed(parts))
def fields(cls):
    while cls:
        child=q(cls+0x38)
        while child:
            yield child,object_name(child),object_name(q(child+0x10))
            child=q(child+0x28)
        cls=q(cls+0x30)
def params(fn):
    result=[]; p=q(fn+0x38)
    while p:
        t=object_name(q(p+0x10))
        if t.endswith('Property'): result.append((object_name(p),t,i(p+0x50),i(p+0x34)))
        p=q(p+0x28)
    return result
pattern=sys.argv[2].lower() if len(sys.argv)>2 else 'menu'
data=rd(objs,count*16)
rows=[struct.unpack_from('<Qii',data,offset) for offset in range(0,len(data),16)]
if '--address' in sys.argv:
    wanted=int(sys.argv[sys.argv.index('--address')+1],16)
    rows=[row for row in rows if row[0]==wanted]
for obj,flags,serial in rows:
    if not obj or flags & ((1<<28)|(1<<29)): continue
    try:
        n=object_name(obj); cls=q(obj+0x10); cn=object_name(cls)
        if cn in ('Class','BlueprintGeneratedClass','Function','DelegateFunction','ScriptStruct','Enum','Package') or cn.endswith('Property') or i(obj+8)&0x10: continue
        matches=pattern in (n+' '+cn).lower()
        if '--isa' in sys.argv:
            expected=sys.argv[sys.argv.index('--isa')+1]; parent=cls
            while parent:
                if object_name(parent)==expected: matches=True; break
                parent=q(parent+0x30)
        if not matches: continue
        print(f'{obj:x} [{cn}] {full(obj)} flags={i(obj+8):x}')
        if '--functions' in sys.argv:
            for p,pn,pt in fields(cls):
                if pt=='Function' and ('--all-functions' in sys.argv or any(x in pn.lower() for x in ('load','start','continue','menu','press','boot','intro','openworld','front','movie','playgame'))) and not pn.startswith('ExecuteUbergraph'): print(' fn',pn,hex(p),params(p))
        if '--fields' in sys.argv:
            for p,pn,pt in fields(cls):
                if pt!='Function' and ('--all-fields' in sys.argv or any(x in pn.lower() for x in ('menu','load','start','save','press','boot','intro','game','fog','region','photo','aim','fire'))):
                    off=i(p+0x50)
                    print(' field',pn,pt,hex(off),rd(obj+off,min(i(p+0x34),16)).hex())
        if '--script' in sys.argv:
            target=sys.argv[sys.argv.index('--script')+1]
            table={struct.unpack_from('<Q',data,o)[0] for o in range(0,len(data),16)}
            for p,pn,pt in fields(cls):
                if pt=='Function' and pn==target:
                    ptr=q(p+0x48); length=i(p+0x50); bytecode=rd(ptr,length); print('script',pn,length,bytecode.hex())
                    references=[]
                    for off in range(len(bytecode)-7):
                        addr=struct.unpack_from('<Q',bytecode,off)[0]
                        if addr and addr in table: references.append((off,hex(addr),full(addr)))
                    print('references',json.dumps(references))
        if 'GameMode' in cn:
            for p,pn,pt in fields(cls):
                if pt=='BoolProperty' and any(x in pn.lower() for x in ('menu','load','save','setup')):
                    raw=rd(obj+i(p+0x50)+rd(p+0x79,1)[0],1)[0]; mask=rd(p+0x7b,1)[0]
                    print(' bool',pn,bool(raw&mask))
    except OSError: pass
k.CloseHandle(handle)
