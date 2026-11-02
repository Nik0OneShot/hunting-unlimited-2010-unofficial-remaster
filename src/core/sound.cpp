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

// The engine has no threads, so opening and decoding Ogg files holds up the frame.
using OpenFn = int(__cdecl*)(void*, void*, char*, long, void*, void*, void*, void*);
using ReadFn = long(__cdecl*)(void*, char*, int, int, int, int, int*);

OpenFn g_real_open = nullptr;
ReadFn g_real_read = nullptr;

int __cdecl hook_open(void* source, void* file, char* initial, long size, void* read, void* seek, void* close, void* tell) {
    ProfileTimer timer(ProfileSection::Decode);
    return g_real_open(source, file, initial, size, read, seek, close, tell);
}

long __cdecl hook_read(void* file, char* out, int size, int big_endian, int word, int is_signed, int* stream) {
    ProfileTimer timer(ProfileSection::Decode);
    return g_real_read(file, out, size, big_endian, word, is_signed, stream);
}

void time_decoding() {
    HMODULE shared = GetModuleHandleW(L"sndshared.dll");
    if (!shared) return;
    iat_patch(shared, "vorbisfile.dll", "ov_open_callbacks", reinterpret_cast<void*>(&hook_open), reinterpret_cast<void**>(&g_real_open));
    iat_patch(shared, "vorbisfile.dll", "ov_read", reinterpret_cast<void*>(&hook_read), reinterpret_cast<void**>(&g_real_read));
}

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
    time_decoding();
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
