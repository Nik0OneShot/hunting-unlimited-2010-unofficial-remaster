#include "longshot.h"

#include <cstring>
#include <string>

namespace longshot {

namespace {

// Offsets into game.dll.
// Neither list has the game coming back to an item that was waiting on something else to go
// away, at +0xde1c and +0xddf3. A light never waits.
constexpr uintptr_t kChooseItem = 0xd8e0;
constexpr uintptr_t kChooseItemCalls[] = {0x81453, 0x88072, 0x88425, 0xe262};
constexpr uintptr_t kUseItem = 0xd740;
constexpr uintptr_t kUseItemCalls[] = {0xdb42, 0xe255};
constexpr uintptr_t kSelectItem = 0xa1130;
constexpr uintptr_t kPlaySound = 0x16420;
constexpr size_t kMenuQuiet = 0x80;
constexpr size_t kHunterWaiting = 0x58;
constexpr size_t kHunterItems = 0x88;
constexpr size_t kHunterHasHands = 0x100;
constexpr size_t kItemsSelected = 0x10;
constexpr size_t kItemFlags = 0x6;
constexpr size_t kItemName = 0x30;
constexpr size_t kItemState = 0x58;
constexpr unsigned char kItemExists = 0x80;
constexpr int kItemDown = 2;
constexpr uintptr_t kEnvironment = 0xe6080;
constexpr size_t kEnvironmentAmbient = 0xb4;
constexpr size_t kEnvironmentDiffuse = 0xc4;

constexpr char kFlashlightName[] = "flashlight";
constexpr char kLanternName[] = "lantern";
// The menu's own click is also the sound of cycling through your items. This is a short, sharp
// tick, which a hunt otherwise only has as a small part of how a few guns load.
constexpr char kSwitchSound[] = "/sounds/weapons/ws_44cal_missfire.ogg";
// Neither is ever raised, so they only need to be something the loadout can hold.
constexpr char kLightItems[] =
    "\r\ndef flashlight:eqitem_base {\r\n"
    "\tshort:\"Flashlight\"\r\n"
    "\tlong:\"Flashlight\"\r\n"
    "\tdesc:\"Lights up what's in front of you. It stays on whatever else you're using, and works with your hands full.\"\r\n"
    "\tscript:\"/script/item/map.sii\"\r\n"
    "\tpba:\"/models/equipment/map/equip.pba\"\r\n"
    "\tcamo_look:\"true\"\r\n"
    "\tarea_variant:\"true\"\r\n"
    "\ticon:\"/material/ui/icon/i_predatorcall.mat\"\r\n"
    "\tglow:\"/material/ui/icon/i_predatorcall_glow.mat\"\r\n"
    "\tuse_sound:\"/sounds/items/eq_map.ogg\"\r\n"
    "\tcategory:\"misc\"\r\n"
    "}\r\n"
    "def lantern:eqitem_base {\r\n"
    "\tshort:\"Lantern\"\r\n"
    "\tlong:\"Lantern\"\r\n"
    "\tdesc:\"Lights up the ground all around you. It stays on whatever else you're using, and works with your hands full.\"\r\n"
    "\tscript:\"/script/item/map.sii\"\r\n"
    "\tpba:\"/models/equipment/map/equip.pba\"\r\n"
    "\tcamo_look:\"true\"\r\n"
    "\tarea_variant:\"true\"\r\n"
    "\ticon:\"/material/ui/icon/i_predatorcall.mat\"\r\n"
    "\tglow:\"/material/ui/icon/i_predatorcall_glow.mat\"\r\n"
    "\tuse_sound:\"/sounds/items/eq_map.ogg\"\r\n"
    "\tcategory:\"misc\"\r\n"
    "}\r\n";

// How far a pixel can be lifted, as the light a surface would get by day over what it's getting now.
constexpr float kLitSurface = 0.8f;
constexpr float kDiffuseShare = 0.5f;
constexpr float kDarkest = 0.02f;
constexpr float kMostLift = 10.0f;

using ChooseItemFn = void(__fastcall*)(void*, void*, const char* const*);
using UseItemFn = void(__fastcall*)(void*, void*);
using SelectItemFn = bool(__fastcall*)(void*, void*, const char* const*);
using PlaySoundFn = void(__fastcall*)(void*, void*, const char* const*);
using EnvironmentFn = char*(__cdecl*)();

char* g_game = nullptr;
bool g_beam = false;
bool g_glow = false;
// Playing a sound belongs to a menu, and only looks in it to see if it's been made quiet.
char g_menu[kMenuQuiet + 1] = {};

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

float light_lift(float ambient, float diffuse) {
    const float level = ambient + diffuse * kDiffuseShare;
    const float lift = kLitSurface / (level > kDarkest ? level : kDarkest);
    return lift < 1.0f ? 1.0f : lift > kMostLift ? kMostLift : lift;
}

bool* light_named(const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, kFlashlightName)) return &g_beam;
    return strcmp(name, kLanternName) ? nullptr : &g_glow;
}

void toggle(bool* light) {
    *light = !*light;
    const char* sound = kSwitchSound;
    reinterpret_cast<PlaySoundFn>(g_game + kPlaySound)(g_menu, nullptr, &sound);
    logf("lights: %s %s", light == &g_beam ? kFlashlightName : kLanternName, *light ? "on" : "off");
}

char* selected_item(void* hunter) {
    char* item = field<char*>(field<char*>(hunter, kHunterItems), kItemsSelected);
    return item && (field<unsigned char>(item, kItemFlags) & kItemExists) ? item : nullptr;
}

// Choosing one flips it right away. It also becomes the selected item, so the item key goes on
// flipping it, unless that would take something out of your hands.
void __fastcall hook_choose_item(void* hunter, void* unused, const char* const* name) {
    bool* light = name ? light_named(*name) : nullptr;
    if (!light) {
        reinterpret_cast<ChooseItemFn>(g_game + kChooseItem)(hunter, unused, name);
        return;
    }
    toggle(light);
    char* held = selected_item(hunter);
    if (field<int>(hunter, kHunterWaiting) == 0 && field<bool>(hunter, kHunterHasHands) &&
        (!held || field<int>(held, kItemState) == kItemDown)) {
        reinterpret_cast<SelectItemFn>(g_game + kSelectItem)(field<void*>(hunter, kHunterItems), nullptr, name);
    }
}

// The game would put the gun away and raise the item. These only have a switch.
void __fastcall hook_use_item(void* hunter, void* unused) {
    char* item = selected_item(hunter);
    bool* light = item ? light_named(field<const char*>(item, kItemName)) : nullptr;
    if (light) {
        toggle(light);
    } else {
        reinterpret_cast<UseItemFn>(g_game + kUseItem)(hunter, unused);
    }
}

bool read_daylight(float* ambient, float* diffuse) {
    __try {
        char* place = reinterpret_cast<EnvironmentFn>(g_game + kEnvironment)();
        if (!place) return false;
        memcpy(ambient, place + kEnvironmentAmbient, 3 * sizeof(float));
        memcpy(diffuse, place + kEnvironmentDiffuse, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool lights_available() { return g_game != nullptr; }

std::string lights_add_items(const std::string& equipment) { return equipment + kLightItems; }

void lights_hunt_started() { g_beam = g_glow = false; }

bool lights_wanted() { return (g_beam || g_glow) && walking_player() != nullptr; }

bool lights_beam_on() { return g_beam; }

bool lights_glow_on() { return g_glow; }

bool lights_this_frame(PostLight* light) {
    if (!lights_wanted()) return false;
    float ambient[3], diffuse[3];
    if (!read_daylight(ambient, diffuse)) return false;
    light->beam = g_beam ? 1.0f : 0.0f;
    light->glow = g_glow ? 1.0f : 0.0f;
    for (int part = 0; part < 3; ++part) light->lift[part] = light_lift(ambient[part], diffuse[part]);
    return true;
}

void lights_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    bool use = true;
    for (const uintptr_t call : kChooseItemCalls) {
        use = patch_call(g_game + call, g_game + kChooseItem, reinterpret_cast<const void*>(&hook_choose_item)) && use;
    }
    for (const uintptr_t call : kUseItemCalls) {
        use = patch_call(g_game + call, g_game + kUseItem, reinterpret_cast<const void*>(&hook_use_item)) && use;
    }
    logf("lights: switches=%d", use);
    if (!use) g_game = nullptr;
}

int lights_test() {
    if (light_lift(1.0f, 1.0f) != 1.0f) return 1;                          // nothing to add by day
    if (light_lift(0.13f, 0.0f) < 5.0f || light_lift(0.13f, 0.0f) > 7.0f) return 2;  // but plenty by moonlight
    if (light_lift(0.0f, 0.0f) != kMostLift) return 3;                     // and it stops somewhere
    if (light_lift(0.13f, 0.0f) <= light_lift(0.22f, 0.0f)) return 4;      // the blue of moonlight comes back out
    const std::string with_items = lights_add_items("def a {\n}\n");
    if (with_items.find("def a {") != 0) return 5;
    if (with_items.find("def flashlight:eqitem_base") == std::string::npos) return 6;
    if (with_items.find("def lantern:eqitem_base") == std::string::npos) return 7;
    if (with_items.find("i_predatorcall.mat") == std::string::npos) return 8;
    if (light_named(kFlashlightName) != &g_beam || light_named(kLanternName) != &g_glow) return 9;
    if (light_named("map") || light_named(nullptr)) return 10;
    return 0;
}

}  // namespace longshot
