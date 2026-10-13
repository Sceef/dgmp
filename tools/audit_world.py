"""Read-only category, HUD and dialogue diagnostics for DGMP."""
from pathlib import Path
exec(Path(__file__).with_name('inspect.py').read_text().split('pattern=sys.argv')[0])
from collections import Counter
data=rd(objs,count*16)
rows=[struct.unpack_from('<Qii',data,offset) for offset in range(0,len(data),16)]
components=[]
icons=[]
for obj,flags,serial in rows:
    if not obj or flags & ((1<<28)|(1<<29)): continue
    try:
        cn=object_name(q(obj+0x10)); n=object_name(obj)
        if cn=='Enum' and n=='EMiniMapLocation':
            print('ENUM',hex(obj),rd(obj+0x28,80).hex())
            for offset in (0x30,0x40,0x50):
                ptr=q(obj+offset); num=i(obj+offset+8)
                if ptr>0x10000 and 0<num<150:
                    try: print(' enum names',hex(offset),[(fname(ptr+j*16),struct.unpack('<q',rd(ptr+j*16+8,8))[0]) for j in range(num)])
                    except OSError: pass
        if cn in ('Class','BlueprintGeneratedClass','WidgetBlueprintGeneratedClass') or i(obj+8)&0x10: continue
        if cn=='MiniMapLocationComponent': components.append((obj,rd(obj+0xd8,1)[0],full(q(obj+0x20))))
        if cn=='MapWidgetComponent' and not i(obj+8)&0x20:
            wi,ws=struct.unpack('<ii',rd(obj+0x5ec,8)); loc=struct.unpack_from('<Q',data,wi*16)[0] if 0<=wi<count else 0
            comp=q(loc+0x30) if loc else 0
            icons.append((hex(obj),full(obj),hex(loc),hex(comp),rd(comp+0xd8,1)[0] if comp else -1,rd(obj+0xf0,1).hex()))
        if cn=='BP_Map3D_C':
            print('MAPPRIVATE',hex(obj),[(hex(off),hex(q(obj+off))) for off in range(0x348,0x3d0,8)])
        if cn in ('BendOpenWorldVOManager','XpecVOManager','BendHUD'):
            print('OBJECT',hex(obj),cn,n)
            for p,pn,pt in fields(q(obj+0x10)):
                if pt=='Function' and any(x in pn for x in ('Toggle','Skip','Pause','CanPlay','DisableRFO','SetHud','SetGameHud')): print(' fn',pn,params(p))
        if cn=='BendHUD':
            slate=q(obj+0x480); widget=q(slate+0xb0)
            print('HUDWIDGET',hex(widget),object_name(q(widget+0x10)))
            for p,pn,pt in fields(q(widget+0x10)):
                if pt=='ObjectProperty': print(' hud field',pn,hex(i(p+0x50)),hex(q(widget+i(p+0x50))))
    except OSError: pass
print('MINIMAP counts',Counter(t for _,t,_ in components))
print('ICONS',icons)
for category in sorted({t for _,t,_ in components}): print(' category',category,[(hex(o),owner) for o,t,owner in components if t==category][:4])
k.CloseHandle(handle)
