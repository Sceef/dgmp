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
      if (m.cmd === "internet") later({ type: "internet", results: [{ address: "95.31.4.18:7777", ...sample["95.31.4.18:7777"] }] }, 700);
      if (m.cmd === "setNick") later({ type: "nickResult", ok: true, nick: m.nick }, 10);
      if (m.cmd === "connect") later({ type: "status", text: t("mock.launch"), busy: true });
    },
  };
})();

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const NICK_RE = /^[\p{L}\p{N}_\-.\[\]()]{3,24}$/u;
// ---------------- language ----------------
// English by default; the choice is kept by DGMP.exe (launcher.json "lang") and comes with the state message.
// Static text: data-i18n (text), data-i18n-html, data-i18n-placeholder, data-i18n-title. "{name}" = parameter.
const I18N = {
  en: {
    "lang.title": "Language",
    "nick.label": "Nickname", "nick.placeholder": "Enter a nickname", "nick.help": "3–24 characters: letters, digits, _ - . [ ] ( )",
    "nav.servers": "Servers", "nav.host": "Host a server", "nav.editor": "Editor",
    "foot.modes": "Modes folder", "foot.logs": "Logs", "version": "Version {v} · protocol {p}",
    "list.favorites": "Favorites", "list.recent": "Recent", "list.lan": "LAN", "list.internet": "Internet",
    "address.placeholder": "IP:port, e.g. 192.168.1.20:7777", "fav.add": "Add to favorites", "fav.remove": "Remove from favorites",
    "connect": "Connect", "refresh": "Refresh (F5)", "play": "Play",
    "th.name": "Name", "mode": "Mode", "th.players": "Players", "th.ping": "Ping",
    "copy.title": "Copy the address", "copy.done": "Address {a} copied",
    "ping.ms": "{n} ms", "ping.none": "no response", "tag.version": "other version",
    "empty.favorites": "No favorites yet.<br>Enter a server address above and press “Add to favorites”.",
    "empty.recent": "Servers you've played on will show up here.",
    "empty.lanSearch": "Searching the local network…", "empty.lan": "No LAN servers found.<br>Ports 7777–7786.",
    "empty.internetSearch": "Loading the server list…", "empty.internet": "No public servers right now.",
    "empty.internetError": "The server list is unavailable: {e}.",
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
      + "Quests and jobs are made in the game's own editor windows. Story, saves and photo mode are disabled.",
    "editor.refresh": "Restore the original world template before launch", "editor.run": "Open editor",
    "status.ready": "Ready", "game.running": "Game running",
    "pass.title": "Password required", "cancel": "Cancel", "update.available": "Version {v} is out — download",
    "mock.launch": "Launching Days Gone…",
  },
  ru: {
    "lang.title": "Язык",
    "nick.label": "Ник", "nick.placeholder": "Введите ник", "nick.help": "3–24 символа: буквы, цифры, _ - . [ ] ( )",
    "nav.servers": "Серверы", "nav.host": "Создать сервер", "nav.editor": "Редактор",
    "foot.modes": "Папка режимов", "foot.logs": "Логи", "version": "Версия {v} · протокол {p}",
    "list.favorites": "Избранное", "list.recent": "Недавние", "list.lan": "Локальная сеть", "list.internet": "Интернет",
    "address.placeholder": "IP:порт, например 192.168.1.20:7777", "fav.add": "В избранное", "fav.remove": "Убрать из избранного",
    "connect": "Подключиться", "refresh": "Обновить (F5)", "play": "Играть",
    "th.name": "Название", "mode": "Режим", "th.players": "Игроки", "th.ping": "Пинг",
    "copy.title": "Скопировать адрес", "copy.done": "Адрес {a} скопирован",
    "ping.ms": "{n} мс", "ping.none": "нет ответа", "tag.version": "другая версия",
    "empty.favorites": "В избранном пока пусто.<br>Введите адрес сервера сверху и нажмите «В избранное».",
    "empty.recent": "Здесь появятся серверы, на которых вы играли.",
    "empty.lanSearch": "Поиск серверов в локальной сети…", "empty.lan": "Серверы в локальной сети не найдены.<br>Порты 7777–7786.",
    "empty.internetSearch": "Загрузка списка серверов…", "empty.internet": "Сейчас нет открытых серверов.",
    "empty.internetError": "Список серверов недоступен: {e}.",
    "detail.players": "{n}/{max} игроков", "detail.password": "пароль", "detail.incompatible": "несовместимая версия",
    "detail.offline": "сервер не отвечает", "update.available": "Вышла версия {v} — скачать",
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
      + "Квесты и работы создаются в окнах редактора внутри игры. Сюжет, сохранения и фоторежим отключены.",
    "editor.refresh": "Восстановить исходный шаблон мира перед запуском", "editor.run": "Открыть редактор",
    "status.ready": "Готово", "game.running": "Игра запущена",
    "pass.title": "Сервер защищён паролем", "cancel": "Отмена",
    "mock.launch": "Запуск Days Gone…",
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
  internet: [],      // listed by the master server
  internetError: "",
  selected: null,
  querying: false,
  serverRunning: false,
  serverPort: 0,
  gameRunning: false,
  pendingConnect: null,
};

// ---------------- rendering ----------------
function entries() {
  if (app.list === "lan" || app.list === "internet") return app[app.list].map((r) => ({ address: r.address, name: r.name }));
  return (app.state?.[app.list] || []);
}
function isFavorite(address) { return (app.state?.favorites || []).some((f) => f.address === address); }

// The address as a button: a click copies it (the row is not selected by it).
const COPY_ICON = `<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="5" y="5" width="9" height="9" rx="1.5"/><path d="M11 5V3.5A1.5 1.5 0 0 0 9.5 2h-6A1.5 1.5 0 0 0 2 3.5v6A1.5 1.5 0 0 0 3.5 11H5"/></svg>`;
function copyButton(address) {
  return `<button class="copy" data-copy="${esc(address)}" title="${t("copy.title")}">${esc(address)}${COPY_ICON}</button>`;
}
async function copyAddress(button) {
  const a = button.dataset.copy;
  try { await navigator.clipboard.writeText(a); }
  catch {
    const area = Object.assign(document.createElement("textarea"), { value: a });
    document.body.append(area); area.select(); document.execCommand("copy"); area.remove();
  }
  setStatus(t("copy.done", { a }));
  button.classList.add("done");
  setTimeout(() => button.classList.remove("done"), 1200);
}
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
      <td><span class="srv-name">${esc(name)}</span>${tags}<span class="srv-addr">${copyButton(e.address)}</span></td>
      <td class="c-mode">${online ? esc(i.mode) : ""}</td>
      <td class="c-players">${players}</td>
      <td class="c-ping ${i ? pingClass(i.ping) : "ping-none"}">${ping}</td>
      <td class="c-act"><button class="star ${isFavorite(e.address) ? "on" : ""}" data-star="${esc(e.address)}" title="${t("list.favorites")}">★</button></td>
    </tr>`;
  }).join("");
  const empty = $("#empty");
  empty.hidden = rows.length > 0;
  empty.innerHTML = app.list === "internet"
    ? (app.querying ? t("empty.internetSearch") : app.internetError ? t("empty.internetError", { e: esc(app.internetError) }) : t("empty.internet"))
    : t(app.list === "favorites" ? "empty.favorites" : app.list === "recent" ? "empty.recent" : app.querying ? "empty.lanSearch" : "empty.lan");
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
  $("#d-meta").innerHTML = copyButton(address) + esc(online
    ? ` · ${i.mode} · ${t("detail.players", { n: i.players, max: i.maxPlayers })} · ${t("ping.ms", { n: i.ping })}`
      + (i.password ? " · " + t("detail.password") : "") + (i.compatible ? "" : " · " + t("detail.incompatible"))
    : ` · ${t("detail.offline")}`);
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
  if (app.list === "lan" || app.list === "internet") { app.querying = true; app[app.list] = []; renderServers(); bridge.send({ cmd: app.list }); return; }
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

// ---------------- language switch ----------------
function storedLang() { try { return localStorage.getItem("dgmp.lang") || "en"; } catch { return "en"; } }
// save: the user picked it here (DGMP.exe keeps it in launcher.json).
function setLang(l, save = false) {
  lang = I18N[l] ? l : "en";
  try { localStorage.setItem("dgmp.lang", lang); } catch { /* preview without storage */ }
  if (save) bridge.send({ cmd: "setLang", lang });
  document.documentElement.lang = lang;
  for (const el of $$("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of $$("[data-i18n-html]")) el.innerHTML = t(el.dataset.i18nHtml);
  for (const el of $$("[data-i18n-placeholder]")) el.placeholder = t(el.dataset.i18nPlaceholder);
  for (const el of $$("[data-i18n-title]")) el.title = t(el.dataset.i18nTitle);
  $$("[data-lang]").forEach((b) => b.classList.toggle("active", b.dataset.lang === lang));
  // Text built by the code is rebuilt; form values stay as they are.
  if (app.state) { renderVersion(); renderServers(); renderServerState(); describeHost(); }
}
function renderVersion() { $("#version").textContent = t("version", { v: app.state.version, p: app.state.protocol }); }

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
        $("#game-pill").hidden = !m.gameRunning;
        setServerRunning(m.serverRunning, m.serverPort);
        fillHost();
        if (!m.gameFound) setStatus(t("err.noGame"), false, true);
        refresh();
      } else renderServers();
      break;
    }
    case "query":
      for (const r of m.results) app.info[r.address] = r;
      if (m.tag === app.list) renderServers();
      break;
    case "lan":
    case "internet":
      if (app.list === m.type) app.querying = false;
      app[m.type] = m.results;
      if (m.type === "internet") app.internetError = m.error || "";
      for (const r of m.results) app.info[r.address] = r;
      if (app.list === m.type) renderServers();
      break;
    case "update": {
      // A newer DGMP (launcher.json "updateUrl" manifest): a link to its download page.
      if (!(m.url || "").startsWith("https://")) break;
      const u = $("#update");
      u.href = m.url;
      u.textContent = t("update.available", { v: m.version });
      u.title = m.notes || "";
      u.hidden = false;
      break;
    }
    case "nickResult":
      $("#nick-state").textContent = m.ok ? "✓" : "✕";
      $("#nick-state").className = "nick-state " + (m.ok ? "ok" : "bad");
      break;
    case "status": setStatus(m.text, m.busy, m.error); break;
    case "game":
      app.gameRunning = m.running;
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
  const copy = e.target.closest("[data-copy]");
  if (copy) { copyAddress(copy); return; }
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
$("#d-meta").addEventListener("click", (e) => { const copy = e.target.closest("[data-copy]"); if (copy) copyAddress(copy); });
$("#server-rows").addEventListener("dblclick", (e) => { if (e.target.closest("[data-copy]")) e.stopPropagation(); }, true);
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

document.addEventListener("keydown", (e) => {
  const page = $(".page.active").id;
  if (e.key === "F5") { e.preventDefault(); refresh(); }
  if (e.key === "Escape") $("#pass-modal").hidden = true;
  if (e.key === "Delete" && page === "page-servers" && app.selected && app.list === "favorites" && document.activeElement.tagName !== "INPUT") toggleFavorite(app.selected);
});
// Keep the visible list fresh.
setInterval(() => { if (!document.hidden && $("#page-servers").classList.contains("active") && app.list !== "lan") refresh(); }, 30000);

// DGMP.exe sends the saved language with the state; the last choice in this profile avoids a flash before that.
setLang(storedLang());
bridge.send({ cmd: "init" });
