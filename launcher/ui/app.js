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
      if (m.cmd === "init") later({ type: "state", nick: "Sceef", version: "0.6.0", protocol: 1, gameFound: true, serverAvailable: true,
        favorites: [{ address: "127.0.0.1:7777", name: "Фарвелл Фрирум" }, { address: "95.31.4.18:7777", name: "[RU] Орда — выживание" }, { address: "10.0.0.5:7777", name: "Старый сервер" }],
        recent: [{ address: "127.0.0.1:7777", name: "Фарвелл Фрирум" }], host: { name: "Мой сервер", mode: "freeroam", port: 7777, maxPlayers: 16 },
        modes: [{ id: "freeroam", name: "Freeroam", description: "Свободная езда, чат, телепорты, время и погода." }],
        addresses: [{ ip: "192.168.1.20", adapter: "Ethernet" }] }, 50);
      if (m.cmd === "query") later({ type: "query", tag: m.tag, results: m.addresses.map((a) => sample[a] ? { address: a, ...sample[a] } : { address: a, ping: -1 }) }, 400);
      if (m.cmd === "lan") later({ type: "lan", results: [{ address: "192.168.1.20:7777", ...sample["127.0.0.1:7777"] }] }, 600);
      if (m.cmd === "setNick") later({ type: "nickResult", ok: true, nick: m.nick }, 10);
      if (m.cmd === "connect") later({ type: "status", text: "Запуск Days Gone…", busy: true });
    },
  };
})();

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const NICK_RE = /^[\p{L}\p{N}_\-.\[\]()]{3,24}$/u;

const app = {
  state: null,
  list: "favorites",
  info: {},          // address -> last query result
  lan: [],
  selected: null,
  querying: false,
  serverRunning: false,
  gameRunning: false,
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
    const ping = !i ? "…" : online ? `${i.ping} мс` : "нет ответа";
    const tags = online && !i.compatible ? `<span class="tag">другая версия</span>` : "";
    return `<tr data-address="${esc(e.address)}" class="${online ? "" : "offline"} ${app.selected === e.address ? "sel" : ""}">
      <td class="c-lock">${online && i.password ? "🔒" : ""}</td>
      <td><span class="srv-name">${esc(name)}</span>${tags}<span class="srv-addr">${esc(e.address)}</span></td>
      <td class="c-mode">${online ? esc(i.mode) : ""}</td>
      <td class="c-players">${players}</td>
      <td class="c-ping ${i ? pingClass(i.ping) : "ping-none"}">${ping}</td>
      <td class="c-act"><button class="star ${isFavorite(e.address) ? "on" : ""}" data-star="${esc(e.address)}" title="Избранное">★</button></td>
    </tr>`;
  }).join("");
  const empty = $("#empty");
  empty.hidden = rows.length > 0;
  empty.innerHTML = app.list === "favorites" ? "В избранном пока пусто.<br>Введите адрес сервера сверху и нажмите «В избранное»."
    : app.list === "recent" ? "Здесь появятся серверы, на которых вы играли."
    : app.querying ? "Поиск серверов в локальной сети…" : "Серверы в локальной сети не найдены.<br>Порты 7777–7786.";
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
    ? `${address} · ${i.mode} · ${i.players}/${i.maxPlayers} игроков · ${i.ping} мс${i.password ? " · пароль" : ""}${i.compatible ? "" : " · несовместимая версия"}`
    : `${address} · сервер не отвечает`;
  $("#d-fav").textContent = isFavorite(address) ? "Убрать из избранного" : "В избранное";
  $("#d-play").disabled = app.gameRunning || (online && !i.compatible);
}

function setStatus(text, busy = false, error = false) {
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
  if (!nickValid()) { setStatus("Сначала введите корректный ник", false, true); $("#nick").focus(); return; }
  if (app.gameRunning) { setStatus("Игра уже запущена", false, true); return; }
  const i = app.info[address];
  if (i && i.ping >= 0 && !i.compatible) { setStatus("Версия сервера не совпадает с вашей", false, true); return; }
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
  $("#h-name").value = h.name || ($("#nick").value.trim() ? `Сервер ${$("#nick").value.trim()}` : "DGMP Server");
  $("#h-mode").innerHTML = modes.map((m) => `<option value="${esc(m.id)}">${esc(m.name)}${m.version ? " " + esc(m.version) : ""}</option>`).join("");
  if (h.mode && modes.some((m) => m.id === h.mode)) $("#h-mode").value = h.mode;
  $("#h-port").value = h.port || 7777;
  $("#h-max").value = h.maxPlayers || 16;
  describeMode();
  const can = app.state.serverAvailable && modes.length > 0;
  $("#h-play").disabled = $("#h-only").disabled = !can || app.serverRunning;
  if (!app.state.serverAvailable) $("#h-mode-desc").textContent = "dgmp-server.exe не найден рядом с лаунчером.";
  else if (!modes.length) $("#h-mode-desc").textContent = "В папке modes нет режимов.";
}
function describeMode() {
  const m = (app.state.modes || []).find((x) => x.id === $("#h-mode").value);
  $("#h-mode-desc").textContent = m ? [m.description, m.author && `Автор: ${m.author}`].filter(Boolean).join(" · ") : "";
}
function hostConfig() {
  return { name: $("#h-name").value.trim() || "DGMP Server", mode: $("#h-mode").value, port: +$("#h-port").value || 7777,
           maxPlayers: +$("#h-max").value || 16, password: $("#h-pass").value };
}
function setServerRunning(running, port) {
  app.serverRunning = running;
  $("#server-dot").hidden = !running;
  $("#srv-stop").hidden = !running;
  $("#console-line").disabled = !running;
  $("#srv-state").textContent = running ? `Работает на порту ${port}` : "Сервер не запущен";
  $("#srv-state").classList.toggle("on", running);
  const addr = $("#srv-addr");
  addr.hidden = !running;
  if (running) {
    const list = [{ ip: "127.0.0.1", adapter: "этот компьютер" }, ...(app.state.addresses || [])];
    addr.innerHTML = list.map((a) => `<span class="addr">${esc(a.ip)}:${port} <small>${esc(a.adapter)}</small><button data-copy="${esc(a.ip)}:${port}">копировать</button></span>`).join("");
  }
  if (app.state) fillHost();
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

// ---------------- messages from DGMP.exe ----------------
bridge.listen((m) => {
  switch (m.type) {
    case "state": {
      const first = !app.state;
      app.state = m;
      if (first) {
        $("#nick").value = m.nick || "";
        validateNick();
        $("#version").textContent = `Версия ${m.version} · протокол ${m.protocol}`;
        app.gameRunning = m.gameRunning;
        $("#game-pill").hidden = !m.gameRunning;
        setServerRunning(m.serverRunning, m.serverPort);
        fillHost();
        if (!m.gameFound) setStatus("Days Gone не найден: DGMP должен лежать в папке игры", false, true);
        refresh();
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
    case "game":
      app.gameRunning = m.running;
      $("#game-pill").hidden = !m.running;
      renderDetail();
      break;
    case "server": setServerRunning(m.running, m.port); $("#console").textContent = ""; break;
    case "serverLog": consoleLine(m.line); break;
    case "serverExit":
      setServerRunning(false, 0);
      consoleLine(`— сервер остановлен (код ${m.code}) —`);
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
  if (!nickValid()) { setStatus("Сначала введите корректный ник", false, true); $("#nick").focus(); return; }
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
  b.textContent = "скопировано"; setTimeout(() => (b.textContent = "копировать"), 1200);
});

$("#e-run").addEventListener("click", () => bridge.send({ cmd: "editor", refreshSeed: $("#e-refresh").checked }));

document.addEventListener("keydown", (e) => {
  if (e.key === "F5") { e.preventDefault(); refresh(); }
  if (e.key === "Escape") $("#pass-modal").hidden = true;
  if (e.key === "Delete" && app.selected && app.list === "favorites" && document.activeElement.tagName !== "INPUT") toggleFavorite(app.selected);
});
// Keep the visible list fresh.
setInterval(() => { if (!document.hidden && $("#page-servers").classList.contains("active") && app.list !== "lan") refresh(); }, 30000);

bridge.send({ cmd: "init" });
