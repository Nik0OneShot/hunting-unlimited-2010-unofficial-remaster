#include "longshot.h"

#include <cmath>
#include <cstring>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kCreateContactCall = 0x12765d;  // the only place contacts become joints
constexpr uintptr_t kCreateContact = 0x10b7d0;      // dJointCreateContact
constexpr uintptr_t kAvatarListenerVtable = 0x1b1c6c;

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

char* g_game = nullptr;

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

}  // namespace

float walking_drift(float along, float across) { return atan2f(across, along) * kDegrees; }

void walking_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    const bool ok = patch_call(g_game + kCreateContactCall, g_game + kCreateContact,
                               reinterpret_cast<const void*>(&hook_create_contact));
    logf("walking: contact hook %s", ok ? "installed" : "FAILED");
}

}  // namespace longshot
