#include "longshot.h"

#include <cmath>
#include <cstring>

namespace longshot {

namespace {

constexpr float kDegToRad = 3.14159265f / 180.0f;

constexpr char kGetFov[] = "?get_fov@frustum_t@prism@@QBEMXZ";

// Offsets into game.dll.
constexpr uintptr_t kDetailScale = 0xd01d0;
constexpr uintptr_t kTileRadiusBase = 0x1a873c;
constexpr uintptr_t kGrassFadeX = 0x1e6338;
constexpr uintptr_t kGrassFadeZ = 0x1e6350;
constexpr size_t kCameraFrustum = 0x48;
// The address inside the call that gets the field of view for the sun's glare, and what it holds.
constexpr uintptr_t kGlareFovCall = 0xec734;
constexpr uintptr_t kGetFovSlot = 0x19a4c4;

constexpr unsigned char kDetailScaleStart[] = {0xD9, 0x44, 0x24, 0x04, 0x83, 0xC1, 0x48};
constexpr unsigned char kGameGrassFade[] = {0x89, 0x88, 0x08, 0x3D};

// A shader fades grass out over a disc around the camera, and it has gone by this share of the
// radius, which is where attn.png drops below the alpha test's 0.3.
constexpr float kGrassVisibleShare = 0.805f;
constexpr float kGameGrassRadius = 30.0f;
// The eyelids close when the sun is near the middle of the view, by the sun's angle as a share of
// half the field of view. On a wide screen that takes in most of the sky, so this field of view
// stays narrow. At this width they start closing with the sun 25 degrees off centre.
constexpr float kWidestGlareFov = 50.0f;

// Blocks of grass are 20 m wide, and stop being drawn at 62 m to their centres. The fade has to end
// before their square edges would show.
constexpr float kWidestWithGameReach = 47.0f;
constexpr float kWidestGrass = 85.0f;

struct Patch {
    uintptr_t at;
    unsigned char game[4];
    unsigned char wide[4];
    size_t size;
};

// Walks 5 blocks either way instead of 3, and draws them out to 100 m instead of 62.
constexpr Patch kGrassReach[] = {
    {0xf26fc, {0x8D, 0x41, 0xFD}, {0x8D, 0x41, 0xFB}, 3},
    {0xf26ff, {0x8D, 0x51, 0x03}, {0x8D, 0x51, 0x05}, 3},
    {0xf2706, {0x8D, 0x59, 0xFD}, {0x8D, 0x59, 0xFB}, 3},
    {0xf2709, {0x83, 0xC1, 0x03}, {0x83, 0xC1, 0x05}, 3},
    {0x1aaac4, {0x00, 0x40, 0x70, 0x45}, {0x00, 0x40, 0x1C, 0x46}, 4},
};

// Keeps 255 blocks instead of 98, or the wider reach would rebuild them every frame. The count is
// only ever tested for being equal to the limit, so once it's past 98 this can't go back.
constexpr Patch kGrassCache = {0xf1c6b, {0x83, 0x7F, 0x68, 0x62}, {0x80, 0x7F, 0x68, 0xFF}, 4};

using GetFovFn = float(__fastcall*)(const void*, void*);

char* g_game = nullptr;
GetFovFn g_get_fov = nullptr;
bool g_grass_ready = false;

bool matches_game(const Patch& patch) { return memcmp(g_game + patch.at, patch.game, patch.size) == 0; }

void set_patch(const Patch& patch, bool wide) {
    void* at = g_game + patch.at;
    const unsigned char* from = wide ? patch.game : patch.wide;
    const unsigned char* to = wide ? patch.wide : patch.game;
    if (memcmp(at, to, patch.size) != 0) patch_bytes(at, from, to, patch.size);
}

// Trees and animals keep their detail over ranges set for an 80 degree view, scaled by how far the
// camera's field of view differs from that. It's meant for scopes, but Longshot widens that field
// of view on wide screens, which would cut the ranges short. So this goes by the game's own.
float __fastcall hook_detail_scale(void* camera, void*, float reference_fov) {
    const void* frustum = static_cast<const char*>(camera) + kCameraFrustum;
    const float game_fov = view_game_fov(frustum);
    const float fov = game_fov > 0.0f ? game_fov : g_get_fov(frustum, nullptr);
    return detail_scale(reference_fov, fov, g_cfg.lod_distance);
}

// A whole tile of trees drops to billboards once it's further off than (10 + 70v) * s, which is
// 60 * s short of where single trees finish fading, so the nearer ones pop. Starting from 80
// instead of 10 keeps a tile going until all of its trees have faded.
void fix_tile_radius() {
    const float game = 10.0f;
    const float covering = 80.0f;
    const bool ok = patch_bytes(g_game + kTileRadiusBase, &game, &covering, sizeof(game));
    logf("detail: tree tile radius %s", ok ? "extended" : "FAILED");
}

float __fastcall hook_glare_fov(const void* frustum, void*) {
    const float game_fov = view_game_fov(frustum);
    return detail_glare_fov(game_fov > 0.0f ? game_fov : g_get_fov(frustum, nullptr));
}

void* g_glare_call = reinterpret_cast<void*>(&hook_glare_fov);

void fix_glare() {
    const void* game = g_game + kGetFovSlot;
    void* const ours = &g_glare_call;
    const bool ok = g_get_fov && patch_bytes(g_game + kGlareFovCall, &game, &ours, sizeof(ours));
    logf("detail: sun glare %s", ok ? "narrowed" : "FAILED");
}

void hook_scale() {
    HMODULE core = GetModuleHandleW(L"p3core.dll");
    g_get_fov = core ? reinterpret_cast<GetFovFn>(GetProcAddress(core, kGetFov)) : nullptr;
    void* at = g_game + kDetailScale;
    const bool known = memcmp(at, kDetailScaleStart, sizeof(kDetailScaleStart)) == 0;
    const bool ok = g_get_fov && known && write_jump(at, reinterpret_cast<void*>(&hook_detail_scale));
    logf("detail: detail scale hook %s", ok ? "installed" : "FAILED");
}

void check_grass() {
    g_grass_ready = memcmp(g_game + kGrassFadeX, kGameGrassFade, sizeof(kGameGrassFade)) == 0 &&
                    memcmp(g_game + kGrassFadeZ, kGameGrassFade, sizeof(kGameGrassFade)) == 0 &&
                    matches_game(kGrassCache);
    for (const Patch& patch : kGrassReach) g_grass_ready = g_grass_ready && matches_game(patch);
    logf("detail: grass %s", g_grass_ready ? "found" : "not as expected, leaving it alone");
}

}  // namespace

float detail_scale(float reference_fov, float fov, float multiplier) {
    const float reference = tanf(reference_fov * 0.5f * kDegToRad);
    const float current = tanf(fov * 0.5f * kDegToRad);
    return reference / current * (multiplier > 0.25f ? multiplier : 0.25f);
}

float detail_glare_fov(float fov) { return fov < kWidestGlareFov ? fov : kWidestGlareFov; }

float detail_grass_radius(float visible) {
    const float clamped = visible < kWidestGrass ? visible : kWidestGrass;
    const float radius = clamped / kGrassVisibleShare;
    return radius > kGameGrassRadius ? radius : kGameGrassRadius;
}

void detail_settings_changed() {
    if (!g_grass_ready) return;
    const float radius = detail_grass_radius(g_cfg.grass_distance);
    *reinterpret_cast<float*>(g_game + kGrassFadeX) = 1.0f / radius;
    *reinterpret_cast<float*>(g_game + kGrassFadeZ) = 1.0f / radius;

    const bool wide = g_cfg.grass_distance > kWidestWithGameReach;
    for (const Patch& patch : kGrassReach) set_patch(patch, wide);
    if (wide) set_patch(kGrassCache, true);
    logf("detail: grass reaches %.0f m, lod_distance %.2f", radius * kGrassVisibleShare, g_cfg.lod_distance);
}

void detail_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    hook_scale();
    fix_glare();
    fix_tile_radius();
    check_grass();
    detail_settings_changed();
}

}  // namespace longshot
