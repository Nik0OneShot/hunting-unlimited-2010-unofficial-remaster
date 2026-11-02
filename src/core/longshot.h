#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
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
    int anti_aliasing = 0;
    bool smooth_foliage = true;
    int texture_filtering = 0;
    float grass_distance = 0.0f;
    float lod_distance = 1.0f;
    bool occlusion_culling = true;
    float sharpening = 0.0f;
    float vibrance = 0.0f;
    float bloom = 0.0f;

    bool raw_input = true;
    bool fps_independent = true;
    float reference_hz = 240.0f;
    float sensitivity_x = 1.0f;
    float sensitivity_y = 1.0f;
    ZoomMode zoom_mode = ZoomMode::Proportional;
    float cm_per_360 = 0.0f;
    int mouse_dpi = 800;

    bool high_res_timer = true;

    bool skip_intro = false;
    bool pause_in_background = false;
    bool straight_walking = true;
    bool weapon_queue = true;
    float action_speed = 1.0f;
    bool strong_atv = true;
    int freelook_key = 0xA4;  // a Windows virtual key, left alt, and 0 for none
    float freelook_yaw = 100.0f;
    float freelook_pitch = 60.0f;
    bool stand_hidden = true;
    bool stand_clear_view = true;
    bool faster_walking = true;
    float walk_speed = 1.9f;
    float side_speed = 0.8f;
    bool speed_noise = true;
    bool irons_view_sway = true;
    bool free_hunt_opening = false;
    bool free_hunt_animals = true;
    bool free_hunt_calm_animals = true;
    bool free_hunt_any_marker = true;
    bool free_hunt_wildlife = true;
    float free_hunt_day_minutes = 48.0f;  // how long a whole day takes, and 0 stops the clock
    float free_hunt_night_light = 1.0f;
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
std::string config_read(const wchar_t* section, const wchar_t* key, const char* fallback);
void config_write(const wchar_t* section, const wchar_t* key, const char* value);

// patch.cpp
// Redirects one import of `mod`. Returns false when the module does not import it.
bool iat_patch(HMODULE mod, const char* dll, const char* func, void* hook, void** original);
// Replaces the start of a function that will never be called again with a jump.
bool write_jump(void* at, void* to);
// Swaps a call to a fixed address for one to the hook, leaving the function whole for other callers.
bool patch_call(void* site, const void* target, const void* hook);
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

// profile.cpp
// Sections are timed separately, so a slow frame in the log says where its time went. Files,
// Uploads, Post, Grass, Terrain, Decode, Queues and Occlusion happen inside the others, and are
// counted on their own as well.
enum class ProfileSection { Sim, World, Ui, Swap, Sound, Files, Uploads, Post, Grass, Terrain, Decode, Queues, Occlusion, Count };

class ProfileTimer {
   public:
    explicit ProfileTimer(ProfileSection section);
    ~ProfileTimer();

   private:
    ProfileSection section_;
    double started_;
};

void profile_attach_module(HMODULE mod);
void profile_attach_game(HMODULE game, bool verified);
void profile_game_ready();
void* profile_wrap_gl(const char* name, void* proc);
void profile_ui_begin();
void profile_ui_end();
bool profile_is_slow(float frame_ms, float typical_ms);
void profile_frame_done(float frame_ms, float typical_ms, bool focused);
void profile_grass_block(double seconds);
// How long a section has taken so far this frame.
double profile_frame_seconds(ProfileSection section);
void profile_report_window();

// menu.cpp
void menu_attach_game(HMODULE game, bool verified);
void menu_game_ready();
float menu_glide_step(float shown, float seconds, float time_constant);
float menu_glide_add(float shown, float delta, float limit);
// True if a row at rest between `top` and `bottom` stays with the rest of its list after `shift`.
bool menu_row_fits(float top, float bottom, float shift, float rows_top, float rows_bottom,
                   float overhang);

// layout.cpp
void layout_install(HMODULE p3core, HMODULE p3shared);
// The game opens some files itself, like a map's birds, rather than through p3shared.
void layout_attach_game(HMODULE game, bool verified);
// Materials and effects are opened by the graphics and loader modules, so they need the same hook.
void layout_attach_module(HMODULE module, const char* name);
// The text a layout file should have at this aspect, or empty to leave it as it is.
std::string layout_rewrite(const char* path, const std::string& original, float aspect,
                           HudLayout hud);
// A whole file from the game's own files, or false if there's no such file.
bool layout_read_file(const char* path, std::string* text);

// graphics.cpp
void graphics_attach_gl(HMODULE gl);
void* graphics_wrap_gl(const char* name, void* proc);
// An OpenGL function by name, whether it's built into opengl32.dll or comes from the driver.
void* graphics_gl_function(const char* name);
// The sample count a window could get, or 0. Needs a window with no pixel format yet.
int graphics_test_multisample(HDC dc, int samples);
// Runs `work` with a throwaway OpenGL context current, on a hidden window `size` pixels square.
void graphics_in_context(int size, int samples, const std::function<void()>& work);

// weapons.cpp
void weapons_attach_game(HMODULE game, bool verified);
// Needs the game's input functions to be bound, so it comes after ui_init.
void weapons_game_ready();
void weapons_end_frame();
void weapons_settings_changed();
// 0 if the input queue behaves, or which check failed.
int weapons_test();

// ODE's dContact, as built into this game.
struct Contact {
    unsigned mode;
    float surface[10];
    float position[4];
    float normal[4];
    float depth;
    char* first;
    char* second;
    int sides[2];
    float friction_direction[4];
};
static_assert(sizeof(Contact) == 0x70, "the game's contacts are 0x70 bytes");

// walking.cpp
void walking_attach_game(HMODULE game, bool verified);
void walking_settings_changed();
// How far off course a walk went, in degrees, with left as negative.
float walking_drift(float along, float across);
// How much of the player's noise animals can hear, and how loud a footstep is, at a speed in m/s.
float walking_noise(float speed);
float walking_step_volume(float speed);
// The player in the current hunt, or null, and which kind of view they're in.
char* walking_player();
const void* walking_view_class(char* player);
// The kind of ground a footstep should sound like.
unsigned walking_step_ground(unsigned ground);

// vehicle.cpp
void vehicle_attach_game(HMODULE game, bool verified);
void vehicle_settings_changed();
// Called with every contact that isn't the player's.
void vehicle_contact(Contact* contact);
// 0 if tyres are told from everything else, or which check failed.
int vehicle_test();

// What the flashlight and lantern want from post-processing this frame. `lift` is how much a pixel
// in full light is multiplied by, for red, green and blue.
struct PostLight {
    float beam;
    float glow;
    float lift[3];
};

// stash.cpp
void stash_attach_game(HMODULE game, bool verified);
// The interact key's label for stashing, after the game's own.
std::string stash_add_label(const std::string& text);
// Finds the map's camps as it loads, from the path of its landscape.
void stash_map_loading(const char* landscape);
void stash_end_frame();
// 0 if finding camps and the label behave, or which check failed.
int stash_test();

// lights.cpp
void lights_attach_game(HMODULE game, bool verified);
// False on an unknown build, where they couldn't be switched on.
bool lights_available();
// The game's equipment list with the two items added.
std::string lights_add_items(const std::string& equipment);
void lights_hunt_started();
bool lights_wanted();
bool lights_beam_on();
bool lights_glow_on();
bool lights_this_frame(PostLight* light);
// 0 if the items and how far light lifts a picture behave, or which check failed.
int lights_test();

// daytime.cpp
void daytime_attach_game(HMODULE game, bool verified);
void daytime_hunt_started();
void daytime_hunt_ended();
void daytime_end_frame();
void daytime_before_render();
// The hour of the day, with its fraction, or below zero if the clock isn't running.
float daytime_hour();
// 0 by day and 1 at night, or 0 when the clock isn't running.
float daytime_darkness();
float daytime_light_level();
// 0 if the clock and the moonlight behave, or which check failed.
int daytime_test();

// freehunt.cpp
void free_hunt_attach_game(HMODULE game, bool verified);
// Where the view is and which way it faces, or false outside a hunt.
bool free_hunt_read_view(float* position, float* forward);
// How deep the water is at a place, which is 0 on dry land, and how high the ground is there.
// False with no hunt loaded.
bool free_hunt_water_depth(const float* place, float* depth, float* ground = nullptr);
void free_hunt_settings_changed();
void free_hunt_end_frame();
// In a free hunt, and not paused.
bool free_hunt_running();
// 0 if the choice of animal and place behaves, or which check failed.
int free_hunt_test();

// What wildlife changes about one animal's needs while it decides what to do, to put back afterwards.
struct WildNeeds {
    bool changed;
    float hunger, thirst, tiredness, prey, hunter_fear, enemy_fear, lure, aggression;
    float used_hunger, used_thirst, used_tiredness, used_prey, used_hunter_fear, used_enemy_fear, used_lure, used_aggression;
    bool hid_hurt;
};

// wildlife.cpp
void wildlife_attach_game(HMODULE game, bool verified);
// False on an unknown build, or with the setting off.
bool wildlife_available();
// The game's files with Longshot's herds, carcasses and birds.
std::string wildlife_species_text(const std::string& original);
std::string wildlife_base_text(const std::string& original);
// Bigger blood and prints, and more of them kept.
std::string wildlife_sticker_text(const std::string& original);
// Blood multiplied with the ground, rather than drawn over it unlit. Empty when there's nothing to change.
std::string wildlife_blood_material_text(const std::string& original);
std::string wildlife_blood_state_text(const std::string& original);
std::string wildlife_birds_text(const std::string& original);
// While set, the generator keeps a herd together rather than splitting it into groups.
void wildlife_whole_herd(bool whole);
// How big a herd of a species should arrive, and what to ask the generator for to get that.
int wildlife_herd_size(const char* species, int short_of);
int wildlife_herd_group(int size);
bool wildlife_near_water(const float* place);
// For a species that lives up high, moves a place to the best high ground near it, at least nearest
// from the hunter. False, and the place as it was, for anything else.
bool wildlife_high_ground(const char* species, const float* hunter, float nearest, float* place);
// Spreads out big groups and adds young, before the animals are made.
void wildlife_settle_herds(void* animals, char* definitions);
void wildlife_hunt_started(int hour);
void wildlife_hunt_ended();
void wildlife_end_frame();
void wildlife_before_needs(char* brain, WildNeeds* needs);
void wildlife_after_needs(char* brain, const WildNeeds& needs);
// 0 if the numbers, the text changes and the timing behave, or which check failed.
int wildlife_test();

// terrain.cpp
void terrain_attach_game(HMODULE game, bool verified);
// Logs how much terrain had to be loaded since the last report, if any did.
void terrain_report();
// 0 if the chunks let go of to make room are the right ones, or which check failed.
int terrain_test();

// freelook.cpp
void freelook_attach_game(HMODULE game, bool verified);
// While the key is held on foot, the mouse turns the head and not the body.
bool freelook_takes_mouse();
// Walking, rather than driving or up a stand.
bool freelook_on_foot();
void freelook_turn(bool sideways, float radians);
void freelook_before_render();
// Throws the view along a direction on the ground, by how much the push was from behind and from
// the side, and lets it spring back. Strength is 0 to 1.
void freelook_shove(const float* push, float back, float side, float strength);
// 0 if the head stops where it should and comes back, or which check failed.
int freelook_test();

// stand.cpp
void stand_attach_game(HMODULE game, bool verified);
void stand_before_world();
void stand_after_world();
void* stand_wrap_gl(const char* name, void* proc);
// The game's leaf program with near leaves thinned out, or empty for any other program.
std::string stand_leaf_program(const std::string& program);
// 0 if the leaf program changes as it should and the driver takes it, or which check failed.
int stand_test();

// paper.cpp
void paper_attach_game(HMODULE game, bool verified);
// False on an unknown build, where the two items would only be more maps.
bool paper_available();
// While the calculator or range sheet is up, the mouse works it and not the view.
bool paper_takes_mouse();
void paper_mouse_moved(long dx, long dy);
void paper_mouse_down(int button);
// Draws over the finished frame, just before it's shown.
void paper_draw();
// The game's equipment list with the two items added.
std::string paper_add_items(const std::string& equipment);
// 0 if the sums, the reading of ammo files and the drawing behave, or which check failed.
int paper_test();

// occlusion.cpp
void occlusion_attach_game(HMODULE game, bool verified);
// Logs how much was hidden since the last report, if anything was looked at.
void occlusion_report();
// 0 if the terrain hides what it should and nothing else, or which check failed.
int occlusion_test();

// detail.cpp
void detail_attach_game(HMODULE game, bool verified);
void detail_settings_changed();
float detail_scale(float reference_fov, float fov, float multiplier);
float detail_glare_fov(float fov);
float detail_grass_radius(float visible);

// post.cpp
// Runs after the world is drawn and before the UI, so it never touches the HUD or menus.
void post_render();
// Keeps the depth of the world that's just been drawn, as the weapon clears it, while a light is on.
void post_capture_world(const float* projection);
// 0 if the effects work on this PC's driver, or which check failed.
int post_test();

// sound.cpp
void sound_attach_ds8(HMODULE ds8, bool verified);

// settings.cpp
void settings_attach_game(HMODULE game, bool verified);
// False on an unknown build, where the options menu can't be extended.
bool settings_available();
// The Longshot tab button, and the page it shows, as layout text for the options menu.
std::string settings_tab_entity();
std::string settings_page_entity();

// display.cpp
void display_resolve_mode();
void display_install(HMODULE exe);
void display_attach_gl(HMODULE gl);
// Clears the side bars just before the UI draws, so the UI can draw over them.
void display_before_ui();
// Picks up display settings that changed while the game was running.
void display_settings_changed();

// view.cpp
void view_attach_game(HMODULE game, bool verified);
// A hunt is on screen and the world is moving.
bool view_hunt_running();
// The field of view the game asked for, before Longshot widens it, or 0 for any other frustum.
float view_game_fov(const void* frustum);
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
void input_attach_di8(HMODULE di8, bool verified);
void input_attach_game(HMODULE game, bool verified);
void input_game_ready();
void input_set_fov(float game_fov_degrees);
// True while looking through a scope, binoculars or iron sights.
bool input_is_zoomed();
// What to multiply aim speed by so that a turn takes `cm_per_360`, whatever the in-game slider says.
float input_turn_scale(float cm_per_360, int dpi, float slider, float reference_hz);
int input_test_wheel();
bool input_rawmouse_start();
void input_rawmouse_read(long* dx, long* dy);
long input_rawmouse_events();

// True while one of this process's windows is in the foreground.
bool game_has_focus();

}  // namespace longshot
