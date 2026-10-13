"""Read-only focused reflection probe. No game calls or writes."""
from pathlib import Path
import sys, re

source = (Path(__file__).with_name('inspect.py')).read_text(encoding='utf-8')
exec(source.split('pattern=sys.argv')[0])
wanted = re.compile(sys.argv[2], re.I)
member = re.compile(sys.argv[3] if len(sys.argv)>3 else '.', re.I)
data=rd(objs,count*16)
classes={}
try:
    for offset in range(0,len(data),16):
        obj=struct.unpack_from('<Q',data,offset)[0]
        if not obj: continue
        try:
            head=rd(obj,40)
            if struct.unpack_from('<I',head,8)[0]&0x30: continue
            cls=struct.unpack_from('<Q',head,16)[0]
            if cls not in classes: classes[cls]=object_name(cls)
            cn=classes[cls]
            if not wanted.search(cn) or cn.endswith('Class') or cn=='Blueprint': continue
            path=full(obj)
            if '.WidgetTree.' in path and path.startswith('/Game/'): continue
            print(hex(obj),cn,path)
            for prop,pn,pt in fields(cls):
                if not member.search(pn): continue
                if pt=='Function': print(' fn',pn,params(prop))
                elif pt.endswith('Property'):
                    off=i(prop+0x50); size=i(prop+0x34); value=rd(obj+off,min(size,16))
                    extra=''
                    if pt in ('ObjectProperty','ClassProperty'): extra=full(q(obj+off))
                    elif pt=='FloatProperty': extra=str(struct.unpack('<f',value)[0])
                    elif pt=='BoolProperty': extra=str(bool(rd(obj+off+rd(prop+0x79,1)[0],1)[0]&rd(prop+0x7b,1)[0]))
                    print(' field',pn,pt,hex(off),value.hex(),extra)
        except OSError: pass
finally: k.CloseHandle(handle)
