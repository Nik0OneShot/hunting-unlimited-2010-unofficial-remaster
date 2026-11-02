#include "longshot.h"

#include <cmath>
#include <cstring>

namespace longshot {

namespace {

struct Tuned {
    uintptr_t at;  // an offset into game.dll
    uint32_t game;
    uint32_t strong;
};

// The game cuts the ATV's engine at 25 km/h, and gives it only enough force to climb 21 degrees,
// which is why it takes a slope to get going. Strong means 55 km/h and 37 degrees.
constexpr Tuned kAtv[] = {
    // The squared speed in m/s that cuts the engine, from 25 km/h to 55.
    {0x1a274c, 0x4240A787, 0x43696916},
    // Force at each rear wheel, from 600 N to 1000 N.
    {0x1a2780, 0xC4160000, 0xC47A0000},
    // Share of its speed kept each physics tick with no throttle. The game's 0.8 is fine at 25
    // km/h, but at 55 it would stop the ATV dead.
    {0x9864f, 0x3F4CCCCD, 0x3F7AE148},
    // Turns speed into engine sound, so that the top of the range is still the top speed.
    {0x1a2748, 0x3E138CE9, 0x3D860D08},
    // How much sideways speed the tyres can take off in a tick, each way. They have no friction, so
    // this is all that holds the ATV sideways, on hillsides and over bumps as well as in turns. The
    // game's 0.8 m/s is 2.4 g of cornering, which flips it and kills the rider. 0.4 m/s is 1.2 g, and
    // the steering limit below keeps turns to 0.9 g, which leaves the rest for everything else.
    {0x986ed, 0x3F4CCCCD, 0x3ECCCCCD},
    {0x986f5, 0xBF4CCCCD, 0xBECCCCCD},
    // The suspension, as ODE's softness and correction. The game's come to a 40000 N/m spring with
    // 12% of the damping it takes to settle, so it shakes about 3.5 times a second and is still going
    // when the next bump arrives. These are 15000 N/m with 66%, which settles within one swing.
    // They're read when an ATV is made, so changing this takes a new ATV.
    {0x9b21d, 0x3A378034, 0x3A0BCF65},
    {0x9b22a, 0x3F333333, 0x3E4CCCCD},
};

// Those limits only apply past a sideways speed that's checked separately, against values other
// code shares. So the checks are pointed at values of their own.
constexpr float kGripLimit = 0.40f;
const float kGripAbove = kGripLimit;
const float kGripBelow = -kGripLimit;

// The handler at +0x99370 plays the crash sound and raises dust whenever the chassis touches anything
// with the ATV moving at over 1 m/s, however lightly. At 55 km/h the nose grazes the ground on dips
// and ridges whatever the springs are, because it reaches 40 cm past the front axle and its great
// inertia is slow to follow the ground. So that speed gets set for each contact: as the game has it
// for a tree or a wall, but against the ground only a real hit counts. 5 m/s is a drop of 1.3 m.
constexpr float kGameCrashSpeedSquared = 1.0f;
constexpr float kNoCrashSpeedSquared = 1e12f;
constexpr float kHardHit = 5.0f;
constexpr float kLeastUpward = 0.5f;  // how much of a contact has to face up for it to be the ground
float g_crash_speed_squared = kGameCrashSpeedSquared;

struct Pointed {
    uintptr_t at;    // the address inside an instruction
    uintptr_t game;  // what it points at in game.dll
    const float* strong;
};

const Pointed kPointers[] = {
    {0x9888f, 0x1a277c, &kGripBelow},
    {0x988a2, 0x19bfe0, &kGripAbove},
    {0x993d3, 0x19aed4, &g_crash_speed_squared},
};

// A wheel's shape. Map objects can share the material, but only the ATV is in this group, and only
// its wheels have this material.
constexpr size_t kGeomShape = 0xc;
constexpr size_t kGeomBody = 0x10;
constexpr size_t kBodyPosition = 0x98;
constexpr size_t kBodyVelocity = 0xe8;
constexpr size_t kBodySpin = 0xf8;
constexpr size_t kShapeGeom = 0x88;
constexpr size_t kShapeMaterial = 0x68;
constexpr size_t kShapeGroup = 0x6c;
constexpr uint32_t kTyreMaterial = 0x5;
constexpr uint32_t kVehicleGroup = 0x6;

constexpr unsigned kContactBounce = 4;     // dContactBounce
constexpr unsigned kContactSoftErp = 8;    // dContactSoftERP
constexpr unsigned kContactSoftCfm = 0x10;  // dContactSoftCFM
constexpr int kSurfaceBounce = 2;
constexpr int kSurfaceBounceSpeed = 3;
constexpr int kSurfaceSoftErp = 4;
constexpr int kSurfaceSoftCfm = 5;

// A tyre that gives, as a spring and damper. The game's wheels are hard balls on hard ground, so any
// ridge throws the wheel off the ground, and it lands a tick or two later. Giving them this much
// keeps them on it, and they settle about 4 cm into the ground under the ATV's weight.
constexpr float kPhysicsTick = 1.0f / 40.0f;
constexpr float kTyreSpring = 20000.0f;
constexpr float kTyreDamping = 1200.0f;
constexpr float kTyreErp = kPhysicsTick * kTyreSpring / (kPhysicsTick * kTyreSpring + kTyreDamping);
constexpr float kTyreCfm = 1.0f / (kPhysicsTick * kTyreSpring + kTyreDamping);

// Soft springs and tyres let the ATV sit 8 cm lower than the game's, and a hard hit takes it lower
// still, until the chassis meets the ground. Only the chassis is wired to the crash sound and dust at
// +0x99370. Starting each wheel below where it belongs preloads the spring, as on a real one, which
// lifts the ATV without firming anything up. 10 cm rather than 8 keeps the worst dips in the model
// from going any lower than the game's own suspension did.
// The game starts a wheel halfway through its suspension animation, but those animations' two frames
// are identical, so there's no travel to halve. Longshot's drop gets loaded in place of that.
constexpr uintptr_t kWheelHeight = 0x9b11f;
constexpr uintptr_t kHalf = 0x19b090;
constexpr size_t kWheelHeightSize = 9;
const float kWheelDrop = -0.10f;
unsigned char g_wheel_game[kWheelHeightSize] = {0xD9, 0x46, 0x54, 0xD8, 0x0D};  // fld [esi+0x54], fmul [kHalf]
unsigned char g_wheel_strong[kWheelHeightSize] = {0xD9, 0x05, 0, 0, 0, 0, 0x90, 0x90, 0x90};  // fld [kWheelDrop]

// Holding back selects reverse, which turns off the slowing that happens with no keys held, and
// pushes back with only 800 N, or nothing above the top speed. So back did less than letting go, and
// couldn't hold the ATV on a slope of more than 14 degrees. These run once a physics tick, on the
// ground, as a change in speed, the same way the game slows it.
constexpr uintptr_t kTick = 0x98510;
constexpr uintptr_t kTickPointer = 0x9a62f;  // inside the mov that gives the physics its tick
constexpr uintptr_t kControlsSlot = 0x1a28a8;
constexpr uintptr_t kMostSteering = 0x1ee5e0;
constexpr uintptr_t kSetVelocity = 0xbf30;
constexpr uintptr_t kLocalVelocity = 0x154230;
constexpr size_t kAtvActive = 0x58;
constexpr size_t kAtvBody = 0xc0;
constexpr size_t kAtvOnGround = 0x118;
constexpr size_t kAtvGear = 0x11c;
constexpr size_t kAtvRunning = 0x2c0;
constexpr size_t kMethodVelocity = 0x60;

constexpr int kGearForward = 0;
constexpr int kGearBack = 1;
constexpr int kGearNone = 2;

constexpr float kBrakeFrom = 0.5f;      // m/s the wrong way before a key counts as braking
constexpr float kBraking = 10.0f;       // m/s2 on top of reverse's own push: 55 km/h stops in 9 m
constexpr float kRollingDrag = 2.0f;    // m/s2 with no keys held, so that it stops on a 12 degree slope

// Steering goes up to 30 degrees at any speed, on a wheelbase of 1.12 m, which is a 1.9 m circle.
// That asks the tyres for 12 g at top speed, so they wash out and it runs wide. Steering stops where
// a turn would take this many m/s2, which is 2.5 degrees at the top speed and all of it below 15 km/h.
constexpr float kWheelbase = 1.12f;
constexpr float kMostCornering = 9.0f;

// A report of how far the tyres sank and how often the chassis touched, every 10 seconds or so of
// driving on all four wheels.
constexpr int kContactsPerReport = 1600;
constexpr int kMostReports = 60;

// A line for each time the chassis touches, counting contacts this close together as one.
constexpr DWORD kTouchGapMs = 250;
constexpr int kMostTouches = 100;

// The ATV's actor holds ODE's body at +0xb8, with methods to read and set its spin. The body has its
// rotation as three rows of four, and its joints as a list, each naming the body at the other end,
// which for a wheel is the chassis.
constexpr size_t kActorBody = 0xb8;
constexpr size_t kMethodSpin = 0x64;
constexpr size_t kMethodSetSpin = 0x84;
constexpr size_t kBodyJoints = 0x14;
constexpr size_t kBodyFlags = 0x18;
constexpr size_t kBodyRotation = 0xa8;
constexpr size_t kJointOtherBody = 4;
constexpr uint32_t kBodyDisabled = 4;

// Weight. Off the ground the ATV falls at twice the pace, so a bump throws it half as high, and its
// pitch and roll come round to land it on its wheels: nose along the way it's flying, within 20
// degrees, and level side to side. On the ground with one axle up, that end can't keep rising, which
// is how it went over backwards or onto its nose, and past 45 degrees it's brought back.
constexpr float kExtraFall = 9.81f;          // m/s2, on top of the world's own
constexpr float kSettleSeconds = 0.1f;       // spin comes round to what's wanted about this quickly
constexpr float kLevelSeconds = 0.3f;        // and what's wanted turns it level about this quickly
constexpr float kMostLevelling = 3.0f;       // rad/s
constexpr float kFlightShare = 0.6f;
constexpr float kMostFlightPitch = 0.35f;    // 20 degrees
constexpr float kLeastFlightSpeed = 3.0f;
constexpr float kSteepest = 0.785f;          // 45 degrees
constexpr float kLongJumpSeconds = 0.25f;
const float kSettleShare = 1.0f - expf(-kPhysicsTick / kSettleSeconds);

// The view from behind a vehicle orbits a point that eases toward it by 4 times the frame's seconds
// (+0x15426), which leaves it a quarter of a second behind: nearly 4 m at 55 km/h. So before each
// update that point is moved most of the way the vehicle went across the ground. Rising and falling
// are left to the game, which is what smooths the bumps.
constexpr uintptr_t kVehicleViewUpdateSlot = 0x19c624;
constexpr uintptr_t kVehicleViewTarget = 0x15150;
constexpr size_t kViewFollowing = 0x164;
constexpr float kKeepUp = 0.9f;           // what's left is 40 cm behind at the top speed
constexpr DWORD kViewGapMs = 500;         // longer than this since the last update starts afresh

// Which wheels touched since the ATV's last tick, front and back. One that hasn't been touched for a
// second is an ATV that's gone, so its place is freed.
struct Footing {
    char* chassis;
    int front;
    int back;
    int seen;
};
constexpr int kMostFootings = 4;
constexpr int kGoneTicks = 40;

using TickFn = void(__thiscall*)(void*, void*);
using ControlsFn = void(__thiscall*)(void*, float);
using VelocityFn = const float*(__thiscall*)(void*);
using SetVelocityFn = void(__thiscall*)(void*, const float*);
using LocalVelocityFn = void*(__thiscall*)(void*, float*);
using SpinFn = const float*(__thiscall*)(void*);
using ViewUpdateFn = void(__thiscall*)(void*, float);
using ViewTargetFn = void*(__thiscall*)(void*, float*);
using SetSpinFn = void(__thiscall*)(void*, const float*);

char* g_game = nullptr;
bool g_ready = false;
void* g_real_controls = nullptr;

float g_deepest = 0.0f;
int g_contacts = 0;
int g_chassis_contacts = 0;
int g_reports = 0;
bool g_told_bounce = false;
DWORD g_last_touch = 0;
int g_touches = 0;

Footing g_footing[kMostFootings] = {};
int g_unplaced = 0;  // tyre contacts whose chassis couldn't be found
int g_ticks = 0;
int g_air_ticks = 0;
int g_jumps = 0;
int g_longest_air = 0;
int g_ground_holds = 0;
float g_most_spin_taken = 0.0f;

void* g_real_view_update = nullptr;
char* g_followed_view = nullptr;
float g_followed[3] = {};
DWORD g_followed_at = 0;

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

enum class Part { None, Tyre, Chassis };

struct Touch {
    float closing;  // how fast the chassis is going into whatever it touched, where it touched
    float ahead;    // how far the contact is in the direction of travel, from the centre of mass
};

// Every contact in the world comes through here, and a shape is all that's known of the other side,
// so a bad read isn't fatal.
Part vehicle_part(const char* geom) {
    __try {
        const char* shape = geom ? *reinterpret_cast<char* const*>(geom + kGeomShape) : nullptr;
        if (!shape || *reinterpret_cast<const uint32_t*>(shape + kShapeGroup) != kVehicleGroup) return Part::None;
        return *reinterpret_cast<const uint32_t*>(shape + kShapeMaterial) == kTyreMaterial ? Part::Tyre : Part::Chassis;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Part::None;
    }
}

// An off-centre shape like the chassis is a geom inside another geom. Contacts name the inner one,
// but only the outer one has a body, and the shape knows that one.
bool read_touch(const Contact& contact, const char* geom, Touch* touch) {
    __try {
        const char* shape = *reinterpret_cast<char* const*>(geom + kGeomShape);
        const char* outer = *reinterpret_cast<char* const*>(shape + kShapeGeom);
        const char* body = *reinterpret_cast<char* const*>(outer + kGeomBody);
        if (!body) return false;
        const float* position = reinterpret_cast<const float*>(body + kBodyPosition);
        const float* velocity = reinterpret_cast<const float*>(body + kBodyVelocity);
        const float* spin = reinterpret_cast<const float*>(body + kBodySpin);
        const float* normal = contact.normal;
        const float arm[3] = {contact.position[0] - position[0], contact.position[1] - position[1],
                              contact.position[2] - position[2]};
        const float at[3] = {velocity[0] + spin[1] * arm[2] - spin[2] * arm[1],
                             velocity[1] + spin[2] * arm[0] - spin[0] * arm[2],
                             velocity[2] + spin[0] * arm[1] - spin[1] * arm[0]};
        touch->closing = fabsf(at[0] * normal[0] + at[1] * normal[1] + at[2] * normal[2]);
        const float speed = sqrtf(velocity[0] * velocity[0] + velocity[2] * velocity[2]);
        touch->ahead = speed > 0.1f ? (arm[0] * velocity[0] + arm[2] * velocity[2]) / speed : 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A wheel's body, which a geom inside a transform only has through its shape.
char* body_of(const char* geom) {
    char* body = *reinterpret_cast<char* const*>(geom + kGeomBody);
    if (body) return body;
    const char* shape = *reinterpret_cast<char* const*>(geom + kGeomShape);
    const char* outer = shape ? *reinterpret_cast<char* const*>(shape + kShapeGeom) : nullptr;
    return outer ? *reinterpret_cast<char* const*>(outer + kGeomBody) : nullptr;
}

// Which ATV a tyre contact holds up, and at which end.
void note_footing(const Contact& contact, const char* tyre) {
    __try {
        char* wheel = body_of(tyre);
        char* joint = wheel ? *reinterpret_cast<char**>(wheel + kBodyJoints) : nullptr;
        char* chassis = joint ? *reinterpret_cast<char**>(joint + kJointOtherBody) : nullptr;
        if (!chassis) {
            ++g_unplaced;
            return;
        }
        const float* r = reinterpret_cast<const float*>(chassis + kBodyRotation);
        const float* at = reinterpret_cast<const float*>(chassis + kBodyPosition);
        const float ahead = -((contact.position[0] - at[0]) * r[2] + (contact.position[1] - at[1]) * r[6] +
                              (contact.position[2] - at[2]) * r[10]);
        Footing* slot = nullptr;
        for (Footing& footing : g_footing) {
            if (footing.chassis == chassis) slot = &footing;
        }
        for (Footing& footing : g_footing) {
            if (!slot && !footing.chassis) slot = &footing;
        }
        if (!slot) {
            ++g_unplaced;
            return;
        }
        slot->chassis = chassis;
        slot->seen = g_ticks;
        if (ahead > 0.0f) ++slot->front;
        else ++slot->back;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_unplaced;
    }
}

// The game's handler runs straight after this, for this same contact.
void judge_chassis_contact(const Contact& contact, const char* geom) {
    ++g_chassis_contacts;
    Touch touch = {};
    const bool known = read_touch(contact, geom, &touch);
    const float upward = fabsf(contact.normal[1]);
    // The way a contact faces tells the ground from a tree even if the speed can't be read.
    const bool light = upward > kLeastUpward && (!known || touch.closing < kHardHit);
    g_crash_speed_squared = light ? kNoCrashSpeedSquared : kGameCrashSpeedSquared;

    const DWORD now = GetTickCount();
    if (now - g_last_touch > kTouchGapMs && g_touches < kMostTouches) {
        ++g_touches;
        if (known) {
            logf("vehicle: chassis touched %.2f m %s the centre, closing at %.1f m/s, facing up %.2f, %s", fabsf(touch.ahead),
                 touch.ahead < 0.0f ? "behind" : "ahead of", touch.closing, upward, light ? "quiet" : "crash");
        } else {
            logf("vehicle: chassis touched, speed unreadable, facing up %.2f, %s", upward, light ? "quiet" : "crash");
        }
    }
    g_last_touch = now;
}

// Whether the game's tyres bounce depends on values it never sets, so it can only be seen running.
void report_tyres(const Contact& contact) {
    if (!g_told_bounce && (contact.mode & kContactBounce)) {
        g_told_bounce = true;
        logf("vehicle: the game asked tyres to bounce, by %.3f above %.3f m/s", contact.surface[kSurfaceBounce],
             contact.surface[kSurfaceBounceSpeed]);
    }
    if (contact.depth > g_deepest) g_deepest = contact.depth;
    if (++g_contacts < kContactsPerReport) return;
    g_contacts = 0;
    if (g_reports < kMostReports) {
        ++g_reports;
        logf("vehicle: tyres sank up to %.1f cm, %d chassis contacts, soft tyres %s", g_deepest * 100.0f,
             g_chassis_contacts, g_cfg.strong_atv ? "on" : "off");
        logf("vehicle: %d jumps over a quarter second, longest %.2f s, %d holds on the ground, spin taken up to %.1f rad/s",
             g_jumps, g_longest_air * kPhysicsTick, g_ground_holds, g_most_spin_taken);
        g_jumps = g_longest_air = g_ground_holds = 0;
        g_most_spin_taken = 0.0f;
    }
    g_deepest = 0.0f;
    g_chassis_contacts = 0;
}

// `along` is the speed in the way it's pointing, and backwards if negative.
float atv_speed_after(float speed, float along, int gear) {
    const bool braking = (gear == kGearBack && along > kBrakeFrom) || (gear == kGearForward && along < -kBrakeFrom);
    const float slowing = braking ? kBraking : gear == kGearNone ? kRollingDrag : 0.0f;
    const float left = speed - slowing * kPhysicsTick;
    return left > 0.0f ? left : 0.0f;
}

float atv_steering_limit(float speed, float most) {
    if (speed < 1.0f) return most;
    const float limit = atanf(kWheelbase * kMostCornering / (speed * speed));
    return limit < most ? limit : most;
}

const float* velocity_of(char* body) {
    const auto method = (*reinterpret_cast<void***>(body))[kMethodVelocity / sizeof(void*)];
    return reinterpret_cast<VelocityFn>(method)(body);
}

float length(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void slow_down(char* atv) {
    char* body = field<char*>(atv, kAtvBody);
    if (!body || !field<uint8_t>(atv, kAtvOnGround)) return;
    const float* velocity = velocity_of(body);
    const float speed = length(velocity);
    if (speed <= 0.0f) return;
    float local[3];
    reinterpret_cast<LocalVelocityFn>(g_game + kLocalVelocity)(body, local);
    const float left = atv_speed_after(speed, -local[2], field<int>(atv, kAtvGear));
    if (left == speed) return;
    const float share = left / speed;
    const float slower[3] = {velocity[0] * share, velocity[1] * share, velocity[2] * share};
    reinterpret_cast<SetVelocityFn>(g_game + kSetVelocity)(body, slower);
}

float clamped(float value, float most) { return value > most ? most : value < -most ? -most : value; }

// Pitch is nose up and roll is the right side up, in radians, and their rates are how fast they grow.
struct Attitude {
    float pitch;
    float roll;
    float pitch_rate;
    float roll_rate;
    float climb;  // m/s up
    float speed;  // m/s along the ground
};

struct Steadied {
    float pitch_rate;
    float roll_rate;
    float fall;  // m/s taken off how fast it's going up
};

Steadied atv_steadied(const Attitude& now, int front, int back) {
    Steadied next = {now.pitch_rate, now.roll_rate, 0.0f};
    if (front == 0 && back == 0) {
        const float flight = now.speed > kLeastFlightSpeed ? atan2f(now.climb, now.speed) * kFlightShare : 0.0f;
        const float pitch_wanted = clamped((clamped(flight, kMostFlightPitch) - now.pitch) / kLevelSeconds, kMostLevelling);
        const float roll_wanted = clamped(-now.roll / kLevelSeconds, kMostLevelling);
        next.pitch_rate += (pitch_wanted - now.pitch_rate) * kSettleShare;
        next.roll_rate += (roll_wanted - now.roll_rate) * kSettleShare;
        next.fall = kExtraFall * kPhysicsTick;
        return next;
    }
    // Front up and still rising, or back up and still rising.
    if ((front == 0 && now.pitch_rate > 0.0f) || (back == 0 && now.pitch_rate < 0.0f)) {
        next.pitch_rate -= now.pitch_rate * kSettleShare;
    }
    if (fabsf(now.pitch) > kSteepest) {
        const float back_down = -copysignf(fminf((fabsf(now.pitch) - kSteepest) / kLevelSeconds, kMostLevelling), now.pitch);
        if (now.pitch > 0.0f ? next.pitch_rate > back_down : next.pitch_rate < back_down) {
            next.pitch_rate += (back_down - next.pitch_rate) * kSettleShare;
        }
    }
    return next;
}

Footing take_footing(char* chassis) {
    ++g_ticks;
    Footing found = {chassis, 0, 0, g_ticks};
    for (Footing& footing : g_footing) {
        if (footing.chassis == chassis) {
            found = footing;
            footing = {};
        } else if (footing.chassis && g_ticks - footing.seen > kGoneTicks) {
            footing = {};
        }
    }
    // If it can't be told which ATV a wheel was on, every ATV may have been standing on it.
    if (g_unplaced > 0) {
        found.front += g_unplaced;
        found.back += g_unplaced;
        g_unplaced = 0;
    }
    return found;
}

float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void steady(char* atv) {
    char* actor = field<char*>(atv, kAtvBody);
    char* body = actor ? field<char*>(actor, kActorBody) : nullptr;
    if (!body) return;
    const Footing footing = take_footing(body);
    if (field<uint32_t>(body, kBodyFlags) & kBodyDisabled) return;
    const float* r = reinterpret_cast<const float*>(body + kBodyRotation);
    const float side[3] = {r[0], r[4], r[8]};
    const float ahead[3] = {-r[2], -r[6], -r[10]};
    if (r[5] <= 0.0f) return;  // already over, so there's nothing to save
    const auto spin_method = (*reinterpret_cast<void***>(actor))[kMethodSpin / sizeof(void*)];
    const float* spin_now = reinterpret_cast<SpinFn>(spin_method)(actor);
    const float* velocity_now = velocity_of(actor);
    const float spin[3] = {spin_now[0], spin_now[1], spin_now[2]};
    const float velocity[3] = {velocity_now[0], velocity_now[1], velocity_now[2]};
    Attitude now;
    now.pitch = asinf(clamped(ahead[1], 1.0f));
    now.roll = asinf(clamped(side[1], 1.0f));
    now.pitch_rate = dot(spin, side);
    now.roll_rate = -dot(spin, ahead);
    now.climb = velocity[1];
    now.speed = sqrtf(velocity[0] * velocity[0] + velocity[2] * velocity[2]);
    const Steadied next = atv_steadied(now, footing.front, footing.back);

    const float pitch_change = next.pitch_rate - now.pitch_rate;
    const float roll_change = next.roll_rate - now.roll_rate;
    if (pitch_change != 0.0f || roll_change != 0.0f) {
        const float spun[3] = {spin[0] + side[0] * pitch_change - ahead[0] * roll_change,
                               spin[1] + side[1] * pitch_change - ahead[1] * roll_change,
                               spin[2] + side[2] * pitch_change - ahead[2] * roll_change};
        const auto set_spin = (*reinterpret_cast<void***>(actor))[kMethodSetSpin / sizeof(void*)];
        reinterpret_cast<SetSpinFn>(set_spin)(actor, spun);
        const float taken = sqrtf(pitch_change * pitch_change + roll_change * roll_change) / kSettleShare;
        if (taken > g_most_spin_taken) g_most_spin_taken = taken;
    }
    if (next.fall > 0.0f) {
        const float falling[3] = {velocity[0], velocity[1] - next.fall, velocity[2]};
        reinterpret_cast<SetVelocityFn>(g_game + kSetVelocity)(actor, falling);
        ++g_air_ticks;
    } else {
        if (g_air_ticks * kPhysicsTick >= kLongJumpSeconds) ++g_jumps;
        if (g_air_ticks > g_longest_air) g_longest_air = g_air_ticks;
        g_air_ticks = 0;
        if (pitch_change != 0.0f) ++g_ground_holds;
    }
}

bool steady_safely(char* atv) {
    __try {
        steady(atv);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_steadying = true;

void __fastcall hook_atv_tick(char* atv, void*, void* step) {
    const bool running = field<uint8_t>(atv, kAtvActive) || field<int>(atv, kAtvRunning) == 1;
    if (g_cfg.strong_atv && running) slow_down(atv);
    if (g_cfg.strong_atv && g_steadying && !steady_safely(atv)) {
        g_steadying = false;
        logf("vehicle: something went wrong steadying the ATV, so it's left to the game");
    }
    reinterpret_cast<TickFn>(g_game + kTick)(atv, step);
}

// Moves the point the view follows along with the vehicle, across the ground.
void keep_up(float* following, const float* was, const float* now) {
    following[0] += (now[0] - was[0]) * kKeepUp;
    following[2] += (now[2] - was[2]) * kKeepUp;
}

bool view_target(char* view, float* target) {
    __try {
        reinterpret_cast<ViewTargetFn>(g_game + kVehicleViewTarget)(view, target);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall hook_vehicle_view(char* view, void*, float seconds) {
    float target[7];
    const DWORD now = GetTickCount();
    if (g_cfg.strong_atv && view_target(view, target)) {
        if (view == g_followed_view && now - g_followed_at < kViewGapMs) {
            keep_up(reinterpret_cast<float*>(view + kViewFollowing), g_followed, target);
        }
        memcpy(g_followed, target, sizeof(g_followed));
        g_followed_view = view;
        g_followed_at = now;
    } else {
        g_followed_view = nullptr;
    }
    reinterpret_cast<ViewUpdateFn>(g_real_view_update)(view, seconds);
}

// The handlebars and the rider read the same limit, so it's only changed while this one runs.
void __fastcall hook_atv_controls(char* atv, void*, float seconds) {
    float& most = *reinterpret_cast<float*>(g_game + kMostSteering);
    const float game_most = most;
    char* body = field<char*>(atv, kAtvBody);
    if (g_cfg.strong_atv && body) most = atv_steering_limit(length(velocity_of(body)), game_most);
    reinterpret_cast<ControlsFn>(g_real_controls)(atv, seconds);
    most = game_most;
}

}  // namespace

void vehicle_contact(Contact* contact) {
    if (!g_ready) return;
    const Part first = vehicle_part(contact->first);
    const Part second = vehicle_part(contact->second);
    if (first == Part::Chassis) judge_chassis_contact(*contact, contact->first);
    else if (second == Part::Chassis) judge_chassis_contact(*contact, contact->second);
    if (first != Part::Tyre && second != Part::Tyre) return;
    report_tyres(*contact);
    note_footing(*contact, first == Part::Tyre ? contact->first : contact->second);
    if (!g_cfg.strong_atv) return;
    contact->mode = (contact->mode & ~kContactBounce) | kContactSoftErp | kContactSoftCfm;
    contact->surface[kSurfaceSoftErp] = kTyreErp;
    contact->surface[kSurfaceSoftCfm] = kTyreCfm;
}

void vehicle_settings_changed() {
    if (!g_ready) return;
    for (const Tuned& tuned : kAtv) {
        void* at = g_game + tuned.at;
        const uint32_t wanted = g_cfg.strong_atv ? tuned.strong : tuned.game;
        const uint32_t other = g_cfg.strong_atv ? tuned.game : tuned.strong;
        if (memcmp(at, &wanted, sizeof(wanted)) != 0) patch_bytes(at, &other, &wanted, sizeof(wanted));
    }
    for (const Pointed& check : kPointers) {
        void* at = g_game + check.at;
        const void* game = g_game + check.game;
        const void* wanted = g_cfg.strong_atv ? static_cast<const void*>(check.strong) : game;
        const void* other = g_cfg.strong_atv ? game : static_cast<const void*>(check.strong);
        if (memcmp(at, &wanted, sizeof(wanted)) != 0) patch_bytes(at, &other, &wanted, sizeof(wanted));
    }
    const unsigned char* wanted = g_cfg.strong_atv ? g_wheel_strong : g_wheel_game;
    const unsigned char* other = g_cfg.strong_atv ? g_wheel_game : g_wheel_strong;
    if (memcmp(g_game + kWheelHeight, wanted, kWheelHeightSize) != 0) {
        patch_bytes(g_game + kWheelHeight, other, wanted, kWheelHeightSize);
    }
    logf("vehicle: ATV is %s", g_cfg.strong_atv ? "strong" : "as the game has it");
}

void vehicle_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    g_ready = true;
    for (const Tuned& tuned : kAtv) g_ready = g_ready && memcmp(g_game + tuned.at, &tuned.game, sizeof(tuned.game)) == 0;
    for (const Pointed& check : kPointers) {
        const void* original = g_game + check.game;
        g_ready = g_ready && memcmp(g_game + check.at, &original, sizeof(original)) == 0;
    }
    const void* half = g_game + kHalf;
    const float* drop = &kWheelDrop;
    memcpy(g_wheel_game + 5, &half, sizeof(half));
    memcpy(g_wheel_strong + 2, &drop, sizeof(drop));
    g_ready = g_ready && memcmp(g_game + kWheelHeight, g_wheel_game, kWheelHeightSize) == 0;
    logf("vehicle: ATV %s", g_ready ? "found" : "not as expected, leaving it alone");
    vehicle_settings_changed();
    if (!g_ready) return;
    const void* game_tick = g_game + kTick;
    const void* our_tick = reinterpret_cast<const void*>(&hook_atv_tick);
    const bool tick = patch_bytes(g_game + kTickPointer, &game_tick, &our_tick, sizeof(our_tick));
    const bool controls = vtable_patch(reinterpret_cast<void**>(g_game + kControlsSlot),
                                       reinterpret_cast<void*>(&hook_atv_controls), &g_real_controls);
    const bool view = vtable_patch(reinterpret_cast<void**>(g_game + kVehicleViewUpdateSlot),
                                   reinterpret_cast<void*>(&hook_vehicle_view), &g_real_view_update);
    logf("vehicle: brakes=%d steering=%d view=%d", tick, controls, view);
}

int vehicle_test() {
    constexpr unsigned kMu2 = 1;
    char shape[kShapeGeom + sizeof(char*)] = {};
    char other_shape[kShapeGeom + sizeof(char*)] = {};
    char body[kBodySpin + 3 * sizeof(float)] = {};
    // As in the game, the geom that contacts name has no body. The one around it does.
    char* geom[5] = {nullptr, nullptr, nullptr, shape, nullptr};
    char* outer_geom[5] = {nullptr, nullptr, nullptr, shape, body};
    char* other_geom[5] = {nullptr, nullptr, nullptr, other_shape, nullptr};
    static_assert(sizeof(geom[0]) * 3 == kGeomShape, "a geom's shape is its fourth field");
    static_assert(sizeof(geom[0]) * 4 == kGeomBody, "and its body the fifth");
    *reinterpret_cast<char**>(shape + kShapeGeom) = reinterpret_cast<char*>(outer_geom);
    const auto set_shape = [](char* into, uint32_t material, uint32_t group) {
        *reinterpret_cast<uint32_t*>(into + kShapeMaterial) = material;
        *reinterpret_cast<uint32_t*>(into + kShapeGroup) = group;
    };
    const auto touch = [&](char* first, char* second) {
        Contact contact = {};
        contact.mode = kMu2 | kContactBounce;
        contact.first = first;
        contact.second = second;
        vehicle_contact(&contact);
        return contact;
    };
    const auto softened = [](const Contact& contact) {
        return contact.mode == (kMu2 | kContactSoftErp | kContactSoftCfm) &&
               contact.surface[kSurfaceSoftErp] == kTyreErp && contact.surface[kSurfaceSoftCfm] == kTyreCfm;
    };
    const auto untouched = [](const Contact& contact) {
        return contact.mode == (kMu2 | kContactBounce) && contact.surface[kSurfaceSoftErp] == 0.0f;
    };
    char* const tyre = reinterpret_cast<char*>(geom);
    char* const other = reinterpret_cast<char*>(other_geom);
    char* const bad = reinterpret_cast<char*>(1);
    float* const velocity = reinterpret_cast<float*>(body + kBodyVelocity);
    // The crash speed the game's handler would see for a chassis moving like this.
    const auto crash_speed = [&](float across, float down, float normal_x, float normal_y, bool chassis_first) {
        velocity[0] = across;
        velocity[1] = -down;
        Contact contact = {};
        contact.normal[0] = normal_x;
        contact.normal[1] = normal_y;
        contact.first = chassis_first ? tyre : other;
        contact.second = chassis_first ? other : tyre;
        vehicle_contact(&contact);
        return g_crash_speed_squared;
    };

    const bool was_ready = g_ready;
    const bool was_strong = g_cfg.strong_atv;
    g_ready = true;
    g_cfg.strong_atv = true;
    int failed = 0;
    set_shape(shape, kTyreMaterial, kVehicleGroup);
    if (!softened(touch(tyre, other))) failed = 1;
    if (!failed && !softened(touch(other, tyre))) failed = 2;
    set_shape(shape, kTyreMaterial, 0);  // a map object with the same material
    if (!failed && !untouched(touch(tyre, other))) failed = 3;
    set_shape(shape, 0, kVehicleGroup);  // the ATV's body
    if (!failed && !untouched(touch(tyre, other))) failed = 4;
    if (!failed && !untouched(touch(bad, nullptr))) failed = 5;
    set_shape(shape, kTyreMaterial, kVehicleGroup);
    g_cfg.strong_atv = false;
    if (!failed && !untouched(touch(tyre, other))) failed = 6;
    set_shape(shape, 0, kVehicleGroup);
    if (!failed && crash_speed(15.0f, 1.0f, 0.0f, 1.0f, true) != kNoCrashSpeedSquared) failed = 7;  // grazing the ground
    if (!failed && crash_speed(15.0f, 1.0f, 0.0f, 1.0f, false) != kNoCrashSpeedSquared) failed = 8;
    if (!failed && crash_speed(15.0f, 6.0f, 0.0f, 1.0f, true) != kGameCrashSpeedSquared) failed = 9;  // landing hard
    if (!failed && crash_speed(-1.0f, 0.0f, 1.0f, 0.0f, true) != kGameCrashSpeedSquared) failed = 10;  // a tree
    *reinterpret_cast<char**>(shape + kShapeGeom) = nullptr;  // and with no way to read the speed
    if (!failed && crash_speed(15.0f, 1.0f, 0.0f, 1.0f, true) != kNoCrashSpeedSquared) failed = 11;
    if (!failed && crash_speed(-1.0f, 0.0f, 1.0f, 0.0f, true) != kGameCrashSpeedSquared) failed = 12;
    g_crash_speed_squared = kGameCrashSpeedSquared;
    g_cfg.strong_atv = was_strong;
    g_ready = was_ready;
    if (failed) return failed;

    const float braked = kBraking * kPhysicsTick;
    const float drag = kRollingDrag * kPhysicsTick;
    const auto about = [](float a, float b) { return fabsf(a - b) < 0.0001f; };
    if (!about(atv_speed_after(15.0f, 15.0f, kGearBack), 15.0f - braked)) return 13;       // back, going forwards
    if (!about(atv_speed_after(15.0f, -15.0f, kGearForward), 15.0f - braked)) return 14;   // forward, going backwards
    if (!about(atv_speed_after(15.0f, 15.0f, kGearForward), 15.0f)) return 15;             // just driving
    if (!about(atv_speed_after(5.0f, -5.0f, kGearBack), 5.0f)) return 16;                  // just reversing
    if (!about(atv_speed_after(0.2f, 0.2f, kGearBack), 0.2f)) return 17;                   // stopped, so back reverses
    if (!about(atv_speed_after(15.0f, 15.0f, kGearNone), 15.0f - drag)) return 18;         // no keys
    if (!about(atv_speed_after(0.04f, 0.04f, kGearNone), 0.0f)) return 19;                 // and never past a stop

    // The view keeps up across the ground, but rises and falls the game's way.
    float following[3] = {10.0f, 5.0f, -3.0f};
    const float was[3] = {12.0f, 5.0f, -3.0f};
    const float moved[3] = {13.0f, 6.0f, -5.0f};
    keep_up(following, was, moved);
    if (!about(following[0], 10.0f + kKeepUp) || following[1] != 5.0f || !about(following[2], -3.0f - 2.0f * kKeepUp)) return 36;

    // In the air, a nose coming up fast is slowed and turned back, and it falls faster.
    const Steadied flipping = atv_steadied({0.2f, 0.0f, 3.0f, 0.0f, 0.0f, 15.0f}, 0, 0);
    if (flipping.pitch_rate >= 3.0f || flipping.pitch_rate < 0.0f || !about(flipping.fall, kExtraFall * kPhysicsTick)) return 25;
    const Steadied rolling = atv_steadied({0.0f, 0.3f, 0.0f, 0.0f, 0.0f, 15.0f}, 0, 0);
    if (rolling.roll_rate >= 0.0f || rolling.pitch_rate != 0.0f) return 26;                // level side to side
    const Steadied climbing = atv_steadied({0.0f, 0.0f, 0.0f, 0.0f, 15.0f, 15.0f}, 0, 0);
    if (climbing.pitch_rate <= 0.0f) return 27;                                            // nose along the way it flies
    const Steadied steep_flight = atv_steadied({kMostFlightPitch, 0.0f, 0.0f, 0.0f, 50.0f, 5.0f}, 0, 0);
    if (!about(steep_flight.pitch_rate, 0.0f)) return 28;                                  // but only so far
    // On all four wheels it goes where the ground takes it.
    const Steadied bump = atv_steadied({0.3f, 0.1f, 2.0f, 1.0f, 3.0f, 15.0f}, 2, 2);
    if (bump.pitch_rate != 2.0f || bump.roll_rate != 1.0f || bump.fall != 0.0f) return 29;
    // On one axle, the end that's up stops rising, but can come down.
    if (atv_steadied({0.3f, 0.0f, 2.0f, 0.0f, 0.0f, 10.0f}, 0, 2).pitch_rate >= 2.0f) return 30;
    if (atv_steadied({-0.3f, 0.0f, -2.0f, 0.0f, 0.0f, 10.0f}, 2, 0).pitch_rate <= -2.0f) return 31;
    if (atv_steadied({0.3f, 0.0f, -2.0f, 0.0f, 0.0f, 10.0f}, 0, 2).pitch_rate != -2.0f) return 32;
    // Past 45 degrees it's brought back, either way.
    if (atv_steadied({1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f}, 2, 2).pitch_rate >= 0.0f) return 33;
    if (atv_steadied({-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f}, 2, 2).pitch_rate <= 0.0f) return 34;
    if (atv_steadied({1.0f, 0.0f, -2.0f, 0.0f, 0.0f, 2.0f}, 2, 2).pitch_rate != -2.0f) return 35;  // already coming back

    constexpr float kFullLock = 0.5236f;
    if (!about(atv_steering_limit(0.0f, kFullLock), kFullLock)) return 21;
    if (!about(atv_steering_limit(4.0f, kFullLock), kFullLock)) return 22;                 // below 15 km/h
    const float at_top = atv_steering_limit(15.3f, kFullLock);
    if (at_top < 0.03f || at_top > 0.06f) return 23;
    if (atv_steering_limit(8.0f, kFullLock) <= atv_steering_limit(12.0f, kFullLock)) return 24;
    return 0;
}

}  // namespace longshot
