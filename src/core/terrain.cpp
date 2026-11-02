#include "longshot.h"

#include <cstdint>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kKeepPhysics = 0xd8eab;    // inside the cmp that says how many chunks keep their physics
constexpr uintptr_t kKeepTriangles = 0xd8f5b;  // and their triangles
constexpr uintptr_t kUseTriangles = 0xda590;
constexpr uintptr_t kDropTriangles = 0xd9420;
constexpr uintptr_t kPieceCount = 0x171b10;
constexpr uintptr_t kUsePhysics = 0xda170;
constexpr uintptr_t kUsePhysicsCall = 0xd77e9;
constexpr uintptr_t kPhysicsStale = 0xd8ed0;    // the ja for a chunk's physics not used for 100 frames
constexpr uintptr_t kTrianglesFresh = 0xd8f80;  // the jbe for its triangles being used since then
constexpr size_t kLandscapeChunks = 0x2c;
constexpr size_t kLandscapeChunkCount = 0x30;
constexpr size_t kLandscapeHeap = 0xb0;
constexpr size_t kChunkMesh = 0x20;
constexpr size_t kChunkPhysicsHeld = 0xa1;
constexpr size_t kChunkHeld = 0xc4;
constexpr size_t kChunkLastUsed = 0xc8;
constexpr size_t kMeshFlags = 0x6;
constexpr size_t kMeshModel = 0x60;
constexpr size_t kHeapSlots = 0x8;
constexpr size_t kHeapFreeSlots = 0x18;

constexpr uint8_t kMeshAlive = 0x80;
constexpr uint8_t kGameChunksKept = 9;
constexpr uint8_t kChunksKept = 127;  // the most this cmp can hold

// The heap's slot number goes in one byte of each triangle's id, and it refuses an append once it
// has grown to 255 slots, even if some are free, because the count of them never comes back down.
constexpr unsigned kMostPieces = 254;

// A chunk also goes once it's unused for 100 frames, which is half a second at 200 FPS. Animals only
// look at the ground now and then, so chunks were rebuilt over and over for them, at 2.7 ms each.
// Nothing is short of room, so they stay until the room is wanted.
constexpr unsigned char kGamePhysicsStale[] = {0x77, 0x0E};
constexpr unsigned char kNeverStale[] = {0x90, 0x90};
constexpr unsigned char kGameTrianglesFresh[] = {0x76, 0x16};
constexpr unsigned char kAlwaysFresh[] = {0xEB, 0x16};

struct UseSite {
    uintptr_t at;
    const char* name;
};

constexpr UseSite kUseSites[] = {
    {0xd436a, "ground"},
    {0xd773a, "shadows"},
    {0xd428a, "collisions"},
    {0xf1e9a, "grass"},
};
constexpr int kUseSiteCount = sizeof(kUseSites) / sizeof(kUseSites[0]);

using UseTrianglesFn = void(__thiscall*)(void*, void*);
using DropTrianglesFn = bool(__thiscall*)(void*, void*);
using PieceCountFn = unsigned(__thiscall*)(void*);
using UsePhysicsFn = void(__thiscall*)(void*, void*, void*, void*);

char* g_game = nullptr;
int g_loaded[kUseSiteCount] = {};
int g_dropped = 0;
int g_physics_loaded = 0;

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

unsigned pieces_held(char* heap) {
    return field<unsigned>(heap, kHeapSlots) - field<unsigned>(heap, kHeapFreeSlots);
}

unsigned pieces_in(char* chunk) {
    char* mesh = field<char*>(chunk, kChunkMesh);
    if (!mesh || !(field<uint8_t>(mesh, kMeshFlags) & kMeshAlive)) return 0;
    void* model = field<void*>(mesh, kMeshModel);
    return model ? reinterpret_cast<PieceCountFn>(g_game + kPieceCount)(model) : 0;
}

// Lets go of the chunks used longest ago until `wanted` fits, and says how many that took.
template <typename Drop>
int make_room(char* heap, char** chunks, unsigned count, const char* wanted, unsigned pieces, Drop drop) {
    int dropped = 0;
    while (pieces_held(heap) + pieces > kMostPieces) {
        char* oldest = nullptr;
        for (unsigned i = 0; i < count; ++i) {
            char* chunk = chunks[i];
            if (chunk == wanted || !field<uint8_t>(chunk, kChunkHeld)) continue;
            if (!oldest || field<unsigned>(chunk, kChunkLastUsed) < field<unsigned>(oldest, kChunkLastUsed)) {
                oldest = chunk;
            }
        }
        if (!oldest) break;
        drop(oldest);
        ++dropped;
    }
    return dropped;
}

template <int site>
void __fastcall hook_use_triangles(char* chunk, void*, char* heap) {
    if (!field<uint8_t>(chunk, kChunkHeld)) {
        ProfileTimer timer(ProfileSection::Terrain);
        ++g_loaded[site];
        char* landscape = heap - kLandscapeHeap;
        const auto drop = [heap](char* oldest) {
            reinterpret_cast<DropTrianglesFn>(g_game + kDropTriangles)(oldest, heap);
        };
        g_dropped += make_room(heap, field<char**>(landscape, kLandscapeChunks),
                               field<unsigned>(landscape, kLandscapeChunkCount), chunk, pieces_in(chunk), drop);
        reinterpret_cast<UseTrianglesFn>(g_game + kUseTriangles)(chunk, heap);
        return;
    }
    reinterpret_cast<UseTrianglesFn>(g_game + kUseTriangles)(chunk, heap);
}

void __fastcall hook_use_physics(char* chunk, void*, void* world, void* box, void* out) {
    const auto use = reinterpret_cast<UsePhysicsFn>(g_game + kUsePhysics);
    if (field<uint8_t>(chunk, kChunkPhysicsHeld)) {
        use(chunk, world, box, out);
        return;
    }
    ProfileTimer timer(ProfileSection::Terrain);
    use(chunk, world, box, out);
    if (field<uint8_t>(chunk, kChunkPhysicsHeld)) ++g_physics_loaded;
}

using UseHookFn = void(__fastcall*)(char*, void*, char*);

constexpr UseHookFn kUseHooks[kUseSiteCount] = {
    &hook_use_triangles<0>,
    &hook_use_triangles<1>,
    &hook_use_triangles<2>,
    &hook_use_triangles<3>,
};

}  // namespace

void terrain_report() {
    int loaded = 0;
    for (const int count : g_loaded) loaded += count;
    if (loaded == 0 && g_dropped == 0 && g_physics_loaded == 0) return;
    logf("terrain: loaded physics for %d chunks and triangles for %d, for %s %d, %s %d, %s %d, %s %d, and let go of %d "
         "to make room", g_physics_loaded, loaded, kUseSites[0].name, g_loaded[0], kUseSites[1].name, g_loaded[1],
         kUseSites[2].name, g_loaded[2], kUseSites[3].name, g_loaded[3], g_dropped);
    for (int& count : g_loaded) count = 0;
    g_dropped = 0;
    g_physics_loaded = 0;
}

void terrain_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);

    bool sites = true;
    for (int i = 0; i < kUseSiteCount; ++i) {
        sites = patch_call(g_game + kUseSites[i].at, g_game + kUseTriangles, reinterpret_cast<const void*>(kUseHooks[i])) && sites;
    }
    const bool timed = patch_call(g_game + kUsePhysicsCall, g_game + kUsePhysics, reinterpret_cast<const void*>(&hook_use_physics));
    const bool physics = patch_bytes(g_game + kKeepPhysics, &kGameChunksKept, &kChunksKept, 1) &&
                         patch_bytes(g_game + kPhysicsStale, kGamePhysicsStale, kNeverStale, sizeof(kNeverStale));
    // Without room being made at every site, keeping more would fill the heap.
    const bool triangles = sites && patch_bytes(g_game + kKeepTriangles, &kGameChunksKept, &kChunksKept, 1) &&
                           patch_bytes(g_game + kTrianglesFresh, kGameTrianglesFresh, kAlwaysFresh, sizeof(kAlwaysFresh));
    logf("terrain: chunks kept physics=%d triangles=%d, timed=%d", physics, triangles, timed);
}

int terrain_test() {
    constexpr unsigned kChunks = 3;
    constexpr size_t kChunkSize = kChunkLastUsed + sizeof(unsigned);
    constexpr size_t kHeapSize = kHeapFreeSlots + sizeof(unsigned);
    constexpr unsigned kPieces = 100;

    alignas(4) char memory[kChunks][kChunkSize] = {};
    alignas(4) char heap[kHeapSize] = {};
    char* chunks[kChunks] = {memory[0], memory[1], memory[2]};
    const auto hold = [&](unsigned last_used_0, unsigned last_used_1, unsigned last_used_2) {
        field<unsigned>(chunks[0], kChunkLastUsed) = last_used_0;
        field<unsigned>(chunks[1], kChunkLastUsed) = last_used_1;
        field<unsigned>(chunks[2], kChunkLastUsed) = last_used_2;
        field<uint8_t>(chunks[0], kChunkHeld) = 1;
        field<uint8_t>(chunks[1], kChunkHeld) = 1;
        field<uint8_t>(chunks[2], kChunkHeld) = 0;
        field<unsigned>(heap, kHeapSlots) = 2 * kPieces;
        field<unsigned>(heap, kHeapFreeSlots) = 0;
    };
    const auto drop = [&](char* chunk) {
        field<uint8_t>(chunk, kChunkHeld) = 0;
        field<unsigned>(heap, kHeapFreeSlots) += kPieces;
    };

    hold(10, 20, 0);
    if (make_room(heap, chunks, kChunks, chunks[2], 50, drop) != 0) return 1;  // it fits
    if (!field<uint8_t>(chunks[0], kChunkHeld) || !field<uint8_t>(chunks[1], kChunkHeld)) return 2;

    hold(10, 20, 0);
    if (make_room(heap, chunks, kChunks, chunks[2], kPieces, drop) != 1) return 3;  // only one has to go
    if (field<uint8_t>(chunks[0], kChunkHeld) || !field<uint8_t>(chunks[1], kChunkHeld)) return 4;

    hold(20, 10, 0);
    if (make_room(heap, chunks, kChunks, chunks[2], kPieces, drop) != 1) return 5;  // it's the older one
    if (!field<uint8_t>(chunks[0], kChunkHeld) || field<uint8_t>(chunks[1], kChunkHeld)) return 6;

    hold(10, 20, 0);
    if (make_room(heap, chunks, kChunks, chunks[0], kMostPieces, drop) != 1) return 7;  // never the one wanted
    if (!field<uint8_t>(chunks[0], kChunkHeld) || field<uint8_t>(chunks[1], kChunkHeld)) return 8;

    hold(10, 20, 0);
    if (make_room(heap, chunks, kChunks, chunks[2], 1000, drop) != 2) return 9;  // nothing left to let go of
    if (pieces_held(heap) != 0) return 10;
    return 0;
}

}  // namespace longshot
