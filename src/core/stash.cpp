#include "longshot.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace longshot {

namespace {

// Offsets into game.dll.
// A bagged animal goes in one of the profile's tags (`+0x90420`), and a free hunt can't bag another
// once they're all used. Only after the hunt does the trophy room take them out of the tags
// (`+0x40ab3`), and keep the ones moved there in the profile's trophies (`+0x3ff7f`). A stash does
// that halfway through, at a camp, and saves the profile as closing the room does.
constexpr uintptr_t kCurrentGame = 0x4f50;
constexpr uintptr_t kHuntOf = 0x4f10;
constexpr uintptr_t kAddAction = 0x929d0;
constexpr uintptr_t kProfiles = 0x1e9d68;       // its +4 is the profile
constexpr uintptr_t kTakeFromTag = 0x96df0;
constexpr uintptr_t kKeepTrophy = 0xf3e70;
constexpr uintptr_t kSaveProfile = 0xd2450;
constexpr uintptr_t kHudOf = 0x30e0;
constexpr uintptr_t kReleaseResource = 0x7b120;
constexpr uintptr_t kShowMessage = 0x816a0;
// What the interact key can do is a list on the hunt, scored every frame through each one's +4, the
// lowest score at or above 0 going first, and done through its +8 (`+0x93140`).
constexpr size_t kHuntActionsHead = 0x148;
constexpr size_t kActionProvider = 8;
constexpr size_t kProfileTrophies = 0xac;
constexpr size_t kProfileTags = 0xb0;
constexpr size_t kTrophyList = 0x20;
constexpr size_t kListItems = 4;
constexpr size_t kListCount = 8;
constexpr size_t kTagTrophy = 4;
constexpr size_t kUnitUses = 4;
constexpr size_t kUnitFlags = 6;
constexpr unsigned char kUnitAlive = 0x80;
constexpr unsigned char kUnitOwned = 0x40;
constexpr unsigned kMostActions = 1000;

// Anything else on offer, an animal to bag or a vehicle to get in, scores lower and goes first.
constexpr float kStashScore = 1.0e6f;
constexpr float kNotHere = -1.0f;
constexpr float kNoTime = -1.0f;  // the message stays as long as the game's own do
// A camp reaches this far past the building or vehicle, all round. Its size is the box in its model's
// geometry, after the version and the bounding sphere, lowest corner then highest. One whose size
// can't be read is taken as this far from the middle either way.
constexpr float kCampMargin = 10.0f;
constexpr float kUnknownSize = 5.0f;
constexpr size_t kGeometryBox = 0x24;
constexpr float kLargestModel = 500.0f;

// Anywhere with a building or a parked vehicle is a camp. Each map has at least one.
// The lighthouse isn't one: its model's box takes in the beam, 100 m long.
constexpr const char* kCampModels[] = {"house",   "house2floor", "hcabin",   "hguard",   "hut",       "tent",
                                       "campfire", "hgarage",    "h2garage", "hshed",    "hcowshed",  "hroof",
                                       "ruinhouse", "ruinstation", "ranger",  "landrover"};
constexpr char kModels[] = "/models/objects/";
constexpr char kModelEnding[] = ".ma";
constexpr char kGeometryEnding[] = ".pmg";
constexpr char kWorld[] = "world.sii";
constexpr char kUnitsKey[] = "world_units_filename: \"";
constexpr char kUnitsFallback[] = "landscape.game.sii";
constexpr char kDescription[] = "\nbody_desc : ";
constexpr char kPlaced[] = "\nbody_inst : ";
constexpr char kModelKey[] = "model_file: \"";
constexpr char kDescriptionKey[] = "body_desc: ";
constexpr char kPlacementKey[] = "placement: (";

constexpr char kTemplate[] = "def template";
constexpr char kLabel[] =
    "def template%d {\r\n"
    "\tname:\"[Activate: Stash]\"\r\n"
    "\ttext:\"<offset hshift=12><img src=/material/ui/ingame/x_claimanimal.mat width=64 height=64></align><ret>"
    "<offset vshift=68><align hstyle=center><font @@__fnt_text@@>Stash</align></font>\"\r\n"
    "\twidth:\"88\"\r\n"
    "\theight:\"88\"\r\n"
    "}\r\n";
constexpr char kStashed[] = "<img src=/material/ui/ingame/x_ok.mat><br>%d %s stashed in your trophy room.<br>Your tags are free again.";

struct Box {
    float low[3];
    float high[3];
};

// Where it is, which way it's turned (w, then x, y and z), and how big it is.
struct Camp {
    float place[3];
    float turn[4];
    Box box;
};

using BoxOf = std::function<bool(const std::string& model, Box* box)>;

struct Held {
    void* resource;
    char* contents;
};

using CurrentGameFn = char*(__cdecl*)();
using HuntOfFn = char*(__fastcall*)(char*, void*);
using AddActionFn = char**(__fastcall*)(char*, void*, char**, void*, int, int);
using TakeFromTagFn = char**(__fastcall*)(char*, void*, char**, unsigned);
using KeepTrophyFn = void*(__fastcall*)(char*, void*, char**);
using SaveProfileFn = bool(__fastcall*)(char*, void*);
using HudOfFn = Held*(__fastcall*)(char*, void*, Held*);
using ReleaseResourceFn = void(__fastcall*)(Held*, void*);
using ShowMessageFn = void(__fastcall*)(char*, void*, const char* const*, float);

char* g_game = nullptr;
int g_label = -1;
std::vector<Camp> g_camps;
char* g_hunt = nullptr;
char* g_node = nullptr;
bool g_can_stash = false;
int g_bagged = 0;
unsigned g_stashes = 0;

template <typename T>
T& field(void* base, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

// "/models/objects/<name>/..." is a camp if <name> is one of the camp's.
bool camp_model(const std::string& model) {
    const size_t start = sizeof(kModels) - 1;
    if (model.compare(0, start, kModels) != 0) return false;
    const size_t end = model.find('/', start);
    if (end == std::string::npos) return false;
    const std::string name = model.substr(start, end - start);
    for (const char* camp : kCampModels) {
        if (name == camp) return true;
    }
    return false;
}

// A number as the files write it: plain, or & and the float's bits in hex.
float sii_number(const char* text, const char** end) {
    while (*text == ' ') ++text;
    char* after = nullptr;
    float value = 0.0f;
    if (*text == '&') {
        const uint32_t bits = strtoul(text + 1, &after, 16);
        memcpy(&value, &bits, sizeof(value));
    } else {
        value = strtof(text, &after);
    }
    *end = after;
    return value;
}

// The rest of the line after key, inside the block from start to end.
std::string value_in(const std::string& text, size_t start, size_t end, const char* key) {
    const size_t at = text.find(key, start);
    if (at == std::string::npos || at >= end) return std::string();
    const size_t from = at + strlen(key);
    const size_t line_end = text.find_first_of("\"\r\n", from);
    return text.substr(from, (line_end == std::string::npos ? text.size() : line_end) - from);
}

// The box a model's geometry gives, if it makes sense.
bool box_in_geometry(const std::string& geometry, Box* box) {
    if (geometry.size() < kGeometryBox + sizeof(Box)) return false;
    memcpy(box, geometry.data() + kGeometryBox, sizeof(Box));
    for (int i = 0; i < 3; ++i) {
        if (!(box->low[i] <= box->high[i]) || box->high[i] - box->low[i] > kLargestModel) return false;
    }
    return true;
}

// Skips spaces and the given character, if it's there.
const char* past(const char* text, char wanted) {
    while (*text == ' ') ++text;
    return *text == wanted ? text + 1 : text;
}

// Where each building and parked vehicle is, and its size, from a map's world units.
std::vector<Camp> camps_in(const std::string& units, const BoxOf& box_of) {
    std::vector<std::pair<std::string, Box>> descriptions;
    for (size_t at = units.find(kDescription); at != std::string::npos; at = units.find(kDescription, at + 1)) {
        const size_t name = at + sizeof(kDescription) - 1;
        const size_t end = units.find("\n}", name);
        if (end == std::string::npos) break;
        const std::string model = value_in(units, name, end, kModelKey);
        if (!camp_model(model)) continue;
        Box box = {{-kUnknownSize, -kUnknownSize, -kUnknownSize}, {kUnknownSize, kUnknownSize, kUnknownSize}};
        if (!box_of(model, &box)) box = {{-kUnknownSize, -kUnknownSize, -kUnknownSize}, {kUnknownSize, kUnknownSize, kUnknownSize}};
        descriptions.emplace_back(units.substr(name, units.find(' ', name) - name), box);
    }
    std::vector<Camp> camps;
    if (descriptions.empty()) return camps;
    for (size_t at = units.find(kPlaced); at != std::string::npos; at = units.find(kPlaced, at + 1)) {
        const size_t end = units.find("\n}", at + 1);
        if (end == std::string::npos) break;
        std::string description = value_in(units, at, end, kDescriptionKey);
        while (!description.empty() && description.back() == ' ') description.pop_back();
        const Box* box = nullptr;
        for (const auto& known : descriptions) {
            if (known.first == description) box = &known.second;
        }
        const size_t placement = units.find(kPlacementKey, at);
        if (!box || placement == std::string::npos || placement >= end) continue;
        Camp found = {{}, {1.0f, 0.0f, 0.0f, 0.0f}, *box};
        const char* text = units.c_str() + placement + sizeof(kPlacementKey) - 1;
        for (float& value : found.place) {
            value = sii_number(text, &text);
            text = past(text, ',');
        }
        // Then ") scale (w; x, y, z)".
        text = past(text, ')');
        sii_number(text, &text);
        text = past(text, '(');
        found.turn[0] = sii_number(text, &text);
        text = past(text, ';');
        for (int i = 1; i < 4; ++i) {
            found.turn[i] = sii_number(text, &text);
            text = past(text, ',');
        }
        camps.push_back(found);
    }
    return camps;
}

// A world direction turned back into the model's own.
void into_model(const float* turn, const float* world, float* local) {
    const float u[3] = {-turn[1], -turn[2], -turn[3]};
    const float w = turn[0];
    const float cross[3] = {u[1] * world[2] - u[2] * world[1], u[2] * world[0] - u[0] * world[2], u[0] * world[1] - u[1] * world[0]};
    const float twice[3] = {2.0f * (u[1] * cross[2] - u[2] * cross[1]), 2.0f * (u[2] * cross[0] - u[0] * cross[2]),
                            2.0f * (u[0] * cross[1] - u[1] * cross[0])};
    for (int i = 0; i < 3; ++i) local[i] = world[i] + 2.0f * w * cross[i] + twice[i];
}

// The first camp a place is within reach of, across the ground, or -1.
int camp_at(const std::vector<Camp>& camps, const float* place) {
    for (size_t i = 0; i < camps.size(); ++i) {
        const Camp& camp = camps[i];
        const float offset[3] = {place[0] - camp.place[0], place[1] - camp.place[1], place[2] - camp.place[2]};
        float local[3];
        into_model(camp.turn, offset, local);
        if (local[0] >= camp.box.low[0] - kCampMargin && local[0] <= camp.box.high[0] + kCampMargin &&
            local[2] >= camp.box.low[2] - kCampMargin && local[2] <= camp.box.high[2] + kCampMargin) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The label's added after the game's own, as the number the HUD knows it by.
std::string add_label(const std::string& text, int* label) {
    int count = 0;
    for (size_t at = text.find(kTemplate); at != std::string::npos; at = text.find(kTemplate, at + 1)) ++count;
    *label = -1;
    if (count == 0) return text;
    char added[512];
    snprintf(added, sizeof(added), kLabel, count);
    *label = count;
    std::string with_label = text;
    if (!with_label.empty() && with_label.back() != '\n') with_label += "\r\n";
    return with_label + "\r\n" + added;
}

std::string stashed_text(int count) {
    char text[256];
    snprintf(text, sizeof(text), kStashed, count, count == 1 ? "animal" : "animals");
    return text;
}

char* profile() {
    char* profiles = field<char*>(g_game, kProfiles);
    return profiles ? field<char*>(profiles, sizeof(void*)) : nullptr;
}

char* current_hunt() {
    char* game = reinterpret_cast<CurrentGameFn>(g_game + kCurrentGame)();
    return game ? reinterpret_cast<HuntOfFn>(g_game + kHuntOf)(game, nullptr) : nullptr;
}

int bagged(char* player_profile) {
    char* tags = field<char*>(player_profile, kProfileTags);
    if (!tags) return 0;
    int count = 0;
    for (unsigned i = 0; i < field<unsigned>(tags, kListCount); ++i) {
        char* tag = field<char**>(tags, kListItems)[i];
        char* trophy = tag ? field<char*>(tag, kTagTrophy) : nullptr;
        count += trophy && (field<unsigned char>(trophy, kUnitFlags) & kUnitAlive) ? 1 : 0;
    }
    return count;
}

// Each trophy out of its tag, held the way the trophy room holds it, then moved into the profile's.
int stash_trophies(char* player_profile) {
    char* tags = field<char*>(player_profile, kProfileTags);
    char* kept = field<char*>(player_profile, kProfileTrophies);
    if (!tags || !kept) return 0;
    int stashed = 0;
    for (unsigned i = 0; i < field<unsigned>(tags, kListCount); ++i) {
        char* tag = field<char**>(tags, kListItems)[i];
        char* in_tag = tag ? field<char*>(tag, kTagTrophy) : nullptr;
        if (!in_tag || !(field<unsigned char>(in_tag, kUnitFlags) & kUnitAlive)) continue;
        char* trophy = nullptr;
        reinterpret_cast<TakeFromTagFn>(g_game + kTakeFromTag)(tags, nullptr, &trophy, i);
        if (!trophy) continue;
        field<unsigned char>(trophy, kUnitFlags) |= kUnitOwned;
        ++field<unsigned short>(trophy, kUnitUses);
        char* held = trophy;
        if (reinterpret_cast<KeepTrophyFn>(g_game + kKeepTrophy)(kept + kTrophyList, nullptr, &held)) {
            ++stashed;
        } else {
            logf("stash: the trophy room had no room for one");
        }
    }
    return stashed;
}

void show_message(const std::string& message) {
    char* game = reinterpret_cast<CurrentGameFn>(g_game + kCurrentGame)();
    if (!game) return;
    Held hud = {};
    reinterpret_cast<HudOfFn>(g_game + kHudOf)(game, nullptr, &hud);
    const char* text = message.c_str();
    if (hud.contents) reinterpret_cast<ShowMessageFn>(g_game + kShowMessage)(hud.contents, nullptr, &text, kNoTime);
    reinterpret_cast<ReleaseResourceFn>(g_game + kReleaseResource)(&hud, nullptr);
}

void stash() {
    char* player_profile = profile();
    if (!player_profile) return;
    const int stashed = stash_trophies(player_profile);
    if (stashed == 0) return;
    const bool saved = reinterpret_cast<SaveProfileFn>(g_game + kSaveProfile)(player_profile, nullptr);
    show_message(stashed_text(stashed));
    g_can_stash = false;
    ++g_stashes;
    logf("stash: %d stashed in the trophy room, profile %s", stashed, saved ? "saved" : "NOT saved");
}

bool stash_safely() {
    __try {
        stash();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The three things anything the interact key does has: let go, score, and do it.
void* __fastcall action_let_go(void* action, void*, unsigned) { return action; }

float __fastcall action_score(void*, void*, int) { return g_can_stash ? kStashScore : kNotHere; }

void __fastcall action_do(void*, void*, int) {
    if (g_can_stash && !stash_safely()) {
        g_can_stash = false;
        logf("stash: something went wrong stashing");
    }
}

void* g_action_methods[3] = {};
void** g_action = g_action_methods;

bool listed(char* hunt) {
    char* head = hunt + kHuntActionsHead;
    char* node = field<char*>(head, 0);
    for (unsigned i = 0; node && node != head && i < kMostActions; ++i, node = field<char*>(node, 0)) {
        if (node == g_node && field<void*>(node, kActionProvider) == &g_action) return true;
    }
    return false;
}

void update() {
    g_can_stash = false;
    char* hunt = current_hunt();
    char* player_profile = profile();
    if (!hunt || !player_profile) return;
    if (hunt != g_hunt || !listed(hunt)) {
        char* node = nullptr;
        reinterpret_cast<AddActionFn>(g_game + kAddAction)(hunt, nullptr, &node, &g_action, g_label, 0);
        g_hunt = hunt;
        g_node = node;
        logf("stash: %zu camps on this map, label %d", g_camps.size(), g_label);
    }
    float place[3], forward[3];
    if (!freelook_on_foot() || !free_hunt_read_view(place, forward) || camp_at(g_camps, place) < 0) return;
    g_bagged = bagged(player_profile);
    g_can_stash = g_bagged > 0;
}

bool update_safely() {
    __try {
        update();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

std::string stash_add_label(const std::string& text) {
    int label = -1;
    std::string with_label = add_label(text, &label);
    if (label >= 0) g_label = label;
    return with_label;
}

void stash_map_loading(const char* landscape) {
    g_camps.clear();
    if (!landscape) return;
    const std::string path = landscape;
    const size_t folder_end = path.rfind('/');
    if (folder_end == std::string::npos) return;
    const std::string folder = path.substr(0, folder_end + 1);
    std::string world, units;
    std::string units_path = folder + kUnitsFallback;
    if (layout_read_file((folder + kWorld).c_str(), &world)) {
        const size_t key = world.find(kUnitsKey);
        if (key != std::string::npos) {
            const size_t from = key + sizeof(kUnitsKey) - 1;
            units_path = world.substr(from, world.find('"', from) - from);
        }
    }
    if (!layout_read_file(units_path.c_str(), &units)) return;
    std::map<std::string, std::pair<bool, Box>> sizes;
    const BoxOf box_of = [&sizes](const std::string& model, Box* box) {
        auto known = sizes.find(model);
        if (known == sizes.end()) {
            std::string geometry;
            Box read = {};
            std::string path = model;
            if (path.size() > sizeof(kModelEnding) - 1) path.replace(path.size() - (sizeof(kModelEnding) - 1), std::string::npos, kGeometryEnding);
            const bool ok = layout_read_file(path.c_str(), &geometry) && box_in_geometry(geometry, &read);
            known = sizes.emplace(model, std::make_pair(ok, read)).first;
        }
        *box = known->second.second;
        return known->second.first;
    };
    g_camps = camps_in(units, box_of);
}

void stash_end_frame() {
    g_can_stash = false;
    if (!g_game || g_label < 0 || g_camps.empty() || !free_hunt_running()) return;
    if (update_safely()) return;
    g_game = nullptr;
    logf("stash: something went wrong, so there's no stashing");
}

void stash_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    g_action_methods[0] = reinterpret_cast<void*>(&action_let_go);
    g_action_methods[1] = reinterpret_cast<void*>(&action_score);
    g_action_methods[2] = reinterpret_cast<void*>(&action_do);
    logf("stash: ready");
}

int stash_test() {
    const std::string units =
        "SiiNunit\n{\n"
        "body_inst : units.objects_ins.a {\n body_desc: _nameless.truck\n placement: (&43ac900f, &409307d4, &c48eeb37) 1 (1; 0, 0, 0)\n}\n\n"
        "body_desc : _nameless.truck {\n model_file: \"/models/objects/ranger/object.ma\"\n look: army\n}\n\n"
        "body_inst : units.objects_ins.b {\n body_desc: _nameless.ruin\n placement: (10, 2, 20) 1 (1; 0, 0, 0)\n}\n\n"
        "body_desc : _nameless.ruin {\n model_file: \"/models/objects/ruinfoundation/object.ma\"\n}\n\n"
        "body_inst : units.objects_ins.c {\n body_desc: units.house\n placement: (-1200, 50.5, 300) 1 (1; 0, 0, 0)\n}\n\n"
        "body_desc : units.house {\n model_file: \"/models/objects/house2floor/object.ma\"\n}\n\n"
        "body_inst : units.objects_ins.d {\n body_desc: units.station\n placement: (500, 0, 500) 1 (&3f3504f3; 0, &3f3504f3, 0)\n}\n\n"
        "body_desc : units.station {\n model_file: \"/models/objects/ruinstation/object.ma\"\n}\n\n"
        "}\n";
    // A station's long way is its z, and turned a quarter round that runs along the world's x.
    const BoxOf box_of = [](const std::string& model, Box* box) {
        if (model.find("ruinstation") == std::string::npos) return false;
        *box = {{-4.34f, -0.43f, -16.28f}, {4.34f, 7.02f, 16.28f}};
        return true;
    };
    const std::vector<Camp> camps = camps_in(units, box_of);
    if (camps.size() != 3) return 1;                                                  // the truck, the house and the station, not the ruin
    if (fabsf(camps[0].place[0] - 345.13f) > 0.01f || fabsf(camps[0].place[2] + 1143.35f) > 0.01f) return 2;  // read from its bits
    if (camps[1].place[0] != -1200.0f || camps[1].place[1] != 50.5f || camps[1].place[2] != 300.0f) return 3;  // and as written
    const float near_truck[3] = {355.0f, 0.0f, -1140.0f};
    const float far_off[3] = {0.0f, 0.0f, 0.0f};
    if (camp_at(camps, near_truck) != 0 || camp_at(camps, far_off) != -1) return 4;
    if (fabsf(camps[2].turn[0] - 0.7071f) > 0.001f || fabsf(camps[2].turn[2] - 0.7071f) > 0.001f) return 10;  // its turn is read
    const float platform_end[3] = {524.0f, 0.0f, 500.0f};                             // 24 m along it, past where 15 m reached
    const float beside_it[3] = {500.0f, 0.0f, 516.0f};                                // and 16 m out from its side
    if (camp_at(camps, platform_end) != 2 || camp_at(camps, beside_it) != -1) return 11;
    Box read = {};
    std::string geometry(kGeometryBox + sizeof(Box), '\0');
    const Box station = {{-4.34f, -0.43f, -16.28f}, {4.34f, 7.02f, 16.28f}};
    memcpy(&geometry[kGeometryBox], &station, sizeof(station));
    if (!box_in_geometry(geometry, &read) || read.high[2] != 16.28f || box_in_geometry("short", &read)) return 12;
    if (camp_model("/models/objects/hcabin/object.ma") != true || camp_model("/models/objects/house") || camp_model("/models/animals/hut/x.ma")) return 5;
    int label = -1;
    const std::string txt = add_label("def template0 {\r\n}\r\n\r\ndef template1 {\r\n}\r\n", &label);
    if (label != 2 || txt.find("def template2 {") == std::string::npos || txt.find("[Activate: Stash]") == std::string::npos) return 6;
    add_label("nothing here", &label);
    if (label != -1) return 7;                                                        // no templates, no label
    if (stashed_text(1).find("1 animal stashed") == std::string::npos || stashed_text(3).find("3 animals stashed") == std::string::npos) return 8;
    if (action_score(nullptr, nullptr, 0) != kNotHere) return 9;                      // nothing to stash
    return 0;
}

}  // namespace longshot
