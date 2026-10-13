from pathlib import Path
import sys
exec((Path(__file__).with_name('inspect.py')).read_text().split('pattern=sys.argv')[0])
data=rd(objs,count*16); class_names={}; o=0
for at in range(0,len(data),16):
 candidate=struct.unpack_from('<Q',data,at)[0]
 if not candidate: continue
 try:
  head=rd(candidate,40); ctype=struct.unpack_from('<Q',head,16)[0]
  if ctype not in class_names: class_names[ctype]=object_name(ctype)
  if class_names[ctype]=='OptionsTopMenuWidget_C' and not (struct.unpack_from('<I',head,8)[0]&0x30): o=candidate; break
 except OSError: pass
if not o: raise SystemExit('Pause menu is not open')
print('menu',hex(o)); cls=q(o+16)
props={n:p for p,n,t in fields(cls) if t.endswith('Property')}
p=q(q(props['MenuProfiles']+0x78)+0x78) if False else 0
# Array inner property -> struct -> Type byte enum.
inner=q(props['MenuProfiles']+0x78); st=q(inner+0x78)
print('inner',object_name(inner),'struct',full(st))
t=next(p for p,n,k in fields(st) if n=='ProfileOption'); e=q(t+0x78)
print('enum',full(e)); ep=q(e+0x40); en=i(e+0x48)
values={}
for n in range(en):
 v=rd(ep+n*16,16); value=v[8]; values[value]=fname(ep+n*16); 
 if value in (33,34,38,40): print(value,values[value])
for key in ('MenuProfiles','ActiveMenuProfile','MenuProfilesMain_savedOff'):
 p=props[key]; a=o+i(p+0x50); ptr=q(a); num=i(a+8)
 print(key,num)
 for n in range(num): print(n,rd(ptr+n*64+1,1)[0],values.get(rd(ptr+n*64+1,1)[0]),'disabled',rd(ptr+n*64+0x38,1)[0])
cat=q(o+i(props['OptionsCategoryWidget']+0x50)); print('category',hex(cat),object_name(q(cat+16)))
for p,n,t in fields(q(cat+16)):
 if t=='Function' and n in ('SetCategory','ForceSelection','SetCategoryActivated','SetCategoryEnabled','SelectNextCategory','SelectPreviousCategory'): print(n,params(p))
 if t.endswith('Property') and n in ('CategoryIndex','MaxCategoryIndex','CategoryWidgets'):
  a=cat+i(p+0x50); print(n,rd(a,min(i(p+0x34),16)).hex())
  if n=='CategoryWidgets':
   for ix in range(i(a+8)):
    wgt=q(q(a)+ix*8); print(' widget',ix,hex(wgt),object_name(q(wgt+16)))
k.CloseHandle(handle)
