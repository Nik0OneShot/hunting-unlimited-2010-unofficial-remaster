#include "longshot.h"

#include <cmath>
#include <cstring>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kCreateContactCall = 0x12765d;  // the only place contacts become joints
constexpr uintptr_t kCreateContact = 0x10b7d0;      // dJointCreateContact
constexpr uintptr_t kAvatarListenerVtable = 0x1b1c6c;
constexpr uintptr_t kWalkSpeed = 0x1350f;   // inside the mov that picks the walking speed
constexpr uintptr_t kMoveShares = 0x19c214;  // backward, left and right, as a share of forward
constexpr uintptr_t kStepRates = 0x19c1ec;   // how fast the head bobs and steps land: forward, backward
constexpr uintptr_t kFootstepCall = 0xcd86;
constexpr uintptr_t kFootstep = 0xc670;
constexpr uintptr_t kSetHearingCall = 0xbee5;
constexpr uintptr_t kSetHearing = 0xb8fb0;
constexpr uintptr_t kCurrentHunt = 0x4f50;
constexpr uintptr_t kHuntPlayer = 0x2b30;
constexpr uintptr_t kHunterTable = 0x19bb70;
constexpr uintptr_t kPlayerRunning = 0xba50;
constexpr uintptr_t kVehicleVisorClass = 0x19c5c8;

// The player. Stance is standing, crouched or prone. Moving is still, forward, backward, left or right.
constexpr size_t kPlayerStance = 0x40;
constexpr size_t kPlayerMoving = 0x44;
constexpr size_t kPlayerVisor = 0x8c;
constexpr size_t kVisorClassMethod = 0x1c;

// Which kinds of ground in material.db have footsteps in player.def. The search for what's under
// the player leaves this at 0 if it finds no terrain there, which also happens on a rock or a log.
constexpr bool kGroundHasSteps[] = {false, false, false, true,  true, true, false, true,  true,
                                    true,  true,  true,  true,  true, true, false, true,  false};
constexpr unsigned kGroundTypes = sizeof(kGroundHasSteps) / sizeof(kGroundHasSteps[0]);
constexpr unsigned kGrassGround = 5;

constexpr float kGameWalkSpeed = 1.6f;
constexpr float kRunSpeed = 4.5f;
constexpr float kGameMoveShare = 0.5f;
constexpr float kGameStepRate = 1.0f;
constexpr float kStanceSpeeds[] = {1.0f, 0.5f, 0.2f};
constexpr int kStances = 3;
constexpr int kMoveStates = 5;
constexpr int kSideMoves = 3;

struct Point {
    float speed;
    float value;
};

// Both pass close to the game's own values at its speeds, so a normal walk and a crouch sound as
// they did. The difference is at the ends: creeping is quieter and running is louder.
constexpr Point kNoise[] = {{0.0f, 0.0f}, {0.4f, 0.15f}, {1.0f, 0.35f}, {1.9f, 0.7f}, {4.5f, 1.0f}};
constexpr Point kStepVolume[] = {{0.0f, 0.15f}, {0.4f, 0.25f}, {1.0f, 0.5f}, {1.9f, 0.8f}, {4.5f, 1.0f}};

constexpr size_t kGeomData = 0xc;
constexpr size_t kShapeListener = 4;
constexpr size_t kAvatarListener = 0x20;
constexpr size_t kAvatarWish = 0xc;  // the velocity the player is asking for
constexpr size_t kAvatarWalking = 0x38;

constexpr unsigned kContactFrictionDirection = 2;  // dContactFDir1
constexpr float kShortestWish = 0.001f;

// A report of how straight the player walked, once this far has been covered.
constexpr float kMeasureDistance = 3.0f;
constexpr int kMostReports = 200;
constexpr float kDegrees = 180.0f / 3.14159265f;

using CreateContactFn = void*(__cdecl*)(void*, void*, Contact*);
using FootstepFn = void(__cdecl*)(unsigned, float);
using SetHearingFn = void(__fastcall*)(void*, void*, float);
using CurrentHuntFn = char*(__cdecl*)();
using HuntPlayerFn = char*(__fastcall*)(char*, void*);
using PlayerRunningFn = bool(__fastcall*)(char*, void*);
using VisorClassFn = const void*(__fastcall*)(void*, void*);

char* g_game = nullptr;
bool g_ready = false;
float g_walk_speed = kGameWalkSpeed;
float g_move_share = kGameMoveShare;
float g_forward_rate = kGameStepRate;
float g_backward_rate = kGameStepRate;
unsigned g_step_ground = kGrassGround;
bool g_told_ground[kGroundTypes + 1] = {};

float g_start_x = 0.0f, g_start_z = 0.0f;
float g_wish_x = 0.0f, g_wish_z = 0.0f;
bool g_measuring = false;
int g_reports = 0;

// Every contact in the world comes through here, and only the player's geometry leads to this
// listener. Anything else can hold whatever it likes in these fields, so a bad read isn't fatal.
char* find_walking_avatar(char* geom) {
    __try {
        char* shape = geom ? *reinterpret_cast<char**>(geom + kGeomData) : nullptr;
        char* listener = shape ? *reinterpret_cast<char**>(shape + kShapeListener) : nullptr;
        if (!listener) return nullptr;
        if (*reinterpret_cast<void**>(listener) != g_game + kAvatarListenerVtable) return nullptr;
        char* avatar = listener - kAvatarListener;
        return avatar[kAvatarWalking] ? avatar : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Reports how far off the direction asked for the player actually went. This is what shows the
// drift for what it is, and it runs whether or not it's being corrected.
void measure(const float* wish, const float* position) {
    const float length = sqrtf(wish[0] * wish[0] + wish[2] * wish[2]);
    if (length < kShortestWish) {
        g_measuring = false;
        return;
    }
    if (!g_measuring) {
        g_measuring = true;
        g_start_x = position[0];
        g_start_z = position[2];
        g_wish_x = g_wish_z = 0.0f;
    }
    g_wish_x += wish[0] / length;
    g_wish_z += wish[2] / length;

    const float moved_x = position[0] - g_start_x;
    const float moved_z = position[2] - g_start_z;
    if (moved_x * moved_x + moved_z * moved_z < kMeasureDistance * kMeasureDistance) return;
    g_measuring = false;
    if (g_reports >= kMostReports) return;
    ++g_reports;

    const float along = g_wish_x * moved_x + g_wish_z * moved_z;
    const float across = g_wish_x * moved_z - g_wish_z * moved_x;
    const float off = walking_drift(along, across);
    const float heading = atan2f(g_wish_x, -g_wish_z) * kDegrees;
    logf("walking: heading %.0f, went %.1f degrees %s, correction %s", heading < 0.0f ? heading + 360.0f : heading,
         fabsf(off), off < 0.0f ? "left" : "right", g_cfg.straight_walking ? "on" : "off");
}

// The ground's friction is a box lined up with the world, so walking across its corners pulls the
// player sideways, toward whichever axis is nearest. Lining it up with the walk removes that.
void line_up_friction(Contact* contact, const float* wish) {
    const float* normal = contact->normal;
    const float along = wish[0] * normal[0] + wish[1] * normal[1] + wish[2] * normal[2];
    const float flat[3] = {wish[0] - along * normal[0], wish[1] - along * normal[1], wish[2] - along * normal[2]};
    const float length = sqrtf(flat[0] * flat[0] + flat[1] * flat[1] + flat[2] * flat[2]);
    if (length < kShortestWish) return;
    for (int i = 0; i < 3; ++i) contact->friction_direction[i] = flat[i] / length;
    contact->mode |= kContactFrictionDirection;
}

void* __cdecl hook_create_contact(void* world, void* group, Contact* contact) {
    char* avatar = find_walking_avatar(contact->first);
    if (!avatar) avatar = find_walking_avatar(contact->second);
    if (avatar) {
        const float* wish = reinterpret_cast<const float*>(avatar + kAvatarWish);
        measure(wish, contact->position);
        if (g_cfg.straight_walking) line_up_friction(contact, wish);
    } else {
        vehicle_contact(contact);
    }
    return reinterpret_cast<CreateContactFn>(g_game + kCreateContact)(world, group, contact);
}

float along_curve(const Point* points, int count, float speed) {
    if (speed <= points[0].speed) return points[0].value;
    for (int i = 1; i < count; ++i) {
        if (speed > points[i].speed) continue;
        const float share = (speed - points[i - 1].speed) / (points[i].speed - points[i - 1].speed);
        return points[i - 1].value + (points[i].value - points[i - 1].value) * share;
    }
    return points[count - 1].value;
}

// The world behind the menus keeps something of its own in the player's place, which isn't a hunter.
char* current_player() {
    char* hunt = reinterpret_cast<CurrentHuntFn>(g_game + kCurrentHunt)();
    char* player = hunt ? reinterpret_cast<HuntPlayerFn>(g_game + kHuntPlayer)(hunt, nullptr) : nullptr;
    return player && *reinterpret_cast<char**>(player) == g_game + kHunterTable ? player : nullptr;
}

const void* view_class(char* player) {
    void* visor = *reinterpret_cast<void**>(player + kPlayerVisor);
    if (!visor) return nullptr;
    void** methods = *reinterpret_cast<void***>(visor);
    return reinterpret_cast<VisorClassFn>(methods[kVisorClassMethod / sizeof(void*)])(visor, nullptr);
}

// The speed the game is asking for on foot, in m/s, or below zero when that doesn't apply.
float foot_speed(char* player) {
    if (!player) return -1.0f;
    const void* visor_class = view_class(player);
    if (!visor_class || visor_class == g_game + kVehicleVisorClass) return -1.0f;

    const int stance = *reinterpret_cast<int*>(player + kPlayerStance);
    const int moving = *reinterpret_cast<int*>(player + kPlayerMoving);
    if (stance < 0 || stance >= kStances || moving < 0 || moving >= kMoveStates) return -1.0f;
    if (moving == 0) return 0.0f;
    const bool running = reinterpret_cast<PlayerRunningFn>(g_game + kPlayerRunning)(player, nullptr);
    const float share = moving == 1 ? 1.0f : g_move_share;
    return (running ? kRunSpeed : g_walk_speed) * share * kStanceSpeeds[stance];
}

void __cdecl hook_footstep(unsigned substance, float volume) {
    substance = walking_step_ground(substance);
    if (g_cfg.speed_noise) {
        const float speed = foot_speed(current_player());
        if (speed >= 0.0f) volume = walking_step_volume(speed);
    }
    reinterpret_cast<FootstepFn>(g_game + kFootstep)(substance, volume);
}

// The game goes by stance and walking or running, so a slow creep and a full walk sound the same.
void __fastcall hook_set_hearing(void* hunter, void* unused, float hearing) {
    if (g_cfg.speed_noise) {
        const float speed = foot_speed(current_player());
        if (speed >= 0.0f) hearing = walking_noise(speed);
    }
    reinterpret_cast<SetHearingFn>(g_game + kSetHearing)(hunter, unused, hearing);
}

void set_value(uintptr_t at, float* current, float wanted) {
    if (*current == wanted) return;
    if (patch_bytes(g_game + at, current, &wanted, sizeof(wanted))) *current = wanted;
}

float clamped(float value, float lowest, float highest) {
    return value < lowest ? lowest : value > highest ? highest : value;
}

}  // namespace

float walking_noise(float speed) {
    return along_curve(kNoise, sizeof(kNoise) / sizeof(kNoise[0]), speed);
}

float walking_step_volume(float speed) {
    return along_curve(kStepVolume, sizeof(kStepVolume) / sizeof(kStepVolume[0]), speed);
}

char* walking_player() { return g_game ? current_player() : nullptr; }

const void* walking_view_class(char* player) { return view_class(player); }

// A step on ground with no sound plays nothing at all, so it borrows the last one that had any.
unsigned walking_step_ground(unsigned ground) {
    if (ground < kGroundTypes && kGroundHasSteps[ground]) {
        g_step_ground = ground;
        return ground;
    }
    const unsigned told = ground < kGroundTypes ? ground : kGroundTypes;
    if (!g_told_ground[told]) {
        g_told_ground[told] = true;
        logf("walking: ground type %u has no footsteps, so it sounds like type %u", ground, g_step_ground);
    }
    return g_step_ground;
}

// Steps quicken with the square root of the speed, because a stride gets longer as well.
void walking_settings_changed() {
    if (!g_ready) return;
    const bool faster = g_cfg.faster_walking;
    const float walk = faster ? clamped(g_cfg.walk_speed, 0.5f, kRunSpeed) : kGameWalkSpeed;
    const float share = faster ? clamped(g_cfg.side_speed, 0.1f, 1.0f) : kGameMoveShare;
    const float forward_rate = sqrtf(walk / kGameWalkSpeed);
    const float backward_rate = sqrtf(walk * share / (kGameWalkSpeed * kGameMoveShare));

    set_value(kWalkSpeed, &g_walk_speed, walk);
    for (int i = 0; i < kSideMoves; ++i) {
        float was = g_move_share;
        set_value(kMoveShares + i * sizeof(float), &was, share);
    }
    g_move_share = share;
    set_value(kStepRates, &g_forward_rate, forward_rate);
    set_value(kStepRates + sizeof(float), &g_backward_rate, backward_rate);
    logf("walking: %.2f m/s forward, %.2f m/s backward and sideways", g_walk_speed, g_walk_speed * g_move_share);
}

float walking_drift(float along, float across) { return atan2f(across, along) * kDegrees; }

void walking_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    const bool ok = patch_call(g_game + kCreateContactCall, g_game + kCreateContact,
                               reinterpret_cast<const void*>(&hook_create_contact));
    logf("walking: contact hook %s", ok ? "installed" : "FAILED");

    const bool steps = patch_call(g_game + kFootstepCall, g_game + kFootstep,
                                  reinterpret_cast<const void*>(&hook_footstep));
    const bool hearing = patch_call(g_game + kSetHearingCall, g_game + kSetHearing,
                                    reinterpret_cast<const void*>(&hook_set_hearing));
    logf("walking: footsteps=%d hearing=%d", steps, hearing);
    g_ready = true;
    walking_settings_changed();
}

}  // namespace longshot
