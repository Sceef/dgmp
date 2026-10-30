"""Validate a shipped ZIP and run its real prepare-only path in a disposable fake game install.
The game is never started; actual game files, saves and server data are not touched.
"""
from hashlib import sha256
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
archive = Path((ROOT / "build/releases/latest-client.txt").read_text(encoding="utf-8"))
run = Path(tempfile.mkdtemp(prefix="package-validation-", dir=ROOT / "build"))
game = run / "Другая установка Days Gone"
game.mkdir()
flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

with zipfile.ZipFile(archive) as z:
    names = z.namelist()
    assert all(n.startswith("dgmp/") and ".." not in PurePosixPath(n).parts for n in names)
    forbidden = {"profiles", "logs", "runtime", "cache", "dev", ".git"}
    assert all(not (set(PurePosixPath(n).parts) & forbidden) for n in names)
    assert not any(n.endswith((".pdb", ".map", ".dmp", "server.ini", "skins.json")) for n in names)
    assert "dgmp/dgmp-server.exe" not in names
    assert not any(n.endswith("DaysGone.exe") for n in names)
    info = json.loads(z.read("dgmp/build-info.json"))
    assert not info["serverIncluded"] and not info["gameFilesIncluded"]
    for f in info["files"]:
        assert sha256(z.read("dgmp/" + f["path"])).hexdigest() == f["sha256"]
    z.extractall(game)
print("PASS archive excludes personal data, symbols, server and game binaries")

client = game / "dgmp"
source = game / "BendGame/Binaries/Win64"
source.mkdir(parents=True)
sentinel = b"DGMP disposable installation fixture; not an executable"
(source / "DaysGone.exe").write_bytes(sentinel)
for folder in ("Engine", "BendGame/Content/Paks", "BendGame/Content/Localization", "BendGame/Content/Movies"):
    (game / folder).mkdir(parents=True, exist_ok=True)

result = subprocess.run([str(client / "DGMP.exe"), "--prepare-only"], cwd=client,
                        creationflags=flags, timeout=60)
assert result.returncode == 0, (client / "logs/prepare-error.log").read_text(encoding="utf-8-sig", errors="replace")
assert (source / "DaysGone.exe").read_bytes() == sentinel
assert (client / "runtime/BendGame/Binaries/Win64/DaysGone.exe").read_bytes() == sentinel
assert sha256((client / "runtime/BendGame/Binaries/Win64/version.dll").read_bytes()).hexdigest() == info["clientSha256"]
assert (client / "runtime/BendGame/Binaries/Win64/dgmp-crashdump.exe").is_file()
assert (client / "runtime/Engine").resolve() == (game / "Engine").resolve()
assert (client / "runtime/BendGame/Content/Paks").resolve() == (game / "BendGame/Content/Paks").resolve()
print("PASS packaged launcher prepares a fresh install with Unicode and spaces in its path")

# Validate that a new account gets its own private save folder rather than the author's profile.
script = run / "check-account.ps1"
script.write_text("""param([string]$ClientRoot)
$ErrorActionPreference='Stop'
. (Join-Path $ClientRoot 'world-template.ps1')
Initialize-DGMPWorldProfile $ClientRoot (Join-Path $ClientRoot 'worlds/bootstrap') '76561198000000001' | Out-Null
""", encoding="utf-8-sig")
result = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                         "-ClientRoot", str(client)], cwd=client, creationflags=flags, timeout=30,
                        capture_output=True, text=True, errors="replace")
assert result.returncode == 0, result.stderr
private = client / "profiles/editor/BendGame/Saved/76561198000000001/SaveGames"
assert (private / "slot01.sav").read_bytes() == (client / "worlds/bootstrap/world.sav").read_bytes()
assert (private / "profile.sav").read_bytes() == (client / "worlds/bootstrap/profile.sav").read_bytes()
print("PASS template installs in a separate account folder without importing personal saves")

print("Archive:", archive)
print("Evidence:", run)
