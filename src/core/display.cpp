#include "longshot.h"

#include <algorithm>
#include <vector>

namespace longshot {

DisplayMode g_mode;

namespace {

using SwapBuffersFn = BOOL(WINAPI*)(HDC);
using ChangeDisplaySettingsFn = LONG(WINAPI*)(DEVMODEA*, DWORD);
using CreateWindowExFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND,
                                       HMENU, HINSTANCE, LPVOID);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using WglGetProcAddressFn = PROC(WINAPI*)(LPCSTR);
using SwapIntervalFn = BOOL(WINAPI*)(int);
using GlGetStringFn = const unsigned char*(WINAPI*)(unsigned int);

// Just enough OpenGL to clear part of the frame and put the engine's state back afterwards.
struct GlClear {
    using IsEnabledFn = unsigned char(WINAPI*)(unsigned int);
    using EnableFn = void(WINAPI*)(unsigned int);
    using GetIntegerFn = void(WINAPI*)(unsigned int, int*);
    using GetFloatFn = void(WINAPI*)(unsigned int, float*);
    using GetBooleanFn = void(WINAPI*)(unsigned int, unsigned char*);
    using ScissorFn = void(WINAPI*)(int, int, int, int);
    using ClearColorFn = void(WINAPI*)(float, float, float, float);
    using ColorMaskFn = void(WINAPI*)(unsigned char, unsigned char, unsigned char, unsigned char);
    using ClearFn = void(WINAPI*)(unsigned int);

    IsEnabledFn is_enabled = nullptr;
    EnableFn enable = nullptr;
    EnableFn disable = nullptr;
    GetIntegerFn get_integer = nullptr;
    GetFloatFn get_float = nullptr;
    GetBooleanFn get_boolean = nullptr;
    ScissorFn scissor = nullptr;
    ClearColorFn clear_color = nullptr;
    ColorMaskFn color_mask = nullptr;
    ClearFn clear = nullptr;

    bool ready() const {
        return is_enabled && enable && disable && get_integer && get_float && get_boolean &&
               scissor && clear_color && color_mask && clear;
    }
};

SwapBuffersFn g_real_swap = nullptr;
SwapBuffersFn g_real_wgl_swap = nullptr;
ChangeDisplaySettingsFn g_real_cds = nullptr;
CreateWindowExFn g_real_create_window = nullptr;
GetProcAddressFn g_real_get_proc = nullptr;
WglGetProcAddressFn g_real_wgl_get_proc = nullptr;
SwapIntervalFn g_real_swap_interval = nullptr;
WNDPROC g_real_window_proc = nullptr;

// How long each frame waits while the game is in the background, to free up the GPU.
constexpr DWORD kBackgroundSleepMs = 25;

HWND g_main_window = nullptr;
bool g_topmost_dropped = false;

// Times frames and reports percentiles, the numbers that show stutter an average hides.
class FrameStats {
   public:
    void on_frame() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (last_ != 0) {
            const float frame_ms = static_cast<float>(now.QuadPart - last_) * ms_per_tick();
            frames_ms_.push_back(frame_ms);
            profile_frame_done(frame_ms, typical_ms_, game_has_focus());
        }
        last_ = now.QuadPart;

        if (window_start_ == 0) window_start_ = now.QuadPart;
        const float elapsed = static_cast<float>(now.QuadPart - window_start_) * ms_per_tick();
        if (elapsed >= kWindowMs) {
            report(elapsed);
            window_start_ = now.QuadPart;
        }
    }

   private:
    static constexpr float kWindowMs = 10000.0f;

    float ms_per_tick() {
        if (ms_per_tick_ == 0.0f) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            ms_per_tick_ = 1000.0f / static_cast<float>(freq.QuadPart);
        }
        return ms_per_tick_;
    }

    void report(float elapsed_ms) {
        if (frames_ms_.empty()) return;
        std::sort(frames_ms_.begin(), frames_ms_.end());
        const size_t n = frames_ms_.size();
        const float median = frames_ms_[n / 2];
        size_t spikes = 0;
        for (const float ms : frames_ms_) spikes += ms > median * 2.0f ? 1 : 0;
        logf("frames: %.1f fps | ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | spikes(>2x median)=%u/%u",
             n * 1000.0f / elapsed_ms, median, frames_ms_[n * 95 / 100], frames_ms_[n * 99 / 100],
             frames_ms_[n - 1], static_cast<unsigned>(spikes), static_cast<unsigned>(n));
        profile_report_window();
        typical_ms_ = median;
        frames_ms_.clear();
    }

    std::vector<float> frames_ms_;
    LONGLONG last_ = 0;
    LONGLONG window_start_ = 0;
    float ms_per_tick_ = 0.0f;
    float typical_ms_ = 0.0f;
};

class FrameLimiter {
   public:
    void wait() {
        if (g_cfg.fps_cap <= 0) return;
        if (freq_ == 0) {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            freq_ = value.QuadPart;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
        }
        const LONGLONG interval = freq_ / g_cfg.fps_cap;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONGLONG target = next_ + interval;
        LONGLONG remaining = target - now.QuadPart;

        // Sleep through most of the wait, then spin for accuracy.
        const LONGLONG spin_ticks = freq_ / 2000;
        if (timer_ && remaining > spin_ticks) {
            LARGE_INTEGER due;
            due.QuadPart = -(remaining - spin_ticks) * 10000000 / freq_;
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_, INFINITE);
            }
        }
        do {
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < target);

        // After a slow frame, restart the cadence instead of rushing to catch up.
        next_ = now.QuadPart - target > interval ? now.QuadPart : target;
    }

   private:
    LONGLONG freq_ = 0;
    LONGLONG next_ = 0;
    HANDLE timer_ = nullptr;
};

FrameStats g_stats;
FrameLimiter g_limiter;
bool g_presented = false;
bool g_clipped = false;
bool g_bars_cleared = false;

FARPROC opengl_proc(const char* name) {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    return gl ? GetProcAddress(gl, name) : nullptr;
}

void apply_swap_interval() {
    if (!g_real_swap_interval) {
        auto wgl_get = reinterpret_cast<WglGetProcAddressFn>(opengl_proc("wglGetProcAddress"));
        if (wgl_get) {
            g_real_swap_interval = reinterpret_cast<SwapIntervalFn>(wgl_get("wglSwapIntervalEXT"));
        }
    }
    const BOOL ok = g_real_swap_interval && g_real_swap_interval(g_cfg.vsync ? 1 : 0);
    logf("display: swap interval %d %s", g_cfg.vsync ? 1 : 0, ok ? "set" : "FAILED");
}

void log_gl_info() {
    constexpr unsigned int kVendor = 0x1F00, kRenderer = 0x1F01, kVersion = 0x1F02;
    auto get_string = reinterpret_cast<GlGetStringFn>(opengl_proc("glGetString"));
    if (!get_string) return;
    for (const unsigned int name : {kVendor, kRenderer, kVersion}) {
        const unsigned char* value = get_string(name);
        logf("opengl: %s", value ? reinterpret_cast<const char*>(value) : "(null)");
    }
}

GlClear load_gl_clear() {
    GlClear gl;
    gl.is_enabled = reinterpret_cast<GlClear::IsEnabledFn>(opengl_proc("glIsEnabled"));
    gl.enable = reinterpret_cast<GlClear::EnableFn>(opengl_proc("glEnable"));
    gl.disable = reinterpret_cast<GlClear::EnableFn>(opengl_proc("glDisable"));
    gl.get_integer = reinterpret_cast<GlClear::GetIntegerFn>(opengl_proc("glGetIntegerv"));
    gl.get_float = reinterpret_cast<GlClear::GetFloatFn>(opengl_proc("glGetFloatv"));
    gl.get_boolean = reinterpret_cast<GlClear::GetBooleanFn>(opengl_proc("glGetBooleanv"));
    gl.scissor = reinterpret_cast<GlClear::ScissorFn>(opengl_proc("glScissor"));
    gl.clear_color = reinterpret_cast<GlClear::ClearColorFn>(opengl_proc("glClearColor"));
    gl.color_mask = reinterpret_cast<GlClear::ColorMaskFn>(opengl_proc("glColorMask"));
    gl.clear = reinterpret_cast<GlClear::ClearFn>(opengl_proc("glClear"));
    logf("display: side bar clearing %s", gl.ready() ? "ready" : "UNAVAILABLE");
    return gl;
}

// The engine never clears the frame, so screens that don't reach the bars would show stale frames
// there, and the scratch area where it renders environment maps.
void clear_side_bars(int bar_width) {
    static const GlClear gl = load_gl_clear();
    if (bar_width <= 0 || !gl.ready()) return;

    constexpr unsigned int kScissorTest = 0x0C11, kScissorBox = 0x0C10;
    constexpr unsigned int kClearValue = 0x0C22, kWriteMask = 0x0C23, kColorBit = 0x4000;

    const unsigned char scissor_on = gl.is_enabled(kScissorTest);
    int box[4];
    float color[4];
    unsigned char mask[4];
    gl.get_integer(kScissorBox, box);
    gl.get_float(kClearValue, color);
    gl.get_boolean(kWriteMask, mask);

    gl.enable(kScissorTest);
    gl.color_mask(1, 1, 1, 1);
    gl.clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    gl.scissor(0, 0, bar_width, g_mode.height);
    gl.clear(kColorBit);
    gl.scissor(g_mode.width - bar_width, 0, bar_width, g_mode.height);
    gl.clear(kColorBit);

    gl.clear_color(color[0], color[1], color[2], color[3]);
    gl.color_mask(mask[0], mask[1], mask[2], mask[3]);
    gl.scissor(box[0], box[1], box[2], box[3]);
    if (!scissor_on) gl.disable(kScissorTest);
}

// Windows can refuse a new process the foreground, for example when it was started in the
// background. The engine tries once while the window is being set up, this tries once it is drawing.
void claim_focus() {
    HWND foreground = GetForegroundWindow();
    if (!g_main_window || foreground == g_main_window) return;

    const DWORD self = GetCurrentThreadId();
    const DWORD other = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = other && other != self && AttachThreadInput(self, other, TRUE);
    BringWindowToTop(g_main_window);
    const BOOL ok = SetForegroundWindow(g_main_window);
    if (attached) AttachThreadInput(self, other, FALSE);
    logf("display: game started without focus, requesting it %s", ok ? "worked" : "FAILED");
}

// The window starts topmost so it covers the screen even without focus. Once it has focus,
// topmost is dropped, so alt-tab can show other windows and system dialogs appear over it.
void drop_topmost_once_focused() {
    if (!g_main_window || g_topmost_dropped || GetForegroundWindow() != g_main_window) return;
    g_topmost_dropped = true;
    SetWindowPos(g_main_window, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    logf("display: window has focus");
}

void update_cursor_clip() {
    if (!g_cfg.raw_input) return;
    const bool focused = game_has_focus();
    if (focused == g_clipped) return;
    g_clipped = focused;
    RECT rect;
    if (focused && GetWindowRect(GetForegroundWindow(), &rect)) {
        ClipCursor(&rect);
    } else {
        ClipCursor(nullptr);
    }
}

BOOL present(SwapBuffersFn real, HDC dc) {
    if (!g_presented) {
        g_presented = true;
        log_gl_info();
        apply_swap_interval();
        claim_focus();
    }
    drop_topmost_once_focused();
    view_per_frame();
    if (!g_bars_cleared) clear_side_bars(view_take_bar_width());
    g_bars_cleared = false;
    update_cursor_clip();
    view_update_background_pause();
    if (g_main_window && !game_has_focus()) Sleep(kBackgroundSleepMs);
    BOOL ok;
    {
        ProfileTimer timer(ProfileSection::Swap);
        g_limiter.wait();
        ok = real(dc);
    }
    g_stats.on_frame();
    return ok;
}

BOOL WINAPI hook_swap_buffers(HDC dc) { return present(g_real_swap, dc); }
BOOL WINAPI hook_wgl_swap_buffers(HDC dc) { return present(g_real_wgl_swap, dc); }

BOOL WINAPI hook_swap_interval(int) {
    // Vsync belongs to longshot.ini, whatever the engine asks for.
    return g_real_swap_interval(g_cfg.vsync ? 1 : 0);
}

template <typename Proc>
void* wrap_gl(const char* name, Proc proc) {
    return profile_wrap_gl(name, graphics_wrap_gl(name, reinterpret_cast<void*>(proc)));
}

PROC WINAPI hook_wgl_get_proc(LPCSTR name) {
    PROC proc = g_real_wgl_get_proc(name);
    if (proc && !strcmp(name, "wglSwapIntervalEXT")) {
        g_real_swap_interval = reinterpret_cast<SwapIntervalFn>(proc);
        return reinterpret_cast<PROC>(&hook_swap_interval);
    }
    return proc ? reinterpret_cast<PROC>(wrap_gl(name, proc)) : proc;
}

FARPROC WINAPI hook_gl_get_proc(HMODULE mod, LPCSTR name) {
    FARPROC proc = g_real_get_proc(mod, name);
    if (!proc || IS_INTRESOURCE(name)) return proc;
    if (!strcmp(name, "wglGetProcAddress")) {
        g_real_wgl_get_proc = reinterpret_cast<WglGetProcAddressFn>(proc);
        return reinterpret_cast<FARPROC>(&hook_wgl_get_proc);
    }
    if (!strcmp(name, "wglSwapBuffers")) {
        g_real_wgl_swap = reinterpret_cast<SwapBuffersFn>(proc);
        return reinterpret_cast<FARPROC>(&hook_wgl_swap_buffers);
    }
    return reinterpret_cast<FARPROC>(wrap_gl(name, proc));
}

// The engine's fullscreen path already makes a borderless popup window, so skipping the
// mode switch is all borderless fullscreen needs.
LONG WINAPI hook_change_display_settings(DEVMODEA* mode, DWORD flags) {
    if (mode) {
        logf("display: mode request %lux%lu %lubpp %luHz flags=%lu%s", mode->dmPelsWidth,
             mode->dmPelsHeight, mode->dmBitsPerPel, mode->dmDisplayFrequency, flags,
             g_mode.matches_desktop ? " (kept desktop mode)" : "");
    }
    return g_mode.matches_desktop ? DISP_CHANGE_SUCCESSFUL : g_real_cds(mode, flags);
}

// When the app loses focus, the game restarts the whole video system: it destroys the window and
// renderer, and builds them again on return. Exclusive fullscreen needed that. Borderless doesn't,
// and any dialog taking focus would make the game vanish. Input has its own message, WM_ACTIVATE,
// so it still stops and starts as normal.
LRESULT CALLBACK hook_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ACTIVATEAPP && !wparam) {
        logf("display: lost focus, game stays up");
        return 0;
    }
    return CallWindowProcA(g_real_window_proc, window, message, wparam, lparam);
}

HWND WINAPI hook_create_window(DWORD ex_style, LPCSTR cls, LPCSTR title, DWORD style, int x, int y,
                               int width, int height, HWND parent, HMENU menu, HINSTANCE instance,
                               LPVOID param) {
    HWND window = g_real_create_window(ex_style, cls, title, style, x, y, width, height, parent,
                                       menu, instance, param);
    logf("display: window %p style=%08lX ex=%08lX at %d,%d size %dx%d", window, style, ex_style, x,
         y, width, height);

    if (window && (style & WS_POPUP) && g_mode.matches_desktop) {
        g_main_window = window;
        g_topmost_dropped = false;
        g_real_window_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hook_window_proc)));
    }
    return window;
}

}  // namespace

void display_settings_changed() { apply_swap_interval(); }

void display_before_ui() {
    clear_side_bars(view_take_bar_width());
    g_bars_cleared = true;
}

bool game_has_focus() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    return process == GetCurrentProcessId();
}

void display_resolve_mode() {
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop);

    const int desktop_w = static_cast<int>(desktop.dmPelsWidth);
    const int desktop_h = static_cast<int>(desktop.dmPelsHeight);
    const int desktop_hz = static_cast<int>(desktop.dmDisplayFrequency);

    g_mode.width = g_cfg.width > 0 ? g_cfg.width : desktop_w;
    g_mode.height = g_cfg.height > 0 ? g_cfg.height : desktop_h;
    g_mode.refresh = g_cfg.refresh > 0 ? g_cfg.refresh : desktop_hz;
    g_mode.aspect = static_cast<float>(g_mode.width) / static_cast<float>(g_mode.height);
    g_mode.matches_desktop = g_mode.width == desktop_w && g_mode.height == desktop_h;

    logf("display: desktop %dx%d@%d, target %dx%d@%d, aspect %.4f, %s", desktop_w, desktop_h,
         desktop_hz, g_mode.width, g_mode.height, g_mode.refresh, g_mode.aspect,
         g_mode.matches_desktop ? "borderless" : "mode switch");
}

void display_install(HMODULE exe) {
    const bool ok = iat_patch(exe, "USER32.dll", "CreateWindowExA",
                              reinterpret_cast<void*>(&hook_create_window),
                              reinterpret_cast<void**>(&g_real_create_window));
    logf("display: window hook %s", ok ? "installed" : "FAILED");
}

void display_attach_gl(HMODULE gl) {
    const bool cds = iat_patch(gl, "USER32.dll", "ChangeDisplaySettingsA",
                               reinterpret_cast<void*>(&hook_change_display_settings),
                               reinterpret_cast<void**>(&g_real_cds));
    const bool swap =
        iat_patch(gl, "GDI32.dll", "SwapBuffers", reinterpret_cast<void*>(&hook_swap_buffers),
                  reinterpret_cast<void**>(&g_real_swap));
    const bool procs = iat_patch(gl, "KERNEL32.dll", "GetProcAddress",
                                 reinterpret_cast<void*>(&hook_gl_get_proc),
                                 reinterpret_cast<void**>(&g_real_get_proc));
    logf("display: gl.dll hooks mode=%d swap=%d procs=%d", cds, swap, procs);
    graphics_attach_gl(gl);
}

}  // namespace longshot
