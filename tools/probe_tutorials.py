from pathlib import Path
import sys
exec((Path(__file__).with_name('inspect.py')).read_text().split('pattern=sys.argv')[0])
data=rd(objs,count*16); classes={}; total=marked=0
for off in range(0,len(data),16):
 o=struct.unpack_from('<Q',data,off)[0]
 if not o: continue
 try:
  h=rd(o,40); cls=struct.unpack_from('<Q',h,16)[0]
  if cls not in classes: classes[cls]=object_name(cls)
  if classes[cls] not in ('BendTutorialSimpleData','BendTutorialMultiData') or struct.unpack_from('<I',h,8)[0]&0x30: continue
  total+=1
  p=next(p for p,n,t in fields(cls) if n=='bTutorialDisplayed')
  marked+=bool(rd(o+i(p+0x50)+rd(p+0x79,1)[0],1)[0]&rd(p+0x7b,1)[0])
 except (OSError,StopIteration): pass
print('Tutorial data:',total,'marked displayed:',marked)
k.CloseHandle(handle)
