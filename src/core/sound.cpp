#include "longshot.h"

#include <cstring>

namespace longshot {

namespace {

// Where ds8.dll picks the flags for a new sound buffer: mov edi, imm32.
constexpr uint32_t kBufferFlags = 0x1191;
constexpr uint32_t kBufferFlags3d = 0x1198;
constexpr uint32_t kPlainFlags = 0xa0;
constexpr uint32_t k3dFlags = 0xb0;
constexpr uint32_t kGlobalFocus = 0x8000;  // DSBCAPS_GLOBALFOCUS

constexpr uint8_t kMovEdi = 0xBF;

bool add_global_focus(char* ds8, uint32_t rva, uint32_t flags) {
    const uint32_t changed = flags | kGlobalFocus;
    uint8_t expected[5] = {kMovEdi};
    uint8_t replacement[5] = {kMovEdi};
    memcpy(expected + 1, &flags, sizeof(flags));
    memcpy(replacement + 1, &changed, sizeof(changed));
    return patch_bytes(ds8 + rva, expected, replacement, sizeof(expected));
}

}  // namespace

// Windows silences these buffers while their window is out of focus, and brings them back with it.
// That suits a paused hunt, where it's the only thing that can quiet a sound without losing it. A
// hunt that keeps running should stay audible, so that case opts out.
void sound_attach_ds8(HMODULE ds8, bool verified) {
    if (g_cfg.pause_in_background) {
        logf("sound: silent in the background");
        return;
    }
    if (!verified) {
        logf("sound: unknown ds8.dll, silent in the background");
        return;
    }
    char* base = reinterpret_cast<char*>(ds8);
    const bool plain = add_global_focus(base, kBufferFlags, kPlainFlags);
    const bool spatial = add_global_focus(base, kBufferFlags3d, k3dFlags);
    logf("sound: plays in the background plain=%d 3d=%d", plain, spatial);
}

}  // namespace longshot
