**English** · [Русский](master.ru.md)

# Master server: the shared server list

The launcher's **"Internet"** tab shows the servers that have announced themselves to the master server. The master server is the same `dgmp-server` in a separate mode. It stores only addresses and queries every server itself, so a foreign or non-existent address cannot be added to the list.

## How it works

1. A server with `Master=` in `server.ini` sends the master a heartbeat from its game port (UDP) once a minute (more often during the first minute).
2. The master queries it with the ordinary status request. If it answers, the server goes into the list; if it stops answering or there is no heartbeat for 3 minutes, it disappears.
3. The launcher takes the list of addresses from the master and queries every server itself: name, mode, players, ping.

Master limits: up to 5000 servers, up to 32 per IP, heartbeats no more than twice a second per address. The list contains only servers with the same protocol version as the launcher.

## Running a master server

```
dgmp-server --master [--port 7700]
```

The default port is **UDP 7700**; it must be opened in the firewall. The log is `logs/master.log`. On a Linux VPS — the unit [`tools/dgmp-master.service`](../tools/dgmp-master.service) (next to the regular `dgmp-server.service`, the same binary):

```
sudo cp tools/dgmp-master.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo systemctl enable --now dgmp-master
sudo ufw allow 7700/udp
```

## Server: getting into the list

`server.ini`:

```ini
Master=master.example.com        ; or an IP; port 7700 by default, your own — master.example.com:7701
```

Or with the command-line key `--master-server master.example.com`. An empty value — the server does not announce itself (default). Password-protected servers also get into the list, with a lock.

## Launcher

The default master address is set in the launcher code (`kDefaultMaster` in `src/launcher/main.cpp`); your own — in `profiles/launcher.json`:

```json
{ "master": "master.example.com:7700" }
```

## Verification

```
build\dgmp-probe.exe master 127.0.0.1        the master's list and a query to every server
```
