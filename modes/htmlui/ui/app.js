// The page's side of main.lua: dgmp.on(name, fn) gets player:uiSend(name, data), dgmp.send(name, data)
// reaches on("uiMessage").
const $ = (id) => document.getElementById(id);
let clicks = 0;

dgmp.on('status', (s) => {
  $('who').textContent = s.name;
  $('clock').textContent = s.time;
  $('online').textContent = s.players;
});

dgmp.on('phone', (p) => {
  $('phone').classList.toggle('hidden', !p.open);
  if (!p.open) return;
  const list = $('players');
  list.replaceChildren();
  for (const pl of p.players || []) {
    const li = document.createElement('li');
    li.textContent = pl.name;
    const sms = document.createElement('button');
    sms.textContent = 'SMS';
    sms.onclick = () => {
      const text = $('sayText').value.trim() || 'Hello from the phone!';
      dgmp.send('sms', { to: pl.id, text });
    };
    li.append(sms);
    list.append(li);
  }
  $('sayText').focus();
});

dgmp.on('pong', (p) => { $('pong').textContent = `Lua answered: click ${p.clicks}, server time ${p.at.toFixed(1)} s`; });

let toastTimer = 0;
dgmp.on('sms', (m) => {
  $('toast').textContent = `SMS from ${m.from}: ${m.text}`;
  $('toast').classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('toast').classList.add('hidden'), 6000);
});

// The page learns when it gets or loses the mouse and keyboard.
dgmp.on('focus', (on) => console.log('focus', on));

$('ping').onclick = () => dgmp.send('ping', { clicks: ++clicks });
$('close').onclick = () => dgmp.send('close');
$('say').onsubmit = (e) => {
  e.preventDefault();
  const text = $('sayText').value.trim();
  if (text) dgmp.send('say', { text });
  $('sayText').value = '';
};
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && dgmp.focused) dgmp.send('close'); });
