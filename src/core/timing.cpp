#include "longshot.h"

#include <mmsystem.h>

namespace longshot {

namespace {

constexpr char kTmrRead[] = "?tmr_read@prism@@YA_KXZ";

LONGLONG g_freq = 0;
LONGLONG g_start = 0;
uint64_t g_base_usec = 0;

// Replaces `timeGetTime() * 1000`. sys_frame derives every frame delta from this, so a
// 1 ms clock rounds a 6.06 ms frame at 165 Hz to 6 or 7 ms, about 15% jitter per frame.
uint64_t __cdecl hook_tmr_read() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t ticks = static_cast<uint64_t>(now.QuadPart - g_start);
    const uint64_t freq = static_cast<uint64_t>(g_freq);
    return g_base_usec + (ticks / freq) * 1000000 + ((ticks % freq) * 1000000) / freq;
}

}  // namespace

double now_seconds() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void timing_install(HMODULE p3core) {
    if (!g_cfg.high_res_timer) {
        logf("timing: disabled by config");
        return;
    }

    void* target = GetProcAddress(p3core, kTmrRead);
    if (!target) {
        logf("timing: tmr_read not found, keeping the 1 ms clock");
        return;
    }

    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    g_freq = value.QuadPart;
    QueryPerformanceCounter(&value);
    g_start = value.QuadPart;
    g_base_usec = static_cast<uint64_t>(timeGetTime()) * 1000;

    // tmr_read is never called through the original again, so a plain jump is enough.
    const bool ok = write_jump(target, reinterpret_cast<void*>(&hook_tmr_read));
    logf("timing: high resolution clock %s (%lld Hz)", ok ? "installed" : "FAILED", g_freq);
}

}  // namespace longshot
