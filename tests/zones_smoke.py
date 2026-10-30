"""Real server tests for polygon zones and backwards compatibility; isolated in build/."""
from pathlib import Path
import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
RUN = Path(tempfile.mkdtemp(prefix="zones-smoke-", dir=ROOT / "build"))
shutil.copy2(ROOT / "dgmp-server.exe", RUN / "dgmp-server.exe")
mode = RUN / "modes" / "zonesqa"
mode.mkdir(parents=True)
points = [{"x": x, "y": y} for x, y in [(0, 0), (10, 0), (10, 4), (4, 4), (4, 10), (0, 10)]]
(mode / "world.json").write_text(json.dumps({"zones": [{"name": "from_disk", "shape": "polygon", "type": "pve",
    "x": 10000, "y": 20000, "z": 300, "yaw": 90, "height": 5, "world": 7, "points": points}]}), encoding="utf-8")
(mode / "main.lua").write_text(r'''
local count=0
local function check(v,label) assert(v,label); count=count+1 end
local function inside(name,x,y,z,world)
    for _,n in ipairs(zonesAt(x,y,z,world or 0)) do if n==name then return true end end
    return false
end
local function probe(name,x,y,z,world,expected,label)
    check(inside(name,x,y,z,world)==expected,label)
end
local disk=assert(getZone("from_disk"))
check(disk.shape=="polygon" and #disk.points==6,"disk polygon and vertices")
probe("from_disk",9500,20200,300,7,true,"rotated vertical arm")
probe("from_disk",9800,20800,300,7,true,"rotated horizontal arm")
probe("from_disk",9200,20800,300,7,false,"concave notch excluded")
probe("from_disk",9500,20400,300,7,true,"concave edge belongs to zone")
probe("from_disk",10000,20000,300,7,true,"vertex belongs to zone")
probe("from_disk",9500,20200,801,7,false,"height excludes above")
probe("from_disk",9500,20200,-201,7,false,"height excludes below")
probe("from_disk",9500,20200,300,0,false,"another world excluded")
createZone({name="roundtrip",shape=disk.shape,x=disk.x,y=disk.y,z=disk.z,yaw=disk.yaw,points=disk.points,world=7})
probe("roundtrip",9500,20200,300,7,true,"API round trip inside")
probe("roundtrip",9200,20800,300,7,false,"API round trip notch")
setZone("roundtrip",{x=100000,y=-300000,yaw=37})
local a=math.rad(37)
local function rotated(x,y) return 100000+100*(x*math.cos(a)-y*math.sin(a)),-300000+100*(x*math.sin(a)+y*math.cos(a)) end
local x,y=rotated(4,8)
probe("roundtrip",x,y,300,7,true,"quantised oblique edge")
x,y=rotated(4.1,8)
probe("roundtrip",x,y,300,7,false,"outside oblique edge")
local p={{x=0,y=0},{x=10,y=0},{x=10,y=4},{x=4,y=4},{x=4,y=10},{x=0,y=10}}
createZone({name="reverse",shape="polygon",type="pvp",x=0,y=0,z=0,points={p[6],p[5],p[4],p[3],p[2],p[1]}})
probe("reverse",200,800,0,0,true,"clockwise winding")
probe("reverse",800,800,0,0,false,"clockwise notch")
check(getZone("reverse").type=="pvp","PvP polygon")
setZone("reverse",{points={{x=-10,y=-10},{x=0,y=-10},{x=0,y=0},{x=-10,y=0}}})
probe("reverse",-500,-500,0,0,true,"negative vertices and dynamic update")
probe("reverse",200,800,0,0,false,"old contour removed")
local invalid={
    {{x=0,y=0},{x=1,y=0}},
    {{x=0,y=0},{x=0,y=0},{x=1,y=1}},
    {{x=0,y=0},{x=1,y=0},{x=2,y=0}},
    {{x=0,y=0},{x=10,y=10},{x=0,y=10},{x=10,y=0}},
    {{x=0,y=0},{x=10,y=0},{x=5,y=0},{x=5,y=5}},
    {{x=0,y=0},{x=0/0,y=0},{x=1,y=1}},
    {{x=0,y=0},{x=math.huge,y=0},{x=1,y=1}},
    {{x=0,y=0},{x="bad",y=0},{x=1,y=1}},
}
for i,v in ipairs(invalid) do
    check(not pcall(setZone,"reverse",{points=v}),"invalid contour rejected "..i)
    probe("reverse",-500,-500,0,0,true,"failed update retains prior zone "..i)
end
local large={}; for i=1,129 do local a=i*math.pi*2/129; large[i]={x=math.cos(a),y=math.sin(a)} end
check(not pcall(setZone,"reverse",{points=large}),"vertex limit")
setZone("reverse",{shape="circle",radius=3})
probe("reverse",200,0,0,0,true,"polygon to circle")
probe("reverse",400,0,0,0,false,"circle bounds")
setZone("reverse",{shape="box",length=4,width=2,yaw=90})
probe("reverse",0,190,0,0,true,"rotated legacy box")
probe("reverse",190,0,0,0,false,"rotated box excludes outside")
createZone({name="implicit",x=0,y=0,z=0,points=p})
check(getZone("implicit").shape=="polygon","points infer shape")
setZone("implicit",{enabled=false})
probe("implicit",200,800,0,0,false,"disabled polygon")
log("POLYGON_QA_PASS "..count)
''', encoding="utf-8")
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
proc = subprocess.Popen([str(RUN / "dgmp-server.exe"), "--mode", "zonesqa", "--bind", "127.0.0.1", "--port", str(port)],
    cwd=RUN, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    text=True, encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
done=threading.Event()
lines=[]
def read():
    for line in proc.stdout:
        lines.append(line)
        if "POLYGON_QA_PASS" in line or "Type help" in line: done.set()
thread=threading.Thread(target=read,daemon=True); thread.start()
try:
    assert done.wait(10), "server startup timed out"
    assert any("POLYGON_QA_PASS" in line for line in lines), "".join(lines)
    print(next(line.strip() for line in lines if "POLYGON_QA_PASS" in line))
finally:
    if proc.poll() is None:
        proc.stdin.write("quit\n"); proc.stdin.flush()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    thread.join(2)
    (RUN / "console.log").write_text("".join(lines),encoding="utf-8")
    print("Evidence:", RUN)
