# Квесты: формат файла и запуск

Квест — файл `quests/<id>.json` в папке DGMP (UTF-8). `id` — латиница, цифры, `_` и `-`. Файл создаёт редактор квестов в лаунчере, но его можно писать и руками.

```json
{
  "id": "supplies",
  "title": "Припасы для лагеря",
  "category": "camp",
  "style": "basic",
  "text": "Староста просит проверить старую заправку.",
  "xp": 150,
  "reward": [ { "item": "kConsumableID_Bandage_Small", "count": 2 } ],
  "steps": [
    { "type": "goto", "text": "Доберитесь до заправки", "x": -79464, "y": -301637, "z": -5250, "radius": 6, "route": true },
    { "type": "hold", "text": "Обыщите место", "x": -79464, "y": -301637, "z": -5250, "radius": 8, "seconds": 10, "meter": "Поиск" },
    { "type": "collect", "text": "Обыщите машины", "event": "search", "count": 2,
      "ids": ["BP_Interact_SearchCarTrunk21", "BP_Interact_SearchAmbulance_74"], "marker": true },
    { "type": "collect", "text": "Соберите коктейли Молотова", "event": "pickup",
      "spawn": [ { "item": "kWeaponID_Molotov_Small", "count": 1, "x": -85160, "y": -302467, "z": -5249 } ] },
    { "type": "collect", "text": "Сожгите улики", "event": "fire",
      "area": { "x": -85000, "y": -302000, "z": -5250, "radius": 4 }, "marker": true }
  ]
}
```

## Поля квеста

| Поле | Описание |
|---|---|
| `title` | название (панель задания, экран «Задание выполнено») |
| `category` | `story`, `camp`, `infestation`, `ambush`, `nero`, `horde`, `campmission`, `task` — иконка и цвет |
| `style` | вид маркеров: `basic`, `kill`, `killarea`, `defend`, `defendarea`, `search`, `track`, `nokill` |
| `text` | строка на плашке «Новое задание» |
| `xp` | опыт на экране завершения |
| `reward` | предметы, выдаваемые при завершении |
| `steps` | шаги по порядку |

## Шаги

Координаты — единицы игры (100 = 1 м), как в `telemetry.json` и `/pos`. Радиусы — в метрах.

| `type` | Поля | Когда выполнен |
|---|---|---|
| `goto` | `text, x, y, z, radius = 5, route = true` | игрок дошёл до точки (квестовый маркер, маршрут) |
| `hold` | `text, x, y, z, radius = 6, seconds = 10, meter = "Поиск"` | пробыл в зоне `seconds` секунд (шкала растёт только внутри) |
| `collect` | `text, event, count, item, class, ids, area, marker, route, spawn` | случилось `count` игровых событий, подходящих под фильтры |

Фильтры `collect` — как в `q:collect` (`docs/API.md`):
- `event` — `pickup`, `carry`, `search`, `body`, `interact`, `door`, `explosion`, `fire` или список;
- `item` — ключ предмета или список;
- `class` — часть имени класса объекта;
- `ids` — конкретные объекты мира (их имена);
- `area` — `{x, y, z, radius}`;
- `marker` — маркер над зоной;
- `spawn` — предметы, которые шаг раскладывает сам (с маркерами; засчитываются только они; `count` по умолчанию = их число).

## Запуск из режима

```lua
local Quest = require("quest")
local def = Quest.load("supplies")          -- quests/supplies.json или nil
Quest.run(player, def, function(player) end) -- шаги по порядку, награда и опыт в конце
```

`Quest.list()` — список id всех квестов.

## Проверка из лаунчера

Кнопка «Проверить» сохраняет квест, запускает локальный сервер с режимом `questtest` и игру. Режим выдаёт квест каждому вошедшему игроку (консоль сервера: `quest <id>` — сменить квест, `quest` — выдать заново).

## Данные из игры для редактора

Игра (и в редакторе, и в мультиплеере) раз в секунду пишет `telemetry.json` в папку DGMP. Для редактора квестов в нём есть:
- `x, y, z, yaw` — позиция игрока;
- `aim` — объект под прицелом (до 50 м) или `null`: `{ "id": "BP_Interact_SearchCarTrunk21", "class": "BP_Interact_SearchCarTrunk_C", "x": .., "y": .., "z": .. }`. `id` подходит для `ids`, координаты — для `area`, `spawn`, `goto`.
