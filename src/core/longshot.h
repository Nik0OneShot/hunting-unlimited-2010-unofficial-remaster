#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace longshot {

enum class FovMode { Stock, HorPlus };
enum class UiLayout { Stretch, Center43 };
enum class HudLayout { Edges, Wide169, Center43 };
enum class ZoomMode { Stock, Off, Proportional };

struct Config {
    int width = 0;
    int height = 0;
    int refresh = 0;
    bool vsync = true;
    int fps_cap = 0;

    FovMode fov_mode = FovMode::HorPlus;
    float fov_scale = 1.0f;
    float zoom_smoothing_ms = 60.0f;
    float menu_scroll_smoothing_ms = 45.0f;
    UiLayout ui_layout = UiLayout::Center43;
    HudLayout hud_layout = HudLayout::Edges;

    bool raw_input = true;
    bool fps_independent = true;
    float reference_hz = 240.0f;
    float sensitivity_x = 1.0f;
    float sensitivity_y = 1.0f;
    ZoomMode zoom_mode = ZoomMode::Proportional;

    bool high_res_timer = true;

    bool skip_intro = false;
    bool pause_in_background = false;
};

// The mode the game is asked to run in, resolved from the config and the desktop.
struct DisplayMode {
    int width = 0;
    int height = 0;
    int refresh = 0;
    float aspect = 4.0f / 3.0f;
    bool matches_desktop = false;
};

extern Config g_cfg;
extern DisplayMode g_mode;

// Paths
const std::wstring& root_dir();
std::wstring logs_dir();
void set_root(HMODULE self);

// log.cpp
void log_open();
void logf(const char* fmt, ...);

// config.cpp
void config_load();

// patch.cpp
// Redirects one import of `mod`. Returns false when the module does not import it.
bool iat_patch(HMODULE mod, const char* dll, const char* func, void* hook, void** original);
// Replaces the start of a function that will never be called again with a jump.
bool write_jump(void* at, void* to);
// Redirects one entry of a table of method pointers.
bool vtable_patch(void** slot, void* hook, void** original);
// Overwrites code, but only if it still holds exactly the expected bytes.
bool patch_bytes(void* at, const void* expected, const void* replacement, size_t size);
// True when `mod` is exactly the build these offsets were derived from.
bool module_matches(HMODULE mod, uint32_t timestamp, uint32_t image_size);

// crash.cpp
void crash_install();

// timing.cpp
void timing_install(HMODULE p3core);
double now_seconds();

// menu.cpp
void menu_attach_game(HMODULE game, bool verified);
void menu_game_ready();
float menu_glide_step(float shown, float seconds, float time_constant);
float menu_glide_add(float shown, float delta, float limit);

// layout.cpp
void layout_install(HMODULE p3core, HMODULE p3shared);
// The text a layout file should have at this aspect, or empty to leave it as it is.
std::string layout_rewrite(const char* path, const std::string& original, float aspect,
                           HudLayout hud);

// sound.cpp
void sound_attach_ds8(HMODULE ds8, bool verified);

// display.cpp
void display_resolve_mode();
void display_install(HMODULE exe);
void display_attach_gl(HMODULE gl);
// Clears the side bars just before the UI draws, so the UI can draw over them.
void display_before_ui();

// view.cpp
void view_attach_game(HMODULE game, bool verified);
void view_per_frame();
float view_adjust_fov(float fov, float aspect);
float view_ui_to_pixel(float x, float left, float right, int width);
// Moves a field of view toward its target. Works in zoom ratios, so the rate looks even.
float view_zoom_step(float current, float target, float seconds, float time_constant);
// Width in pixels of each side bar that nothing drew this frame, or 0. Resets for the next frame.
int view_take_bar_width();
// Pauses a hunt while the game is in the background, when the player has asked for that.
void view_update_background_pause();
// 1 to pause, -1 to resume, 0 to leave it. `ours` says whether Longshot set the current pause, so a
// pause the player set themselves is left in place.
int view_background_pause_step(bool* ours, bool in_hunt, bool focused, bool game_paused);

// input.cpp
void input_attach_di8(HMODULE di8);
void input_attach_game(HMODULE game, bool verified);
void input_game_ready();
void input_set_fov(float game_fov_degrees);
// True while looking through a scope, binoculars or iron sights.
bool input_is_zoomed();
int input_test_wheel();
bool input_rawmouse_start();
void input_rawmouse_read(long* dx, long* dy);
long input_rawmouse_events();

// True while one of this process's windows is in the foreground.
bool game_has_focus();

}  // namespace longshot
