"""Native Windows server/probe smoke test, isolated under build/ (no game required).
Run: python tests/outpost_smoke.py
Keeps logs and saves for inspection; never touches data/outpost or a running server.
"""
from pathlib import Path
import json
import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
RUN = Path(tempfile.mkdtemp(prefix="outpost-smoke-", dir=ROOT / "build"))
for name in ("dgmp-server.exe", "server-outpost.ini"):
    shutil.copy2(ROOT / name, RUN / name)
for name in ("outpost", "_lib"):
    shutil.copytree(ROOT / "modes" / name, RUN / "modes" / name)
# Place a test bay only in the isolated fixture; the real editor layout is user-owned.
world_file=RUN / "modes/outpost/world.json"
test_world=json.loads(world_file.read_text(encoding="utf-8"))
garage=test_world["logic"]["garage"]
test_world["logic"]["garage_spawn"]=[{"x":garage["x"]+800,"y":garage["y"]+300,"z":garage["z"],"yaw":71}]
world_file.write_text(json.dumps(test_world),encoding="utf-8")
(RUN / "data").mkdir()
for f in (ROOT / "data").glob("*.txt"):
    shutil.copy2(f, RUN / "data" / f.name)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.bind(("127.0.0.1", 0))
    PORT = sock.getsockname()[1]
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


class Process:
    def __init__(self, args, name, lang="ru"):
        self.lines = []
        self.proc = subprocess.Popen(args, cwd=RUN, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                     creationflags=FLAGS, env={**os.environ, "DGMP_LANG": lang})
        self.log = (RUN / (name + ".log")).open("w", encoding="utf-8")
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.proc.stdout:
            self.lines.append(line.rstrip())
            self.log.write(line)
            self.log.flush()

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def wait(self, pattern, start=0, timeout=8):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            for line in self.lines[start:]:
                m = re.search(pattern, line)
                if m:
                    return m
            if self.proc.poll() is not None:
                break
            time.sleep(.05)
        raise AssertionError(f"Missing {pattern!r}; tail:\n" + "\n".join(self.lines[-15:]))

    def close(self, command=None):
        if self.proc.poll() is None:
            if command:
                self.send(command)
            else:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.thread.join(timeout=2)
        self.log.close()


server = client = english = None
checks = []


def check(label):
    checks.append(label)
    print("PASS", label, flush=True)


def command(line, expected):
    start = len(client.lines)
    client.send(line)
    return client.wait(expected, start)


POINTS = json.loads((RUN / "modes/outpost/world.json").read_text(encoding="utf-8"))["logic"]

def xyz(name):
    return " ".join(str(POINTS[name][axis]) for axis in ("x", "y", "z"))

def move(name):
    command("/scene " + name, r"teleport")
    client.send("!at " + xyz(name))
    time.sleep(.4)


def dialog(line, title, selection):
    m = command(line, r"DIALOG (\d+) .*\[" + title)
    client.send(f"!dlg {m[1]} 1 {selection}")
    time.sleep(.4)


try:
    server = Process([str(RUN / "dgmp-server.exe"), "--config", str(RUN / "server-outpost.ini"),
                      "--port", str(PORT)], "server-console")
    server.wait(r"Type help")
    check("native server loads mode")
    query = subprocess.run([str(ROOT / "build/dgmp-probe.exe"), "query", f"127.0.0.1:{PORT}"],
                           capture_output=True, text=True, encoding="utf-8", timeout=5, creationflags=FLAGS)
    assert query.returncode == 0 and "mode=Rubezh | DGMP RP" in query.stdout, query.stdout
    check("UDP query advertises outpost")
    client = Process([str(ROOT / "build/dgmp-probe.exe"), "join", f"127.0.0.1:{PORT}", "OutpostQA",
                      "--seconds", "110"], "probe")
    client.wait("Первый шаг")
    check("join initializes account and HUD")
    services={"Комендатура":"MAP_player_safehouse","Магазин снабжения":"MAP_objective_encampment_merchant",
              "Медпункт":"MAP_objective_NERO_checkpoint","Гараж":"MAP_objective_encampment_mechanic","Склад":"MAP_objective_encampment_locker"}
    for title,icon in services.items():
        client.wait(r"BLIP \d+ "+icon+r" \["+re.escape(title)+r"\] .+")
    check("five distinct service icons carry Russian names and descriptions")
    client.wait(r"CUSTOMIZE .*choices=6")
    client.wait(r"SKIN 0 \[Выживший в очках\].*m_host_eric")
    assert not any(re.search(r"SKIN .*deacon", line, re.I) for line in client.lines)
    command("!skin 0", r"teleport")
    check("starting appearance picker excludes Deacon and offers survivor with glasses first")
    command("/director kit", "Съёмочный комплект")
    move("office")
    # Discover the real interaction id rather than relying on allocation order.
    npc = client.wait(r"INTERACTION (\d+) .*Комендант")
    dialog("!use " + npc[1], "Комендатура", 0)
    command("/stats", r"репутация 31")
    check("NPC interaction and registration dialog")
    command("/bank deposit 100", r"Банк \$100")
    command("/bank withdraw 101", "Недостаточный баланс")
    check("bank rejects overdraft on native transport")
    assert not any("CARRY " in line for line in client.lines), "commander started delivery before quartermaster"
    move("warehouse")
    warehouse = client.wait(r"INTERACTION (\d+) .*Кладовщик")
    dialog("!use " + warehouse[1], "Кладовщик", 1)
    check("commander referral and quartermaster job acceptance")
    crate = client.wait(r"CARRY (\d+) crate at")
    command("/mission join", "У вас уже есть активное задание")
    check("active cargo prevents accepting an expedition")
    move("cargo")
    command("!pick " + crate[1], r"carrier \d+")
    time.sleep(4)
    move("warehouse")
    start = len(client.lines)
    client.send("!use " + warehouse[1])
    command("/stats", r"репутация 31")
    assert not any("DIALOG " in line for line in client.lines[start:]), "carrying crate opened quartermaster dialog"
    check("NPC interaction does not open a dialog while carrying")
    command("!put " + xyz("warehouse"), "Груз доставлен")
    check("native carry pickup, delivery and payout")
    move("shop")
    dialog("/shop", "Снабжение", 0)
    client.wait("Куплено: Паёк")
    command("/bag", r"ITEM")
    check("shop and native inventory window")
    move("garage")
    start=len(client.lines)
    dialog("/garage", "Гараж", 0)
    client.wait("Мотоцикл куплен и выдан",start)
    bike=client.wait(r"VEHICLE (\d+) model=bike .*yours=1",start)
    bay=POINTS["garage_spawn"][0]
    # createVehicle accepts floor Z; the native vehicle origin sits 60 UU above its wheels.
    expected_position=(bay["x"],bay["y"],bay["z"]+60)
    client.wait(r"VEHICLE "+bike[1]+r" model=bike at "+" ".join(f"{v:.0f}" for v in expected_position),start)
    check("purchase immediately creates and issues personal motorcycle")
    dialog("/garage", "Гараж", 1)
    client.wait("Мотоцикл убран в гараж",start)
    client.wait(r"VEHICLE "+bike[1]+" removed",start)
    start=len(client.lines)
    dialog("/garage", "Гараж", 1)
    client.wait("Мотоцикл уже в гараже",start)
    start=len(client.lines)
    dialog("/garage", "Гараж", 0)
    client.wait("Мотоцикл выдан на месте выдачи",start)
    check("motorcycle parking and free reissue work on native transport")
    move("office")
    command("/mission join", "Возьмите оружие")
    start=len(client.lines)
    move("expedition")
    # Four freakers guard the NERO site; the search starts once they are dead.
    client.wait("уничтожьте фриков у точки: 0/4",start)
    raiders=sorted({m[1] for line in client.lines[start:] for m in [re.search(r"zombie (\d+) Swarmer at",line)] if m})
    assert len(raiders)==4, raiders
    for z in raiders: client.send("!zdie "+z)
    client.wait("район зачищен",start)
    client.wait("Медикаменты найдены", timeout=18)
    move("warehouse")
    client.wait("Экспедиция: медикаменты доставлены")
    check("expedition search, return and reward")
    command("/director evac", "Эвакуация у района")
    command("/director clear", "Съёмочные объекты убраны")
    check("staged helicopter creation and cleanup")
    server.send("reload")
    time.sleep(1)
    command("/stats", "репутация 42")
    check("hot reload preserves character")
    english = Process([str(ROOT / "build/dgmp-probe.exe"), "join", f"127.0.0.1:{PORT}", "OutpostEnglishQA",
                       "--seconds", "20"], "probe-en", lang="en")
    english.wait("First step: register with the commander")
    english.wait(r"SKIN 0 \[Survivor \(glasses\)\]")
    start=len(english.lines); english.send("!skin 0"); english.wait(r"teleport",start)
    english.wait(r"ACTOR .*Commander")
    english.wait(r"INTERACTION .*Commander")
    for title in ("Command office","Supply store","Clinic","Garage","Warehouse"):
        english.wait(r"BLIP \d+ \S+ \["+re.escape(title)+r"\] .+")
    check("service blip labels are translated separately for the English client")
    start=len(english.lines); english.send("/stats"); english.wait(r"reputation 0",start)
    check("English HUD, NPC name, interaction and stats on real transport")
    assert any("Комендант" in line for line in client.lines)
    check("Russian and English clients retain separate NPC translations")
    english.close(); english=None
    client.close()
    client = None
    time.sleep(.5)
    server.close("quit")
    server = None
    state = json.loads((RUN / "data/outpost/storage.json").read_text(encoding="utf-8"))["outpost:v1"]
    account = state["accounts"]["guest:outpostqa"]
    assert account["bank"] == 100 and account["rep"] == 42 and account["bike"]
    assert account["money"] == 1280, account["money"]
    assert "m_host_eric" in account["skin"]
    # 1500 kit - 100 deposit + 65 delivery - 15 ration - 350 bike + 180 expedition.
    check("disk snapshot preserves exact economy")
    check("appearance is persisted in the server character account")
    logs = (RUN / "logs/server.log").read_text(encoding="utf-8", errors="replace")
    assert not re.search(r"error in|callback error|Lua error|cannot load|stack traceback", logs, re.I), logs
    check("no Lua errors in native server log")
    print(f"{len(checks)} native checks passed. Evidence: {RUN}", flush=True)
finally:
    if english:
        english.close()
    if client:
        client.close()
    if server:
        server.close("quit")
    print("Logs:", RUN, flush=True)
