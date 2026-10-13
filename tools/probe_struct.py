from pathlib import Path
import sys
exec((Path(__file__).with_name('inspect.py')).read_text().split('pattern=sys.argv')[0])
f=int(sys.argv[2],16); p=q(f+0x38); s=q(p+0x78)
print('struct',hex(s),full(s))
for p,n,t in fields(s): print(n,t,hex(i(p+0x50)),i(p+0x34))

