#include "longshot.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace longshot {

namespace {

// The options menu handler in game.dll, valid only for the verified build.
constexpr uint32_t kOptionsVtable = 0x1ac7e8;
constexpr size_t kMethodHandle = 0x6c;
constexpr size_t kMethodOpened = 0x80;
constexpr size_t kHandlerWindow = 0x7c;
constexpr size_t kHandlerTabs = 0xbc;
constexpr size_t kHandlerCurrentTab = 0xc8;

constexpr uint32_t kFindChild = 0x8aa0;
constexpr uint32_t kHolderLock = 0x16c0;
constexpr uint32_t kHolderUnlock = 0x1420;
constexpr uint32_t kButtonValueDef = 0x1e7128;
constexpr size_t kValueDefSet = 0x34;

constexpr size_t kItemFlags = 0x04;
constexpr size_t kItemObject = 0x10;
constexpr uint32_t kItemLoaded = 0x04;
constexpr size_t kViewId = 0xdc;
constexpr size_t kViewShow = 0x8c;

constexpr int kActionClicked = 2;
constexpr int kIdApply = 1;
constexpr int kIdCancel = 2;
constexpr int kIdDefaults = 3;
constexpr int kIdFirstTab = 10;
constexpr int kIdLastTab = 12;
constexpr int kIdLongshotTab = 13;
constexpr int kIdPage = 900;
constexpr int kIdFirstRow = 1001;

constexpr char kUiWhide[] = "?ui_whide@prism@@YAXPAVc_item_t@1@@Z";

using HandleFn = bool(__thiscall*)(void*, void*, int, void*);
using OpenedFn = void(__thiscall*)(void*);
using FindChildFn = char*(__thiscall*)(void*, int, int);
using HolderLockFn = char*(__thiscall*)(void*);
using HolderUnlockFn = void(__thiscall*)(void*);
using ViewShowFn = bool(__thiscall*)(void*, bool);
using ValueSetFn = void(__cdecl*)(void*, void*, int, const char*);
using UiWhideFn = void(__cdecl*)(void*);

struct Choice {
    const char* text;
    const char* value;
};

constexpr Choice kOnOff[] = {{"Off", "0"}, {"On", "1"}};
constexpr Choice kHudChoices[] = {
    {"Screen edges", "edges"}, {"Centred 16:9", "16_9"}, {"Centred 4:3", "4_3"}};
constexpr Choice kFrameCaps[] = {{"Off", "0"},     {"60", "60"},   {"120", "120"}, {"144", "144"},
                                 {"165", "165"},   {"240", "240"}, {"360", "360"}};
constexpr Choice kFovScales[] = {
    {"Standard", "1.0"}, {"+10%", "1.1"}, {"+20%", "1.2"}, {"+30%", "1.3"}};
constexpr Choice kAntiAliasing[] = {{"Off", "0"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}};
constexpr Choice kTextureFiltering[] = {
    {"Game setting", "0"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}};
constexpr Choice kGrassDistance[] = {{"Game", "0"}, {"Far", "47"}, {"Very far", "85"}};
constexpr Choice kLodDistance[] = {
    {"Game", "1"}, {"Far", "1.5"}, {"Very far", "2"}, {"Extreme", "3"}};
constexpr Choice kActionSpeed[] = {
    {"Normal", "1"}, {"Quick", "1.25"}, {"Quicker", "1.5"}, {"Quickest", "2"}};
constexpr Choice kSharpening[] = {
    {"Off", "0"}, {"Light", "0.3"}, {"Medium", "0.6"}, {"Strong", "1.0"}};
constexpr Choice kVibrance[] = {{"Original", "0"}, {"Richer", "0.2"}, {"Vivid", "0.4"}};
constexpr Choice kBloom[] = {{"Off", "0"}, {"Soft", "0.15"}, {"Strong", "0.35"}};
constexpr Choice kZoomModes[] = {{"Scales with zoom", "proportional"},
                                 {"Same as unscoped", "off"},
                                 {"Game default", "stock"}};
constexpr Choice kTurnSpeeds[] = {{"Use the game's slider", "0"}, {"15 cm/360", "15"},
                                  {"20 cm/360", "20"},            {"25 cm/360", "25"},
                                  {"30 cm/360", "30"},            {"35 cm/360", "35"},
                                  {"40 cm/360", "40"},            {"50 cm/360", "50"},
                                  {"60 cm/360", "60"},            {"80 cm/360", "80"}};
constexpr Choice kMouseDpis[] = {{"400", "400"},   {"800", "800"},   {"1000", "1000"},
                                 {"1200", "1200"}, {"1600", "1600"}, {"2400", "2400"},
                                 {"3200", "3200"}, {"6400", "6400"}};

struct Row {
    const char* label;
    const wchar_t* section;
    const wchar_t* key;
    const Choice* choices;
    int count;
    const char* fallback;  // the default in longshot.default.ini
    bool needs_restart;
};

template <size_t N>
constexpr int count_of(const Choice (&)[N]) {
    return static_cast<int>(N);
}

const Row kRows[] = {
    {"HUD position", L"view", L"hud_layout", kHudChoices, count_of(kHudChoices), "edges", true},
    {"Field of view", L"view", L"fov_scale", kFovScales, count_of(kFovScales), "1.0", false},
    {"Anti-aliasing", L"view", L"anti_aliasing", kAntiAliasing, count_of(kAntiAliasing), "0", true},
    {"Texture filtering", L"view", L"texture_filtering", kTextureFiltering,
     count_of(kTextureFiltering), "0", true},
    {"Grass distance", L"view", L"grass_distance", kGrassDistance, count_of(kGrassDistance), "0", false},
    {"LOD distance", L"view", L"lod_distance", kLodDistance, count_of(kLodDistance), "1", false},
    {"Sharpening", L"view", L"sharpening", kSharpening, count_of(kSharpening), "0", false},
    {"Colour", L"view", L"vibrance", kVibrance, count_of(kVibrance), "0", false},
    {"Bloom", L"view", L"bloom", kBloom, count_of(kBloom), "0", false},
    {"V-sync", L"display", L"vsync", kOnOff, count_of(kOnOff), "1", false},
    {"Frame rate limit", L"display", L"fps_cap", kFrameCaps, count_of(kFrameCaps), "0", false},
    {"Turn speed", L"mouse", L"cm_per_360", kTurnSpeeds, count_of(kTurnSpeeds), "0", false},
    {"Mouse DPI", L"mouse", L"mouse_dpi", kMouseDpis, count_of(kMouseDpis), "800", false},
    {"Scoped sensitivity", L"mouse", L"zoom_mode", kZoomModes, count_of(kZoomModes), "proportional",
     false},
    {"Straight walking", L"game", L"straight_walking", kOnOff, count_of(kOnOff), "1", false},
    {"Weapon input queue", L"game", L"weapon_queue", kOnOff, count_of(kOnOff), "1", false},
    {"Weapon action speed", L"game", L"action_speed", kActionSpeed, count_of(kActionSpeed), "1", false},
    {"Strong ATV", L"game", L"strong_atv", kOnOff, count_of(kOnOff), "1", false},
    {"Pause in the background", L"game", L"pause_in_background", kOnOff, count_of(kOnOff), "0", true},
    {"Skip the intro", L"game", L"skip_intro", kOnOff, count_of(kOnOff), "0", true},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// A value that isn't one of the row's choices came from editing longshot.ini by hand. It stays
// shown as it is, and is only replaced if the player clicks that row.
struct RowState {
    int choice = 0;
    std::string exact;
    bool changed = false;
};

RowState g_state[kRowCount];
bool g_available = true;
bool g_page_open = false;

char* g_game = nullptr;
HandleFn g_real_handle = nullptr;
OpenedFn g_real_opened = nullptr;
UiWhideFn g_ui_whide = nullptr;

void set_from_value(int row, const std::string& value) {
    RowState& state = g_state[row];
    for (int i = 0; i < kRows[row].count; ++i) {
        if (!_stricmp(kRows[row].choices[i].value, value.c_str())) {
            state.choice = i;
            state.exact.clear();
            return;
        }
    }
    state.choice = -1;
    state.exact = value;
}

void read_saved() {
    for (int row = 0; row < kRowCount; ++row) {
        set_from_value(row, config_read(kRows[row].section, kRows[row].key, kRows[row].fallback));
        g_state[row].changed = false;
    }
}

const char* value_of(int row) {
    const RowState& state = g_state[row];
    return state.choice < 0 ? state.exact.c_str() : kRows[row].choices[state.choice].value;
}

const char* text_of(int row) {
    const RowState& state = g_state[row];
    return state.choice < 0 ? state.exact.c_str() : kRows[row].choices[state.choice].text;
}

char* item_object(char* item) {
    if (!item || !(*reinterpret_cast<uint32_t*>(item + kItemFlags) & kItemLoaded)) return nullptr;
    return *reinterpret_cast<char**>(item + kItemObject);
}

char* find_view(char* handler, int id) {
    char* window = item_object(*reinterpret_cast<char**>(handler + kHandlerWindow));
    if (!window) return nullptr;
    return item_object(reinterpret_cast<FindChildFn>(g_game + kFindChild)(window, id, 1));
}

void show_page(char* handler, bool show) {
    char* page = find_view(handler, kIdPage);
    if (!page) return;
    void** methods = *reinterpret_cast<void***>(page);
    reinterpret_cast<ViewShowFn>(methods[kViewShow / sizeof(void*)])(page, show);
}

void refresh_row(char* handler, int row) {
    char* button = find_view(handler, kIdFirstRow + row);
    if (!button) return;
    char* definition = g_game + kButtonValueDef;
    auto set = *reinterpret_cast<ValueSetFn*>(definition + kValueDefSet);
    if (set) set(button, definition, 0, text_of(row));
}

void refresh_rows(char* handler) {
    for (int row = 0; row < kRowCount; ++row) refresh_row(handler, row);
}

void cycle_row(char* handler, int row) {
    RowState& state = g_state[row];
    state.choice = (state.choice + 1) % kRows[row].count;
    state.exact.clear();
    state.changed = true;
    refresh_row(handler, row);
    logf("settings: %s is now %s", kRows[row].label, text_of(row));
}

void reset_rows(char* handler) {
    for (int row = 0; row < kRowCount; ++row) {
        set_from_value(row, kRows[row].fallback);
        g_state[row].changed = true;
    }
    refresh_rows(handler);
}

void save_rows() {
    bool any = false;
    for (int row = 0; row < kRowCount; ++row) {
        if (!g_state[row].changed) continue;
        config_write(kRows[row].section, kRows[row].key, value_of(row));
        g_state[row].changed = false;
        any = true;
    }
    if (!any) return;
    config_load();
    display_settings_changed();
    detail_settings_changed();
    vehicle_settings_changed();
}

// Hides whichever of the game's own tabs is showing, the same way the game reaches it.
void hide_current_tab(char* handler) {
    const int tab = *reinterpret_cast<int*>(handler + kHandlerCurrentTab);
    void* holder = *reinterpret_cast<void**>(handler + kHandlerTabs + tab * sizeof(void*));
    if (!holder || !g_ui_whide) return;
    char* tab_handler = reinterpret_cast<HolderLockFn>(g_game + kHolderLock)(holder);
    void* window = *reinterpret_cast<void**>(tab_handler + kHandlerWindow);
    reinterpret_cast<HolderUnlockFn>(g_game + kHolderUnlock)(holder);
    g_ui_whide(window);
}

bool __fastcall hook_options_handle(char* handler, void*, char* view, int action, void* param) {
    const int id = action == kActionClicked && view ? *reinterpret_cast<int*>(view + kViewId) : 0;

    // Defaults applies to the tab that's showing, so the game's own tabs have to be left out of it.
    if (id == kIdDefaults && g_page_open) {
        reset_rows(handler);
        return true;
    }
    if (id == kIdApply) save_rows();

    const bool result = g_real_handle(handler, view, action, param);

    if (id == kIdCancel) {
        read_saved();
    } else if (id == kIdLongshotTab && !g_page_open) {
        hide_current_tab(handler);
        show_page(handler, true);
        refresh_rows(handler);
        g_page_open = true;
        logf("settings: tab opened");
    } else if (id >= kIdFirstTab && id <= kIdLastTab && g_page_open) {
        show_page(handler, false);
        g_page_open = false;
    } else if (id >= kIdFirstRow && id < kIdFirstRow + kRowCount) {
        cycle_row(handler, id - kIdFirstRow);
    }
    return result;
}

void __fastcall hook_options_opened(char* handler) {
    g_real_opened(handler);
    read_saved();
    show_page(handler, false);
    g_page_open = false;
}

// A control's layer is its own, whatever group it's in, and the mouse goes to the highest one under
// it. The options window's frame is on layer 1, so every control here has to be above that.
void add_row(std::string* text, int row, int top) {
    const std::string bottom = std::to_string(top - 22);
    const std::string span = bottom + " ";
    const std::string id = std::to_string(kIdFirstRow + row);
    std::string label = kRows[row].label;
    if (kRows[row].needs_restart) label += " *";

    *text += "entity \"ui.comp.common.text\" {\r\nevalues {\r\n\"look\" : \"7\"\r\n\"layer\" : \"2\"\r\n\"value\" : \"" + label +
             "\"\r\n\"coords\" : \"206 " + span + "649 " + std::to_string(top) + "\"\r\n}\r\n}\r\n";
    *text += "entity \"ui.comp.common.button\" {\r\nevalues {\r\n\"look\" : \"26\"\r\n\"layer\" : \"2\"\r\n\"tab\" : \"" + id +
             "\"\r\n\"id\" : \"" + id + "\"\r\n\"coords\" : \"658 " + span + "986 " +
             std::to_string(top) + "\"\r\n}\r\n}\r\n";
}

}  // namespace

bool settings_available() { return g_available; }

std::string settings_tab_entity() {
    char text[320];
    snprintf(text, sizeof(text),
             "entity \"ui.comp.common.button\" {\r\nevalues {\r\n\"value\" : \"LONGSHOT\"\r\n"
             "\"button_type\" : \"2\"\r\n\"layer\" : \"1\"\r\n\"tab\" : \"1\"\r\n\"id\" : \"%d\"\r\n"
             "\"area\" : \"38 250 330 273\"\r\n\"coords\" : \"38 250 330 282\"\r\n"
             "\"name\" : \"longshot\"\r\n}\r\n}",
             kIdLongshotTab);
    return text;
}

std::string settings_page_entity() {
    constexpr int kFirstTop = 640;
    constexpr int kRowStep = 27;

    std::string rows;
    int top = kFirstTop;
    for (int row = 0; row < kRowCount; ++row, top -= kRowStep) add_row(&rows, row, top);
    rows += "entity \"ui.comp.common.text\" {\r\nevalues {\r\n\"look\" : \"7\"\r\n\"layer\" : \"2\"\r\n"
            "\"value\" : \"* Applies after restarting the game\"\r\n"
            "\"coords\" : \"206 " + std::to_string(top - 22) + " 649 " + std::to_string(top) +
            "\"\r\n}\r\n}\r\n";

    return "entity \"ui.comp.group\" {\r\nevalues {\r\n\"fitting\" : \"true\"\r\n\"coords\" : \"206 " +
           std::to_string(top - 22) + " 986 " + std::to_string(kFirstTop) +
           "\"\r\n\"layer\" : \"2\"\r\n\"id\" : \"" + std::to_string(kIdPage) +
           "\"\r\n\"name\" : \"longshot page\"\r\n}\r\nentities {\r\n" + rows + "}\r\n}";
}

void settings_attach_game(HMODULE game, bool verified) {
    g_available = verified;
    if (!verified) return;

    g_game = reinterpret_cast<char*>(game);
    HMODULE shared = GetModuleHandleW(L"p3shared.dll");
    g_ui_whide = shared ? reinterpret_cast<UiWhideFn>(GetProcAddress(shared, kUiWhide)) : nullptr;

    auto** table = reinterpret_cast<void**>(g_game + kOptionsVtable);
    const bool handle = vtable_patch(table + kMethodHandle / sizeof(void*),
                                     reinterpret_cast<void*>(&hook_options_handle),
                                     reinterpret_cast<void**>(&g_real_handle));
    const bool opened = vtable_patch(table + kMethodOpened / sizeof(void*),
                                     reinterpret_cast<void*>(&hook_options_opened),
                                     reinterpret_cast<void**>(&g_real_opened));
    logf("settings: options menu hooks %d %d", handle, opened);
}

}  // namespace longshot
