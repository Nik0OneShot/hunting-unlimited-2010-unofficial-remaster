#include "longshot.h"

#include <cstring>
#include <string>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kSetSmell = 0xb8f70;
constexpr uintptr_t kSetSmellCall = 0xbd5e;
constexpr uintptr_t kSetSight = 0xb8f90;
constexpr uintptr_t kSetSightCall = 0xbe41;
constexpr uintptr_t kLadderViewClass = 0x19c2e8;
constexpr size_t kPlayerView = 0x8c;
constexpr size_t kLadderViewOffset = 0x144;

// The stands all use the ladder view, and only the tree stand's script gives it no view offset.
constexpr float kNoViewOffset = 0.0f;

// Leaves are all drawn by one fragment program, whose fog coordinate is how far away each one is.
// Leaves past kLeavesFrom are left alone, and from there in they thin out to what's left at
// kLeavesTo, which stays well above the alpha test's 0.2.
constexpr float kLeavesFrom = 10.0f;
constexpr float kLeavesTo = 4.0f;
constexpr float kLeavesLeft = 0.3f;
constexpr unsigned kLeavesEnv = 20;
constexpr unsigned kFragmentProgram = 0x8804;  // GL_FRAGMENT_PROGRAM_ARB

constexpr char kLeafSource[] = "dif_opac/hu.dif_opac.cg";
constexpr char kLeafAlpha[] = "MUL result.color.w, R0, fragment.color.primary;";

// The environment parameter starts as all zeros, which leaves every leaf as it was. With
// multisampling, a lower alpha becomes partial coverage. Without it the alpha test makes each pixel
// all or nothing, so w turns on a repeating pattern of pixels to drop instead.
constexpr char kNearLeaves[] =
    "PARAM near_leaves = program.env[20];\n"
    "PARAM leaf_numbers = { 1, 0.5, 0.375, 0 };\n"
    "TEMP R2;\n"
    "SUB R2.x, near_leaves.x, fragment.fogcoord.x;\n"
    "MUL_SAT R2.x, R2.x, near_leaves.y;\n"
    "MUL R2.x, R2.x, near_leaves.z;\n"
    "SUB R2.x, leaf_numbers.x, R2.x;\n"
    "MUL R2.yz, fragment.position.xxyy, leaf_numbers.y;\n"
    "FRC R2.yz, R2;\n"
    "MUL R2.y, R2.y, leaf_numbers.y;\n"
    "ADD R2.y, R2.y, R2.z;\n"
    "SUB R2.y, R2.y, leaf_numbers.z;\n"
    "MUL R2.y, R2.y, near_leaves.w;\n"
    "SUB R2.w, R2.x, R2.y;\n"
    "KIL R2.w;\n"
    "MUL R0.w, R0, fragment.color.primary;\n"
    "MUL result.color.w, R0, R2.x;";

using SetValueFn = void(__fastcall*)(void*, void*, float);
using ProgramStringFn = void(WINAPI*)(unsigned, unsigned, int, const void*);
using ProgramEnvFn = void(WINAPI*)(unsigned, unsigned, float, float, float, float);
using GetErrorFn = unsigned(WINAPI*)();
using GenProgramsFn = void(WINAPI*)(int, unsigned*);
using BindProgramFn = void(WINAPI*)(unsigned, unsigned);
using DeleteProgramsFn = void(WINAPI*)(int, const unsigned*);

constexpr unsigned kProgramAscii = 0x8875;  // GL_PROGRAM_FORMAT_ASCII_ARB

char* g_game = nullptr;
bool g_ready = false;
bool g_was_hidden = false;
bool g_leaves_on = false;
int g_leaf_programs = 0;
ProgramStringFn g_real_program_string = nullptr;
ProgramEnvFn g_program_env = nullptr;

template <typename Fn>
Fn gl_function(const char* name) {
    return reinterpret_cast<Fn>(graphics_gl_function(name));
}

bool up_a_tree() {
    char* player = walking_player();
    if (!player || walking_view_class(player) != g_game + kLadderViewClass) return false;
    char* view = *reinterpret_cast<char**>(player + kPlayerView);
    return *reinterpret_cast<float*>(view + kLadderViewOffset) == kNoViewOffset;
}

bool hidden() {
    const bool now = g_cfg.stand_hidden && up_a_tree();
    if (now != g_was_hidden) {
        g_was_hidden = now;
        logf("stand: %s", now ? "up a tree, so animals can't see or smell you" : "back down");
    }
    return now;
}

void __fastcall hook_set_smell(void* hunter, void* unused, float smell) {
    reinterpret_cast<SetValueFn>(g_game + kSetSmell)(hunter, unused, hidden() ? 0.0f : smell);
}

void __fastcall hook_set_sight(void* hunter, void* unused, float sight) {
    reinterpret_cast<SetValueFn>(g_game + kSetSight)(hunter, unused, hidden() ? 0.0f : sight);
}

void WINAPI hook_program_string(unsigned target, unsigned format, int length, const void* text) {
    const std::string changed = stand_leaf_program(std::string(static_cast<const char*>(text), length));
    if (changed.empty()) {
        g_real_program_string(target, format, length, text);
        return;
    }
    const auto get_error = gl_function<GetErrorFn>("glGetError");
    while (get_error && get_error() != 0) {
    }
    g_real_program_string(target, format, static_cast<int>(changed.size()), changed.data());
    if (get_error && get_error() != 0) {
        logf("stand: the driver turned down the leaf program, so leaves stay as they were");
        g_real_program_string(target, format, length, text);
        return;
    }
    ++g_leaf_programs;
    if (g_leaf_programs == 1) logf("stand: leaves near a tree stand can thin out");
}

}  // namespace

std::string stand_leaf_program(const std::string& program) {
    if (program.find(kLeafSource) == std::string::npos) return std::string();
    const size_t at = program.find(kLeafAlpha);
    if (at == std::string::npos) return std::string();
    return program.substr(0, at) + kNearLeaves + program.substr(at + sizeof(kLeafAlpha) - 1);
}

void stand_before_world() {
    if (!g_ready || !g_cfg.stand_clear_view || !up_a_tree()) return;
    if (!g_program_env) g_program_env = gl_function<ProgramEnvFn>("glProgramEnvParameter4fARB");
    if (!g_program_env) return;
    g_program_env(kFragmentProgram, kLeavesEnv, kLeavesFrom, 1.0f / (kLeavesFrom - kLeavesTo), 1.0f - kLeavesLeft,
                  g_cfg.anti_aliasing > 0 ? 0.0f : 1.0f);
    g_leaves_on = true;
}

void stand_after_world() {
    if (!g_leaves_on) return;
    g_leaves_on = false;
    g_program_env(kFragmentProgram, kLeavesEnv, 0.0f, 0.0f, 0.0f, 0.0f);
}

void* stand_wrap_gl(const char* name, void* proc) {
    if (!strcmp(name, "glProgramStringARB")) {
        g_real_program_string = reinterpret_cast<ProgramStringFn>(proc);
        return reinterpret_cast<void*>(&hook_program_string);
    }
    return proc;
}

void stand_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    const bool smell = patch_call(g_game + kSetSmellCall, g_game + kSetSmell, reinterpret_cast<const void*>(&hook_set_smell));
    const bool sight = patch_call(g_game + kSetSightCall, g_game + kSetSight, reinterpret_cast<const void*>(&hook_set_sight));
    g_ready = smell && sight;
    logf("stand: smell=%d sight=%d", smell, sight);
}

int stand_test() {
    const std::string game =
        "!!ARBfp1.0\n# source file: dif_opac/hu.dif_opac.cg\nPARAM c[2] = { program.local[0],\n{ 1.4423828, 2 } };\n"
        "TEMP R0;\nTEMP R1;\nTEX R0, fragment.texcoord[0], texture[0], 2D;\nMUL R1.x, fragment.fogcoord, c[0].w;\n"
        "MUL result.color.xyz, R0, c[1].y;\nMUL result.color.w, R0, fragment.color.primary;\nEND\n";
    const std::string changed = stand_leaf_program(game);
    if (changed.empty()) return 1;
    if (changed.find(kLeafAlpha) != std::string::npos) return 2;
    if (changed.find("program.env[20]") == std::string::npos) return 3;
    if (!stand_leaf_program("!!ARBfp1.0\n" + std::string(kLeafAlpha) + "\nEND\n").empty()) return 4;  // not a leaf program
    if (!stand_leaf_program("# source file: dif_opac/hu.dif_opac.cg\nEND\n").empty()) return 5;       // nothing to change

    // The driver has to take it, or leaves would fall back to how they were.
    int failed = 0;
    bool compiled = false;
    graphics_in_context(64, 0, [&]() {
        const auto gen = gl_function<GenProgramsFn>("glGenProgramsARB");
        const auto bind = gl_function<BindProgramFn>("glBindProgramARB");
        const auto remove = gl_function<DeleteProgramsFn>("glDeleteProgramsARB");
        const auto text = gl_function<ProgramStringFn>("glProgramStringARB");
        const auto get_error = gl_function<GetErrorFn>("glGetError");
        if (!gen || !bind || !remove || !text || !get_error) return;
        unsigned program = 0;
        gen(1, &program);
        bind(kFragmentProgram, program);
        while (get_error() != 0) {
        }
        text(kFragmentProgram, kProgramAscii, static_cast<int>(changed.size()), changed.data());
        compiled = get_error() == 0;
        remove(1, &program);
        if (!compiled) failed = 6;
    });
    return failed;
}

}  // namespace longshot
