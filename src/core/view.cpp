#include "longshot.h"

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace longshot {

namespace {

constexpr char kSetFov[] = "?set_fov@frustum_t@prism@@QAEXM@Z";
constexpr char kProjection[] =
    "?r_frustum_projection_matrix@prism@@YAXQAV?$mat_sq_t@M$03@1@ABV?$rectangle_t@M@1@MMMM@Z";
constexpr char kUiInit[] = "?ui_init@prism@@YAXXZ";
constexpr char kUiProcess[] = "?ui_process@prism@@YAXPAVr_context_t@1@@Z";
constexpr char kCvarSet[] = "?set@cvar_t@prism@@QAE_NQBD@Z";
constexpr char kUiSetRect[] = "?ui_set_viewport_rect@prism@@YAXABV?$rectangle_t@M@1@@Z";
constexpr char kUiReferenceMode[] = "?ui_get_reference_mode@prism@@YAXQAI0@Z";
constexpr char kUiRef2View[] =
    "?uiref2view@ui_view_t@prism@@SAXPAV?$rectangle_t@M@2@ABVui_rect_t@2@HH@Z";
constexpr char kUiWinsert[] = "?ui_winsert@prism@@YAPAVc_item_t@1@PBD0I0_N@Z";
constexpr char kUiWshow[] = "?ui_wshow@prism@@YAXPAVc_item_t@1@I@Z";
constexpr char kUiWremove[] = "?ui_wremove@prism@@YAXPAVc_item_t@1@@Z";
constexpr char kHudLayout[] = "/ui/ghud.esc";

// The game object, and the world viewport inside it: left, right, bottom, top in pixels.
constexpr uint32_t kGameObject = 0x1e9d60;
constexpr size_t kMainViewport = 0x2a4;

// Methods of the game object. The pause key uses kMethodPause, and while it's set the game frame
// doesn't advance the world.
constexpr size_t kMethodPause = 0x28;
constexpr size_t kMethodIsPaused = 0x2c;

// Where ui_wshow returns to inside the hunt's enter-gameplay method. Nothing else shows the HUD
// like this, and the menu scene is a different class that doesn't have that method.
constexpr uint32_t kEnterGameplayReturn = 0x3684;

// cvar_t objects that live in game.dll's data. Each is checked by name before use.
constexpr uint32_t kCvarQuality = 0x1e4eb8;
constexpr uint32_t kCvarGrass = 0x1e4c48;
constexpr uint32_t kCvarTreeLod = 0x1e4908;
constexpr uint32_t kCvarZclip = 0x1e4b78;
constexpr uint32_t kCvarShadows = 0x1e49d8;
constexpr uint32_t kCvarWater = 0x1e4aa8;
constexpr uint32_t kCvarParticles = 0x1e4768;
constexpr uint32_t kCvarUiMap = 0x1e45c8;
constexpr uint32_t kCvarMode = 0x1e5398;
constexpr uint32_t kCvarFullscreen = 0x1e5468;

// cvar_t layout
constexpr size_t kCvarReadOnly = 0x69;
constexpr size_t kCvarValue = 0xb4;
constexpr size_t kCvarDefault = 0xb8;
constexpr size_t kCvarLink = 0xbc;

constexpr float kBaseAspect = 4.0f / 3.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr float kMaxFov = 170.0f;

using SetFovFn = void(__thiscall*)(void*, float);
using ProjectionFn = void(__cdecl*)(void*, const float*, float, float, float, float);
using UiInitFn = void(__cdecl*)();
using UiProcessFn = void(__cdecl*)(void*);
using CvarSetFn = bool(__thiscall*)(void*, const char*);
using UiSetRectFn = void(__cdecl*)(const float*);
using UiReferenceModeFn = void(__cdecl*)(unsigned int*, unsigned int*);
using UiRef2ViewFn = void(__cdecl*)(float*, const float*, int, int);
using UiWinsertFn = void*(__cdecl*)(const char*, const char*, unsigned int, const char*, bool);
using UiWshowFn = void(__cdecl*)(void*, unsigned int);
using UiWremoveFn = void(__cdecl*)(void*);
using PauseFn = void(__thiscall*)(void*, bool);
using IsPausedFn = bool(__thiscall*)(void*);

SetFovFn g_real_set_fov = nullptr;
ProjectionFn g_real_projection = nullptr;
UiInitFn g_real_ui_init = nullptr;
UiProcessFn g_real_ui_process = nullptr;
CvarSetFn g_cvar_set = nullptr;
UiSetRectFn g_ui_set_rect = nullptr;
UiReferenceModeFn g_ui_reference_mode = nullptr;
UiRef2ViewFn g_real_ui_ref2view = nullptr;
UiWinsertFn g_real_ui_winsert = nullptr;
UiWshowFn g_real_ui_wshow = nullptr;
UiWremoveFn g_real_ui_wremove = nullptr;

char* g_game = nullptr;
bool g_verified = false;
bool g_ui_ready = false;
float g_ui_rect[4] = {};
bool g_ui_rect_valid = false;
bool g_world_drawn = false;
void* g_hud_window = nullptr;
bool g_in_hunt = false;
bool g_background_paused = false;

void set_in_hunt(bool in_hunt) {
    if (in_hunt != g_in_hunt) logf("view: hunt %s", in_hunt ? "started" : "ended");
    g_in_hunt = in_hunt;
}

float g_zoom_target = 0.0f;
float g_zoom_shown = 0.0f;
bool g_was_zoomed = false;
LONGLONG g_zoom_last_tick = 0;

float seconds_since_last_zoom_update() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const LONGLONG elapsed = g_zoom_last_tick ? now.QuadPart - g_zoom_last_tick : 0;
    g_zoom_last_tick = now.QuadPart;
    return static_cast<float>(elapsed) / static_cast<float>(frequency.QuadPart);
}

// A scope writes each zoom level straight into the game's field of view, so a wheel notch is an
// instant jump. This glides what is shown toward that value. Raising or lowering the sight stays
// instant, since the overlay changes in the same frame.
float update_zoom(float target) {
    const float seconds = seconds_since_last_zoom_update();
    const float time_constant = g_cfg.zoom_smoothing_ms / 1000.0f;
    const bool zoomed = input_is_zoomed();

    if (zoomed && g_was_zoomed && time_constant > 0.0f && g_zoom_shown > 0.0f) {
        g_zoom_shown = view_zoom_step(g_zoom_shown, target, seconds, time_constant);
    } else {
        g_zoom_shown = target;
    }
    g_was_zoomed = zoomed;
    g_zoom_target = target;
    return g_zoom_shown;
}

char* game_object() { return g_verified ? *reinterpret_cast<char**>(g_game + kGameObject) : nullptr; }

const float* main_viewport() {
    char* game = game_object();
    return game ? reinterpret_cast<const float*>(game + kMainViewport) : nullptr;
}

// The engine keeps horizontal FOV fixed and derives vertical from it, so wider screens
// lose picture off the top and bottom. Widening the horizontal angle to match the aspect
// keeps the vertical view the game was framed for at 4:3.
void __fastcall hook_set_fov(void* frustum, void*, float fov) {
    const float shown = update_zoom(fov);
    input_set_fov(shown);
    g_real_set_fov(frustum, view_adjust_fov(shown, g_mode.aspect));
}

void __cdecl hook_projection(void* matrix, const float* rect, float near_plane, float far_plane,
                             float fov, float aspect) {
    if (fov == g_zoom_target) fov = g_zoom_shown;
    const float width = rect[1] - rect[0];
    const float height = rect[2] - rect[3];
    float effective = aspect != 0.0f ? aspect : (height != 0.0f ? width / height : 0.0f);

    // The world camera always passes the full window size here, even when the world is
    // showing in a menu panel, so the real viewport is what says which case this is.
    const float screen_width = static_cast<float>(g_mode.width);
    const bool world_camera = width >= screen_width - 1.0f;
    if (world_camera) {
        const float* view = main_viewport();
        const float view_width = view ? view[1] - view[0] : screen_width;
        const float view_height = view ? view[2] - view[3] : 0.0f;
        if (view_width >= screen_width - 1.0f || view_height <= 0.0f) {
            g_world_drawn = true;
        } else {
            effective = view_width / view_height;
            aspect = effective;
            set_in_hunt(false);
        }
    }
    g_real_projection(matrix, rect, near_plane, far_plane, view_adjust_fov(fov, effective), aspect);
}

// The engine scales UI rectangles to pixels here without consulting the UI rect, so 3D
// panels in the UI would otherwise stay in their stretched positions.
void __cdecl hook_ui_ref2view(float* out, const float* in, int width, int height) {
    g_real_ui_ref2view(out, in, width, height);
    if (!g_ui_rect_valid) return;
    out[0] = view_ui_to_pixel(in[0], g_ui_rect[0], g_ui_rect[1], width);
    out[1] = view_ui_to_pixel(in[1], g_ui_rect[0], g_ui_rect[1], width);
}

// The world can be drawing full-screen with no hunt running, behind the intro and before the
// main menu gives it a panel. So a hunt is tracked from the game entering and leaving play.
void* __cdecl hook_ui_winsert(const char* group, const char* name, unsigned int flags,
                              const char* path, bool show) {
    void* window = g_real_ui_winsert(group, name, flags, path, show);
    if (path && !strcmp(path, kHudLayout)) g_hud_window = window;
    return window;
}

void __cdecl hook_ui_wshow(void* window, unsigned int flags) {
    const auto offset =
        reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_game);
    if (offset == kEnterGameplayReturn) set_in_hunt(true);
    g_real_ui_wshow(window, flags);
}

void __cdecl hook_ui_wremove(void* window) {
    if (window && window == g_hud_window) {
        g_hud_window = nullptr;
        set_in_hunt(false);
    }
    g_real_ui_wremove(window);
}

char* cvar(uint32_t rva, const char* name) {
    char* object = g_game + rva;
    if (strcmp(object, name) != 0) {
        logf("view: cvar %s not at its expected address", name);
        return nullptr;
    }
    return object;
}

const char* cvar_value(char* object) {
    while (char* next = *reinterpret_cast<char**>(object + kCvarLink)) object = next;
    const size_t field = object[kCvarReadOnly] ? kCvarDefault : kCvarValue;
    return *reinterpret_cast<const char**>(object + field);
}

void cvar_force(uint32_t rva, const char* name, const char* value) {
    char* object = cvar(rva, name);
    if (!object) return;
    const bool ok = g_cvar_set(object, value);
    logf("view: %s = %s%s", name, value, ok ? "" : " FAILED");
}

// hu2010.exe picks a quality preset before starting the engine. Longshot starts the
// engine directly, so a fresh profile would otherwise keep the -1 placeholder.
void apply_default_quality() {
    char* quality = cvar(kCvarQuality, "g_quality");
    if (!quality) return;
    const char* value = cvar_value(quality);
    logf("view: g_quality is %s", value ? value : "(null)");
    if (!value || strcmp(value, "-1") != 0) return;

    cvar_force(kCvarQuality, "g_quality", "2");
    cvar_force(kCvarGrass, "g_grass_density", "1");
    cvar_force(kCvarTreeLod, "g_tree_lodswitch", "1.0");
    cvar_force(kCvarZclip, "g_zclip", "1.0");
    cvar_force(kCvarShadows, "g_shadows", "3");
    cvar_force(kCvarWater, "g_water_quality", "1");
    cvar_force(kCvarParticles, "g_particles", "1");
    cvar_force(kCvarUiMap, "g_ui_map", "1");
}

void apply_video_mode() {
    char mode[64];
    snprintf(mode, sizeof(mode), "%dx%dx32x%d", g_mode.width, g_mode.height, g_mode.refresh);
    cvar_force(kCvarMode, "r_mode", mode);
    cvar_force(kCvarFullscreen, "r_fullscreen", "1");
}

// ui_set_viewport_rect is the engine's own mapping from the UI's 1024x768 layout to the
// screen, and both drawing and the mouse cursor read it. Widening the rect leaves the
// layout centred at correct proportions.
void compute_ui_rect() {
    if (g_cfg.ui_layout != UiLayout::Center43 || !g_ui_set_rect || !g_ui_reference_mode) return;

    unsigned int ref_width = 0, ref_height = 0;
    g_ui_reference_mode(&ref_width, &ref_height);
    if (!ref_width || !ref_height) return;

    const float layout_width = static_cast<float>(ref_width);
    const float layout_height = static_cast<float>(ref_height);
    const float visible_width = layout_height * g_mode.aspect;
    if (visible_width <= layout_width) return;

    g_ui_rect[0] = (layout_width - visible_width) * 0.5f;
    g_ui_rect[1] = (layout_width + visible_width) * 0.5f;
    g_ui_rect[2] = layout_height;
    g_ui_rect[3] = 0.0f;
    g_ui_rect_valid = true;
    logf("view: ui layout %ux%u mapped to %.1f..%.1f", ref_width, ref_height, g_ui_rect[0],
         g_ui_rect[1]);
}

// game.dll reads r_mode and r_fullscreen straight after ui_init returns, once the config
// files have loaded, so this is the last moment to set them.
void __cdecl hook_ui_init() {
    g_real_ui_init();
    logf("view: ui_init finished");
    g_ui_ready = true;

    if (g_verified && g_cvar_set) {
        apply_default_quality();
        apply_video_mode();
        input_game_ready();
        menu_game_ready();
    }
    compute_ui_rect();
    view_per_frame();
}

// The game draws the whole UI here, once a frame, after the world.
void __cdecl hook_ui_process(void* context) {
    display_before_ui();
    g_real_ui_process(context);
}

}  // namespace

float view_adjust_fov(float fov, float aspect) {
    if (g_cfg.fov_mode != FovMode::HorPlus) return fov;
    float half_tan = tanf(fov * 0.5f * kDegToRad) * g_cfg.fov_scale;
    if (aspect > kBaseAspect + 0.01f) half_tan *= aspect / kBaseAspect;
    const float adjusted = 2.0f * atanf(half_tan) / kDegToRad;
    return adjusted < kMaxFov ? adjusted : kMaxFov;
}

float view_ui_to_pixel(float x, float left, float right, int width) {
    return (x - left) * static_cast<float>(width) / (right - left);
}

float view_zoom_step(float current, float target, float seconds, float time_constant) {
    const float from = std::log(tanf(current * 0.5f * kDegToRad));
    const float to = std::log(tanf(target * 0.5f * kDegToRad));
    const float blended = to + (from - to) * std::exp(-seconds / time_constant);
    return 2.0f * atanf(std::exp(blended)) / kDegToRad;
}

void view_per_frame() {
    if (g_ui_ready && g_ui_rect_valid) g_ui_set_rect(g_ui_rect);
}

int view_background_pause_step(bool* ours, bool in_hunt, bool focused, bool game_paused) {
    const bool want = in_hunt && !focused;
    if (want == *ours || (want && game_paused)) return 0;
    *ours = want;
    return want ? 1 : -1;
}

void view_update_background_pause() {
    char* game = game_object();
    if (!g_cfg.pause_in_background || !game) return;

    void** methods = *reinterpret_cast<void***>(game);
    auto is_paused = reinterpret_cast<IsPausedFn>(methods[kMethodIsPaused / sizeof(void*)]);
    const int change =
        view_background_pause_step(&g_background_paused, g_in_hunt, game_has_focus(), is_paused(game));
    if (change == 0) return;

    // The pause key also switches the sound tag, but that destroys any sound that doesn't loop,
    // music included. Windows silences the sound by itself here. See sound_attach_ds8.
    const bool pause = change > 0;
    reinterpret_cast<PauseFn>(methods[kMethodPause / sizeof(void*)])(game, pause);
    logf("view: hunt %s", pause ? "paused in the background" : "resumed");
}

int view_take_bar_width() {
    const bool world_drawn = g_world_drawn;
    g_world_drawn = false;
    // An unknown build can't track hunts, so it falls back on the world being drawn at all.
    const bool world_fills_bars = world_drawn && (g_in_hunt || !g_verified);
    if (world_fills_bars || !g_ui_rect_valid) return 0;
    const float edge = view_ui_to_pixel(0.0f, g_ui_rect[0], g_ui_rect[1], g_mode.width);
    return static_cast<int>(edge + 0.5f);
}

void view_attach_game(HMODULE game, bool verified) {
    g_game = reinterpret_cast<char*>(game);
    g_verified = verified;

    HMODULE shared = GetModuleHandleW(L"p3shared.dll");
    if (shared) {
        g_cvar_set = reinterpret_cast<CvarSetFn>(GetProcAddress(shared, kCvarSet));
        g_ui_set_rect = reinterpret_cast<UiSetRectFn>(GetProcAddress(shared, kUiSetRect));
        g_ui_reference_mode =
            reinterpret_cast<UiReferenceModeFn>(GetProcAddress(shared, kUiReferenceMode));
    }

    const bool ui = iat_patch(game, "p3shared.dll", kUiInit, reinterpret_cast<void*>(&hook_ui_init),
                              reinterpret_cast<void**>(&g_real_ui_init));
    const bool fov = iat_patch(game, "p3core.dll", kSetFov, reinterpret_cast<void*>(&hook_set_fov),
                               reinterpret_cast<void**>(&g_real_set_fov));
    const bool projection =
        iat_patch(game, "p3core.dll", kProjection, reinterpret_cast<void*>(&hook_projection),
                  reinterpret_cast<void**>(&g_real_projection));
    const bool panels =
        iat_patch(game, "p3shared.dll", kUiRef2View, reinterpret_cast<void*>(&hook_ui_ref2view),
                  reinterpret_cast<void**>(&g_real_ui_ref2view));
    const bool winsert =
        iat_patch(game, "p3shared.dll", kUiWinsert, reinterpret_cast<void*>(&hook_ui_winsert),
                  reinterpret_cast<void**>(&g_real_ui_winsert));
    const bool wshow =
        iat_patch(game, "p3shared.dll", kUiWshow, reinterpret_cast<void*>(&hook_ui_wshow),
                  reinterpret_cast<void**>(&g_real_ui_wshow));
    const bool wremove =
        iat_patch(game, "p3shared.dll", kUiWremove, reinterpret_cast<void*>(&hook_ui_wremove),
                  reinterpret_cast<void**>(&g_real_ui_wremove));
    const bool process =
        iat_patch(game, "p3shared.dll", kUiProcess, reinterpret_cast<void*>(&hook_ui_process),
                  reinterpret_cast<void**>(&g_real_ui_process));
    logf("view: game.dll hooks ui=%d fov=%d projection=%d panels=%d windows=%d%d%d draw=%d", ui, fov,
         projection, panels, winsert, wshow, wremove, process);
}

}  // namespace longshot
