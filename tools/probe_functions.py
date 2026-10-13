from pathlib import Path
import sys,re
exec((Path(__file__).with_name('inspect.py')).read_text().split('pattern=sys.argv')[0])
pat=re.compile(sys.argv[2],re.I)
data=rd(objs,count*16)
for off in range(0,len(data),16):
 o=struct.unpack_from('<Q',data,off)[0]
 if not o: continue
 try:
  if pat.search(object_name(o)):
   typ=object_name(q(o+16))
   if typ=='Function': print(hex(o),full(o),params(o))
 except OSError: pass
