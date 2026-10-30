**English** · [Русский](server-linux.ru.md)

# DGMP server on Linux

`dgmp-server` builds and runs on Linux (x86-64, glibc distributions: Ubuntu 22.04+, Debian 11+, Fedora, Rocky/Alma 9). Behavior, console commands, Lua modes and the network protocol are the same as `dgmp-server.exe`: Windows clients connect to it as usual.

## Dependencies

You need a C++20 compiler (g++ 10+ or clang 12+) and CMake 3.16+. ENet and Lua are in `third_party/` and are built statically together with the server; there is no need to install them separately.

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake
# Fedora / Rocky / Alma
sudo dnf install gcc-c++ make cmake
```

## What to copy to the VPS

From the `dgmp/` folder you need only:

- `CMakeLists.txt`, `src/server/`, `src/shared/`, `third_party/enet/`, `third_party/lua/` — for the build;
- `server.ini`, `modes/`, `quests/`, `data/looks.txt` — for running;
- `docs/server-linux.md`, `tools/dgmp-server.service` — this guide and the systemd unit (`cmake --install` puts them in `docs/`).

For example, from Git Bash on a Windows machine (the folder structure is preserved):

```bash
tar czf dgmp-server-src.tgz CMakeLists.txt src/server src/shared third_party/enet third_party/lua server.ini modes quests data/looks.txt docs/server-linux.md tools/dgmp-server.service
scp dgmp-server-src.tgz user@vps:~
ssh user@vps 'mkdir -p ~/dgmp && tar xzf ~/dgmp-server-src.tgz -C ~/dgmp'
```

## Building

```bash
cd ~/dgmp
cmake -S . -B build-linux
cmake --build build-linux -j
```

The `dgmp-server` binary appears in the root of `dgmp/`, next to `server.ini` and `modes/` — the same as `dgmp-server.exe` on Windows. The build is `RelWithDebInfo` (`-O2 -g`) by default, libstdc++ is linked statically (`-DDGMP_STATIC_RUNTIME=OFF` disables it), so the built file can be moved to other machines with the same or a newer glibc.

A ready server folder without sources:

```bash
sudo cmake --install build-linux --prefix /opt/dgmp
```

`dgmp-server`, `modes/`, `quests/`, `data/looks.txt`, this file and `docs/dgmp-server.service` are copied. `server.ini` is put in only on the first install — on an update your configured file is not overwritten. The install does not touch `data/<mode>/storage.json` (the modes' saves).

## Running

```bash
./dgmp-server --mode freeroam --port 7777
./dgmp-server --config /etc/dgmp/server.ini --name "My server" --max 32 --password secret --bind 0.0.0.0
```

The flags are the same as on Windows and override `server.ini` in the same way. Console commands (`help`, `status`, `say`, `kick`, `bot`, `reload`, `mode`, `time`, `weather`, `quit`) are read from stdin. To keep the console available after SSH disconnects, run the server in `tmux` or `screen`:

```bash
tmux new -s dgmp './dgmp-server --mode race'
# detach: Ctrl+B, then D; come back: tmux attach -t dgmp
```

Ctrl+C, `kill` and `systemctl stop` (SIGINT/SIGTERM/SIGHUP) stop the server like the `quit` command: players get a shutdown message. If you run the server in the background with `&`, it does not stop on reading stdin, it just works without a console.

The rest is as on Windows: modes reload themselves when `.lua`/`.ini`/`.json` change, the log is written to `logs/server.log`.

`server.ini` and `meta.ini` are read as UTF-8 (with or without BOM) or UTF-16 with BOM, so a server name in Russian can be set right in the file. Key and section names are case-insensitive, as on Windows.

## systemd

```bash
sudo useradd --system --home /opt/dgmp --shell /usr/sbin/nologin dgmp
sudo cmake --install build-linux --prefix /opt/dgmp
sudo chown -R dgmp:dgmp /opt/dgmp
sudo cp /opt/dgmp/docs/dgmp-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now dgmp-server
journalctl -u dgmp-server -f        # live log
sudo systemctl restart dgmp-server  # after changing the mode in server.ini
```

The mode and port are set in the unit's `ExecStart` (`tools/dgmp-server.service`) or in `/opt/dgmp/server.ini`. Under systemd there is no console; mode edits are picked up automatically, for anything else restart the service. If you need a console, run the server in `tmux`, not through systemd.

## Firewall

The server listens on **UDP** (7777 by default; the same port for the game and for status queries from the launcher). TCP is not needed.

```bash
sudo ufw allow 7777/udp                                                        # Ubuntu / Debian with ufw
sudo firewall-cmd --permanent --add-port=7777/udp && sudo firewall-cmd --reload  # Fedora / RHEL
```

Many VPS providers have their own network filter (a security group / firewall in the panel) — open UDP 7777 there too. For several servers on one machine, each has its own `--port` and its own open port.

## Crashes

On a crash `CRASH: signal ...` and a call stack are written to `logs/server.log`. Frames like `dgmp-server(+0x1a2b3)` have no function name; the function and source line for the offset are given by:

```bash
addr2line -f -C -e dgmp-server 0x1a2b3
```
