#include "longshot.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace longshot {

namespace {

// Offsets into game.dll. The first three are inside calls made through a pointer.
constexpr uintptr_t kChunkInView = 0xd9105;
constexpr uintptr_t kPlantInView = 0xe24a1;
constexpr uintptr_t kLoadingMessage = 0xd894c;
constexpr uintptr_t kBoxInViewSlot = 0x19a448;
constexpr uintptr_t kMessageSlot = 0x19a510;
constexpr size_t kCameraFrustum = 0x48;
constexpr size_t kCameraPosition = 0x20;

constexpr char kFsOpen[] = "?fs_open@prism@@YA?AVret_t@?$auto_ptr_t@Vfile_t@prism@@@1@ABVstring@1@W4fs_realm_t@1@@Z";
constexpr char kFsFileExists[] = "?fs_file_exists@prism@@YA_NABVstring@1@W4fs_realm_t@1@@Z";
constexpr char kFileSize[] = "?size@file_t@prism@@QAE_KXZ";
constexpr char kFileRead[] = "?read@file_t@prism@@QAE_NQAXI@Z";
constexpr int kGameFiles = 1;  // the realm for files in the game's folders

// A landscape's .lnd starts with how many chunks it has, and each is a .pmg of this layout.
constexpr size_t kLandscapeChunks = 0x4;
constexpr uint32_t kModelMagic = 0x506D6711;
constexpr size_t kModelPieceCount = 0x4;
constexpr size_t kModelBox = 0x24;
constexpr size_t kModelPieces = 0x44;
constexpr size_t kPieceSize = 0x60;
constexpr size_t kPieceIndexCount = 0x0;
constexpr size_t kPieceVertexCount = 0x4;
constexpr size_t kPiecePositions = 0x38;
constexpr size_t kPieceIndices = 0x50;

// Each cell of the height map keeps the lowest surface in it, so that rocks and water can't count
// as cover, and the map can only ever be lower than the real ground. Hiding less is the safe way
// to be wrong: it costs a little speed, where hiding too much would show.
constexpr float kCell = 8.0f;
constexpr float kSample = 4.0f;  // how finely each triangle is read into those cells
constexpr float kUntouched = 1e9f;
constexpr float kNoGround = -1e6f;

// The skyline is how high the ground has risen so far, as a slope, in each direction and at each
// distance out, so asking what's hidden is one look-up. It covers the game's furthest view of 1700 m.
constexpr int kBins = 720;
constexpr float kStep = 8.0f;
constexpr int kSteps = 212;
constexpr float kFullCircle = 6.2831853f;

// Margins, all of which hide less. Ground this close changes too fast as you move to rely on.
constexpr float kFirstOccluder = 24.0f;
constexpr float kNearestHidden = 40.0f;
constexpr float kEyeLift = 1.0f;
constexpr float kTopLift = 3.0f;
constexpr float kMoveBeforeRebuild = 1.0f;

// A chunk's box is only its terrain, and its trees stand on and over that. The tallest is a 44.5 m
// poplar at up to 1.3 times its size, about 18 m wide.
constexpr float kChunkTaller = 60.0f;
constexpr float kChunkWider = 15.0f;

using BoxInViewFn = bool(__thiscall*)(const void*, const float*);
using MessageFn = void(__cdecl*)(const char*, ...);
using FsOpenFn = void**(__cdecl*)(void**, const char* const*, int);
using FsFileExistsFn = bool(__cdecl*)(const char* const*, int);
using FileSizeFn = uint64_t(__thiscall*)(void*);
using FileReadFn = bool(__thiscall*)(void*, void*, unsigned int);
using FileDeleteFn = void(__thiscall*)(void*, int);

struct Ground {
    float left = 0.0f;
    float back = 0.0f;
    int width = 0;
    int depth = 0;
    std::vector<float> lowest;
};

char* g_game = nullptr;
BoxInViewFn g_box_in_view = nullptr;
MessageFn g_message = nullptr;
FsOpenFn g_fs_open = nullptr;
FsFileExistsFn g_fs_file_exists = nullptr;
FileSizeFn g_file_size = nullptr;
FileReadFn g_file_read = nullptr;

std::string g_landscape;
bool g_wanted = false;
bool g_ready = false;
Ground g_ground;
std::vector<float> g_skyline;
std::vector<float> g_sines;
std::vector<float> g_cosines;
float g_eye[3] = {};
bool g_have_skyline = false;

int g_chunks_seen = 0;
int g_chunks_hidden = 0;
int g_plants_seen = 0;
int g_plants_hidden = 0;

template <typename T>
bool read_value(const std::string& data, size_t at, T* out) {
    if (at + sizeof(T) > data.size()) return false;
    memcpy(out, data.data() + at, sizeof(T));
    return true;
}

bool is_model(const std::string& model) {
    uint32_t magic = 0;
    return read_value(model, 0, &magic) && magic == kModelMagic;
}

void add_triangle(const float* a, const float* b, const float* c) {
    const float across = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (fabsf(across) < 1e-6f) return;
    Ground& ground = g_ground;
    const float low_x = fminf(a[0], fminf(b[0], c[0]));
    const float high_x = fmaxf(a[0], fmaxf(b[0], c[0]));
    const float low_z = fminf(a[2], fminf(b[2], c[2]));
    const float high_z = fmaxf(a[2], fmaxf(b[2], c[2]));
    const int per_cell = static_cast<int>(kCell / kSample);
    const int first_x = static_cast<int>(floorf((low_x - ground.left) / kSample));
    const int last_x = static_cast<int>(ceilf((high_x - ground.left) / kSample));
    const int first_z = static_cast<int>(floorf((low_z - ground.back) / kSample));
    const int last_z = static_cast<int>(ceilf((high_z - ground.back) / kSample));
    for (int z = first_z; z <= last_z; ++z) {
        const int cell_z = z / per_cell;
        if (z < 0 || cell_z >= ground.depth) continue;
        const float at_z = ground.back + z * kSample;
        for (int x = first_x; x <= last_x; ++x) {
            const int cell_x = x / per_cell;
            if (x < 0 || cell_x >= ground.width) continue;
            const float at_x = ground.left + x * kSample;
            const float share_a = ((b[2] - c[2]) * (at_x - c[0]) + (c[0] - b[0]) * (at_z - c[2])) / across;
            const float share_b = ((c[2] - a[2]) * (at_x - c[0]) + (a[0] - c[0]) * (at_z - c[2])) / across;
            const float share_c = 1.0f - share_a - share_b;
            if (share_a < -0.01f || share_b < -0.01f || share_c < -0.01f) continue;
            const float height = share_a * a[1] + share_b * b[1] + share_c * c[1];
            float& lowest = ground.lowest[static_cast<size_t>(cell_z) * ground.width + cell_x];
            if (height < lowest) lowest = height;
        }
    }
}

// Says how many triangles it read, and anything that doesn't add up is left out.
unsigned add_model(const std::string& model) {
    uint32_t pieces = 0;
    uint32_t table = 0;
    if (!is_model(model) || !read_value(model, kModelPieceCount, &pieces) || !read_value(model, kModelPieces, &table)) return 0;
    unsigned triangles = 0;
    for (uint32_t piece = 0; piece < pieces; ++piece) {
        const size_t record = table + static_cast<size_t>(piece) * kPieceSize;
        uint32_t index_count = 0, vertex_count = 0, positions = 0, indices = 0;
        if (!read_value(model, record + kPieceIndexCount, &index_count) ||
            !read_value(model, record + kPieceVertexCount, &vertex_count) ||
            !read_value(model, record + kPiecePositions, &positions) ||
            !read_value(model, record + kPieceIndices, &indices)) {
            return triangles;
        }
        if (positions + static_cast<size_t>(vertex_count) * 12 > model.size()) continue;
        if (indices + static_cast<size_t>(index_count) * 2 > model.size()) continue;
        const char* position_data = model.data() + positions;
        const char* index_data = model.data() + indices;
        for (uint32_t i = 0; i + 3 <= index_count; i += 3) {
            uint16_t corner[3];
            memcpy(corner, index_data + i * 2, sizeof(corner));
            if (corner[0] >= vertex_count || corner[1] >= vertex_count || corner[2] >= vertex_count) continue;
            float point[3][3];
            for (int k = 0; k < 3; ++k) memcpy(point[k], position_data + corner[k] * 12, 12);
            add_triangle(point[0], point[1], point[2]);
            ++triangles;
        }
    }
    return triangles;
}

void start_ground(float left, float back, float right, float front) {
    g_ground.left = left;
    g_ground.back = back;
    g_ground.width = static_cast<int>((right - left) / kCell) + 2;
    g_ground.depth = static_cast<int>((front - back) / kCell) + 2;
    g_ground.lowest.assign(static_cast<size_t>(g_ground.width) * g_ground.depth, kUntouched);
}

void finish_ground() {
    for (float& cell : g_ground.lowest) {
        if (cell == kUntouched) cell = kNoGround;  // nothing there can hide anything
    }
    g_have_skyline = false;
}

float ground_at(float x, float z) {
    const float cell_x = (x - g_ground.left) / kCell;
    const float cell_z = (z - g_ground.back) / kCell;
    if (cell_x < 0.0f || cell_z < 0.0f) return kNoGround;
    const int column = static_cast<int>(cell_x);
    const int row = static_cast<int>(cell_z);
    if (column >= g_ground.width || row >= g_ground.depth) return kNoGround;
    return g_ground.lowest[static_cast<size_t>(row) * g_ground.width + column];
}

void build_skyline(const float* eye) {
    if (g_skyline.empty()) {
        g_skyline.resize(static_cast<size_t>(kBins) * kSteps);
        g_sines.resize(kBins);
        g_cosines.resize(kBins);
        for (int bin = 0; bin < kBins; ++bin) {
            const float angle = (bin + 0.5f) * kFullCircle / kBins;
            g_sines[bin] = sinf(angle);
            g_cosines[bin] = cosf(angle);
        }
    }
    const float height = eye[1] + kEyeLift;
    float* out = g_skyline.data();
    for (int bin = 0; bin < kBins; ++bin) {
        const float along_x = g_sines[bin];
        const float along_z = -g_cosines[bin];
        float highest = -1e9f;
        for (int step = 1; step <= kSteps; ++step) {
            const float reach = step * kStep;
            if (reach >= kFirstOccluder) {
                const float slope = (ground_at(eye[0] + along_x * reach, eye[2] + along_z * reach) - height) / reach;
                if (slope > highest) highest = slope;
            }
            *out++ = highest;
        }
    }
    memcpy(g_eye, eye, sizeof(g_eye));
    g_have_skyline = true;
}

// A box is {min x, y, z, max x, y, z}. It's hidden if the ground short of it rises above its top in
// every direction it covers, seen from where the skyline was built.
bool box_hidden(const float* box, float wider, float taller) {
    const float half_x = (box[3] - box[0]) * 0.5f + wider;
    const float half_z = (box[5] - box[2]) * 0.5f + wider;
    const float reach = sqrtf(half_x * half_x + half_z * half_z);
    const float to_x = (box[0] + box[3]) * 0.5f - g_eye[0];
    const float to_z = (box[2] + box[5]) * 0.5f - g_eye[2];
    const float away = sqrtf(to_x * to_x + to_z * to_z);
    const float nearest = away - reach;
    if (nearest < kNearestHidden) return false;
    int step = static_cast<int>(nearest / kStep) - 1;
    if (step >= kSteps) step = kSteps - 1;
    // The top of a box above the eye looks highest from its near side, and one below from its far side.
    const float rise = box[4] + taller + kTopLift - (g_eye[1] + kEyeLift);
    const float slope = rise / (rise > 0.0f ? nearest : away + reach);
    float angle = atan2f(to_x, -to_z);
    if (angle < 0.0f) angle += kFullCircle;
    const float spread = asinf(reach / away);
    const float bins_per_radian = kBins / kFullCircle;
    const int first = static_cast<int>(floorf((angle - spread) * bins_per_radian));
    const int last = static_cast<int>(floorf((angle + spread) * bins_per_radian));
    for (int bin = first; bin <= last; ++bin) {
        const int wrapped = ((bin % kBins) + kBins) % kBins;
        if (g_skyline[static_cast<size_t>(wrapped) * kSteps + step] <= slope) return false;
    }
    return true;
}

bool read_file(const std::string& path, std::string* out) {
    const char* text = path.c_str();
    void* file = nullptr;
    g_fs_open(&file, &text, kGameFiles);
    if (!file) return false;
    out->resize(static_cast<size_t>(g_file_size(file)));
    const bool ok = out->empty() || g_file_read(file, &(*out)[0], static_cast<unsigned int>(out->size()));
    reinterpret_cast<FileDeleteFn>((*static_cast<void***>(file))[0])(file, 1);
    return ok;
}

void load_ground() {
    g_wanted = false;
    const double started = now_seconds();
    std::string landscape;
    uint32_t chunks = 0;
    const size_t name_at = g_landscape.rfind('/');
    if (name_at == std::string::npos || !read_file(g_landscape, &landscape) ||
        !read_value(landscape, kLandscapeChunks, &chunks)) {
        logf("occlusion: could not read %s", g_landscape.c_str());
        return;
    }
    const std::string folder = g_landscape.substr(0, name_at + 1);
    std::vector<std::string> models;
    float left = 1e9f, back = 1e9f, right = -1e9f, front = -1e9f;
    for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
        char name[32];
        snprintf(name, sizeof(name), "landscape_%04u.pmg", chunk);
        const std::string path = folder + name;
        const char* text = path.c_str();
        if (!g_fs_file_exists(&text, kGameFiles)) continue;  // some maps skip a number
        std::string model;
        float box[6];
        if (!read_file(path, &model) || !is_model(model) || !read_value(model, kModelBox, &box)) continue;
        left = fminf(left, box[0]);
        back = fminf(back, box[2]);
        right = fmaxf(right, box[3]);
        front = fmaxf(front, box[5]);
        models.push_back(std::move(model));
    }
    if (models.empty()) {
        logf("occlusion: %s has no terrain to read", g_landscape.c_str());
        return;
    }
    start_ground(left, back, right, front);
    unsigned triangles = 0;
    for (const std::string& model : models) triangles += add_model(model);
    finish_ground();
    g_ready = true;
    logf("occlusion: read %u chunks of %s, %u triangles, in %.0f ms", static_cast<unsigned>(models.size()), folder.c_str(),
         triangles, (now_seconds() - started) * 1000.0);
}

bool hidden_now(const char* frustum, const float* box, float wider, float taller) {
    if (!g_cfg.occlusion_culling) return false;
    if (g_wanted) load_ground();
    if (!g_ready) return false;
    const float* eye = reinterpret_cast<const float*>(frustum - kCameraFrustum + kCameraPosition);
    const float moved_x = eye[0] - g_eye[0];
    const float moved_z = eye[2] - g_eye[2];
    const bool moved = moved_x * moved_x + moved_z * moved_z > kMoveBeforeRebuild * kMoveBeforeRebuild ||
                       fabsf(eye[1] - g_eye[1]) > kMoveBeforeRebuild;
    if (!g_have_skyline || moved) {
        // A second camera in a frame gets no culling, rather than the time for another skyline.
        if (profile_frame_seconds(ProfileSection::Occlusion) > 0.0) return false;
        ProfileTimer timer(ProfileSection::Occlusion);
        build_skyline(eye);
    }
    return box_hidden(box, wider, taller);
}

bool __fastcall hook_chunk_in_view(const char* frustum, void*, const float* box) {
    if (!g_box_in_view(frustum, box)) return false;
    ++g_chunks_seen;
    if (!hidden_now(frustum, box, kChunkWider, kChunkTaller)) return true;
    ++g_chunks_hidden;
    return false;
}

bool __fastcall hook_plant_in_view(const char* frustum, void*, const float* box) {
    if (!g_box_in_view(frustum, box)) return false;
    ++g_plants_seen;
    if (!hidden_now(frustum, box, 0.0f, 0.0f)) return true;
    ++g_plants_hidden;
    return false;
}

void __cdecl hook_loading_message(const char* format, const char* path) {
    g_message(format, path);
    g_landscape = path;
    g_wanted = true;
    g_ready = false;
}

void* g_chunk_call = reinterpret_cast<void*>(&hook_chunk_in_view);
void* g_plant_call = reinterpret_cast<void*>(&hook_plant_in_view);
void* g_message_call = reinterpret_cast<void*>(&hook_loading_message);

bool point_call(uintptr_t at, uintptr_t game_slot, void** ours) {
    const void* game = g_game + game_slot;
    return patch_bytes(g_game + at, &game, &ours, sizeof(ours));
}

}  // namespace

void occlusion_report() {
    if (g_chunks_seen == 0 && g_plants_seen == 0) return;
    logf("occlusion: hid %d of %d chunks in view and %d of %d plants, %.1f%% and %.1f%%", g_chunks_hidden, g_chunks_seen,
         g_plants_hidden, g_plants_seen, g_chunks_seen ? 100.0 * g_chunks_hidden / g_chunks_seen : 0.0,
         g_plants_seen ? 100.0 * g_plants_hidden / g_plants_seen : 0.0);
    g_chunks_seen = g_chunks_hidden = g_plants_seen = g_plants_hidden = 0;
}

void occlusion_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    HMODULE core = GetModuleHandleW(L"p3core.dll");
    g_fs_open = reinterpret_cast<FsOpenFn>(GetProcAddress(core, kFsOpen));
    g_fs_file_exists = reinterpret_cast<FsFileExistsFn>(GetProcAddress(core, kFsFileExists));
    g_file_size = reinterpret_cast<FileSizeFn>(GetProcAddress(core, kFileSize));
    g_file_read = reinterpret_cast<FileReadFn>(GetProcAddress(core, kFileRead));
    if (!g_fs_open || !g_fs_file_exists || !g_file_size || !g_file_read) {
        logf("occlusion: engine file functions not found, nothing is hidden");
        return;
    }
    g_box_in_view = *reinterpret_cast<BoxInViewFn*>(g_game + kBoxInViewSlot);
    g_message = *reinterpret_cast<MessageFn*>(g_game + kMessageSlot);
    const bool map = point_call(kLoadingMessage, kMessageSlot, &g_message_call);
    const bool chunks = map && point_call(kChunkInView, kBoxInViewSlot, &g_chunk_call);
    const bool plants = map && point_call(kPlantInView, kBoxInViewSlot, &g_plant_call);
    logf("occlusion: map=%d chunks=%d plants=%d", map, chunks, plants);
}

int occlusion_test() {
    // A patch of flat ground, with a ridge 40 m high and 100 m wide, 120 m to the north.
    constexpr int kAcross = 6;
    constexpr int kAlong = 6;
    constexpr int kCorners = kAcross * kAlong;
    constexpr int kIndices = (kAcross - 1) * (kAlong - 1) * 6;
    constexpr float kXs[kAcross] = {-1000.0f, -60.0f, -50.0f, 50.0f, 60.0f, 1000.0f};
    constexpr float kZs[kAlong] = {-1000.0f, -140.0f, -130.0f, -110.0f, -100.0f, 1000.0f};
    constexpr uint32_t kTable = 0x8c;
    constexpr uint32_t kPositionsAt = kTable + kPieceSize;
    constexpr uint32_t kIndicesAt = kPositionsAt + kCorners * 12;

    const auto make_model = [&](float ridge) {
        std::string model(kIndicesAt + kIndices * 2, '\0');
        const auto put = [&](size_t at, const void* value, size_t size) { memcpy(&model[at], value, size); };
        const uint32_t magic = kModelMagic, pieces = 1, index_count = kIndices, vertex_count = kCorners;
        put(0, &magic, 4);
        put(kModelPieceCount, &pieces, 4);
        put(kModelPieces, &kTable, 4);
        put(kTable + kPieceIndexCount, &index_count, 4);
        put(kTable + kPieceVertexCount, &vertex_count, 4);
        put(kTable + kPiecePositions, &kPositionsAt, 4);
        put(kTable + kPieceIndices, &kIndicesAt, 4);
        for (int z = 0; z < kAlong; ++z) {
            for (int x = 0; x < kAcross; ++x) {
                const bool on_ridge = (x == 2 || x == 3) && (z == 2 || z == 3);
                const float point[3] = {kXs[x], on_ridge ? ridge : 0.0f, kZs[z]};
                put(kPositionsAt + (z * kAcross + x) * 12, point, 12);
            }
        }
        size_t at = kIndicesAt;
        for (int z = 0; z + 1 < kAlong; ++z) {
            for (int x = 0; x + 1 < kAcross; ++x) {
                const uint16_t a = static_cast<uint16_t>(z * kAcross + x), b = static_cast<uint16_t>(a + 1);
                const uint16_t c = static_cast<uint16_t>(a + kAcross), d = static_cast<uint16_t>(c + 1);
                const uint16_t quad[6] = {a, b, c, b, d, c};
                put(at, quad, sizeof(quad));
                at += sizeof(quad);
            }
        }
        return model;
    };
    const auto box = [](float x, float z, float half, float top, float* out) {
        out[0] = x - half, out[1] = 0.0f, out[2] = z - half;
        out[3] = x + half, out[4] = top, out[5] = z + half;
        return out;
    };
    const float eye[3] = {0.0f, 1.7f, 0.0f};
    float b[6];

    int failed = 0;
    start_ground(-1000.0f, -1000.0f, 1000.0f, 1000.0f);
    if (add_model(make_model(40.0f)) != kIndices / 3) failed = 1;
    finish_ground();
    build_skyline(eye);
    if (!failed && !box_hidden(box(0.0f, -300.0f, 5.0f, 20.0f, b), 0.0f, 0.0f)) failed = 2;     // behind the ridge
    if (!failed && box_hidden(box(0.0f, -300.0f, 5.0f, 150.0f, b), 0.0f, 0.0f)) failed = 3;     // taller than it
    if (!failed && box_hidden(box(0.0f, -80.0f, 2.0f, 5.0f, b), 0.0f, 0.0f)) failed = 4;        // in front of it
    if (!failed && box_hidden(box(0.0f, 300.0f, 5.0f, 20.0f, b), 0.0f, 0.0f)) failed = 5;       // behind the eye
    if (!failed && box_hidden(box(250.0f, -300.0f, 5.0f, 20.0f, b), 0.0f, 0.0f)) failed = 6;    // past its end
    if (!failed && box_hidden(box(0.0f, -300.0f, 200.0f, 20.0f, b), 0.0f, 0.0f)) failed = 7;    // wider than it
    if (!failed && box_hidden(box(0.0f, -300.0f, 5.0f, 20.0f, b), 0.0f, 1000.0f)) failed = 8;   // told it's taller
    if (!failed && box_hidden(box(0.0f, -20.0f, 2.0f, 1.0f, b), 0.0f, 0.0f)) failed = 9;        // too close to hide

    // A lower surface under the same ridge takes it away, and that's the safe way round.
    if (!failed) {
        add_model(make_model(-5.0f));
        finish_ground();
        build_skyline(eye);
        if (box_hidden(box(0.0f, -300.0f, 5.0f, 20.0f, b), 0.0f, 0.0f)) failed = 10;
    }
    // Past the edge of the terrain, nothing can hide anything.
    if (!failed) {
        start_ground(-50.0f, -50.0f, 50.0f, 50.0f);
        finish_ground();
        build_skyline(eye);
        if (box_hidden(box(0.0f, -300.0f, 5.0f, 2.0f, b), 0.0f, 0.0f)) failed = 11;
    }
    // A model that doesn't add up is left out.
    if (!failed) {
        std::string broken = make_model(40.0f);
        broken[0] = 0;
        if (add_model(broken) != 0) failed = 12;
        broken = make_model(40.0f);
        const uint32_t past_the_end = static_cast<uint32_t>(broken.size());
        memcpy(&broken[kTable + kPieceIndices], &past_the_end, 4);
        if (add_model(broken) != 0) failed = 13;
        if (add_model(std::string()) != 0) failed = 14;
    }
    g_ground = Ground();
    g_have_skyline = false;
    return failed;
}

}  // namespace longshot
