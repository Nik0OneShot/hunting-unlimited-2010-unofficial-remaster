#include "longshot.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace longshot {

namespace {

struct KnownBuild {
    const char* name;
    uint32_t timestamp;
    uint32_t image_size;
};

// Steam build 252065. Fixes that use fixed offsets run only on these exact binaries.
constexpr KnownBuild kPrism = {"prism3d.exe", 0x4A378EB6, 0x00050000};
constexpr KnownBuild kCore = {"p3core.dll", 0x4A378EAC, 0x00063000};
constexpr KnownBuild kShared = {"p3shared.dll", 0x4A378EB1, 0x0005D000};
constexpr KnownBuild kGl = {"gl.dll", 0x4A378EB8, 0x0003B000};
constexpr KnownBuild kDi8 = {"di8.dll", 0x4A378EB8, 0x00018000};
constexpr KnownBuild kGame = {"game.dll", 0x4A378EC9, 0x0030B000};

using LoadLibraryFn = HMODULE(WINAPI*)(LPCSTR);
using DpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);

std::wstring g_root;
LoadLibraryFn g_real_load_library = nullptr;
bool g_gl_done = false;
bool g_di8_done = false;
bool g_game_done = false;

bool check_build(HMODULE mod, const KnownBuild& known) {
    const bool ok = module_matches(mod, known.timestamp, known.image_size);
    logf("build: %s %s", known.name, ok ? "verified" : "UNKNOWN VERSION");
    return ok;
}

void on_module_loaded(HMODULE mod, const char* path) {
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    logf("core: loaded %s", path);

    if (!_stricmp(name, "gl.dll") && !g_gl_done) {
        g_gl_done = true;
        check_build(mod, kGl);
        display_attach_gl(mod);
    } else if (!_stricmp(name, "di8.dll") && !g_di8_done) {
        g_di8_done = true;
        check_build(mod, kDi8);
        input_attach_di8(mod);
    } else if (!_stricmp(name, "game.dll") && !g_game_done) {
        g_game_done = true;
        const bool verified = check_build(mod, kGame);
        input_attach_game(mod, verified);
        menu_attach_game(mod, verified);
        view_attach_game(mod, verified);
    }
}

// The engine loads its plugins from lib\ through p3core, so this sees each one arrive.
HMODULE WINAPI hook_load_library(LPCSTR path) {
    HMODULE mod = g_real_load_library(path);
    if (mod && path) on_module_loaded(mod, path);
    return mod;
}

void enable_dpi_awareness() {
    // Without this, Windows scales the desktop size the game sees on high DPI displays.
    auto set_context = reinterpret_cast<DpiContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_context) set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

}  // namespace

const std::wstring& root_dir() { return g_root; }

std::wstring logs_dir() { return g_root + L"\\logs"; }

void set_root(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    g_root = path;
}

}  // namespace longshot

using namespace longshot;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        set_root(instance);
    }
    return TRUE;
}

// Called by the launcher's stub on the main thread, after the game's static imports have
// loaded and before any of its code has run.
extern "C" __declspec(dllexport) void __cdecl longshot_init() {
    log_open();
    logf("longshot starting");
    crash_install();
    config_load();
    enable_dpi_awareness();
    display_resolve_mode();

    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE p3core = GetModuleHandleW(L"p3core.dll");
    HMODULE p3shared = GetModuleHandleW(L"p3shared.dll");
    if (!p3core || !p3shared) {
        logf("not running inside the game, hooks skipped");
        return;
    }

    check_build(exe, kPrism);
    check_build(p3core, kCore);
    check_build(p3shared, kShared);

    timing_install(p3core);
    display_install(exe);
    layout_install(p3core, p3shared);

    const bool ok = iat_patch(p3core, "KERNEL32.dll", "LoadLibraryA",
                              reinterpret_cast<void*>(&hook_load_library),
                              reinterpret_cast<void**>(&g_real_load_library));
    logf("core: plugin loader hook %s", ok ? "installed" : "FAILED");
}

// ---- Tests, run by tests/selftest.cpp in a process that is not the game --------------------

namespace {

DWORD WINAPI fake_tick_count() { return 0x12345678; }

}  // namespace

extern "C" __declspec(dllexport) int __cdecl longshot_test_iat() {
    return iat_patch(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetTickCount",
                     reinterpret_cast<void*>(&fake_tick_count), nullptr)
               ? 0
               : 1;
}

extern "C" __declspec(dllexport) float __cdecl longshot_test_fov(float fov, float aspect) {
    return view_adjust_fov(fov, aspect);
}

extern "C" __declspec(dllexport) float __cdecl longshot_test_ui_x(float x, float left, float right,
                                                                  int width) {
    return view_ui_to_pixel(x, left, right, width);
}

extern "C" __declspec(dllexport) float __cdecl longshot_test_zoom(float current, float target,
                                                                  float seconds, float constant) {
    return view_zoom_step(current, target, seconds, constant);
}

extern "C" __declspec(dllexport) int __cdecl longshot_test_wheel() { return input_test_wheel(); }

// Writes the rewritten text to `out` and returns its size, or 0 to leave the file as it is.
extern "C" __declspec(dllexport) int __cdecl longshot_test_layout(const char* path, const char* text,
                                                                  float aspect, int hud, char* out,
                                                                  int capacity) {
    const std::string changed = layout_rewrite(path, text, aspect, static_cast<HudLayout>(hud));
    if (changed.empty() || static_cast<int>(changed.size()) >= capacity) return 0;
    memcpy(out, changed.c_str(), changed.size() + 1);
    return static_cast<int>(changed.size());
}

extern "C" __declspec(dllexport) float __cdecl longshot_test_glide(float shown, float seconds,
                                                                   float constant) {
    return menu_glide_step(shown, seconds, constant);
}

extern "C" __declspec(dllexport) float __cdecl longshot_test_glide_add(float shown, float delta,
                                                                       float limit) {
    return menu_glide_add(shown, delta, limit);
}

extern "C" __declspec(dllexport) int __cdecl longshot_test_rawmouse() {
    g_cfg.raw_input = true;
    return input_rawmouse_start() ? 0 : 1;
}

extern "C" __declspec(dllexport) void __cdecl longshot_test_rawmouse_read(long* dx, long* dy) {
    input_rawmouse_read(dx, dy);
}

extern "C" __declspec(dllexport) long __cdecl longshot_test_rawmouse_events() {
    return input_rawmouse_events();
}
