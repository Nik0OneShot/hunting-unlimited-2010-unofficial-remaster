#include "longshot.h"

#include <intrin.h>

#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uint32_t kSlotImpulseActivated = 0x2d08e4;  // bound pointer to inp_impulse_is_activated
constexpr uintptr_t kPlayerInputCall = 0xddaf;        // the player's one call to read its input
constexpr uintptr_t kPlayerInput = 0xda50;
constexpr uintptr_t kModelStep = 0x15a8c0;  // moves a model's animation on by some seconds
constexpr uintptr_t kRifleStepCall = 0xab879;
constexpr uintptr_t kBowStepCall = 0xaa267;
constexpr uintptr_t kItemStepCall = 0xa2982;
constexpr uintptr_t kRifleVtable = 0x1a4170;
constexpr uintptr_t kBowVtable = 0x1a3ff8;
constexpr uintptr_t kRifleListenerVtable = 0x1a4168;
constexpr uintptr_t kSwayOffset = 0xa6150;     // sway for a weapon's model and shot
constexpr uintptr_t kShotSwayCall = 0xa7572;
constexpr uintptr_t kModelSwayCall = 0xabbce;
constexpr uintptr_t kViewSway = 0xa68c0;       // sway for the camera, used in a scope
constexpr uintptr_t kViewSwayCheck = 0xaa976;  // skips the camera's sway while the gun is drawn
constexpr uintptr_t kViewSwayCall = 0xaa97b;
constexpr uintptr_t kAimedGunLag = 0x1e1164;   // how far the gun can trail the view while aimed

// The player, what it holds, and what a weapon keeps. Reading these is safe on the verified build.
constexpr size_t kPlayerPending = 0x58;  // a request the game is already working through
constexpr size_t kPlayerHands = 0x88;
constexpr size_t kHandsItem = 0x10;
constexpr size_t kHandsWeapon = 0x14;
constexpr size_t kUnitFlags = 6;
constexpr unsigned char kUnitAlive = 0x80;
constexpr size_t kWeaponState = 0x58;
constexpr int kStateBusy = 0;
constexpr int kStateReady = 1;
constexpr int kStateDown = 2;
constexpr size_t kItemAutoLower = 0x64;  // calls and the like, which hold for their own length
constexpr size_t kRifleAmmo = 0x8c;
constexpr size_t kRifleLowerWanted = 0x1fc;
constexpr size_t kRifleScopeFitted = 0x264;
constexpr size_t kRifleAimed = 0x265;
constexpr size_t kRifleModelShown = 0x2b4;
constexpr size_t kRifleSwayYaw = 0x2bc;
constexpr size_t kRifleSwayPitch = 0x2c0;
constexpr size_t kRiflePhase = 0x340;
constexpr size_t kRifleStopReload = 0x34c;
constexpr size_t kRifleListener = 0x60;  // the rifle hears its animations end through this
constexpr int kEventFinished = 0;

// What a rifle is doing while it's busy. A bow uses its own numbers.
constexpr int kPhaseIdle = 0;
constexpr int kPhaseScopedKick = 3;
constexpr int kPhaseWatchShot = 5;  // waits to show the bullet land, through the scope
constexpr int kPhaseBeforeCock = 6;
constexpr int kPhaseCock = 7;
constexpr int kPhaseOpen = 9;
constexpr int kPhaseLoad = 10;
constexpr int kPhaseAimIn = 13;
constexpr int kPhaseAimOut = 14;
constexpr int kBowPhaseAimIn = 8;
constexpr int kBowPhaseAimOut = 9;

// The camera's sway is twice what the gun's is, which suits the narrow view of a scope. Ironsights
// keep the size the gun had.
constexpr float kIronsSwayShare = 0.5f;
constexpr float kGameAimedGunLag = 0.12f;

enum class Action { None, Fire, Reload, Use, Sights };

struct Impulse {
    int id;
    Action action;
    const char* name;
    double lifetime;  // how long a press stays worth acting on, in seconds
};

constexpr double kUntilDone = 3600.0;

// Raising, lowering and the sights say how the player wants the weapon to end up, so those wait as
// long as it takes. A shot or a reload stops being wanted quite quickly, and firing late is worse
// than not firing.
constexpr Impulse kImpulses[] = {
    {0x82, Action::Fire, "fire", 0.35},
    {0x83, Action::Reload, "reload", 1.0},
    {0x84, Action::Use, "raise or lower", kUntilDone},
    {0x86, Action::Sights, "sights", kUntilDone},
};
constexpr int kImpulseCount = sizeof(kImpulses) / sizeof(kImpulses[0]);

// Holds the one press waiting for the weapon to be free. The latest press replaces it, except that
// raise or lower, or the sights, pressed twice comes back to where it started.
class ActionQueue {
   public:
    void press(int impulse, double now) {
        const Action action = kImpulses[impulse].action;
        const bool toggles = action == Action::Use || action == Action::Sights;
        impulse_ = toggles && waiting() == action ? -1 : impulse;
        pressed_at_ = now;
    }

    void clear() { impulse_ = -1; }

    Action waiting() const { return impulse_ < 0 ? Action::None : kImpulses[impulse_].action; }

    // The impulse to let through now, or -1. Raising and reloading also work on a lowered weapon.
    int release(bool ready, bool down, double now) {
        if (impulse_ < 0) return -1;
        if (now - pressed_at_ > kImpulses[impulse_].lifetime) {
            impulse_ = -1;
            return -1;
        }
        const Action action = waiting();
        if (!ready && !(down && (action == Action::Use || action == Action::Reload))) return -1;
        const int released = impulse_;
        impulse_ = -1;
        return released;
    }

   private:
    int impulse_ = -1;
    double pressed_at_ = 0.0;
};

using ImpulseActivatedFn = bool(__cdecl*)(int);
using PlayerInputFn = void(__fastcall*)(void*, void*);
using ModelStepFn = void(__cdecl*)(void*, double);
using RifleEventFn = void(__fastcall*)(void*, void*, int, void*, void*);
using SwayOffsetFn = float*(__fastcall*)(void*, void*, float*);
using ViewSwayFn = void(__fastcall*)(void*, void*, void*);

char* g_game = nullptr;
bool g_verified = false;
ImpulseActivatedFn g_real_activated = nullptr;
RifleEventFn g_real_rifle_event = nullptr;
void* g_player = nullptr;
ActionQueue g_queue;
bool g_pressed[kImpulseCount] = {};
bool g_polled = false;
bool g_lag_ready = false;
float g_aimed_lag = kGameAimedGunLag;

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

double seconds_now() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) / frequency.QuadPart;
}

void* held(void* player, size_t which) {
    void* hands = field<void*>(player, kPlayerHands);
    void* thing = hands ? field<void*>(hands, which) : nullptr;
    return thing && (field<unsigned char>(thing, kUnitFlags) & kUnitAlive) ? thing : nullptr;
}

bool is_class(void* object, uintptr_t vtable) { return field<void*>(object, 0) == g_game + vtable; }

// Whether the player has already said they don't want this rifle aimed once it's free.
bool leaving_the_scope(void* rifle) {
    const Action waiting = g_queue.waiting();
    const bool aim_out = waiting == Action::Sights && field<bool>(rifle, kRifleAimed);
    return waiting == Action::Use || aim_out || field<bool>(rifle, kRifleLowerWanted);
}

// Skips what only delays something the player has already chosen to do next.
void hurry_rifle(void* rifle) {
    const int phase = field<int>(rifle, kRiflePhase);
    const Action waiting = g_queue.waiting();
    if (leaving_the_scope(rifle) && phase >= kPhaseScopedKick && phase <= kPhaseWatchShot) {
        field<int>(rifle, kRiflePhase) = kPhaseBeforeCock;
    }
    // The game's own way to end a reload early, after the round it's on, so no ammo goes astray.
    const bool can_fire = waiting == Action::Fire && field<int>(rifle, kRifleAmmo) > 0;
    if ((can_fire || waiting == Action::Use) && (phase == kPhaseOpen || phase == kPhaseLoad)) {
        field<bool>(rifle, kRifleStopReload) = true;
    }
}

void poll_input(void* player) {
    g_player = player;
    if (!g_cfg.weapon_queue) {
        g_queue.clear();
        return;
    }
    void* weapon = held(player, kHandsWeapon);
    if (!weapon) {
        g_queue.clear();
        return;
    }

    const int state = field<int>(weapon, kWeaponState);
    const bool busy = state == kStateBusy || field<int>(player, kPlayerPending) != 0;
    const double now = seconds_now();
    for (int i = 0; i < kImpulseCount; ++i) {
        g_pressed[i] = g_real_activated(kImpulses[i].id);
        if (!g_pressed[i]) continue;
        if (busy) {
            g_queue.press(i, now);
            g_pressed[i] = false;
            logf("weapons: %s waits", kImpulses[i].name);
        } else {
            g_queue.clear();
        }
    }
    g_polled = true;

    if (!busy) {
        const int released = g_queue.release(state == kStateReady, state == kStateDown, now);
        if (released >= 0) {
            g_pressed[released] = true;
            logf("weapons: %s goes ahead", kImpulses[released].name);
        }
    } else if (state == kStateBusy && is_class(weapon, kRifleVtable)) {
        hurry_rifle(weapon);
    }
}

// Presses are read once at the start of the player's turn, because nothing else would read most of
// them while the weapon is busy, and an unread press is gone after one frame.
bool __cdecl hook_impulse_activated(int id) {
    if (g_polled) {
        for (int i = 0; i < kImpulseCount; ++i) {
            if (kImpulses[i].id != id) continue;
            const bool pressed = g_pressed[i];
            g_pressed[i] = false;
            return pressed;
        }
    }
    return g_real_activated(id);
}

void __fastcall hook_player_input(void* player, void* unused) {
    poll_input(player);
    reinterpret_cast<PlayerInputFn>(g_game + kPlayerInput)(player, unused);
}

// Cocking a scoped rifle ends by putting the scope back up, because firing never clears the aim. If
// the rifle is about to be lowered, or the sights are about to come down, that would only flash.
void __fastcall hook_rifle_event(void* listener, void* unused, int event, void* first, void* second) {
    void* rifle = static_cast<char*>(listener) - kRifleListener;
    const bool cocked = event == kEventFinished && field<int>(rifle, kRiflePhase) == kPhaseCock;
    if (!cocked || !g_cfg.weapon_queue || !leaving_the_scope(rifle)) {
        g_real_rifle_event(listener, unused, event, first, second);
        return;
    }

    const bool fitted = field<bool>(rifle, kRifleScopeFitted);
    field<bool>(rifle, kRifleScopeFitted) = false;
    g_real_rifle_event(listener, unused, event, first, second);
    field<bool>(rifle, kRifleScopeFitted) = fitted;
    if (g_queue.waiting() == Action::Sights) return;  // goes ahead now the weapon's free
    field<bool>(rifle, kRifleLowerWanted) = true;
    g_queue.clear();
}

void step_model(void* model, double seconds) {
    reinterpret_cast<ModelStepFn>(g_game + kModelStep)(model, seconds);
}

bool is_quick_action(void* weapon) {
    if (field<int>(weapon, kWeaponState) != kStateBusy) return false;
    const int phase = field<int>(weapon, kRiflePhase);
    if (phase == kPhaseIdle) return true;
    if (is_class(weapon, kRifleVtable)) return phase == kPhaseAimIn || phase == kPhaseAimOut;
    return is_class(weapon, kBowVtable) && (phase == kBowPhaseAimIn || phase == kBowPhaseAimOut);
}

// Raising, lowering and aiming. Firing, cocking and reloading are left at their own pace.
void __cdecl hook_weapon_step(void* model, double seconds) {
    void* weapon = g_player ? held(g_player, kHandsWeapon) : nullptr;
    const bool faster = weapon && g_cfg.action_speed > 1.0f && is_quick_action(weapon);
    step_model(model, faster ? seconds * g_cfg.action_speed : seconds);
}

void __cdecl hook_item_step(void* model, double seconds) {
    void* item = g_player ? held(g_player, kHandsItem) : nullptr;
    const bool faster = item && g_cfg.action_speed > 1.0f && !field<bool>(item, kItemAutoLower);
    step_model(model, faster ? seconds * g_cfg.action_speed : seconds);
}

bool hook_step(uintptr_t site, void (__cdecl* hook)(void*, double)) {
    return patch_call(g_game + site, g_game + kModelStep, reinterpret_cast<const void*>(hook));
}

// The sway offset is shared with bows, which keep their own way of aiming.
bool in_ironsights(void* weapon) {
    return g_cfg.irons_view_sway && field<void*>(weapon, 0) == g_game + kRifleVtable &&
           field<bool>(weapon, kRifleAimed) && !field<bool>(weapon, kRifleScopeFitted);
}

// In ironsights the game turns the gun and the shot by the same angle. That isn't the angle the sights
// move on screen, since the gun turns about its own centre and is drawn with its own field of view,
// so the sights and the shot disagree. Turning the camera instead takes both along with it.
float* __fastcall hook_sway_offset(void* weapon, void* unused, float* offset) {
    if (in_ironsights(weapon)) {
        offset[0] = offset[1] = offset[2] = 0.0f;
        return offset;
    }
    return reinterpret_cast<SwayOffsetFn>(g_game + kSwayOffset)(weapon, unused, offset);
}

void __fastcall hook_view_sway(void* rifle, void* unused, void* placement) {
    const auto view_sway = reinterpret_cast<ViewSwayFn>(g_game + kViewSway);
    if (!field<bool>(rifle, kRifleModelShown)) {
        view_sway(rifle, unused, placement);
    } else if (in_ironsights(rifle)) {
        float& yaw = field<float>(rifle, kRifleSwayYaw);
        float& pitch = field<float>(rifle, kRifleSwayPitch);
        const float full_yaw = yaw;
        const float full_pitch = pitch;
        yaw *= kIronsSwayShare;
        pitch *= kIronsSwayShare;
        view_sway(rifle, unused, placement);
        yaw = full_yaw;
        pitch = full_pitch;
    }
}

}  // namespace

// The gun trailing a turn shifts the sights too, and the shot doesn't follow that either.
void weapons_settings_changed() {
    if (!g_lag_ready) return;
    const float wanted = g_cfg.irons_view_sway ? 0.0f : kGameAimedGunLag;
    if (wanted == g_aimed_lag) return;
    if (patch_bytes(g_game + kAimedGunLag, &g_aimed_lag, &wanted, sizeof(wanted))) g_aimed_lag = wanted;
}

void weapons_attach_game(HMODULE game, bool verified) {
    g_game = reinterpret_cast<char*>(game);
    g_verified = verified;
}

void weapons_game_ready() {
    if (!g_verified) return;
    auto** slot = reinterpret_cast<void**>(g_game + kSlotImpulseActivated);
    if (!*slot) {
        logf("weapons: input slot isn't bound yet");
        return;
    }
    g_real_activated = reinterpret_cast<ImpulseActivatedFn>(*slot);
    *slot = reinterpret_cast<void*>(&hook_impulse_activated);

    auto** listener = reinterpret_cast<void**>(g_game + kRifleListenerVtable);
    const bool event = vtable_patch(listener, reinterpret_cast<void*>(&hook_rifle_event),
                                    reinterpret_cast<void**>(&g_real_rifle_event));
    const bool input = patch_call(g_game + kPlayerInputCall, g_game + kPlayerInput,
                                  reinterpret_cast<const void*>(&hook_player_input));
    const bool steps = hook_step(kRifleStepCall, hook_weapon_step) && hook_step(kBowStepCall, hook_weapon_step) &&
                       hook_step(kItemStepCall, hook_item_step);
    logf("weapons: event=%d input=%d steps=%d", event, input, steps);

    const auto offset = reinterpret_cast<const void*>(&hook_sway_offset);
    const bool shot = patch_call(g_game + kShotSwayCall, g_game + kSwayOffset, offset);
    const bool model = patch_call(g_game + kModelSwayCall, g_game + kSwayOffset, offset);
    constexpr unsigned char kSkip[] = {0x75, 0x08};  // jne past the call
    constexpr unsigned char kFallThrough[] = {0x90, 0x90};
    const bool check = patch_bytes(g_game + kViewSwayCheck, kSkip, kFallThrough, sizeof(kSkip));
    const bool view = patch_call(g_game + kViewSwayCall, g_game + kViewSway,
                                 reinterpret_cast<const void*>(&hook_view_sway));
    logf("weapons: sway shot=%d model=%d check=%d view=%d", shot, model, check, view);
    g_lag_ready = true;
    weapons_settings_changed();
}

void weapons_end_frame() { g_polled = false; }

int weapons_test() {
    constexpr int kFire = 0, kReload = 1, kUse = 2, kSights = 3;
    constexpr bool kBusy = false, kFree = true;
    const auto impulse = [](int which) { return which; };

    ActionQueue queue;
    if (queue.release(kFree, false, 0.0) != -1) return 1;  // nothing waiting

    queue.press(kUse, 0.0);
    if (queue.release(kBusy, false, 2.0) != -1) return 2;  // holds while the weapon is busy
    if (queue.release(kFree, false, 2.5) != impulse(kUse)) return 3;  // a holster waits as long as it takes
    if (queue.release(kFree, false, 2.6) != -1) return 4;  // and only happens once

    queue.press(kUse, 0.0);
    queue.press(kFire, 0.1);
    if (queue.release(kFree, false, 0.2) != impulse(kFire)) return 5;  // the latest press decides

    queue.press(kFire, 0.0);
    queue.press(kUse, 0.1);
    if (queue.waiting() != Action::Use) return 6;

    queue.clear();
    queue.press(kUse, 0.0);
    queue.press(kUse, 0.1);
    if (queue.waiting() != Action::None) return 7;  // twice comes back to where it started

    queue.press(kFire, 0.0);
    if (queue.release(kFree, false, 0.5) != -1) return 8;  // a late shot is dropped
    queue.press(kFire, 0.0);
    if (queue.release(kFree, false, 0.3) != impulse(kFire)) return 9;  // but a slightly early one counts

    queue.press(kSights, 0.0);
    if (queue.release(kBusy, true, 0.1) != -1) return 10;  // sights need the weapon up
    if (queue.release(kFree, false, 2.5) != impulse(kSights)) return 11;  // and outlast a cock of the bolt

    queue.press(kSights, 0.0);
    queue.press(kSights, 0.1);
    if (queue.waiting() != Action::None) return 14;  // twice comes back to where it started

    queue.press(kUse, 0.0);
    if (queue.release(kBusy, true, 0.1) != impulse(kUse)) return 12;  // raising works from down
    queue.press(kReload, 0.0);
    if (queue.release(kBusy, true, 0.1) != impulse(kReload)) return 13;  // and so does reloading
    return 0;
}

}  // namespace longshot
