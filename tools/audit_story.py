"""Read-only active story and world marker ownership diagnostics."""
from pathlib import Path
exec(Path(__file__).with_name('inspect.py').read_text().split('pattern=sys.argv')[0])

def prop(obj, key):
    for p,n,t in fields(q(obj+0x10)):
        if n == key: return p,i(p+0x50),t
    return None

def value(obj,key):
    p=prop(obj,key)
    if not p: return None
    ptr,off,t=p
    if t=='ObjectProperty':
        ref=q(obj+off); return hex(ref),full(ref)
    if t=='BoolProperty': return bool(rd(obj+off+rd(ptr+0x79,1)[0],1)[0]&rd(ptr+0x7b,1)[0])
    if t=='ByteProperty': return rd(obj+off,1)[0]
    if t=='ArrayProperty':
        arr=q(obj+off); num=i(obj+off+8)
        return num,[(hex(q(arr+j*8)),full(q(arr+j*8))) for j in range(min(num,20))]
    return rd(obj+off,min(i(ptr+0x34),16)).hex()

data=rd(objs,count*16)
classes={}
def isa(cls,wanted):
    key=(cls,wanted)
    if key not in classes:
        parent=cls; found=False
        while parent:
            if object_name(parent)==wanted: found=True; break
            parent=q(parent+0x30)
        classes[key]=found
    return classes[key]

keys={
 'BendJobManager':('ActiveJobRefsInOrder','PausedJobRefsInOrder','CurrentTrackedJob'),
 'BendGameMission':('IsMissionActive','ActiveMissionData','ActiveMissionScript','CurrentTrackedMission','InPostSaveGameLoad'),
 'BendJobMiniMapMarker':('bHidden','JobData','HasInWorldMarker','IsCurrentlyActive','InWorldIconComponent','InWorldJobIcon_CODE'),
 'BendGameMissionTrigger':('bHidden','CurrentlyTracked','IsIconActive','InWorldIcon_CODE','InWorldIconComponent','MissionData'),
 'BendObjective':('bHidden','IsIconActive','IsObjectiveActive','InWorldIcon_CODE'),
 'MapWaypointMarker':('bHidden','WaypointMarkerType','DesignWaypointWidget','MiniMapLocationComponent'),
 'BendUIObjectiveCalloutWidget':('Visibility','bHidden','ObjectiveIcon','DistanceText'),
 'MapLocationMgr':('WaypointMarkerMgr',),
}
for obj,flags,serial in [struct.unpack_from('<Qii',data,o) for o in range(0,len(data),16)]:
    if not obj or flags&((1<<28)|(1<<29)): continue
    try:
        if i(obj+8)&0x30: continue
        cls=q(obj+0x10)
        for baseclass,props in keys.items():
            if isa(cls,baseclass):
                print(hex(obj),object_name(cls),full(obj),{key:value(obj,key) for key in props})
                break
    except OSError: pass
k.CloseHandle(handle)
