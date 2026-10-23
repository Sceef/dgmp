#include "editorui.h"
#include "overlay.h"
#include "inventory.h"
#include "profile.h"
#include "log.h"
#include "shared/json.h"
#include "worldedit.h"
#include "environment.h"
#include "skins.h"
#include "carry.h"
#include "../api/dgmp.h"
#include "imgui.h"
#include "ImGuizmo.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace ue;
namespace fs = std::filesystem;

namespace editorui {
namespace {
// ---------------------------------------------------------------- language (editor.ini: English by default)
bool english = true;
std::atomic<bool> invertLook{true};  // editor camera: vertical look inverted (editor.ini InvertY)
const char* T(const char* ru) {
    static const std::map<std::string, const char*> en = {
    {"Дойти до точки", "Go to a point"},
    {"Удерживать зону", "Hold an area"},
    {"Сделать N раз", "Do N times"},
    {"Сюжет", "Story"},
    {"Лагерь", "Camp"},
    {"Заражённая зона", "Infestation"},
    {"Засада", "Ambush"},
    {"Орда", "Horde"},
    {"Задание лагеря", "Camp mission"},
    {"Поручение", "Task"},
    {"Обычный", "Basic"},
    {"Убить", "Kill"},
    {"Зачистить зону", "Clear area"},
    {"Защитить", "Defend"},
    {"Оборона зоны", "Defend area"},
    {"Обыск", "Search"},
    {"Выследить", "Track"},
    {"Без убийств", "No kills"},
    {"Подобрать", "Pick up"},
    {"Взять в руки", "Carry"},
    {"Обыскать машину", "Search a car"},
    {"Обыскать труп", "Search a body"},
    {"Взаимодействие", "Interact"},
    {"Дверь", "Door"},
    {"Взрыв", "Explosion"},
    {"Поджог", "Fire"},
    {"Поиск", "Searching"},
    {"Расходники", "Consumables"},
    {"Оружие", "Weapons"},
    {"Патроны", "Ammo"},
    {"Материалы", "Materials"},
    {"Полезное", "Utility"},
    {"Коллекционное", "Collectibles"},
    {"Детали мотоцикла", "Bike parts"},
    {"Детали оружия", "Weapon parts"},
    {"Прочее", "Other"},
    {"Имя файла квеста: латиница, цифры, _ и -", "Quest file name: latin letters, digits, _ and -"},
    {"Не удалось сохранить", "Could not save"},
    {"Сохранено: quests/", "Saved: quests/"},
    {"Мир режима ", "World of mode "},
    {" убрано, ", " removed, "},
    {" сдвинуто, ", " moved, "},
    {" добавлено", " added"},
    {"Поставлено: ", "Placed: "},
    {"Это склеенная группа мелких объектов — её не выбрать целиком", "This is a merged group of small objects — it cannot be selected as a whole"},
    {"Земля и растительность не редактируются", "Terrain and foliage are not editable"},
    {"Выберите этап квеста", "Select a quest step"},
    {"этап ", "step "},
    {"Зона → ", "Area → "},
    {"Точка → ", "Point → "},
    {"Здесь нет объекта: наведитесь на багажник, дверь, предмет или бочку", "No object here: aim at a trunk, door, item or barrel"},
    {"Объект ", "Object "},
    {"Выберите предмет в библиотеке", "Pick an item in the library"},
    {"Предмет ", "Item "},
    {"Убрано: ", "Removed: "},
    {"Удалён проп", "Prop deleted"},
    {"Дублируются поставленные пропсы", "Only placed props can be duplicated"},
    {"Копия", "Copied"},
    {"+ Транспорт", "+ Vehicle"},
    {"Гольф-кар", "Golf cart"},
    {"Самосвал (эксп.)", "Dump truck (exp.)"},
    {"Военный грузовик (эксп.)", "Military truck (exp.)"},
    {"Всем", "Everyone"},
    {"Никому", "Nobody"},
    {"  — Внутри", "  — inside"},
    {"  — Снаружи", "  — outside"},
    {"отмена##veh", "cancel##veh"},
    {"ЛКМ в мире ставит транспорт", "Left click in the world places a vehicle"},
    {"Транспорт режима: мотоциклы, гольф-кары. Сервер создаёт его при запуске режима; код находит его по ключу: getVehicle(\"ключ\").", "The mode's vehicles: bikes, golf carts. The server makes them when the mode starts; the code finds them by key: getVehicle(\"key\")."},
    {"Поставленный транспорт", "Placed vehicles"},
    {"Входы", "Doors"},
    {"+ Вход", "+ Door"},
    {"отмена##door", "cancel##door"},
    {"ЛКМ в мире ставит вход (снаружи)", "Left click in the world places the door (outside)"},
    {"Теперь ЛКМ ставит вторую сторону (внутри)", "Now a left click places the other end (inside)"},
    {"Двери в интерьеры, как входы в дома на RP-серверах SA-MP: [F] у одной стороны переносит к другой и в её виртуальный мир. Код режима может запереть вход (setDoor) или решить сам (событие door).", "Doors into interiors, like house entrances on SA-MP RP servers: [F] at one end takes the player to the other end and into its virtual world. The mode's code can lock a door (setDoor) or decide itself (event door)."},
    {"Поставленные входы", "Placed doors"},
    {"Лут", "Loot"},
    {"+ Лут", "+ Loot"},
    {"отмена##loot", "cancel##loot"},
    {"ЛКМ в мире кладёт предмет (выбранный в библиотеке)", "Left click in the world lays an item (the one picked in the library)"},
    {"Предметы, которые лежат в мире и возвращаются через заданное время после того, как их подобрали.", "Items lying in the world that come back some time after someone took them."},
    {"Поставленный лут", "Placed loot"},
    {"Вход", "Door"},
    {"Снаружи", "Outside"},
    {"Внутри", "Inside"},
    {"Мир снаружи", "World outside"},
    {"Мир внутри", "World inside"},
    {"Надпись [F]", "[F] text"},
    {"Заперт", "Locked"},
    {"К другой стороне", "Other end"},
    {"Владелец (ник)", "Owner (nickname)"},
    {"Бензин", "Fuel"},
    {"Количество", "Count"},
    {"Возвращается через, с (0 — один раз)", "Comes back after, s (0: once)"},
    {"из библиотеки", "from the library"},
    {"Мир", "World"},
    {"Режим", "Mode"},
    {"— выберите —", "— select —"},
    {"Сохранить", "Save"},
    {"Отменить", "Undo"},
    {"Отменено", "Undone"},
    {"Вернуть", "Redo"},
    {"Возвращено", "Redone"},
    {"Убрано: %zu   Сдвинуто: %zu   Добавлено: %zu", "Removed: %zu   Moved: %zu   Added: %zu"},
    {"Выделено", "Selection"},
    {"Кликните по объекту в мире", "Click an object in the world"},
    {"Кликните в мире, чтобы поставить проп", "Click in the world to place the prop"},
    {"Проп ", "Prop "},
    {"Позиция", "Position"},
    {"Поворот", "Rotation"},
    {"Масштаб", "Scale"},
    {"Убрать из мира", "Remove from world"},
    {"Удалить проп", "Delete prop"},
    {"Вернуть на место", "Reset position"},
    {"Дублировать", "Duplicate"},
    {"Убранные объекты", "Removed objects"},
    {"вернуть", "restore"},
    {"Поставленные пропсы", "Placed props"},
    {"Каталог пропсов не найден (data/props.txt).", "Props catalog not found (data/props.txt)."},
    {"Поиск пропа", "Search props"},
    {"…уточните поиск", "…refine the search"},
    {"ЛКМ в мире ставит: ", "LMB in the world places: "},
    {"  (блюпринт)", "  (blueprint)"},
    {"Библиотека", "Library"},
    {"Пропсы", "Props"},
    {"Предметы", "Items"},
    {"Загрузка списка предметов…", "Loading the item list…"},
    {"Поиск по названию или ключу", "Search by name or key"},
    {"ЛКМ в мире кладёт: ", "LMB in the world drops: "},
    {"Этап ", "Step "},
    {"Тип", "Type"},
    {"Текст", "Text"},
    {"Точка: %.0f  %.0f  %.0f", "Point: %.0f  %.0f  %.0f"},
    {"Точка не задана — ЛКМ в мире", "No point yet — LMB in the world"},
    {"Радиус, м", "Radius, m"},
    {"Маршрут по дорогам", "Road route"},
    {"Секунд в зоне", "Seconds in area"},
    {"Шкала", "Meter"},
    {"Кликните в мире…", "Click in the world…"},
    {"Поставить точку", "Place point"},
    {"Что засчитывать:", "What counts:"},
    {"Сколько раз (0 — по числу объектов)", "How many times (0 — number of objects)"},
    {"Предмет (ключ)", "Item (key)"},
    {"Класс объекта содержит", "Object class contains"},
    {"Поставить кликом в мире", "Place by clicking in the world"},
    {"Зону", "Area"},
    {"Объект", "Object"},
    {"Предмет", "Item"},
    {"Повторять", "Repeat"},
    {"Предмет: %s", "Item: %s"},
    {"выберите в библиотеке", "pick in the library"},
    {"Зона: %.0f  %.0f  %.0f", "Area: %.0f  %.0f  %.0f"},
    {"Убрать зону", "Remove area"},
    {"Радиус зоны, м", "Area radius, m"},
    {"Маркер над зоной", "Marker over the area"},
    {"Конкретные объекты:", "Specific objects:"},
    {"Предметы квеста (появятся на этом этапе):", "Quest items (appear at this step):"},
    {"Квест", "Quest"},
    {"(новый)", "(new)"},
    {"Новый", "New"},
    {"Проверить", "Test"},
    {"Удалить", "Delete"},
    {"Удалить квест %s?", "Delete quest %s?"},
    {"Да, удалить", "Yes, delete"},
    {"Откройте квест или создайте новый.", "Open a quest or create a new one."},
    {"Общее", "General"},
    {"Файл (id)", "File (id)"},
    {"Название", "Title"},
    {"Категория", "Category"},
    {"Вид маркеров", "Marker style"},
    {"Вступление", "Intro"},
    {"Опыт", "XP"},
    {"Награда", "Reward"},
    {"+ из библиотеки", "+ from library"},
    {"Этапы", "Steps"},
    {"+ Дойти", "+ Go to"},
    {"+ Удерживать", "+ Hold"},
    {"+ Сделать N раз", "+ Do N times"},
    {"Удалить этап", "Delete step"},
    {"Скопирован предмет", "Item copied"},
    {"Скопирован этап ", "Step copied: "},
    {"Выберите этап «Сделать N раз»", "Select a “Do N times” step"},
    {"Вставлен предмет", "Item pasted"},
    {"Вставлен этап ", "Step pasted: "},
    {": зона", ": area"},
    {"проп", "prop"},
    {"Двигать (W)", "Move (W)"},
    {"Вращать (E)", "Rotate (E)"},
    {"Масштаб (R)", "Scale (R)"},
    {"Ctrl — шаг   Del — убрать", "Ctrl — snap   Del — remove"},
    {"Ctrl — шаг   Del — удалить предмет", "Ctrl — snap   Del — delete item"},
    {"Редактор DGMP", "DGMP Editor"},
    {"Tab — полёт", "Tab — fly"},
    {"Tab — курсор", "Tab — cursor"},
    {"Новый квест", "New quest"},
    {"Квест удалён", "Quest deleted"},
    {"Время и погода", "Time & weather"}, {"Сохранять с режимом", "Save with the mode"},
    {"Режим стартует с этим временем и погодой (сервер применяет их до кода режима)", "The mode starts with this time and weather (the server applies them before the mode's code)"},
    {"Время", "Time"}, {"Остановить часы", "Stop clock"}, {"Погода", "Weather"}, {"авто", "auto"},
    {"Логика режима", "Mode logic"}, {"Обновить из кода", "Rescan code"}, {"Повторять##logic", "Repeat##logic"},
    {"отмена", "cancel"}, {"переставить", "move"}, {"поставить", "place"}, {"добавить", "add"},
    {"не используется в коде", "not used by the code"}, {"триггеры", "triggers"}, {"триггер", "trigger"},
    {"точки", "points"}, {"точка", "point"}, {", не поставлен", ", not placed"},
    {"Код режима не просит объектов: logic.trigger(\"имя\"), logic.points(\"имя\")…", "The mode's code asks for no objects: logic.trigger(\"name\"), logic.points(\"name\")…"},
    {"Дороги и бордюры (сплайны) пока не редактируются", "Roads and curbs (splines) are not editable yet"},
    {"Язык текстов:", "Text language:"},
    {"Военные, NERO, ополчение", "Military, NERO, militia"},
    {"Гнёзда фриков", "Freaker nests"},
    {"Декор и личные вещи", "Decor & belongings"},
    {"Динамические", "Dynamic"},
    {"Дороги", "Roads"},
    {"Заборы и ограждения", "Fences & barriers"},
    {"Заправка", "Gas station"},
    {"Здания: жилые", "Buildings: residential"},
    {"Здания: коммерческие", "Buildings: commercial"},
    {"Здания: общие элементы", "Buildings: parts"},
    {"Знаки и вывески", "Signs"},
    {"Камни", "Rocks"},
    {"Кемпинг и лагеря", "Camping & camps"},
    {"Кухня и быт", "Kitchen & household"},
    {"Магазин и общепит", "Shops & diners"},
    {"Мастерская и инструменты", "Workshop & tools"},
    {"Мебель", "Furniture"},
    {"Медицина", "Medical"},
    {"Мрачное", "Grim"},
    {"Мусор и обломки", "Trash & debris"},
    {"Наборы пропсов", "Prop sets"},
    {"Освещение", "Lighting"},
    {"Отдых и спорт", "Leisure & sport"},
    {"Офис", "Office"},
    {"Предметы", "Items"},
    {"Природа", "Nature"},
    {"Промышленность", "Industrial"},
    {"Пропсы: разное", "Props: misc"},
    {"Разное", "Misc"},
    {"Растения", "Plants"},
    {"Собираемое", "Collectables"},
    {"Транспорт", "Vehicles"},
    {"Укрытия", "Cover"},
    {"Укрытия: природные", "Cover: natural"},
    {"Укрытия: сценарии", "Cover: scenarios"},
    {"Улица", "Street"},
    {"Ферма и ранчо", "Farm & ranch"},
    {"Шахты и пещеры", "Mines & caves"},
    {"игрок видит свой язык, иначе английский", "players see their language, else English"},
    {"Сохранено: modes/", "Saved: modes/"},
    {"Запуск проверки: редактор закроется", "Starting the test: the editor will close"},
    {".json повреждён", ".json is damaged"},
    {"Выберите режим: правки мира хранятся у каждого режима свои (modes/<режим>/world.json).", "Pick a mode: every mode keeps its own world edits (modes/<mode>/world.json)."},
    {"Персонажи", "Characters"},
    {"Свет", "Lights"},
    {"отмена##npc", "cancel##npc"},
    {"ЛКМ в мире ставит NPC", "LMB in the world places an NPC"},
    {"ЛКМ в мире ставит NPC: ", "LMB in the world places the NPC: "},
    {"Статисты режима: облик, анимация, имя. Сервер создаёт их сам при запуске режима; код режима находит их по id: logic.actor(\"id\"), logic.actors(\"группа\"). Клик по NPC в мире выделяет его.",
     "The mode's extras: a look, an animation, a name. The server makes them when the mode starts; the mode's code finds them by id: logic.actor(\"id\"), logic.actors(\"group\"). A click on an NPC in the world selects it."},
    {"Поставленные NPC", "Placed NPCs"},
    {"NPC, созданные кодом режима (createActor), здесь не видны. Ставьте их здесь, а в коде берите через logic.actor(\"id\") / logic.actors(\"группа\").",
     "NPCs created by the mode's code (createActor) are not shown here. Place them here and get them in the code with logic.actor(\"id\") / logic.actors(\"group\")."},
    {"Поставлен NPC ", "NPC placed: "},
    {"id: латиница, цифры, _ и -", "id: latin letters, digits, _ and -"},
    {"Такой id уже есть", "This id is taken"},
    {"Группа", "Group"},
    {"logic.actors(\"группа\") в коде режима вернёт всех NPC этой группы", "logic.actors(\"group\") in the mode's code returns every NPC of the group"},
    {"Имя над головой", "Name above the head"},
    {"облик (пусто — Дикон)", "look (empty: Deacon)"},
    {"Облик", "Look"},
    {"Анимация", "Animation"},
    {"нет такой анимации", "no such animation"},
    {"Твёрдый (в него упираются)", "Solid (players bump into it)"},
    {"Мир (-1 — во всех)", "World (-1: every)"},
    {"NPC, группа", "NPC group"},
    {"Поиск", "Search"},
    {"место выбора скина", "skin choosing place"},
    {"Место выбора скина", "Skin choosing place"},
    {"Поставлено: %.0f %.0f %.0f", "Placed: %.0f %.0f %.0f"},
    {"Не задано: игрок выбирает там, где стоит", "Not set: players choose where they stand"},
    {"Переставить", "Move"},
    {"Поставить в мире", "Place in the world"},
    {"Показать", "Show"},
    {"Убрать##place", "Remove##place"},
    {"ЛКМ в мире ставит место выбора; стрелка — куда смотрит персонаж", "LMB in the world places it; the arrow is where the character faces"},
    {"Например комната под землёй из пропсов со светом. player:customize() переносит игрока туда, после выбора возвращает назад (на спавн при первом входе). Хранится в world.json — «Сохранить» здесь пишет и его.",
     "E.g. a room of props and lights under the ground. player:customize() takes the player there and back after choosing (to the spawn on the first visit). Kept in world.json: Save here writes it too."},
    {"Точечный свет", "Point light"},
    {"Прожектор", "Spot light"},
    {"Цвет", "Colour"},
    {"Яркость", "Intensity"},
    {"Дальность, м", "Reach, m"},
    {"Угол конуса", "Cone angle"},
    {"Тени", "Shadows"},
    {"Свет для интерьеров. Прожектор светит из основания конуса (по умолчанию вниз), поворачивается гизмо. Тени дорогие: включайте немногим.",
     "Lights for interiors. A spot light shines out of the cone's base (down by default); turn it with the gizmo. Shadows are expensive: only a few."},
    {"Формы", "Uniforms"},
    {"Скины на старте", "Start skins"},
    {": id — латиница, цифры, _ и -", ": id - latin letters, digits, _ and -"},
    {"латиница, цифры, _ и -", "latin letters, digits, _ and -"},
    {": облик не собран", ": no look made yet"},
    {": такой id уже есть", ": this id is used twice"},
    {"Облик: ", "Look: "},
    {"Выберите режим: у каждого режима свои скины (modes/<режим>/skins.json).", "Pick a mode: every mode has its own skins (modes/<mode>/skins.json)."},
    {"не сохранено", "not saved"},
    {"Игроки выбирают только из этих скинов (player:customize()); формы режим надевает поверх скина (player:setUniform(\"id\")). Облик собирается на манекене перед камерой: пресет игры или своя сборка из частей.",
     "Players choose only from these skins (player:customize()); the mode puts uniforms over the skin (player:setUniform(\"id\")). A look is made on a mannequin in front of the camera: a preset of the game or your own build from parts."},
    {"Идёт сборка облика: окно слева. «Готово» запишет облик в запись, «Отмена» оставит как было.", "Making the look: the window on the left. Done keeps it in the entry, Cancel leaves the entry as it was."},
    {"Пусто: «Добавить» соберёт форму", "Empty: Add makes a uniform"},
    {"Пусто: игроки получат полный редактор персонажа. «Добавить» соберёт скин", "Empty: players get the full character creator. Add makes a skin"},
    {"  — без облика", "  - no look"},
    {"Добавить", "Add"},
    {"Запись", "Entry"},
    {"Название (EN)", "Name (EN)"},
    {"Название (RU)", "Name (RU)"},
    {"Собрать облик", "Make the look"},
    {"Изменить облик", "Change the look"},
    {"(двойной клик по записи)", "(double click an entry)"},
    {"Не удалось поставить манекен", "Could not place the mannequin"},
    {"PvP и зоны", "PvP and zones"},
    {"Урон между игроками (PvP)", "Damage between players (PvP)"},
    {"Выключено: игроки ранят друг друга только в PvP-зонах. Код режима может менять это: setPvp(), player:setPvp(), setPvpRule().",
     "Off: players hurt each other only in PvP zones. The mode's code can change it: setPvp(), player:setPvp(), setPvpRule()."},
    {"+ PvE зона", "+ PvE zone"}, {"+ PvP зона", "+ PvP zone"},
    {"ЛКМ в мире ставит центр зоны", "LMB in the world places the zone's centre"},
    {"PvE (зелёная) — безопасная зона: игроки не ранят друг друга, стрелять из неё тоже нельзя. PvP (красная) — урон между игроками есть, даже если PvP режима выключен (нужно, чтобы оба были в PvP-зонах). Зомби и NPC опасны везде.",
     "PvE (green) is a safe zone: players cannot hurt each other there, nor shoot out of it. PvP (red): players hurt each other even when the mode's PvP is off (both must stand in PvP zones). Zombies and NPCs are dangerous everywhere."},
    {"Зон нет", "No zones"}, {"м", "m"}, {", выключена", ", off"},
    {"Поставлена зона ", "Zone placed: "},
    {"Имя зоны", "Zone name"},
    {"Имя для кода режима: getZone(\"имя\"), setZone(\"имя\", {...}), player:inZone(\"имя\"). Латиница, цифры, _ и -",
     "The name for the mode's code: getZone(\"name\"), setZone(\"name\", {...}), player:inZone(\"name\"). Latin letters, digits, _ and -"},
    {"PvE (безопасная)", "PvE (safe)"}, {"Круг", "Circle"}, {"Прямоугольник", "Box"}, {"Центр", "Centre"},
    {"Длина, м", "Length, m"}, {"Ширина, м", "Width, m"}, {"Высота вверх и вниз, м", "Height up and down, m"},
    {"Включена при старте", "On at the start"},
    {"Выключенную зону код режима включает сам: setZone(\"имя\", {enabled = true})", "The mode's code turns an off zone on: setZone(\"name\", {enabled = true})"},
    {"Надпись на экране в зоне", "A line on the screen in the zone"},
    {"Своя надпись", "Own text"},
    {"+ Зомби", "+ Zombies"}, {"Зона зомби (салатовая): пока рядом игрок, в ней держится заданное число зомби (server.ini Zombies=1).",
     "Zombie zone (lime): while a player is near it keeps its number of zombies (server.ini Zombies=1)."},
    {"Имя для кода режима: getZombieZone(\"имя\"), setZombieZone(\"имя\", {...}). Латиница, цифры, _ и -",
     "The name for the mode's code: getZombieZone(\"name\"), setZombieZone(\"name\", {...}). Latin letters, digits, _ and -"},
    {"Типы зомби", "Zombie types"},
    {"Через запятую: Swarmer, Newt... Появляются только типы, загруженные в этом регионе (docs/zombies.md)",
     "Comma separated: Swarmer, Newt... Only the types this region has loaded appear (docs/zombies.md)"},
    {"Сколько держать", "How many"}, {"Новый вместо убитого через, с", "A new one for a killed one after, s"},
    {"Просыпается, когда игрок ближе, м", "Wakes when a player is nearer than, m"},
    {"От края зоны. Без игроков рядом 30 с зона засыпает: её зомби исчезают", "From the zone's edge. With nobody near for 30 s the zone sleeps: its zombies go"},

    {"Пусто — стандартная («Безопасная зона» / «PvP-зона»). Можно цвета {RRGGBB}", "Empty: the default one (Safe zone / PvP zone). Colours {RRGGBB} work"},
    {"Переноска", "Carrying"}, {"+ Ящик", "+ Box"}, {"отмена##carry", "cancel##carry"},
    {"ЛКМ в мире ставит ящик", "LMB in the world places a box"},
    {"Ящики, которые игроки носят двумя руками (зажать F). Сервер кладёт их при запуске режима; код режима находит их по ключу: getCarry(\"ключ\"), по метке: carries(\"метка\"). Новый ящик берёт модель, метку и мир выделенного.",
     "Boxes the players carry in both hands (hold F). The server lays them out when the mode starts; the mode's code finds them by key: getCarry(\"key\"), by tag: carries(\"tag\"). A new box takes the model, tag and world of the selected one."},
    {"Перекат с ящиком", "Dodge roll with it"}, {"Ничего", "Nothing"}, {"Падает", "It falls"}, {"Ломается", "It breaks"},
    {"Что будет, если игрок сделает перекат с ящиком в руках: ничего, ящик падает на землю, ящик ломается (исчезает)",
     "What a dodge roll with the box in the hands does: nothing, the box falls to the ground, the box breaks (it is gone)"},
    {"Поставленные ящики", "Placed boxes"}, {"Поставлен ящик ", "Box placed: "}, {"Ящик", "Box"},
    {"Ключ", "Key"}, {"Модель", "Model"}, {"Метка", "Tag"},
    {"Ключ: латиница, цифры, _ и -", "Key: Latin letters, digits, _ and -"}, {"Такой ключ уже есть", "That key is taken"},
    {"Имя для кода режима: getCarry(\"ключ\")", "The name for the mode's code: getCarry(\"key\")"},
    {"carries(\"метка\") в коде режима вернёт все ящики с этой меткой", "carries(\"tag\") in the mode's code returns every box with this tag"},
    // jobs and the deliver step (docs/quests.md)
    {"Перенести ящики", "Carry boxes"}, {"+ Перенести ящики", "+ Carry boxes"},
    {"Квест (один раз)", "Quest (once)"}, {"Работа (повторяется)", "Job (repeats)"}, {"Вид", "Kind"},
    {"Работа", "Job"}, {"NPC из мира", "NPC of the world"}, {"свой NPC (ниже)", "own NPC (below)"},
    {"NPC мира — из режима, открытого во вкладке «Мир»", "World NPCs come from the mode opened on the World tab"},
    {"В режиме «%s» работу даёт этот NPC. В других режимах (и в «Проверить») на его месте встаёт такой же NPC работы.",
     "In the mode \"%s\" this NPC gives the job. In other modes (and in Test) the job puts the same NPC in its place."},
    {"Место NPC: %.0f  %.0f  %.0f", "NPC spot: %.0f  %.0f  %.0f"}, {"Место NPC не задано — ЛКМ в мире", "NPC spot not set: LMB in the world"},
    {"Поставить NPC", "Place the NPC"}, {"Имя NPC", "NPC name"},
    {"Метка на карте", "Map marker"}, {"Форма на работе", "Work uniform"},
    {"Облик на время работы: id формы режима или облик игры (пусто — без формы)", "The look while on the job: a uniform id of the mode or a look of the game (empty: none)"},
    {"Оплата, $ за раз", "Pay, $ each time"}, {"Когда платить", "Paid"}, {"Сразу, за каждый раз", "At once, each time"}, {"При сдаче работы", "When the job is handed in"},
    {"Раз за смену (0 — без ограничения)", "Times a shift (0: no limit)"},
    {"Этапы ниже повторяются по кругу, пока игрок на работе. Раз = все этапы по одному разу.",
     "The steps below repeat in a loop while the player is on the job. One time = every step once."},
    {"Опыт при сдаче работы", "XP when the job is handed in"},
    {"Погрузка: где лежат ящики", "Pick-up: where the boxes lie"}, {"Разгрузка: куда их отнести", "Drop-off: where to carry them"},
    {"Погрузка: %.0f  %.0f  %.0f", "Pick-up: %.0f  %.0f  %.0f"}, {"Разгрузка: %.0f  %.0f  %.0f", "Drop-off: %.0f  %.0f  %.0f"},
    {"Не задано — кнопка ниже, потом ЛКМ в мире", "Not set: the button below, then LMB in the world"},
    {"Поставить погрузку", "Place the pick-up"}, {"Поставить разгрузку", "Place the drop-off"},
    {"Радиус погрузки, м", "Pick-up radius, m"}, {"Радиус разгрузки, м", "Drop-off radius, m"}, {"Ящиков на погрузке", "Boxes at the pick-up"},
    {"Ящики мира (метка)", "Boxes of the world (tag)"}, {"(нет)", "(none)"},
    {"В режиме «%s» с этой меткой ящиков: %d — берутся они и возвращаются на свои места", "The mode \"%s\" has %d boxes with this tag: they are used and go back to their places"},
    {"Ящики кладутся вокруг точки погрузки", "The boxes are laid out around the pick-up point"},
    {"Название на карте", "Name on the map"}, {"Сколько ящиков отнести", "Boxes to deliver"}, {"Маршрут к разгрузке", "Route to the drop-off"},
    {"Засчитывается ящик, поставленный (зажать F) в зоне разгрузки. Брать эти ящики может только тот, кто на этом этапе (у работы — кто на работе).",
     "A box put down (hold F) in the drop-off area counts. Only the player on this step (for a job: on the job) may pick these boxes up."},
    {"Выдаёт работу", "Gives the job"}, {"Место NPC работы", "Job NPC spot"}, {"Погрузка → ", "Pick-up → "}, {"Разгрузка → ", "Drop-off → "},
    {": погрузка", ": pick-up"}, {": разгрузка", ": drop-off"}, {"Выберите этап «Перенести ящики»", "Select a \"Carry boxes\" step"},
    {"Погрузка", "Pick-up"}, {"Разгрузка", "Drop-off"}, {"Кто даёт работу", "Who gives the job"}, {"Форма и оплата", "Uniform and pay"},
    {"без формы", "no uniform"},
    {"Поговорить", "Talk"}, {"+ Поговорить", "+ Talk"}, {"NPC (id из «Мир» → NPC)", "NPC (id from World > NPC)"},
    {"пусто — точка ниже", "empty: the point below"}, {"Кто говорит", "Speaker"},
    {"Реплики (по очереди, в окне диалога):", "Lines (one after another, in a dialog):"}, {"+ реплика", "+ line"}
    };
    if (!english) return ru;
    auto it = en.find(ru);
    return it == en.end() ? ru : it->second;
}
// A translated copy of a label array, for ImGui::Combo.
const char* const* TArr(const char* const* arr, int count) {
    static std::map<const void*, std::vector<const char*>> cache;
    auto& v = cache[arr]; v.assign(count, nullptr);
    for (int i = 0; i < count; ++i) v[i] = T(arr[i]);
    return v.data();
}
// Everything below is shared between the game thread (Update) and the frame thread (Draw).
std::recursive_mutex lock;

// ---------------------------------------------------------------- quest model (docs/quests.md)
const char* kStepTypes[] = {"Дойти до точки", "Удерживать зону", "Сделать N раз", "Перенести ящики", "Поговорить"};
const char* kStepKeys[] = {"goto", "hold", "collect", "deliver", "talk"};
constexpr int kStepCount = 5;
const char* kKinds[] = {"Квест (один раз)", "Работа (повторяется)"};
const char* kPayouts[] = {"Сразу, за каждый раз", "При сдаче работы"};  // "each", "end"
const char* const kCarryRolls[] = {"keep", "drop", "break"};          // a box's dodge roll (world.json carries, deliver steps)
const char* kCategories[] = {"Сюжет", "Лагерь", "Заражённая зона", "Засада", "NERO", "Орда", "Задание лагеря", "Поручение"};
const char* kCategoryKeys[] = {"story", "camp", "infestation", "ambush", "nero", "horde", "campmission", "task"};
const char* kStyles[] = {"Обычный", "Убить", "Зачистить зону", "Защитить", "Оборона зоны", "Обыск", "Выследить", "Без убийств"};
const char* kStyleKeys[] = {"basic", "kill", "killarea", "defend", "defendarea", "search", "track", "nokill"};
const char* kEvents[] = {"Подобрать", "Взять в руки", "Обыскать машину", "Обыскать труп", "Взаимодействие", "Дверь", "Взрыв", "Поджог"};
const char* kEventKeys[] = {"pickup", "carry", "search", "body", "interact", "door", "explosion", "fire"};

// A quest text in both languages (docs/quests.md: a string or {"en": ..., "ru": ...}).
struct LT { char en[512] = ""; char ru[512] = ""; };
int contentLang = 0;  // language the quest texts are edited in: 0 English, 1 Russian
char* Cur(LT& t) { return contentLang ? t.ru : t.en; }
const char* Shown(const LT& t) { const char* a = contentLang ? t.ru : t.en; const char* b = contentLang ? t.en : t.ru; return a[0] ? a : b; }
json::Value LJson(const LT& t) {
    if (t.en[0] && t.ru[0]) { json::Value v = json::Value::Object(); v.set("en", std::string(t.en)); v.set("ru", std::string(t.ru)); return v; }
    return std::string(t.en[0] ? t.en : t.ru);
}
bool HasCyrillic(const std::string& s) { for (size_t i = 0; i + 1 < s.size(); ++i) if ((unsigned char)s[i] == 0xD0 || (unsigned char)s[i] == 0xD1) return true; return false; }
void LFrom(const json::Value& v, LT& t, const char* fallback = "") {
    t = LT{};
    if (v.isObject()) { snprintf(t.en, sizeof(t.en), "%s", v["en"].asString().c_str()); snprintf(t.ru, sizeof(t.ru), "%s", v["ru"].asString().c_str()); return; }
    const std::string text = v.isString() ? v.asString() : std::string(fallback);
    snprintf(HasCyrillic(text) ? t.ru : t.en, sizeof(t.en), "%s", text.c_str());  // old single-language files
}

struct SpawnRow { char item[96] = ""; int count = 1; float x = 0, y = 0, z = 0, yaw = 0; };
struct Step {
    int type = 0;
    LT text;
    bool hasPoint = false; float x = 0, y = 0, z = 0, radius = 5; bool route = true;   // goto / hold
    float seconds = 10; LT meter;                                      // hold
    unsigned events = 0; int count = 0; char item[96] = ""; char cls[96] = "";          // collect
    std::vector<std::string> ids;
    bool hasArea = false; float ax = 0, ay = 0, az = 0, ar = 5; bool marker = true;
    std::vector<SpawnRow> spawn;
    // deliver: boxes from the pick-up area to the drop-off area (docs/quests.md); count = boxes to deliver
    bool hasPickup = false; float px = 0, py = 0, pz = 0, pr = 3; int boxes = 4; char model[32] = "crate"; int roll = 0; char tag[48] = ""; LT pickName;
    bool hasDrop = false; float dx = 0, dy = 0, dz = 0, dr = 4; LT dropName;
    // talk: at an NPC of world.json ("actor") or at the point (hasPoint); the lines show one after another
    char actor[48] = ""; LT speaker; std::vector<LT> lines;
};
struct Reward { char item[96] = ""; int count = 1; };
struct Quest {
    char id[64] = "";
    LT title;
    int category = 7, style = 0;
    LT text;
    int xp = 100;
    std::vector<Reward> reward;
    std::vector<Step> steps;
    // "kind": "job": the steps repeat while the player is on the job (docs/quests.md)
    bool job = false;
    char giverActor[48] = "";                                          // an NPC of the mode's world, else its own NPC:
    bool giverHasPoint = false; float gx = 0, gy = 0, gz = 0, gyaw = 0;  // (also the copy of the world one, for other modes)
    char giverLook[192] = "", giverAnim[128] = "idle"; LT giverName; bool giverMarker = true;
    char uniform[192] = "";
    int pay = 5, payout = 1, cycles = 0;                               // payout: 0 "each", 1 "end"
};

Quest quest;
std::vector<Quest> undoStack, redoStack;  // history (TrackHistory)
void ResetHistory(); void Undo(); void Redo(); void Duplicate();
bool questOpen = false;
std::string loadedId;          // file the open quest came from ("" = new)
int selected = -1;             // selected step
bool previewDirty = true;
double previewAt = 0;
std::vector<std::string> questFiles;
enum ClickMode { kSelect = 0, kPoint = 1, kObject = 2, kItem = 3, kPickup = 4, kDropoff = 5, kGiver = 6 };
int clickMode = kSelect;       // what a left click into the world does
bool repeatPlace = false;      // stay in the placing mode after a placement
// Selection: one editable thing of the quest (a step's point, a collect zone, a quest item).
struct Ref { int step = -1, kind = 0, index = 0; };  // kind 0 point, 1 zone, 2 spawned item, 3 pick-up, 4 drop-off, 5 the job's NPC (step -1)
Ref picked;
int gizmoOp = 0;               // 0 move, 1 rotate
// Camera of the last game frame (game thread → frame thread), for projections and the gizmo.
struct Cam { bool valid = false; FVector loc{}; FRotator rot{}; float fov = 90; };
Cam cam;
std::string toast; double toastUntil = 0;
int mode = 1;                  // 0 world, 1 quest, 2 characters (the mode's skins)
std::string status;            // SetStatus (UTF-8)
bool statusOpen = false;

// ---------------------------------------------------------------- item library
struct Item { std::string key, name, english; int type = 0; };
std::vector<Item> catalog;
bool catalogReady = false;
int libraryType = 5;           // EInventoryBaseType shown (5 consumables)
char librarySearch[64] = "";
char placeItem[96] = "";       // item a click places (quest mode: spawn of the selected step)
const struct { int type; const char* name; } kItemTypes[] = {
    {5, "Расходники"}, {4, "Оружие"}, {3, "Патроны"}, {2, "Материалы"}, {6, "Полезное"}, {12, "Коллекционное"}, {8, "Детали мотоцикла"}, {9, "Детали оружия"}, {0, "Прочее"}};

// ---------------------------------------------------------------- UI → game thread
enum class Action { Save, New, Load, Delete, Test, WorldClick, WorldLoad, WorldSave, SkinsSave };

// ---------------------------------------------------------------- world model (modes/<mode>/world.json)
struct WT { float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0, scale = 1; };
// An added prop, or a light (asset "light:point" / "light:spot"; colour sRGB 0..1, reach in metres).
struct WLight { float color[3] = {1.f, 0.85f, 0.7f}; float intensity = 5000.f, radius = 10.f, cone = 40.f; bool shadows = false; };
struct WAdd { std::string uid, asset, name; WT t; bool light = false; WLight l; };
// Logic objects of the mode (modes/_lib/logic.lua): triggers and points its code asks for by name,
// placed here. A name holds one object or an ordered list (checkpoints, start grid).
struct LItem { WT t; float radius = 5; };     // radius in metres (triggers)
// A character of the mode (world.json "actors", docs/API.md "NPC"): placed here, made by the server.
struct WActor { char id[48] = "", group[48] = "", look[192] = "", anim[128] = "idle", name[96] = ""; WT t; bool solid = true; int world = -1; };
// A PvE / PvP zone (world.json "zones", docs/API.md "PvP и зоны"): sizes in metres, t = the centre on the ground.
// zombie: a zombie zone instead (world.json "zombieZones", docs/zombies.md): a circle that keeps `max` zombies of
// `types` (comma separated) while a player is within `wake` m of it, a dead one back after `respawn` s.
struct WZone { char name[48] = ""; bool pvp = false, box = false; WT t; float radius = 20, length = 40, width = 40, height = 50;
               bool enabled = true, notify = true; char text[160] = ""; int world = -1;
               bool zombie = false; char types[96] = "Swarmer"; int max = 6; float respawn = 60, wake = 150; };
// A box to carry in both hands (world.json "carries", docs/API.md "Переноска"): placed here, made by the server.
struct WCarry { char key[48] = "", model[32] = "crate", tag[48] = ""; WT t; int world = -1; int roll = 0; };  // roll: 0 keep, 1 drop, 2 break
// Things the server makes from world.json without code (docs/API.md "Транспорт", "Входы", "Лут"):
// type 0 a vehicle ("vehicles"), 1 a door ("doors": t outside, t2 inside), 2 loot ("loot").
struct WEnt {
    int type = 0;
    char key[48] = "";
    WT t; int world = 0;
    char model[32] = "bike", owner[48] = ""; bool locked = false; int blip = 0; float fuel = 1;  // vehicle (blip: 0 owner, 1 all, 2 none)
    WT t2; int world2 = 0; char text[96] = "";                                                     // door
    char item[96] = ""; int count = 1, respawn = 300; char tag[48] = "";                           // loot
};
const char* const kEntPrefix[] = {"v", "d", "l"};
const char* const kVehicleModels[] = {"bike", "golfcart", "dumptruck", "militarytruck"};
const char* const kVehicleBlips[] = {"owner", "all", "none"};
struct World {
    std::vector<std::string> remove;          // world objects ("<level>/<actor>")
    std::map<std::string, WT> move;           // world objects moved/turned
    std::vector<WAdd> add;                    // props placed by the mode
    std::map<std::string, std::vector<LItem>> logic;
    std::vector<WActor> actors;
    std::vector<WZone> zones;
    std::vector<WCarry> carries;
    std::vector<WEnt> ents;                   // vehicles, doors, loot
    bool pvp = true;                          // world.json "rules" "pvp": players hurt each other outside PvE zones
    // Time of day and weather the mode starts with (world.json "environment"); weather -1 = automatic.
    struct Env { bool set = false; float time = 12; bool paused = false; int weather = -1; } env;
};
// What the mode's code asks for: logic.trigger("x"), logic.triggers("x"), logic.point("x"), logic.points("x").
struct LDecl { std::string name; bool list = false, trigger = false, actor = false; float radius = 5; };
std::vector<LDecl> logicDecls;
std::string placeLogic;                       // name a click places an object of (world mode)
int placeZone = 0;                            // a click places a zone: 1 PvE, 2 PvP
bool placeCarry = false;                      // a click places a box to carry
int placeEnt = 0;                             // a click places: 1 a vehicle, 2 a door (then its inside end), 3 loot
std::string doorPending;                      // the door whose inside end the next click places
const LDecl* FindDecl(const std::string& name) { for (auto& d : logicDecls) if (d.name == name) return &d; return nullptr; }
void SelectLogic(const std::string& name, int index);
void LoadSkinLists(const std::string& m);
World world;
std::string worldMode;                        // mode whose world is edited
std::vector<std::string> modes;               // modes/* folders
bool worldDirty = false;                      // the game should apply the edits again
bool worldClickPending = false;
// Selection in the world: kind 1 world object (id = key, t = its transform), 2 added prop (id = uid),
// 3 logic object (id = "name#index"), 4 NPC (id = its id), 5 zone (id = its index), 6 box to carry (id = its key).
struct WSel { int kind = 0; std::string id, cls; WT t; UObject* actor = nullptr; };  // actor: the clicked world object
WSel wsel;
std::vector<World> worldUndo, worldRedo;
World worldCommitted; std::string worldCommittedJson; bool worldCommittedValid = false;
int worldGizmo = 0;                           // 0 move, 1 rotate, 2 scale

// Props of the game (data/props.txt, built from the game's packages): category, sub, kind, path, name.
struct Prop { std::string cat, sub, kind, path, name; };
std::vector<Prop> props;
std::vector<std::string> propCats;
int propCat = 0;
char propSearch[64] = "";
std::string placeActor;                       // an NPC a click places: "*" a free one, else a name of the code's logic.actor(s)
std::string placeProp;                        // path a click places (world mode)
int libraryTab = 0;                           // 0 props, 1 items
std::vector<std::pair<Action, std::string>> actions;

double Now() { return GetTickCount64() / 1000.0; }
void Toast(const std::string& text) { toast = text; toastUntil = Now() + 3; }
void Copy(char* dst, size_t size, const std::string& src) { snprintf(dst, size, "%s", src.c_str()); }
int IndexOf(const char* const* keys, int count, const std::string& key, int fallback) {
    for (int i = 0; i < count; ++i) if (key == keys[i]) return i;
    return fallback;
}
fs::path QuestDir() { return fs::path(profile::Root()) / L"quests"; }

// ---------------------------------------------------------------- JSON
json::Value Point(float x, float y, float z) {
    json::Value v = json::Value::Object();
    v.set("x", double(std::round(x))); v.set("y", double(std::round(y))); v.set("z", double(std::round(z)));
    return v;
}
json::Value ToJson(const Quest& q) {
    json::Value v = json::Value::Object();
    v.set("id", std::string(q.id)); v.set("title", LJson(q.title));
    v.set("category", kCategoryKeys[q.category]);
    if (q.style) v.set("style", kStyleKeys[q.style]);
    if (q.text.en[0] || q.text.ru[0]) v.set("text", LJson(q.text));
    if (q.xp) v.set("xp", q.xp);
    if (q.job) {
        v.set("kind", "job");
        json::Value g = json::Value::Object();
        if (q.giverActor[0]) g.set("actor", std::string(q.giverActor));
        if (q.giverHasPoint) { g.set("x", double(std::round(q.gx))); g.set("y", double(std::round(q.gy))); g.set("z", double(std::round(q.gz))); g.set("yaw", double(std::round(q.gyaw))); }
        if (q.giverLook[0]) g.set("look", std::string(q.giverLook));
        if (q.giverAnim[0] && std::strcmp(q.giverAnim, "idle") != 0) g.set("anim", std::string(q.giverAnim));
        if (q.giverName.en[0] || q.giverName.ru[0]) g.set("name", LJson(q.giverName));
        if (!q.giverMarker) g.set("marker", false);
        v.set("giver", g);
        if (q.uniform[0]) v.set("uniform", std::string(q.uniform));
        v.set("pay", q.pay);
        v.set("payout", q.payout ? "end" : "each");
        if (q.cycles > 0) v.set("cycles", q.cycles);
    }
    if (!q.reward.empty() && !q.job) {
        json::Value r = json::Value::Array();
        for (auto& w : q.reward) if (w.item[0]) { json::Value e = json::Value::Object(); e.set("item", std::string(w.item)); e.set("count", w.count); r.push(e); }
        if (r.size()) v.set("reward", r);
    }
    json::Value steps = json::Value::Array();
    for (auto& s : q.steps) {
        json::Value j = json::Value::Object();
        j.set("type", kStepKeys[s.type]); j.set("text", LJson(s.text));
        if (s.type == 0 || s.type == 1) {
            if (s.hasPoint) { j.set("x", double(std::round(s.x))); j.set("y", double(std::round(s.y))); j.set("z", double(std::round(s.z))); }
            j.set("radius", double(s.radius));
            if (s.type == 0 && !s.route) j.set("route", false);
            if (s.type == 1) { j.set("seconds", double(s.seconds)); if (s.meter.en[0] || s.meter.ru[0]) j.set("meter", LJson(s.meter)); }
        } else if (s.type == 4) {
            if (s.actor[0]) j.set("actor", std::string(s.actor));
            else if (s.hasPoint) { j.set("x", double(std::round(s.x))); j.set("y", double(std::round(s.y))); j.set("z", double(std::round(s.z))); }
            j.set("radius", double(s.radius));
            if (!s.route) j.set("route", false);
            if (s.speaker.en[0] || s.speaker.ru[0]) j.set("speaker", LJson(s.speaker));
            json::Value lines = json::Value::Array();
            for (auto& l : s.lines) if (l.en[0] || l.ru[0]) lines.push(LJson(l));
            j.set("lines", lines);
        } else if (s.type == 3) {
            json::Value pu = s.hasPickup ? Point(s.px, s.py, s.pz) : json::Value::Object();
            pu.set("radius", double(s.pr));
            pu.set("boxes", s.boxes); pu.set("model", std::string(s.model));
            if (s.roll) pu.set("roll", kCarryRolls[s.roll]);
            if (s.tag[0]) pu.set("tag", std::string(s.tag));
            if (s.pickName.en[0] || s.pickName.ru[0]) pu.set("name", LJson(s.pickName));
            j.set("pickup", pu);
            json::Value d = s.hasDrop ? Point(s.dx, s.dy, s.dz) : json::Value::Object();
            d.set("radius", double(s.dr));
            if (s.dropName.en[0] || s.dropName.ru[0]) d.set("name", LJson(s.dropName));
            j.set("dropoff", d);
            if (s.count > 1) j.set("count", s.count);
            if (!s.route) j.set("route", false);
        } else {
            json::Value ev = json::Value::Array();
            for (int i = 0; i < 8; ++i) if (s.events & (1u << i)) ev.push(kEventKeys[i]);
            if (ev.size() == 1) j.set("event", ev[0].asString()); else if (ev.size() > 1) j.set("event", ev);
            if (s.count > 0) j.set("count", s.count);
            if (s.item[0]) j.set("item", std::string(s.item));
            if (s.cls[0]) j.set("class", std::string(s.cls));
            if (!s.ids.empty()) { json::Value a = json::Value::Array(); for (auto& id : s.ids) a.push(id); j.set("ids", a); }
            if (s.hasArea) { json::Value a = Point(s.ax, s.ay, s.az); a.set("radius", double(s.ar)); j.set("area", a); }
            if (s.marker && (s.hasArea || !s.spawn.empty())) j.set("marker", true);
            if (!s.spawn.empty()) {
                json::Value a = json::Value::Array();
                for (auto& r : s.spawn) {
                    json::Value e = Point(r.x, r.y, r.z); e.set("item", std::string(r.item));
                    if (r.count != 1) e.set("count", r.count);
                    if (std::fabs(r.yaw) > 0.5f) e.set("yaw", double(std::round(r.yaw)));
                    a.push(e);
                }
                j.set("spawn", a);
            }
        }
        steps.push(j);
    }
    v.set("steps", steps);
    return v;
}
void FromJson(const json::Value& v, Quest& q) {
    q = Quest{};
    Copy(q.id, sizeof(q.id), v["id"].asString()); LFrom(v["title"], q.title);
    q.category = IndexOf(kCategoryKeys, 8, v["category"].asString(), 7);
    q.style = IndexOf(kStyleKeys, 8, v["style"].asString(), 0);
    LFrom(v["text"], q.text);
    q.xp = v["xp"].asInt(0);
    for (auto& r : v["reward"].elements()) { Reward w; Copy(w.item, sizeof(w.item), r["item"].asString()); w.count = r["count"].asInt(1); q.reward.push_back(w); }
    q.job = v["kind"].asString() == "job";
    if (const json::Value& g = v["giver"]; g.isObject()) {
        Copy(q.giverActor, sizeof(q.giverActor), g["actor"].asString());
        q.giverHasPoint = g["x"].isNumber();
        q.gx = float(g["x"].asNumber()); q.gy = float(g["y"].asNumber()); q.gz = float(g["z"].asNumber()); q.gyaw = float(g["yaw"].asNumber());
        Copy(q.giverLook, sizeof(q.giverLook), g["look"].asString());
        Copy(q.giverAnim, sizeof(q.giverAnim), g["anim"].isString() ? g["anim"].asString() : std::string("idle"));
        LFrom(g["name"], q.giverName);
        q.giverMarker = g["marker"].asBool(true);
    }
    Copy(q.uniform, sizeof(q.uniform), v["uniform"].asString());
    q.pay = v["pay"].asInt(q.job ? 0 : 5);
    q.payout = v["payout"].asString() == "each" ? 0 : 1;
    q.cycles = std::max(0, v["cycles"].asInt(0));
    for (auto& j : v["steps"].elements()) {
        Step s;
        s.type = IndexOf(kStepKeys, kStepCount, j["type"].asString(), 0);
        LFrom(j["text"], s.text);
        s.hasPoint = j["x"].isNumber();
        s.x = float(j["x"].asNumber()); s.y = float(j["y"].asNumber()); s.z = float(j["z"].asNumber());
        s.radius = float(j["radius"].asNumber(s.type == 1 ? 6 : 5));
        s.route = j["route"].asBool(true);
        s.seconds = float(j["seconds"].asNumber(10));
        LFrom(j["meter"], s.meter);
        const json::Value& ev = j["event"];
        auto addEvent = [&](const std::string& e) { const int i = IndexOf(kEventKeys, 8, e, -1); if (i >= 0) s.events |= 1u << i; };
        if (ev.isString()) addEvent(ev.asString()); else for (auto& e : ev.elements()) addEvent(e.asString());
        s.count = j["count"].asInt(0);
        Copy(s.item, sizeof(s.item), j["item"].asString()); Copy(s.cls, sizeof(s.cls), j["class"].asString());
        for (auto& id : j["ids"].elements()) s.ids.push_back(id.asString());
        if (j["area"].isObject()) { s.hasArea = true; s.ax = float(j["area"]["x"].asNumber()); s.ay = float(j["area"]["y"].asNumber()); s.az = float(j["area"]["z"].asNumber()); s.ar = float(j["area"]["radius"].asNumber(5)); }
        s.marker = j["marker"].asBool(true);
        for (auto& r : j["spawn"].elements()) {
            SpawnRow row; Copy(row.item, sizeof(row.item), r["item"].asString()); row.count = r["count"].asInt(1);
            row.x = float(r["x"].asNumber()); row.y = float(r["y"].asNumber()); row.z = float(r["z"].asNumber()); row.yaw = float(r["yaw"].asNumber());
            s.spawn.push_back(row);
        }
        if (const json::Value& pu = j["pickup"]; pu.isObject()) {
            s.hasPickup = pu["x"].isNumber();
            s.px = float(pu["x"].asNumber()); s.py = float(pu["y"].asNumber()); s.pz = float(pu["z"].asNumber()); s.pr = float(pu["radius"].asNumber(3));
            s.boxes = std::max(1, pu["boxes"].asInt(4));
            Copy(s.model, sizeof(s.model), pu["model"].isString() ? pu["model"].asString() : std::string("crate"));
            s.roll = IndexOf(kCarryRolls, 3, pu["roll"].asString(), 0);
            Copy(s.tag, sizeof(s.tag), pu["tag"].asString());
            LFrom(pu["name"], s.pickName);
        }
        if (const json::Value& d = j["dropoff"]; d.isObject()) {
            s.hasDrop = d["x"].isNumber();
            s.dx = float(d["x"].asNumber()); s.dy = float(d["y"].asNumber()); s.dz = float(d["z"].asNumber()); s.dr = float(d["radius"].asNumber(4));
            LFrom(d["name"], s.dropName);
        }
        if (s.type == 3 && s.count < 1) s.count = 1;
        if (s.type == 4) {
            Copy(s.actor, sizeof(s.actor), j["actor"].asString());
            LFrom(j["speaker"], s.speaker);
            for (auto& l : j["lines"].elements()) { s.lines.emplace_back(); LFrom(l, s.lines.back()); }
            if (!j["radius"].isNumber()) s.radius = 3;
        }
        q.steps.push_back(s);
    }
}
bool WriteFile(const fs::path& file, const std::string& text) {
    std::error_code ec; fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.wstring() + L".tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << text; if (!out) return false; }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
bool ValidId(const char* id) {
    if (!id[0]) return false;
    for (const char* c = id; *c; ++c) if (!(isalnum(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-')) return false;
    return true;
}

// ---------------------------------------------------------------- world edits: JSON and files
json::Value TJson(const WT& t, json::Value v = json::Value::Object()) {
    v.set("x", double(std::round(t.x))); v.set("y", double(std::round(t.y))); v.set("z", double(std::round(t.z)));
    if (std::fabs(t.pitch) > 0.05f) v.set("pitch", double(std::round(t.pitch * 10) / 10));
    if (std::fabs(t.yaw) > 0.05f) v.set("yaw", double(std::round(t.yaw * 10) / 10));
    if (std::fabs(t.roll) > 0.05f) v.set("roll", double(std::round(t.roll * 10) / 10));
    if (std::fabs(t.scale - 1) > 0.001f) v.set("scale", double(t.scale));
    return v;
}
WT TFrom(const json::Value& v) {
    WT t; t.x = float(v["x"].asNumber()); t.y = float(v["y"].asNumber()); t.z = float(v["z"].asNumber());
    t.pitch = float(v["pitch"].asNumber()); t.yaw = float(v["yaw"].asNumber()); t.roll = float(v["roll"].asNumber()); t.scale = float(v["scale"].asNumber(1));
    return t;
}
json::Value WorldJson(const World& w) {
    json::Value v = json::Value::Object();
    json::Value r = json::Value::Array(); for (auto& k : w.remove) r.push(k); v.set("remove", r);
    json::Value m = json::Value::Object(); for (auto& [k, t] : w.move) m.set(k, TJson(t)); v.set("move", m);
    json::Value a = json::Value::Array();
    for (auto& e : w.add) {
        json::Value j = json::Value::Object(); j.set("uid", e.uid); j.set("asset", e.asset); if (!e.name.empty()) j.set("name", e.name);
        if (e.light) {
            json::Value l = json::Value::Object();
            json::Value c = json::Value::Array(); for (float k : e.l.color) c.push(double(std::round(k * 1000) / 1000)); l.set("color", c);
            l.set("intensity", double(std::round(e.l.intensity))); l.set("radius", double(e.l.radius));
            if (e.asset == "light:spot") l.set("cone", double(e.l.cone));
            if (e.l.shadows) l.set("shadows", true);
            j.set("light", l);
        }
        a.push(TJson(e.t, j));
    }
    v.set("add", a);
    json::Value l = json::Value::Object();
    for (auto& [name, items] : w.logic) {
        const LDecl* d = FindDecl(name);
        auto one = [&](const LItem& it) {
            json::Value j = json::Value::Object();
            j.set("x", double(it.t.x)); j.set("y", double(it.t.y)); j.set("z", double(it.t.z)); j.set("yaw", double(it.t.yaw));
            if (!d || d->trigger) j.set("radius", double(it.radius));
            return j;
        };
        if (d && !d->list && items.size() == 1) { l.set(name, one(items[0])); continue; }
        json::Value list = json::Value::Array(); for (auto& it : items) list.push(one(it));
        l.set(name, list);
    }
    if (!w.logic.empty()) v.set("logic", l);
    if (!w.actors.empty()) {
        json::Value list = json::Value::Array();
        for (auto& npc : w.actors) {
            json::Value j = json::Value::Object();
            j.set("id", std::string(npc.id)); if (npc.group[0]) j.set("group", std::string(npc.group));
            j.set("x", double(std::round(npc.t.x))); j.set("y", double(std::round(npc.t.y))); j.set("z", double(std::round(npc.t.z))); j.set("yaw", double(std::round(npc.t.yaw)));
            j.set("look", std::string(npc.look)); j.set("anim", std::string(npc.anim));
            if (npc.name[0]) j.set("name", std::string(npc.name));
            if (!npc.solid) j.set("solid", false);
            if (npc.world >= 0) j.set("world", double(npc.world));
            list.push(j);
        }
        v.set("actors", list);
    }
    if (!w.zones.empty()) {
        json::Value list = json::Value::Array(), zombieList = json::Value::Array();
        for (auto& z : w.zones) {
            json::Value j = json::Value::Object();
            if (z.zombie) {
                j.set("name", std::string(z.name));
                j.set("x", double(std::round(z.t.x))); j.set("y", double(std::round(z.t.y))); j.set("z", double(std::round(z.t.z)));
                j.set("radius", double(z.radius));
                json::Value types = json::Value::Array();
                for (const char* p = z.types; *p;) {
                    while (*p == ',' || *p == ' ') ++p;
                    const char* e = p; while (*e && *e != ',' && *e != ' ') ++e;
                    if (e > p) types.push(std::string(p, e));
                    p = e;
                }
                j.set("types", types); j.set("max", z.max); j.set("respawn", double(z.respawn)); j.set("wake", double(z.wake));
                if (z.world > 0) j.set("world", double(z.world));
                if (!z.enabled) j.set("enabled", false);
                zombieList.push(j);
                continue;
            }
            j.set("name", std::string(z.name)); j.set("type", std::string(z.pvp ? "pvp" : "pve")); j.set("shape", std::string(z.box ? "box" : "circle"));
            j.set("x", double(std::round(z.t.x))); j.set("y", double(std::round(z.t.y))); j.set("z", double(std::round(z.t.z)));
            if (z.box) { j.set("yaw", double(std::round(z.t.yaw * 10) / 10)); j.set("length", double(z.length)); j.set("width", double(z.width)); }
            else j.set("radius", double(z.radius));
            j.set("height", double(z.height));
            if (z.world >= 0) j.set("world", double(z.world));
            if (!z.enabled) j.set("enabled", false);
            if (!z.notify) j.set("notify", false);
            if (z.text[0]) j.set("text", std::string(z.text));
            list.push(j);
        }
        if (list.size()) v.set("zones", list);
        if (zombieList.size()) v.set("zombieZones", zombieList);
    }
    if (!w.carries.empty()) {
        json::Value list = json::Value::Array();
        for (auto& c : w.carries) {
            json::Value j = json::Value::Object();
            j.set("key", std::string(c.key)); j.set("model", std::string(c.model));
            j.set("x", double(std::round(c.t.x))); j.set("y", double(std::round(c.t.y))); j.set("z", double(std::round(c.t.z))); j.set("yaw", double(std::round(c.t.yaw)));
            if (c.world >= 0) j.set("world", double(c.world));
            if (c.tag[0]) j.set("tag", std::string(c.tag));
            if (c.roll) j.set("roll", std::string(kCarryRolls[c.roll]));
            list.push(j);
        }
        v.set("carries", list);
    }
    {
        json::Value vehicles = json::Value::Array(), doors = json::Value::Array(), loot = json::Value::Array();
        auto at = [](json::Value& j, const WT& t, int inWorld, bool withWorld) {
            j.set("x", double(std::round(t.x))); j.set("y", double(std::round(t.y))); j.set("z", double(std::round(t.z))); j.set("yaw", double(std::round(t.yaw)));
            if (withWorld) j.set("world", double(inWorld));
        };
        for (auto& e : w.ents) {
            json::Value j = json::Value::Object();
            j.set("key", std::string(e.key));
            if (e.type == 0) {
                j.set("model", std::string(e.model)); at(j, e.t, e.world, e.world != 0);
                if (e.locked) j.set("locked", true);
                if (e.owner[0]) j.set("owner", std::string(e.owner));
                if (e.blip) j.set("blip", std::string(kVehicleBlips[e.blip]));
                if (e.fuel < 1) j.set("fuel", double(std::round(e.fuel * 100) / 100));
                vehicles.push(j);
            } else if (e.type == 1) {
                if (e.text[0]) j.set("text", std::string(e.text));
                if (e.locked) j.set("locked", true);
                json::Value outside = json::Value::Object(), inside = json::Value::Object();
                at(outside, e.t, e.world, true); at(inside, e.t2, e.world2, true);
                j.set("a", outside); j.set("b", inside);
                doors.push(j);
            } else {
                j.set("item", std::string(e.item)); if (e.count != 1) j.set("count", e.count);
                at(j, e.t, e.world, e.world >= 0);
                j.set("respawn", e.respawn);
                if (e.tag[0]) j.set("tag", std::string(e.tag));
                loot.push(j);
            }
        }
        if (vehicles.size()) v.set("vehicles", vehicles);
        if (doors.size()) v.set("doors", doors);
        if (loot.size()) v.set("loot", loot);
    }
    if (!w.pvp) { json::Value rules = json::Value::Object(); rules.set("pvp", false); v.set("rules", rules); }
    if (w.env.set) {
        json::Value e = json::Value::Object();
        e.set("time", double(w.env.time)); e.set("paused", w.env.paused);
        if (w.env.weather < 0) e.set("weather", std::string("auto")); else e.set("weather", double(w.env.weather));
        v.set("environment", e);
    }
    return v;
}
void WorldFrom(const json::Value& v, World& w) {
    w = World{};
    for (auto& k : v["remove"].elements()) w.remove.push_back(k.asString());
    for (auto& [k, t] : v["move"].items()) w.move[k] = TFrom(t);
    for (auto& e : v["add"].elements()) {
        WAdd a; a.uid = e["uid"].asString(); a.asset = e["asset"].asString(); a.name = e["name"].asString(); a.t = TFrom(e);
        a.light = a.asset.rfind("light:", 0) == 0;
        if (const json::Value& l = e["light"]; a.light && l.isObject()) {
            if (l["color"].isArray() && l["color"].size() >= 3) for (int k = 0; k < 3; ++k) a.l.color[k] = float(l["color"][size_t(k)].asNumber());
            a.l.intensity = float(l["intensity"].asNumber(a.l.intensity)); a.l.radius = float(l["radius"].asNumber(a.l.radius));
            a.l.cone = float(l["cone"].asNumber(a.l.cone)); a.l.shadows = l["shadows"].asBool(false);
        }
        w.add.push_back(a);
    }
    if (const json::Value& e = v["environment"]; e.isObject()) {
        w.env.set = true; w.env.time = float(e["time"].asNumber(12)); w.env.paused = e["paused"].asBool(false);
        w.env.weather = e["weather"].isNumber() ? int(e["weather"].asNumber()) : -1;
    }
    for (auto& [name, e] : v["logic"].items()) {
        auto one = [](const json::Value& j) { LItem it; it.t.x = float(j["x"].asNumber()); it.t.y = float(j["y"].asNumber()); it.t.z = float(j["z"].asNumber()); it.t.yaw = float(j["yaw"].asNumber()); it.radius = float(j["radius"].asNumber(5)); return it; };
        auto& items = w.logic[name];
        if (e.isArray()) for (auto& j : e.elements()) items.push_back(one(j)); else if (e.isObject()) items.push_back(one(e));
    }
    for (auto& j : v["actors"].elements()) {
        WActor a;
        Copy(a.id, sizeof a.id, j["id"].asString()); Copy(a.group, sizeof a.group, j["group"].asString());
        a.t.x = float(j["x"].asNumber()); a.t.y = float(j["y"].asNumber()); a.t.z = float(j["z"].asNumber()); a.t.yaw = float(j["yaw"].asNumber());
        Copy(a.look, sizeof a.look, j["look"].asString()); Copy(a.anim, sizeof a.anim, j["anim"].isString() ? j["anim"].asString() : std::string("idle"));
        Copy(a.name, sizeof a.name, j["name"].asString());
        a.solid = j["solid"].asBool(true); a.world = j["world"].isNumber() ? int(j["world"].asNumber()) : -1;
        if (a.id[0]) w.actors.push_back(a);
    }
    for (auto& j : v["zones"].elements()) {
        WZone z;
        Copy(z.name, sizeof z.name, j["name"].asString());
        z.pvp = j["type"].asString() == "pvp";
        z.box = j["shape"].asString() == "box" || (!j["shape"].isString() && (j["width"].isNumber() || j["length"].isNumber()));
        z.t.x = float(j["x"].asNumber()); z.t.y = float(j["y"].asNumber()); z.t.z = float(j["z"].asNumber()); z.t.yaw = float(j["yaw"].asNumber());
        z.radius = float(j["radius"].asNumber(20)); z.length = float(j["length"].asNumber(40)); z.width = float(j["width"].asNumber(40));
        z.height = float(j["height"].asNumber(50)); z.world = j["world"].isNumber() ? int(j["world"].asNumber()) : -1;
        z.enabled = j["enabled"].asBool(true); z.notify = j["notify"].asBool(true); Copy(z.text, sizeof z.text, j["text"].asString());
        if (z.name[0]) w.zones.push_back(z);
    }
    for (auto& j : v["zombieZones"].elements()) {
        WZone z; z.zombie = true; z.world = 0;
        Copy(z.name, sizeof z.name, j["name"].asString());
        z.t.x = float(j["x"].asNumber()); z.t.y = float(j["y"].asNumber()); z.t.z = float(j["z"].asNumber());
        z.radius = float(j["radius"].asNumber(30)); z.max = j["max"].asInt(6); z.respawn = float(j["respawn"].asNumber(60)); z.wake = float(j["wake"].asNumber(150));
        std::string types;
        for (auto& t : j["types"].elements()) if (t.isString()) types += (types.empty() ? "" : ", ") + t.asString();
        if (j["type"].isString()) types = j["type"].asString();
        Copy(z.types, sizeof z.types, types.empty() ? std::string("Swarmer") : types);
        z.world = j["world"].isNumber() ? int(j["world"].asNumber()) : 0; z.enabled = j["enabled"].asBool(true);
        if (z.name[0]) w.zones.push_back(z);
    }
    for (auto& j : v["carries"].elements()) {
        WCarry c;
        Copy(c.key, sizeof c.key, j["key"].asString()); Copy(c.tag, sizeof c.tag, j["tag"].asString());
        Copy(c.model, sizeof c.model, j["model"].isString() ? j["model"].asString() : std::string("crate"));
        c.t.x = float(j["x"].asNumber()); c.t.y = float(j["y"].asNumber()); c.t.z = float(j["z"].asNumber()); c.t.yaw = float(j["yaw"].asNumber());
        c.world = j["world"].isNumber() ? int(j["world"].asNumber()) : -1;
        c.roll = IndexOf(kCarryRolls, 3, j["roll"].asString(), 0);
        if (c.key[0]) w.carries.push_back(c);
    }
    auto at = [](const json::Value& j, WT& t) { t.x = float(j["x"].asNumber()); t.y = float(j["y"].asNumber()); t.z = float(j["z"].asNumber()); t.yaw = float(j["yaw"].asNumber()); };
    for (auto& j : v["vehicles"].elements()) {
        WEnt e; e.type = 0; Copy(e.key, sizeof e.key, j["key"].asString()); at(j, e.t);
        Copy(e.model, sizeof e.model, j["model"].isString() ? j["model"].asString() : std::string("bike"));
        e.world = j["world"].isNumber() ? int(j["world"].asNumber()) : 0;
        e.locked = j["locked"].asBool(false); Copy(e.owner, sizeof e.owner, j["owner"].asString());
        e.blip = IndexOf(kVehicleBlips, 3, j["blip"].asString(), 0); e.fuel = float(j["fuel"].asNumber(1));
        if (e.key[0]) w.ents.push_back(e);
    }
    for (auto& j : v["doors"].elements()) {
        WEnt e; e.type = 1; Copy(e.key, sizeof e.key, j["key"].asString()); Copy(e.text, sizeof e.text, j["text"].asString());
        e.locked = j["locked"].asBool(false);
        at(j["a"], e.t); e.world = j["a"]["world"].isNumber() ? int(j["a"]["world"].asNumber()) : 0;
        at(j["b"], e.t2); e.world2 = j["b"]["world"].isNumber() ? int(j["b"]["world"].asNumber()) : 0;
        if (e.key[0]) w.ents.push_back(e);
    }
    for (auto& j : v["loot"].elements()) {
        WEnt e; e.type = 2; Copy(e.key, sizeof e.key, j["key"].asString()); at(j, e.t);
        Copy(e.item, sizeof e.item, j["item"].asString()); e.count = std::max(1, j["count"].asInt(1));
        e.world = j["world"].isNumber() ? int(j["world"].asNumber()) : -1;
        e.respawn = std::max(0, j["respawn"].asInt(300)); Copy(e.tag, sizeof e.tag, j["tag"].asString());
        if (e.key[0]) w.ents.push_back(e);
    }
    w.pvp = v["rules"]["pvp"].asBool(true);
}
// ---------------------------------------------------------------- vehicles, doors, loot (world.json)
// Selection id: "v:key", "l:key", "d:key:a" / "d:key:b" (the door's outside / inside end).
WEnt* EntOf(const std::string& id, bool* inside = nullptr) {
    if (id.size() < 3 || id[1] != ':') return nullptr;
    const int type = id[0] == 'v' ? 0 : id[0] == 'd' ? 1 : 2;
    std::string key = id.substr(2);
    bool b = false;
    if (type == 1 && key.size() > 2 && key[key.size() - 2] == ':') { b = key.back() == 'b'; key.resize(key.size() - 2); }
    if (inside) *inside = b;
    for (auto& e : world.ents) if (e.type == type && key == e.key) return &e;
    return nullptr;
}
std::string EntId(const WEnt& e, bool inside = false) { return std::string(kEntPrefix[e.type]) + ":" + e.key + (e.type == 1 ? (inside ? ":b" : ":a") : ""); }
void SelectEnt(const WEnt& e, bool inside = false) { wsel = {7, EntId(e, inside), "", inside ? e.t2 : e.t}; }
std::string FreeEntKey(int type, const std::string& base) {
    for (int n = 1;; ++n) {
        const std::string key = base + std::to_string(n);
        bool taken = false; for (auto& e : world.ents) taken |= e.type == type && key == e.key;
        if (!taken) return key;
    }
}
// ---------------------------------------------------------------- boxes to carry (world.json "carries")
WCarry* CarryByKey(const std::string& key) { for (auto& c : world.carries) if (key == c.key) return &c; return nullptr; }
std::string FreeCarryKey(const std::string& base) {
    for (int n = 1;; ++n) { const std::string key = base + std::to_string(n); if (!CarryByKey(key)) return key; }
}
void SelectCarry(const std::string& key) { if (WCarry* c = CarryByKey(key)) wsel = {6, key, "", c->t}; else wsel = {}; }
const char* CarryModelName(const char* model) {
    int n = 0; const carry::ModelInfo* m = carry::Models(n);
    for (int i = 0; i < n; ++i) if (std::strcmp(m[i].name, model) == 0) return english ? m[i].en : m[i].ru;
    return model;
}
WZone* ZoneOf(const std::string& id) {
    const int i = id.empty() ? -1 : std::atoi(id.c_str());
    return i >= 0 && i < int(world.zones.size()) ? &world.zones[size_t(i)] : nullptr;
}
void SelectZone(int index) {
    if (index < 0 || index >= int(world.zones.size())) { wsel = {}; return; }
    wsel = {5, std::to_string(index), "", world.zones[size_t(index)].t};
}
std::string FreeZoneName(const std::string& base) {
    for (int n = 1;; ++n) {
        const std::string name = base + std::to_string(n);
        bool used = false; for (auto& z : world.zones) used |= name == z.name;
        if (!used) return name;
    }
}
// ---------------------------------------------------------------- the mode's NPCs (world.json "actors")
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(std::max(n, 0)), wchar_t(0));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n);
    return out;
}
WActor* ActorById(const std::string& id) { for (auto& a : world.actors) if (id == a.id) return &a; return nullptr; }
std::string FreeActorId(const std::string& base) {
    for (int n = 1;; ++n) { const std::string id = base + std::to_string(n); if (!ActorById(id)) return id; }
}
void SelectActor(const std::string& id) { if (WActor* a = ActorById(id)) wsel = {4, id, "", a->t}; else wsel = {}; }
std::string ModeSkinLook(const std::string& id);  // a skin id of the mode's skins.json (below)
std::vector<std::pair<std::string, std::string>> ModeSkinChoices();  // (id, label) of the mode's skins and uniforms
// The look an NPC wears: a skins.json id, a look name, a path or a "skin:" string ("": Deacon).
std::string ActorLook(const std::string& look) {
    if (look.empty()) return "/Game/Characters/Deacon/looks/deacon_standard.deacon_standard";
    const std::string skin = ModeSkinLook(look);
    return skin.empty() ? skins::LookPath(look) : skin;
}
// data/anims.txt: animation names (short names first) and their assets.
const std::vector<std::pair<std::string, std::string>>& Anims() {
    static std::vector<std::pair<std::string, std::string>> list;
    static std::once_flag once;
    std::call_once(once, [] {
        std::ifstream in(fs::path(profile::Root()) / L"data" / L"anims.txt");
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            const size_t t1 = line.find('\t'); if (t1 == std::string::npos) continue;
            const size_t t2 = line.find('\t', t1 + 1);
            list.emplace_back(line.substr(0, t1), line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1));
        }
    });
    return list;
}
std::string AnimPath(const std::string& name) {
    if (!name.empty() && name[0] == '/') return name;
    for (auto& [n, path] : Anims()) if (_stricmp(n.c_str(), name.c_str()) == 0) return path;
    return {};
}

// The logic objects a mode's scripts ask for (names written as plain strings).
void ScanLogic(const std::string& m) {
    logicDecls.clear();
    static const std::regex call(R"(logic\s*\.\s*(triggers|trigger|points|point|actors|actor)\s*\(\s*["']([^"']+)["']([^)]*))");
    static const std::regex radius(R"(radius\s*=\s*([0-9.]+))");
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::path(profile::Root()) / L"modes" / m, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != L".lua") continue;
        std::ifstream in(it->path(), std::ios::binary); std::stringstream ss; ss << in.rdbuf();
        const std::string text = ss.str();
        for (auto c = std::sregex_iterator(text.begin(), text.end(), call); c != std::sregex_iterator(); ++c) {
            const std::string kind = (*c)[1], name = (*c)[2], rest = (*c)[3];
            if (FindDecl(name)) continue;
            LDecl d; d.name = name; d.list = kind.back() == 's'; d.trigger = kind.rfind("trigger", 0) == 0; d.actor = kind.rfind("actor", 0) == 0;
            std::smatch r; if (std::regex_search(rest, r, radius)) d.radius = std::stof(r[1]);
            logicDecls.push_back(d);
        }
    }
    // Every mode has it: where players choose their skin (player:customize() takes them there).
    if (!FindDecl("wardrobe")) logicDecls.push_back({"wardrobe", false, false, false, 5});
    // And the spawn points: the server puts joining and respawning players on one of them (else server.ini's).
    if (!FindDecl("spawn")) logicDecls.push_back({"spawn", true, false, false, 5});
}
fs::path WorldFile(const std::string& m) { return fs::path(profile::Root()) / L"modes" / m / L"world.json"; }

// ---------------------------------------------------------------- game thread work
void RefreshQuestList() {
    questFiles.clear();
    std::error_code ec;
    for (auto it = fs::directory_iterator(QuestDir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == L".json") questFiles.push_back(it->path().stem().string());
    std::sort(questFiles.begin(), questFiles.end());
}
bool SaveQuest() {
    if (!ValidId(quest.id)) { Toast(T("Имя файла квеста: латиница, цифры, _ и -")); return false; }
    if (!WriteFile(QuestDir() / (std::string(quest.id) + ".json"), ToJson(quest).dump())) { Toast(T("Не удалось сохранить")); return false; }
    if (!loadedId.empty() && loadedId != quest.id) { std::error_code ec; fs::remove(QuestDir() / (loadedId + ".json"), ec); }
    loadedId = quest.id;
    RefreshQuestList();
    Toast(T("Сохранено: quests/") + loadedId + ".json");
    return true;
}
void LoadQuest(const std::string& id) {
    std::ifstream in(QuestDir() / (id + ".json"), std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (!json::Value::Parse(ss.str(), v) || !v.isObject()) { Toast("quests/" + id + T(".json повреждён")); return; }
    FromJson(v, quest);
    if (!quest.id[0]) Copy(quest.id, sizeof(quest.id), id);
    loadedId = id; questOpen = true; selected = quest.steps.empty() ? -1 : 0; previewDirty = true;
    ResetHistory();
}
void WritePreview() {
    const fs::path file = fs::path(profile::Root()) / L"quest_preview.json";
    if (!questOpen || mode != 1) { std::error_code ec; fs::remove(file, ec); return; }  // (the game clears the markers)
    json::Value v = ToJson(quest);
    v.set("selected", selected);
    // Only real changes reach the game (it re-reads the file when it changes).
    static std::string last;
    std::string text = v.dump();
    if (text == last) return;
    last = std::move(text);
    WriteFile(file, last);
}
std::string TextToString(const void* text) {
    static UObject* lib = nullptr;
    if (!IsValid(lib)) lib = FindObject("/Script/Engine.Default__KismetTextLibrary");
    auto f = lib ? Fn(lib, "Conv_TextToString") : nullptr; if (!f) return {};
    Params p(f);
    if (auto prop = FindProperty(f, "InText")) std::memcpy(static_cast<uint8_t*>(p.Data()) + Field<int>(prop, off::UProperty_Offset), text, 0x18);
    Call(lib, "Conv_TextToString", &p);
    const FString s = p.Get<FString>("ReturnValue");
    if (!s.Data || s.Num <= 1) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.Data, s.Num - 1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, s.Data, s.Num - 1, out.data(), n, nullptr, nullptr);
    return out;
}
void BuildCatalog() {
    std::vector<Item> items;
    for (auto& [key, obj] : inventory::AllItems()) {
        Item it; it.key = key; it.type = Field<uint8_t>(obj, 0xA0);   // InventoryItem.BaseType
        it.name = TextToString(reinterpret_cast<const uint8_t*>(obj) + 0x38);  // DisplayName
        if (it.name.empty() || it.name.rfind("kWeapon", 0) == 0) it.name = key;
        // kConsumableID_Bandage_Small → "Bandage Small"
        std::string e = key; const size_t id = e.find("ID_"); if (id != std::string::npos) e = e.substr(id + 3);
        std::replace(e.begin(), e.end(), '_', ' ');
        it.english = e;
        items.push_back(std::move(it));
    }
    if (items.empty()) return;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
    std::lock_guard g(lock);
    catalog = std::move(items); catalogReady = true;
    dg::Log("Editor item library: %zu items", catalog.size());
}

FRotator ActorRotation(UObject* a) { auto f = Fn(a, "K2_GetActorRotation"); if (!f) return {}; Params p(f); Call(a, "K2_GetActorRotation", &p); return p.Get<FRotator>("ReturnValue"); }
// Logic objects shown in the world: real objects, so they sit still and the ground hides what is
// below it. A trigger: a beam in its centre and a ring of posts on its radius; a point: a post and a
// cone pointing its way. Editor only, no collision.
struct LVis { UObject* actor = nullptr; UObject* label = nullptr; UObject* material = nullptr; std::string geo; int color = -1; std::string text; };
std::map<std::string, LVis> logicVis;  // "name#index"
UObject* EngineAsset(const char* path) { UObject* o = FindObject(path); return o ? o : LoadAsset(path); }
// An asset kept for good (rooted): a loaded asset nothing else holds is collected after a level change,
// and a cached pointer to it would crash the next call.
UObject* KeptAsset(UObject*& cache, const char* path) { if (!IsValid(cache)) { cache = EngineAsset(path); if (cache) AddToRoot(cache); } return cache; }
UObject* EditorStatics() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }
UObject* SpawnBare(UObject* cls, const FVector& at, UObject* context) {
    if (!cls || !EditorStatics()) return nullptr;
    FTransform t; t.Translation = at;
    Params begin(Fn(EditorStatics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(EditorStatics(), "BeginSpawningActorFromClass", &begin);
    UObject* a = begin.Get<UObject*>("ReturnValue");
    if (!a) return nullptr;
    if (UObject* root = Field<UObject*>(a, off::Actor_RootComponent)) {  // movable before it registers
        for (auto c = root->Class; c; c = SuperOf(c)) if (auto prop = FindProperty(c, "Mobility")) { *(reinterpret_cast<uint8_t*>(root) + Field<int>(prop, off::UProperty_Offset)) = 2; break; }
    }
    Params finish(Fn(EditorStatics(), "FinishSpawningActor")); finish.Set("Actor", a); finish.Set("SpawnTransform", t);
    Call(EditorStatics(), "FinishSpawningActor", &finish);
    if (auto f = Fn(a, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(a, "SetActorEnableCollision", &p); }
    return a;
}
UObject* AddShape(UObject* actor, UObject* mesh, UObject* material, const FVector& loc, const FRotator& rot, const FVector& scale) {
    static UObject* cls = FindClass("StaticMeshComponent");
    UObject* c = cls && mesh ? NewObject(cls, actor) : nullptr;
    if (!c) return nullptr;
    for (auto k = c->Class; k; k = SuperOf(k)) if (auto prop = FindProperty(k, "Mobility")) { *(reinterpret_cast<uint8_t*>(c) + Field<int>(prop, off::UProperty_Offset)) = 2; break; }
    if (auto f = Fn(c, "SetStaticMesh")) { Params p(f); p.Set("NewMesh", mesh); Call(c, "SetStaticMesh", &p); }
    RegisterComponent(c);
    if (auto f = Fn(c, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 0); Call(c, "SetCollisionEnabled", &p); }
    if (auto f = Fn(c, "K2_AttachTo")) { Params p(f); p.Set("InParent", Field<UObject*>(actor, off::Actor_RootComponent)); p.Set<uint8_t>("AttachType", 0); Call(c, "K2_AttachTo", &p); }
    if (auto f = Fn(c, "K2_SetRelativeLocationAndRotation")) { Params p(f); p.Set("NewLocation", loc); p.Set("NewRotation", rot); Call(c, "K2_SetRelativeLocationAndRotation", &p); }
    if (auto f = Fn(c, "SetRelativeScale3D")) { Params p(f); p.Set("NewScale3D", scale); Call(c, "SetRelativeScale3D", &p); }
    if (material) if (auto f = Fn(c, "SetMaterial")) { Params p(f); p.Set<int>("ElementIndex", 0); p.Set("Material", material); Call(c, "SetMaterial", &p); }
    return c;
}
void DestroyVis(LVis& v) {
    for (UObject* a : {v.actor, v.label}) if (IsValid(a)) Call(a, "K2_DestroyActor");
    v = LVis{};
}
void UpdateLogicVisuals(UObject* pawn) {
    // shape: 0 a point, 1 a ring of `radius` m, 2 a box of halfX x halfY m (zones); zone: taller posts.
    struct Want { std::string key, text; LItem item; bool trigger; int color; int shape = 0; float halfX = 0, halfY = 0; bool zone = false; };
    std::vector<Want> wants;
    FVector camAt{};
    {
        std::lock_guard g(lock);
        camAt = cam.loc;
        if (mode == 0 && !worldMode.empty())
            for (auto& [name, items] : world.logic) {
                const LDecl* d = FindDecl(name);
                for (int i = 0; i < int(items.size()); ++i) {
                    const std::string key = name + "#" + std::to_string(i);
                    const bool trig = !d || d->trigger;
                    wants.push_back({key, items.size() > 1 || (d && d->list) ? name + " " + std::to_string(i + 1) : name, items[i], trig,
                                     wsel.kind == 3 && wsel.id == key ? 2 : trig ? 0 : 1, trig ? 1 : 0});
                }
            }
        if (mode == 0 && !worldMode.empty())
            for (auto& e : world.ents) for (int end = 0; end < (e.type == 1 ? 2 : 1); ++end) {
                static const char* const marks[] = {"[car] ", "[door] ", "[loot] "};
                std::string text = marks[e.type] + std::string(e.key);
                if (e.type == 0) text += std::string("  ") + e.model;
                if (e.type == 1) text += end ? "  (in)" : "  (out)";
                if (e.type == 2 && e.item[0]) text += std::string("  ") + e.item;
                const int inWorld = end ? e.world2 : e.world;
                if (inWorld) text += "  w" + std::to_string(inWorld);
                const std::string id = EntId(e, end == 1);
                Want want{"ent#" + id, text, {}, false, wsel.kind == 7 && wsel.id == id ? 2 : 5 + e.type, 0};
                want.item.t = end ? e.t2 : e.t;
                wants.push_back(want);
            }
        if (mode == 0 && !worldMode.empty())
            for (int i = 0; i < int(world.zones.size()); ++i) {
                const WZone& z = world.zones[size_t(i)];
                Want w{"zone#" + std::to_string(i), std::string(z.name) + (z.zombie ? "  (zombies)" : z.pvp ? "  (PvP)" : "  (PvE)") + (z.enabled ? "" : " -"), {}, true,
                       wsel.kind == 5 && wsel.id == std::to_string(i) ? 2 : z.zombie ? 8 : z.pvp ? 4 : 3, z.box && !z.zombie ? 2 : 1, z.length / 2, z.width / 2, true};
                w.item.t = z.t; w.item.t.pitch = w.item.t.roll = 0; w.item.radius = z.radius;
                if (!z.box) w.item.t.yaw = 0;
                wants.push_back(w);
            }
    }
    // Gone ones first.
    for (auto it = logicVis.begin(); it != logicVis.end();) {
        bool keep = false; for (auto& w : wants) if (w.key == it->first) { keep = true; break; }
        if (!keep) { DestroyVis(it->second); it = logicVis.erase(it); } else ++it;
    }
    if (wants.empty()) return;
    static UObject *cylinderAsset = nullptr, *coneAsset = nullptr, *baseMatAsset = nullptr;
    UObject* cylinder = KeptAsset(cylinderAsset, "/Engine/BasicShapes/Cylinder.Cylinder");
    UObject* cone = KeptAsset(coneAsset, "/Engine/BasicShapes/Cone.Cone");
    UObject* baseMat = KeptAsset(baseMatAsset, "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
    static UObject* smaCls = FindClass("StaticMeshActor");
    static UObject* textCls = FindClass("TextRenderActor");
    static const FName colorName = FindName("Color");
    for (auto& w : wants) {
        LVis& v = logicVis[w.key];
        if (v.actor && !IsValid(v.actor)) v = LVis{};
        char geo[64];
        snprintf(geo, sizeof geo, "%d %.2f %.2f %.2f", w.shape, w.shape == 1 ? w.item.radius : w.halfX, w.halfY, w.zone ? 1.f : 0.f);
        if (!v.actor || v.geo != geo) {
            DestroyVis(v);
            v.actor = SpawnBare(smaCls, {w.item.t.x, w.item.t.y, w.item.t.z}, pawn);
            if (!v.actor) continue;
            v.geo = geo;
            // One coloured material for all its shapes.
            UObject* root = Field<UObject*>(v.actor, off::Actor_RootComponent);
            UObject* first = AddShape(v.actor, cylinder, nullptr, {0, 0, 200}, {0, 0, 0}, {0.08f, 0.08f, 4.f});  // the beam, 4 m
            if (first) if (auto f = Fn(first, "CreateDynamicMaterialInstance")) {
                Params p(f); p.Set<int>("ElementIndex", 0); p.Set("SourceMaterial", baseMat); Call(first, "CreateDynamicMaterialInstance", &p);
                v.material = p.Get<UObject*>("ReturnValue");
            }
            const float postH = w.zone ? 3.f : 1.5f, postW = w.zone ? 0.1f : 0.06f;  // zones: taller posts, 3 m
            if (w.shape == 1) {
                const float r = w.item.radius * 100;
                const int posts = std::clamp(int(r * 6.2831853f / 150.f), 12, w.zone ? 128 : 64);  // about every 1.5 m
                for (int k = 0; k < posts; ++k) {
                    const float a = k * 6.2831853f / posts;
                    AddShape(v.actor, cylinder, v.material, {cosf(a) * r, sinf(a) * r, postH * 50}, {0, 0, 0}, {postW, postW, postH});
                }
            } else if (w.shape == 2) {  // posts along the box's edges, the corners included
                const float hx = w.halfX * 100, hy = w.halfY * 100;
                const FVector corners[4] = {{hx, hy, 0}, {-hx, hy, 0}, {-hx, -hy, 0}, {hx, -hy, 0}};
                const float step = std::max(150.f, 4 * (hx + hy) / 128.f);
                for (int e = 0; e < 4; ++e) {
                    const FVector a = corners[e], b = corners[(e + 1) % 4];
                    const float len = std::sqrt((b.X - a.X) * (b.X - a.X) + (b.Y - a.Y) * (b.Y - a.Y));
                    const int n = std::max(1, int(len / step));
                    for (int k = 0; k < n; ++k) {
                        const float f = float(k) / n;
                        AddShape(v.actor, cylinder, v.material, {a.X + (b.X - a.X) * f, a.Y + (b.Y - a.Y) * f, postH * 50}, {0, 0, 0}, {postW, postW, postH});
                    }
                }
            } else {
                AddShape(v.actor, cone, v.material, {80, 0, 120}, {-90, 0, 0}, {0.4f, 0.4f, 0.8f});  // points along the actor's yaw
            }
            (void)root;
            v.label = SpawnBare(textCls, {w.item.t.x, w.item.t.y, w.item.t.z + 430}, pawn);
            v.color = -1; v.text.clear();
        }
        if (auto f = Fn(v.actor, "K2_SetActorLocationAndRotation")) {
            Params p(f); p.Set("NewLocation", FVector{w.item.t.x, w.item.t.y, w.item.t.z}); p.Set("NewRotation", FRotator{0, w.item.t.yaw, 0}); p.SetBool("bTeleport", true);
            Call(v.actor, "K2_SetActorLocationAndRotation", &p);
        }
        // 0 trigger, 1 point, 2 selected, 3 PvE, 4 PvP, 5 vehicle, 6 door, 7 loot, 8 zombie zone
        static const float colors[9][4] = {{0.15f, 0.75f, 0.9f, 1}, {1.f, 0.5f, 0.15f, 1}, {1.f, 0.85f, 0.2f, 1}, {0.2f, 0.85f, 0.35f, 1}, {0.95f, 0.25f, 0.2f, 1},
                                           {0.7f, 0.35f, 0.95f, 1}, {0.25f, 0.9f, 0.8f, 1}, {0.95f, 0.7f, 0.25f, 1}, {0.6f, 0.85f, 0.15f, 1}};
        if (v.color != w.color && IsValid(v.material)) if (auto f = Fn(v.material, "SetVectorParameterValue")) {
            Params p(f); p.Set("ParameterName", colorName);
            struct { float r, g, b, a; } c{colors[w.color][0], colors[w.color][1], colors[w.color][2], colors[w.color][3]};
            p.Set("Value", c); Call(v.material, "SetVectorParameterValue", &p);
        }
        UObject* tc = IsValid(v.label) ? Field<UObject*>(v.label, off::TextRenderActor_Component) : nullptr;
        if (IsValid(tc)) {
            if (v.text != w.text || v.color != w.color) {
                const std::wstring wt(w.text.begin(), w.text.end());
                Params t(Fn(tc, "SetText")); t.Set("Value", FString{wt.c_str(), int32_t(wt.size() + 1), int32_t(wt.size() + 1)}); Call(tc, "SetText", &t);
                if (auto f = Fn(tc, "SetHorizontalAlignment")) { Params a(f); a.Set<uint8_t>("Value", 1); Call(tc, "SetHorizontalAlignment", &a); }
                if (auto f = Fn(tc, "SetWorldSize")) { Params a(f); a.Set<float>("Value", 45.f); Call(tc, "SetWorldSize", &a); }
                static const uint32_t argbs[9] = {0xFF5AD2E6, 0xFFF09646, 0xFFFFD84A, 0xFF5AE678, 0xFFF05A50, 0xFFB35AF2, 0xFF40E6CC, 0xFFF2B340, 0xFF9AD926};
                const uint32_t argb = argbs[w.color];
                if (auto f = Fn(tc, "SetTextRenderColor")) { Params a(f); a.Set<uint32_t>("Value", argb); Call(tc, "SetTextRenderColor", &a); }
                v.text = w.text;
            }
            const FVector at{w.item.t.x, w.item.t.y, w.item.t.z + 430};
            const float yaw = std::atan2(camAt.Y - at.Y, camAt.X - at.X) * 57.2957795f;
            if (auto f = Fn(v.label, "K2_SetActorLocationAndRotation")) { Params p(f); p.Set("NewLocation", at); p.Set("NewRotation", FRotator{0, yaw, 0}); p.SetBool("bTeleport", true); Call(v.label, "K2_SetActorLocationAndRotation", &p); }
        }
        v.color = w.color;
    }
}
// Outline of the selection: the game's own survival-vision outline. Selected meshes render custom
// depth with stencil 2, and the controller's FocusPostHighlightOnly post process draws it.
std::vector<UObject*> outlined;  // primitive components currently outlined
// NPCs of the mode as they will stand: the look, the animation, the id and name above them.
struct APreview { UObject* body = nullptr; UObject* label = nullptr; std::string look, anim, text; };
std::map<std::string, APreview> actorPreviews;
void UpdateActorPreviews(UObject* pawn) {
    struct Want { std::string id, look, anim, text; WT t; };
    std::vector<Want> wants;
    FVector camAt{};
    {
        std::lock_guard g(lock);
        camAt = cam.loc;
        if (!worldMode.empty())
            for (auto& a : world.actors) wants.push_back({a.id, ActorLook(a.look), AnimPath(a.anim[0] ? a.anim : "idle"), std::string(a.id) + (a.name[0] ? std::string("  ") + a.name : std::string()), a.t});
    }
    for (auto it = actorPreviews.begin(); it != actorPreviews.end();) {
        bool keep = false; for (auto& w : wants) if (w.id == it->first) { keep = true; break; }
        if (!keep) { for (UObject* o : {it->second.body, it->second.label}) if (IsValid(o)) Call(o, "K2_DestroyActor"); it = actorPreviews.erase(it); } else ++it;
    }
    static UObject* skelCls = FindClass("SkeletalMeshActor");
    static UObject* textCls = FindClass("TextRenderActor");
    for (auto& w : wants) {
        APreview& p = actorPreviews[w.id];
        // A label lives and goes with its body: never one left over from an earlier body.
        if (p.body && !IsValid(p.body)) { if (IsValid(p.label)) Call(p.label, "K2_DestroyActor"); p = APreview{}; }
        if (p.label && !IsValid(p.label)) { p.label = nullptr; p.text.clear(); }
        const FVector at{w.t.x, w.t.y, w.t.z};
        if (!p.body) {
            if (IsValid(p.label)) Call(p.label, "K2_DestroyActor");
            p = APreview{};
            p.body = SpawnBare(skelCls, at, pawn);
            if (!p.body) continue;
        }
        // The label first: it turns to the camera every frame, whatever happens to the body below.
        if (!p.label) { p.label = SpawnBare(textCls, {at.X, at.Y, at.Z + 215}, pawn); p.text.clear(); }
        UObject* tc = IsValid(p.label) ? Field<UObject*>(p.label, off::TextRenderActor_Component) : nullptr;
        if (IsValid(tc)) {
            if (p.text != w.text) {
                const std::wstring wt = Wide(w.text);
                Params t(Fn(tc, "SetText")); t.Set("Value", FString{wt.c_str(), int32_t(wt.size() + 1), int32_t(wt.size() + 1)}); Call(tc, "SetText", &t);
                if (auto f = Fn(tc, "SetHorizontalAlignment")) { Params a(f); a.Set<uint8_t>("Value", 1); Call(tc, "SetHorizontalAlignment", &a); }
                if (auto f = Fn(tc, "SetWorldSize")) { Params a(f); a.Set<float>("Value", 26.f); Call(tc, "SetWorldSize", &a); }
                if (auto f = Fn(tc, "SetTextRenderColor")) { Params a(f); a.Set<uint32_t>("Value", 0xFFF2D16B); Call(tc, "SetTextRenderColor", &a); }
                p.text = w.text;
            }
            const FVector top{at.X, at.Y, at.Z + 215};
            const float yaw = std::atan2(camAt.Y - top.Y, camAt.X - top.X) * 57.2957795f;
            if (auto f = Fn(p.label, "K2_SetActorLocationAndRotation")) { Params q(f); q.Set("NewLocation", top); q.Set("NewRotation", FRotator{0, yaw, 0}); q.SetBool("bTeleport", true); Call(p.label, "K2_SetActorLocationAndRotation", &q); }
        }
        UObject* mesh = Field<UObject*>(p.body, off::SkelMeshActor_Component);
        if (!IsValid(mesh)) continue;
        if (p.look != w.look) { if (!w.look.empty()) skins::Wear(mesh, w.look); p.look = w.look; p.anim.clear(); }
        if (!w.anim.empty()) {
            bool playing = p.anim == w.anim;
            if (playing) if (auto f = Fn(mesh, "IsPlaying")) { Params q(f); Call(mesh, "IsPlaying", &q); playing = q.GetBool("ReturnValue"); }
            if (!playing) {
                UObject* anim = EngineAsset(w.anim.c_str());
                if (anim) {
                    if (auto f = Fn(mesh, "SetAnimationMode")) { Params q(f); q.Set<uint8_t>("InAnimationMode", 1); Call(mesh, "SetAnimationMode", &q); }
                    if (auto f = Fn(mesh, "PlayAnimation")) { Params q(f); q.Set("NewAnimToPlay", anim); q.SetBool("bLooping", true); Call(mesh, "PlayAnimation", &q); }
                }
                p.anim = w.anim;
            }
        }
        if (auto f = Fn(p.body, "K2_SetActorLocationAndRotation")) { Params q(f); q.Set("NewLocation", at); q.Set("NewRotation", FRotator{0, w.t.yaw - 90.f, 0}); q.SetBool("bTeleport", true); Call(p.body, "K2_SetActorLocationAndRotation", &q); }
    }
}
// Boxes to carry as they will lie: the model's own pickup (its interaction off), the key and tag above.
struct CPreview { UObject* body = nullptr; UObject* label = nullptr; std::string model, text; WT at; };
std::map<std::string, CPreview> carryPreviews;
void UpdateCarryPreviews(UObject* pawn) {
    struct Want { std::string key, model, text; WT t; };
    std::vector<Want> wants;
    FVector camAt{};
    {
        std::lock_guard g(lock);
        camAt = cam.loc;
        if (!worldMode.empty())
            for (auto& c : world.carries) wants.push_back({c.key, c.model, std::string(c.key) + (c.tag[0] ? std::string("  [") + c.tag + "]" : std::string()), c.t});
    }
    auto destroy = [](CPreview& p) { for (UObject* o : {p.body, p.label}) if (IsValid(o)) Call(o, "K2_DestroyActor"); p = CPreview{}; };
    for (auto it = carryPreviews.begin(); it != carryPreviews.end();) {
        bool keep = false; for (auto& w : wants) if (w.key == it->first) { keep = true; break; }
        if (!keep) { destroy(it->second); it = carryPreviews.erase(it); } else ++it;
    }
    static UObject* textCls = FindClass("TextRenderActor");
    for (auto& w : wants) {
        CPreview& p = carryPreviews[w.key];
        if ((p.body && !IsValid(p.body)) || (p.body && p.model != w.model)) destroy(p);
        const FVector at{w.t.x, w.t.y, w.t.z};
        if (!p.body) {
            p.body = carry::SpawnPreview(w.model, at, w.t.yaw, pawn);
            p.model = w.model; p.at = w.t;
            if (!p.body) continue;
        }
        if (p.at.x != w.t.x || p.at.y != w.t.y || p.at.z != w.t.z || p.at.yaw != w.t.yaw) {
            if (auto f = Fn(p.body, "K2_SetActorLocationAndRotation")) { Params q(f); q.Set("NewLocation", at); q.Set("NewRotation", FRotator{0, w.t.yaw, 0}); q.SetBool("bTeleport", true); Call(p.body, "K2_SetActorLocationAndRotation", &q); }
            p.at = w.t;
        }
        if (p.label && !IsValid(p.label)) { p.label = nullptr; p.text.clear(); }
        const FVector top{at.X, at.Y, at.Z + 90};
        if (!p.label) { p.label = SpawnBare(textCls, top, pawn); p.text.clear(); }
        UObject* tc = IsValid(p.label) ? Field<UObject*>(p.label, off::TextRenderActor_Component) : nullptr;
        if (!IsValid(tc)) continue;
        if (p.text != w.text) {
            const std::wstring wt = Wide(w.text);
            Params t(Fn(tc, "SetText")); t.Set("Value", FString{wt.c_str(), int32_t(wt.size() + 1), int32_t(wt.size() + 1)}); Call(tc, "SetText", &t);
            if (auto f = Fn(tc, "SetHorizontalAlignment")) { Params a(f); a.Set<uint8_t>("Value", 1); Call(tc, "SetHorizontalAlignment", &a); }
            if (auto f = Fn(tc, "SetWorldSize")) { Params a(f); a.Set<float>("Value", 20.f); Call(tc, "SetWorldSize", &a); }
            if (auto f = Fn(tc, "SetTextRenderColor")) { Params a(f); a.Set<uint32_t>("Value", 0xFF9FD3FF); Call(tc, "SetTextRenderColor", &a); }
            p.text = w.text;
        }
        const float yaw = std::atan2(camAt.Y - top.Y, camAt.X - top.X) * 57.2957795f;
        if (auto f = Fn(p.label, "K2_SetActorLocationAndRotation")) { Params q(f); q.Set("NewLocation", top); q.Set("NewRotation", FRotator{0, yaw, 0}); q.SetBool("bTeleport", true); Call(p.label, "K2_SetActorLocationAndRotation", &q); }
    }
}
void Outline(UObject* comp, bool on) {
    if (!IsValid(comp)) return;
    if (auto f = Fn(comp, "SetRenderCustomDepth")) { Params p(f); p.SetBool("bValue", on); Call(comp, "SetRenderCustomDepth", &p); }
    if (auto f = Fn(comp, "SetCustomDepthStencilValue")) { Params p(f); p.Set<int>("Value", on ? 2 : 0); Call(comp, "SetCustomDepthStencilValue", &p); }
}
void UpdateSelectionOutline(UObject* controller) {
    std::vector<UObject*> actors;
    {
        std::lock_guard g(lock);
        if (wsel.kind == 1) actors = worldedit::Members(wsel.id);
        if (wsel.kind == 1 && actors.empty() && IsValid(wsel.actor)) actors.push_back(wsel.actor);
        else if (wsel.kind == 2) if (UObject* a = worldedit::FindAdded(wsel.id)) actors.push_back(a);
        if (wsel.kind == 6) if (auto it = carryPreviews.find(wsel.id); it != carryPreviews.end() && IsValid(it->second.body)) actors.push_back(it->second.body);
    }
    static UObject* primitive = nullptr;
    if (!primitive) primitive = FindClass("PrimitiveComponent");
    std::vector<UObject*> comps;
    for (UObject* a : actors) {
        auto f = Fn(a, "GetComponentsByClass"); if (!f || !primitive) continue;
        Params p(f); p.Set("ComponentClass", primitive); Call(a, "GetComponentsByClass", &p);
        struct Raw { UObject** data; int num, max; };
        const Raw list = p.Get<Raw>("ReturnValue");
        for (int i = 0; i < list.num && i < 64; ++i) if (IsValid(list.data[i])) comps.push_back(list.data[i]);
    }
    for (UObject* c : outlined) if (std::find(comps.begin(), comps.end(), c) == comps.end()) Outline(c, false);
    for (UObject* c : comps) if (std::find(outlined.begin(), outlined.end(), c) == outlined.end()) Outline(c, true);
    outlined = comps;
    // The post process is the game's; it is on while something is selected.
    static bool shown = false;
    UObject* post = nullptr;
    if (IsValid(controller)) if (auto prop = FindProperty(controller->Class, "FocusPostHighlightOnly")) post = Field<UObject*>(controller, Field<int>(prop, off::UProperty_Offset));
    if (IsValid(post) && (!comps.empty() || shown)) { SetBoolProp(post, "bEnabled", !comps.empty()); shown = !comps.empty(); }
}
FVector ActorLocation(UObject* a) { auto f = Fn(a, "K2_GetActorLocation"); if (!f) return {}; Params p(f); Call(a, "K2_GetActorLocation", &p); return p.Get<FVector>("ReturnValue"); }
void ListModes() {
    modes.clear();
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::path(profile::Root()) / L"modes", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory() && it->path().filename() != L"_lib") modes.push_back(it->path().filename().string());
    std::sort(modes.begin(), modes.end());
}
void LoadWorld(const std::string& m) {
    worldMode = m; world = World{}; wsel = {};
    std::ifstream in(WorldFile(m), std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (!ss.str().empty() && json::Value::Parse(ss.str(), v) && v.isObject()) WorldFrom(v, world);
    ScanLogic(m);
    LoadSkinLists(m);
    placeLogic.clear(); placeZone = 0; placeCarry = false; placeEnt = 0; doorPending.clear();
    if (world.env.set) {  // the mode's own time and weather
        DGMP_SetTimeOfDay(world.env.time); DGMP_SetTimePaused(world.env.paused);
        if (world.env.weather < 0) DGMP_SetWeatherAutomatic(1); else DGMP_SetWeather(uint32_t(world.env.weather));
    }
    worldUndo.clear(); worldRedo.clear(); worldCommittedValid = false; worldDirty = true;
    Toast(T("Мир режима ") + m + ": " + std::to_string(world.remove.size()) + T(" убрано, ") + std::to_string(world.move.size()) + T(" сдвинуто, ") + std::to_string(world.add.size()) + T(" добавлено"));
}
void LoadProps() {
    std::ifstream in(fs::path(profile::Root()) / L"data" / L"props.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f; size_t at = 0;
        for (size_t tab; (tab = line.find('\t', at)) != std::string::npos; at = tab + 1) f.push_back(line.substr(at, tab - at));
        f.push_back(line.substr(at));
        if (f.size() < 5) continue;
        props.push_back({f[0], f[1], f[2], f[3], f[4]});
        if (propCats.empty() || propCats.back() != f[0]) if (std::find(propCats.begin(), propCats.end(), f[0]) == propCats.end()) propCats.push_back(f[0]);
    }
}
// A left click into the world while the cursor belongs to the editor: what is under the mouse.
struct Pick { bool valid = false, object = false; std::string id, cls; FVector at{}; UObject* actor = nullptr; };
Pick PickUnderMouse(UObject* pawn, UObject* controller, bool complex = false) {
    Pick pick;
    auto mouse = Fn(controller, "GetMousePosition"), deproject = Fn(controller, "DeprojectScreenPositionToWorld");
    static UObject* kismet = nullptr; static UObject* statics = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr, brk = statics ? Fn(statics, "BreakHitResult") : nullptr;
    if (!mouse || !deproject || !trace || !brk) return pick;
    Params m(mouse); Call(controller, "GetMousePosition", &m);
    if (!m.GetBool("ReturnValue")) return pick;
    Params d(deproject); d.Set<float>("ScreenX", m.Get<float>("LocationX")); d.Set<float>("ScreenY", m.Get<float>("LocationY"));
    Call(controller, "DeprojectScreenPositionToWorld", &d);
    if (!d.GetBool("ReturnValue")) return pick;
    const FVector from = d.Get<FVector>("WorldLocation"), dir = d.Get<FVector>("WorldDirection");
    const FVector to{from.X + dir.X * 200000.f, from.Y + dir.Y * 200000.f, from.Z + dir.Z * 200000.f};
    Params t(trace); t.Set("WorldContextObject", pawn); t.Set("Start", from); t.Set("End", to); t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", complex); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    if (!t.GetBool("ReturnValue")) return pick;
    Params b(brk);
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
    std::memcpy(static_cast<uint8_t*>(b.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
    Call(statics, "BreakHitResult", &b);
    pick.valid = true; pick.at = b.Get<FVector>("ImpactPoint"); pick.actor = b.Get<UObject*>("HitActor");
    if (complex) return pick;  // world editing wants the hit object itself
    // Quest objectives name the interactable (car search, door, pickup, explosive) near the hit.
    static std::vector<UObject*> classes;
    if (classes.empty()) for (const char* c : {"BendBasicInteract", "BendInventoryPickup", "BendDoor", "Explosive_C"}) if (auto cls = FindClass(c)) classes.push_back(cls);
    UObject* best = nullptr; float bestDist = 250.f * 250.f;
    for (int i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || (o->ObjectFlags & 0x10) || !IsValid(o)) continue;
        bool wanted = false; for (auto cls : classes) if (IsA(o, cls)) { wanted = true; break; }
        if (!wanted) continue;
        auto f = Fn(o, "K2_GetActorLocation"); if (!f) continue;
        Params l(f); Call(o, "K2_GetActorLocation", &l); const FVector at = l.Get<FVector>("ReturnValue");
        const float dx = at.X - pick.at.X, dy = at.Y - pick.at.Y, dz = at.Z - pick.at.Z, dist = dx * dx + dy * dy + dz * dz;
        if (dist < bestDist) { bestDist = dist; best = o; }
    }
    if (best) { pick.object = true; pick.id = GetName(best); pick.cls = GetName(best->Class); }
    return pick;
}
// A click into the world in world mode: place the armed prop, or select what was hit.
void WorldClick(UObject* pawn, UObject* controller) {
    const Pick pick = PickUnderMouse(pawn, controller, true);
    std::lock_guard g(lock);
    if (!pick.valid) { wsel = {}; return; }
    if (placeZone) {
        WZone z; z.pvp = placeZone == 2; z.zombie = placeZone == 3;
        if (z.zombie) { z.radius = 30; z.world = 0; }
        Copy(z.name, sizeof z.name, FreeZoneName(z.zombie ? "zombies" : z.pvp ? "arena" : "safe"));
        z.t.x = pick.at.X; z.t.y = pick.at.Y; z.t.z = pick.at.Z; z.t.yaw = std::round(cam.rot.Yaw);
        world.zones.push_back(z);
        SelectZone(int(world.zones.size()) - 1);
        Toast(T("Поставлена зона ") + std::string(z.name));
        if (!repeatPlace) placeZone = 0;
        return;
    }
    if (!doorPending.empty()) {  // the door placed last: its inside end
        WEnt* d = EntOf("d:" + doorPending + ":b");
        if (d) { d->t2.x = pick.at.X; d->t2.y = pick.at.Y; d->t2.z = pick.at.Z; d->t2.yaw = std::round(cam.rot.Yaw); SelectEnt(*d, true); Toast(T("Вход") + std::string(" ") + d->key); }
        doorPending.clear(); placeEnt = 0;
        return;
    }
    if (placeEnt) {
        WEnt e; e.type = placeEnt - 1;
        static const char* const bases[] = {"car", "door", "loot"};
        Copy(e.key, sizeof e.key, FreeEntKey(e.type, bases[e.type]));
        e.t.x = pick.at.X; e.t.y = pick.at.Y; e.t.z = pick.at.Z; e.t.yaw = std::round(cam.rot.Yaw);
        if (WEnt* sel = wsel.kind == 7 ? EntOf(wsel.id) : nullptr; sel && sel->type == e.type) {  // like the selected one
            Copy(e.model, sizeof e.model, sel->model); e.world = sel->world; e.world2 = sel->world2; e.blip = sel->blip; e.locked = sel->locked;
            Copy(e.item, sizeof e.item, sel->item); e.count = sel->count; e.respawn = sel->respawn; Copy(e.tag, sizeof e.tag, sel->tag);
        }
        if (e.type == 1) { e.t2 = e.t; e.world2 = std::max(1, e.world2); }
        if (e.type == 2) { e.world = -1; if (placeItem[0]) Copy(e.item, sizeof e.item, placeItem); }
        world.ents.push_back(e);
        SelectEnt(world.ents.back());
        if (e.type == 1) { doorPending = e.key; Toast(T("Теперь ЛКМ ставит вторую сторону (внутри)")); return; }
        Toast(T("Поставлено: ") + std::string(e.key));
        if (!repeatPlace) placeEnt = 0;
        return;
    }
    if (placeCarry) {
        WCarry c; Copy(c.key, sizeof c.key, FreeCarryKey("box"));
        if (WCarry* sel = wsel.kind == 6 ? CarryByKey(wsel.id) : nullptr) { Copy(c.model, sizeof c.model, sel->model); Copy(c.tag, sizeof c.tag, sel->tag); c.world = sel->world; c.roll = sel->roll; }
        c.t.x = pick.at.X; c.t.y = pick.at.Y; c.t.z = pick.at.Z; c.t.yaw = std::round(cam.rot.Yaw);
        world.carries.push_back(c);
        SelectCarry(c.key);
        Toast(T("Поставлен ящик ") + std::string(c.key));
        if (!repeatPlace) placeCarry = false;
        return;
    }
    if (!placeLogic.empty()) {
        const LDecl* d = FindDecl(placeLogic);
        LItem it; it.t.x = pick.at.X; it.t.y = pick.at.Y; it.t.z = pick.at.Z; it.t.yaw = cam.rot.Yaw; it.radius = d ? d->radius : 5;
        auto& items = world.logic[placeLogic];
        if (d && !d->list) items.assign(1, it); else items.push_back(it);
        SelectLogic(placeLogic, int(items.size()) - 1);
        Toast(T("Поставлено: ") + placeLogic + (items.size() > 1 || (d && d->list) ? " " + std::to_string(items.size()) : std::string()));
        if (!repeatPlace || (d && !d->list)) placeLogic.clear();
        return;
    }
    if (!placeActor.empty()) {
        const LDecl* d = placeActor == "*" ? nullptr : FindDecl(placeActor);
        WActor a;
        if (d && !d->list) {  // one NPC of this id: it moves here
            world.actors.erase(std::remove_if(world.actors.begin(), world.actors.end(), [&](const WActor& o) { return placeActor == o.id; }), world.actors.end());
            Copy(a.id, sizeof a.id, placeActor);
        } else if (d) { Copy(a.group, sizeof a.group, placeActor); Copy(a.id, sizeof a.id, FreeActorId(placeActor + "_")); }
        else Copy(a.id, sizeof a.id, FreeActorId("npc"));
        float yaw = cam.rot.Yaw + 180.f; while (yaw > 180) yaw -= 360; while (yaw < -180) yaw += 360;
        a.t.x = pick.at.X; a.t.y = pick.at.Y; a.t.z = pick.at.Z; a.t.yaw = std::round(yaw);  // facing the camera
        world.actors.push_back(a);
        SelectActor(a.id);
        Toast(T("Поставлен NPC ") + std::string(a.id));
        if (!repeatPlace || (d && !d->list)) placeActor.clear();
        return;
    }
    if (!placeProp.empty()) {
        WAdd a; a.uid = "p" + std::to_string(GetTickCount64() % 100000000);
        a.asset = placeProp; a.t.x = pick.at.X; a.t.y = pick.at.Y; a.t.z = pick.at.Z; a.t.yaw = cam.rot.Yaw;
        for (auto& pr : props) if (pr.path == placeProp) { a.name = pr.name; break; }
        if (placeProp.rfind("light:", 0) == 0) {
            // Off the clicked surface (a wall, a ceiling), towards the camera.
            const float dx = cam.loc.X - pick.at.X, dy = cam.loc.Y - pick.at.Y, dz = cam.loc.Z - pick.at.Z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len > 1) { a.t.x += dx / len * 40; a.t.y += dy / len * 40; a.t.z += dz / len * 40; }
            a.t.yaw = 0; a.light = true;
            a.name = placeProp == "light:spot" ? T("Прожектор") : T("Точечный свет");
        }
        world.add.push_back(a);
        wsel = {2, a.uid, "", a.t};
        if (!repeatPlace) placeProp.clear();
        worldDirty = true;
        Toast(T("Поставлено: ") + a.name);
        return;
    }
    UObject* hit = pick.actor;
    if (!IsValid(hit)) { wsel = {}; return; }
    for (auto& a : world.add) if (worldedit::FindAdded(a.uid) == hit) { wsel = {2, a.uid, GetName(hit->Class), a.t}; return; }
    for (auto& [key, p] : carryPreviews) if (p.body == hit) { SelectCarry(key); return; }
    std::string cls = GetName(hit->Class);
    // Objects of merged collections: a click selects the whole object (a car), Alt+click one part of it.
    const bool part = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (cls == "BendStaticMeshCollection") {
        if (!worldedit::Editable(hit)) { wsel = {}; Toast(std::string(T("Дороги и бордюры (сплайны) пока не редактируются")) + "\n" + worldedit::KeyOf(hit)); return; }
        const int piece = worldedit::PieceAt(hit, pick.at);
        UObject* actor = piece >= 0 ? worldedit::Detach(hit, piece, pawn, part) : nullptr;
        if (actor) { hit = actor; cls = "StaticMeshActor"; }
    }
    std::string key = part ? worldedit::PartKey(hit) : std::string();
    if (key.empty()) key = worldedit::KeyOf(hit);
    if (UObject* primary = worldedit::Primary(key)) hit = primary;
    if (cls.find("Landscape") != std::string::npos || cls == "InstancedFoliageActor" || cls == "BendStaticMeshCollection" || cls == "WorldSettings") {
        wsel = {};
        Toast(cls == "BendStaticMeshCollection" ? std::string(T("Это склеенная группа мелких объектов — её не выбрать целиком")) + "\n" + worldedit::KeyOf(hit) : std::string(T("Земля и растительность не редактируются")));
        return;
    }
    WSel sel; sel.kind = 1; sel.id = key; sel.cls = cls; sel.actor = hit;
    if (auto m = world.move.find(sel.id); m != world.move.end()) sel.t = m->second;
    else { const FVector at = ActorLocation(hit); const FRotator r = ActorRotation(hit); sel.t = {at.X, at.Y, at.Z, r.Pitch, r.Yaw, r.Roll, 1}; }
    wsel = sel;
}

void ApplyPick(const Pick& pick) {
    std::lock_guard g(lock);
    if (mode != 1) return;  // world editing comes later
    if (clickMode == kGiver) {  // the job's own NPC, facing the camera
        if (!questOpen) return;
        float yaw = cam.rot.Yaw + 180.f; while (yaw > 180) yaw -= 360; while (yaw < -180) yaw += 360;
        quest.giverActor[0] = 0; quest.giverHasPoint = true;
        quest.gx = pick.at.X; quest.gy = pick.at.Y; quest.gz = pick.at.Z; quest.gyaw = std::round(yaw);
        picked = {-1, 5, 0};
        Toast(T("Место NPC работы"));
        clickMode = kSelect; previewDirty = true;
        return;
    }
    if (!questOpen || selected < 0 || selected >= int(quest.steps.size())) { Toast(T("Выберите этап квеста")); return; }
    Step& s = quest.steps[selected];
    const std::string where = T("этап ") + std::to_string(selected + 1);
    if (clickMode == kSelect) return;
    if (s.type == 3) {
        if (clickMode == kDropoff) { s.hasDrop = true; s.dx = pick.at.X; s.dy = pick.at.Y; s.dz = pick.at.Z; picked = {selected, 4, 0}; Toast(T("Разгрузка → ") + where); }
        else { s.hasPickup = true; s.px = pick.at.X; s.py = pick.at.Y; s.pz = pick.at.Z; picked = {selected, 3, 0}; Toast(T("Погрузка → ") + where); }
        clickMode = kSelect; previewDirty = true;
        return;
    }
    if (clickMode == kPickup || clickMode == kDropoff) { Toast(T("Выберите этап «Перенести ящики»")); clickMode = kSelect; return; }
    if (s.type != 2 || clickMode == kPoint) {
        if (s.type == 2) { s.hasArea = true; s.ax = pick.at.X; s.ay = pick.at.Y; s.az = pick.at.Z; Toast(T("Зона → ") + where); }
        else { s.hasPoint = true; s.x = pick.at.X; s.y = pick.at.Y; s.z = pick.at.Z; Toast(T("Точка → ") + where); }
    } else if (clickMode == kObject) {
        if (!pick.object) { Toast(T("Здесь нет объекта: наведитесь на багажник, дверь, предмет или бочку")); return; }
        if (std::find(s.ids.begin(), s.ids.end(), pick.id) == s.ids.end()) s.ids.push_back(pick.id);
        Toast(T("Объект ") + pick.id + " → " + where);
    } else {
        if (!placeItem[0]) { Toast(T("Выберите предмет в библиотеке")); return; }
        SpawnRow row; Copy(row.item, sizeof(row.item), placeItem); row.x = pick.at.X; row.y = pick.at.Y; row.z = pick.at.Z;
        s.spawn.push_back(row);
        picked = {selected, 2, int(s.spawn.size()) - 1};
        Toast(std::string(T("Предмет ")) + placeItem + " → " + where);
    }
    if (!repeatPlace) clickMode = kSelect;
    previewDirty = true;
}

// ---------------------------------------------------------------- UI
const char* ItemName(const std::string& key) {
    for (auto& it : catalog) if (it.key == key) return english ? it.english.c_str() : it.name.c_str();
    return key.c_str();
}
bool Contains(const std::string& text, const char* part) {
    if (!part[0]) return true;
    std::string a = text, b = part;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(tolower(c)); });
    return a.find(b) != std::string::npos;
}

// History of the world edits (same rule as the quest: a finished edit is one step).
void TrackWorldHistory() {
    if (worldMode.empty()) return;
    if (ImGui::IsAnyItemActive() || ImGuizmo::IsUsing() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    std::string now = WorldJson(world).dump();
    if (!worldCommittedValid) { worldCommittedJson = std::move(now); worldCommitted = world; worldCommittedValid = true; return; }
    if (now == worldCommittedJson) return;
    worldUndo.push_back(worldCommitted);
    if (worldUndo.size() > 100) worldUndo.erase(worldUndo.begin());
    worldRedo.clear();
    worldCommittedJson = std::move(now); worldCommitted = world;
}
void WorldRestore(std::vector<World>& from, std::vector<World>& to, const char* done) {
    if (from.empty()) return;
    to.push_back(world); world = from.back(); from.pop_back();
    worldCommittedJson = WorldJson(world).dump(); worldCommitted = world; worldCommittedValid = true;
    wsel = {}; worldDirty = true; Toast(done);
}
LItem* LogicOf(const std::string& id) {
    const size_t hash = id.rfind('#');
    if (hash == std::string::npos) return nullptr;
    auto it = world.logic.find(id.substr(0, hash));
    const int index = std::atoi(id.c_str() + hash + 1);
    return it != world.logic.end() && index >= 0 && index < int(it->second.size()) ? &it->second[index] : nullptr;
}
void SelectLogic(const std::string& name, int index) {
    auto it = world.logic.find(name);
    if (it == world.logic.end() || index < 0 || index >= int(it->second.size())) { wsel = {}; return; }
    wsel = {3, name + "#" + std::to_string(index), "", it->second[index].t};
}
void WorldDelete() {
    if (wsel.kind == 1) {
        if (std::find(world.remove.begin(), world.remove.end(), wsel.id) == world.remove.end()) world.remove.push_back(wsel.id);
        world.move.erase(wsel.id);
        Toast(T("Убрано: ") + wsel.id);
    } else if (wsel.kind == 2) {
        world.add.erase(std::remove_if(world.add.begin(), world.add.end(), [](const WAdd& a) { return a.uid == wsel.id; }), world.add.end());
        Toast(T("Удалён проп"));
    } else if (wsel.kind == 3) {
        const size_t hash = wsel.id.rfind('#');
        auto it = world.logic.find(wsel.id.substr(0, hash));
        const int index = std::atoi(wsel.id.c_str() + hash + 1);
        if (it != world.logic.end() && index < int(it->second.size())) it->second.erase(it->second.begin() + index);
        if (it != world.logic.end() && it->second.empty()) world.logic.erase(it);
    } else if (wsel.kind == 5) {
        const int i = std::atoi(wsel.id.c_str());
        if (i >= 0 && i < int(world.zones.size())) world.zones.erase(world.zones.begin() + i);
    } else if (wsel.kind == 4) {
        world.actors.erase(std::remove_if(world.actors.begin(), world.actors.end(), [](const WActor& a) { return wsel.id == a.id; }), world.actors.end());
    } else if (wsel.kind == 6) {
        world.carries.erase(std::remove_if(world.carries.begin(), world.carries.end(), [](const WCarry& c) { return wsel.id == c.key; }), world.carries.end());
    } else if (wsel.kind == 7) {
        const WEnt* e = EntOf(wsel.id);
        if (e) world.ents.erase(world.ents.begin() + (e - world.ents.data()));
    } else return;
    wsel = {}; worldDirty = true;
}
void WorldDuplicate() {
    if (wsel.kind == 4) {
        if (WActor* a = ActorById(wsel.id)) {
            WActor copy = *a;
            Copy(copy.id, sizeof copy.id, FreeActorId(copy.group[0] ? std::string(copy.group) + "_" : std::string("npc")));
            copy.t.x += 100; copy.t.y += 100;
            world.actors.push_back(copy); SelectActor(copy.id); Toast(T("Копия"));
        }
        return;
    }
    if (wsel.kind == 7) {
        if (WEnt* e = EntOf(wsel.id)) {
            WEnt copy = *e;
            std::string base = copy.key; while (!base.empty() && isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
            Copy(copy.key, sizeof copy.key, FreeEntKey(copy.type, base.empty() ? "x" : base));
            copy.t.x += 150; copy.t.y += 150; copy.t2.x += 150; copy.t2.y += 150;
            world.ents.push_back(copy); SelectEnt(world.ents.back()); Toast(T("Копия"));
        }
        return;
    }
    if (wsel.kind == 6) {
        if (WCarry* c = CarryByKey(wsel.id)) {
            WCarry copy = *c;
            std::string base = copy.key; while (!base.empty() && isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
            Copy(copy.key, sizeof copy.key, FreeCarryKey(base.empty() ? "box" : base));
            copy.t.x += 80; copy.t.y += 80;
            world.carries.push_back(copy); SelectCarry(copy.key); Toast(T("Копия"));
        }
        return;
    }
    if (wsel.kind == 5) {
        if (WZone* z = ZoneOf(wsel.id)) {
            WZone copy = *z;
            std::string base = copy.name; while (!base.empty() && isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
            Copy(copy.name, sizeof copy.name, FreeZoneName(base.empty() ? "zone" : base));
            copy.t.x += 500; copy.t.y += 500;
            world.zones.push_back(copy); SelectZone(int(world.zones.size()) - 1); Toast(T("Копия"));
        }
        return;
    }
    if (wsel.kind != 2) { Toast(T("Дублируются поставленные пропсы")); return; }
    for (auto& a : world.add) if (a.uid == wsel.id) {
        WAdd copy = a; copy.uid = "p" + std::to_string(GetTickCount64() % 100000000); copy.t.x += 100; copy.t.y += 100;
        world.add.push_back(copy); wsel = {2, copy.uid, "", copy.t}; worldDirty = true; Toast(T("Копия")); return;
    }
}
// Time of day and weather of the mode: changed live, kept in world.json when "with the mode" is on.
void DrawEnvironment() {
    if (!ImGui::CollapsingHeader(T("Время и погода"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto snap = environment::GetSnapshot();
    World::Env& e = world.env;
    if (!e.set) {  // show what the world has now
        e.time = snap.state.hours; e.paused = snap.state.timePaused != 0; e.weather = snap.state.weatherAutomatic ? -1 : int(snap.state.weatherId);
    }
    bool changed = false;
    ImGui::Checkbox(T("Сохранять с режимом"), &e.set);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Режим стартует с этим временем и погодой (сервер применяет их до кода режима)"));
    float t = e.time;
    const int hh = int(t) % 24, mm = int((t - int(t)) * 60);
    char label[32]; snprintf(label, sizeof label, "%02d:%02d", hh, mm);
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat(T("Время"), &t, 0.f, 23.99f, label)) { e.time = t; DGMP_SetTimeOfDay(t); changed = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Остановить часы"), &e.paused)) { DGMP_SetTimePaused(e.paused); changed = true; }
    std::string current = T("авто");
    for (auto& w : snap.weather) if (int(w.id) == e.weather) current = w.name;
    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo(T("Погода"), current.c_str())) {
        if (ImGui::Selectable(T("авто"), e.weather < 0)) { e.weather = -1; DGMP_SetWeatherAutomatic(1); changed = true; }
        for (auto& w : snap.weather) if (ImGui::Selectable(w.name, int(w.id) == e.weather)) { e.weather = int(w.id); DGMP_SetWeather(w.id); changed = true; }
        ImGui::EndCombo();
    }
    if (changed) e.set = true;
}
// Logic objects: what the mode's code asks for, and what is placed.
void DrawLogicList() {
    ImGui::SeparatorText(T("Логика режима"));
    if (ImGui::SmallButton(T("Обновить из кода"))) ScanLogic(worldMode);
    ImGui::SameLine(); ImGui::Checkbox(T("Повторять##logic"), &repeatPlace);
    if (logicDecls.empty() && world.logic.empty()) { ImGui::TextDisabled("%s", T("Код режима не просит объектов: logic.trigger(\"имя\"), logic.points(\"имя\")…")); return; }
    std::vector<std::string> names;
    for (auto& d : logicDecls) names.push_back(d.name);
    for (auto& [n, items] : world.logic) if (!FindDecl(n)) names.push_back(n);  // placed, but the code no longer asks
    for (auto& name : names) {
        const LDecl* d = FindDecl(name);
        if (d && d->actor) {  // logic.actor("id") / logic.actors("group"): NPCs placed here
            std::vector<const WActor*> mine;
            for (auto& a : world.actors) if (d->list ? name == a.group : name == a.id) mine.push_back(&a);
            ImGui::PushID(name.c_str());
            const bool armed = placeActor == name;
            if (ImGui::SmallButton(armed ? T("отмена") : !d->list ? (mine.empty() ? T("поставить") : T("переставить")) : T("добавить"))) {
                placeActor = armed ? "" : name; placeProp.clear(); placeLogic.clear(); placeZone = 0; placeCarry = false; placeEnt = 0;
                if (!armed) Toast(T("ЛКМ в мире ставит NPC: ") + name);
            }
            ImGui::SameLine();
            if (!d->list) {
                if (ImGui::Selectable((name + "  —  NPC" + (mine.empty() ? std::string(T(", не поставлен")) : std::string())).c_str(), wsel.kind == 4 && wsel.id == name) && !mine.empty()) SelectActor(name);
            } else if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s  —  %s, %d", name.c_str(), T("NPC, группа"), int(mine.size()))) {
                for (const WActor* a : mine) if (ImGui::Selectable(a->id, wsel.kind == 4 && wsel.id == a->id)) SelectActor(a->id);
                ImGui::TreePop();
            }
            ImGui::PopID();
            continue;
        }
        auto it = world.logic.find(name);
        const int count = it == world.logic.end() ? 0 : int(it->second.size());
        ImGui::PushID(name.c_str());
        const bool armed = placeLogic == name;
        if (ImGui::SmallButton(armed ? T("отмена") : d && !d->list ? (count ? T("переставить") : T("поставить")) : T("добавить"))) {
            placeLogic = armed ? "" : name; placeProp.clear(); placeActor.clear(); placeZone = 0; placeCarry = false; placeEnt = 0;
            if (!armed) Toast(T("ЛКМ в мире ставит: ") + name);
        }
        ImGui::SameLine();
        const char* kind = name == "wardrobe" ? T("место выбора скина") : !d ? T("не используется в коде") : d->trigger ? (d->list ? T("триггеры") : T("триггер")) : (d->list ? T("точки") : T("точка"));
        if (d && !d->list) {
            if (ImGui::Selectable((name + "  —  " + kind + (count ? "" : std::string(T(", не поставлен")))).c_str(), wsel.kind == 3 && wsel.id == name + "#0") && count) SelectLogic(name, 0);
        } else if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s  —  %s, %d", name.c_str(), kind, count)) {
            for (int i = 0; i < count; ++i) {
                ImGui::PushID(i);
                const std::string id = name + "#" + std::to_string(i);
                if (ImGui::Selectable((std::to_string(i + 1) + ". " + name).c_str(), wsel.kind == 3 && wsel.id == id, 0, ImVec2(160, 0))) SelectLogic(name, i);
                ImGui::SameLine();
                if (ImGui::SmallButton("^") && i > 0) { std::swap(it->second[i], it->second[i - 1]); SelectLogic(name, i - 1); }
                ImGui::SameLine();
                if (ImGui::SmallButton("v") && i + 1 < count) { std::swap(it->second[i], it->second[i + 1]); SelectLogic(name, i + 1); }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) { it->second.erase(it->second.begin() + i); wsel = {}; ImGui::PopID(); break; }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
// PvP of the mode and its PvE / PvP zones (World > PvP and zones).
void DrawZones() {
    if (!ImGui::CollapsingHeader(T("PvP и зоны"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Checkbox(T("Урон между игроками (PvP)"), &world.pvp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Выключено: игроки ранят друг друга только в PvP-зонах. Код режима может менять это: setPvp(), player:setPvp(), setPvpRule()."));
    for (int kind = 1; kind <= 3; ++kind) {
        const bool armed = placeZone == kind;
        if (kind > 1) ImGui::SameLine();
        if (ImGui::Button(armed ? (std::string(T("отмена")) + "##zone" + std::to_string(kind)).c_str() : kind == 1 ? T("+ PvE зона") : kind == 2 ? T("+ PvP зона") : T("+ Зомби"))) {
            placeZone = armed ? 0 : kind; placeProp.clear(); placeLogic.clear(); placeActor.clear(); placeCarry = false; placeEnt = 0;
            if (!armed) Toast(T("ЛКМ в мире ставит центр зоны"));
        }
    }
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("PvE (зелёная) — безопасная зона: игроки не ранят друг друга, стрелять из неё тоже нельзя. PvP (красная) — урон между игроками есть, даже если PvP режима выключен (нужно, чтобы оба были в PvP-зонах). Зомби и NPC опасны везде."));
    ImGui::TextDisabled("%s", T("Зона зомби (салатовая): пока рядом игрок, в ней держится заданное число зомби (server.ini Zombies=1)."));
    if (world.zones.empty()) { ImGui::TextDisabled("%s", T("Зон нет")); return; }
    for (int i = 0; i < int(world.zones.size()); ++i) {
        const WZone& z = world.zones[size_t(i)];
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, z.zombie ? IM_COL32(160, 220, 40, 255) : z.pvp ? IM_COL32(240, 110, 100, 255) : IM_COL32(110, 220, 130, 255));
        ImGui::TextUnformatted(z.zombie ? "Z  " : z.pvp ? "PvP" : "PvE");
        ImGui::PopStyleColor(); ImGui::SameLine();
        char label[160];
        if (z.zombie) snprintf(label, sizeof label, "%s  —  R %.0f %s, %d  %s%s", z.name, z.radius, T("м"), z.max, z.types, z.enabled ? "" : T(", выключена"));
        else if (z.box) snprintf(label, sizeof label, "%s  —  %.0f x %.0f %s%s", z.name, z.length, z.width, T("м"), z.enabled ? "" : T(", выключена"));
        else snprintf(label, sizeof label, "%s  —  R %.0f %s%s", z.name, z.radius, T("м"), z.enabled ? "" : T(", выключена"));
        if (ImGui::Selectable(label, wsel.kind == 5 && wsel.id == std::to_string(i))) SelectZone(i);
        ImGui::PopID();
    }
}
// The selected zombie zone's settings.
void DrawZombieZoneProps(WZone* z) {
    char name[48]; Copy(name, sizeof name, z->name);
    if (ImGui::InputText(T("Имя зоны"), name, sizeof name)) {
        bool used = false; for (auto& o : world.zones) used |= &o != z && std::strcmp(o.name, name) == 0;
        if (ValidId(name) && !used) Copy(z->name, sizeof z->name, name);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Имя для кода режима: getZombieZone(\"имя\"), setZombieZone(\"имя\", {...}). Латиница, цифры, _ и -"));
    if (ImGui::DragFloat3(T("Центр"), &z->t.x, 5.f, 0, 0, "%.0f")) wsel.t = z->t;
    ImGui::DragFloat(T("Радиус, м"), &z->radius, 0.5f, 3.f, 1000.f, "%.0f");
    ImGui::InputText(T("Типы зомби"), z->types, sizeof z->types);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Через запятую: Swarmer, Newt... Появляются только типы, загруженные в этом регионе (docs/zombies.md)"));
    ImGui::SetNextItemWidth(120); ImGui::InputInt(T("Сколько держать"), &z->max); z->max = std::clamp(z->max, 0, 50);
    ImGui::DragFloat(T("Новый вместо убитого через, с"), &z->respawn, 1.f, 0.f, 3600.f, "%.0f");
    ImGui::DragFloat(T("Просыпается, когда игрок ближе, м"), &z->wake, 1.f, 0.f, 1000.f, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("От края зоны. Без игроков рядом 30 с зона засыпает: её зомби исчезают"));
    ImGui::Checkbox(T("Включена при старте"), &z->enabled);
    ImGui::SetNextItemWidth(120); ImGui::InputInt(T("Мир"), &z->world); if (z->world < 0) z->world = 0;
    const std::string luaName = z->name;
    if (ImGui::Button(T("Удалить"))) { WorldDelete(); return; }
    ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) WorldDuplicate();
    ImGui::TextDisabled("Lua: getZombieZone(\"%s\")", luaName.c_str());
}
void DrawZoneProps() {
    WZone* z = ZoneOf(wsel.id);
    if (!z) { wsel = {}; return; }
    ImGui::PushID("zone");
    if (z->zombie) { DrawZombieZoneProps(z); ImGui::PopID(); return; }
    char name[48]; Copy(name, sizeof name, z->name);
    if (ImGui::InputText(T("Имя зоны"), name, sizeof name)) {
        bool used = false; for (auto& o : world.zones) used |= &o != z && std::strcmp(o.name, name) == 0;
        if (ValidId(name) && !used) Copy(z->name, sizeof z->name, name);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Имя для кода режима: getZone(\"имя\"), setZone(\"имя\", {...}), player:inZone(\"имя\"). Латиница, цифры, _ и -"));
    if (ImGui::RadioButton(T("PvE (безопасная)"), !z->pvp)) z->pvp = false;
    ImGui::SameLine(); if (ImGui::RadioButton("PvP", z->pvp)) z->pvp = true;
    if (ImGui::RadioButton(T("Круг"), !z->box)) z->box = false;
    ImGui::SameLine(); if (ImGui::RadioButton(T("Прямоугольник"), z->box)) z->box = true;
    bool moved = ImGui::DragFloat3(T("Центр"), &z->t.x, 5.f, 0, 0, "%.0f");
    if (z->box) {
        moved |= ImGui::DragFloat(T("Поворот"), &z->t.yaw, 1.f, -360, 360, "%.0f°");
        ImGui::DragFloat(T("Длина, м"), &z->length, 0.5f, 1.f, 5000.f, "%.0f");
        ImGui::DragFloat(T("Ширина, м"), &z->width, 0.5f, 1.f, 5000.f, "%.0f");
    } else ImGui::DragFloat(T("Радиус, м"), &z->radius, 0.5f, 1.f, 5000.f, "%.0f");
    ImGui::DragFloat(T("Высота вверх и вниз, м"), &z->height, 0.5f, 1.f, 2000.f, "%.0f");
    if (moved) wsel.t = z->t;
    ImGui::Checkbox(T("Включена при старте"), &z->enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Выключенную зону код режима включает сам: setZone(\"имя\", {enabled = true})"));
    ImGui::Checkbox(T("Надпись на экране в зоне"), &z->notify);
    if (z->notify) {
        ImGui::InputText(T("Своя надпись"), z->text, sizeof z->text);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Пусто — стандартная («Безопасная зона» / «PvP-зона»). Можно цвета {RRGGBB}"));
    }
    ImGui::SetNextItemWidth(120); ImGui::InputInt(T("Мир (-1 — во всех)"), &z->world); if (z->world < -1) z->world = -1;
    const std::string luaName = z->name;
    if (ImGui::Button(T("Удалить"))) { ImGui::PopID(); WorldDelete(); return; }
    ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) WorldDuplicate();
    ImGui::TextDisabled("Lua: getZone(\"%s\")", luaName.c_str());
    ImGui::PopID();
}
// The selected box's settings (World > Selected).
char carryKeyEdit[48] = "";
std::string carryKeyFor;  // the box carryKeyEdit belongs to
void DrawCarryProps() {
    WCarry* c = CarryByKey(wsel.id);
    if (!c) { wsel = {}; return; }
    ImGui::PushID("carry");
    ImGui::TextWrapped("%s  %s", T("Ящик"), c->key);
    bool moved = false;
    moved |= ImGui::DragFloat3(T("Позиция"), &c->t.x, 5.f, 0, 0, "%.0f");
    moved |= ImGui::DragFloat(T("Поворот"), &c->t.yaw, 1.f, -360, 360, "%.0f°");
    if (moved) wsel.t = c->t;
    if (carryKeyFor != c->key) { carryKeyFor = c->key; Copy(carryKeyEdit, sizeof carryKeyEdit, c->key); }
    ImGui::InputText(T("Ключ"), carryKeyEdit, sizeof carryKeyEdit);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!ValidId(carryKeyEdit)) Toast(T("Ключ: латиница, цифры, _ и -"));
        else if (CarryByKey(carryKeyEdit) && std::strcmp(carryKeyEdit, c->key) != 0) Toast(T("Такой ключ уже есть"));
        else { Copy(c->key, sizeof c->key, carryKeyEdit); wsel.id = c->key; carryKeyFor = c->key; }
        Copy(carryKeyEdit, sizeof carryKeyEdit, c->key);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Имя для кода режима: getCarry(\"ключ\")"));
    if (ImGui::BeginCombo(T("Модель"), CarryModelName(c->model))) {
        int n = 0; const carry::ModelInfo* m = carry::Models(n);
        for (int i = 0; i < n; ++i) if (ImGui::Selectable(english ? m[i].en : m[i].ru, std::strcmp(m[i].name, c->model) == 0)) Copy(c->model, sizeof c->model, m[i].name);
        ImGui::EndCombo();
    }
    ImGui::InputText(T("Метка"), c->tag, sizeof c->tag);
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("carries(\"метка\") в коде режима вернёт все ящики с этой меткой"));
    static const char* const rolls[] = {"Ничего", "Падает", "Ломается"};
    ImGui::Combo(T("Перекат с ящиком"), &c->roll, TArr(rolls, 3), 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Что будет, если игрок сделает перекат с ящиком в руках: ничего, ящик падает на землю, ящик ломается (исчезает)"));
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(T("Мир (-1 — во всех)"), &c->world)) c->world = std::max(-1, c->world);
    ImGui::TextDisabled(c->tag[0] ? "Lua: getCarry(\"%s\")  /  carries(\"%s\")" : "Lua: getCarry(\"%s\")", c->key, c->tag);
    if (ImGui::Button(T("Удалить"))) { ImGui::PopID(); WorldDelete(); return; }
    ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) WorldDuplicate();
    ImGui::PopID();
}
// The selected vehicle, door or loot (World > Selected).
char entKeyEdit[48] = "";
std::string entKeyFor;
void DrawEntProps() {
    bool inside = false;
    WEnt* e = EntOf(wsel.id, &inside);
    if (!e) { wsel = {}; return; }
    ImGui::PushID("ent");
    static const char* const names[] = {"Транспорт", "Вход", "Лут"};
    ImGui::TextWrapped("%s  %s%s", T(names[e->type]), e->key, e->type == 1 ? (inside ? T("  — Внутри") : T("  — Снаружи")) : "");
    WT& t = inside ? e->t2 : e->t;
    bool moved = false;
    moved |= ImGui::DragFloat3(T("Позиция"), &t.x, 5.f, 0, 0, "%.0f");
    moved |= ImGui::DragFloat(T("Поворот"), &t.yaw, 1.f, -360, 360, "%.0f°");
    if (moved) wsel.t = t;
    const std::string id = EntId(*e, inside);
    if (entKeyFor != id) { entKeyFor = id; Copy(entKeyEdit, sizeof entKeyEdit, e->key); }
    ImGui::InputText(T("Ключ"), entKeyEdit, sizeof entKeyEdit);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        bool taken = false; for (auto& o : world.ents) taken |= &o != e && o.type == e->type && std::strcmp(o.key, entKeyEdit) == 0;
        if (!ValidId(entKeyEdit)) Toast(T("Ключ: латиница, цифры, _ и -"));
        else if (taken) Toast(T("Такой ключ уже есть"));
        else { Copy(e->key, sizeof e->key, entKeyEdit); wsel.id = EntId(*e, inside); entKeyFor = wsel.id; }
        Copy(entKeyEdit, sizeof entKeyEdit, e->key);
    }
    ImGui::SetNextItemWidth(120);
    if (e->type == 0) {
        int model = IndexOf(kVehicleModels, 4, e->model, 0);
        static const char* const models[] = {"Мотоцикл", "Гольф-кар", "Самосвал (эксп.)", "Военный грузовик (эксп.)"};
        if (ImGui::Combo(T("Модель"), &model, TArr(models, 4), 4)) Copy(e->model, sizeof e->model, kVehicleModels[model]);
        ImGui::Checkbox(T("Заперт"), &e->locked);
        ImGui::InputText(T("Владелец (ник)"), e->owner, sizeof e->owner);
        static const char* const blips[] = {"Владельцу", "Всем", "Никому"};
        ImGui::Combo(T("Метка на карте"), &e->blip, TArr(blips, 3), 3);
        ImGui::SliderFloat(T("Бензин"), &e->fuel, 0, 1, "%.2f");
        ImGui::SetNextItemWidth(120); if (ImGui::InputInt(T("Мир"), &e->world)) e->world = std::max(0, e->world);
        ImGui::TextDisabled("Lua: getVehicle(\"%s\")", e->key);
    } else if (e->type == 1) {
        ImGui::InputText(T("Надпись [F]"), e->text, sizeof e->text);
        ImGui::Checkbox(T("Заперт"), &e->locked);
        ImGui::SetNextItemWidth(120); if (ImGui::InputInt(T("Мир снаружи"), &e->world)) e->world = std::max(0, e->world);
        ImGui::SetNextItemWidth(120); if (ImGui::InputInt(T("Мир внутри"), &e->world2)) e->world2 = std::max(0, e->world2);
        if (ImGui::Button(T("К другой стороне"))) SelectEnt(*e, !inside);
        ImGui::TextDisabled("Lua: getDoor(\"%s\"), setDoor(\"%s\", {locked = true})", e->key, e->key);
    } else {
        ImGui::InputText(T("Предмет"), e->item, sizeof e->item);
        ImGui::SameLine(); if (ImGui::SmallButton(T("из библиотеки")) && placeItem[0]) Copy(e->item, sizeof e->item, placeItem);
        ImGui::SetNextItemWidth(120); if (ImGui::InputInt(T("Количество"), &e->count)) e->count = std::max(1, e->count);
        ImGui::SetNextItemWidth(120); if (ImGui::InputInt(T("Возвращается через, с (0 — один раз)"), &e->respawn)) e->respawn = std::max(0, e->respawn);
        ImGui::InputText(T("Метка"), e->tag, sizeof e->tag);
        ImGui::TextDisabled("Lua: loot()");
    }
    if (ImGui::Button(T("Удалить"))) { ImGui::PopID(); WorldDelete(); return; }
    ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) WorldDuplicate();
    ImGui::PopID();
}
// A look: a skin of the mode or a look of the game, typed or picked from the ▼ list (with a search).
void LookField(const char* label, char* look, size_t size, const char* hint = nullptr) {
    static char filter[64] = "";
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::InputTextWithHint("##look", hint ? hint : T("облик (пусто — Дикон)"), look, size);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::ArrowButton("##lookpick", ImGuiDir_Down)) { filter[0] = 0; ImGui::OpenPopup("lookpick"); }
    ImGui::SameLine(); ImGui::TextUnformatted(label);
    if (ImGui::BeginPopup("lookpick")) {
        ImGui::SetNextItemWidth(300);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##f", T("Поиск"), filter, sizeof filter);
        if (ImGui::BeginChild("##looks", ImVec2(360, 320))) {
            for (auto& [id, text] : ModeSkinChoices()) {
                if (filter[0] && !Contains(text, filter)) continue;
                if (ImGui::Selectable(text.c_str())) { Copy(look, size, id); ImGui::CloseCurrentPopup(); }
            }
            static const std::vector<std::string> names = skins::LookNames();
            int shown = 0;
            for (auto& n : names) {
                if (filter[0] && !Contains(n, filter)) continue;
                if (++shown > 400) { ImGui::TextDisabled("%s", T("…уточните поиск")); break; }
                if (ImGui::Selectable(n.c_str())) { Copy(look, size, n); ImGui::CloseCurrentPopup(); }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
// An animation: a short name or any of data/anims.txt.
void AnimField(const char* label, char* anim, size_t size) {
    static char filter[64] = "";
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::InputText("##anim", anim, size);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::ArrowButton("##animpick", ImGuiDir_Down)) { filter[0] = 0; ImGui::OpenPopup("animpick"); }
    ImGui::SameLine(); ImGui::TextUnformatted(label);
    if (anim[0] && AnimPath(anim).empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("нет такой анимации"));
    if (ImGui::BeginPopup("animpick")) {
        ImGui::SetNextItemWidth(300);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##f", T("Поиск"), filter, sizeof filter);
        if (ImGui::BeginChild("##anims", ImVec2(420, 320))) {
            int shown = 0;
            for (auto& [n, path] : Anims()) {
                if (filter[0] && !Contains(n, filter)) continue;
                if (++shown > 400) { ImGui::TextDisabled("%s", T("…уточните поиск")); break; }
                if (ImGui::Selectable(n.c_str())) { Copy(anim, size, n); ImGui::CloseCurrentPopup(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// The selected NPC's settings (World > Selected).
char actorIdEdit[48] = "";
std::string actorIdFor;  // the NPC actorIdEdit belongs to
void DrawActorProps() {
    WActor* a = ActorById(wsel.id);
    if (!a) { wsel = {}; return; }
    ImGui::TextWrapped("NPC  %s", a->id);
    bool moved = false;
    moved |= ImGui::DragFloat3(T("Позиция"), &a->t.x, 5.f, 0, 0, "%.0f");
    moved |= ImGui::DragFloat(T("Поворот"), &a->t.yaw, 1.f, -360, 360, "%.0f°");
    if (moved) wsel.t = a->t;
    if (actorIdFor != a->id) { actorIdFor = a->id; Copy(actorIdEdit, sizeof actorIdEdit, a->id); }
    ImGui::InputText("id", actorIdEdit, sizeof actorIdEdit);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!ValidId(actorIdEdit)) Toast(T("id: латиница, цифры, _ и -"));
        else if (ActorById(actorIdEdit) && std::strcmp(actorIdEdit, a->id) != 0) Toast(T("Такой id уже есть"));
        else { Copy(a->id, sizeof a->id, actorIdEdit); wsel.id = a->id; actorIdFor = a->id; }
        Copy(actorIdEdit, sizeof actorIdEdit, a->id);
    }
    ImGui::InputText(T("Группа"), a->group, sizeof a->group);
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("logic.actors(\"группа\") в коде режима вернёт всех NPC этой группы"));
    ImGui::InputText(T("Имя над головой"), a->name, sizeof a->name);
    LookField(T("Облик"), a->look, sizeof a->look);
    AnimField(T("Анимация"), a->anim, sizeof a->anim);
    ImGui::Checkbox(T("Твёрдый (в него упираются)"), &a->solid);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(T("Мир (-1 — во всех)"), &a->world)) a->world = std::max(-1, a->world);
    ImGui::TextDisabled(a->group[0] ? "Lua: logic.actor(\"%s\")  /  logic.actors(\"%s\")" : "Lua: logic.actor(\"%s\")", a->id, a->group);
    if (ImGui::Button(T("Удалить"))) { WorldDelete(); return; }
    ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) WorldDuplicate();
}
void DrawWorld() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 520, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin(T("Мир"));
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(T("Режим"), worldMode.empty() ? T("— выберите —") : worldMode.c_str())) {
        for (auto& m : modes) if (ImGui::Selectable(m.c_str(), m == worldMode)) actions.push_back({Action::WorldLoad, m});
        ImGui::EndCombo();
    }
    if (worldMode.empty()) { ImGui::TextWrapped(T("Выберите режим: правки мира хранятся у каждого режима свои (modes/<режим>/world.json).")); ImGui::End(); return; }
    ImGui::SameLine(); if (ImGui::Button(T("Сохранить"))) actions.push_back({Action::WorldSave, ""});
    ImGui::BeginDisabled(worldUndo.empty()); if (ImGui::Button(T("Отменить"))) WorldRestore(worldUndo, worldRedo, T("Отменено")); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(worldRedo.empty()); if (ImGui::Button(T("Вернуть"))) WorldRestore(worldRedo, worldUndo, T("Возвращено")); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextDisabled("Ctrl+Z/Y/D   Del");
    ImGui::Text(T("Убрано: %zu   Сдвинуто: %zu   Добавлено: %zu"), world.remove.size(), world.move.size(), world.add.size());
    DrawEnvironment();
    ImGui::SeparatorText(T("Свет"));
    for (const char* kind : {"light:point", "light:spot"}) {
        const bool armed = placeProp == kind;
        const char* label = std::strcmp(kind, "light:spot") == 0 ? T("Прожектор") : T("Точечный свет");
        if (std::strcmp(kind, "light:spot") == 0) ImGui::SameLine();
        if (ImGui::Button(armed ? (std::string(T("отмена")) + "##" + kind).c_str() : (std::string("+ ") + label).c_str())) {
            placeProp = armed ? "" : kind; placeLogic.clear(); placeZone = 0; placeCarry = false; placeEnt = 0;
            if (!armed) Toast(T("ЛКМ в мире ставит: ") + std::string(label));
        }
    }
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Свет для интерьеров. Прожектор светит из основания конуса (по умолчанию вниз), поворачивается гизмо. Тени дорогие: включайте немногим."));
    ImGui::SeparatorText("NPC");
    {
        const bool armed = placeActor == "*";
        if (ImGui::Button(armed ? T("отмена##npc") : "+ NPC")) {
            placeActor = armed ? "" : "*"; placeProp.clear(); placeLogic.clear(); placeZone = 0; placeCarry = false; placeEnt = 0;
            if (!armed) Toast(T("ЛКМ в мире ставит NPC"));
        }
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Статисты режима: облик, анимация, имя. Сервер создаёт их сам при запуске режима; код режима находит их по id: logic.actor(\"id\"), logic.actors(\"группа\"). Клик по NPC в мире выделяет его."));
        if (!world.actors.empty()) {
            ImGui::SameLine();
            if (ImGui::TreeNode(T("Поставленные NPC"))) {
                for (auto& a : world.actors) if (ImGui::Selectable((std::string(a.id) + (a.name[0] ? "  —  " + std::string(a.name) : std::string()) + "##npc").c_str(), wsel.kind == 4 && wsel.id == a.id)) SelectActor(a.id);
                ImGui::TreePop();
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", T("NPC, созданные кодом режима (createActor), здесь не видны. Ставьте их здесь, а в коде берите через logic.actor(\"id\") / logic.actors(\"группа\")."));
        ImGui::PopStyleColor();
    }
    ImGui::SeparatorText(T("Переноска"));
    {
        if (ImGui::Button(placeCarry ? T("отмена##carry") : T("+ Ящик"))) {
            placeCarry = !placeCarry; placeProp.clear(); placeLogic.clear(); placeActor.clear(); placeZone = 0; placeEnt = 0;
            if (placeCarry) Toast(T("ЛКМ в мире ставит ящик"));
        }
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Ящики, которые игроки носят двумя руками (зажать F). Сервер кладёт их при запуске режима; код режима находит их по ключу: getCarry(\"ключ\"), по метке: carries(\"метка\"). Новый ящик берёт модель, метку и мир выделенного."));
        if (!world.carries.empty()) {
            ImGui::SameLine();
            if (ImGui::TreeNode(T("Поставленные ящики"))) {
                for (auto& c : world.carries) if (ImGui::Selectable((std::string(c.key) + "  —  " + CarryModelName(c.model) + (c.tag[0] ? std::string("  [") + c.tag + "]" : std::string()) + "##carry").c_str(), wsel.kind == 6 && wsel.id == c.key)) SelectCarry(c.key);
                ImGui::TreePop();
            }
        }
    }
    {
        static const char* const titles[] = {"Транспорт", "Входы", "Лут"};
        static const char* const adds[] = {"+ Транспорт", "+ Вход", "+ Лут"};
        static const char* const cancels[] = {"отмена##veh", "отмена##door", "отмена##loot"};
        static const char* const hints[] = {"ЛКМ в мире ставит транспорт", "ЛКМ в мире ставит вход (снаружи)", "ЛКМ в мире кладёт предмет (выбранный в библиотеке)"};
        static const char* const helps[] = {"Транспорт режима: мотоциклы, гольф-кары. Сервер создаёт его при запуске режима; код находит его по ключу: getVehicle(\"ключ\").",
                                           "Двери в интерьеры, как входы в дома на RP-серверах SA-MP: [F] у одной стороны переносит к другой и в её виртуальный мир. Код режима может запереть вход (setDoor) или решить сам (событие door).",
                                           "Предметы, которые лежат в мире и возвращаются через заданное время после того, как их подобрали."};
        static const char* const lists[] = {"Поставленный транспорт", "Поставленные входы", "Поставленный лут"};
        for (int type = 0; type < 3; ++type) {
            ImGui::SeparatorText(T(titles[type]));
            const bool armed = placeEnt == type + 1;
            if (ImGui::Button(armed ? T(cancels[type]) : T(adds[type]))) {
                placeEnt = armed ? 0 : type + 1; doorPending.clear();
                placeProp.clear(); placeLogic.clear(); placeActor.clear(); placeZone = 0; placeCarry = false;
                if (!armed) Toast(T(hints[type]));
            }
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(helps[type]));
            bool any = false; for (auto& e : world.ents) any |= e.type == type;
            if (!any) continue;
            ImGui::SameLine();
            if (ImGui::TreeNode(T(lists[type]))) {
                for (auto& e : world.ents) if (e.type == type) {
                    std::string label = e.key;
                    if (type == 0) label += std::string("  —  ") + e.model;
                    if (type == 1 && e.text[0]) label += std::string("  —  ") + e.text;
                    if (type == 2) label += std::string("  —  ") + e.item;
                    if (ImGui::Selectable((label + "##ent" + std::to_string(type)).c_str(), wsel.kind == 7 && EntOf(wsel.id) == &e)) SelectEnt(e);
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::SeparatorText(T("Выделено"));
    if (wsel.kind == 0) ImGui::TextDisabled(placeProp.empty() ? T("Кликните по объекту в мире") : T("Кликните в мире, чтобы поставить проп"));
    else if (wsel.kind == 4) DrawActorProps();
    else if (wsel.kind == 5) DrawZoneProps();
    else if (wsel.kind == 6) DrawCarryProps();
    else if (wsel.kind == 7) DrawEntProps();
    else {
        ImGui::TextWrapped("%s", wsel.kind == 2 ? (T("Проп ") + wsel.id).c_str() : wsel.id.c_str());
        if (!wsel.cls.empty()) ImGui::TextDisabled("%s", wsel.cls.c_str());
        WT& t = wsel.t;
        bool changed = false;
        changed |= ImGui::DragFloat3(T("Позиция"), &t.x, 5.f, 0, 0, "%.0f");
        changed |= ImGui::DragFloat3(T("Поворот"), &t.pitch, 1.f, -360, 360, "%.0f°");
        WAdd* added = nullptr;
        if (wsel.kind == 2) for (auto& a : world.add) if (a.uid == wsel.id) added = &a;
        if (wsel.kind == 2 && !(added && added->light)) changed |= ImGui::DragFloat(T("Масштаб"), &t.scale, 0.01f, 0.05f, 20.f, "%.2f");
        if (added && added->light) {
            WLight& l = added->l;
            bool lit = false;
            lit |= ImGui::ColorEdit3(T("Цвет"), l.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
            lit |= ImGui::DragFloat(T("Яркость"), &l.intensity, 50.f, 0.f, 200000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
            lit |= ImGui::DragFloat(T("Дальность, м"), &l.radius, 0.1f, 0.5f, 200.f, "%.1f");
            if (added->asset == "light:spot") lit |= ImGui::DragFloat(T("Угол конуса"), &l.cone, 0.5f, 2.f, 89.f, "%.0f°");
            lit |= ImGui::Checkbox(T("Тени"), &l.shadows);
            if (lit) worldDirty = true;
        }
        LItem* li = wsel.kind == 3 ? LogicOf(wsel.id) : nullptr;
        const LDecl* ld = li ? FindDecl(wsel.id.substr(0, wsel.id.rfind('#'))) : nullptr;
        if (li && (!ld || ld->trigger)) ImGui::DragFloat(T("Радиус, м"), &li->radius, 0.1f, 0.5f, 200.f, "%.1f");
        if (changed) {
            if (wsel.kind == 1) world.move[wsel.id] = t;
            else if (li) li->t = t;
            else for (auto& a : world.add) if (a.uid == wsel.id) a.t = t;
            worldDirty = true;
        }
        if (ImGui::Button(wsel.kind == 1 ? T("Убрать из мира") : wsel.kind == 3 ? T("Удалить") : T("Удалить проп"))) WorldDelete();
        if (wsel.kind == 1 && world.move.count(wsel.id)) { ImGui::SameLine(); if (ImGui::Button(T("Вернуть на место"))) { world.move.erase(wsel.id); wsel = {}; worldDirty = true; } }
        if (wsel.kind == 2) { ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) WorldDuplicate(); }
    }
    if (!world.remove.empty() && ImGui::TreeNode(T("Убранные объекты"))) {
        for (size_t i = 0; i < world.remove.size(); ++i) {
            ImGui::PushID(int(i));
            if (ImGui::SmallButton(T("вернуть"))) { world.remove.erase(world.remove.begin() + long(i)); worldDirty = true; ImGui::PopID(); break; }
            ImGui::SameLine(); ImGui::TextUnformatted(world.remove[i].c_str());
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (!world.add.empty() && ImGui::TreeNode(T("Поставленные пропсы"))) {
        for (auto& a : world.add) if (ImGui::Selectable((a.name + "##" + a.uid).c_str(), wsel.kind == 2 && wsel.id == a.uid)) wsel = {2, a.uid, "", a.t};
        ImGui::TreePop();
    }
    DrawZones();
    DrawLogicList();
    ImGui::End();
}

void DrawProps() {
    if (props.empty()) { ImGui::TextWrapped(T("Каталог пропсов не найден (data/props.txt).")); return; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##psearch", T("Поиск пропа"), propSearch, sizeof(propSearch));
    if (ImGui::BeginChild("pcats", ImVec2(150, 0), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < int(propCats.size()); ++i) if (ImGui::Selectable((std::string(T(propCats[i].c_str())) + "##c" + std::to_string(i)).c_str(), propCat == i)) propCat = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("plist", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const bool searching = propSearch[0] != 0;
        std::string lastSub;
        int shown = 0;
        for (auto& pr : props) {
            if (searching ? !(Contains(pr.name, propSearch) || Contains(pr.path, propSearch)) : (propCat >= int(propCats.size()) || pr.cat != propCats[propCat])) continue;
            if (++shown > 600) { ImGui::TextDisabled(T("…уточните поиск")); break; }
            if (!searching && pr.sub != lastSub) { lastSub = pr.sub; if (!pr.sub.empty()) ImGui::SeparatorText(pr.sub.c_str()); }
            ImGui::PushID(pr.path.c_str());
            if (ImGui::Selectable(pr.name.c_str(), placeProp == pr.path)) {
                placeProp = placeProp == pr.path ? "" : pr.path;
                if (!placeProp.empty()) Toast(T("ЛКМ в мире ставит: ") + pr.name);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", pr.path.c_str(), pr.kind == "blueprint" ? T("  (блюпринт)") : "");
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawLibrary() {
    ImGui::SetNextWindowPos(ImVec2(20, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin(T("Библиотека"));
    if (ImGui::BeginTabBar("libtabs")) {
        if (ImGui::BeginTabItem(T("Пропсы"))) { libraryTab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(T("Предметы"))) { libraryTab = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (libraryTab == 0) { DrawProps(); ImGui::End(); return; }
    if (!catalogReady) { ImGui::TextDisabled(T("Загрузка списка предметов…")); ImGui::End(); return; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", T("Поиск по названию или ключу"), librarySearch, sizeof(librarySearch));
    if (ImGui::BeginChild("types", ImVec2(130, 0), ImGuiChildFlags_Borders)) {
        for (auto& t : kItemTypes) if (ImGui::Selectable(T(t.name), libraryType == t.type)) libraryType = t.type;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("items", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const bool searching = librarySearch[0] != 0;
        for (auto& it : catalog) {
            const bool known = it.type == 2 || it.type == 3 || it.type == 4 || it.type == 5 || it.type == 6 || it.type == 8 || it.type == 9 || it.type == 12;
            if (!searching && (known ? it.type != libraryType : libraryType != 0)) continue;
            if (searching && !Contains(it.name, librarySearch) && !Contains(it.english, librarySearch) && !Contains(it.key, librarySearch)) continue;
            ImGui::PushID(it.key.c_str());
            if (ImGui::Selectable(english ? it.english.c_str() : it.name.c_str(), it.key == placeItem)) {
                Copy(placeItem, sizeof(placeItem), it.key);
                if (mode == 1) { clickMode = kItem; Toast(T("ЛКМ в мире кладёт: ") + (english ? it.english : it.name)); }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.key.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// A button that arms what the next left click into the world places (orange while armed).
void ClickModeButton(const char* label, int m) {
    const bool on = clickMode == m;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1));
    if (ImGui::Button(label)) clickMode = on ? kSelect : m;
    if (on) ImGui::PopStyleColor();
}
// Boxes to carry from the pick-up area to the drop-off area (docs/quests.md "deliver").
void DrawDeliver(Step& s, int index) {
    ImGui::TextWrapped("%s", T("Засчитывается ящик, поставленный (зажать F) в зоне разгрузки. Брать эти ящики может только тот, кто на этом этапе (у работы — кто на работе)."));
    ImGui::SeparatorText(T("Погрузка: где лежат ящики"));
    ImGui::PushID("pick");
    if (s.hasPickup) {
        char line[120]; snprintf(line, sizeof(line), T("Погрузка: %.0f  %.0f  %.0f"), s.px, s.py, s.pz);
        if (ImGui::Selectable(line, picked.step == index && picked.kind == 3)) picked = {index, 3, 0};
    } else ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T("Не задано — кнопка ниже, потом ЛКМ в мире"));
    ClickModeButton(clickMode == kPickup ? T("Кликните в мире…") : T("Поставить погрузку"), kPickup);
    if (ImGui::SliderFloat(T("Радиус погрузки, м"), &s.pr, 1, 20, "%.0f")) previewDirty = true;
    ImGui::InputTextWithHint(T("Название на карте"), T("Погрузка"), Cur(s.pickName), sizeof(s.pickName.en));
    // Boxes placed in the world (World > Carrying) by tag, else new ones around the point.
    ImGui::InputText(T("Ящики мира (метка)"), s.tag, sizeof(s.tag));
    std::vector<std::string> tags;
    for (auto& c : world.carries) if (c.tag[0] && std::find(tags.begin(), tags.end(), c.tag) == tags.end()) tags.emplace_back(c.tag);
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::SmallButton(tags[i].c_str())) Copy(s.tag, sizeof(s.tag), tags[i]);
    }
    const auto tagged = s.tag[0] ? std::count_if(world.carries.begin(), world.carries.end(), [&](const WCarry& c) { return std::strcmp(c.tag, s.tag) == 0; }) : 0;
    if (tagged > 0) ImGui::TextWrapped(T("В режиме «%s» с этой меткой ящиков: %d — берутся они и возвращаются на свои места"), worldMode.c_str(), int(tagged));
    else ImGui::TextDisabled("%s", T("Ящики кладутся вокруг точки погрузки"));
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(T("Ящиков на погрузке"), &s.boxes)) s.boxes = std::clamp(s.boxes, 1, 24);
    if (ImGui::BeginCombo(T("Модель"), CarryModelName(s.model))) {
        int n = 0; const carry::ModelInfo* m = carry::Models(n);
        for (int i = 0; i < n; ++i) if (ImGui::Selectable(english ? m[i].en : m[i].ru, std::strcmp(m[i].name, s.model) == 0)) Copy(s.model, sizeof(s.model), m[i].name);
        ImGui::EndCombo();
    }
    static const char* const rolls[] = {"Ничего", "Падает", "Ломается"};
    ImGui::Combo(T("Перекат с ящиком"), &s.roll, TArr(rolls, 3), 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Что будет, если игрок сделает перекат с ящиком в руках: ничего, ящик падает на землю, ящик ломается (исчезает)"));
    ImGui::PopID();
    ImGui::SeparatorText(T("Разгрузка: куда их отнести"));
    ImGui::PushID("drop");
    if (s.hasDrop) {
        char line[120]; snprintf(line, sizeof(line), T("Разгрузка: %.0f  %.0f  %.0f"), s.dx, s.dy, s.dz);
        if (ImGui::Selectable(line, picked.step == index && picked.kind == 4)) picked = {index, 4, 0};
    } else ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T("Не задано — кнопка ниже, потом ЛКМ в мире"));
    ClickModeButton(clickMode == kDropoff ? T("Кликните в мире…") : T("Поставить разгрузку"), kDropoff);
    if (ImGui::SliderFloat(T("Радиус разгрузки, м"), &s.dr, 1, 20, "%.0f")) previewDirty = true;
    ImGui::InputTextWithHint(T("Название на карте"), T("Разгрузка"), Cur(s.dropName), sizeof(s.dropName.en));
    ImGui::PopID();
    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(T("Сколько ящиков отнести"), &s.count)) s.count = std::max(1, s.count);
    ImGui::Checkbox(T("Маршрут к разгрузке"), &s.route);
}

void DrawStepEditor(Step& s, int index) {
    ImGui::SeparatorText((T("Этап ") + std::to_string(index + 1)).c_str());
    if (ImGui::Combo(T("Тип"), &s.type, TArr(kStepTypes, kStepCount), kStepCount)) { if (s.type == 3 && s.count < 1) s.count = 1; previewDirty = true; }
    if (ImGui::InputText(T("Текст"), Cur(s.text), sizeof(s.text.en))) previewDirty = true;
    if (s.type == 0 || s.type == 1) {
        if (s.hasPoint) {
            char point[120]; snprintf(point, sizeof(point), T("Точка: %.0f  %.0f  %.0f"), s.x, s.y, s.z);
            if (ImGui::Selectable(point, picked.step == index && picked.kind == 0)) picked = {index, 0, 0};
        }
        else ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), T("Точка не задана — ЛКМ в мире"));
        if (ImGui::SliderFloat(T("Радиус, м"), &s.radius, 1, 50, "%.0f")) previewDirty = true;
        if (s.type == 0) ImGui::Checkbox(T("Маршрут по дорогам"), &s.route);
        else { ImGui::SliderFloat(T("Секунд в зоне"), &s.seconds, 1, 120, "%.0f"); ImGui::InputTextWithHint(T("Шкала"), contentLang ? "Поиск" : "Searching", Cur(s.meter), sizeof(s.meter.en)); }
        if (ImGui::Button(clickMode == kPoint ? T("Кликните в мире…") : T("Поставить точку"))) clickMode = clickMode == kPoint ? kSelect : kPoint;
        return;
    }
    if (s.type == 3) { DrawDeliver(s, index); return; }
    if (s.type == 4) {
        ImGui::InputTextWithHint(T("NPC (id из «Мир» → NPC)"), T("пусто — точка ниже"), s.actor, sizeof(s.actor));
        if (!s.actor[0]) {
            if (s.hasPoint) {
                char point[120]; snprintf(point, sizeof(point), T("Точка: %.0f  %.0f  %.0f"), s.x, s.y, s.z);
                if (ImGui::Selectable(point, picked.step == index && picked.kind == 0)) picked = {index, 0, 0};
            } else ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), T("Точка не задана — ЛКМ в мире"));
            if (ImGui::Button(clickMode == kPoint ? T("Кликните в мире…") : T("Поставить точку"))) clickMode = clickMode == kPoint ? kSelect : kPoint;
        }
        if (ImGui::SliderFloat(T("Радиус, м"), &s.radius, 1, 20, "%.0f")) previewDirty = true;
        ImGui::Checkbox(T("Маршрут по дорогам"), &s.route);
        ImGui::InputText(T("Кто говорит"), Cur(s.speaker), sizeof(s.speaker.en));
        ImGui::TextUnformatted(T("Реплики (по очереди, в окне диалога):"));
        for (size_t i = 0; i < s.lines.size(); ++i) {
            ImGui::PushID(2000 + int(i));
            if (ImGui::SmallButton("×")) { s.lines.erase(s.lines.begin() + long(i)); ImGui::PopID(); break; }
            ImGui::SameLine();
            ImGui::InputTextMultiline("##line", Cur(s.lines[i]), sizeof(s.lines[i].en), ImVec2(-1, ImGui::GetTextLineHeight() * 3));
            ImGui::PopID();
        }
        if (ImGui::SmallButton(T("+ реплика"))) s.lines.emplace_back();
        return;
    }
    ImGui::TextUnformatted(T("Что засчитывать:"));
    for (int i = 0; i < 8; ++i) {
        bool on = (s.events & (1u << i)) != 0;
        if (i % 2) ImGui::SameLine(200);
        if (ImGui::Checkbox(T(kEvents[i]), &on)) { if (on) s.events |= 1u << i; else s.events &= ~(1u << i); }
    }
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(T("Сколько раз (0 — по числу объектов)"), &s.count);
    if (s.count < 0) s.count = 0;
    ImGui::InputText(T("Предмет (ключ)"), s.item, sizeof(s.item));
    ImGui::InputText(T("Класс объекта содержит"), s.cls, sizeof(s.cls));

    ImGui::SeparatorText(T("Поставить кликом в мире"));
    ClickModeButton(T("Зону"), kPoint); ImGui::SameLine();
    ClickModeButton(T("Объект"), kObject); ImGui::SameLine();
    ClickModeButton(T("Предмет"), kItem); ImGui::SameLine();
    ImGui::Checkbox(T("Повторять"), &repeatPlace);
    if (clickMode == kItem) ImGui::TextDisabled(T("Предмет: %s"), placeItem[0] ? ItemName(placeItem) : T("выберите в библиотеке"));

    if (s.hasArea) {
        char zone[120]; snprintf(zone, sizeof(zone), T("Зона: %.0f  %.0f  %.0f"), s.ax, s.ay, s.az);
        if (ImGui::Selectable(zone, picked.step == index && picked.kind == 1, 0, ImVec2(ImGui::CalcTextSize(zone).x, 0))) picked = {index, 1, 0};
        ImGui::SameLine(); if (ImGui::SmallButton(T("Убрать зону"))) { s.hasArea = false; previewDirty = true; }
        if (ImGui::SliderFloat(T("Радиус зоны, м"), &s.ar, 1, 50, "%.0f")) previewDirty = true;
        ImGui::Checkbox(T("Маркер над зоной"), &s.marker);
    }
    if (!s.ids.empty()) {
        ImGui::TextUnformatted(T("Конкретные объекты:"));
        for (size_t i = 0; i < s.ids.size(); ++i) {
            ImGui::PushID(int(i));
            if (ImGui::SmallButton("×")) { s.ids.erase(s.ids.begin() + long(i)); ImGui::PopID(); break; }
            ImGui::SameLine(); ImGui::TextUnformatted(s.ids[i].c_str());
            ImGui::PopID();
        }
    }
    if (!s.spawn.empty()) {
        ImGui::TextUnformatted(T("Предметы квеста (появятся на этом этапе):"));
        for (size_t i = 0; i < s.spawn.size(); ++i) {
            ImGui::PushID(1000 + int(i));
            if (ImGui::SmallButton("×")) { s.spawn.erase(s.spawn.begin() + long(i)); previewDirty = true; ImGui::PopID(); break; }
            ImGui::SameLine();
            // Click a row to select the item (the gizmo appears on it in the world).
            char row[200]; snprintf(row, sizeof(row), "%s  (%.0f %.0f %.0f)", ItemName(s.spawn[i].item), s.spawn[i].x, s.spawn[i].y, s.spawn[i].z);
            if (ImGui::Selectable(row, picked.step == index && picked.kind == 2 && picked.index == int(i))) picked = {index, 2, int(i)};
            ImGui::PopID();
        }
    }
}

// The job's giver, uniform and pay ("kind": "job", docs/quests.md).
void DrawJob() {
    ImGui::PushID("job");
    ImGui::TextWrapped("%s", T("Этапы ниже повторяются по кругу, пока игрок на работе. Раз = все этапы по одному разу."));
    ImGui::SeparatorText(T("Кто даёт работу"));
    const std::string own = T("свой NPC (ниже)");
    if (ImGui::BeginCombo(T("NPC из мира"), quest.giverActor[0] ? quest.giverActor : own.c_str())) {
        if (ImGui::Selectable(own.c_str(), !quest.giverActor[0])) { quest.giverActor[0] = 0; previewDirty = true; }
        for (auto& a : world.actors) {
            const std::string label = std::string(a.id) + (a.name[0] ? std::string("  ") + a.name : std::string());
            if (ImGui::Selectable(label.c_str(), std::strcmp(a.id, quest.giverActor) == 0)) {
                Copy(quest.giverActor, sizeof(quest.giverActor), a.id);
                // The same NPC of the job's own, for modes without this one (questtest).
                quest.giverHasPoint = true; quest.gx = a.t.x; quest.gy = a.t.y; quest.gz = a.t.z; quest.gyaw = a.t.yaw;
                const std::string look = ModeSkinLook(a.look);
                Copy(quest.giverLook, sizeof(quest.giverLook), look.empty() ? std::string(a.look) : look);
                Copy(quest.giverAnim, sizeof(quest.giverAnim), a.anim[0] ? std::string(a.anim) : std::string("idle"));
                LFrom(json::Value(std::string(a.name)), quest.giverName);
                previewDirty = true;
            }
        }
        ImGui::EndCombo();
    }
    if (worldMode.empty()) ImGui::TextDisabled("%s", T("NPC мира — из режима, открытого во вкладке «Мир»"));
    else if (quest.giverActor[0]) ImGui::TextWrapped(T("В режиме «%s» работу даёт этот NPC. В других режимах (и в «Проверить») на его месте встаёт такой же NPC работы."), worldMode.c_str());
    if (quest.giverHasPoint) {
        char line[120]; snprintf(line, sizeof(line), T("Место NPC: %.0f  %.0f  %.0f"), quest.gx, quest.gy, quest.gz);
        if (ImGui::Selectable(line, picked.kind == 5, 0, ImVec2(ImGui::CalcTextSize(line).x, 0))) picked = {-1, 5, 0};
    } else ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T("Место NPC не задано — ЛКМ в мире"));
    ClickModeButton(clickMode == kGiver ? T("Кликните в мире…") : T("Поставить NPC"), kGiver);
    ImGui::InputTextWithHint(T("Имя NPC"), contentLang ? "Бригадир" : "Foreman", Cur(quest.giverName), sizeof(quest.giverName.en));
    LookField(T("Облик"), quest.giverLook, sizeof(quest.giverLook));
    AnimField(T("Анимация"), quest.giverAnim, sizeof(quest.giverAnim));
    ImGui::Checkbox(T("Метка на карте"), &quest.giverMarker);
    ImGui::SeparatorText(T("Форма и оплата"));
    LookField(T("Форма на работе"), quest.uniform, sizeof(quest.uniform), T("без формы"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Облик на время работы: id формы режима или облик игры (пусто — без формы)"));
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(T("Оплата, $ за раз"), &quest.pay);
    ImGui::Combo(T("Когда платить"), &quest.payout, TArr(kPayouts, 2), 2);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(T("Раз за смену (0 — без ограничения)"), &quest.cycles)) quest.cycles = std::max(0, quest.cycles);
    ImGui::PopID();
}

void DrawQuest() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 520, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 760), ImGuiCond_FirstUseEver);
    ImGui::Begin(T("Квест"));
    // File row
    const char* current = questOpen ? (loadedId.empty() ? T("(новый)") : loadedId.c_str()) : T("— выберите —");
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##file", current)) {
        for (auto& id : questFiles) if (ImGui::Selectable(id.c_str(), id == loadedId)) actions.push_back({Action::Load, id});
        ImGui::EndCombo();
    }
    ImGui::SameLine(); if (ImGui::Button(T("Новый"))) actions.push_back({Action::New, ""});
    if (questOpen) {
        ImGui::SameLine(); if (ImGui::Button(T("Сохранить"))) actions.push_back({Action::Save, ""});
        ImGui::SameLine(); if (ImGui::Button(T("Проверить"))) actions.push_back({Action::Test, ""});
        ImGui::SameLine(); if (ImGui::Button(T("Удалить"))) ImGui::OpenPopup("delete");
        if (ImGui::BeginPopup("delete")) {
            ImGui::Text(T("Удалить квест %s?"), loadedId.c_str());
            if (ImGui::Button(T("Да, удалить"))) { actions.push_back({Action::Delete, loadedId}); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (!questOpen) { ImGui::TextWrapped(T("Откройте квест или создайте новый.")); ImGui::End(); return; }
    ImGui::BeginDisabled(undoStack.empty()); if (ImGui::Button(T("Отменить"))) Undo(); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(redoStack.empty()); if (ImGui::Button(T("Вернуть"))) Redo(); ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button(T("Дублировать"))) Duplicate();
    ImGui::SameLine(); ImGui::TextDisabled("Ctrl+Z/Y/C/V/D");
    ImGui::TextUnformatted(T("Язык текстов:")); ImGui::SameLine();
    ImGui::RadioButton("EN##clang", &contentLang, 0); ImGui::SameLine(); ImGui::RadioButton("RU##clang", &contentLang, 1);
    ImGui::SameLine(); ImGui::TextDisabled("%s", T("игрок видит свой язык, иначе английский"));

    if (ImGui::CollapsingHeader(T("Общее"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(T("Файл (id)"), quest.id, sizeof(quest.id));
        int kind = quest.job ? 1 : 0;
        if (ImGui::Combo(T("Вид"), &kind, TArr(kKinds, 2), 2)) { quest.job = kind == 1; previewDirty = true; }
        ImGui::InputText(T("Название"), Cur(quest.title), sizeof(quest.title.en));
        ImGui::Combo(T("Категория"), &quest.category, TArr(kCategories, 8), 8);
        ImGui::Combo(T("Вид маркеров"), &quest.style, TArr(kStyles, 8), 8);
        ImGui::InputTextMultiline(T("Вступление"), Cur(quest.text), sizeof(quest.text.en), ImVec2(-1, 50));
        ImGui::InputInt(quest.job ? T("Опыт при сдаче работы") : T("Опыт"), &quest.xp);
        if (!quest.job && ImGui::TreeNode(T("Награда"))) {
            for (size_t i = 0; i < quest.reward.size(); ++i) {
                ImGui::PushID(2000 + int(i));
                ImGui::SetNextItemWidth(220); ImGui::InputText("##item", quest.reward[i].item, sizeof(quest.reward[i].item));
                ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("##count", &quest.reward[i].count);
                ImGui::SameLine(); if (ImGui::SmallButton("×")) { quest.reward.erase(quest.reward.begin() + long(i)); ImGui::PopID(); break; }
                ImGui::PopID();
            }
            if (ImGui::SmallButton(T("+ из библиотеки")) && placeItem[0]) { Reward w; Copy(w.item, sizeof(w.item), placeItem); quest.reward.push_back(w); }
            ImGui::TreePop();
        }
    }
    if (quest.job && ImGui::CollapsingHeader(T("Работа"), ImGuiTreeNodeFlags_DefaultOpen)) DrawJob();
    ImGui::SeparatorText(T("Этапы"));
    for (size_t i = 0; i < quest.steps.size(); ++i) {
        const Step& s = quest.steps[i];
        const std::string label = std::to_string(i + 1) + ". " + T(kStepTypes[s.type]) + ": " + Shown(s.text) + "##step" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected == int(i))) { selected = int(i); previewDirty = true; }
    }
    const char* adds[] = {T("+ Дойти"), T("+ Удерживать"), T("+ Сделать N раз"), T("+ Перенести ящики"), T("+ Поговорить")};
    for (int t = 0; t < kStepCount; ++t) {
        if (t) ImGui::SameLine();
        if (ImGui::Button(adds[t])) {
            Step s; s.type = t;
            if (t == 2) s.events = 1;
            if (t == 3) s.count = 1;
            if (t == 4) { s.radius = 3; s.lines.emplace_back(); }
            quest.steps.push_back(s); selected = int(quest.steps.size()) - 1; previewDirty = true;
        }
    }
    if (selected >= 0 && selected < int(quest.steps.size())) {
        ImGui::SameLine(); if (ImGui::Button("↑") && selected > 0) { std::swap(quest.steps[selected], quest.steps[selected - 1]); --selected; previewDirty = true; }
        ImGui::SameLine(); if (ImGui::Button("↓") && selected + 1 < int(quest.steps.size())) { std::swap(quest.steps[selected], quest.steps[selected + 1]); ++selected; previewDirty = true; }
        ImGui::SameLine(); if (ImGui::Button(T("Удалить этап"))) { quest.steps.erase(quest.steps.begin() + selected); selected = std::min(selected, int(quest.steps.size()) - 1); previewDirty = true; }
        if (selected >= 0) DrawStepEditor(quest.steps[selected], selected);
    }
    ImGui::End();
}


// ---------------------------------------------------------------- history and clipboard
// Snapshots of the whole quest. A change becomes one step once the edit is finished (no field, slider
// or gizmo being dragged), so a drag or a typed word is undone at once.
Quest committedQuest; std::string committedJson; bool committedValid = false;
void ResetHistory() { undoStack.clear(); redoStack.clear(); committedValid = false; }
void TrackHistory() {
    if (!questOpen) { committedValid = false; return; }
    if (ImGui::IsAnyItemActive() || ImGuizmo::IsUsing() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    std::string now = ToJson(quest).dump();
    if (!committedValid) { committedJson = std::move(now); committedQuest = quest; committedValid = true; return; }
    if (now == committedJson) return;
    undoStack.push_back(committedQuest);
    if (undoStack.size() > 100) undoStack.erase(undoStack.begin());
    redoStack.clear();
    committedJson = std::move(now); committedQuest = quest;
}
void Restore(std::vector<Quest>& from, std::vector<Quest>& to, const char* done) {
    if (from.empty()) return;
    to.push_back(quest);
    quest = from.back(); from.pop_back();
    committedJson = ToJson(quest).dump(); committedQuest = quest; committedValid = true;
    if (selected >= int(quest.steps.size())) selected = int(quest.steps.size()) - 1;
    picked = {};
    previewDirty = true;
    Toast(done);
}
void Undo() { Restore(undoStack, redoStack, T("Отменено")); }
void Redo() { Restore(redoStack, undoStack, T("Возвращено")); }

// Clipboard: a quest item or a whole step.
struct Clip { int kind = 0; SpawnRow item; Step step; };  // kind 0 empty, 1 item, 2 step
Clip clip;
void Copy() {
    if (picked.kind == 2 && picked.step >= 0 && picked.step < int(quest.steps.size()) && picked.index < int(quest.steps[picked.step].spawn.size())) {
        clip.kind = 1; clip.item = quest.steps[picked.step].spawn[picked.index]; Toast(T("Скопирован предмет"));
    } else if (selected >= 0 && selected < int(quest.steps.size())) {
        clip.kind = 2; clip.step = quest.steps[selected]; Toast(T("Скопирован этап ") + std::to_string(selected + 1));
    }
}
void Paste() {
    if (clip.kind == 1) {
        if (selected < 0 || selected >= int(quest.steps.size()) || quest.steps[selected].type != 2) { Toast(T("Выберите этап «Сделать N раз»")); return; }
        SpawnRow row = clip.item; row.x += 100; row.y += 100;  // beside the original
        quest.steps[selected].spawn.push_back(row);
        picked = {selected, 2, int(quest.steps[selected].spawn.size()) - 1};
        Toast(T("Вставлен предмет"));
    } else if (clip.kind == 2) {
        const int at = selected < 0 ? int(quest.steps.size()) : selected + 1;
        quest.steps.insert(quest.steps.begin() + at, clip.step);
        selected = at; picked = {};
        Toast(T("Вставлен этап ") + std::to_string(at + 1));
    } else return;
    previewDirty = true;
}
void Duplicate() { const Clip keep = clip; Copy(); Paste(); clip = keep; }
void HistoryKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (!questOpen || io.WantTextInput || !io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
    else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    else if (ImGui::IsKeyPressed(ImGuiKey_C, false)) Copy();
    else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) Paste();
    else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) Duplicate();
}

// ---------------------------------------------------------------- selection and gizmo
// The game camera as matrices for ImGuizmo (column-major, row-vector convention). World stays in
// Unreal units; the view maps it to an OpenGL-style camera (x right, y up, looking down -z).
void Matrices(float* view, float* proj, float width, float height) {
    const float rad = 0.01745329252f, cp = cosf(cam.rot.Pitch * rad), sp = sinf(cam.rot.Pitch * rad), cy = cosf(cam.rot.Yaw * rad), sy = sinf(cam.rot.Yaw * rad);
    const FVector f{cp * cy, cp * sy, sp}, r{-sy, cy, 0}, u{-sp * cy, -sp * sy, cp};
    const FVector e = cam.loc;
    auto dot = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
    const float v[16] = {r.X, u.X, -f.X, 0, r.Y, u.Y, -f.Y, 0, r.Z, u.Z, -f.Z, 0, -dot(e, r), -dot(e, u), dot(e, f), 1};
    std::memcpy(view, v, sizeof(v));
    const float n = 10.f, fr = 2000000.f, t = 1.f / tanf(cam.fov * rad * 0.5f), aspect = width / std::max(height, 1.f);
    const float p[16] = {t, 0, 0, 0, 0, t * aspect, 0, 0, 0, 0, -(fr + n) / (fr - n), -1, 0, 0, -2 * fr * n / (fr - n), 0};
    std::memcpy(proj, p, sizeof(p));
}
// ImGuizmo expects a right-handed world; Unreal's is left-handed (X forward, Y right, Z up), which
// makes the rotation rings draw their back halves. Hand it the world mirrored in Y and mirror back.
bool Gizmo(const float* view, const float* proj, ImGuizmo::OPERATION op, float* m, const float* snap) {
    float v[16], mm[16];
    std::memcpy(v, view, sizeof(v)); std::memcpy(mm, m, sizeof(mm));
    for (int c = 0; c < 4; ++c) v[4 + c] = -v[4 + c];                 // view' = F * view
    for (int c = 0; c < 4; ++c) mm[4 + c] = -mm[4 + c];               // model' = F * model * F
    for (int r = 0; r < 4; ++r) mm[r * 4 + 1] = -mm[r * 4 + 1];
    const bool changed = ImGuizmo::Manipulate(v, proj, op, ImGuizmo::WORLD, mm, nullptr, snap);
    if (changed) {
        for (int c = 0; c < 4; ++c) mm[4 + c] = -mm[4 + c];
        for (int r = 0; r < 4; ++r) mm[r * 4 + 1] = -mm[r * 4 + 1];
        std::memcpy(m, mm, sizeof(mm));
    }
    return changed;
}
bool Project(const float* view, const float* proj, float width, float height, float x, float y, float z, ImVec2& out) {
    float c[4] = {}, vw[4] = {};
    const float pt[4] = {x, y, z, 1};
    for (int i = 0; i < 4; ++i) vw[i] = pt[0] * view[i] + pt[1] * view[4 + i] + pt[2] * view[8 + i] + pt[3] * view[12 + i];
    for (int i = 0; i < 4; ++i) c[i] = vw[0] * proj[i] + vw[1] * proj[4 + i] + vw[2] * proj[8 + i] + vw[3] * proj[12 + i];
    if (c[3] <= 0.01f) return false;
    out = ImVec2((c[0] / c[3] * 0.5f + 0.5f) * width, (1.f - (c[1] / c[3] * 0.5f + 0.5f)) * height);
    return true;
}
// Position (and yaw) of a selectable thing; null when it no longer exists.
float* RefPosition(const Ref& ref, float** yaw) {
    *yaw = nullptr;
    if (ref.kind == 5) { if (quest.job && quest.giverHasPoint) { *yaw = &quest.gyaw; return &quest.gx; } return nullptr; }
    if (ref.step < 0 || ref.step >= int(quest.steps.size())) return nullptr;
    Step& s = quest.steps[ref.step];
    if (ref.kind == 0 && (s.type == 0 || s.type == 1 || (s.type == 4 && !s.actor[0])) && s.hasPoint) return &s.x;
    if (ref.kind == 3 && s.type == 3 && s.hasPickup) return &s.px;
    if (ref.kind == 4 && s.type == 3 && s.hasDrop) return &s.dx;
    if (ref.kind == 1 && s.type == 2 && s.hasArea) return &s.ax;
    if (ref.kind == 2 && s.type == 2 && ref.index < int(s.spawn.size())) { *yaw = &s.spawn[ref.index].yaw; return &s.spawn[ref.index].x; }
    return nullptr;
}
std::vector<std::pair<Ref, std::string>> Selectables() {
    std::vector<std::pair<Ref, std::string>> out;
    if (quest.job && quest.giverHasPoint) out.push_back({{-1, 5, 0}, T("Выдаёт работу")});
    for (int i = 0; i < int(quest.steps.size()); ++i) {
        const Step& s = quest.steps[i];
        const std::string n = std::to_string(i + 1);
        if ((s.type == 0 || s.type == 1 || (s.type == 4 && !s.actor[0])) && s.hasPoint) out.push_back({{i, 0, 0}, T("Этап ") + n});
        if (s.type == 3 && s.hasPickup) out.push_back({{i, 3, 0}, T("Этап ") + n + T(": погрузка")});
        if (s.type == 3 && s.hasDrop) out.push_back({{i, 4, 0}, T("Этап ") + n + T(": разгрузка")});
        if (s.type == 2 && s.hasArea) out.push_back({{i, 1, 0}, T("Этап ") + n + T(": зона")});
        if (s.type == 2) for (int k = 0; k < int(s.spawn.size()); ++k) out.push_back({{i, 2, k}, T("Этап ") + n + ": " + ItemName(s.spawn[k].item)});
    }
    return out;
}
// Labels over the quest's points, click-to-select and the move/rotate gizmo of the selection.
// Matrix of a world transform (row-vector convention, Unreal rotation axes) and back.
void ToMatrix(const WT& t, float* m) {
    const float rad = 0.01745329252f, sp = sinf(t.pitch * rad), cp = cosf(t.pitch * rad), sy = sinf(t.yaw * rad), cy = cosf(t.yaw * rad), sr = sinf(t.roll * rad), cr = cosf(t.roll * rad);
    const float x[3] = {cp * cy, cp * sy, sp};
    const float y[3] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const float z[3] = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    const float s = t.scale;
    const float v[16] = {x[0] * s, x[1] * s, x[2] * s, 0, y[0] * s, y[1] * s, y[2] * s, 0, z[0] * s, z[1] * s, z[2] * s, 0, t.x, t.y, t.z, 1};
    std::memcpy(m, v, sizeof(v));
}
void FromMatrix(const float* m, WT& t) {
    const float s = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float x[3] = {m[0] / s, m[1] / s, m[2] / s};
    const float ys = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]), zs = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    t.x = m[12]; t.y = m[13]; t.z = m[14];
    t.pitch = atan2f(x[2], sqrtf(x[0] * x[0] + x[1] * x[1])) * 57.2957795f;
    t.yaw = atan2f(x[1], x[0]) * 57.2957795f;
    t.roll = atan2f(-m[6] / ys, m[10] / zs) * 57.2957795f;
    t.scale = s;
}
void DrawWorldTools() {
    if (worldMode.empty() || !cam.valid) return;
    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    float view[16], proj[16];
    Matrices(view, proj, w, h);
    bool busy = false;
    if (wsel.kind) {
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) worldGizmo = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) worldGizmo = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_R) && wsel.kind == 2) worldGizmo = 2;
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) { WorldDelete(); return; }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) WorldDuplicate();
        }
        if (worldGizmo == 2 && wsel.kind != 2) worldGizmo = 0;
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
        ImGuizmo::SetRect(0, 0, w, h);
        float m[16]; ToMatrix(wsel.t, m);
        const ImGuizmo::OPERATION op = worldGizmo == 0 ? ImGuizmo::TRANSLATE : worldGizmo == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALEU;
        const float snapMove[3] = {50, 50, 50}, snapTurn[3] = {15, 15, 15}, snapScale[3] = {0.1f, 0.1f, 0.1f};
        const float* snap = io.KeyCtrl ? (worldGizmo == 0 ? snapMove : worldGizmo == 1 ? snapTurn : snapScale) : nullptr;
        if (Gizmo(view, proj, op, m, snap)) {
            FromMatrix(m, wsel.t);
            if (wsel.kind == 1) world.move[wsel.id] = wsel.t;
            else if (wsel.kind == 3) { if (LItem* li = LogicOf(wsel.id)) li->t = wsel.t; }
            else if (wsel.kind == 4) { if (WActor* a = ActorById(wsel.id)) { wsel.t.pitch = wsel.t.roll = 0; wsel.t.scale = 1; a->t = wsel.t; } }
            else if (wsel.kind == 5) { if (WZone* z = ZoneOf(wsel.id)) { wsel.t.pitch = wsel.t.roll = 0; wsel.t.scale = 1; z->t = wsel.t; } }
            else if (wsel.kind == 6) { if (WCarry* c = CarryByKey(wsel.id)) { wsel.t.pitch = wsel.t.roll = 0; wsel.t.scale = 1; c->t = wsel.t; } }
            else if (wsel.kind == 7) { bool inside = false; if (WEnt* e = EntOf(wsel.id, &inside)) { wsel.t.pitch = wsel.t.roll = 0; wsel.t.scale = 1; (inside ? e->t2 : e->t) = wsel.t; } }
            else for (auto& a : world.add) if (a.uid == wsel.id) a.t = wsel.t;
            worldDirty = true;
        }
        busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
        ImVec2 sp;
        if (Project(view, proj, w, h, wsel.t.x, wsel.t.y, wsel.t.z, sp)) ImGui::GetBackgroundDrawList()->AddText(ImVec2(sp.x + 12, sp.y + 12), IM_COL32(255, 200, 80, 255), wsel.kind == 2 ? T("проп") : wsel.kind == 5 && ZoneOf(wsel.id) ? ZoneOf(wsel.id)->name : wsel.id.c_str());
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 12), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##wgizmo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        if (ImGui::RadioButton(T("Двигать (W)"), worldGizmo == 0)) worldGizmo = 0;
        ImGui::SameLine(); if (ImGui::RadioButton(T("Вращать (E)"), worldGizmo == 1)) worldGizmo = 1;
        if (wsel.kind == 2) { ImGui::SameLine(); if (ImGui::RadioButton(T("Масштаб (R)"), worldGizmo == 2)) worldGizmo = 2; }
        ImGui::SameLine(); ImGui::TextDisabled(T("Ctrl — шаг   Del — убрать"));
        ImGui::End();
    }
    // Logic objects: rings for triggers, arrows for points, lines along lists; a click on a label selects.
    int hoverIndex = -1; std::string hoverName; float hoverBest = 20.f * 20.f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (auto& [name, items] : world.logic) {
        const LDecl* d = FindDecl(name);
        const bool trigger = !d || d->trigger;
        ImVec2 prev; bool hasPrev = false;
        for (int i = 0; i < int(items.size()); ++i) {
            const LItem& it = items[i];
            const bool isSel = wsel.kind == 3 && wsel.id == name + "#" + std::to_string(i);
            const ImU32 col = isSel ? IM_COL32(255, 200, 80, 255) : trigger ? IM_COL32(90, 210, 230, 220) : IM_COL32(240, 150, 70, 220);
            ImVec2 c;
            if (!Project(view, proj, w, h, it.t.x, it.t.y, it.t.z + 200, c)) { hasPrev = false; continue; }  // the middle of its beam
            (void)col; (void)trigger; (void)prev; (void)hasPrev; (void)dl;  // drawn in the world (UpdateLogicVisuals)
            const float dx = c.x - io.MousePos.x, dy = c.y - io.MousePos.y;
            if (dx * dx + dy * dy < hoverBest) { hoverBest = dx * dx + dy * dy; hoverIndex = i; hoverName = name; }
        }
    }
    int hoverZone = -1;  // zones: their centre beam
    for (int i = 0; i < int(world.zones.size()); ++i) {
        ImVec2 c;
        const WZone& z = world.zones[size_t(i)];
        if (!Project(view, proj, w, h, z.t.x, z.t.y, z.t.z + 200, c)) continue;
        const float dx = c.x - io.MousePos.x, dy = c.y - io.MousePos.y;
        if (dx * dx + dy * dy < hoverBest) { hoverBest = dx * dx + dy * dy; hoverZone = i; hoverIndex = -1; }
    }
    int hoverActor = -1;  // NPCs: a click on the body (its chest) selects it
    for (int i = 0; i < int(world.actors.size()); ++i) {
        ImVec2 c;
        const WActor& a = world.actors[size_t(i)];
        if (!Project(view, proj, w, h, a.t.x, a.t.y, a.t.z + 110, c)) continue;
        const float dx = c.x - io.MousePos.x, dy = c.y - io.MousePos.y;
        if (dx * dx + dy * dy < std::max(hoverBest, 30.f * 30.f)) { hoverBest = dx * dx + dy * dy; hoverActor = i; hoverIndex = -1; hoverZone = -1; }
    }
    int hoverCarry = -1;  // boxes: a click on the box selects it
    for (int i = 0; i < int(world.carries.size()); ++i) {
        ImVec2 c;
        const WCarry& b = world.carries[size_t(i)];
        if (!Project(view, proj, w, h, b.t.x, b.t.y, b.t.z + 25, c)) continue;
        const float dx = c.x - io.MousePos.x, dy = c.y - io.MousePos.y;
        if (dx * dx + dy * dy < std::max(hoverBest, 25.f * 25.f)) { hoverBest = dx * dx + dy * dy; hoverCarry = i; hoverActor = -1; hoverIndex = -1; hoverZone = -1; }
    }
    std::string hoverEnt;  // vehicles, doors, loot: their beam
    for (auto& e : world.ents) for (int end = 0; end < (e.type == 1 ? 2 : 1); ++end) {
        const WT& t = end ? e.t2 : e.t;
        ImVec2 c;
        if (!Project(view, proj, w, h, t.x, t.y, t.z + 200, c)) continue;
        const float dx = c.x - io.MousePos.x, dy = c.y - io.MousePos.y;
        if (dx * dx + dy * dy < hoverBest) { hoverBest = dx * dx + dy * dy; hoverEnt = EntId(e, end == 1); hoverCarry = hoverActor = hoverIndex = hoverZone = -1; }
    }
    if (overlay::CursorMode() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !busy) {
        const bool placing = !placeLogic.empty() || !placeProp.empty() || !placeActor.empty() || placeZone || placeCarry || placeEnt || !doorPending.empty();
        if (!hoverEnt.empty() && !placing) { bool inside = false; if (WEnt* e = EntOf(hoverEnt, &inside)) SelectEnt(*e, inside); }
        else if (hoverCarry >= 0 && !placing) SelectCarry(world.carries[size_t(hoverCarry)].key);
        else if (hoverActor >= 0 && !placing) SelectActor(world.actors[size_t(hoverActor)].id);
        else if (hoverZone >= 0 && !placing) SelectZone(hoverZone);
        else if (hoverIndex >= 0 && !placing) SelectLogic(hoverName, hoverIndex);
        else actions.push_back({Action::WorldClick, ""});
    }
    if (!io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) WorldRestore(worldRedo, worldUndo, T("Возвращено")); else WorldRestore(worldUndo, worldRedo, T("Отменено")); }
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) WorldRestore(worldRedo, worldUndo, T("Возвращено"));
    }
    TrackWorldHistory();
}

void DrawSceneTools() {
    if (mode == 0) { DrawWorldTools(); return; }
    if (mode != 1 || !questOpen || !cam.valid) return;
    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    float view[16], proj[16];
    Matrices(view, proj, w, h);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const auto items = Selectables();
    int hover = -1; float best = 22.f * 22.f;
    for (int i = 0; i < int(items.size()); ++i) {
        float* yaw = nullptr; float* p = RefPosition(items[i].first, &yaw); if (!p) continue;
        ImVec2 sp;
        if (!Project(view, proj, w, h, p[0], p[1], p[2], sp)) continue;
        const bool isPicked = picked.step == items[i].first.step && picked.kind == items[i].first.kind && picked.index == items[i].first.index;
        const bool isStep = items[i].first.step == selected;
        const ImU32 color = isPicked ? IM_COL32(255, 190, 60, 255) : isStep ? IM_COL32(255, 220, 120, 220) : IM_COL32(200, 200, 200, 180);
        dl->AddCircle(sp, isPicked ? 9.f : 6.f, color, 16, 2.f);
        dl->AddText(ImVec2(sp.x + 10, sp.y - 9), color, items[i].second.c_str());
        const float dx = sp.x - io.MousePos.x, dy = sp.y - io.MousePos.y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; hover = i; }
    }
    float* yaw = nullptr;
    float* pos = RefPosition(picked, &yaw);
    bool gizmoBusy = false;
    if (pos) {
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_E) && yaw) gizmoOp = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && picked.kind == 2) {
                auto& sp = quest.steps[picked.step].spawn; sp.erase(sp.begin() + picked.index); picked = {}; previewDirty = true; return;
            }
        }
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(dl);
        ImGuizmo::SetRect(0, 0, w, h);
        const float a = (yaw ? *yaw : 0.f) * 0.01745329252f;
        float m[16] = {cosf(a), sinf(a), 0, 0, -sinf(a), cosf(a), 0, 0, 0, 0, 1, 0, pos[0], pos[1], pos[2], 1};
        const bool rotate = gizmoOp == 1 && yaw;
        const float snapMove[3] = {50, 50, 50}, snapTurn[3] = {15, 15, 15};
        const bool snap = io.KeyCtrl;
        if (Gizmo(view, proj, rotate ? ImGuizmo::ROTATE_Z : ImGuizmo::TRANSLATE, m, snap ? (rotate ? snapTurn : snapMove) : nullptr)) {
            pos[0] = m[12]; pos[1] = m[13]; pos[2] = m[14];
            if (yaw) *yaw = atan2f(m[1], m[0]) * 57.2957795f;
            previewDirty = true;
        }
        gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 12), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##gizmo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        if (ImGui::RadioButton(T("Двигать (W)"), gizmoOp == 0)) gizmoOp = 0;
        if (yaw) { ImGui::SameLine(); if (ImGui::RadioButton(T("Вращать (E)"), gizmoOp == 1)) gizmoOp = 1; }
        ImGui::SameLine(); ImGui::TextDisabled(T("Ctrl — шаг   Del — удалить предмет"));
        ImGui::End();
    }
    // Click selection: in the world (not on a window or the gizmo) and only when nothing is being placed.
    if (clickMode == kSelect && overlay::CursorMode() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !gizmoBusy) {
        if (hover >= 0) { picked = items[hover].first; if (picked.step >= 0) selected = picked.step; previewDirty = true; }
        else picked = {};
    }
}

// ---------------------------------------------------------------- mode skins (modes/<mode>/skins.json)
// What players of a mode may wear: "skins" they choose from when they start (player:customize()) and
// "uniforms" the mode puts over them (player:setUniform("id")). Each is built in the character
// creator on a mannequin in front of the camera, from a preset of the game or its parts.
struct SkinEntry { char id[48] = ""; LT name; std::string look; };
std::vector<SkinEntry> skinLists[2];
const char* kSkinKinds[] = {"skins", "uniforms"};
int skinKind = 0, skinSel = -1;
bool skinsChanged = false;  // not saved yet
struct Wardrobe { bool want = false, open = false; int kind = 0, index = -1; std::string start, title; UObject* body = nullptr; } wardrobe;
const char* kMannequinIdle = "/Game/Animation/Deacon/ambient/dcn_idle_normal.dcn_idle_normal";
const char* kMannequinLook = "/Game/Characters/Deacon/looks/deacon_standard.deacon_standard";

fs::path SkinsFile(const std::string& m) { return fs::path(profile::Root()) / L"modes" / m / L"skins.json"; }
void LoadSkinLists(const std::string& m) {
    for (auto& l : skinLists) l.clear();
    skinSel = -1; skinsChanged = false;
    std::ifstream in(SkinsFile(m), std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject()) return;
    for (int k = 0; k < 2; ++k) {
        const json::Value& list = v[kSkinKinds[k]];
        for (size_t i = 0; i < list.size(); ++i) {
            SkinEntry e;
            Copy(e.id, sizeof e.id, list[i]["id"].asString());
            LFrom(list[i]["name"], e.name);
            e.look = list[i]["look"].asString();
            skinLists[k].push_back(std::move(e));
        }
    }
}
// "" when the lists can be saved, else what is wrong.
std::string SkinsProblem() {
    for (int k = 0; k < 2; ++k) {
        const char* kind = k ? T("Формы") : T("Скины на старте");
        for (size_t i = 0; i < skinLists[k].size(); ++i) {
            const SkinEntry& e = skinLists[k][i];
            if (!ValidId(e.id)) return std::string(kind) + " #" + std::to_string(i + 1) + T(": id — латиница, цифры, _ и -");
            if (e.look.empty()) return std::string(kind) + " " + e.id + T(": облик не собран");
            for (size_t j = 0; j < i; ++j) if (std::strcmp(skinLists[k][j].id, e.id) == 0) return std::string(kind) + " " + e.id + T(": такой id уже есть");
        }
    }
    return {};
}
bool SaveSkinLists() {
    const std::string problem = SkinsProblem();
    if (!problem.empty()) { Toast(problem); return false; }
    json::Value v = json::Value::Object();
    for (int k = 0; k < 2; ++k) {
        json::Value list = json::Value::Array();
        for (auto& e : skinLists[k]) {
            json::Value j = json::Value::Object();
            j.set("id", std::string(e.id));
            if (e.name.en[0] || e.name.ru[0]) j.set("name", LJson(e.name));
            j.set("look", e.look);
            list.push(j);
        }
        v.set(kSkinKinds[k], list);
    }
    if (!WriteFile(SkinsFile(worldMode), v.dump())) { Toast(T("Не удалось сохранить")); return false; }
    skinsChanged = false;
    Toast(T("Сохранено: modes/") + worldMode + "/skins.json");
    return true;
}
void OpenWardrobe(int kind, int index) {
    const SkinEntry& e = skinLists[kind][size_t(index)];
    wardrobe = {};
    wardrobe.want = true; wardrobe.kind = kind; wardrobe.index = index; wardrobe.start = e.look;
    wardrobe.title = std::string(T("Облик: ")) + (Shown(e.name)[0] ? Shown(e.name) : e.id);
}
bool Dressing() { return wardrobe.want || wardrobe.open; }
std::string ModeSkinLook(const std::string& id) {
    for (auto& list : skinLists) for (auto& e : list) if (id == e.id) return e.look;
    return {};
}
std::vector<std::pair<std::string, std::string>> ModeSkinChoices() {
    std::vector<std::pair<std::string, std::string>> out;
    for (int k = 0; k < 2; ++k) for (auto& e : skinLists[k]) out.emplace_back(e.id, std::string(k ? "[uniform] " : "[skin] ") + e.id + "  " + Shown(e.name));
    return out;
}

void DrawSkins() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 520, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin(T("Персонажи"));
    ImGui::BeginDisabled(Dressing());
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(T("Режим"), worldMode.empty() ? T("— выберите —") : worldMode.c_str())) {
        for (auto& m : modes) if (ImGui::Selectable(m.c_str(), m == worldMode)) actions.push_back({Action::WorldLoad, m});
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (worldMode.empty()) { ImGui::TextWrapped(T("Выберите режим: у каждого режима свои скины (modes/<режим>/skins.json).")); ImGui::End(); return; }
    ImGui::SameLine(); ImGui::BeginDisabled(Dressing()); if (ImGui::Button(T("Сохранить"))) { actions.push_back({Action::SkinsSave, ""}); actions.push_back({Action::WorldSave, ""}); } ImGui::EndDisabled();
    if (skinsChanged) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", T("не сохранено")); }
    ImGui::TextWrapped("%s", T("Игроки выбирают только из этих скинов (player:customize()); формы режим надевает поверх скина (player:setUniform(\"id\")). Облик собирается на манекене перед камерой: пресет игры или своя сборка из частей."));
    {
        auto it = world.logic.find("wardrobe");
        const bool placed = it != world.logic.end() && !it->second.empty();
        ImGui::SeparatorText(T("Место выбора скина"));
        if (placed) ImGui::Text(T("Поставлено: %.0f %.0f %.0f"), it->second[0].t.x, it->second[0].t.y, it->second[0].t.z);
        else ImGui::TextDisabled("%s", T("Не задано: игрок выбирает там, где стоит"));
        ImGui::BeginDisabled(Dressing());
        if (ImGui::Button(placed ? T("Переставить") : T("Поставить в мире"))) {
            mode = 0; placeLogic = "wardrobe"; placeProp.clear();
            Toast(T("ЛКМ в мире ставит место выбора; стрелка — куда смотрит персонаж"));
        }
        if (placed) {
            ImGui::SameLine(); if (ImGui::Button(T("Показать"))) { mode = 0; SelectLogic("wardrobe", 0); }
            ImGui::SameLine(); if (ImGui::Button(T("Убрать##place"))) world.logic.erase("wardrobe");
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Например комната под землёй из пропсов со светом. player:customize() переносит игрока туда, после выбора возвращает назад (на спавн при первом входе). Хранится в world.json — «Сохранить» здесь пишет и его."));
    }
    if (Dressing()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", T("Идёт сборка облика: окно слева. «Готово» запишет облик в запись, «Отмена» оставит как было."));
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##kinds")) {
        for (int k = 0; k < 2; ++k) {
            const std::string label = std::string(k ? T("Формы") : T("Скины на старте")) + " (" + std::to_string(skinLists[k].size()) + ")###kind" + std::to_string(k);
            if (ImGui::BeginTabItem(label.c_str())) { if (skinKind != k) { skinKind = k; skinSel = -1; } ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    auto& list = skinLists[skinKind];
    if (skinSel >= int(list.size())) skinSel = -1;
    if (ImGui::BeginChild("##skinlist", ImVec2(0, 180), ImGuiChildFlags_Borders)) {
        if (list.empty()) ImGui::TextDisabled("%s", skinKind ? T("Пусто: «Добавить» соберёт форму") : T("Пусто: игроки получат полный редактор персонажа. «Добавить» соберёт скин"));
        for (int i = 0; i < int(list.size()); ++i) {
            const SkinEntry& e = list[size_t(i)];
            const std::string label = std::string(Shown(e.name)[0] ? Shown(e.name) : e.id) + "   (" + e.id + ")" + (e.look.empty() ? T("  — без облика") : "") + "##" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), i == skinSel)) skinSel = i;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { skinSel = i; OpenWardrobe(skinKind, i); }
        }
    }
    ImGui::EndChild();
    if (ImGui::Button(T("Добавить"))) {
        SkinEntry e;
        const char* base = skinKind ? "uniform" : "skin";
        for (int n = int(list.size()) + 1;; ++n) {
            const std::string id = base + std::to_string(n);
            bool used = false; for (auto& o : list) used |= id == o.id;
            if (!used) { Copy(e.id, sizeof e.id, id); break; }
        }
        Copy(e.name.en, sizeof e.name.en, skinKind ? "New uniform" : "New skin");
        Copy(e.name.ru, sizeof e.name.ru, skinKind ? "Новая форма" : "Новый скин");
        if (skinSel >= 0) e.look = list[size_t(skinSel)].look;  // from the selected one
        list.push_back(e);
        skinSel = int(list.size()) - 1; skinsChanged = true;
        OpenWardrobe(skinKind, skinSel);
    }
    const bool has = skinSel >= 0;
    ImGui::BeginDisabled(!has);
    ImGui::SameLine();
    if (ImGui::Button(T("Дублировать")) && has) {
        SkinEntry e = list[size_t(skinSel)];
        const std::string id = std::string(e.id) + "_2"; Copy(e.id, sizeof e.id, id);
        list.insert(list.begin() + skinSel + 1, e); ++skinSel; skinsChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Удалить")) && has) { list.erase(list.begin() + skinSel); skinSel = -1; skinsChanged = true; }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##up", ImGuiDir_Up) && has && skinSel > 0) { std::swap(list[size_t(skinSel)], list[size_t(skinSel - 1)]); --skinSel; skinsChanged = true; }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down) && has && skinSel + 1 < int(list.size())) { std::swap(list[size_t(skinSel)], list[size_t(skinSel + 1)]); ++skinSel; skinsChanged = true; }
    ImGui::EndDisabled();
    if (has) {
        SkinEntry& e = list[size_t(skinSel)];
        ImGui::SeparatorText(T("Запись"));
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("id", e.id, sizeof e.id)) skinsChanged = true;
        if (!ValidId(e.id)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("латиница, цифры, _ и -")); }
        if (ImGui::InputText(T("Название (EN)"), e.name.en, sizeof e.name.en)) skinsChanged = true;
        if (ImGui::InputText(T("Название (RU)"), e.name.ru, sizeof e.name.ru)) skinsChanged = true;
        if (ImGui::Button(e.look.empty() ? T("Собрать облик") : T("Изменить облик"), ImVec2(180, 0))) OpenWardrobe(skinKind, skinSel);
        ImGui::SameLine(); ImGui::TextDisabled("%s", T("(двойной клик по записи)"));
        if (!e.look.empty()) ImGui::TextWrapped("%s", e.look.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled(skinKind ? "Lua: player:setUniform(\"%s\")" : "Lua: player:setSkin(\"%s\")", e.id);
    }
    ImGui::End();
}

// Game thread: the mannequin the creator dresses, standing in front of the editor camera.
UObject* SpawnMannequin(UObject* pawn, const std::string& look, const LItem* spot) {
    static UObject* statics = nullptr; static UObject* kismet = nullptr;
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    Cam view; { std::lock_guard g(lock); view = cam; }
    if (!statics || !view.valid) return nullptr;
    const float rad = 0.0174532925f, cy = std::cos(view.rot.Yaw * rad), sy = std::sin(view.rot.Yaw * rad);
    FVector at{view.loc.X + cy * 350.f, view.loc.Y + sy * 350.f, view.loc.Z - 150.f};
    float yaw = view.rot.Yaw + 180.f;  // facing the camera
    if (spot) {
        // The mode's choosing place: there, facing its way. The hidden player goes along to stream it in
        // (the editor camera follows it there afterwards).
        at = {spot->t.x, spot->t.y, spot->t.z}; yaw = spot->t.yaw;
        if (auto f = Fn(pawn, "K2_SetActorLocation")) { Params p(f); p.Set("NewLocation", FVector{at.X, at.Y, at.Z + 100.f}); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(pawn, "K2_SetActorLocation", &p); }
    } else if (auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr; trace) if (auto brk = Fn(statics, "BreakHitResult")) {
        Params t(trace); t.Set("WorldContextObject", pawn); t.Set("Start", FVector{at.X, at.Y, view.loc.Z + 50.f}); t.Set("End", FVector{at.X, at.Y, view.loc.Z - 3000.f});
        t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
        Call(kismet, "LineTraceSingle_NEW", &t);
        if (t.GetBool("ReturnValue")) {
            Params b(brk);
            auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
            std::memcpy(static_cast<uint8_t*>(b.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
            Call(statics, "BreakHitResult", &b);
            at.Z = b.Get<FVector>("ImpactPoint").Z;
        }
    }
    FTransform t; t.Translation = at;
    Params begin(Fn(statics, "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", pawn); begin.Set("ActorClass", FindClass("SkeletalMeshActor")); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(statics, "BeginSpawningActorFromClass", &begin);
    UObject* body = begin.Get<UObject*>("ReturnValue");
    if (!body) return nullptr;
    Params finish(Fn(statics, "FinishSpawningActor")); finish.Set("Actor", body); finish.Set("SpawnTransform", t);
    Call(statics, "FinishSpawningActor", &finish);
    if (auto f = Fn(body, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(body, "SetActorEnableCollision", &p); }
    if (auto f = Fn(body, "K2_SetActorRotation")) { Params p(f); p.Set("NewRotation", FRotator{0, yaw, 0}); Call(body, "K2_SetActorRotation", &p); }
    UObject* mesh = Field<UObject*>(body, off::SkelMeshActor_Component);
    SetBoolProp(mesh, "bDisableClothSimulation", true);
    if (!IsValid(mesh) || !skins::Wear(mesh, look.empty() ? kMannequinLook : look)) { Call(body, "K2_DestroyActor"); return nullptr; }
    return body;
}
// The mannequin stands idle (a new look may stop its animation).
void KeepIdle(UObject* mesh) {
    static UObject* idle = nullptr;
    if (!IsValid(idle)) { idle = FindObject(kMannequinIdle); if (!idle) idle = LoadAsset(kMannequinIdle); }
    if (!idle) return;
    if (auto f = Fn(mesh, "IsPlaying")) { Params p(f); Call(mesh, "IsPlaying", &p); if (p.GetBool("ReturnValue")) return; }
    if (auto f = Fn(mesh, "SetAnimationMode")) { Params p(f); p.Set<uint8_t>("InAnimationMode", 1); Call(mesh, "SetAnimationMode", &p); }
    if (auto f = Fn(mesh, "PlayAnimation")) { Params p(f); p.Set("NewAnimToPlay", idle); p.SetBool("bLooping", true); Call(mesh, "PlayAnimation", &p); }
}
void UpdateWardrobe(UObject* pawn, UObject* controller) {
    Wardrobe w; LItem spot; bool hasSpot = false;
    {
        std::lock_guard g(lock); w = wardrobe;
        if (auto it = world.logic.find("wardrobe"); it != world.logic.end() && !it->second.empty()) { spot = it->second[0]; hasSpot = true; }
    }
    if (w.want && !w.open) {
        UObject* body = SpawnMannequin(pawn, w.start, hasSpot ? &spot : nullptr);
        std::lock_guard g(lock);
        wardrobe.want = false;
        if (!body) { Toast(T("Не удалось поставить манекен")); return; }
        wardrobe.open = true; wardrobe.body = body;
        skins::Options o;
        o.title = w.title; o.current = w.start; o.embedded = true; o.rootHeight = -90.f;
        skins::OpenCreator(o);
        return;
    }
    if (!w.open) return;
    UObject* mesh = IsValid(w.body) ? Field<UObject*>(w.body, off::SkelMeshActor_Component) : nullptr;
    if (!IsValid(mesh)) { w.body = nullptr; mesh = nullptr; skins::CloseCreator(); }
    bool ok = false; std::string look;
    if (!skins::UpdateCreator(w.body, controller, mesh, ok, look)) { if (mesh) KeepIdle(mesh); return; }
    if (IsValid(w.body)) Call(w.body, "K2_DestroyActor");
    std::lock_guard g(lock);
    auto& list = skinLists[w.kind];
    if (ok && !look.empty() && w.index >= 0 && w.index < int(list.size())) { list[size_t(w.index)].look = look; skinsChanged = true; }
    wardrobe = {};
}

void Draw() {
    std::lock_guard g(lock);
    ImGuizmo::BeginFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 20, vp->WorkPos.y + 120), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 90), ImGuiCond_FirstUseEver);
    ImGui::Begin(T("Редактор DGMP"), nullptr, ImGuiWindowFlags_NoCollapse);
    const int wasMode = mode;
    ImGui::RadioButton(T("Мир"), &mode, 0); ImGui::SameLine(); if (ImGui::RadioButton(T("Квест"), &mode, 1)) previewDirty = true;
    ImGui::SameLine(); ImGui::RadioButton(T("Персонажи"), &mode, 2);
    if (mode != wasMode && wasMode == 2 && Dressing()) skins::CloseCreator();
    ImGui::SameLine(0, 24);
    ImGui::TextDisabled("%s", overlay::CursorMode() ? T("Tab — полёт") : T("Tab — курсор"));
    ImGui::SameLine(ImGui::GetWindowWidth() - 150);
    bool inv = invertLook;
    if (ImGui::Checkbox("Invert Y", &inv)) {
        invertLook = inv;
        WritePrivateProfileStringW(L"Editor", L"InvertY", inv ? L"1" : L"0", (fs::path(profile::Root()) / L"editor.ini").c_str());
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 50);
    if (ImGui::SmallButton(english ? "RU" : "EN")) {
        english = !english;
        WritePrivateProfileStringW(L"Editor", L"Language", english ? L"en" : L"ru", (fs::path(profile::Root()) / L"editor.ini").c_str());
    }
    ImGui::End();
    if (mode == 2) {
        DrawSkins();
        if (Dressing()) skins::DrawCreator();
    } else {
        DrawLibrary();
        if (mode == 1) DrawQuest();
        else DrawWorld();
        DrawSceneTools();
    }
    if (mode == 1) { HistoryKeys(); TrackHistory(); }
    // Status, bottom left: one line "DGMP v…" until clicked.
    {
        const std::string title = status.substr(0, status.find(" |"));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 10, vp->WorkPos.y + vp->WorkSize.y - 10), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Selectable(((statusOpen ? "▾ " : "▸ ") + (title.empty() ? std::string("DGMP") : title)).c_str(), false, 0, ImVec2(0, 0))) statusOpen = !statusOpen;
        if (statusOpen) ImGui::TextUnformatted(status.c_str());
        ImGui::End();
    }
    if (!toast.empty() && Now() < toastUntil) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 120), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(toast.c_str());
        ImGui::End();
    }
}
}  // namespace

void SetStatus(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), char(0)); WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n, nullptr, nullptr);
    std::lock_guard g(lock); status = std::move(out);
}

bool InvertLook() { return invertLook; }
void Start() {
    if (!overlay::Install()) return;
    wchar_t lang[8] = L"en";
    GetPrivateProfileStringW(L"Editor", L"Language", L"en", lang, 8, (fs::path(profile::Root()) / L"editor.ini").c_str());
    english = wcscmp(lang, L"ru") != 0;
    invertLook = GetPrivateProfileIntW(L"Editor", L"InvertY", 1, (fs::path(profile::Root()) / L"editor.ini").c_str()) != 0;
    overlay::SetDraw(&Draw);
    overlay::SetVisible(true);
    overlay::SetKeepFocus(true);
}

void Update(double now, UObject* pawn, UObject* controller) {
    if (!overlay::Visible() || !IsValid(pawn) || !IsValid(controller)) return;
    if (UObject* mgr = Field<UObject*>(controller, off::PC_CameraManager); IsValid(mgr)) {
        std::lock_guard g(lock);
        cam.loc = Field<FVector>(mgr, off::CamMgr_POV_Location);
        cam.rot = Field<FRotator>(mgr, off::CamMgr_POV_Location + 0xC);  // FMinimalViewInfo: Location, Rotation, FOV
        cam.fov = Field<float>(mgr, off::CamMgr_POV_Location + 0x18);
        cam.valid = cam.fov > 5 && cam.fov < 170;
    }
    if (!catalogReady) { static double nextTry = 0; if (now >= nextTry) { nextTry = now + 3; BuildCatalog(); } }
    static double nextList = 0;
    if (now >= nextList) { nextList = now + 2; std::lock_guard g(lock); RefreshQuestList(); }
    {
        std::lock_guard g(lock);
        auto pending = std::move(actions); actions.clear();
        for (auto& [action, arg] : pending) {
            switch (action) {
            case Action::New:
                quest = Quest{}; Copy(quest.id, sizeof(quest.id), "quest_" + std::to_string(GetTickCount64() % 100000));
                Copy(quest.title.en, sizeof(quest.title.en), "New quest"); Copy(quest.title.ru, sizeof(quest.title.ru), "Новый квест");
                loadedId.clear(); questOpen = true; selected = -1; previewDirty = true; ResetHistory(); break;
            case Action::Load: LoadQuest(arg); break;
            case Action::Save: SaveQuest(); break;
            case Action::Delete: { std::error_code ec; fs::remove(QuestDir() / (arg + ".json"), ec); questOpen = false; loadedId.clear(); RefreshQuestList(); previewDirty = true; Toast(T("Квест удалён")); break; }
            case Action::WorldLoad: LoadWorld(arg); break;
            case Action::SkinsSave: SaveSkinLists(); break;
            case Action::WorldSave: {
                std::error_code ec; fs::create_directories(WorldFile(worldMode).parent_path(), ec);
                if (WriteFile(WorldFile(worldMode), WorldJson(world).dump())) Toast(T("Сохранено: modes/") + worldMode + "/world.json"); else Toast(T("Не удалось сохранить"));
                break;
            }
            case Action::WorldClick: worldClickPending = true; break;  // handled below, outside the lock
            case Action::Test:
                // The launcher that started the editor runs the test (local server + game) when asked.
                if (SaveQuest()) {
                    json::Value req = json::Value::Object(); req.set("cmd", "questTest"); req.set("id", loadedId);
                    WriteFile(fs::path(profile::Root()) / L"editor_request.json", req.dump());
                    Toast(T("Запуск проверки: редактор закроется"));
                }
                break;
            }
        }
        if (previewDirty && now >= previewAt) { previewDirty = false; previewAt = now + 0.3; WritePreview(); }
    }
    // World edits: the editor's preview (hidden, not destroyed, so removals can be undone).
    bool worldClick = false;
    {
        std::lock_guard g(lock);
        static bool listed = false;
        if (!listed) { listed = true; ListModes(); LoadProps(); }
        if (worldDirty) { worldDirty = false; worldedit::Set(WorldJson(world), true); }
        worldClick = worldClickPending; worldClickPending = false;
    }
    if (worldClick) WorldClick(pawn, controller);
    worldedit::Update(pawn);
    UpdateLogicVisuals(pawn);
    UpdateActorPreviews(pawn);
    UpdateCarryPreviews(pawn);
    UpdateSelectionOutline(controller);
    UpdateWardrobe(pawn, controller);
    // Left click into the world (not on a window) while the cursor belongs to the editor.
    static bool wasDown = false;
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    bool placing; { std::lock_guard g(lock); placing = clickMode != kSelect; }
    if (down && !wasDown && placing && overlay::CursorMode() && !overlay::MouseOverUi()) {
        const Pick pick = PickUnderMouse(pawn, controller);
        if (pick.valid) ApplyPick(pick);
    }
    wasDown = down;
}
}  // namespace editorui
