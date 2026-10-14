"use strict";
// Bridge to DGMP.exe. Outside WebView2 (plain browser preview) a mock answers with sample data.
const bridge = (() => {
  const wv = window.chrome && window.chrome.webview;
  if (wv) return { send: (m) => wv.postMessage(JSON.stringify(m)), listen: (fn) => wv.addEventListener("message", (e) => fn(e.data)) };
  let handler = () => {};
  const sample = {
    "127.0.0.1:7777": { name: "Фарвелл Фрирум", mode: "Freeroam", players: 3, maxPlayers: 16, password: false, ping: 4, compatible: true },
    "95.31.4.18:7777": { name: "[RU] Орда — выживание", mode: "Horde", players: 15, maxPlayers: 16, password: true, ping: 48, compatible: true },
  };
  const later = (m, ms = 250) => setTimeout(() => handler(m), ms);
  const quests = { supplies: { id: "supplies", title: "Припасы для лагеря", category: "camp", xp: 150, reward: [{ item: "kConsumableID_Bandage_Small", count: 2 }],
    steps: [{ type: "goto", text: "Доберитесь до заправки", x: -79464, y: -301637, z: -5250, radius: 6 },
            { type: "collect", text: "Обыщите машины", event: "search", count: 2, ids: ["BP_Interact_SearchCarTrunk21"], marker: true }] } };
  const questList = () => ({ type: "questList", quests: Object.values(quests).map((q) => ({ id: q.id, title: q.title, category: q.category })) });
  return {
    listen: (fn) => (handler = fn),
    send: (m) => {
      if (m.cmd === "init") later({ type: "state", nick: "Sceef", lang: storedLang(), version: "0.6.0", protocol: 1, gameFound: true, serverAvailable: true,
        favorites: [{ address: "127.0.0.1:7777", name: "Фарвелл Фрирум" }, { address: "95.31.4.18:7777", name: "[RU] Орда — выживание" }, { address: "10.0.0.5:7777", name: "Старый сервер" }],
        recent: [{ address: "127.0.0.1:7777", name: "Фарвелл Фрирум" }], host: { name: "Мой сервер", mode: "freeroam", port: 7777, maxPlayers: 16 },
        modes: [{ id: "freeroam", name: "Freeroam", description: "Свободная езда, чат, телепорты, время и погода." }],
        addresses: [{ ip: "192.168.1.20", adapter: "Ethernet" }] }, 50);
      if (m.cmd === "query") later({ type: "query", tag: m.tag, results: m.addresses.map((a) => sample[a] ? { address: a, ...sample[a] } : { address: a, ping: -1 }) }, 400);
      if (m.cmd === "lan") later({ type: "lan", results: [{ address: "192.168.1.20:7777", ...sample["127.0.0.1:7777"] }] }, 600);
      if (m.cmd === "setNick") later({ type: "nickResult", ok: true, nick: m.nick }, 10);
      if (m.cmd === "connect") later({ type: "status", text: t("mock.launch"), busy: true });
      if (m.cmd === "questList") later(questList(), 20);
      if (m.cmd === "questLoad") later({ type: "quest", id: m.id, quest: quests[m.id] }, 20);
      if (m.cmd === "questSave") {
        if (m.oldId && m.oldId !== m.quest.id) delete quests[m.oldId];
        quests[m.quest.id] = m.quest;
        later({ type: "questSaved", ok: true, id: m.quest.id }, 20); later(questList(), 30);
      }
      if (m.cmd === "questDelete") { delete quests[m.id]; later(questList(), 20); }
      if (m.cmd === "questTest") later({ type: "status", text: t("mock.questTest", { id: m.id }), busy: true });
      if (m.cmd === "itemKeys") later({ type: "itemKeys", keys: ["kConsumableID_Bandage_Small", "kWeaponID_Molotov_Small", "kAmmoID_Pistol"] }, 20);
      if (m.cmd === "telemetry") later({ type: "telemetry", live: true, x: -85703.4, y: -302173.7, z: -5258.6, yaw: -70.9,
        aim: { id: "BP_Interact_SearchCarTrunk21", class: "BP_Interact_SearchCarTrunk_C", x: -85160, y: -302467, z: -5249 } }, 20);
    },
  };
})();

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const NICK_RE = /^[\p{L}\p{N}_\-.\[\]()]{3,24}$/u;
// index.html?view=quests: the quest window DGMP.exe opens next to the editor game.
const QUEST_VIEW = new URLSearchParams(location.search).get("view") === "quests";

// ---------------- language ----------------
// English by default; the choice is kept by DGMP.exe (launcher.json "lang") and comes with the state message.
// Static text: data-i18n (text), data-i18n-html, data-i18n-placeholder, data-i18n-title. "{name}" = parameter.
const I18N = {
  en: {
    "lang.title": "Language",
    "nick.label": "Nickname", "nick.placeholder": "Enter a nickname", "nick.help": "3–24 characters: letters, digits, _ - . [ ] ( )",
    "nav.servers": "Servers", "nav.host": "Host a server", "nav.editor": "Editor",
    "foot.modes": "Modes folder", "foot.logs": "Logs", "version": "Version {v} · protocol {p}",
    "list.favorites": "Favorites", "list.recent": "Recent", "list.lan": "LAN",
    "address.placeholder": "IP:port, e.g. 192.168.1.20:7777", "fav.add": "Add to favorites", "fav.remove": "Remove from favorites",
    "connect": "Connect", "refresh": "Refresh (F5)", "play": "Play",
    "th.name": "Name", "mode": "Mode", "th.players": "Players", "th.ping": "Ping",
    "ping.ms": "{n} ms", "ping.none": "no response", "tag.version": "other version",
    "empty.favorites": "No favorites yet.<br>Enter a server address above and press “Add to favorites”.",
    "empty.recent": "Servers you've played on will show up here.",
    "empty.lanSearch": "Searching the local network…", "empty.lan": "No LAN servers found.<br>Ports 7777–7786.",
    "detail.players": "{n}/{max} players", "detail.password": "password", "detail.incompatible": "incompatible version",
    "detail.offline": "server not responding",
    "err.nick": "Enter a valid nickname first", "err.gameRunning": "The game is already running",
    "err.version": "The server runs a different version", "err.noGame": "Days Gone not found: DGMP must be in the game folder",
    "host.name": "Server name", "host.port": "Port (UDP)", "host.slots": "Slots", "password": "Password", "optional": "(optional)",
    "host.play": "Start and play", "host.only": "Server only", "host.defaultName": "{nick}'s server",
    "host.noServer": "dgmp-server.exe not found next to the launcher.", "host.noModes": "No modes in the modes folder.",
    "mode.author": "Author: {name}",
    "console.title": "Server console", "console.placeholder": "Command: help, status, say, kick, bot, reload…",
    "srv.stopped": "Server not running", "srv.running": "Running on port {port}", "srv.stop": "Stop",
    "srv.thisPc": "this PC", "srv.copy": "copy", "srv.copied": "copied", "srv.exit": "— server stopped (code {code}) —",
    "editor.desc": "Single-player mode for building maps, modes and quests: fly (F2), dev menu (F3), save the start point (F5). "
      + "A quest window opens with the game. Story, saves and photo mode are disabled.",
    "editor.refresh": "Restore the original world template before launch", "editor.run": "Open editor",
    "status.ready": "Ready", "game.running": "Game running",
    "pass.title": "Password required", "cancel": "Cancel",
    "quests.title": "Quests", "quests.pickHint": "In game: F6 — point, F7 — object, F9 — item for the step",
    "tele.off": "game not running", "tele.on": "Game: {x} {y} {z} • aim: {aim}",
    "arm.text": "Left-click in the game…", "arm.repeat": "Repeat",
    "q.new": "New quest", "q.listEmpty": "No quests yet.<br>Quest files live in the quests folder.",
    "q.none": "Pick a quest on the left or create a new one.", "q.broken": "file error", "q.unsaved": "• unsaved",
    "delete": "Delete", "save": "Save", "q.test": "Test", "q.testHint": "Save, start a server in questtest mode and join it",
    "q.idHint": "(file name: a-z, 0-9, _ and -)", "q.title": "Title", "q.category": "Category", "q.style": "Marker style",
    "q.text": "“New task” text", "q.reward": "Reward", "q.xp": "XP", "q.addItem": "+ Item", "q.steps": "Steps",
    "addStep.goto": "+ Go to point", "addStep.hold": "+ Hold area", "addStep.collect": "+ Do N times",
    "cat.story": "Story", "cat.camp": "Camp", "cat.infestation": "Infestation", "cat.ambush": "Ambush", "cat.nero": "NERO",
    "cat.horde": "Horde", "cat.campmission": "Camp mission", "cat.task": "Errand",
    "style.basic": "Basic", "style.kill": "Kill", "style.killarea": "Kill in area", "style.defend": "Defend",
    "style.defendarea": "Defend area", "style.search": "Search", "style.track": "Track", "style.nokill": "No kills",
    "step.goto": "Go to point", "step.hold": "Hold area", "step.collect": "Do N times",
    "pick.point": "point", "pick.object": "object", "pick.item": "item",
    "event.pickup": "pick up", "event.carry": "carry", "event.search": "search car", "event.body": "search body",
    "event.interact": "interact", "event.door": "door", "event.explosion": "explosion", "event.fire": "set on fire",
    "err.nan": "{what}: not a number", "err.coords": "{what}: enter x, y, z coordinates", "err.coord": "{what}: {k} is empty",
    "err.id": "Quest id: only a-z, 0-9, _ and -", "err.itemKey": "{what}: enter an item key", "err.more": " (+{n} more)",
    "w.rewardCount": "Reward {n}, count", "w.step": "Step {n}", "w.radius": "{what}, radius", "w.seconds": "{what}, seconds",
    "w.count": "{what}, count", "w.area": "{what}, area", "w.areaRadius": "{what}, area radius", "w.item": "{what}, item {n}",
    "f.radius": "Radius, m", "f.itemKey": "Item key, e.g. kWeaponID_Molotov_Small", "f.count": "Count", "f.remove": "Remove",
    "pos.me": "My position", "pos.meHint": "Player position from the game", "pos.aim": "Aim", "pos.aimHint": "Position of the aimed object",
    "arm.pointArea": "Set point (area)", "arm.object": "Set object", "arm.item": "Set item", "arm.point": "Set point",
    "s.text": "Objective text", "s.route": "Route on map", "s.seconds": "Seconds in area", "s.meter": "Meter label",
    "s.events": "Events", "s.count": "Times", "s.item": "Item", "s.commaSep": "(comma-separated)", "s.class": "Class",
    "s.namePart": "(part of name)", "s.ids": "World objects", "s.objectName": "Object name", "s.addAim": "+ Aimed object",
    "s.addManual": "+ Manually", "s.area": "Area", "s.marker": "Marker over area", "s.spawn": "Place items",
    "s.spawnHint": "(only these count)", "s.up": "Move up", "s.down": "Move down", "s.remove": "Remove step",
    "toast.noQuest": "Open or create a quest", "toast.noStep": "Select a step", "toast.noCoords": "The pick has no coordinates",
    "toast.area": "Area → step {n}", "toast.point": "Point → step {n}", "toast.needCollect": "Select a “Do N times” step",
    "toast.noName": "The object has no name", "toast.inStep": "{id} is already in step {n}", "toast.object": "Object {id} → step {n}",
    "toast.item": "Item → step {n}",
    "confirm.discard": "Unsaved quest changes will be lost. Continue?", "confirm.delete": "Delete quest “{title}” (quests/{id}.json)?",
    "confirm.closeEditor": "The editor will close and the quest will start on a test server",
    "err.noPosition": "The game isn't running: no position data", "err.noAim": "Nothing under the crosshair",
    "err.aimObject": "Aim at an object in the game", "err.notRunning": "The game isn't running",
    "err.aimKind": "Aim at a trunk, door, item or barrel", "err.inList": "{id} is already in the list",
    "mock.launch": "Launching Days Gone…", "mock.questTest": "Testing quest “{id}”: starting the server…",
  },
  ru: {
    "lang.title": "Язык",
    "nick.label": "Ник", "nick.placeholder": "Введите ник", "nick.help": "3–24 символа: буквы, цифры, _ - . [ ] ( )",
    "nav.servers": "Серверы", "nav.host": "Создать сервер", "nav.editor": "Редактор",
    "foot.modes": "Папка режимов", "foot.logs": "Логи", "version": "Версия {v} · протокол {p}",
    "list.favorites": "Избранное", "list.recent": "Недавние", "list.lan": "Локальная сеть",
    "address.placeholder": "IP:порт, например 192.168.1.20:7777", "fav.add": "В избранное", "fav.remove": "Убрать из избранного",
    "connect": "Подключиться", "refresh": "Обновить (F5)", "play": "Играть",
    "th.name": "Название", "mode": "Режим", "th.players": "Игроки", "th.ping": "Пинг",
    "ping.ms": "{n} мс", "ping.none": "нет ответа", "tag.version": "другая версия",
    "empty.favorites": "В избранном пока пусто.<br>Введите адрес сервера сверху и нажмите «В избранное».",
    "empty.recent": "Здесь появятся серверы, на которых вы играли.",
    "empty.lanSearch": "Поиск серверов в локальной сети…", "empty.lan": "Серверы в локальной сети не найдены.<br>Порты 7777–7786.",
    "detail.players": "{n}/{max} игроков", "detail.password": "пароль", "detail.incompatible": "несовместимая версия",
    "detail.offline": "сервер не отвечает",
    "err.nick": "Сначала введите корректный ник", "err.gameRunning": "Игра уже запущена",
    "err.version": "Версия сервера не совпадает с вашей", "err.noGame": "Days Gone не найден: DGMP должен лежать в папке игры",
    "host.name": "Название сервера", "host.port": "Порт (UDP)", "host.slots": "Слоты", "password": "Пароль", "optional": "(необязательно)",
    "host.play": "Запустить и играть", "host.only": "Только сервер", "host.defaultName": "Сервер {nick}",
    "host.noServer": "dgmp-server.exe не найден рядом с лаунчером.", "host.noModes": "В папке modes нет режимов.",
    "mode.author": "Автор: {name}",
    "console.title": "Консоль сервера", "console.placeholder": "Команда: help, status, say, kick, bot, reload…",
    "srv.stopped": "Сервер не запущен", "srv.running": "Работает на порту {port}", "srv.stop": "Остановить",
    "srv.thisPc": "этот компьютер", "srv.copy": "копировать", "srv.copied": "скопировано", "srv.exit": "— сервер остановлен (код {code}) —",
    "editor.desc": "Одиночный режим для создания карт, режимов и квестов: полёт (F2), дев-меню (F3), сохранение точки старта (F5). "
      + "Вместе с игрой открывается окно квестов. Сюжет, сохранения и фоторежим отключены.",
    "editor.refresh": "Восстановить исходный шаблон мира перед запуском", "editor.run": "Открыть редактор",
    "status.ready": "Готово", "game.running": "Игра запущена",
    "pass.title": "Сервер защищён паролем", "cancel": "Отмена",
    "quests.title": "Квесты", "quests.pickHint": "В игре: F6 — точка, F7 — объект, F9 — предмет для шага",
    "tele.off": "игра не запущена", "tele.on": "Игра: {x} {y} {z} • прицел: {aim}",
    "arm.text": "Кликните ЛКМ в игре…", "arm.repeat": "Повторять",
    "q.new": "Новый квест", "q.listEmpty": "Квестов пока нет.<br>Файлы лежат в папке quests.",
    "q.none": "Выберите квест слева или создайте новый.", "q.broken": "ошибка в файле", "q.unsaved": "• не сохранён",
    "delete": "Удалить", "save": "Сохранить", "q.test": "Проверить", "q.testHint": "Сохранить, запустить сервер с режимом questtest и войти в игру",
    "q.idHint": "(имя файла: a-z, 0-9, _ и -)", "q.title": "Название", "q.category": "Категория", "q.style": "Вид маркеров",
    "q.text": "Текст «Новое задание»", "q.reward": "Награда", "q.xp": "Опыт", "q.addItem": "+ Предмет", "q.steps": "Шаги",
    "addStep.goto": "+ Дойти до точки", "addStep.hold": "+ Удерживать зону", "addStep.collect": "+ Сделать N раз",
    "cat.story": "Сюжет", "cat.camp": "Лагерь", "cat.infestation": "Заражение", "cat.ambush": "Засада", "cat.nero": "НЕРО",
    "cat.horde": "Орда", "cat.campmission": "Задание лагеря", "cat.task": "Поручение",
    "style.basic": "Обычный", "style.kill": "Убийство", "style.killarea": "Убийство в зоне", "style.defend": "Защита",
    "style.defendarea": "Защита зоны", "style.search": "Поиск", "style.track": "Выслеживание", "style.nokill": "Без убийств",
    "step.goto": "Дойти до точки", "step.hold": "Удерживать зону", "step.collect": "Сделать N раз",
    "pick.point": "точка", "pick.object": "объект", "pick.item": "предмет",
    "event.pickup": "подбор", "event.carry": "взять в руки", "event.search": "обыск машины", "event.body": "обыск трупа",
    "event.interact": "взаимодействие", "event.door": "дверь", "event.explosion": "взрыв", "event.fire": "поджог",
    "err.nan": "{what}: не число", "err.coords": "{what}: укажите координаты x, y, z", "err.coord": "{what}: не заполнено {k}",
    "err.id": "Id квеста: только латиница a-z, цифры, _ и -", "err.itemKey": "{what}: укажите ключ предмета", "err.more": " (и ещё {n})",
    "w.rewardCount": "Награда {n}, количество", "w.step": "Шаг {n}", "w.radius": "{what}, радиус", "w.seconds": "{what}, секунды",
    "w.count": "{what}, количество", "w.area": "{what}, зона", "w.areaRadius": "{what}, радиус зоны", "w.item": "{what}, предмет {n}",
    "f.radius": "Радиус, м", "f.itemKey": "Ключ предмета, например kWeaponID_Molotov_Small", "f.count": "Количество", "f.remove": "Убрать",
    "pos.me": "Моя позиция", "pos.meHint": "Координаты игрока из игры", "pos.aim": "Прицел", "pos.aimHint": "Координаты объекта под прицелом",
    "arm.pointArea": "Назначить точку (зона)", "arm.object": "Назначить объект", "arm.item": "Назначить предмет", "arm.point": "Назначить точку",
    "s.text": "Текст задачи", "s.route": "Маршрут на карте", "s.seconds": "Секунд в зоне", "s.meter": "Подпись шкалы",
    "s.events": "События", "s.count": "Сколько раз", "s.item": "Предмет", "s.commaSep": "(через запятую)", "s.class": "Класс",
    "s.namePart": "(часть имени)", "s.ids": "Объекты мира", "s.objectName": "Имя объекта", "s.addAim": "+ Объект под прицелом",
    "s.addManual": "+ Вручную", "s.area": "Зона", "s.marker": "Маркер над зоной", "s.spawn": "Разложить предметы",
    "s.spawnHint": "(засчитываются только они)", "s.up": "Выше", "s.down": "Ниже", "s.remove": "Удалить шаг",
    "toast.noQuest": "Откройте или создайте квест", "toast.noStep": "Выберите шаг", "toast.noCoords": "В пике нет координат",
    "toast.area": "Зона → шаг {n}", "toast.point": "Точка → шаг {n}", "toast.needCollect": "Выберите шаг «сделать N раз»",
    "toast.noName": "У объекта нет имени", "toast.inStep": "{id} уже в шаге {n}", "toast.object": "Объект {id} → шаг {n}",
    "toast.item": "Предмет → шаг {n}",
    "confirm.discard": "Несохранённые изменения квеста будут потеряны. Продолжить?", "confirm.delete": "Удалить квест «{title}» (quests/{id}.json)?",
    "confirm.closeEditor": "Редактор будет закрыт, квест запустится на тестовом сервере",
    "err.noPosition": "Игра не запущена: нет данных о позиции", "err.noAim": "Под прицелом ничего нет",
    "err.aimObject": "Наведите прицел на объект в игре", "err.notRunning": "Игра не запущена",
    "err.aimKind": "Наведитесь на багажник, дверь, предмет или бочку", "err.inList": "{id} уже в списке",
    "mock.launch": "Запуск Days Gone…", "mock.questTest": "Проверка квеста «{id}»: запуск сервера…",
  },
};
let lang = "en";
function t(key, params) {
  const s = I18N[lang][key] ?? I18N.en[key] ?? key;
  return params ? s.replace(/\{(\w+)\}/g, (m, k) => (k in params ? params[k] : m)) : s;
}

const app = {
  state: null,
  list: "favorites",
  info: {},          // address -> last query result
  lan: [],
  selected: null,
  querying: false,
  serverRunning: false,
  serverPort: 0,
  gameRunning: false,
  gameEditor: false,  // the running game is the editor
  pendingConnect: null,
};

// ---------------- rendering ----------------
function entries() {
  if (app.list === "lan") return app.lan.map((r) => ({ address: r.address, name: r.name }));
  return (app.state?.[app.list] || []);
}
function isFavorite(address) { return (app.state?.favorites || []).some((f) => f.address === address); }

function pingClass(p) { return p < 0 ? "ping-none" : p < 80 ? "ping-good" : p < 160 ? "ping-mid" : "ping-bad"; }

function renderServers() {
  const rows = entries();
  const body = $("#server-rows");
  body.innerHTML = rows.map((e) => {
    const i = app.info[e.address];
    const online = i && i.ping >= 0;
    const name = online ? i.name : (e.name || e.address);
    const players = online ? `<span class="${i.players >= i.maxPlayers ? "full" : ""}">${i.players} / ${i.maxPlayers}</span>` : "—";
    const ping = !i ? "…" : online ? t("ping.ms", { n: i.ping }) : t("ping.none");
    const tags = online && !i.compatible ? `<span class="tag">${t("tag.version")}</span>` : "";
    return `<tr data-address="${esc(e.address)}" class="${online ? "" : "offline"} ${app.selected === e.address ? "sel" : ""}">
      <td class="c-lock">${online && i.password ? "🔒" : ""}</td>
      <td><span class="srv-name">${esc(name)}</span>${tags}<span class="srv-addr">${esc(e.address)}</span></td>
      <td class="c-mode">${online ? esc(i.mode) : ""}</td>
      <td class="c-players">${players}</td>
      <td class="c-ping ${i ? pingClass(i.ping) : "ping-none"}">${ping}</td>
      <td class="c-act"><button class="star ${isFavorite(e.address) ? "on" : ""}" data-star="${esc(e.address)}" title="${t("list.favorites")}">★</button></td>
    </tr>`;
  }).join("");
  const empty = $("#empty");
  empty.hidden = rows.length > 0;
  empty.innerHTML = t(app.list === "favorites" ? "empty.favorites" : app.list === "recent" ? "empty.recent"
    : app.querying ? "empty.lanSearch" : "empty.lan");
  renderDetail();
}

function renderDetail() {
  const d = $("#detail");
  const address = app.selected;
  if (!address || !entries().some((e) => e.address === address)) { d.hidden = true; return; }
  const i = app.info[address];
  const online = i && i.ping >= 0;
  d.hidden = false;
  $("#d-name").textContent = online ? i.name : (entries().find((e) => e.address === address)?.name || address);
  $("#d-meta").textContent = online
    ? `${address} · ${i.mode} · ${t("detail.players", { n: i.players, max: i.maxPlayers })} · ${t("ping.ms", { n: i.ping })}`
      + (i.password ? " · " + t("detail.password") : "") + (i.compatible ? "" : " · " + t("detail.incompatible"))
    : `${address} · ${t("detail.offline")}`;
  $("#d-fav").textContent = t(isFavorite(address) ? "fav.remove" : "fav.add");
  $("#d-play").disabled = app.gameRunning || (online && !i.compatible);
}

function setStatus(text, busy = false, error = false) {
  $("#status-text").removeAttribute("data-i18n");  // "Ready" until the first status
  $("#status-text").textContent = text;
  $("#spinner").hidden = !busy;
  $("#statusbar").classList.toggle("error", !!error);
}

// ---------------- actions ----------------
function refresh() {
  if (app.list === "lan") { app.querying = true; app.lan = []; renderServers(); bridge.send({ cmd: "lan" }); return; }
  const addresses = entries().map((e) => e.address);
  if (!addresses.length) return;
  for (const a of addresses) delete app.info[a];
  renderServers();
  bridge.send({ cmd: "query", tag: app.list, addresses });
}

function nickValid() { return NICK_RE.test($("#nick").value.trim()); }

function connect(address, password) {
  if (!nickValid()) { setStatus(t("err.nick"), false, true); $("#nick").focus(); return; }
  if (app.gameRunning) { setStatus(t("err.gameRunning"), false, true); return; }
  const i = app.info[address];
  if (i && i.ping >= 0 && !i.compatible) { setStatus(t("err.version"), false, true); return; }
  if (password === undefined && i && i.password) { openPassword(address); return; }
  bridge.send({ cmd: "connect", address, password: password || "", name: i && i.ping >= 0 ? i.name : "" });
}

function openPassword(address) {
  app.pendingConnect = address;
  $("#pass-server").textContent = app.info[address]?.name || address;
  $("#pass-input").value = "";
  $("#pass-modal").hidden = false;
  $("#pass-input").focus();
}

function toggleFavorite(address) {
  const i = app.info[address];
  bridge.send({ cmd: "favorite", address, add: !isFavorite(address), name: i && i.ping >= 0 ? i.name : "" });
}

function selectPage(page) {
  $$(".nav-item").forEach((b) => b.classList.toggle("active", b.dataset.page === page));
  $$(".page").forEach((p) => p.classList.toggle("active", p.id === "page-" + page));
  if (page === "quests") openQuestsPage();
}

// ---------------- host ----------------
function fillHost() {
  const h = app.state.host || {};
  const modes = app.state.modes || [];
  $("#h-name").value = h.name || ($("#nick").value.trim() ? t("host.defaultName", { nick: $("#nick").value.trim() }) : "DGMP Server");
  $("#h-mode").innerHTML = modes.map((m) => `<option value="${esc(m.id)}">${esc(m.name)}${m.version ? " " + esc(m.version) : ""}</option>`).join("");
  if (h.mode && modes.some((m) => m.id === h.mode)) $("#h-mode").value = h.mode;
  $("#h-port").value = h.port || 7777;
  $("#h-max").value = h.maxPlayers || 16;
  const can = app.state.serverAvailable && modes.length > 0;
  $("#h-play").disabled = $("#h-only").disabled = !can || app.serverRunning;
  describeHost();
}
// Mode description, or why no server can be started.
function describeHost() {
  describeMode();
  if (!app.state.serverAvailable) $("#h-mode-desc").textContent = t("host.noServer");
  else if (!(app.state.modes || []).length) $("#h-mode-desc").textContent = t("host.noModes");
}
function describeMode() {
  const m = (app.state.modes || []).find((x) => x.id === $("#h-mode").value);
  $("#h-mode-desc").textContent = m ? [m.description, m.author && t("mode.author", { name: m.author })].filter(Boolean).join(" · ") : "";
}
function hostConfig() {
  return { name: $("#h-name").value.trim() || "DGMP Server", mode: $("#h-mode").value, port: +$("#h-port").value || 7777,
           maxPlayers: +$("#h-max").value || 16, password: $("#h-pass").value };
}
function setServerRunning(running, port) {
  app.serverRunning = running;
  app.serverPort = port;
  $("#server-dot").hidden = !running;
  $("#srv-stop").hidden = !running;
  $("#console-line").disabled = !running;
  renderServerState();
  if (app.state) fillHost();
}
function renderServerState() {
  const running = app.serverRunning, port = app.serverPort;
  $("#srv-state").textContent = running ? t("srv.running", { port }) : t("srv.stopped");
  $("#srv-state").classList.toggle("on", running);
  const addr = $("#srv-addr");
  addr.hidden = !running;
  if (running) {
    const list = [{ ip: "127.0.0.1", adapter: t("srv.thisPc") }, ...(app.state.addresses || [])];
    addr.innerHTML = list.map((a) => `<span class="addr">${esc(a.ip)}:${port} <small>${esc(a.adapter)}</small><button data-copy="${esc(a.ip)}:${port}">${t("srv.copy")}</button></span>`).join("");
  }
}
function consoleLine(line) {
  const c = $("#console");
  const atBottom = c.scrollTop + c.clientHeight >= c.scrollHeight - 8;
  const div = document.createElement("div");
  div.textContent = line;
  if (/error|CRASH|failed|Cannot/i.test(line)) div.className = "err";
  else if (/\] <[^>]+> /.test(line)) div.className = "chat";
  else if (/^\[[\d:]+\] (Connection from|Mode scripts changed)/.test(line)) div.className = "sys";
  c.appendChild(div);
  while (c.childNodes.length > 2000) c.removeChild(c.firstChild);
  if (atBottom) c.scrollTop = c.scrollHeight;
}

// ---------------- quests ----------------
// The editor keeps the quest as raw form text (qe.quest) and builds the file JSON only on save (buildQuest).
// id -> label in the current language (rebuilt by setLang).
let QUEST_CATEGORIES, QUEST_STYLES, STEP_TYPES, PICK_KINDS, QUEST_EVENTS;
const labelMap = (prefix, ids) => Object.fromEntries(ids.map((id) => [id, t(prefix + id)]));
function buildLabels() {
  QUEST_CATEGORIES = labelMap("cat.", ["story", "camp", "infestation", "ambush", "nero", "horde", "campmission", "task"]);
  QUEST_STYLES = labelMap("style.", ["basic", "kill", "killarea", "defend", "defendarea", "search", "track", "nokill"]);
  STEP_TYPES = labelMap("step.", ["goto", "hold", "collect"]);
  PICK_KINDS = labelMap("pick.", ["point", "object", "item"]);
  QUEST_EVENTS = labelMap("event.", ["pickup", "carry", "search", "body", "interact", "door", "explosion", "fire"]);
}
const QUEST_ID_RE = /^[a-z0-9_-]{1,64}$/;
const QUEST_KEYS = ["id", "title", "category", "style", "text", "xp", "reward", "steps"];
const STEP_KEYS = ["type", "text", "x", "y", "z", "radius", "seconds", "meter", "event", "count", "item", "class", "ids", "area", "marker", "route", "spawn"];

const qe = {
  list: [],           // [{id, title, category, broken}] from quests/*.json
  quest: null,        // the quest being edited
  loadedId: null,     // file it came from (null = not saved yet)
  dirty: false,
  selected: -1,       // selected step: picks from the game go there, the game highlights it
  pendingTest: false, // "Проверить" waits for questSaved
  closeEditor: false, // ...and may close the editor game first
  arm: null,          // {kind, timer}: waiting for a left click in the game
  tele: { live: false },
  opened: false,
};

const str = (v) => (v === undefined || v === null ? "" : String(v));
// Fields this editor doesn't know survive a load/save round trip.
const extraFields = (obj, known) => Object.fromEntries(Object.entries(obj || {}).filter(([k]) => !known.includes(k)));
function getPath(obj, path) { return path.split(".").reduce((o, k) => (o == null ? o : o[k]), obj); }
function setPath(obj, path, value) {
  const keys = path.split(".");
  const last = keys.pop();
  (keys.length ? getPath(obj, keys.join(".")) : obj)[last] = value;
}

function stepModel(s = {}) {
  const type = STEP_TYPES[s.type] ? s.type : "goto";
  const area = s.area || {};
  return {
    type, text: str(s.text), x: str(s.x), y: str(s.y), z: str(s.z), radius: str(s.radius), seconds: str(s.seconds), meter: str(s.meter),
    event: [].concat(s.event || []), count: str(s.count), item: [].concat(s.item || []).join(", "), class: str(s.class),
    ids: (s.ids || []).map(str), area: { x: str(area.x), y: str(area.y), z: str(area.z), radius: str(area.radius) },
    marker: !!s.marker, route: type === "goto" ? s.route !== false : !!s.route,
    spawn: (s.spawn || []).map((r) => ({ item: str(r.item), count: str(r.count), x: str(r.x), y: str(r.y), z: str(r.z) })),
    extra: extraFields(s, STEP_KEYS),
  };
}
function questModel(q = {}) {
  return {
    id: str(q.id), title: str(q.title), category: QUEST_CATEGORIES[q.category] ? q.category : "camp", style: QUEST_STYLES[q.style] ? q.style : "basic",
    text: str(q.text), xp: str(q.xp), reward: (q.reward || []).map((r) => ({ item: str(r.item), count: str(r.count) })),
    steps: (q.steps || []).map(stepModel), extra: extraFields(q, QUEST_KEYS),
  };
}

// Form model -> file JSON. Empty and default values are left out; returns { quest, errors }.
function buildQuest(m) {
  const errors = [];
  const num = (v, what) => {
    const text = str(v).trim().replace(",", ".");
    if (!text) return undefined;
    const n = Number(text);
    if (!Number.isFinite(n)) { errors.push(t("err.nan", { what })); return undefined; }
    return n;
  };
  const put = (o, k, v) => { if (v !== undefined && v !== "" && !(Array.isArray(v) && !v.length)) o[k] = v; };
  const one = (list) => (list.length === 1 ? list[0] : list);  // "a" or ["a", "b"]
  const point = (o, src, what, required) => {
    const has = ["x", "y", "z"].some((k) => str(src[k]).trim());
    if (!has) { if (required) errors.push(t("err.coords", { what })); return; }
    for (const k of ["x", "y", "z"]) {
      const n = num(src[k], `${what}, ${k}`);
      if (n === undefined) errors.push(t("err.coord", { what, k }));
      else o[k] = n;
    }
  };

  const id = m.id.trim();
  if (!QUEST_ID_RE.test(id)) errors.push(t("err.id"));
  const q = { ...m.extra, id };
  put(q, "title", m.title.trim());
  q.category = m.category;
  if (m.style !== "basic") q.style = m.style;
  put(q, "text", m.text.trim());
  const xp = num(m.xp, t("q.xp"));
  if (xp) q.xp = xp;
  const reward = [];
  m.reward.forEach((r, i) => {
    if (!r.item.trim()) return;
    reward.push({ item: r.item.trim(), count: num(r.count, t("w.rewardCount", { n: i + 1 })) ?? 1 });
  });
  put(q, "reward", reward);

  q.steps = m.steps.map((s, i) => {
    const what = t("w.step", { n: i + 1 });
    const o = { ...s.extra, type: s.type };
    put(o, "text", s.text.trim());
    if (s.type === "goto" || s.type === "hold") {
      point(o, s, what, true);
      const r = num(s.radius, t("w.radius", { what }));
      if (r !== undefined && r !== (s.type === "goto" ? 5 : 6)) o.radius = r;
    }
    if (s.type === "goto" && !s.route) o.route = false;
    if (s.type === "hold") {
      const sec = num(s.seconds, t("w.seconds", { what }));
      if (sec !== undefined && sec !== 10) o.seconds = sec;
      if (s.meter.trim() && s.meter.trim() !== "Поиск") o.meter = s.meter.trim();
    }
    if (s.type === "collect") {
      const events = Object.keys(QUEST_EVENTS).filter((e) => s.event.includes(e));
      if (events.length) o.event = one(events);
      put(o, "count", num(s.count, t("w.count", { what })));
      const items = s.item.split(",").map((x) => x.trim()).filter(Boolean);
      if (items.length) o.item = one(items);
      put(o, "class", s.class.trim());
      put(o, "ids", s.ids.map((x) => x.trim()).filter(Boolean));
      const area = {};
      point(area, s.area, t("w.area", { what }), false);
      if (Object.keys(area).length) {
        put(area, "radius", num(s.area.radius, t("w.areaRadius", { what })));
        o.area = area;
      } else if (str(s.area.radius).trim()) errors.push(t("err.coords", { what: t("w.area", { what }) }));
      if (s.marker) o.marker = true;
      if (s.route) o.route = true;
      const spawn = [];
      s.spawn.forEach((r, k) => {
        if (![r.item, r.x, r.y, r.z].some((v) => str(v).trim())) return;
        const itemWhat = t("w.item", { what, n: k + 1 });
        const row = { item: r.item.trim(), count: num(r.count, itemWhat) ?? 1 };
        if (!row.item) errors.push(t("err.itemKey", { what: itemWhat }));
        point(row, r, itemWhat, true);
        spawn.push(row);
      });
      put(o, "spawn", spawn);
    }
    return o;
  });
  return { quest: q, errors };
}

// ---- rendering ----
function renderQuestList() {
  $("#q-list").innerHTML = qe.list.map((q) => `<button type="button" class="quest-item ${q.id === qe.loadedId ? "sel" : ""} ${q.broken ? "broken" : ""}" data-qid="${esc(q.id)}">
      <span class="q-name">${q.broken ? t("q.broken") : esc(q.title || q.id)}</span>
      <span class="q-meta">${esc(q.id)}${QUEST_CATEGORIES[q.category] ? " · " + QUEST_CATEGORIES[q.category] : ""}</span>
    </button>`).join("");
  $("#q-list-empty").hidden = qe.list.length > 0;
}
function renderQuestFile() {
  const id = qe.quest?.id.trim() || "…";
  $("#q-file").innerHTML = `quests/${esc(id)}.json${qe.dirty ? `<span class="dirty">${t("q.unsaved")}</span>` : ""}`;
  $("#q-id").classList.toggle("bad", !!qe.quest && !QUEST_ID_RE.test(qe.quest.id.trim()));
}
function renderQuestForm() {
  const q = qe.quest;
  schedulePreview(true);
  $("#q-none").hidden = !!q;
  $("#q-form").hidden = !q;
  if (!q) return;
  for (const k of ["id", "title", "category", "style", "text", "xp"]) $("#q-" + k).value = q[k];
  renderQuestFile();
  renderReward();
  renderSteps();
}

const fieldHtml = (label, f, v, attrs = "") => `<div class="field"><label>${label}</label><input data-f="${f}" value="${esc(v)}" ${attrs}></div>`;
const coordAttrs = 'type="number" step="any"';
const posButtons = (prefix) => `<div class="btns">
    <button type="button" class="btn tiny" data-pos="me" data-target="${prefix}" title="${t("pos.meHint")}">${t("pos.me")}</button>
    <button type="button" class="btn tiny" data-pos="aim" data-target="${prefix}" title="${t("pos.aimHint")}">${t("pos.aim")}</button>
  </div>`;
// x / y / z (+ radius) with the two "take from game" buttons. prefix: "" for the step itself, "area." etc.
function coordsHtml(prefix, o, radius) {
  return `<div class="coords">
    ${["x", "y", "z"].map((k) => fieldHtml(k, prefix + k, o[k], coordAttrs)).join("")}
    ${radius ? fieldHtml(t("f.radius"), prefix + "radius", o.radius, `type="number" step="any" min="0" placeholder="${radius}"`) : ""}
    ${posButtons(prefix)}
  </div>`;
}
function itemRowHtml(prefix, r, withCoords) {
  return `<div class="item-row">
    <input class="key" data-f="${prefix}item" value="${esc(r.item)}" list="item-keys" placeholder="${t("f.itemKey")}">
    <input class="cnt" data-f="${prefix}count" value="${esc(r.count)}" type="number" min="1" placeholder="1" title="${t("f.count")}">
    ${withCoords ? ["x", "y", "z"].map((k) => `<input class="coord" data-f="${prefix}${k}" value="${esc(r[k])}" ${coordAttrs} placeholder="${k}">`).join("") + posButtons(prefix) : ""}
    <button type="button" class="btn x" data-del="${prefix.slice(0, -1)}" title="${t("f.remove")}">✕</button>
  </div>`;
}
function renderReward() {
  $("#q-reward").innerHTML = qe.quest.reward.map((r, i) => itemRowHtml(`reward.${i}.`, r, false)).join("");
}

// "Pick in the game" buttons (quest window, selected step only).
function armBarHtml(s) {
  const kinds = s.type === "collect" ? { point: t("arm.pointArea"), object: t("arm.object"), item: t("arm.item") } : { point: t("arm.point") };
  return `<div class="arm-bar">${Object.entries(kinds).map(([k, label]) => `<button type="button" class="btn tiny" data-arm="${k}">${label}</button>`).join("")}</div>`;
}
function stepBodyHtml(s) {
  const text = armBarHtml(s) + fieldHtml(t("s.text"), "text", s.text, 'maxlength="200"');
  if (s.type === "goto") {
    return text + coordsHtml("", s, 5) + `<label class="check inline"><input type="checkbox" data-f="route" ${s.route ? "checked" : ""}> ${t("s.route")}</label>`;
  }
  if (s.type === "hold") {
    return text + coordsHtml("", s, 6) + `<div class="grid4">
      ${fieldHtml(t("s.seconds"), "seconds", s.seconds, 'type="number" min="1" step="any" placeholder="10"')}
      ${fieldHtml(t("s.meter"), "meter", s.meter, 'placeholder="Поиск"')}
    </div>`;
  }
  return text + `
    <div class="field"><label>${t("s.events")}</label><div class="events">
      ${Object.entries(QUEST_EVENTS).map(([e, label]) => `<label class="check inline"><input type="checkbox" data-event="${e}" ${s.event.includes(e) ? "checked" : ""}> ${label}</label>`).join("")}
    </div></div>
    <div class="grid3">
      ${fieldHtml(t("s.count"), "count", s.count, `type="number" min="1" placeholder="${s.spawn.length || 1}"`)}
      ${fieldHtml(`${t("s.item")} <span class="muted">${t("s.commaSep")}</span>`, "item", s.item, 'list="item-keys"')}
      ${fieldHtml(`${t("s.class")} <span class="muted">${t("s.namePart")}</span>`, "class", s.class, 'placeholder="BP_Interact_SearchCarTrunk"')}
    </div>
    <div class="field"><label>${t("s.ids")} <span class="muted">(ids)</span></label>
      <div class="rows">${s.ids.map((id, i) => `<div class="item-row"><input class="key" data-f="ids.${i}" value="${esc(id)}" placeholder="${t("s.objectName")}"><button type="button" class="btn x" data-del="ids.${i}" title="${t("f.remove")}">✕</button></div>`).join("")}</div>
      <div class="actions">
        <button type="button" class="btn small" data-add="aim-id">${t("s.addAim")}</button>
        <button type="button" class="btn small" data-add="ids">${t("s.addManual")}</button>
      </div>
    </div>
    <div class="field"><label>${t("s.area")} <span class="muted">${t("optional")}</span></label>${coordsHtml("area.", s.area, "")}</div>
    <div class="checks">
      <label class="check inline"><input type="checkbox" data-f="marker" ${s.marker ? "checked" : ""}> ${t("s.marker")}</label>
      <label class="check inline"><input type="checkbox" data-f="route" ${s.route ? "checked" : ""}> ${t("s.route")}</label>
    </div>
    <div class="field"><label>${t("s.spawn")} <span class="muted">${t("s.spawnHint")}</span></label>
      <div class="rows">${s.spawn.map((r, i) => itemRowHtml(`spawn.${i}.`, r, true)).join("")}</div>
      <button type="button" class="btn small" data-add="spawn">${t("q.addItem")}</button>
    </div>`;
}
function renderSteps() {
  const steps = qe.quest.steps;
  $("#q-steps").innerHTML = steps.map((s, i) => `<div class="step ${i === qe.selected ? "sel" : ""}" data-step="${i}">
      <div class="step-head">
        <span class="step-num">${i + 1}</span>
        <select data-step-type>${Object.entries(STEP_TYPES).map(([t, label]) => `<option value="${t}" ${t === s.type ? "selected" : ""}>${label}</option>`).join("")}</select>
        <span class="spacer"></span>
        <button type="button" class="btn x" data-act="up" title="${t("s.up")}" ${i === 0 ? "disabled" : ""}>↑</button>
        <button type="button" class="btn x" data-act="down" title="${t("s.down")}" ${i === steps.length - 1 ? "disabled" : ""}>↓</button>
        <button type="button" class="btn x" data-act="remove" title="${t("s.remove")}">✕</button>
      </div>
      <div class="step-body">${stepBodyHtml(s)}</div>
    </div>`).join("");
}
function renderTelemetry() {
  const tl = qe.tele, el = $("#q-tele");
  el.classList.toggle("on", !!tl.live);
  el.textContent = tl.live ? t("tele.on", { x: Math.round(tl.x), y: Math.round(tl.y), z: Math.round(tl.z), aim: tl.aim?.id || "—" }) : t("tele.off");
}

let toastTimer = 0;
function toast(text, error = false) {
  const t = $("#toast");
  t.textContent = text;
  t.classList.toggle("error", error);
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 2500);
}

// ---- link with the editor game (quest window only) ----
// quest_preview.json: the quest as it would be saved (unsaved edits too) + the selected step, for the game's markers.
let previewTimer = 0;
function schedulePreview(now = false) {
  if (!QUEST_VIEW) return;
  clearTimeout(previewTimer);
  previewTimer = setTimeout(() => {
    if (!qe.quest) { bridge.send({ cmd: "questPreview", quest: null }); return; }
    const { quest } = buildQuest(qe.quest);
    quest.selected = qe.selected;
    bridge.send({ cmd: "questPreview", quest });
  }, now ? 0 : 300);
}
function selectStep(i) {
  if (i === qe.selected) return;
  qe.selected = i;
  $$("#q-steps .step").forEach((el) => el.classList.toggle("sel", +el.dataset.step === i));
  schedulePreview(true);
}
function lastSpawnItem(q) {
  for (const s of [...q.steps].reverse()) for (const r of [...s.spawn].reverse()) if (r.item.trim()) return r.item.trim();
  return "";
}
// A pick from the game (F6/F7/F9 or an armed left click) goes into the selected step.
function applyPick(p) {
  const q = qe.quest, i = qe.selected, s = q?.steps[i];
  if (!q) { toast(t("toast.noQuest"), true); return; }
  if (!s) { toast(t("toast.noStep"), true); return; }
  const n = i + 1;
  const at = (o) => { for (const k of ["x", "y", "z"]) o[k] = String(Math.round(p[k])); };
  if ((p.kind === "point" || p.kind === "item") && typeof p.x !== "number") { toast(t("toast.noCoords"), true); return; }
  let focus = "";
  if (p.kind === "point") {
    if (s.type === "collect") {
      at(s.area);
      if (!s.area.radius.trim()) s.area.radius = "5";
      toast(t("toast.area", { n }));
    } else {
      at(s);
      toast(t("toast.point", { n }));
    }
  } else if (p.kind === "object" || p.kind === "item") {
    if (s.type !== "collect") { toast(t("toast.needCollect"), true); return; }
    if (p.kind === "object") {
      if (!p.id) { toast(t("toast.noName"), true); return; }
      if (s.ids.includes(p.id)) { toast(t("toast.inStep", { id: p.id, n })); return; }
      s.ids.push(p.id);
      toast(t("toast.object", { id: p.id, n }));
    } else {
      const row = { item: lastSpawnItem(q), count: "", x: "", y: "", z: "" };
      at(row);
      s.spawn.push(row);
      if (!row.item) focus = `[data-step="${i}"] [data-f="spawn.${s.spawn.length - 1}.item"]`;
      toast(t("toast.item", { n }));
    }
  } else return;
  renderSteps();
  markDirty();
  if (focus) $(focus)?.focus();
}
// Arms a pick: the game takes the next left click (DGMP.exe writes editor_pick_mode.json).
function armPick(kind) {
  if (qe.arm) clearTimeout(qe.arm.timer);
  qe.arm = { kind, timer: 0 };
  bridge.send({ cmd: "pickMode", kind });
  renderArm();
}
function cancelArm() {
  if (qe.arm) clearTimeout(qe.arm.timer);
  qe.arm = null;
  bridge.send({ cmd: "pickMode", kind: "" });
  renderArm();
}
function renderArm() {
  $("#q-arm").hidden = !qe.arm;
  if (qe.arm) $("#q-arm-text").textContent = `${t("arm.text")} (${PICK_KINDS[qe.arm.kind]})`;
}

// ---- actions ----
function openQuestsPage() {
  bridge.send({ cmd: "questList" });
  bridge.send({ cmd: "telemetry" });
  if (!qe.opened) { qe.opened = true; bridge.send({ cmd: "itemKeys" }); }
}
function markDirty() { qe.dirty = true; renderQuestFile(); schedulePreview(); }
function discardChanges() { return !qe.dirty || confirm(t("confirm.discard")); }

function newQuest() {
  if (!discardChanges()) return;
  let n = 1;
  while (qe.list.some((q) => q.id === `quest_${n}`)) n++;
  qe.quest = questModel({ id: `quest_${n}`, steps: [{ type: "goto" }] });
  qe.selected = 0;
  qe.loadedId = null;
  qe.dirty = true;
  renderQuestForm(); renderQuestList();
  $("#q-title").focus();
}
function deleteQuest() {
  const q = qe.quest;
  if (!q) return;
  if (qe.loadedId === null) {
    if (!discardChanges()) return;
  } else {
    if (!confirm(t("confirm.delete", { title: q.title || qe.loadedId, id: qe.loadedId }))) return;
    bridge.send({ cmd: "questDelete", id: qe.loadedId });
  }
  qe.quest = null; qe.loadedId = null; qe.dirty = false; qe.selected = -1;
  renderQuestForm(); renderQuestList();
}
function saveQuest(test) {
  if (!qe.quest) return;
  const { quest, errors } = buildQuest(qe.quest);
  if (errors.length) { setStatus(errors[0] + (errors.length > 1 ? t("err.more", { n: errors.length - 1 }) : ""), false, true); return; }
  if (test && !nickValid()) { setStatus(t("err.nick"), false, true); $("#nick").focus(); return; }
  qe.closeEditor = test && app.gameRunning && app.gameEditor;
  if (qe.closeEditor && !confirm(t("confirm.closeEditor"))) return;
  qe.pendingTest = test;
  bridge.send({ cmd: "questSave", oldId: qe.loadedId || "", quest });
}

// Fills x/y/z at `prefix` of `base` from the game: the player ("me") or the aimed object ("aim").
function fillFromGame(base, prefix, source) {
  const tl = qe.tele;
  if (!tl.live) { setStatus(t("err.noPosition"), false, true); return false; }
  const p = source === "aim" ? tl.aim : tl;
  if (!p || typeof p.x !== "number") { setStatus(t("err.noAim"), false, true); return false; }
  for (const k of ["x", "y", "z"]) setPath(base, prefix + k, String(Math.round(p[k])));
  return true;
}

function onQuestInput(e) {
  const t = e.target;
  if (!qe.quest) return;
  const stepEl = t.closest("[data-step]");
  const step = stepEl ? qe.quest.steps[+stepEl.dataset.step] : null;
  if (t.dataset.q) qe.quest[t.dataset.q] = t.value;
  else if (t.dataset.f) setPath(step || qe.quest, t.dataset.f, t.type === "checkbox" ? t.checked : t.value);
  else if (t.dataset.event && step) step.event = $$(`[data-step="${stepEl.dataset.step}"] [data-event]`).filter((c) => c.checked).map((c) => c.dataset.event);
  else if (t.matches("[data-step-type]") && step) {
    if (e.type !== "change") return;
    step.type = t.value;
    step.route = step.type === "goto";
    renderSteps();
  } else return;
  markDirty();
}

function onQuestClick(e) {
  const b = e.target.closest("button");
  if (!b || !qe.quest) return;
  const q = qe.quest;
  const stepEl = b.closest("[data-step]");
  const i = stepEl ? +stepEl.dataset.step : -1;
  const step = q.steps[i];
  const base = step || q;
  if (b.dataset.arm) {
    armPick(b.dataset.arm);
    return;
  } else if (b.dataset.addStep) {
    q.steps.push(stepModel({ type: b.dataset.addStep }));
    qe.selected = q.steps.length - 1;
    renderSteps();
    $("#q-steps").lastElementChild.scrollIntoView({ block: "nearest" });
  } else if (b.dataset.act) {
    const j = b.dataset.act === "up" ? i - 1 : i + 1;
    if (b.dataset.act === "remove") {
      q.steps.splice(i, 1);
      if (qe.selected > i || qe.selected >= q.steps.length) qe.selected--;
    } else if (j >= 0 && j < q.steps.length) {
      [q.steps[i], q.steps[j]] = [q.steps[j], q.steps[i]];
      qe.selected = qe.selected === i ? j : qe.selected === j ? i : qe.selected;
    }
    renderSteps();
  } else if (b.dataset.pos !== undefined) {
    if (!fillFromGame(base, b.dataset.target, b.dataset.pos)) return;
    step ? renderSteps() : renderReward();
  } else if (b.dataset.del) {
    const path = b.dataset.del.split(".");
    const index = +path.pop();
    getPath(base, path.join(".")).splice(index, 1);
    step ? renderSteps() : renderReward();
  } else if (b.dataset.add === "ids") {
    step.ids.push("");
    renderSteps();
    $(`[data-step="${i}"] [data-f="ids.${step.ids.length - 1}"]`).focus();
  } else if (b.dataset.add === "aim-id") {
    const aim = qe.tele.live ? qe.tele.aim : null;
    if (!aim?.id) { setStatus(t(qe.tele.live ? "err.aimObject" : "err.notRunning"), false, true); return; }
    if (aim.object === false) { setStatus(t("err.aimKind"), false, true); return; }
    const id = aim.id;
    if (step.ids.includes(id)) { setStatus(t("err.inList", { id })); return; }
    step.ids.push(id);
    renderSteps();
  } else if (b.dataset.add === "spawn") {
    step.spawn.push({ item: "", count: "", x: "", y: "", z: "" });
    renderSteps();
  } else if (b.id === "q-reward-add") {
    q.reward.push({ item: "", count: "" });
    renderReward();
    $(`[data-f="reward.${q.reward.length - 1}.item"]`).focus();
  } else return;
  markDirty();
}

// ---------------- language switch ----------------
function storedLang() { try { return localStorage.getItem("dgmp.lang") || "en"; } catch { return "en"; } }
// save: the user picked it here (DGMP.exe keeps it in launcher.json and tells the other window).
function setLang(l, save = false) {
  lang = I18N[l] ? l : "en";
  try { localStorage.setItem("dgmp.lang", lang); } catch { /* preview without storage */ }
  if (save) bridge.send({ cmd: "setLang", lang });
  document.documentElement.lang = lang;
  buildLabels();
  for (const el of $$("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of $$("[data-i18n-html]")) el.innerHTML = t(el.dataset.i18nHtml);
  for (const el of $$("[data-i18n-placeholder]")) el.placeholder = t(el.dataset.i18nPlaceholder);
  for (const el of $$("[data-i18n-title]")) el.title = t(el.dataset.i18nTitle);
  $$("[data-lang]").forEach((b) => b.classList.toggle("active", b.dataset.lang === lang));
  // Text built by the code is rebuilt; form values stay as they are.
  if (app.state) { renderVersion(); renderServers(); renderServerState(); describeHost(); }
  fillQuestSelects();
  renderQuestList();
  if (qe.quest) { renderQuestFile(); renderReward(); renderSteps(); }
  renderTelemetry();
  renderArm();
}
function renderVersion() { $("#version").textContent = t("version", { v: app.state.version, p: app.state.protocol }); }
function fillQuestSelects() {
  for (const [id, labels] of [["#q-category", QUEST_CATEGORIES], ["#q-style", QUEST_STYLES]]) {
    const select = $(id), value = select.value;
    select.innerHTML = Object.entries(labels).map(([v, label]) => `<option value="${v}">${label}</option>`).join("");
    if (value) select.value = value;
  }
}

// ---------------- messages from DGMP.exe ----------------
bridge.listen((m) => {
  switch (m.type) {
    case "state": {
      const first = !app.state;
      if (first) setLang(m.lang);
      app.state = m;
      if (first) {
        $("#nick").value = m.nick || "";
        validateNick();
        renderVersion();
        app.gameRunning = m.gameRunning;
        app.gameEditor = !!m.gameEditor;
        $("#game-pill").hidden = !m.gameRunning;
        setServerRunning(m.serverRunning, m.serverPort);
        fillHost();
        if (!m.gameFound) setStatus(t("err.noGame"), false, true);
        if (!QUEST_VIEW) refresh();
      } else renderServers();
      break;
    }
    case "query":
      for (const r of m.results) app.info[r.address] = r;
      if (m.tag === app.list) renderServers();
      break;
    case "lan":
      app.querying = false;
      app.lan = m.results;
      for (const r of m.results) app.info[r.address] = r;
      if (app.list === "lan") renderServers();
      break;
    case "nickResult":
      $("#nick-state").textContent = m.ok ? "✓" : "✕";
      $("#nick-state").className = "nick-state " + (m.ok ? "ok" : "bad");
      break;
    case "status": setStatus(m.text, m.busy, m.error); break;
    case "lang": if (m.lang !== lang) setLang(m.lang); break;  // switched in the other window
    case "game":
      app.gameRunning = m.running;
      app.gameEditor = m.running && !!m.editor;
      $("#game-pill").hidden = !m.running;
      renderDetail();
      break;
    case "server": setServerRunning(m.running, m.port); $("#console").textContent = ""; break;
    case "serverLog": consoleLine(m.line); break;
    case "serverExit":
      setServerRunning(false, 0);
      consoleLine(t("srv.exit", { code: m.code }));
      break;
    case "hostReady": connect(m.address, m.password); break;
    case "questList":
      qe.list = m.quests;
      renderQuestList();
      break;
    case "quest":
      qe.quest = questModel(m.quest);
      qe.loadedId = m.id;
      qe.dirty = false;
      qe.selected = qe.quest.steps.length ? 0 : -1;
      renderQuestForm(); renderQuestList();
      break;
    case "questSaved":
      if (m.ok && qe.quest) {
        qe.loadedId = m.id;
        qe.dirty = false;
        renderQuestFile();
        if (qe.pendingTest) bridge.send({ cmd: "questTest", id: m.id, closeEditor: qe.closeEditor });
      }
      qe.pendingTest = false;
      break;
    case "itemKeys": $("#item-keys").innerHTML = m.keys.map((k) => `<option value="${esc(k)}">`).join(""); break;
    case "telemetry": qe.tele = m; renderTelemetry(); break;
    case "pick":
      applyPick(m);
      // An armed click came back: wait for the next one ("Повторять") or stop waiting.
      if (qe.arm && qe.arm.kind === m.kind) {
        if ($("#q-arm-repeat").checked) armPick(m.kind);
        else { clearTimeout(qe.arm.timer); qe.arm = null; renderArm(); }
      }
      break;
    case "pickModeEnd":
      // The game took (or dropped) the click; its pick normally follows within one poll.
      if (qe.arm) qe.arm.timer = setTimeout(() => { qe.arm = null; renderArm(); }, 1000);
      break;
  }
});

// ---------------- events ----------------
let nickTimer = 0;
function validateNick() {
  const ok = nickValid();
  const empty = !$("#nick").value.trim();
  $("#nick-help").classList.toggle("bad", !ok && !empty);
  $("#nick-state").textContent = empty ? "" : ok ? "" : "✕";
  $("#nick-state").className = "nick-state " + (ok ? "ok" : "bad");
  return ok;
}
$("#nick").addEventListener("input", () => {
  clearTimeout(nickTimer);
  if (validateNick()) nickTimer = setTimeout(() => bridge.send({ cmd: "setNick", nick: $("#nick").value.trim() }), 400);
});

$$(".nav-item").forEach((b) => b.addEventListener("click", () => selectPage(b.dataset.page)));
$$(".tab").forEach((t) => t.addEventListener("click", () => {
  $$(".tab").forEach((x) => x.classList.toggle("active", x === t));
  app.list = t.dataset.list; app.selected = null;
  renderServers(); refresh();
}));
$$("[data-lang]").forEach((b) => b.addEventListener("click", () => b.dataset.lang !== lang && setLang(b.dataset.lang, true)));
$$("[data-open]").forEach((b) => b.addEventListener("click", () => bridge.send({ cmd: "openFolder", which: b.dataset.open })));

$("#refresh").addEventListener("click", refresh);
$("#direct").addEventListener("click", () => { const a = $("#address").value.trim(); if (a) connect(a); });
$("#address").addEventListener("keydown", (e) => { if (e.key === "Enter") $("#direct").click(); });
$("#add-fav").addEventListener("click", () => {
  const a = $("#address").value.trim();
  if (!a) return $("#address").focus();
  bridge.send({ cmd: "favorite", address: a, add: true, name: "" });
  bridge.send({ cmd: "query", tag: "favorites", addresses: [a] });
  $("#address").value = "";
  if (app.list !== "favorites") $$(".tab")[0].click();
});

$("#server-rows").addEventListener("click", (e) => {
  const star = e.target.closest("[data-star]");
  if (star) { toggleFavorite(star.dataset.star); return; }
  const row = e.target.closest("tr[data-address]");
  if (!row) return;
  app.selected = row.dataset.address;
  renderServers();
});
$("#server-rows").addEventListener("dblclick", (e) => {
  const row = e.target.closest("tr[data-address]");
  if (row) connect(row.dataset.address);
});
$("#d-play").addEventListener("click", () => app.selected && connect(app.selected));
$("#d-fav").addEventListener("click", () => app.selected && toggleFavorite(app.selected));

$("#pass-form").addEventListener("submit", (e) => {
  e.preventDefault();
  $("#pass-modal").hidden = true;
  connect(app.pendingConnect, $("#pass-input").value);
});
$("#pass-cancel").addEventListener("click", () => ($("#pass-modal").hidden = true));

$("#h-mode").addEventListener("change", describeMode);
$("#host-form").addEventListener("submit", (e) => {
  e.preventDefault();
  if (!nickValid()) { setStatus(t("err.nick"), false, true); $("#nick").focus(); return; }
  bridge.send({ cmd: "host", config: hostConfig(), play: true });
});
$("#h-only").addEventListener("click", () => bridge.send({ cmd: "host", config: hostConfig(), play: false }));
$("#srv-stop").addEventListener("click", () => bridge.send({ cmd: "stopServer" }));
$("#console-form").addEventListener("submit", (e) => {
  e.preventDefault();
  const line = $("#console-line").value.trim();
  if (!line) return;
  consoleLine("> " + line);
  bridge.send({ cmd: "serverCommand", line });
  $("#console-line").value = "";
});
$("#srv-addr").addEventListener("click", (e) => {
  const b = e.target.closest("[data-copy]");
  if (!b) return;
  bridge.send({ cmd: "copy", text: b.dataset.copy });
  b.textContent = t("srv.copied"); setTimeout(() => (b.textContent = t("srv.copy")), 1200);
});

$("#e-run").addEventListener("click", () => bridge.send({ cmd: "editor", refreshSeed: $("#e-refresh").checked }));

$("#q-new").addEventListener("click", newQuest);
$("#q-list").addEventListener("click", (e) => {
  const item = e.target.closest("[data-qid]");
  if (!item || item.dataset.qid === qe.loadedId || !discardChanges()) return;
  qe.dirty = false;
  bridge.send({ cmd: "questLoad", id: item.dataset.qid });
});
$("#q-form").addEventListener("input", onQuestInput);
$("#q-form").addEventListener("change", onQuestInput);
$("#q-form").addEventListener("click", onQuestClick);
// Clicking or tabbing into a step card selects it.
$("#q-steps").addEventListener("mousedown", (e) => { const el = e.target.closest("[data-step]"); if (el) selectStep(+el.dataset.step); });
$("#q-steps").addEventListener("focusin", (e) => { const el = e.target.closest("[data-step]"); if (el) selectStep(+el.dataset.step); });
$("#q-arm-cancel").addEventListener("click", cancelArm);
$("#q-form").addEventListener("submit", (e) => { e.preventDefault(); saveQuest(false); });
$("#q-test").addEventListener("click", () => saveQuest(true));
$("#q-delete").addEventListener("click", deleteQuest);

document.addEventListener("keydown", (e) => {
  const page = $(".page.active").id;
  if (e.key === "F5") { e.preventDefault(); page === "page-quests" ? bridge.send({ cmd: "questList" }) : refresh(); }
  if (e.key === "Escape") $("#pass-modal").hidden = true;
  if (e.key === "Delete" && page === "page-servers" && app.selected && app.list === "favorites" && document.activeElement.tagName !== "INPUT") toggleFavorite(app.selected);
  if (e.key === "s" && e.ctrlKey && page === "page-quests") { e.preventDefault(); saveQuest(false); }
});
// Keep the visible list fresh.
setInterval(() => { if (!document.hidden && $("#page-servers").classList.contains("active") && app.list !== "lan") refresh(); }, 30000);
// Player position and aimed object for the quest editor, while its page is open.
setInterval(() => { if (!document.hidden && $("#page-quests").classList.contains("active")) bridge.send({ cmd: "telemetry" }); }, 1000);

// DGMP.exe sends the saved language with the state; the last choice in this profile avoids a flash before that.
setLang(storedLang());
if (QUEST_VIEW) { document.body.classList.add("quests-view"); selectPage("quests"); }
bridge.send({ cmd: "init" });
