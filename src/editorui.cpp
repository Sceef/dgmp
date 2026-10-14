#include "editorui.h"
#include "overlay.h"
#include "inventory.h"
#include "profile.h"
#include "log.h"
#include "shared/json.h"
#include "worldedit.h"
#include "imgui.h"
#include "ImGuizmo.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
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
    {"Выберите режим: правки мира хранятся у каждого режима свои (modes/<режим>/world.json).", "Pick a mode: every mode keeps its own world edits (modes/<mode>/world.json)."}
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
const char* kStepTypes[] = {"Дойти до точки", "Удерживать зону", "Сделать N раз"};
const char* kStepKeys[] = {"goto", "hold", "collect"};
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
enum ClickMode { kSelect = 0, kPoint = 1, kObject = 2, kItem = 3 };
int clickMode = kSelect;       // what a left click into the world does
bool repeatPlace = false;      // stay in the placing mode after a placement
// Selection: one editable thing of the quest (a step's point, a collect zone, a quest item).
struct Ref { int step = -1, kind = 0, index = 0; };  // kind 0 point, 1 zone, 2 spawned item
Ref picked;
int gizmoOp = 0;               // 0 move, 1 rotate
// Camera of the last game frame (game thread → frame thread), for projections and the gizmo.
struct Cam { bool valid = false; FVector loc{}; FRotator rot{}; float fov = 90; };
Cam cam;
std::string toast; double toastUntil = 0;
int mode = 1;                  // 0 world, 1 quest
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
enum class Action { Save, New, Load, Delete, Test, WorldClick, WorldLoad, WorldSave };

// ---------------------------------------------------------------- world model (modes/<mode>/world.json)
struct WT { float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0, scale = 1; };
struct WAdd { std::string uid, asset, name; WT t; };
struct World {
    std::vector<std::string> remove;          // world objects ("<level>/<actor>")
    std::map<std::string, WT> move;           // world objects moved/turned
    std::vector<WAdd> add;                    // props placed by the mode
};
World world;
std::string worldMode;                        // mode whose world is edited
std::vector<std::string> modes;               // modes/* folders
bool worldDirty = false;                      // the game should apply the edits again
bool worldClickPending = false;
// Selection in the world: kind 1 world object (id = key, t = its transform), 2 added prop (id = uid).
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
    if (!q.reward.empty()) {
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
    for (auto& j : v["steps"].elements()) {
        Step s;
        s.type = IndexOf(kStepKeys, 3, j["type"].asString(), 0);
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
    for (auto& e : w.add) { json::Value j = json::Value::Object(); j.set("uid", e.uid); j.set("asset", e.asset); if (!e.name.empty()) j.set("name", e.name); a.push(TJson(e.t, j)); }
    v.set("add", a);
    return v;
}
void WorldFrom(const json::Value& v, World& w) {
    w = World{};
    for (auto& k : v["remove"].elements()) w.remove.push_back(k.asString());
    for (auto& [k, t] : v["move"].items()) w.move[k] = TFrom(t);
    for (auto& e : v["add"].elements()) { WAdd a; a.uid = e["uid"].asString(); a.asset = e["asset"].asString(); a.name = e["name"].asString(); a.t = TFrom(e); w.add.push_back(a); }
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
// Outline of the selection: the game's own survival-vision outline. Selected meshes render custom
// depth with stencil 2, and the controller's FocusPostHighlightOnly post process draws it.
std::vector<UObject*> outlined;  // primitive components currently outlined
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
    if (!placeProp.empty()) {
        WAdd a; a.uid = "p" + std::to_string(GetTickCount64() % 100000000);
        a.asset = placeProp; a.t.x = pick.at.X; a.t.y = pick.at.Y; a.t.z = pick.at.Z; a.t.yaw = cam.rot.Yaw;
        for (auto& pr : props) if (pr.path == placeProp) { a.name = pr.name; break; }
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
    if (!questOpen || selected < 0 || selected >= int(quest.steps.size())) { Toast(T("Выберите этап квеста")); return; }
    Step& s = quest.steps[selected];
    const std::string where = T("этап ") + std::to_string(selected + 1);
    if (clickMode == kSelect) return;
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
void WorldDelete() {
    if (wsel.kind == 1) {
        if (std::find(world.remove.begin(), world.remove.end(), wsel.id) == world.remove.end()) world.remove.push_back(wsel.id);
        world.move.erase(wsel.id);
        Toast(T("Убрано: ") + wsel.id);
    } else if (wsel.kind == 2) {
        world.add.erase(std::remove_if(world.add.begin(), world.add.end(), [](const WAdd& a) { return a.uid == wsel.id; }), world.add.end());
        Toast(T("Удалён проп"));
    } else return;
    wsel = {}; worldDirty = true;
}
void WorldDuplicate() {
    if (wsel.kind != 2) { Toast(T("Дублируются поставленные пропсы")); return; }
    for (auto& a : world.add) if (a.uid == wsel.id) {
        WAdd copy = a; copy.uid = "p" + std::to_string(GetTickCount64() % 100000000); copy.t.x += 100; copy.t.y += 100;
        world.add.push_back(copy); wsel = {2, copy.uid, "", copy.t}; worldDirty = true; Toast(T("Копия")); return;
    }
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
    ImGui::SeparatorText(T("Выделено"));
    if (wsel.kind == 0) ImGui::TextDisabled(placeProp.empty() ? T("Кликните по объекту в мире") : T("Кликните в мире, чтобы поставить проп"));
    else {
        ImGui::TextWrapped("%s", wsel.kind == 1 ? wsel.id.c_str() : (T("Проп ") + wsel.id).c_str());
        if (!wsel.cls.empty()) ImGui::TextDisabled("%s", wsel.cls.c_str());
        WT& t = wsel.t;
        bool changed = false;
        changed |= ImGui::DragFloat3(T("Позиция"), &t.x, 5.f, 0, 0, "%.0f");
        changed |= ImGui::DragFloat3(T("Поворот"), &t.pitch, 1.f, -360, 360, "%.0f°");
        if (wsel.kind == 2) changed |= ImGui::DragFloat(T("Масштаб"), &t.scale, 0.01f, 0.05f, 20.f, "%.2f");
        if (changed) {
            if (wsel.kind == 1) world.move[wsel.id] = t; else for (auto& a : world.add) if (a.uid == wsel.id) a.t = t;
            worldDirty = true;
        }
        if (ImGui::Button(wsel.kind == 1 ? T("Убрать из мира") : T("Удалить проп"))) WorldDelete();
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

void DrawStepEditor(Step& s, int index) {
    ImGui::SeparatorText((T("Этап ") + std::to_string(index + 1)).c_str());
    if (ImGui::Combo(T("Тип"), &s.type, TArr(kStepTypes, 3), 3)) previewDirty = true;
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
    auto modeButton = [](const char* label, int m) {
        const bool on = clickMode == m;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1));
        if (ImGui::Button(label)) clickMode = on ? kSelect : m;
        if (on) ImGui::PopStyleColor();
    };
    modeButton(T("Зону"), kPoint); ImGui::SameLine();
    modeButton(T("Объект"), kObject); ImGui::SameLine();
    modeButton(T("Предмет"), kItem); ImGui::SameLine();
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
        ImGui::InputText(T("Название"), Cur(quest.title), sizeof(quest.title.en));
        ImGui::Combo(T("Категория"), &quest.category, TArr(kCategories, 8), 8);
        ImGui::Combo(T("Вид маркеров"), &quest.style, TArr(kStyles, 8), 8);
        ImGui::InputTextMultiline(T("Вступление"), Cur(quest.text), sizeof(quest.text.en), ImVec2(-1, 50));
        ImGui::InputInt(T("Опыт"), &quest.xp);
        if (ImGui::TreeNode(T("Награда"))) {
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
    ImGui::SeparatorText(T("Этапы"));
    for (size_t i = 0; i < quest.steps.size(); ++i) {
        const Step& s = quest.steps[i];
        const std::string label = std::to_string(i + 1) + ". " + T(kStepTypes[s.type]) + ": " + Shown(s.text) + "##step" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected == int(i))) { selected = int(i); previewDirty = true; }
    }
    const char* adds[] = {T("+ Дойти"), T("+ Удерживать"), T("+ Сделать N раз")};
    for (int t = 0; t < 3; ++t) {
        if (t) ImGui::SameLine();
        if (ImGui::Button(adds[t])) { Step s; s.type = t; if (t == 2) s.events = 1; quest.steps.push_back(s); selected = int(quest.steps.size()) - 1; previewDirty = true; }
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
    if (ref.step < 0 || ref.step >= int(quest.steps.size())) return nullptr;
    Step& s = quest.steps[ref.step];
    if (ref.kind == 0 && s.type != 2 && s.hasPoint) return &s.x;
    if (ref.kind == 1 && s.type == 2 && s.hasArea) return &s.ax;
    if (ref.kind == 2 && s.type == 2 && ref.index < int(s.spawn.size())) { *yaw = &s.spawn[ref.index].yaw; return &s.spawn[ref.index].x; }
    return nullptr;
}
std::vector<std::pair<Ref, std::string>> Selectables() {
    std::vector<std::pair<Ref, std::string>> out;
    for (int i = 0; i < int(quest.steps.size()); ++i) {
        const Step& s = quest.steps[i];
        const std::string n = std::to_string(i + 1);
        if (s.type != 2 && s.hasPoint) out.push_back({{i, 0, 0}, T("Этап ") + n});
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
            if (wsel.kind == 1) world.move[wsel.id] = wsel.t; else for (auto& a : world.add) if (a.uid == wsel.id) a.t = wsel.t;
            worldDirty = true;
        }
        busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
        ImVec2 sp;
        if (Project(view, proj, w, h, wsel.t.x, wsel.t.y, wsel.t.z, sp)) ImGui::GetBackgroundDrawList()->AddText(ImVec2(sp.x + 12, sp.y + 12), IM_COL32(255, 200, 80, 255), wsel.kind == 1 ? wsel.id.c_str() : T("проп"));
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 12), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##wgizmo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        if (ImGui::RadioButton(T("Двигать (W)"), worldGizmo == 0)) worldGizmo = 0;
        ImGui::SameLine(); if (ImGui::RadioButton(T("Вращать (E)"), worldGizmo == 1)) worldGizmo = 1;
        if (wsel.kind == 2) { ImGui::SameLine(); if (ImGui::RadioButton(T("Масштаб (R)"), worldGizmo == 2)) worldGizmo = 2; }
        ImGui::SameLine(); ImGui::TextDisabled(T("Ctrl — шаг   Del — убрать"));
        ImGui::End();
    }
    if (overlay::CursorMode() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !busy) actions.push_back({Action::WorldClick, ""});
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
        if (hover >= 0) { picked = items[hover].first; selected = picked.step; previewDirty = true; }
        else picked = {};
    }
}

void Draw() {
    std::lock_guard g(lock);
    ImGuizmo::BeginFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 20, vp->WorkPos.y + 120), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 90), ImGuiCond_FirstUseEver);
    ImGui::Begin(T("Редактор DGMP"), nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::RadioButton(T("Мир"), &mode, 0); ImGui::SameLine(); if (ImGui::RadioButton(T("Квест"), &mode, 1)) previewDirty = true;
    ImGui::SameLine(200);
    ImGui::TextDisabled("%s", overlay::CursorMode() ? T("Tab — полёт") : T("Tab — курсор"));
    ImGui::SameLine(ImGui::GetWindowWidth() - 70);
    if (ImGui::SmallButton(english ? "RU" : "EN")) {
        english = !english;
        WritePrivateProfileStringW(L"Editor", L"Language", english ? L"en" : L"ru", (fs::path(profile::Root()) / L"editor.ini").c_str());
    }
    ImGui::End();
    DrawLibrary();
    if (mode == 1) DrawQuest();
    else DrawWorld();
    DrawSceneTools();
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

void Start() {
    if (!overlay::Install()) return;
    wchar_t lang[8] = L"en";
    GetPrivateProfileStringW(L"Editor", L"Language", L"en", lang, 8, (fs::path(profile::Root()) / L"editor.ini").c_str());
    english = wcscmp(lang, L"ru") != 0;
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
    UpdateSelectionOutline(controller);
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
