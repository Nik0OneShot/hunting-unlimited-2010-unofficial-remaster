#include "longshot.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kDefaults = 0x1e2020;      // what a new profile's free hunt starts with
constexpr uintptr_t kDefaultsFirst = 0xd0bf0;  // inside the mov that reads the first name
constexpr uintptr_t kDefaultsWalk = 0xd0bf7;   // inside the mov that starts walking the list
constexpr uintptr_t kOpeningCheck = 0x37a2;    // goes past the shot of an animal, unless it's a free hunt
constexpr uintptr_t kReadDangerousCall = 0x94f31;
constexpr uintptr_t kCreateAnimalsCall = 0x94f50;
constexpr uintptr_t kCreateAnimals = 0xbd600;
constexpr uintptr_t kGenerateAnimals = 0xbe4b0;
constexpr uintptr_t kNameFormat = 0x1a5b4c;    // how a generated animal is named
constexpr uintptr_t kNameFormatPush = 0xbe874;
constexpr uintptr_t kAddAnimal = 0xbd480;
constexpr uintptr_t kReadStartPoints = 0xd75e0;
constexpr uintptr_t kStartPointListClass = 0x19ce6c;
constexpr uintptr_t kFreeStartPoints = 0x259f0;
constexpr uintptr_t kCurrentHunt = 0x4f50;
constexpr uintptr_t kAnimalManager = 0xbab80;
constexpr uintptr_t kGameState = 0x1e9d68;
constexpr uintptr_t kProfileSection = 0xd07e0;
constexpr uintptr_t kLoadResource = 0x2d0870;
constexpr uintptr_t kHoldResource = 0x15e590;
constexpr uintptr_t kReleaseResource = 0x7b120;
constexpr uintptr_t kClosestHerd = 0xbe436;    // inside the mov of how close a herd may start
constexpr uintptr_t kStartingStress = 0xbeb85;  // inside the push of a generated animal's stress
constexpr uintptr_t kWeighNeedsCall = 0xb689d;
constexpr uintptr_t kWeighNeeds = 0xb66a0;
constexpr uintptr_t kMarkerSpotsCall = 0x25efe;
constexpr uintptr_t kMarkerSpots = 0x25ac0;
constexpr uintptr_t kAddMarkerSpot = 0x25970;
constexpr uintptr_t kOpeningSlot = 0x19b0d4;
constexpr uintptr_t kFloorAt = 0xd4540;
// A hunt has 11 goals at +0x10, a kind and a number each, and a list of bytes at +0x80 for each.
// One of kind 1 keeps a byte for every animal, by its place in the manager's list, and that's sized
// when the hunt starts (+0x92c9b). Animals brought in later are past the end of it, and a hit on one
// stopped the game with an index overflow at +0x8fb83. So the lists grow with the animals.
constexpr uintptr_t kHuntOf = 0x4f10;
constexpr uintptr_t kResizeBytes = 0x7e00;
constexpr size_t kHuntGoals = 0x10;
constexpr size_t kGoalSize = 8;
constexpr size_t kHuntGoalLists = 0x80;
constexpr size_t kGoalListSize = 0x10;
constexpr int kGoals = 11;
constexpr int kGoalForEachAnimal = 1;
// With every tag used, a hunt says its goals are accomplished, and to end it. In a free hunt the
// animals can be stashed instead, so that's what it says.
constexpr uintptr_t kAccomplishedText = 0x9069a;  // inside the mov of the message
constexpr uintptr_t kGameAccomplished = 0x1a1e1c;

// Slots that hold the engine's functions, once game.dll has loaded.
constexpr uintptr_t kGetValue = 0x19aaa8;
constexpr uintptr_t kGetBool = 0x19aa74;
constexpr uintptr_t kGetInt = 0x19ab10;
constexpr uintptr_t kGetPoint = 0x19a90c;
constexpr uintptr_t kSetPoint = 0x19aa5c;
constexpr uintptr_t kFindInPool = 0x19ab0c;
constexpr uintptr_t kMakePool = 0x19aa7c;
constexpr uintptr_t kClearPool = 0x19aa9c;
constexpr uintptr_t kCopyString = 0x19a1e0;
constexpr uintptr_t kRotate = 0x19a174;

// Pools of definitions, the things in them, and the animal manager.
constexpr size_t kPoolEntries = 0x50;
constexpr size_t kEntryNext = 4;
constexpr size_t kEntryObject = 8;
constexpr size_t kObjectChildren = 0xac;
constexpr size_t kManagerAnimals = 4;
constexpr size_t kManagerCount = 8;
constexpr size_t kUnitFlags = 6;
constexpr unsigned char kUnitAlive = 0x80;
constexpr size_t kAnimalName = 0x15c;
constexpr size_t kAnimalNameText = 0x160;
constexpr size_t kAnimalPosition = 0x338;
constexpr size_t kAnimalDead = 0xbe7;
constexpr size_t kAnimalBrain = 0x4f8;
constexpr size_t kBrainHunterFear = 0xdc;
constexpr size_t kBrainEnemyFear = 0xe0;
constexpr size_t kBrainStress = 0x124;
constexpr size_t kBrainClock = 0x25c;
constexpr size_t kHuntClock = 0x40;
constexpr size_t kHuntScene = 0xac;
constexpr size_t kHuntPlayer = 0x100;
constexpr size_t kMethodPosition = 0x3c;
constexpr size_t kMethodMoveTo = 0x44;
constexpr size_t kListItems = 4;
constexpr size_t kListCount = 8;
constexpr size_t kMethodInit = 0x54;
constexpr size_t kMethodStart = 0x58;
constexpr size_t kHuntView = 0x58;
constexpr size_t kViewPlace = 0x50;
constexpr size_t kPlacePosition = 0x20;
constexpr size_t kPlaceRotation = 0x2c;
constexpr size_t kStartPointPosition = 0x20;

constexpr char kFreeHuntBriefing[] = "@@mm_freehunt@@";
constexpr char kAccomplished[] = "@@g_lc_accomplished@@";
constexpr char kBagsFull[] =
    "<img src=/material/ui/ingame/x_ok.mat><br>Bags full! Stash your animals at a camp,<br>or press @@__key_ingamescore@@ to leave the hunt.";
constexpr char kGameDefinitions[] = "/def/game.def";
constexpr char kGameNameFormat[] = "%s_%s%.2i";

constexpr unsigned char kOpeningShown[] = {0x0F, 0x85, 0x07, 0x05, 0x00, 0x00};  // jne
constexpr unsigned char kOpeningSkipped[] = {0x90, 0xE9, 0x07, 0x05, 0x00, 0x00};  // nop, then jmp

struct Default {
    const char* name;
    const char* value;
};

// A slot's number is the animal's place in game.def, and its density is 1 low, 2 medium or 3 high.
// Setting last_map also skips the game's own first visit, which would pick a different loadout.
constexpr Default kNewProfile[] = {
    {"weapon00", "308_bolt|scope3_9"},
    {"weapon01", "375_magnum_revolver"},
    {"item00", "atv"},
    {"item01", "map"},
    {"item02", "rf_binocs"},
    {"item03", "cover_scent"},
    {"item04", "treestand"},
    {"item05", "tripod"},
    {"last_map", "2"},
    {"last_dangerous_animals", "1"},
    {"alberta_last_time", "9"},
    {"alberta_last_weather", "0"},
    {"alberta_last_slot00", "0"},
    {"alberta_last_density00", "3"},
    {"alberta_last_slot01", "1"},
    {"alberta_last_density01", "2"},
    {"alberta_last_slot02", "4"},
    {"alberta_last_density02", "2"},
    {"alberta_last_slot03", "5"},
    {"alberta_last_density03", "1"},
    {"alberta_last_slot04", "6"},
    {"alberta_last_density04", "1"},
    {"alberta_last_slot05", "3"},
    {"alberta_last_density05", "1"},
};

constexpr float kGameClosestHerd = 175.0f;
// Beyond what an animal can smell or hear, and about a four minute walk. The game counts up to
// 1000 m as near, and that's left alone: any further and they get too thin on the ground to find.
constexpr float kClosestHerdDistance = 450.0f;
constexpr uint32_t kGameStartingStress = 0x3E99999A;  // 0.3
constexpr float kThreat = 0.1f;             // fear below this isn't reacting to anything in particular

constexpr float kNearby = 1000.0f;          // animals further off than this don't count as being around
constexpr float kArriveNear = 500.0f;       // a herd starts no closer than this
constexpr float kArriveFar = 1000.0f;
constexpr float kInView = 0.5f;             // the cosine of how far off the view counts as in sight
constexpr double kLookSeconds = 10.0;
constexpr double kFirstHerdSeconds = 60.0;  // give the animals the hunt started with a chance
constexpr double kHerdGapSeconds = 30.0;
constexpr float kPreferWater = 0.5f;        // how often a herd arrives near water, when it can
constexpr int kMostNearby = 90;             // animals left far behind don't count, or none would arrive
constexpr int kMostInList = 320;            // the dead stay listed, so this stops a very long hunt growing
constexpr int kMostLoggedHerds = 200;

constexpr float kMarkerStep = 20.0f;        // about what the marker can be told apart by on the map
constexpr float kMarkerReach = 220.0f;      // covers the gaps in the widest grid of start points, 300 m
constexpr size_t kMarkerSpotSize = 0x28;
constexpr float kDeepestWade = 0.3f;
constexpr float kShoreStep = 15.0f;
constexpr int kShoreRings = 30;
constexpr int kShoreSpokes = 12;

struct Species {
    std::string name;
    int wanted = 0;
    int nearby = 0;
};

struct Spot {
    float position[3];
};

// One row of the free hunt's animals, as its generator reads them.
struct HerdSlot {
    const void* string_class;
    const char* species;
    unsigned length;
    int group;
    int last_animal;
};
static_assert(sizeof(HerdSlot) == 0x14, "the game's slots are 0x14 bytes");

struct StartPointList {
    const void* list_class;
    char** items;
    unsigned count;
    unsigned capacity;
};

struct Held {
    void* resource;
    void* contents;
};

// One place the map marker can go. The menu fills in the last three from the position.
struct MarkerSpot {
    float position[3];
    float angles[3];
    int index;
    float map_x;
    float map_y;
    bool flag;
};
static_assert(sizeof(MarkerSpot) == kMarkerSpotSize, "the menu's spots are 0x28 bytes");

// What the game works out about the ground at a place. Its own copy sits inside the player.
struct Floor {
    float surface;
    float depth;
    float ground;
    char rest[0x54];
};

using GetValueFn = const char*(__fastcall*)(void*, void*, const char*, unsigned);
using GetBoolFn = bool(__fastcall*)(void*, void*, const char*, unsigned);
using GetIntFn = int(__fastcall*)(void*, void*, const char*, unsigned);
using GetPointFn = float*(__fastcall*)(void*, void*, float*, const char*, unsigned);
using SetPointFn = void(__fastcall*)(void*, void*, const char*, const float*, unsigned);
using FindInPoolFn = char*(__fastcall*)(void*, void*, const char*, unsigned, bool);
using MakePoolFn = void*(__fastcall*)(void*, void*, bool);
using ClearPoolFn = void(__fastcall*)(void*, void*);
using CopyStringFn = bool(__fastcall*)(void*, void*, const char* const*, unsigned, unsigned, unsigned);
using RotateFn = float*(__fastcall*)(const void*, void*, float*, const float*);
using SectionFn = void*(__fastcall*)(void*, void*, const char* const*);
using CreateAnimalsFn = void(__fastcall*)(void*, void*, void*, void*);
using GenerateFn = void(__cdecl*)(void*, void*, const float*, HerdSlot*, unsigned, const char* const*, int, void*);
using AddAnimalFn = unsigned(__fastcall*)(void*, void*, const char* const*);
using InitAnimalFn = bool(__fastcall*)(void*, void*, unsigned, void*, void*, void*, void*);
using StartAnimalFn = void(__fastcall*)(void*, void*);
using ReadStartPointsFn = bool(__cdecl*)(StartPointList*, const char* const*);
using FreeStartPointsFn = void(__fastcall*)(StartPointList*, void*);
using CurrentHuntFn = char*(__cdecl*)();
using ManagerFn = char*(__cdecl*)();
using ObjectFn = char*(__fastcall*)(void*, void*);
using LoadResourceFn = void*(__cdecl*)(int, const char* const*, int, int);
using HoldResourceFn = void*(__fastcall*)(Held*, void*, void*);
using ReleaseResourceFn = void(__fastcall*)(Held*, void*);
using WeighNeedsFn = void(__fastcall*)(char*, void*);
using MarkerSpotsFn = bool(__cdecl*)(char*, const void*);
using AddMarkerSpotFn = void*(__fastcall*)(char*, void*, const MarkerSpot*);
using OpeningFn = void(__fastcall*)(char*, void*);
using FloorAtFn = bool(__fastcall*)(void*, void*, Floor*, const float*);
using PositionFn = const float*(__fastcall*)(void*, void*);
using MoveToFn = void(__fastcall*)(void*, void*, const float*);
using HuntOfFn = char*(__fastcall*)(char*, void*);
using ResizeBytesFn = bool(__fastcall*)(char*, void*, unsigned);

char* g_game = nullptr;
bool g_ready = false;
bool g_stopped = false;
bool g_free_hunt = false;
std::vector<Default> g_defaults;
std::string g_area;
int g_hour = 8;
std::vector<Species> g_species;
std::vector<Spot> g_spots;
Held g_definitions = {};
OpeningFn g_real_opening = nullptr;
alignas(16) char g_pool[0x200];
bool g_pool_made = false;
char g_name_format[32] = {};
int g_herds = 0;
double g_next_look = 0.0;
double g_next_herd = 0.0;
char g_accomplished[256] = {};
unsigned g_goal_lists_grown = 0;

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

template <typename Fn>
Fn engine(uintptr_t slot) {
    return *reinterpret_cast<Fn*>(g_game + slot);
}

template <typename Fn>
Fn method(void* object, size_t offset) {
    return reinterpret_cast<Fn>(field<void**>(object, 0)[offset / sizeof(void*)]);
}

const char* text_of(void* object, const char* key) {
    const char* value = engine<GetValueFn>(kGetValue)(object, nullptr, key, static_cast<unsigned>(-1));
    return value ? value : "";
}

// The menu only writes this into the hunt when its box is clicked, so a box that's already ticked
// when the menu opens does nothing. What's saved in the profile is what the player sees.
bool saved_dangerous(void* general, const char* key, unsigned index) {
    char** state = *reinterpret_cast<char***>(g_game + kGameState);
    const char* section_name = "free_hunt";
    void* section = reinterpret_cast<SectionFn>(g_game + kProfileSection)(state[1], nullptr, &section_name);
    if (!section) return engine<GetBoolFn>(kGetBool)(general, nullptr, key, index);
    return engine<GetBoolFn>(kGetBool)(section, nullptr, "last_dangerous_animals", static_cast<unsigned>(-1));
}

bool __fastcall hook_read_dangerous(void* general, void* unused, const char* key, unsigned index) {
    g_free_hunt = !strcmp(text_of(general, "briefing"), kFreeHuntBriefing);
    strcpy_s(g_accomplished, g_free_hunt ? kBagsFull : kAccomplished);
    if (!g_free_hunt) return engine<GetBoolFn>(kGetBool)(general, unused, key, index);
    g_area = text_of(general, "area");
    g_hour = engine<GetIntFn>(kGetInt)(general, nullptr, "time", static_cast<unsigned>(-1));
    const bool dangerous = saved_dangerous(general, key, index);
    logf("free hunt: %s at %d:00, dangerous animals %s", g_area.c_str(), g_hour, dangerous ? "on" : "off");
    return dangerous;
}

void count_wanted(void* animals) {
    g_species.clear();
    for (char* entry = field<char*>(animals, kPoolEntries); entry; entry = field<char*>(entry, kEntryNext)) {
        const char* name = text_of(field<void*>(entry, kEntryObject), "species");
        Species* found = nullptr;
        for (Species& species : g_species) {
            if (species.name == name) found = &species;
        }
        if (!found) {
            g_species.push_back(Species());
            found = &g_species.back();
            found->name = name;
        }
        ++found->wanted;
    }
}

void read_spots() {
    g_spots.clear();
    StartPointList list = {g_game + kStartPointListClass, nullptr, 0, 0};
    const char* area = g_area.c_str();
    if (!reinterpret_cast<ReadStartPointsFn>(g_game + kReadStartPoints)(&list, &area)) return;
    for (unsigned i = 0; i < list.count; ++i) {
        Spot spot;
        memcpy(spot.position, list.items[i] + kStartPointPosition, sizeof(spot.position));
        g_spots.push_back(spot);
    }
    reinterpret_cast<FreeStartPointsFn>(g_game + kFreeStartPoints)(&list, nullptr);
}

// Anything still holding these when the game quits stops its memory manager shutting down, so
// they're only held while a herd is being made.
char* hold_definitions() {
    const char* path = kGameDefinitions;
    void* resource = (*reinterpret_cast<LoadResourceFn*>(g_game + kLoadResource))(1, &path, 0, 0);
    if (resource) reinterpret_cast<HoldResourceFn>(g_game + kHoldResource)(&g_definitions, nullptr, resource);
    return static_cast<char*>(g_definitions.contents);
}

void release_definitions() {
    if (g_definitions.resource) reinterpret_cast<ReleaseResourceFn>(g_game + kReleaseResource)(&g_definitions, nullptr);
    g_definitions = {};
}

void __fastcall hook_create_animals(void* manager, void* unused, void* animals, void* definitions) {
    if (g_free_hunt) wildlife_settle_herds(animals, static_cast<char*>(definitions));
    reinterpret_cast<CreateAnimalsFn>(g_game + kCreateAnimals)(manager, unused, animals, definitions);
    lights_hunt_started();
    g_species.clear();
    g_spots.clear();
    if (!g_free_hunt) {
        wildlife_hunt_ended();
        daytime_hunt_ended();
        return;
    }
    wildlife_hunt_started(g_hour);
    daytime_hunt_started();
    count_wanted(animals);
    read_spots();
    if (!g_pool_made) {
        engine<MakePoolFn>(kMakePool)(g_pool, nullptr, false);
        g_pool_made = true;
    }
    const double now = now_seconds();
    g_next_look = now + kLookSeconds;
    g_next_herd = now + kFirstHerdSeconds;
    int wanted = 0;
    for (const Species& species : g_species) wanted += species.wanted;
    logf("free hunt: keeping %d animals of %d kinds around, %d places to arrive at", wanted,
         static_cast<int>(g_species.size()), static_cast<int>(g_spots.size()));
}

// Names are "wtail_buck00", or that with "sp12_" in front for an animal brought in later.
int species_of(const char* name) {
    if (name[0] == 's' && name[1] == 'p' && name[2] >= '0' && name[2] <= '9') {
        const char* after = strchr(name, '_');
        if (after) name = after + 1;
    }
    for (size_t i = 0; i < g_species.size(); ++i) {
        const std::string& species = g_species[i].name;
        if (!strncmp(name, species.c_str(), species.size()) && name[species.size()] == '_') {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool add_to_hunt(char* manager, char* definition, char* definitions, float now) {
    char* all_species = engine<FindInPoolFn>(kFindInPool)(definitions, nullptr, "species", static_cast<unsigned>(-1), false);
    if (!all_species) return false;
    char* species = engine<FindInPoolFn>(kFindInPool)(field<void*>(all_species, kObjectChildren), nullptr,
                                                      text_of(definition, "species"), static_cast<unsigned>(-1), false);
    if (!species) return false;
    char* type = engine<FindInPoolFn>(kFindInPool)(field<void*>(species, kObjectChildren), nullptr,
                                                   text_of(definition, "type"), static_cast<unsigned>(-1), false);
    if (!type) return false;

    const char* control = text_of(type, "type");
    const unsigned index = reinterpret_cast<AddAnimalFn>(g_game + kAddAnimal)(manager, nullptr, &control);
    if (index >= field<unsigned>(manager, kManagerCount)) return false;
    char* animal = field<char**>(manager, kManagerAnimals)[index];

    const char* name = definition;
    engine<CopyStringFn>(kCopyString)(animal + kAnimalName, nullptr, &name, 0, 0, static_cast<unsigned>(-1));
    if (!method<InitAnimalFn>(animal, kMethodInit)(animal, nullptr, index, definition, species, type, definitions)) {
        field<bool>(animal, kAnimalDead) = true;
        return false;
    }
    // A new animal's clock is at 0. That's right as a hunt loads, but later on its first update
    // would count the whole hunt so far as time passing.
    field<float>(animal, kAnimalBrain + kBrainClock) = now;
    method<StartAnimalFn>(animal, kMethodStart)(animal, nullptr);
    return true;
}

// The game writes a whole herd with natural sizes, antlers and moods, around a place of its own.
// That place might be in plain sight, so the herd is moved, keeping its spacing.
int bring_in_herd(char* manager, int species, int group, const float* hunter, const float* arrive) {
    char* hunt = reinterpret_cast<CurrentHuntFn>(g_game + kCurrentHunt)();
    char* definitions = hunt ? hold_definitions() : nullptr;
    if (!definitions) return 0;
    const float now = field<float>(hunt, kHuntClock);
    engine<ClearPoolFn>(kClearPool)(g_pool, nullptr);

    snprintf(g_name_format, sizeof(g_name_format), "sp%d_%s", ++g_herds, kGameNameFormat);
    HerdSlot slot = {nullptr, g_species[species].name.c_str(), 0, group, 0};
    const char* area = g_area.c_str();
    wildlife_whole_herd(true);
    reinterpret_cast<GenerateFn>(g_game + kGenerateAnimals)(g_pool, definitions, hunter, &slot, 1, &area, g_hour, nullptr);
    wildlife_whole_herd(false);
    strcpy_s(g_name_format, kGameNameFormat);
    wildlife_settle_herds(g_pool, definitions);

    float centre[3] = {};
    bool have_centre = false;
    int added = 0;
    for (char* entry = field<char*>(g_pool, kPoolEntries); entry; entry = field<char*>(entry, kEntryNext)) {
        char* definition = field<char*>(entry, kEntryObject);
        float position[3];
        engine<GetPointFn>(kGetPoint)(definition, nullptr, position, "position", static_cast<unsigned>(-1));
        if (!have_centre) {
            memcpy(centre, position, sizeof(centre));
            have_centre = true;
        }
        const float moved[3] = {arrive[0] + position[0] - centre[0], arrive[1],
                                arrive[2] + position[2] - centre[2]};
        engine<SetPointFn>(kSetPoint)(definition, nullptr, "position", moved, static_cast<unsigned>(-1));
        if (add_to_hunt(manager, definition, definitions, now)) ++added;
    }
    engine<ClearPoolFn>(kClearPool)(g_pool, nullptr);
    release_definitions();
    return added;
}

// With nothing better to do, an animal's leftover stress alone counts as danger, and it gallops
// away from wherever the hunter is, however far off. So stress only counts while it's reacting to
// something. The fear itself still fades slowly, so one that has been scared stays wary.
void __fastcall hook_weigh_needs(char* brain, void* unused) {
    const auto weigh = reinterpret_cast<WeighNeedsFn>(g_game + kWeighNeeds);
    if (!g_free_hunt) {
        weigh(brain, unused);
        return;
    }
    WildNeeds needs;
    wildlife_before_needs(brain, &needs);
    const bool afraid = field<float>(brain, kBrainHunterFear) >= kThreat || field<float>(brain, kBrainEnemyFear) >= kThreat;
    if (!g_cfg.free_hunt_calm_animals || afraid) {
        weigh(brain, unused);
    } else {
        float& stress = field<float>(brain, kBrainStress);
        const float held = stress;
        stress = 0.0f;
        weigh(brain, unused);
        stress += held;
    }
    wildlife_after_needs(brain, needs);
}

// The spots a marker can sit on beyond the start points themselves, filling in around them. A
// spot takes the height of the nearest start point, and the hunter is put on the ground from there.
void extra_marker_spots(const Spot* known, int count, std::vector<Spot>* extra) {
    extra->clear();
    if (count <= 0) return;
    float lowest_x = known[0].position[0], highest_x = known[0].position[0];
    float lowest_z = known[0].position[2], highest_z = known[0].position[2];
    for (int i = 1; i < count; ++i) {
        lowest_x = fminf(lowest_x, known[i].position[0]);
        highest_x = fmaxf(highest_x, known[i].position[0]);
        lowest_z = fminf(lowest_z, known[i].position[2]);
        highest_z = fmaxf(highest_z, known[i].position[2]);
    }
    for (float x = lowest_x - kMarkerReach; x <= highest_x + kMarkerReach; x += kMarkerStep) {
        for (float z = lowest_z - kMarkerReach; z <= highest_z + kMarkerReach; z += kMarkerStep) {
            float closest = kMarkerReach * kMarkerReach;
            int nearest = -1;
            for (int i = 0; i < count; ++i) {
                const float dx = known[i].position[0] - x;
                const float dz = known[i].position[2] - z;
                const float squared = dx * dx + dz * dz;
                if (squared <= closest) {
                    closest = squared;
                    nearest = i;
                }
            }
            if (nearest < 0) continue;
            Spot spot = {{x, known[nearest].position[1], z}};
            extra->push_back(spot);
        }
    }
}

bool __cdecl hook_marker_spots(char* list, const void* area) {
    const bool found = reinterpret_cast<MarkerSpotsFn>(g_game + kMarkerSpots)(list, area);
    if (!found || !g_cfg.free_hunt_any_marker) return found;

    const unsigned count = field<unsigned>(list, kListCount);
    const char* items = field<const char*>(list, kListItems);
    std::vector<Spot> known;
    for (unsigned i = 0; i < count; ++i) {
        Spot spot;
        memcpy(spot.position, items + i * kMarkerSpotSize, sizeof(spot.position));
        known.push_back(spot);
    }
    std::vector<Spot> extra;
    extra_marker_spots(known.data(), static_cast<int>(known.size()), &extra);

    const auto add = reinterpret_cast<AddMarkerSpotFn>(g_game + kAddMarkerSpot);
    for (const Spot& spot : extra) {
        MarkerSpot added = {};
        memcpy(added.position, spot.position, sizeof(added.position));
        added.index = -1;
        add(list, nullptr, &added);
    }
    logf("free hunt: the marker can use %u start points and %d more spots", count, static_cast<int>(extra.size()));
    return found;
}

// Steps outward from the marker in rings, nearest first. False once the rings run out.
bool shore_search(int step, float* x, float* z) {
    if (step < 0 || step >= kShoreRings * kShoreSpokes) return false;
    const int ring = step / kShoreSpokes + 1;
    const float angle = (step % kShoreSpokes) * 6.2831853f / kShoreSpokes;
    *x = sinf(angle) * ring * kShoreStep;
    *z = cosf(angle) * ring * kShoreStep;
    return true;
}

bool dry_ground(void* scene, const float* place) {
    Floor floor = {};
    const bool found = reinterpret_cast<FloorAtFn>(g_game + kFloorAt)(scene, nullptr, &floor, place);
    return found && floor.depth < kDeepestWade;
}

// The map can't say what's a lake, because the world isn't loaded in the menu. It is by now.
void keep_out_of_water(char* hunt) {
    void* player = field<void*>(hunt, kHuntPlayer);
    void* scene = field<void*>(hunt, kHuntScene);
    if (!player || !scene) return;
    float start[3];
    memcpy(start, method<PositionFn>(player, kMethodPosition)(player, nullptr), sizeof(start));
    if (dry_ground(scene, start)) return;

    float x = 0.0f, z = 0.0f;
    for (int step = 0; shore_search(step, &x, &z); ++step) {
        const float place[3] = {start[0] + x, start[1], start[2] + z};
        if (!dry_ground(scene, place)) continue;
        method<MoveToFn>(player, kMethodMoveTo)(player, nullptr, place);
        logf("free hunt: the marker was in water, so the hunt starts %.0f m away on dry ground", sqrtf(x * x + z * z));
        return;
    }
    logf("free hunt: no dry ground found near the marker");
}

bool keep_out_of_water_safely(char* hunt) {
    __try {
        keep_out_of_water(hunt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// This runs as the hunt starts, and puts the hunter on the ground below wherever they are.
void __fastcall hook_opening(char* hunt, void* unused) {
    if (g_free_hunt && !keep_out_of_water_safely(hunt)) logf("free hunt: couldn't check the marker for water");
    g_real_opening(hunt, unused);
}

char* view_place() {
    char* hunt = reinterpret_cast<CurrentHuntFn>(g_game + kCurrentHunt)();
    if (!hunt) return nullptr;
    void* view = method<ObjectFn>(hunt, kHuntView)(hunt, nullptr);
    return view ? method<ObjectFn>(view, kViewPlace)(view, nullptr) : nullptr;
}

bool read_view(float* position, float* forward) {
    char* place = view_place();
    if (!place) return false;
    memcpy(position, place + kPlacePosition, 3 * sizeof(float));
    const float ahead[3] = {0.0f, 0.0f, -1.0f};
    engine<RotateFn>(kRotate)(place + kPlaceRotation, nullptr, forward, ahead);
    return true;
}

// How many are alive, with each species' nearby count filled in.
int count_alive(char* manager, const float* hunter) {
    for (Species& species : g_species) species.nearby = 0;
    int alive = 0;
    const unsigned count = field<unsigned>(manager, kManagerCount);
    char** animals = field<char**>(manager, kManagerAnimals);
    for (unsigned i = 0; i < count; ++i) {
        char* animal = animals[i];
        if (!animal || !(field<unsigned char>(animal, kUnitFlags) & kUnitAlive)) continue;
        if (field<bool>(animal, kAnimalDead)) continue;
        ++alive;
        const float* position = reinterpret_cast<float*>(animal + kAnimalPosition);
        const float x = position[0] - hunter[0];
        const float z = position[2] - hunter[2];
        if (x * x + z * z > kNearby * kNearby) continue;
        const char* name = field<const char*>(animal, kAnimalNameText);
        const int species = name ? species_of(name) : -1;
        if (species >= 0) ++g_species[species].nearby;
    }
    return alive;
}

int pick_species(const Species* species, int count) {
    int best = -1;
    float best_share = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (species[i].wanted <= 0 || species[i].nearby >= species[i].wanted) continue;
        const float short_of = static_cast<float>(species[i].wanted - species[i].nearby) / species[i].wanted;
        if (short_of > best_share) {
            best_share = short_of;
            best = i;
        }
    }
    return best;
}

// A place in the ring around the hunter, out of view if one exists. `roll` is between 0 and 1, and
// `liked` marks places worth arriving at more often, or is null.
int pick_spot(const Spot* spots, int count, const float* hunter, const float* forward, float roll, const std::vector<char>* liked) {
    std::vector<int> hidden;
    std::vector<int> ring;
    const float ahead = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
    for (int i = 0; i < count; ++i) {
        const float x = spots[i].position[0] - hunter[0];
        const float z = spots[i].position[2] - hunter[2];
        const float distance = sqrtf(x * x + z * z);
        if (distance < kArriveNear || distance > kArriveFar) continue;
        ring.push_back(i);
        const float towards = ahead > 0.0f ? (x * forward[0] + z * forward[2]) / (distance * ahead) : 0.0f;
        if (towards < kInView) hidden.push_back(i);
    }
    const std::vector<int>& choices = hidden.empty() ? ring : hidden;
    if (choices.empty()) return -1;
    if (liked && roll < kPreferWater) {
        std::vector<int> better;
        for (const int choice : choices) {
            if ((*liked)[choice]) better.push_back(choice);
        }
        if (!better.empty()) {
            const int pick = static_cast<int>(roll / kPreferWater * better.size());
            return better[pick >= static_cast<int>(better.size()) ? better.size() - 1 : pick];
        }
    }
    const int pick = static_cast<int>(roll * choices.size());
    return choices[pick >= static_cast<int>(choices.size()) ? choices.size() - 1 : pick];
}

// The new places in a goal's list are zero, as the hunt starts them.
bool grow_goal_list(char* list, unsigned animals) {
    const unsigned had = field<unsigned>(list, kListCount);
    if (had >= animals) return false;
    if (!reinterpret_cast<ResizeBytesFn>(g_game + kResizeBytes)(list, nullptr, animals)) return false;
    if (field<unsigned>(list, kListCount) != animals) return false;
    memset(field<char*>(list, kListItems) + had, 0, animals - had);
    return true;
}

void grow_goal_lists(char* manager) {
    char* game = reinterpret_cast<CurrentHuntFn>(g_game + kCurrentHunt)();
    char* hunt = game ? reinterpret_cast<HuntOfFn>(g_game + kHuntOf)(game, nullptr) : nullptr;
    if (!hunt) return;
    const unsigned animals = field<unsigned>(manager, kManagerCount);
    for (int goal = 0; goal < kGoals; ++goal) {
        if (field<int>(hunt, kHuntGoals + goal * kGoalSize) != kGoalForEachAnimal) continue;
        if (grow_goal_list(hunt + kHuntGoalLists + goal * kGoalListSize, animals)) ++g_goal_lists_grown;
    }
}

void look_around(double now) {
    char* manager = reinterpret_cast<ManagerFn>(g_game + kAnimalManager)();
    float hunter[3];
    float forward[3];
    if (!manager || !read_view(hunter, forward)) return;

    const int alive = count_alive(manager, hunter);
    int nearby = 0;
    for (const Species& kind : g_species) nearby += kind.nearby;
    if (now < g_next_herd || nearby >= kMostNearby) return;
    if (field<unsigned>(manager, kManagerCount) >= static_cast<unsigned>(kMostInList)) return;

    const int species = pick_species(g_species.data(), static_cast<int>(g_species.size()));
    if (species < 0) return;
    const float roll = static_cast<float>(rand()) / (static_cast<float>(RAND_MAX) + 1.0f);
    std::vector<char> near_water(g_spots.size());
    bool any_near_water = false;
    for (size_t i = 0; i < g_spots.size(); ++i) {
        near_water[i] = wildlife_near_water(g_spots[i].position);
        any_near_water = any_near_water || near_water[i];
    }
    const int spot = pick_spot(g_spots.data(), static_cast<int>(g_spots.size()), hunter, forward, roll, any_near_water ? &near_water : nullptr);
    if (spot < 0) return;
    float arrive[3];
    memcpy(arrive, g_spots[spot].position, sizeof(arrive));
    const bool climbed = wildlife_high_ground(g_species[species].name.c_str(), hunter, kArriveNear, arrive);

    const int size = wildlife_herd_size(g_species[species].name.c_str(), g_species[species].wanted - g_species[species].nearby);
    if (size <= 0) return;
    const double started = now_seconds();
    const int added = bring_in_herd(manager, species, wildlife_herd_group(size), hunter, arrive);
    g_next_herd = now + kHerdGapSeconds;
    grow_goal_lists(manager);
    if (g_herds <= kMostLoggedHerds) {
        const float x = arrive[0] - hunter[0];
        const float z = arrive[2] - hunter[2];
        logf("free hunt: %d %s arrived %.0f m away%s in %.1f ms, %d of %d around, %d alive, goal lists grown %u times", added,
             g_species[species].name.c_str(), sqrtf(x * x + z * z), climbed ? ", up high" : "", (now_seconds() - started) * 1000.0,
             g_species[species].nearby, g_species[species].wanted, alive + added, g_goal_lists_grown);
    }
}

// Bringing animals in touches a lot of the game's data while a hunt runs. If any of it goes wrong
// the hunt is worth more than the next herd, so this stops rather than risk it again.
bool look_around_safely(double now) {
    __try {
        look_around(now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void apply_opening() {
    const bool show = g_cfg.free_hunt_opening;
    const unsigned char* wanted = show ? kOpeningShown : kOpeningSkipped;
    const unsigned char* other = show ? kOpeningSkipped : kOpeningShown;
    if (memcmp(g_game + kOpeningCheck, wanted, sizeof(kOpeningShown)) != 0) {
        patch_bytes(g_game + kOpeningCheck, other, wanted, sizeof(kOpeningShown));
    }
}

bool install_defaults() {
    auto* game = reinterpret_cast<Default*>(g_game + kDefaults);
    g_defaults.clear();
    for (; game->name; ++game) {
        bool changed = false;
        for (const Default& ours : kNewProfile) changed = changed || !strcmp(ours.name, game->name);
        if (!changed) g_defaults.push_back(*game);
    }
    for (const Default& ours : kNewProfile) g_defaults.push_back(ours);
    g_defaults.push_back({nullptr, nullptr});

    const void* was = g_game + kDefaults;
    const void* now = g_defaults.data();
    return patch_bytes(g_game + kDefaultsFirst, &was, &now, sizeof(now)) &&
           patch_bytes(g_game + kDefaultsWalk, &was, &now, sizeof(now));
}

bool set_float(uintptr_t at, float was, float now) {
    return patch_bytes(g_game + at, &was, &now, sizeof(now));
}

bool hook_dangerous() {
    void* slot = g_game + kGetBool;
    unsigned char expected[6] = {0xFF, 0x15};
    memcpy(expected + 2, &slot, sizeof(slot));
    unsigned char replacement[6] = {0xE8};
    const auto hook = reinterpret_cast<char*>(&hook_read_dangerous);
    const int32_t relative = static_cast<int32_t>(hook - (g_game + kReadDangerousCall + 5));
    memcpy(replacement + 1, &relative, sizeof(relative));
    replacement[5] = 0x90;
    return patch_bytes(g_game + kReadDangerousCall, expected, replacement, sizeof(expected));
}

}  // namespace

void free_hunt_settings_changed() {
    if (g_ready) apply_opening();
}

bool free_hunt_running() { return g_free_hunt && view_hunt_running(); }

bool free_hunt_read_view(float* position, float* forward) { return g_game && read_view(position, forward); }

bool free_hunt_water_depth(const float* place, float* depth, float* ground) {
    if (!g_game) return false;
    char* hunt = reinterpret_cast<CurrentHuntFn>(g_game + kCurrentHunt)();
    void* scene = hunt ? field<void*>(hunt, kHuntScene) : nullptr;
    Floor floor = {};
    if (!scene || !reinterpret_cast<FloorAtFn>(g_game + kFloorAt)(scene, nullptr, &floor, place)) return false;
    *depth = floor.depth;
    if (ground) *ground = floor.ground;
    return true;
}

void free_hunt_end_frame() {
    daytime_end_frame();
    wildlife_end_frame();
    stash_end_frame();
    if (!g_ready || g_stopped || !g_cfg.free_hunt_animals || g_species.empty()) return;
    if (!view_hunt_running()) return;
    const double now = now_seconds();
    if (now < g_next_look) return;
    g_next_look = now + kLookSeconds;
    if (look_around_safely(now)) return;
    g_stopped = true;
    release_definitions();
    logf("free hunt: something went wrong bringing in animals, so no more will arrive");
}

void free_hunt_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    strcpy_s(g_name_format, kGameNameFormat);

    const void* format_was = g_game + kNameFormat;
    const void* format_now = g_name_format;
    const bool names = patch_bytes(g_game + kNameFormatPush, &format_was, &format_now, sizeof(format_now));
    strcpy_s(g_accomplished, kAccomplished);
    const void* accomplished_was = g_game + kGameAccomplished;
    const void* accomplished_now = g_accomplished;
    const bool bags = patch_bytes(g_game + kAccomplishedText, &accomplished_was, &accomplished_now, sizeof(accomplished_now));
    const bool defaults = install_defaults();
    const bool dangerous = hook_dangerous();
    const bool animals = patch_call(g_game + kCreateAnimalsCall, g_game + kCreateAnimals,
                                    reinterpret_cast<const void*>(&hook_create_animals));
    logf("free hunt: defaults=%d dangerous=%d animals=%d names=%d bags=%d", defaults, dangerous, animals, names, bags);

    const bool closest = set_float(kClosestHerd, kGameClosestHerd, kClosestHerdDistance);
    const uint32_t calm = 0;
    const bool stress = patch_bytes(g_game + kStartingStress, &kGameStartingStress, &calm, sizeof(calm));
    const bool needs = patch_call(g_game + kWeighNeedsCall, g_game + kWeighNeeds,
                                  reinterpret_cast<const void*>(&hook_weigh_needs));
    const bool spots = patch_call(g_game + kMarkerSpotsCall, g_game + kMarkerSpots,
                                  reinterpret_cast<const void*>(&hook_marker_spots));
    const bool opening = vtable_patch(reinterpret_cast<void**>(g_game + kOpeningSlot),
                                      reinterpret_cast<void*>(&hook_opening),
                                      reinterpret_cast<void**>(&g_real_opening));
    logf("free hunt: closest=%d stress=%d needs=%d spots=%d opening=%d", closest, stress, needs, spots, opening);
    g_stopped = !(names && dangerous && animals);
    g_ready = true;
    apply_opening();
}

int free_hunt_test() {
    Species species[3];
    species[0].wanted = 5;
    species[0].nearby = 4;
    species[1].wanted = 3;
    species[1].nearby = 0;
    species[2].wanted = 2;
    species[2].nearby = 2;
    if (pick_species(species, 3) != 1) return 1;
    species[1].nearby = 3;
    if (pick_species(species, 3) != 0) return 2;
    species[0].nearby = 5;
    if (pick_species(species, 3) != -1) return 3;

    const float hunter[3] = {0.0f, 0.0f, 0.0f};
    const float forward[3] = {0.0f, 0.0f, -1.0f};
    const Spot spots[] = {
        {{0.0f, 0.0f, 300.0f}},    // too close
        {{0.0f, 0.0f, -800.0f}},   // in plain sight
        {{0.0f, 0.0f, 800.0f}},    // behind
        {{3000.0f, 0.0f, 0.0f}},   // too far
    };
    if (pick_spot(spots, 4, hunter, forward, 0.0f, nullptr) != 2) return 7;
    if (pick_spot(spots, 4, hunter, forward, 0.999f, nullptr) != 2) return 8;
    if (pick_spot(spots, 2, hunter, forward, 0.5f, nullptr) != 1) return 9;  // only in sight, so it's that or nothing
    if (pick_spot(spots, 1, hunter, forward, 0.5f, nullptr) != -1) return 10;
    const Spot several[] = {
        {{0.0f, 0.0f, 700.0f}},
        {{0.0f, 0.0f, 800.0f}},
        {{0.0f, 0.0f, 900.0f}},
    };
    const std::vector<char> by_water = {0, 0, 1};
    if (pick_spot(several, 3, hunter, forward, 0.1f, &by_water) != 2) return 4;    // a low roll goes to the water
    if (pick_spot(several, 3, hunter, forward, 0.9f, &by_water) != 2) return 5;    // and a high one is any of them
    if (pick_spot(several, 3, hunter, forward, 0.6f, &by_water) != 1) return 6;

    g_species.clear();
    Species deer;
    deer.name = "wtail";
    g_species.push_back(deer);
    Species goat;
    goat.name = "goat";
    g_species.push_back(goat);
    const bool names = species_of("wtail_buck00") == 0 && species_of("goat_doe01") == 1 &&
                       species_of("sp12_goat_buck00") == 1 && species_of("wolf_buck00") == -1;
    g_species.clear();
    if (!names) return 11;

    const Spot grid[] = {{{0.0f, 10.0f, 0.0f}}, {{200.0f, 30.0f, 0.0f}}};
    std::vector<Spot> extra;
    extra_marker_spots(grid, 2, &extra);
    if (extra.size() < 400) return 12;
    bool heights = true;
    bool in_reach = true;
    bool between = false;
    for (const Spot& spot : extra) {
        const float x = spot.position[0];
        const float z = spot.position[2];
        heights = heights && spot.position[1] == (x < 100.0f ? 10.0f : 30.0f);
        const float to_first = sqrtf(x * x + z * z);
        const float to_second = sqrtf((x - 200.0f) * (x - 200.0f) + z * z);
        in_reach = in_reach && fminf(to_first, to_second) <= kMarkerReach;
        between = between || (fabsf(x - 100.0f) <= kMarkerStep && fabsf(z) <= kMarkerStep);
    }
    if (!heights) return 13;
    if (!in_reach) return 14;
    if (!between) return 15;
    extra_marker_spots(grid, 0, &extra);
    if (!extra.empty()) return 16;

    float x = 0.0f, z = 0.0f;
    if (!shore_search(0, &x, &z) || fabsf(sqrtf(x * x + z * z) - kShoreStep) > 0.01f) return 17;
    if (!shore_search(kShoreSpokes, &x, &z) || fabsf(sqrtf(x * x + z * z) - 2.0f * kShoreStep) > 0.01f) return 18;
    if (shore_search(kShoreRings * kShoreSpokes, &x, &z)) return 19;

    return 0;
}

}  // namespace longshot
