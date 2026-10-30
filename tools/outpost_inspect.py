from pathlib import Path
import os,time,sys
root=Path(__file__).resolve().parents[1]
dev=root/'dev'
seq=max(int((dev/'seq').read_text().strip()),int((dev/'cmd.txt').read_text().splitlines()[0]))+1
(dev/'seq').write_text(str(seq))
pending=dev/'cmd.inspect.tmp'
pending.write_text(str(seq)+'\n'+' '.join(sys.argv[1:])+'\n',encoding='utf-8')
os.replace(pending,dev/'cmd.txt')
for _ in range(60):
    try:
        text=(dev/'out.txt').read_text(encoding='utf-8')
        if text.startswith(str(seq)+'\n'):
            print(text); break
    except OSError: pass
    time.sleep(.1)
else: raise SystemExit('DGMP dev command timed out')
