from pathlib import Path
import sys
exec((Path(__file__).with_name('inspect.py')).read_text().split('pattern=sys.argv')[0])
o=int(sys.argv[2],16)
while o:
 print(hex(o),object_name(o),hex(i(o+8)),object_name(q(o+16)))
 o=q(o+32)
