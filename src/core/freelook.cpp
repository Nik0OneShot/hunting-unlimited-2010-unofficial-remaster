#include "longshot.h"

#include <cmath>
#include <cstring>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kFootViewClass = 0x19c220;
constexpr uintptr_t kFollowBodySlot = 0x19c270;  // where the foot view catches up with the body
constexpr uintptr_t kUpdateFrustum = 0xd0160;
constexpr size_t kPlacePosition = 0x20;
constexpr size_t kPlaceRotation = 0x2c;

constexpr char kAttitudeSet[] = "?set@attitude_t@prism@@QAEXABVquat_t@2@@Z";
constexpr char kAttitudeQuat[] = "?get_quaternion@attitude_t@prism@@QBE?AVquat_t@2@XZ";

constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr float kMostPitch = 1.22173f;      // the game's own limit, 70 degrees, for where the view ends up
constexpr double kReturnSeconds = 0.07;     // how quickly the head comes back to the front
constexpr float kStraightAhead = 0.001f;

// A knock from an animal throws the view along it, tips and rolls it, and it springs back in about
// half a second.
constexpr float kShoveSpring = 12.0f;      // about 2 swings a second
constexpr float kShoveDamping = 0.55f;
constexpr float kShoveSpeed = 5.0f;        // m/s at full strength, which moves it about 0.2 m
constexpr float kShoveTipSpeed = 2.5f;     // rad/s, which tips it about 6 degrees
constexpr float kShoveRollSpeed = 3.0f;    // and rolls it about 7
constexpr float kShoveStep = 1.0f / 240.0f;
constexpr float kShoveSettled = 1e-4f;
enum ShoveAxis { kShoveX, kShoveZ, kShoveTip, kShoveRoll, kShoveAxes };

struct Spring {
    float at = 0.0f;
    float speed = 0.0f;
};

using AttitudeSetFn = void(__thiscall*)(float*, const float*);
using AttitudeQuatFn = float*(__thiscall*)(const float*, float*);
using UpdateFrustumFn = void(__thiscall*)(void*);
using FollowBodyFn = void(__fastcall*)(char*, void*);

char* g_game = nullptr;
AttitudeSetFn g_attitude_set = nullptr;
AttitudeQuatFn g_attitude_quat = nullptr;
FollowBodyFn g_real_follow_body = nullptr;

float g_yaw = 0.0f;
float g_pitch = 0.0f;
double g_last_frame = 0.0;
Spring g_shove[kShoveAxes];

float limited(float value, float most) { return value > most ? most : value < -most ? -most : value; }

void step_spring(Spring* spring, float seconds) {
    spring->speed += (-kShoveSpring * kShoveSpring * spring->at - 2.0f * kShoveDamping * kShoveSpring * spring->speed) * seconds;
    spring->at += spring->speed * seconds;
}

void step_shove(float seconds) {
    for (Spring& spring : g_shove) {
        if (spring.at == 0.0f && spring.speed == 0.0f) continue;
        for (float left = seconds; left > 0.0f; left -= kShoveStep) step_spring(&spring, fminf(left, kShoveStep));
        if (fabsf(spring.at) < kShoveSettled && fabsf(spring.speed) < kShoveSettled) spring = Spring();
    }
}

bool shoved() {
    for (const Spring& spring : g_shove) {
        if (spring.at != 0.0f) return true;
    }
    return false;
}

bool key_held() {
    return g_cfg.freelook_key != 0 && game_has_focus() && (GetAsyncKeyState(g_cfg.freelook_key) & 0x8000) != 0;
}

bool on_foot() {
    char* player = walking_player();
    return player && walking_view_class(player) == g_game + kFootViewClass;
}

void update_frustum(char* place) { reinterpret_cast<UpdateFrustumFn>(g_game + kUpdateFrustum)(place); }

// Comes back at the same pace at any frame rate.
float towards_front(float angle, double seconds) {
    const float left = angle * static_cast<float>(exp(-seconds / kReturnSeconds));
    return fabsf(left) < kStraightAhead ? 0.0f : left;
}

// The view takes its direction from the body as the world starts to draw, so the head goes on after.
// It stays on until the next time, as shots and the gun read the view and should go where you look.
// Walking and turning read the body, so they never see it.
void __fastcall hook_follow_body(char* view, void* unused) {
    g_real_follow_body(view, unused);
    const bool knocked = shoved();
    if (g_yaw == 0.0f && g_pitch == 0.0f && !knocked) return;
    float* position = reinterpret_cast<float*>(view + kPlacePosition);
    position[0] += g_shove[kShoveX].at;
    position[2] += g_shove[kShoveZ].at;
    float attitude[4] = {};
    g_attitude_set(attitude, reinterpret_cast<const float*>(view + kPlaceRotation));
    attitude[0] += g_yaw;
    attitude[1] = limited(attitude[1] + g_pitch + g_shove[kShoveTip].at, kMostPitch);
    attitude[2] += g_shove[kShoveRoll].at;
    float turned[4];
    g_attitude_quat(attitude, turned);
    memcpy(view + kPlaceRotation, turned, sizeof(turned));
    update_frustum(view);
}

}  // namespace

bool freelook_on_foot() { return g_game && on_foot(); }

bool freelook_takes_mouse() { return g_game && g_attitude_set && key_held() && on_foot(); }

void freelook_turn(bool sideways, float radians) {
    if (sideways) g_yaw = limited(g_yaw + radians, g_cfg.freelook_yaw * kDegToRad);
    else g_pitch = limited(g_pitch + radians, g_cfg.freelook_pitch * kDegToRad);
}

void freelook_before_render() {
    if (!g_game || !g_attitude_set) return;
    const double now = now_seconds();
    const double seconds = g_last_frame > 0.0 ? now - g_last_frame : 0.0;
    g_last_frame = now;
    if (!key_held()) {
        g_yaw = towards_front(g_yaw, seconds);
        g_pitch = towards_front(g_pitch, seconds);
    }
    step_shove(static_cast<float>(seconds));
    if ((g_yaw != 0.0f || g_pitch != 0.0f || shoved()) && !on_foot()) {
        g_yaw = g_pitch = 0.0f;
        for (Spring& spring : g_shove) spring = Spring();
    }
}

void freelook_shove(const float* push, float back, float side, float strength) {
    if (!g_game || !g_attitude_set) return;
    g_shove[kShoveX].speed += push[0] * kShoveSpeed * strength;
    g_shove[kShoveZ].speed += push[1] * kShoveSpeed * strength;
    g_shove[kShoveTip].speed += back * kShoveTipSpeed * strength;
    g_shove[kShoveRoll].speed += side * kShoveRollSpeed * strength;
}

void freelook_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    HMODULE core = GetModuleHandleW(L"p3core.dll");
    g_attitude_set = reinterpret_cast<AttitudeSetFn>(GetProcAddress(core, kAttitudeSet));
    g_attitude_quat = reinterpret_cast<AttitudeQuatFn>(GetProcAddress(core, kAttitudeQuat));
    g_game = reinterpret_cast<char*>(game);
    const bool ready = g_attitude_set && g_attitude_quat &&
                       vtable_patch(reinterpret_cast<void**>(g_game + kFollowBodySlot), reinterpret_cast<void*>(&hook_follow_body),
                                    reinterpret_cast<void**>(&g_real_follow_body));
    logf("freelook: %s", ready ? "ready" : "engine functions not found");
    if (!ready) g_attitude_set = nullptr;
}

int freelook_test() {
    const float was_yaw = g_cfg.freelook_yaw;
    const float was_pitch = g_cfg.freelook_pitch;
    g_cfg.freelook_yaw = 100.0f;
    g_cfg.freelook_pitch = 60.0f;
    g_yaw = g_pitch = 0.0f;
    int failed = 0;
    freelook_turn(true, 0.5f);
    freelook_turn(false, -0.25f);
    if (fabsf(g_yaw - 0.5f) > 1e-5f || fabsf(g_pitch + 0.25f) > 1e-5f) failed = 1;  // each axis on its own
    freelook_turn(true, 100.0f);
    if (!failed && fabsf(g_yaw - 100.0f * kDegToRad) > 1e-5f) failed = 2;           // clamped one way
    freelook_turn(true, -100.0f);
    if (!failed && fabsf(g_yaw + 100.0f * kDegToRad) > 1e-5f) failed = 3;           // and the other
    freelook_turn(false, -100.0f);
    if (!failed && fabsf(g_pitch + 60.0f * kDegToRad) > 1e-5f) failed = 4;          // and up and down on its own limit
    if (!failed && fabsf(towards_front(1.0f, kReturnSeconds) - 0.3679f) > 0.001f) failed = 5;
    if (!failed && towards_front(1.0f, 10.0) != 0.0f) failed = 6;                   // and settles exactly
    const float small_steps = towards_front(towards_front(1.0f, 0.01), 0.01);
    if (!failed && fabsf(small_steps - towards_front(1.0f, 0.02)) > 0.001f) failed = 7;  // the same at any frame rate
    Spring spring;
    spring.speed = kShoveSpeed;
    float furthest = 0.0f;
    for (int step = 0; step < 60; ++step) {
        step_spring(&spring, 1.0f / 60.0f);
        furthest = fmaxf(furthest, spring.at);
    }
    if (!failed && (furthest < 0.15f || furthest > 0.35f)) failed = 8;            // a knock moves it a little
    if (!failed && fabsf(spring.at) > 0.02f) failed = 9;                           // and it's back within a second
    g_yaw = g_pitch = 0.0f;
    g_cfg.freelook_yaw = was_yaw;
    g_cfg.freelook_pitch = was_pitch;
    return failed;
}

}  // namespace longshot
