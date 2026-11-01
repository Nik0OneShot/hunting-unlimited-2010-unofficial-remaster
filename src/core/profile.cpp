#include "longshot.h"

#include <psapi.h>

#include <cstdio>
#include <cstring>

namespace longshot {

namespace {

// Offsets in game.dll, valid only for the verified build.
constexpr uint32_t kGameVtable = 0x1b06cc;
constexpr size_t kMethodFrame = 0x04;
constexpr uint32_t kRenderCall = 0x10360d;  // where the frame calls its render function
constexpr uint32_t kRender = 0x102df0;
constexpr uint32_t kSlotSoundUpdate = 0x2d0664;  // bound pointer to snd_update_state

constexpr char kFsOpen[] =
    "?fs_open@prism@@YA?AVret_t@?$auto_ptr_t@Vfile_t@prism@@@1@ABVstring@1@W4fs_realm_t@1@@Z";
constexpr char kFileRead[] = "?read@file_t@prism@@QAE_NQAXI@Z";
constexpr char kFileBlockRead[] = "?block_read@file_t@prism@@QAE_NQAXI_K@Z";

constexpr int kMaxSlowLines = 12;  // in one reporting window

using FrameFn = void(__thiscall*)(void*);
using SoundUpdateFn = void(__cdecl*)();
using FsOpenFn = void**(__cdecl*)(void**, const char* const*, int);
using FileReadFn = bool(__thiscall*)(void*, void*, unsigned int);
using FileBlockReadFn = bool(__thiscall*)(void*, void*, unsigned int, uint64_t);
using TexImageFn = void(WINAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
using TexSubImageFn = void(WINAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
using CompressedTexImageFn = void(WINAPI*)(unsigned, int, unsigned, int, int, int, int, const void*);
using BufferDataFn = void(WINAPI*)(unsigned, ptrdiff_t, const void*, unsigned);

struct Totals {
    double seconds[static_cast<int>(ProfileSection::Count)] = {};
    int files = 0;
    int uploads = 0;
};

Totals g_frame;
Totals g_window;
int g_window_frames = 0;
int g_window_slow = 0;
double g_frame_started = 0.0;
double g_render_started = 0.0;
double g_ui_started = 0.0;

char* g_game = nullptr;
FrameFn g_real_frame = nullptr;
SoundUpdateFn g_real_sound_update = nullptr;
FsOpenFn g_real_fs_open = nullptr;
FileReadFn g_real_read = nullptr;
FileBlockReadFn g_real_block_read = nullptr;
TexImageFn g_real_tex_image = nullptr;
TexSubImageFn g_real_tex_sub_image = nullptr;
CompressedTexImageFn g_real_compressed_tex_image = nullptr;
BufferDataFn g_real_buffer_data = nullptr;

void add(ProfileSection section, double seconds) {
    g_frame.seconds[static_cast<int>(section)] += seconds;
}

double seconds_of(const Totals& totals, ProfileSection section) {
    return totals.seconds[static_cast<int>(section)];
}

void __fastcall hook_game_frame(void* game) {
    g_frame_started = now_seconds();
    g_real_frame(game);
}

void __fastcall hook_render(void* game) {
    g_render_started = now_seconds();
    add(ProfileSection::Sim, g_render_started - g_frame_started);
    reinterpret_cast<FrameFn>(g_game + kRender)(game);
}

void __cdecl hook_sound_update() {
    ProfileTimer timer(ProfileSection::Sound);
    g_real_sound_update();
}

void** __cdecl hook_fs_open(void** out, const char* const* path, int realm) {
    ProfileTimer timer(ProfileSection::Files);
    ++g_frame.files;
    return g_real_fs_open(out, path, realm);
}

bool __fastcall hook_read(void* file, void*, void* data, unsigned int size) {
    ProfileTimer timer(ProfileSection::Files);
    return g_real_read(file, data, size);
}

bool __fastcall hook_block_read(void* file, void*, void* data, unsigned int size, uint64_t offset) {
    ProfileTimer timer(ProfileSection::Files);
    return g_real_block_read(file, data, size, offset);
}

void WINAPI hook_tex_image(unsigned target, int level, int inner, int width, int height, int border,
                           unsigned format, unsigned type, const void* pixels) {
    ProfileTimer timer(ProfileSection::Uploads);
    ++g_frame.uploads;
    g_real_tex_image(target, level, inner, width, height, border, format, type, pixels);
}

void WINAPI hook_tex_sub_image(unsigned target, int level, int x, int y, int width, int height,
                               unsigned format, unsigned type, const void* pixels) {
    ProfileTimer timer(ProfileSection::Uploads);
    ++g_frame.uploads;
    g_real_tex_sub_image(target, level, x, y, width, height, format, type, pixels);
}

void WINAPI hook_compressed_tex_image(unsigned target, int level, unsigned inner, int width, int height,
                                      int border, int size, const void* data) {
    ProfileTimer timer(ProfileSection::Uploads);
    ++g_frame.uploads;
    g_real_compressed_tex_image(target, level, inner, width, height, border, size, data);
}

void WINAPI hook_buffer_data(unsigned target, ptrdiff_t size, const void* data, unsigned usage) {
    ProfileTimer timer(ProfileSection::Uploads);
    ++g_frame.uploads;
    g_real_buffer_data(target, size, data, usage);
}

void add_totals(Totals* into, const Totals& from) {
    for (int i = 0; i < static_cast<int>(ProfileSection::Count); ++i) {
        into->seconds[i] += from.seconds[i];
    }
    into->files += from.files;
    into->uploads += from.uploads;
}

void format_sections(char* out, size_t size, const Totals& totals, double scale) {
    const auto ms = [&](ProfileSection section) { return seconds_of(totals, section) * scale * 1000.0; };
    snprintf(out, size, "sim %.2f world %.2f ui %.2f swap %.2f sound %.2f | post %.2f | "
             "files %.2f ms, %d opened | uploads %.2f ms, %d calls",
             ms(ProfileSection::Sim), ms(ProfileSection::World), ms(ProfileSection::Ui),
             ms(ProfileSection::Swap), ms(ProfileSection::Sound), ms(ProfileSection::Post),
             ms(ProfileSection::Files), totals.files, ms(ProfileSection::Uploads), totals.uploads);
}

// Time outside every section, like reading the screen back for a screenshot. Files and Uploads
// aren't counted here, because they happen inside the other sections.
double unaccounted_ms(float frame_ms) {
    const double seconds = seconds_of(g_frame, ProfileSection::Sim) +
                           seconds_of(g_frame, ProfileSection::World) +
                           seconds_of(g_frame, ProfileSection::Ui) +
                           seconds_of(g_frame, ProfileSection::Swap) +
                           seconds_of(g_frame, ProfileSection::Sound);
    const double other = frame_ms - seconds * 1000.0;
    return other > 0.0 ? other : 0.0;
}

// The game is 32-bit, so this shows how close it gets to its address space running out.
void report_memory() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                              sizeof(memory))) {
        return;
    }
    constexpr size_t kMegabyte = 1024 * 1024;
    logf("memory: %u MB now, %u MB at most", static_cast<unsigned>(memory.PrivateUsage / kMegabyte),
         static_cast<unsigned>(memory.PeakPagefileUsage / kMegabyte));
}

}  // namespace

ProfileTimer::ProfileTimer(ProfileSection section) : section_(section), started_(now_seconds()) {}

ProfileTimer::~ProfileTimer() { add(section_, now_seconds() - started_); }

void profile_ui_begin() {
    g_ui_started = now_seconds();
    if (g_render_started > 0.0) add(ProfileSection::World, g_ui_started - g_render_started);
}

void profile_ui_end() { add(ProfileSection::Ui, now_seconds() - g_ui_started); }

bool profile_is_slow(float frame_ms, float typical_ms) {
    return typical_ms > 0.0f && frame_ms > typical_ms * 2.0f;
}

void profile_frame_done(float frame_ms, float typical_ms, bool focused) {
    ++g_window_frames;
    add_totals(&g_window, g_frame);

    if (focused && profile_is_slow(frame_ms, typical_ms)) {
        ++g_window_slow;
        if (g_window_slow <= kMaxSlowLines) {
            char text[180];
            format_sections(text, sizeof(text), g_frame, 1.0);
            logf("slow frame: %.2f ms | other %.2f %s", frame_ms, unaccounted_ms(frame_ms), text);
        }
    }
    g_frame = Totals{};
    g_render_started = 0.0;
}

void profile_report_window() {
    if (g_window_frames == 0) return;
    char text[180];
    format_sections(text, sizeof(text), g_window, 1.0 / g_window_frames);
    logf("frame average: %s", text);
    report_memory();
    if (g_window_slow > kMaxSlowLines) {
        logf("slow frames: %d more not listed", g_window_slow - kMaxSlowLines);
    }
    g_window = Totals{};
    g_window_frames = 0;
    g_window_slow = 0;
}

void profile_attach_module(HMODULE mod) {
    auto wrap = [mod](const char* name, void* hook, void** real) {
        void* before = *real;
        void* original = nullptr;
        if (!iat_patch(mod, "p3core.dll", name, hook, &original)) return;
        // Every module imports the same engine function, so one saved pointer does for them all.
        if (!before) *real = original;
    };
    wrap(kFsOpen, reinterpret_cast<void*>(&hook_fs_open), reinterpret_cast<void**>(&g_real_fs_open));
    wrap(kFileRead, reinterpret_cast<void*>(&hook_read), reinterpret_cast<void**>(&g_real_read));
    wrap(kFileBlockRead, reinterpret_cast<void*>(&hook_block_read),
         reinterpret_cast<void**>(&g_real_block_read));
}

void profile_attach_game(HMODULE game, bool verified) {
    profile_attach_module(game);
    if (!verified) return;

    g_game = reinterpret_cast<char*>(game);
    auto** table = reinterpret_cast<void**>(g_game + kGameVtable);
    const bool frame = vtable_patch(table + kMethodFrame / sizeof(void*),
                                    reinterpret_cast<void*>(&hook_game_frame),
                                    reinterpret_cast<void**>(&g_real_frame));

    // The other places that call the render function are the loading screens.
    const bool render = patch_call(g_game + kRenderCall, g_game + kRender,
                                   reinterpret_cast<const void*>(&hook_render));
    logf("profile: frame=%d render=%d", frame, render);
}

void profile_game_ready() {
    if (!g_game) return;
    auto** slot = reinterpret_cast<void**>(g_game + kSlotSoundUpdate);
    if (!*slot) return;
    g_real_sound_update = reinterpret_cast<SoundUpdateFn>(*slot);
    *slot = reinterpret_cast<void*>(&hook_sound_update);
}

void* profile_wrap_gl(const char* name, void* proc) {
    if (!strcmp(name, "glTexImage2D")) {
        g_real_tex_image = reinterpret_cast<TexImageFn>(proc);
        return reinterpret_cast<void*>(&hook_tex_image);
    }
    if (!strcmp(name, "glTexSubImage2D")) {
        g_real_tex_sub_image = reinterpret_cast<TexSubImageFn>(proc);
        return reinterpret_cast<void*>(&hook_tex_sub_image);
    }
    if (!strcmp(name, "glCompressedTexImage2DARB")) {
        g_real_compressed_tex_image = reinterpret_cast<CompressedTexImageFn>(proc);
        return reinterpret_cast<void*>(&hook_compressed_tex_image);
    }
    if (!strcmp(name, "glBufferDataARB")) {
        g_real_buffer_data = reinterpret_cast<BufferDataFn>(proc);
        return reinterpret_cast<void*>(&hook_buffer_data);
    }
    return proc;
}

}  // namespace longshot
