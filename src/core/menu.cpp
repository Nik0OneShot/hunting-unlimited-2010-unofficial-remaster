#include "longshot.h"

#include <intrin.h>

#include <cmath>

#pragma intrinsic(_ReturnAddress)

namespace longshot {

namespace {

// Offsets in game.dll, valid only for the verified build.
constexpr uint32_t kIntroUpdateBegin = 0x93f0;
constexpr uint32_t kIntroUpdateEnd = 0x9634;
constexpr uint32_t kIntroFirstLogoGuard = 0x9440;  // jbe that refuses to skip the first logo
constexpr uint32_t kSlotTriggerByName = 0x2d0948;  // bound pointer to _inp_get_trigger_name
constexpr uint32_t kListVtable = 0x1be468;         // ui.comp.common.list

// ui_view_t virtual methods, as offsets into the vtable.
constexpr size_t kMethodHandler = 0x54;
constexpr size_t kMethodAction = 0x7c;
constexpr size_t kMethodOffset = 0x80;

// List fields.
constexpr size_t kListColumns = 0xfc;
constexpr size_t kListRows = 0x100;       // vptrlist_t of c_item_t*
constexpr size_t kListFirstItem = 0x114;  // index of the item in the first slot
constexpr size_t kListItemHeight = 0x144;
constexpr size_t kListItemGap = 0x14c;

constexpr size_t kVptrlistHead = 4;
constexpr size_t kVptrnodeNext = 4;
constexpr size_t kVptrnodeData = 8;
constexpr size_t kItemUnit = 0x10;  // c_item_t's pointer to the object it wraps

// The wheel arrives as these UI actions, one for each direction.
constexpr int kActionWheelFirst = 0x12;
constexpr int kActionWheelLast = 0x13;

constexpr int kMoveDrawnAndHitAreas = 3;  // the flags move_to passes to offset

constexpr size_t kTriggerSize = 0x208;
constexpr size_t kTriggerValue = 0x34;

// A glide this close to home is under a quarter of a layout unit, so it can snap there.
constexpr float kGlideSnap = 0.25f;
constexpr double kMaxGlideStep = 0.05;
constexpr int kMaxGlides = 4;

using TriggerByNameFn = void*(__cdecl*)(const char*);
using ListHandlerFn = void(__thiscall*)(void*);
using ListActionFn = bool(__thiscall*)(void*, void*, int, void*);
using ViewOffsetFn = void(__thiscall*)(void*, float, float, int);

// One list whose rows are currently shifted away from their real position.
struct Glide {
    void* list = nullptr;
    void* first_row = nullptr;
    float shown = 0.0f;
    double last_step = 0.0;
};

char* g_game = nullptr;
bool g_verified = false;
bool g_skip_intro = false;
TriggerByNameFn g_real_trigger_by_name = nullptr;
ListHandlerFn g_real_list_handler = nullptr;
ListActionFn g_real_list_action = nullptr;
char g_pressed_trigger[kTriggerSize] = {};
Glide g_glides[kMaxGlides];

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

// ---- Intro --------------------------------------------------------------------------------

bool any_input_down() {
    if (!game_has_focus()) return false;
    for (int key = 1; key < 256; ++key) {
        if (GetAsyncKeyState(key) & 0x8000) return true;
    }
    return false;
}

// The intro only ever asks for the esc trigger. Answering "held" every frame once something has
// been pressed runs through all four screens in a few frames, however short the press was.
void* __cdecl hook_trigger_by_name(const char* name) {
    const auto offset =
        reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_game);
    if (offset >= kIntroUpdateBegin && offset < kIntroUpdateEnd) {
        if (!g_skip_intro && any_input_down()) {
            g_skip_intro = true;
            logf("menu: skipping the intro");
        }
        if (g_skip_intro) return g_pressed_trigger;
    }
    return g_real_trigger_by_name(name);
}

// ---- Smooth list scrolling ----------------------------------------------------------------
// A list has a fixed set of row views, and scrolling only changes what they show. So after each
// scroll the rows are shifted to where the content appeared to be, then eased back to their real
// positions. Row positions are only set when rows are created, so the shift survives refills.

void* first_row_of(void* list) { return field<void*>(list, kListRows + kVptrlistHead); }

void shift_rows(void* list, float dy) {
    for (void* node = first_row_of(list); node; node = field<void*>(node, kVptrnodeNext)) {
        void* item = field<void*>(node, kVptrnodeData);
        void* view = item ? field<void*>(item, kItemUnit) : nullptr;
        if (!view) continue;
        void** vtable = field<void**>(view, 0);
        auto offset = reinterpret_cast<ViewOffsetFn>(vtable[kMethodOffset / sizeof(void*)]);
        offset(view, 0.0f, dy, kMoveDrawnAndHitAreas);
    }
}

Glide* find_glide(void* list) {
    for (Glide& glide : g_glides) {
        if (glide.list == list) return &glide;
    }
    return nullptr;
}

// A list destroyed mid-glide leaves a stale entry, which only ever holds a pointer. Comparing the
// first row catches a new list that lands on the same address, whose rows were never shifted.
Glide* claim_glide(void* list) {
    Glide* glide = find_glide(list);
    if (glide && glide->first_row == first_row_of(list)) return glide;
    if (!glide) glide = find_glide(nullptr);

    if (!glide) {
        glide = &g_glides[0];
        for (Glide& other : g_glides) {
            if (other.last_step < glide->last_step) glide = &other;
        }
    }
    *glide = Glide{list, first_row_of(list), 0.0f, now_seconds()};
    return glide;
}

void start_glide(void* list, int first_before) {
    const int first_after = field<int>(list, kListFirstItem);
    const int columns = field<int>(list, kListColumns);
    const float pitch = field<float>(list, kListItemHeight) + field<float>(list, kListItemGap);
    if (first_after == first_before || first_before < 0 || first_after < 0) return;
    if (columns <= 0 || pitch <= 0.0f) return;

    // Layout coordinates run upward, so scrolling down starts the rows one step lower.
    const float rows = static_cast<float>(first_after - first_before) / columns;
    Glide* glide = claim_glide(list);
    const float wanted = menu_glide_add(glide->shown, -rows * pitch, pitch);
    shift_rows(list, wanted - glide->shown);
    glide->shown = wanted;
    glide->last_step = now_seconds();
}

void step_glide(void* list) {
    Glide* glide = find_glide(list);
    if (!glide) return;
    if (glide->first_row != first_row_of(list)) {
        *glide = Glide{};
        return;
    }

    const double now = now_seconds();
    double seconds = now - glide->last_step;
    if (seconds > kMaxGlideStep) seconds = kMaxGlideStep;
    glide->last_step = now;

    const float next = menu_glide_step(glide->shown, static_cast<float>(seconds),
                                       g_cfg.menu_scroll_smoothing_ms / 1000.0f);
    shift_rows(list, next - glide->shown);
    glide->shown = next;
    if (next == 0.0f) *glide = Glide{};
}

void __fastcall hook_list_handler(void* list, void*) {
    g_real_list_handler(list);
    step_glide(list);
}

bool __fastcall hook_list_action(void* list, void*, void* source, int type, void* param) {
    const int first_before = field<int>(list, kListFirstItem);
    const bool handled = g_real_list_action(list, source, type, param);
    if (type >= kActionWheelFirst && type <= kActionWheelLast) start_glide(list, first_before);
    return handled;
}

void install_list_hooks() {
    if (g_cfg.menu_scroll_smoothing_ms <= 0.0f) {
        logf("menu: smooth scrolling off");
        return;
    }
    auto** table = reinterpret_cast<void**>(g_game + kListVtable);
    const bool handler = vtable_patch(&table[kMethodHandler / sizeof(void*)],
                                      reinterpret_cast<void*>(&hook_list_handler),
                                      reinterpret_cast<void**>(&g_real_list_handler));
    const bool action = vtable_patch(&table[kMethodAction / sizeof(void*)],
                                     reinterpret_cast<void*>(&hook_list_action),
                                     reinterpret_cast<void**>(&g_real_list_action));
    logf("menu: list hooks handler=%d action=%d", handler, action);
}

void install_intro_skip() {
    auto** slot = reinterpret_cast<void**>(g_game + kSlotTriggerByName);
    if (!*slot) {
        logf("menu: engine API not bound yet, intro skip unavailable");
        return;
    }

    const unsigned char guard[] = {0x76, 0x12};
    const unsigned char removed[] = {0x90, 0x90};
    if (!patch_bytes(g_game + kIntroFirstLogoGuard, guard, removed, sizeof(guard))) {
        logf("menu: first logo guard not found, intro skip unavailable");
        return;
    }

    field<float>(g_pressed_trigger, kTriggerValue) = 1.0f;
    g_real_trigger_by_name = reinterpret_cast<TriggerByNameFn>(*slot);
    *slot = reinterpret_cast<void*>(&hook_trigger_by_name);
    logf("menu: intro skip installed");
}

}  // namespace

float menu_glide_step(float shown, float seconds, float time_constant) {
    const float next = shown * std::exp(-seconds / time_constant);
    return std::fabs(next) < kGlideSnap ? 0.0f : next;
}

float menu_glide_add(float shown, float delta, float limit) {
    const float wanted = shown + delta;
    return wanted > limit ? limit : (wanted < -limit ? -limit : wanted);
}

void menu_attach_game(HMODULE game, bool verified) {
    g_game = reinterpret_cast<char*>(game);
    g_verified = verified;
    g_skip_intro = g_cfg.skip_intro;
}

void menu_game_ready() {
    if (!g_verified) return;
    install_intro_skip();
    install_list_hooks();
}

}  // namespace longshot
