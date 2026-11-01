#include "longshot.h"

#include <cstring>

namespace longshot {

namespace {

// WGL_ARB_pixel_format and WGL_ARB_multisample
constexpr int kDrawToWindow = 0x2001;
constexpr int kAcceleration = 0x2003;
constexpr int kFullAcceleration = 0x2027;
constexpr int kSupportOpenGl = 0x2010;
constexpr int kDoubleBuffer = 0x2011;
constexpr int kPixelType = 0x2013;
constexpr int kTypeRgba = 0x202B;
constexpr int kColorBits = 0x2014;
constexpr int kAlphaBits = 0x201B;
constexpr int kDepthBits = 0x2022;
constexpr int kStencilBits = 0x2023;
constexpr int kSampleBuffers = 0x2041;
constexpr int kSamples = 0x2042;

constexpr unsigned kAlphaTest = 0x0BC0;
constexpr unsigned kBlend = 0x0BE2;
constexpr unsigned kSampleAlphaToCoverage = 0x809E;
constexpr unsigned kMaxAnisotropy = 0x84FE;

// The game also makes a tiny hidden window, to look at what the driver supports.
constexpr LONG kSmallestGameWindow = 320;

using SetPixelFormatFn = BOOL(WINAPI*)(HDC, int, const PIXELFORMATDESCRIPTOR*);
using CreateContextFn = HGLRC(WINAPI*)(HDC);
using DeleteContextFn = BOOL(WINAPI*)(HGLRC);
using MakeCurrentFn = BOOL(WINAPI*)(HDC, HGLRC);
using GetCurrentContextFn = HGLRC(WINAPI*)();
using GetCurrentDcFn = HDC(WINAPI*)();
using GetProcFn = PROC(WINAPI*)(LPCSTR);
using ChooseFormatFn = BOOL(WINAPI*)(HDC, const int*, const float*, UINT, int*, UINT*);
using GetFormatAttribFn = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
using CapabilityFn = void(WINAPI*)(unsigned);
using TexParameterfFn = void(WINAPI*)(unsigned, unsigned, float);
using TexParameteriFn = void(WINAPI*)(unsigned, unsigned, int);

SetPixelFormatFn g_real_set_pixel_format = nullptr;
CapabilityFn g_real_enable = nullptr;
CapabilityFn g_real_disable = nullptr;
TexParameterfFn g_real_tex_parameterf = nullptr;
TexParameteriFn g_real_tex_parameteri = nullptr;

bool g_multisampled = false;
bool g_alpha_test = false;
bool g_blend = false;
bool g_coverage = false;
bool g_filtering_logged = false;

template <typename T>
T opengl_function(const char* name) {
    static HMODULE module = LoadLibraryW(L"opengl32.dll");
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

int choose_multisampled(HDC dc, const PIXELFORMATDESCRIPTOR& wanted, int samples, int* got);

// wglChoosePixelFormatARB can only be fetched with an OpenGL context current, and a window's pixel
// format can only be set once. So that takes a throwaway window with a context of its own.
class ThrowawayContext {
   public:
    explicit ThrowawayContext(int size = 16, int samples = 0) {
        WNDCLASSW cls{};
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"LongshotPixelFormat";
        RegisterClassW(&cls);
        window_ = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP, 0, 0, size, size, nullptr,
                                  nullptr, cls.hInstance, nullptr);
        dc_ = window_ ? GetDC(window_) : nullptr;
        if (!dc_) return;

        PIXELFORMATDESCRIPTOR format{};
        format.nSize = sizeof(format);
        format.nVersion = 1;
        format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        format.cColorBits = 32;
        format.cDepthBits = 24;
        int got = 0;
        const int index =
            samples ? choose_multisampled(dc_, format, samples, &got) : ChoosePixelFormat(dc_, &format);
        if (!index || !SetPixelFormat(dc_, index, &format)) return;

        auto create = opengl_function<CreateContextFn>("wglCreateContext");
        make_current_ = opengl_function<MakeCurrentFn>("wglMakeCurrent");
        auto current_context = opengl_function<GetCurrentContextFn>("wglGetCurrentContext");
        auto current_dc = opengl_function<GetCurrentDcFn>("wglGetCurrentDC");
        if (!create || !make_current_ || !current_context || !current_dc) return;

        previous_dc_ = current_dc();
        previous_context_ = current_context();
        context_ = create(dc_);
        current_ = context_ && make_current_(dc_, context_);
    }

    ~ThrowawayContext() {
        if (current_) make_current_(previous_dc_, previous_context_);
        if (context_) {
            if (auto destroy = opengl_function<DeleteContextFn>("wglDeleteContext")) destroy(context_);
        }
        if (dc_) ReleaseDC(window_, dc_);
        if (window_) DestroyWindow(window_);
    }

    bool ready() const { return current_; }

   private:
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC context_ = nullptr;
    HDC previous_dc_ = nullptr;
    HGLRC previous_context_ = nullptr;
    MakeCurrentFn make_current_ = nullptr;
    bool current_ = false;
};

// The closest format to what the game asked for, at the highest sample count up to `samples`.
int choose_multisampled(HDC dc, const PIXELFORMATDESCRIPTOR& wanted, int samples, int* got) {
    ThrowawayContext context;
    if (!context.ready()) return 0;

    auto get_proc = opengl_function<GetProcFn>("wglGetProcAddress");
    auto choose = get_proc ? reinterpret_cast<ChooseFormatFn>(get_proc("wglChoosePixelFormatARB")) : nullptr;
    if (!choose) return 0;

    for (int count = samples; count >= 2; count /= 2) {
        const int attributes[] = {kDrawToWindow,  1,
                                  kSupportOpenGl, 1,
                                  kDoubleBuffer,  1,
                                  kAcceleration,  kFullAcceleration,
                                  kPixelType,     kTypeRgba,
                                  kColorBits,     wanted.cColorBits,
                                  kAlphaBits,     wanted.cAlphaBits,
                                  kDepthBits,     wanted.cDepthBits,
                                  kStencilBits,   wanted.cStencilBits,
                                  kSampleBuffers, 1,
                                  kSamples,       count,
                                  0};
        int format = 0;
        UINT found = 0;
        if (choose(dc, attributes, nullptr, 1, &format, &found) && found > 0 && format > 0) {
            *got = count;
            return format;
        }
    }
    return 0;
}

bool is_game_window(HDC dc) {
    RECT rect{};
    HWND window = WindowFromDC(dc);
    return window && GetClientRect(window, &rect) && rect.right >= kSmallestGameWindow;
}

BOOL WINAPI hook_set_pixel_format(HDC dc, int format, const PIXELFORMATDESCRIPTOR* wanted) {
    if (g_cfg.anti_aliasing >= 2 && wanted && is_game_window(dc)) {
        int samples = 0;
        const int chosen = choose_multisampled(dc, *wanted, g_cfg.anti_aliasing, &samples);
        if (chosen) {
            logf("graphics: %dx anti-aliasing", samples);
            g_multisampled = true;
            return g_real_set_pixel_format(dc, chosen, wanted);
        }
        logf("graphics: anti-aliasing isn't available here");
    }
    return g_real_set_pixel_format(dc, format, wanted);
}

// Multisampling only smooths the edges of shapes, and a leaf's edge is a hole in its texture.
// Coverage from the texture's alpha gives those edges the same treatment, wherever the game cuts
// holes with the alpha test. Blended surfaces already have soft edges.
void update_coverage() {
    const bool wanted = g_multisampled && g_cfg.smooth_foliage && g_alpha_test && !g_blend;
    if (wanted == g_coverage) return;
    g_coverage = wanted;
    (wanted ? g_real_enable : g_real_disable)(kSampleAlphaToCoverage);
}

void WINAPI hook_enable(unsigned capability) {
    g_real_enable(capability);
    if (capability == kAlphaTest) g_alpha_test = true;
    else if (capability == kBlend) g_blend = true;
    else return;
    update_coverage();
}

void WINAPI hook_disable(unsigned capability) {
    g_real_disable(capability);
    if (capability == kAlphaTest) g_alpha_test = false;
    else if (capability == kBlend) g_blend = false;
    else return;
    update_coverage();
}

float filtered(float requested) {
    if (g_cfg.texture_filtering <= 0) return requested;
    if (!g_filtering_logged) {
        g_filtering_logged = true;
        logf("graphics: texture filtering %dx, in place of %.0fx", g_cfg.texture_filtering, requested);
    }
    return static_cast<float>(g_cfg.texture_filtering);
}

void WINAPI hook_tex_parameterf(unsigned target, unsigned name, float value) {
    g_real_tex_parameterf(target, name, name == kMaxAnisotropy ? filtered(value) : value);
}

void WINAPI hook_tex_parameteri(unsigned target, unsigned name, int value) {
    const float changed = name == kMaxAnisotropy ? filtered(static_cast<float>(value)) : 0.0f;
    g_real_tex_parameteri(target, name, name == kMaxAnisotropy ? static_cast<int>(changed) : value);
}

}  // namespace

void graphics_attach_gl(HMODULE gl) {
    const bool ok = iat_patch(gl, "GDI32.dll", "SetPixelFormat",
                              reinterpret_cast<void*>(&hook_set_pixel_format),
                              reinterpret_cast<void**>(&g_real_set_pixel_format));
    logf("graphics: pixel format hook %s", ok ? "installed" : "FAILED");
}

void* graphics_wrap_gl(const char* name, void* proc) {
    if (!strcmp(name, "glEnable")) {
        g_real_enable = reinterpret_cast<CapabilityFn>(proc);
        return reinterpret_cast<void*>(&hook_enable);
    }
    if (!strcmp(name, "glDisable")) {
        g_real_disable = reinterpret_cast<CapabilityFn>(proc);
        return reinterpret_cast<void*>(&hook_disable);
    }
    if (!strcmp(name, "glTexParameterf")) {
        g_real_tex_parameterf = reinterpret_cast<TexParameterfFn>(proc);
        return reinterpret_cast<void*>(&hook_tex_parameterf);
    }
    if (!strcmp(name, "glTexParameteri")) {
        g_real_tex_parameteri = reinterpret_cast<TexParameteriFn>(proc);
        return reinterpret_cast<void*>(&hook_tex_parameteri);
    }
    return proc;
}

void graphics_in_context(int size, int samples, const std::function<void()>& work) {
    ThrowawayContext context(size, samples);
    if (context.ready()) work();
}

int graphics_test_multisample(HDC dc, int samples) {
    PIXELFORMATDESCRIPTOR wanted{};
    wanted.cColorBits = 32;
    wanted.cDepthBits = 24;
    wanted.cStencilBits = 8;
    int got = 0;
    return choose_multisampled(dc, wanted, samples, &got) ? got : 0;
}

}  // namespace longshot
