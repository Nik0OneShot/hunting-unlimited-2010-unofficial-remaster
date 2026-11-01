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
    // How much sideways speed the tyres can take off in a tick, each way. The game's 0.8 m/s is about
    // 3 g of cornering, far past the 1 g or so that rolls the ATV, which is fine at 25 km/h but kills
    // the rider at any real speed. At 0.22 m/s it's about 0.8 g, so the tyres let go first.
    {0x986ed, 0x3F4CCCCD, 0x3E6147AE},
    {0x986f5, 0xBF4CCCCD, 0xBE6147AE},
    // The suspension, as ODE's softness and correction. The game's come to a 40000 N/m spring with
    // 12% of the damping it takes to settle, so it shakes about 3.5 times a second and is still going
    // when the next bump arrives. These are 15000 N/m with 66%, which settles within one swing.
    // They're read when an ATV is made, so changing this takes a new ATV.
    {0x9b21d, 0x3A378034, 0x3A0BCF65},
    {0x9b22a, 0x3F333333, 0x3E4CCCCD},
};

// Those limits only apply past a sideways speed that's checked separately, against values other
// code shares. So the checks are pointed at values of their own.
constexpr float kGripLimit = 0.22f;
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

// A report of how far the tyres sank and how often the chassis touched, every 10 seconds or so of
// driving on all four wheels.
constexpr int kContactsPerReport = 1600;
constexpr int kMostReports = 60;

// A line for each time the chassis touches, counting contacts this close together as one.
constexpr DWORD kTouchGapMs = 250;
constexpr int kMostTouches = 100;

char* g_game = nullptr;
bool g_ready = false;

float g_deepest = 0.0f;
int g_contacts = 0;
int g_chassis_contacts = 0;
int g_reports = 0;
bool g_told_bounce = false;
DWORD g_last_touch = 0;
int g_touches = 0;

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
    }
    g_deepest = 0.0f;
    g_chassis_contacts = 0;
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
    return failed;
}

}  // namespace longshot
