"""Build a shareable client ZIP from an explicit allowlist. No game install or player data."""
from datetime import datetime, timedelta, timezone
from hashlib import sha256
import json
from pathlib import Path
import re
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build" / "releases"


def main():
    version = re.search(r'kVersion\s*=\s*"([^"]+)"', (ROOT / "src/shared/version.h").read_text())[1]
    protocol = int(re.search(r'kProtocolVersion\s*=\s*(\d+)', (ROOT / "src/shared/protocol.h").read_text())[1])
    files = {}

    def include(path, destination=None):
        source = ROOT / path
        if not source.is_file() or source.is_symlink():
            raise ValueError(f"Required regular file missing: {source}")
        files[destination or path] = source.read_bytes()

    for path in ("DGMP.exe", "build/version.dll", "build/dgmp-crashdump.exe", "prepare.ps1", "world-template.ps1"):
        include(path)
    for path in ("launcher/ui/index.html", "launcher/ui/app.css", "launcher/ui/app.js"):
        include(path)
    for name in ("anims.txt", "lookparts.txt", "looks.txt", "parts.txt", "props_stats.txt", "props.txt", "sounds.txt"):
        include("data/" + name)
    for name in ("manifest.json", "world.sav", "profile.sav"):
        include("worlds/bootstrap/" + name)
    # The fixed shared bootstrap is required; never copy saves from profiles/ or AppData.
    seed = json.loads(files["worlds/bootstrap/manifest.json"])
    for entry in seed["files"]:
        data = files["worlds/bootstrap/" + entry["name"]]
        assert len(data) == entry["bytes"] and sha256(data).hexdigest().upper() == entry["sha256"].upper()

    for path in ("DGMP.exe", "build/version.dll", "build/dgmp-crashdump.exe"):
        data = files[path]
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[:2] == b"MZ" and data[pe:pe + 4] == b"PE\0\0"
        assert struct.unpack_from("<H", data, pe + 4)[0] == 0x8664, path + " is not x64"

    # Clean defaults, independent of the developer's microphone, spawn, language and browser state.
    files["DGMP.ini"] = b"""[DGMP]
AutoLoad=1
AutoFly=1
FlySpeed=2000
SuppressStory=1
SuppressAI=1

[World]
RevealMap=1

[Map]
Player=1
Motorcycle=1
Fuel=1
UserWaypoint=1
OtherNativeMarkers=0

[Voice]
OpenMic=0
TalkKey=N
RadioKey=K
Volume=100
MicVolume=100
Microphone=0

[Development]
Diagnostics=1
""".replace(b"\n", b"\r\n")
    files["editor.ini"] = b"[Editor]\r\nLanguage=ru\r\nDiagnostics=1\r\n"
    files["READ-ME-FIRST.txt"] = ("\ufeff" + """DGMP — тестовый клиент для Days Gone

УСТАНОВКА
1. Нужна своя установленная Steam-версия Days Gone, Windows x64 и запущенный Steam.
   Поддерживается EXE игры с TimeDateStamp 1752517958.
2. Распакуй папку dgmp из архива в корень Days Gone, рядом с BendGame и Engine:
      Days Gone/BendGame/
      Days Gone/Engine/
      Days Gone/dgmp/DGMP.exe
   Не клади её в Binaries/Win64. Для этой установки используй чистую папку dgmp.
3. Закрой игру и запусти dgmp/DGMP.exe. Сборка проекта и Visual Studio не нужны.
4. Выбери русский или английский язык, укажи свой ник и подключись по IP:порту,
   который даст владелец сервера. Если у сервера есть пароль — введи его в лаунчере.

Первый запуск готовит отдельную копию EXE и отдельный профиль. Игровой контент
подключается из твоей установки игры; он не входит в этот архив.
Личные сохранения игры не импортируются. Вместо них используется общий стартовый
шаблон DGMP в worlds/bootstrap. RP-персонаж и его прогресс хранятся на сервере.
Папка runtime содержит ссылки на контент игры: не удаляй через них файлы игры.

Лаунчеру нужен Microsoft Edge WebView2 Runtime. Если он отсутствует, лаунчер
покажет сообщение со ссылкой установки. Не запускай DaysGone.exe из runtime
напрямую — вход в игру должен идти через DGMP.exe.

УПРАВЛЕНИЕ
T — чат; F — NPC; удерживать F — поднять/поставить ящик; E — транспорт.
I — рюкзак; M — карта; N — голос рядом; K — рация. /voice — настройка микрофона.

ДИАГНОСТИКА ТЕСТОВОЙ СБОРКИ
Логи и дампы сохраняются локально в dgmp/logs/, автоматической отправки нет.
При падении закрой игру и сохрани logs, telemetry.json и build-info.json до нового
запуска: при перезапуске основной лог заменяется. Передай их разработчику вручную.
Дампы могут содержать память процесса; не публикуй их в открытом доступе.
Отключить расширенный сбор можно в DGMP.ini: [Development] Diagnostics=0.

Это тестовая сборка. Исправление сбоя при езде на серверном мотоцикле включено,
но повторная поездка в игре после исправления ещё не проверена. Загрузка общего
шаблона на другом Steam-аккаунте также входит в проверку этого тестового запуска.

В пакете только клиент. Сервер запускает владелец сервера; IP и порт узнавай у него.
""").encode("utf-8")

    for source, destination in (
        ("third_party/enet/LICENSE", "licenses/ENet.txt"),
        ("third_party/imgui/LICENSE.txt", "licenses/Dear-ImGui.txt"),
        ("third_party/imguizmo/LICENSE", "licenses/ImGuizmo.txt"),
        ("third_party/webview2/LICENSE.txt", "licenses/WebView2-SDK.txt"),
    ):
        include(source, destination)
    lua = (ROOT / "third_party/lua/src/lua.h").read_text(encoding="utf-8")
    files["licenses/Lua.txt"] = lua[lua.index("Copyright (C) 1994"):lua.rindex("******************************************************************************/")].replace("\n* ", "\n").encode("utf-8")

    dll_hash = sha256(files["build/version.dll"]).hexdigest()
    now = datetime.now(timezone.utc)
    local = now.astimezone(timezone(timedelta(hours=7)))
    identifier = f"{version}-test-{local:%Y%m%d-%H%M%S}-{dll_hash[:8]}"
    manifest = {
        "schema": 1, "kind": "client-test", "buildId": identifier, "version": version,
        "protocol": protocol, "gameBuild": 1752517958, "createdUtc": now.isoformat(),
        "clientSha256": dll_hash, "serverIncluded": False, "gameFilesIncluded": False,
        "files": [{"path": name, "bytes": len(data), "sha256": sha256(data).hexdigest()} for name, data in sorted(files.items())],
    }
    files["build-info.json"] = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    archive = OUTPUT / ("DGMP-client-" + identifier + ".zip")
    if archive.exists():
        raise FileExistsError(archive)
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            z.writestr("dgmp/" + name, data)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert set(z.namelist()) == {"dgmp/" + name for name in files}
        for entry in manifest["files"]:
            assert sha256(z.read("dgmp/" + entry["path"])).hexdigest() == entry["sha256"]
    checksum = sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(checksum + "  " + archive.name + "\n", encoding="ascii")
    # Private symbols stay with the developer and match the exact DLL sent to testers.
    symbols = OUTPUT / ("symbols-" + identifier)
    symbols.mkdir()
    for name in ("version.dll", "version.map", "version.pdb"):
        shutil.copy2(ROOT / "build" / name, symbols / name)
    (symbols / "build-info.json").write_bytes(files["build-info.json"])
    (OUTPUT / "latest-client.txt").write_text(str(archive), encoding="utf-8")
    print(archive)
    print(f"Files: {len(files)}; ZIP bytes: {archive.stat().st_size}; SHA256: {checksum}")


if __name__ == "__main__":
    main()
