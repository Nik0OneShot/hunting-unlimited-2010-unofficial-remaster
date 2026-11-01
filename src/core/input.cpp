#include "longshot.h"

#include <initguid.h>

#include <dinput.h>
#include <intrin.h>

#include <cmath>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace longshot {

namespace {

// Offsets in game.dll, valid only for the verified build.
constexpr uint32_t kSlotImpulseValue = 0x2d08e8;  // bound pointer to inp_impulse_value
constexpr uint32_t kSlotLastDelta = 0x2d05f4;     // bound pointer to sys_get_lastdelta
constexpr uint32_t kLookBegin = 0x13070;          // player look function
constexpr uint32_t kLookEnd = 0x13580;
constexpr uint32_t kGameState = 0x1e9d68;  // pointer to pointer to the game state
constexpr size_t kAimScale = 0x70;         // float: 1.0, or lower while looking down a sight

// Impulse ids from game.dll's impulse name table.
constexpr int kImpulseMouseLeft = 0x6e;
constexpr int kImpulseMouseRight = 0x6f;
constexpr int kImpulseMouseDown = 0x71;

// Engine input event types, as posted by di8.dll.
constexpr int kEventButtonUp = 2;
constexpr int kEventButtonDown = 3;

constexpr DWORD kOfsX = 0;
constexpr DWORD kOfsY = 4;
constexpr DWORD kOfsZ = 8;
constexpr DWORD kOfsButton0 = 12;
// di8.dll drops every mouse button after the third, though the engine defines mouse4+.
constexpr int kDi8ButtonCount = 3;

constexpr float kDegToRad = 3.14159265f / 180.0f;
// Radians per second for a full look input, from the look function.
constexpr float kLookRate = 0.872665f;

struct InpEvent {
    int type;
    int modifiers;
    int code;
    int unused[4];
};

using ImpulseValueFn = float(__cdecl*)(int);
using LastDeltaFn = double(__cdecl*)();
using InpEventFn = void(__cdecl*)(const InpEvent*);
using MouseSensitivityFn = float(__cdecl*)();
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

char* g_game = nullptr;
bool g_verified = false;
ImpulseValueFn g_real_impulse_value = nullptr;
LastDeltaFn g_last_delta = nullptr;
InpEventFn g_inp_event = nullptr;
MouseSensitivityFn g_mouse_sensitivity = nullptr;
DirectInput8CreateFn g_real_create = nullptr;

float g_fov = 0.0f;
float g_hip_fov = 0.0f;
float g_reported_sensitivity = -1.0f;

// ---- Raw Input ---------------------------------------------------------------------------
// A dedicated thread drains the mouse at its full polling rate. Accumulating here means a
// slow frame can never lose movement, unlike di8.dll's 128-event DirectInput buffer.

struct ButtonEdge {
    unsigned char index;
    bool down;
};

class RawMouse {
   public:
    bool start() {
        if (thread_) return true;
        InitializeSRWLock(&lock_);
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, &RawMouse::thread_main, this, 0, nullptr);
        if (!thread_) return false;
        WaitForSingleObject(ready_, 5000);
        return started_;
    }

    long event_count() const { return events_; }

    void take_motion(long* dx, long* dy, long* wheel) {
        *dx = InterlockedExchange(&dx_, 0);
        *dy = InterlockedExchange(&dy_, 0);
        *wheel = InterlockedExchange(&wheel_, 0);
    }

    bool take_button(ButtonEdge* edge) {
        AcquireSRWLockExclusive(&lock_);
        const bool any = head_ != tail_;
        if (any) {
            *edge = ring_[tail_];
            tail_ = (tail_ + 1) % kRingSize;
        }
        ReleaseSRWLockExclusive(&lock_);
        return any;
    }

   private:
    static constexpr unsigned kRingSize = 64;

    static DWORD WINAPI thread_main(void* self) {
        static_cast<RawMouse*>(self)->run();
        return 0;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_INPUT) {
            auto* self = reinterpret_cast<RawMouse*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (self) self->on_input(reinterpret_cast<HRAWINPUT>(lparam));
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        WNDCLASSW cls{};
        cls.lpfnWndProc = &RawMouse::window_proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"LongshotRawMouse";
        RegisterClassW(&cls);

        HWND window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                      nullptr, cls.hInstance, nullptr);
        if (window) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;  // generic desktop
        device.usUsage = 0x02;      // mouse
        device.dwFlags = RIDEV_INPUTSINK;
        device.hwndTarget = window;
        started_ = window && RegisterRawInputDevices(&device, 1, sizeof(device));
        logf("input: raw mouse thread %s", started_ ? "running" : "FAILED");
        SetEvent(ready_);
        if (!started_) return;

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    }

    void push_button(unsigned char index, bool down) {
        AcquireSRWLockExclusive(&lock_);
        const unsigned next = (head_ + 1) % kRingSize;
        if (next != tail_) {
            ring_[head_] = {index, down};
            head_ = next;
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    void on_input(HRAWINPUT handle) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1)) {
            return;
        }
        if (raw.header.dwType != RIM_TYPEMOUSE) return;
        InterlockedIncrement(&events_);

        const RAWMOUSE& mouse = raw.data.mouse;
        if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (mouse.lLastX) InterlockedExchangeAdd(&dx_, mouse.lLastX);
            if (mouse.lLastY) InterlockedExchangeAdd(&dy_, mouse.lLastY);
        }

        const USHORT flags = mouse.usButtonFlags;
        if (flags & RI_MOUSE_WHEEL) {
            InterlockedExchangeAdd(&wheel_, static_cast<short>(mouse.usButtonData));
        }
        static const struct {
            USHORT down, up;
        } kButtons[] = {
            {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP},
            {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP},
            {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP},
            {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP},
            {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP},
        };
        for (unsigned char i = 0; i < 5; ++i) {
            if (flags & kButtons[i].down) push_button(i, true);
            if (flags & kButtons[i].up) push_button(i, false);
        }
    }

    HANDLE thread_ = nullptr;
    HANDLE ready_ = nullptr;
    bool started_ = false;
    volatile LONG dx_ = 0;
    volatile LONG dy_ = 0;
    volatile LONG wheel_ = 0;
    volatile LONG events_ = 0;
    SRWLOCK lock_{};
    ButtonEdge ring_[kRingSize] = {};
    unsigned head_ = 0;
    unsigned tail_ = 0;
};

RawMouse g_raw;

// After each zoom step the game ignores input for 0.07 seconds, so wheel notches during that time
// vanish. Through a sight, this holds them back and hands them over one at a time as it clears.
class WheelPacer {
   public:
    long take(long incoming, bool paced, double now) {
        const bool reversed = incoming != 0 && (incoming > 0) != (pending_ > 0);
        if (reversed) pending_ = 0;
        pending_ = clamp(pending_ + incoming, kMaxPending);

        if (!paced) {
            const long everything = pending_;
            pending_ = 0;
            return everything;
        }
        if (pending_ == 0 || now < next_) return 0;

        const long step = clamp(pending_, kNotch);
        pending_ -= step;
        next_ = now + kNotchInterval;
        return step;
    }

   private:
    static constexpr long kNotch = 120;  // WHEEL_DELTA
    static constexpr long kMaxPending = 12 * kNotch;
    static constexpr double kNotchInterval = 0.078;  // the game's lockout, plus about a frame

    static long clamp(long value, long limit) {
        return value > limit ? limit : (value < -limit ? -limit : value);
    }

    long pending_ = 0;
    double next_ = 0.0;
};

WheelPacer g_wheel;

// ---- DirectInput mouse stand-in -----------------------------------------------------------
// di8.dll keeps talking to what it thinks is an exclusive DirectInput mouse, so everything
// above it in the engine, including key bindings and menus, works unchanged.

class MouseDevice final : public IDirectInputDevice8A {
   public:
    explicit MouseDevice(IDirectInputDevice8A* real) : real_(real) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirectInputDevice8A)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return real_->QueryInterface(iid, out);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            real_->Release();
            delete this;
        }
        return refs;
    }

    // Never held exclusively, so alt-tab and other apps keep working.
    STDMETHODIMP SetCooperativeLevel(HWND window, DWORD) override {
        return real_->SetCooperativeLevel(window, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
    }
    STDMETHODIMP Acquire() override { return DI_OK; }
    STDMETHODIMP Unacquire() override { return DI_OK; }

    STDMETHODIMP GetDeviceData(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                               DWORD) override {
        const DWORD capacity = *count;
        DWORD used = 0;

        long dx, dy, wheel;
        g_raw.take_motion(&dx, &dy, &wheel);
        const bool focused = game_has_focus();

        auto push = [&](DWORD offset, DWORD value) {
            if (used >= capacity) return;
            auto* item = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data) +
                                                               used * size);
            memset(item, 0, size);
            item->dwOfs = offset;
            item->dwData = value;
            item->dwTimeStamp = GetTickCount();
            ++used;
        };

        if (focused) {
            if (dx) push(kOfsX, static_cast<DWORD>(dx));
            if (dy) push(kOfsY, static_cast<DWORD>(dy));
        }
        const long notch = g_wheel.take(focused ? wheel : 0, input_is_zoomed(), now_seconds());
        if (notch) push(kOfsZ, static_cast<DWORD>(notch));

        ButtonEdge edge;
        while (used < capacity && g_raw.take_button(&edge)) {
            if (!focused) continue;
            if (edge.index < kDi8ButtonCount) {
                push(kOfsButton0 + edge.index, edge.down ? 0x80 : 0);
            } else if (g_inp_event) {
                InpEvent event{};
                event.type = edge.down ? kEventButtonDown : kEventButtonUp;
                event.code = edge.index;
                g_inp_event(&event);
            }
        }

        *count = used;
        return DI_OK;
    }

    // Everything else passes straight to the real device.
    STDMETHODIMP GetCapabilities(LPDIDEVCAPS a) override { return real_->GetCapabilities(a); }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA a, LPVOID b, DWORD c) override {
        return real_->EnumObjects(a, b, c);
    }
    STDMETHODIMP GetProperty(REFGUID a, LPDIPROPHEADER b) override {
        return real_->GetProperty(a, b);
    }
    STDMETHODIMP SetProperty(REFGUID a, LPCDIPROPHEADER b) override {
        return real_->SetProperty(a, b);
    }
    STDMETHODIMP GetDeviceState(DWORD a, LPVOID b) override { return real_->GetDeviceState(a, b); }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT a) override { return real_->SetDataFormat(a); }
    STDMETHODIMP SetEventNotification(HANDLE a) override { return real_->SetEventNotification(a); }
    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA a, DWORD b, DWORD c) override {
        return real_->GetObjectInfo(a, b, c);
    }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA a) override { return real_->GetDeviceInfo(a); }
    STDMETHODIMP RunControlPanel(HWND a, DWORD b) override { return real_->RunControlPanel(a, b); }
    STDMETHODIMP Initialize(HINSTANCE a, DWORD b, REFGUID c) override {
        return real_->Initialize(a, b, c);
    }
    STDMETHODIMP CreateEffect(REFGUID a, LPCDIEFFECT b, LPDIRECTINPUTEFFECT* c,
                              LPUNKNOWN d) override {
        return real_->CreateEffect(a, b, c, d);
    }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA a, LPVOID b, DWORD c) override {
        return real_->EnumEffects(a, b, c);
    }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA a, REFGUID b) override {
        return real_->GetEffectInfo(a, b);
    }
    STDMETHODIMP GetForceFeedbackState(LPDWORD a) override {
        return real_->GetForceFeedbackState(a);
    }
    STDMETHODIMP SendForceFeedbackCommand(DWORD a) override {
        return real_->SendForceFeedbackCommand(a);
    }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b,
                                          DWORD c) override {
        return real_->EnumCreatedEffectObjects(a, b, c);
    }
    STDMETHODIMP Escape(LPDIEFFESCAPE a) override { return real_->Escape(a); }
    STDMETHODIMP Poll() override { return DI_OK; }
    STDMETHODIMP SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override {
        return real_->SendDeviceData(a, b, c, d);
    }
    STDMETHODIMP EnumEffectsInFile(LPCSTR a, LPDIENUMEFFECTSINFILECALLBACK b, LPVOID c,
                                   DWORD d) override {
        return real_->EnumEffectsInFile(a, b, c, d);
    }
    STDMETHODIMP WriteEffectToFile(LPCSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) override {
        return real_->WriteEffectToFile(a, b, c, d);
    }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override {
        return real_->BuildActionMap(a, b, c);
    }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override {
        return real_->SetActionMap(a, b, c);
    }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA a) override {
        return real_->GetImageInfo(a);
    }

   private:
    IDirectInputDevice8A* real_;
    volatile LONG refs_ = 1;
};

class DirectInput final : public IDirectInput8A {
   public:
    explicit DirectInput(IDirectInput8A* real) : real_(real) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirectInput8A)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return real_->QueryInterface(iid, out);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            real_->Release();
            delete this;
        }
        return refs;
    }

    STDMETHODIMP CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override {
        const HRESULT result = real_->CreateDevice(guid, out, outer);
        if (SUCCEEDED(result) && IsEqualGUID(guid, GUID_SysMouse) && input_rawmouse_start()) {
            *out = new MouseDevice(*out);
            logf("input: mouse now served from Raw Input");
        }
        return result;
    }

    STDMETHODIMP EnumDevices(DWORD a, LPDIENUMDEVICESCALLBACKA b, LPVOID c, DWORD d) override {
        return real_->EnumDevices(a, b, c, d);
    }
    STDMETHODIMP GetDeviceStatus(REFGUID a) override { return real_->GetDeviceStatus(a); }
    STDMETHODIMP RunControlPanel(HWND a, DWORD b) override { return real_->RunControlPanel(a, b); }
    STDMETHODIMP Initialize(HINSTANCE a, DWORD b) override { return real_->Initialize(a, b); }
    STDMETHODIMP FindDevice(REFGUID a, LPCSTR b, LPGUID c) override {
        return real_->FindDevice(a, b, c);
    }
    STDMETHODIMP EnumDevicesBySemantics(LPCSTR a, LPDIACTIONFORMATA b,
                                        LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID d,
                                        DWORD e) override {
        return real_->EnumDevicesBySemantics(a, b, c, d, e);
    }
    STDMETHODIMP ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a, LPDICONFIGUREDEVICESPARAMSA b,
                                  DWORD c, LPVOID d) override {
        return real_->ConfigureDevices(a, b, c, d);
    }

   private:
    IDirectInput8A* real_;
    volatile LONG refs_ = 1;
};

HRESULT WINAPI hook_direct_input_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out,
                                        LPUNKNOWN outer) {
    const HRESULT result = g_real_create(instance, version, iid, out, outer);
    if (SUCCEEDED(result) && IsEqualIID(iid, IID_IDirectInput8A)) {
        *out = static_cast<IDirectInput8A*>(new DirectInput(static_cast<IDirectInput8A*>(*out)));
    } else {
        logf("input: DirectInput8Create left unwrapped (hr=%08lX)", result);
    }
    return result;
}

// ---- Aim speed ----------------------------------------------------------------------------

float read_aim_scale() {
    if (!g_game || !g_verified) return 1.0f;
    auto*** state = reinterpret_cast<char***>(g_game + kGameState);
    if (!*state || !**state) return 1.0f;
    return *reinterpret_cast<float*>(**state + kAimScale);
}

float zoom_scale() {
    if (g_cfg.zoom_mode == ZoomMode::Stock) return 1.0f;

    const float game_scale = read_aim_scale();
    if (game_scale <= 0.0f) return 1.0f;
    if (game_scale >= 0.999f && g_fov > g_hip_fov) g_hip_fov = g_fov;
    if (g_cfg.zoom_mode == ZoomMode::Off || g_fov <= 0.0f || g_hip_fov <= 0.0f) {
        return 1.0f / game_scale;
    }
    const float ratio = tanf(g_fov * 0.5f * kDegToRad) / tanf(g_hip_fov * 0.5f * kDegToRad);
    return ratio / game_scale;
}

// Logs how far one mouse count turns the view. Past about a pixel, slow aiming looks steppy.
void report_aim_step() {
    if (!g_mouse_sensitivity) return;
    const float sensitivity = g_mouse_sensitivity();
    if (sensitivity == g_reported_sensitivity) return;
    g_reported_sensitivity = sensitivity;

    const float radians =
        sensitivity * g_cfg.sensitivity_x * kLookRate / g_cfg.reference_hz;
    const float half_tan = tanf(view_adjust_fov(g_fov, g_mode.aspect) * 0.5f * kDegToRad);
    const float pixels = radians * (g_mode.width * 0.5f) / half_tan;
    logf("input: aim step %.4f degrees per count, %.2f pixels at screen centre (sensitivity %.3f)",
         radians / kDegToRad, pixels, sensitivity);
}

float look_scale(bool horizontal) {
    report_aim_step();
    float scale = horizontal ? g_cfg.sensitivity_x : g_cfg.sensitivity_y;
    if (g_cfg.fps_independent) {
        // The look function multiplies its input by frame time, but mouse movement is
        // already a total for the frame. Dividing it back out keeps aim speed constant.
        double delta = g_last_delta();
        if (delta < 0.00025) delta = 0.00025;
        if (delta > 0.1) delta = 0.1;
        scale *= static_cast<float>((1.0 / g_cfg.reference_hz) / delta);
    }
    return scale * zoom_scale();
}

float __cdecl hook_impulse_value(int id) {
    const float value = g_real_impulse_value(id);
    if (id < kImpulseMouseLeft || id > kImpulseMouseDown || value == 0.0f) return value;

    const auto offset =
        reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_game);
    if (offset < kLookBegin || offset >= kLookEnd) return value;
    return value * look_scale(id <= kImpulseMouseRight);
}

}  // namespace

bool input_rawmouse_start() { return g_cfg.raw_input && g_raw.start(); }

void input_rawmouse_read(long* dx, long* dy) {
    long wheel;
    g_raw.take_motion(dx, dy, &wheel);
}

long input_rawmouse_events() { return g_raw.event_count(); }

bool input_is_zoomed() { return read_aim_scale() < 0.999f; }

int input_test_wheel() {
    WheelPacer pacer;
    int failures = 0;
    const auto expect = [&](long incoming, bool paced, double now, long wanted) {
        if (pacer.take(incoming, paced, now) != wanted) ++failures;
    };
    expect(360, true, 0.00, 120);    // three quick notches: the first goes straight through
    expect(0, true, 0.05, 0);        // the game is still locked out
    expect(0, true, 0.08, 120);      // the second waits for the lockout to end
    expect(0, true, 0.16, 120);      // and so does the third
    expect(0, true, 0.24, 0);        // nothing lost, and nothing extra
    expect(240, true, 0.30, 120);    // two more
    expect(-120, true, 0.31, 0);     // changing direction drops what was queued
    expect(0, true, 0.40, -120);     // and only the new direction arrives
    expect(0, true, 0.50, 0);
    expect(360, false, 0.60, 360);   // outside a sight, the wheel isn't held back
    return failures;
}

void input_set_fov(float game_fov_degrees) { g_fov = game_fov_degrees; }

void input_attach_di8(HMODULE di8) {
    HMODULE exe = GetModuleHandleW(nullptr);
    g_inp_event = reinterpret_cast<InpEventFn>(GetProcAddress(exe, "inp_event"));
    g_mouse_sensitivity =
        reinterpret_cast<MouseSensitivityFn>(GetProcAddress(exe, "inp_get_mouse_sensitivity"));
    if (!g_cfg.raw_input) {
        logf("input: raw input disabled by config");
        return;
    }
    const bool ok = iat_patch(di8, "DINPUT8.dll", "DirectInput8Create",
                              reinterpret_cast<void*>(&hook_direct_input_create),
                              reinterpret_cast<void**>(&g_real_create));
    logf("input: DirectInput hook %s (extra buttons %s)", ok ? "installed" : "FAILED",
         g_inp_event ? "available" : "unavailable");
}

void input_attach_game(HMODULE game, bool verified) {
    g_game = reinterpret_cast<char*>(game);
    g_verified = verified;
}

// game.dll binds its engine API by name into pointer slots, so swapping the slot catches
// every call without touching any code.
void input_game_ready() {
    if (!g_verified) return;

    auto** impulse_slot = reinterpret_cast<void**>(g_game + kSlotImpulseValue);
    auto** delta_slot = reinterpret_cast<void**>(g_game + kSlotLastDelta);
    if (!*impulse_slot || !*delta_slot) {
        logf("input: engine API not bound yet, aim fix unavailable");
        return;
    }
    g_real_impulse_value = reinterpret_cast<ImpulseValueFn>(*impulse_slot);
    g_last_delta = reinterpret_cast<LastDeltaFn>(*delta_slot);
    *impulse_slot = reinterpret_cast<void*>(&hook_impulse_value);
    logf("input: aim speed hook installed");
}

}  // namespace longshot
