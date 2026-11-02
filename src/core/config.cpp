#include "longshot.h"

#include <cstdlib>
#include <cstring>

namespace longshot {

Config g_cfg;

namespace {

std::wstring g_ini;

void read_str(const wchar_t* section, const wchar_t* key, char* out, DWORD size) {
    wchar_t wide[128];
    wide[0] = 0;
    GetPrivateProfileStringW(section, key, L"", wide, 128, g_ini.c_str());
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, size, nullptr, nullptr);
}

int read_int(const wchar_t* section, const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(section, key, fallback, g_ini.c_str());
}

bool read_bool(const wchar_t* section, const wchar_t* key, bool fallback) {
    return read_int(section, key, fallback ? 1 : 0) != 0;
}

float read_float(const wchar_t* section, const wchar_t* key, float fallback) {
    char text[64];
    read_str(section, key, text, sizeof(text));
    if (!text[0]) return fallback;
    char* end = nullptr;
    const float value = static_cast<float>(strtod(text, &end));
    return end != text ? value : fallback;
}

float positive(float value, float fallback) { return value > 0.0f ? value : fallback; }

}  // namespace

std::string config_read(const wchar_t* section, const wchar_t* key, const char* fallback) {
    char text[64];
    read_str(section, key, text, sizeof(text));
    return text[0] ? text : fallback;
}

void config_write(const wchar_t* section, const wchar_t* key, const char* value) {
    wchar_t wide[64];
    MultiByteToWideChar(CP_UTF8, 0, value, -1, wide, 64);
    WritePrivateProfileStringW(section, key, wide, g_ini.c_str());
}

void config_load() {
    g_ini = root_dir() + L"\\longshot.ini";

    g_cfg.width = read_int(L"display", L"width", g_cfg.width);
    g_cfg.height = read_int(L"display", L"height", g_cfg.height);
    g_cfg.refresh = read_int(L"display", L"refresh", g_cfg.refresh);
    g_cfg.vsync = read_bool(L"display", L"vsync", g_cfg.vsync);
    g_cfg.fps_cap = read_int(L"display", L"fps_cap", g_cfg.fps_cap);

    char text[64];
    read_str(L"view", L"fov_mode", text, sizeof(text));
    if (!_stricmp(text, "stock")) g_cfg.fov_mode = FovMode::Stock;
    g_cfg.fov_scale = positive(read_float(L"view", L"fov_scale", g_cfg.fov_scale), 1.0f);
    g_cfg.zoom_smoothing_ms = read_float(L"view", L"zoom_smoothing_ms", g_cfg.zoom_smoothing_ms);
    g_cfg.menu_scroll_smoothing_ms =
        read_float(L"view", L"menu_scroll_smoothing_ms", g_cfg.menu_scroll_smoothing_ms);
    read_str(L"view", L"ui_layout", text, sizeof(text));
    if (!_stricmp(text, "stretch")) g_cfg.ui_layout = UiLayout::Stretch;
    g_cfg.anti_aliasing = read_int(L"view", L"anti_aliasing", g_cfg.anti_aliasing);
    g_cfg.smooth_foliage = read_bool(L"view", L"smooth_foliage", g_cfg.smooth_foliage);
    g_cfg.texture_filtering = read_int(L"view", L"texture_filtering", g_cfg.texture_filtering);
    g_cfg.grass_distance = read_float(L"view", L"grass_distance", g_cfg.grass_distance);
    g_cfg.lod_distance = read_float(L"view", L"lod_distance", g_cfg.lod_distance);
    g_cfg.occlusion_culling = read_bool(L"view", L"occlusion_culling", g_cfg.occlusion_culling);
    g_cfg.sharpening = read_float(L"view", L"sharpening", g_cfg.sharpening);
    g_cfg.vibrance = read_float(L"view", L"vibrance", g_cfg.vibrance);
    g_cfg.bloom = read_float(L"view", L"bloom", g_cfg.bloom);
    read_str(L"view", L"hud_layout", text, sizeof(text));
    if (!_stricmp(text, "16_9")) g_cfg.hud_layout = HudLayout::Wide169;
    else if (!_stricmp(text, "4_3")) g_cfg.hud_layout = HudLayout::Center43;
    else if (!_stricmp(text, "edges")) g_cfg.hud_layout = HudLayout::Edges;

    g_cfg.raw_input = read_bool(L"mouse", L"raw_input", g_cfg.raw_input);
    g_cfg.fps_independent = read_bool(L"mouse", L"fps_independent", g_cfg.fps_independent);
    g_cfg.reference_hz =
        positive(read_float(L"mouse", L"reference_hz", g_cfg.reference_hz), g_cfg.reference_hz);
    g_cfg.sensitivity_x = read_float(L"mouse", L"sensitivity_x", g_cfg.sensitivity_x);
    g_cfg.sensitivity_y = read_float(L"mouse", L"sensitivity_y", g_cfg.sensitivity_y);
    read_str(L"mouse", L"zoom_mode", text, sizeof(text));
    if (!_stricmp(text, "stock")) g_cfg.zoom_mode = ZoomMode::Stock;
    else if (!_stricmp(text, "off")) g_cfg.zoom_mode = ZoomMode::Off;
    else if (!_stricmp(text, "proportional")) g_cfg.zoom_mode = ZoomMode::Proportional;
    g_cfg.cm_per_360 = read_float(L"mouse", L"cm_per_360", g_cfg.cm_per_360);
    g_cfg.mouse_dpi = read_int(L"mouse", L"mouse_dpi", g_cfg.mouse_dpi);
    if (g_cfg.mouse_dpi <= 0) g_cfg.mouse_dpi = 800;

    g_cfg.high_res_timer = read_bool(L"timing", L"high_res_timer", g_cfg.high_res_timer);
    g_cfg.skip_intro = read_bool(L"game", L"skip_intro", g_cfg.skip_intro);
    g_cfg.straight_walking = read_bool(L"game", L"straight_walking", g_cfg.straight_walking);
    g_cfg.weapon_queue = read_bool(L"game", L"weapon_queue", g_cfg.weapon_queue);
    g_cfg.action_speed = read_float(L"game", L"action_speed", g_cfg.action_speed);
    g_cfg.strong_atv = read_bool(L"game", L"strong_atv", g_cfg.strong_atv);
    g_cfg.freelook_key = read_int(L"game", L"freelook_key", g_cfg.freelook_key);
    g_cfg.freelook_yaw = read_float(L"game", L"freelook_yaw", g_cfg.freelook_yaw);
    g_cfg.freelook_pitch = read_float(L"game", L"freelook_pitch", g_cfg.freelook_pitch);
    g_cfg.stand_hidden = read_bool(L"game", L"stand_hidden", g_cfg.stand_hidden);
    g_cfg.stand_clear_view = read_bool(L"game", L"stand_clear_view", g_cfg.stand_clear_view);
    g_cfg.faster_walking = read_bool(L"game", L"faster_walking", g_cfg.faster_walking);
    g_cfg.walk_speed = read_float(L"game", L"walk_speed", g_cfg.walk_speed);
    g_cfg.side_speed = read_float(L"game", L"side_speed", g_cfg.side_speed);
    g_cfg.speed_noise = read_bool(L"game", L"speed_noise", g_cfg.speed_noise);
    g_cfg.irons_view_sway = read_bool(L"game", L"irons_view_sway", g_cfg.irons_view_sway);
    g_cfg.free_hunt_opening = read_bool(L"game", L"free_hunt_opening", g_cfg.free_hunt_opening);
    g_cfg.free_hunt_animals = read_bool(L"game", L"free_hunt_animals", g_cfg.free_hunt_animals);
    g_cfg.free_hunt_calm_animals = read_bool(L"game", L"free_hunt_calm_animals", g_cfg.free_hunt_calm_animals);
    g_cfg.free_hunt_wildlife = read_bool(L"game", L"free_hunt_wildlife", g_cfg.free_hunt_wildlife);
    g_cfg.free_hunt_day_minutes = read_float(L"game", L"free_hunt_day_minutes", g_cfg.free_hunt_day_minutes);
    g_cfg.free_hunt_night_light = read_float(L"game", L"free_hunt_night_light", g_cfg.free_hunt_night_light);
    g_cfg.free_hunt_any_marker = read_bool(L"game", L"free_hunt_any_marker", g_cfg.free_hunt_any_marker);
    g_cfg.pause_in_background =
        read_bool(L"game", L"pause_in_background", g_cfg.pause_in_background);

    logf("config: %dx%d@%d vsync=%d fps_cap=%d", g_cfg.width, g_cfg.height, g_cfg.refresh,
         g_cfg.vsync, g_cfg.fps_cap);
    logf("config: fov_mode=%d fov_scale=%.3f zoom_smoothing_ms=%.0f menu_scroll_smoothing_ms=%.0f "
         "ui_layout=%d hud_layout=%d skip_intro=%d",
         static_cast<int>(g_cfg.fov_mode), g_cfg.fov_scale, g_cfg.zoom_smoothing_ms,
         g_cfg.menu_scroll_smoothing_ms, static_cast<int>(g_cfg.ui_layout),
         static_cast<int>(g_cfg.hud_layout), g_cfg.skip_intro);
    logf("config: raw=%d fps_independent=%d ref_hz=%.1f sens=%.4f/%.4f zoom_mode=%d cm_per_360=%.1f "
         "dpi=%d",
         g_cfg.raw_input, g_cfg.fps_independent, g_cfg.reference_hz, g_cfg.sensitivity_x,
         g_cfg.sensitivity_y, static_cast<int>(g_cfg.zoom_mode), g_cfg.cm_per_360, g_cfg.mouse_dpi);
    logf("config: high_res_timer=%d pause_in_background=%d", g_cfg.high_res_timer,
         g_cfg.pause_in_background);
    logf("config: anti_aliasing=%d smooth_foliage=%d texture_filtering=%d sharpening=%.2f "
         "vibrance=%.2f bloom=%.2f",
         g_cfg.anti_aliasing, g_cfg.smooth_foliage, g_cfg.texture_filtering, g_cfg.sharpening,
         g_cfg.vibrance, g_cfg.bloom);
}

}  // namespace longshot
