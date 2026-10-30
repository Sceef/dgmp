[English](server-linux.md) · **Русский**

# Сервер DGMP на Linux

`dgmp-server` собирается и работает на Linux (x86-64, дистрибутивы с glibc: Ubuntu 22.04+, Debian 11+, Fedora, Rocky/Alma 9). Поведение, консольные команды, Lua-режимы и сетевой протокол те же, что у `dgmp-server.exe`: клиенты на Windows подключаются к нему как обычно.

## Зависимости

Нужен компилятор с C++20 (g++ 10+ или clang 12+) и CMake 3.16+. ENet и Lua лежат в `third_party/` и собираются статически вместе с сервером, отдельно ставить их не нужно.

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake
# Fedora / Rocky / Alma
sudo dnf install gcc-c++ make cmake
```

## Что скопировать на VPS

Из папки `dgmp/` нужны только:

- `CMakeLists.txt`, `src/server/`, `src/shared/`, `third_party/enet/`, `third_party/lua/` — для сборки;
- `server.ini`, `modes/`, `quests/`, `data/looks.txt` — для работы;
- `docs/server-linux.md`, `tools/dgmp-server.service` — эта инструкция и юнит systemd (`cmake --install` кладёт их в `docs/`).

Например, из Git Bash на Windows-машине (структура папок сохраняется):

```bash
tar czf dgmp-server-src.tgz CMakeLists.txt src/server src/shared third_party/enet third_party/lua server.ini modes quests data/looks.txt docs/server-linux.md tools/dgmp-server.service
scp dgmp-server-src.tgz user@vps:~
ssh user@vps 'mkdir -p ~/dgmp && tar xzf ~/dgmp-server-src.tgz -C ~/dgmp'
```

## Сборка

```bash
cd ~/dgmp
cmake -S . -B build-linux
cmake --build build-linux -j
```

Бинарник `dgmp-server` появляется в корне `dgmp/`, рядом с `server.ini` и `modes/` — так же, как `dgmp-server.exe` в Windows. По умолчанию сборка `RelWithDebInfo` (`-O2 -g`), libstdc++ линкуется статически (`-DDGMP_STATIC_RUNTIME=OFF` отключает), поэтому собранный файл можно переносить на другие машины с тем же или более новым glibc.

Готовая папка сервера без исходников:

```bash
sudo cmake --install build-linux --prefix /opt/dgmp
```

Копируются `dgmp-server`, `modes/`, `quests/`, `data/looks.txt`, этот файл и `docs/dgmp-server.service`. `server.ini` кладётся только при первой установке — при обновлении ваш настроенный файл не перезаписывается. `data/<mode>/storage.json` (сохранения режимов) установка не трогает.

## Запуск

```bash
./dgmp-server --mode freeroam --port 7777
./dgmp-server --config /etc/dgmp/server.ini --name "Мой сервер" --max 32 --password secret --bind 0.0.0.0
```

Флаги те же, что в Windows, и так же перекрывают `server.ini`. Консольные команды (`help`, `status`, `say`, `kick`, `bot`, `reload`, `mode`, `time`, `weather`, `quit`) читаются из stdin. Чтобы консоль оставалась доступной после отключения SSH, запускайте сервер в `tmux` или `screen`:

```bash
tmux new -s dgmp './dgmp-server --mode race'
# отсоединиться: Ctrl+B, затем D; вернуться: tmux attach -t dgmp
```

Ctrl+C, `kill` и `systemctl stop` (SIGINT/SIGTERM/SIGHUP) завершают сервер как команда `quit`: игроки получают сообщение о выключении. Если запустить сервер в фоне через `&`, он не останавливается при чтении stdin, просто работает без консоли.

Остальное как в Windows: режимы перезагружаются сами при изменении `.lua`/`.ini`/`.json`, журнал пишется в `logs/server.log`.

`server.ini` и `meta.ini` читаются как UTF-8 (с BOM или без) или UTF-16 с BOM, поэтому название сервера по-русски задаётся прямо в файле. Имена ключей и секций нечувствительны к регистру, как в Windows.

## systemd

```bash
sudo useradd --system --home /opt/dgmp --shell /usr/sbin/nologin dgmp
sudo cmake --install build-linux --prefix /opt/dgmp
sudo chown -R dgmp:dgmp /opt/dgmp
sudo cp /opt/dgmp/docs/dgmp-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now dgmp-server
journalctl -u dgmp-server -f        # живой журнал
sudo systemctl restart dgmp-server  # после смены режима в server.ini
```

Режим и порт задаются в `ExecStart` юнита (`tools/dgmp-server.service`) или в `/opt/dgmp/server.ini`. Под systemd консоли нет; правки режимов подхватываются автоматически, для остального — перезапуск сервиса. Если нужна консоль, запускайте сервер в `tmux`, а не через systemd.

## Файрвол

Сервер слушает **UDP** (по умолчанию 7777; один и тот же порт для игры и для запросов статуса из лаунчера). TCP не нужен.

```bash
sudo ufw allow 7777/udp                                                        # Ubuntu / Debian с ufw
sudo firewall-cmd --permanent --add-port=7777/udp && sudo firewall-cmd --reload  # Fedora / RHEL
```

У многих провайдеров VPS есть свой сетевой фильтр (security group / firewall в панели) — там тоже откройте UDP 7777. Для нескольких серверов на одной машине у каждого свой `--port` и свой открытый порт.

## Падения

При падении в `logs/server.log` пишется `CRASH: signal ...` и стек вызовов. У кадров вида `dgmp-server(+0x1a2b3)` имени функции нет; функцию и строку исходника по смещению даёт:

```bash
addr2line -f -C -e dgmp-server 0x1a2b3
```
