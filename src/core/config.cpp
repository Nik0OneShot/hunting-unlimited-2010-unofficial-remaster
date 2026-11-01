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
    read_str(L"view", L"hud_layout", text, sizeof(text));
    if (!_stricmp(text, "16_9")) g_cfg.hud_layout = HudLayout::Wide169;
    else if (!_stricmp(text, "4_3")) g_cfg.hud_layout = HudLayout::Center43;

    g_cfg.raw_input = read_bool(L"mouse", L"raw_input", g_cfg.raw_input);
    g_cfg.fps_independent = read_bool(L"mouse", L"fps_independent", g_cfg.fps_independent);
    g_cfg.reference_hz =
        positive(read_float(L"mouse", L"reference_hz", g_cfg.reference_hz), g_cfg.reference_hz);
    g_cfg.sensitivity_x = read_float(L"mouse", L"sensitivity_x", g_cfg.sensitivity_x);
    g_cfg.sensitivity_y = read_float(L"mouse", L"sensitivity_y", g_cfg.sensitivity_y);
    read_str(L"mouse", L"zoom_mode", text, sizeof(text));
    if (!_stricmp(text, "stock")) g_cfg.zoom_mode = ZoomMode::Stock;
    else if (!_stricmp(text, "off")) g_cfg.zoom_mode = ZoomMode::Off;

    g_cfg.high_res_timer = read_bool(L"timing", L"high_res_timer", g_cfg.high_res_timer);
    g_cfg.skip_intro = read_bool(L"game", L"skip_intro", g_cfg.skip_intro);
    g_cfg.pause_in_background =
        read_bool(L"game", L"pause_in_background", g_cfg.pause_in_background);

    logf("config: %dx%d@%d vsync=%d fps_cap=%d", g_cfg.width, g_cfg.height, g_cfg.refresh,
         g_cfg.vsync, g_cfg.fps_cap);
    logf("config: fov_mode=%d fov_scale=%.3f zoom_smoothing_ms=%.0f menu_scroll_smoothing_ms=%.0f "
         "ui_layout=%d hud_layout=%d skip_intro=%d",
         static_cast<int>(g_cfg.fov_mode), g_cfg.fov_scale, g_cfg.zoom_smoothing_ms,
         g_cfg.menu_scroll_smoothing_ms, static_cast<int>(g_cfg.ui_layout),
         static_cast<int>(g_cfg.hud_layout), g_cfg.skip_intro);
    logf("config: raw=%d fps_independent=%d ref_hz=%.1f sens=%.4f/%.4f zoom_mode=%d",
         g_cfg.raw_input, g_cfg.fps_independent, g_cfg.reference_hz, g_cfg.sensitivity_x,
         g_cfg.sensitivity_y, static_cast<int>(g_cfg.zoom_mode));
    logf("config: high_res_timer=%d pause_in_background=%d", g_cfg.high_res_timer,
         g_cfg.pause_in_background);
}

}  // namespace longshot
