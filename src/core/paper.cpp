#include "longshot.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kGameState = 0x1e9d68;
constexpr uintptr_t kProfileSection = 0xd07e0;
constexpr uintptr_t kMapInitSlot = 0x1a26c0;   // the map item's method that reads its definition
constexpr uintptr_t kMapStageSlot = 0x1a26e8;  // and the one told as it's raised and lowered
constexpr uintptr_t kSolverStart = 0xf5880;
constexpr uintptr_t kSolve = 0xf58d0;
constexpr uintptr_t kSolverEnd = 0xf58c0;
constexpr uintptr_t kGetValue = 0x19aaa8;
constexpr uintptr_t kGetBool = 0x19aa74;
constexpr uintptr_t kGetInt = 0x19ab10;
constexpr uintptr_t kFindInPool = 0x19ab0c;
constexpr uintptr_t kTranslate = 0x19ab04;
constexpr size_t kMethodLower = 0x38;
constexpr size_t kSolverBallistic = 0x4;
constexpr size_t kSolverMuzzle = 0x8;
constexpr size_t kSolverWeight = 0xc;
constexpr size_t kSolverElevation = 0x10;
constexpr size_t kSolverSightHeight = 0x18;
constexpr size_t kSolverSize = 0x30;

constexpr int kStageRaised = 1;
constexpr float kSightHeight = 1.5f;  // inches above the bore, unless a gun sets zero_sights
constexpr float kMostMoa = 600.0f;    // the game stops sights at 10 degrees
constexpr int kStepYards = 25;
constexpr int kRows = 40;
constexpr double kSecondsPerFrame = 0.0015;  // a table is filled in over a few frames, not all at once
constexpr unsigned kMostGuns = 8;
constexpr int kLeastZero = 5;  // the game keeps a gun's zero range between these
constexpr int kMostZero = 300;

// Both items are the map as far as the game goes, down to the model in your hands, except that its
// window stays shut and Longshot draws over the view instead. The paper key says which one an item is.
constexpr char kPaperKey[] = "paper";
constexpr char kCalculatorName[] = "calculator";
constexpr char kSheetName[] = "range_sheet";
constexpr char kNewItems[] =
    "\r\ndef calculator:eqitem_base {\r\n"
    "\tpaper:\"calculator\"\r\n"
    "\tshort:\"Calculator\"\r\n"
    "\tlong:\"Calculator\"\r\n"
    "\tdesc:\"A pocket calculator, for working out the sights for a range between two rows of your Range Sheet.\"\r\n"
    "\tscript:\"/script/item/map.sii\"\r\n"
    "\tpba:\"/models/equipment/map/equip.pba\"\r\n"
    "\tcamo_look:\"true\"\r\n"
    "\tarea_variant:\"true\"\r\n"
    "\ticon:\"/material/ui/icon/i_map.mat\"\r\n"
    "\tglow:\"/material/ui/icon/i_map_glow.mat\"\r\n"
    "\tuse_sound:\"/sounds/items/eq_map.ogg\"\r\n"
    "\tcategory:\"misc\"\r\n"
    "}\r\n"
    "def range_sheet:eqitem_base {\r\n"
    "\tpaper:\"range_sheet\"\r\n"
    "\tshort:\"Range Sheet\"\r\n"
    "\tlong:\"Range Sheet\"\r\n"
    "\tdesc:\"A table of ranges and the sight elevation in MOA for each, with a page for every gun you carry.\"\r\n"
    "\tscript:\"/script/item/map.sii\"\r\n"
    "\tpba:\"/models/equipment/map/equip.pba\"\r\n"
    "\tcamo_look:\"true\"\r\n"
    "\tarea_variant:\"true\"\r\n"
    "\ticon:\"/material/ui/icon/i_map.mat\"\r\n"
    "\tglow:\"/material/ui/icon/i_map_glow.mat\"\r\n"
    "\tuse_sound:\"/sounds/items/eq_map.ogg\"\r\n"
    "\tcategory:\"misc\"\r\n"
    "}\r\n";

enum class Paper { None, Calculator, Sheet };

struct Gun {
    std::string name;
    bool fixed_sights = false;
    int zero_yards = 0;
    float zero_moa = 0.0f;
    float ballistic = 0.0f;
    float muzzle = 0.0f;
    float weight = 0.0f;
    float sight_height = 0.0f;
    float moa[kRows] = {};
    int rows_done = 0;
    bool zero_done = false;
};

using MapInitFn = bool(__fastcall*)(void*, void*, void*);
using MapStageFn = void(__fastcall*)(void*, void*, int, bool);
using LowerFn = void(__thiscall*)(void*);
using SectionFn = void*(__fastcall*)(void*, void*, const char* const*);
using GetValueFn = const char*(__fastcall*)(void*, void*, const char*, unsigned);
using GetBoolFn = bool(__fastcall*)(void*, void*, const char*, unsigned);
using GetIntFn = int(__fastcall*)(void*, void*, const char*, unsigned);
using FindInPoolFn = char*(__fastcall*)(void*, void*, const char*, unsigned, bool);
using TranslateFn = const char* const*(__cdecl*)(const char* const*);
using SolverFn = void(__thiscall*)(void*);
using SolveFn = void(__thiscall*)(void*, unsigned, unsigned, const float*);

// Just what drawing here needs, none of which the game's own wrappers should see.
struct Gl {
    void(WINAPI* Begin)(unsigned);
    void(WINAPI* End)();
    void(WINAPI* Vertex2f)(float, float);
    void(WINAPI* TexCoord2f)(float, float);
    void(WINAPI* Color4f)(float, float, float, float);
    void(WINAPI* Enable)(unsigned);
    void(WINAPI* Disable)(unsigned);
    void(WINAPI* BlendFunc)(unsigned, unsigned);
    void(WINAPI* MatrixMode)(unsigned);
    void(WINAPI* LoadIdentity)();
    void(WINAPI* PushMatrix)();
    void(WINAPI* PopMatrix)();
    void(WINAPI* Ortho)(double, double, double, double, double, double);
    void(WINAPI* BindTexture)(unsigned, unsigned);
    void(WINAPI* GenTextures)(int, unsigned*);
    void(WINAPI* DeleteTextures)(int, const unsigned*);
    void(WINAPI* TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void(WINAPI* TexParameteri)(unsigned, unsigned, int);
    void(WINAPI* PixelStorei)(unsigned, int);
    void(WINAPI* GetIntegerv)(unsigned, int*);
    void(WINAPI* PushAttrib)(unsigned);
    void(WINAPI* PopAttrib)();
    void(WINAPI* Viewport)(int, int, int, int);
    void(WINAPI* ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void(WINAPI* TexEnvi)(unsigned, unsigned, int);
    void(APIENTRY* ActiveTexture)(unsigned);
    void(APIENTRY* UseProgram)(unsigned);
};

using CurrentContextFn = void*(WINAPI*)();

constexpr unsigned kQuads = 0x0007;
constexpr unsigned kTriangles = 0x0004;
constexpr unsigned kBlend = 0x0BE2;
constexpr unsigned kTexture2d = 0x0DE1;
constexpr unsigned kSrcAlpha = 0x0302;
constexpr unsigned kOneMinusSrcAlpha = 0x0303;
constexpr unsigned kModelview = 0x1700;
constexpr unsigned kProjection = 0x1701;
constexpr unsigned kAlpha = 0x1906;
constexpr unsigned kUnsignedByte = 0x1401;
constexpr unsigned kMinFilter = 0x2801;
constexpr unsigned kMagFilter = 0x2800;
constexpr int kLinear = 0x2601;
constexpr unsigned kUnpackAlignment = 0x0CF5;
constexpr unsigned kAllAttribBits = 0xFFFFFFFF;
constexpr unsigned kCurrentProgram = 0x8B8D;
constexpr unsigned kMostTextureUnits = 0x84E2;
constexpr unsigned kTexture0 = 0x84C0;
constexpr unsigned kTextureMatrix = 0x1702;
constexpr unsigned kTextureEnv = 0x2300;
constexpr unsigned kTextureEnvMode = 0x2200;
constexpr int kModulate = 0x2100;

// Anything of the game's that could get in the way. glPopAttrib puts these back.
constexpr unsigned kStatesOff[] = {
    0x0B71,  // depth test
    0x0B90,  // stencil test
    0x0C11,  // scissor test
    0x0BC0,  // alpha test
    0x0B44,  // cull face
    0x0B60,  // fog
    0x0B50,  // lighting
    0x809E,  // sample alpha to coverage
    0x8620,  // ARB vertex programs
    0x8804,  // ARB fragment programs
    0x0C60,  // texture coordinate generation
    0x0C61,
    0x0C62,
    0x0C63,
};
constexpr unsigned kTextureTargets[] = {kTexture2d, 0x8513, 0x806F};  // 2D, cube map and 3D

constexpr char kFirstGlyph = ' ';
constexpr int kGlyphs = 95;
constexpr int kAtlasColumns = 16;

struct Glyph {
    float left, top, right, bottom;
    float width;
};

struct Font {
    unsigned texture = 0;
    float height = 0.0f;
    Glyph glyphs[kGlyphs] = {};
};

struct Colour {
    float r, g, b, a;
};

struct Box {
    float left, top, right, bottom;
    bool holds(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

constexpr Colour kPaperColour = {0.93f, 0.90f, 0.80f, 1.0f};
constexpr Colour kPaperBand = {0.87f, 0.83f, 0.71f, 1.0f};
constexpr Colour kInk = {0.12f, 0.11f, 0.10f, 1.0f};
constexpr Colour kFaintInk = {0.38f, 0.35f, 0.30f, 1.0f};
constexpr Colour kCaseColour = {0.15f, 0.16f, 0.18f, 1.0f};
constexpr Colour kScreenColour = {0.69f, 0.76f, 0.63f, 1.0f};
constexpr Colour kKeyColour = {0.30f, 0.32f, 0.35f, 1.0f};
constexpr Colour kKeyLit = {0.42f, 0.45f, 0.50f, 1.0f};
constexpr Colour kOpKeyColour = {0.55f, 0.36f, 0.18f, 1.0f};
constexpr Colour kOpKeyLit = {0.72f, 0.48f, 0.25f, 1.0f};
constexpr Colour kKeyInk = {0.95f, 0.95f, 0.93f, 1.0f};
constexpr Colour kShade = {0.0f, 0.0f, 0.0f, 0.62f};
constexpr Colour kCursor = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr Colour kCursorEdge = {0.0f, 0.0f, 0.0f, 1.0f};

// The sizes below are for a screen 1080 tall, and scale with it.
constexpr float kReferenceHeight = 1080.0f;
constexpr float kSheetWidth = 860.0f;
constexpr float kSheetHeight = 1010.0f;
constexpr float kRowHeight = 36.0f;
constexpr float kCaseWidth = 430.0f;
constexpr float kCaseHeight = 640.0f;
constexpr float kKeyGap = 12.0f;
constexpr int kKeyColumns = 4;
constexpr int kKeyRows = 5;
constexpr char kKeys[kKeyRows][kKeyColumns + 1] = {"cb%/", "789*", "456-", "123+", "0.=="};
constexpr char kPutAwayHint[] = "Right click to put it away";

class Calculator {
   public:
    void press(char key);
    const std::string& display() const { return shown_; }

   private:
    void show(double value);
    double entered() const { return entry_.empty() ? value_ : atof(entry_.c_str()); }
    void work_out();

    std::string entry_;
    std::string shown_ = "0";
    double value_ = 0.0;
    char pending_ = 0;
    bool failed_ = false;
};

char* g_game = nullptr;
MapInitFn g_real_map_init = nullptr;
MapStageFn g_real_map_stage = nullptr;
std::map<void*, Paper> g_papers;
Paper g_open = Paper::None;
void* g_held = nullptr;
bool g_clicked = false;
bool g_put_away = false;
Calculator g_calculator;
std::vector<Gun> g_guns;
int g_page = 0;
float g_cursor_x = 0.0f;
float g_cursor_y = 0.0f;
bool g_keys_down[256] = {};

Gl g_gl = {};
bool g_gl_ready = false;
bool g_gl_failed = false;
Font g_small;
Font g_medium;
Font g_large;
int g_fonts_for_height = 0;
void* g_context = nullptr;

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

template <typename Fn>
Fn engine(uintptr_t slot) {
    return *reinterpret_cast<Fn*>(g_game + slot);
}

void Calculator::show(double value) {
    char text[32];
    snprintf(text, sizeof(text), "%.10g", value);
    shown_ = text;
}

void Calculator::work_out() {
    const double right = entered();
    if (pending_ == '+') value_ += right;
    else if (pending_ == '-') value_ -= right;
    else if (pending_ == '*') value_ *= right;
    else if (pending_ == '/') {
        if (right == 0.0) failed_ = true;
        else value_ /= right;
    } else value_ = right;
}

void Calculator::press(char key) {
    if (key == 'c' || failed_) {
        entry_.clear();
        value_ = 0.0;
        pending_ = 0;
        failed_ = false;
        shown_ = "0";
        if (key == 'c') return;
    }
    if ((key >= '0' && key <= '9') || key == '.') {
        if (key == '.' && entry_.find('.') != std::string::npos) return;
        if (entry_.size() >= 12) return;
        if (key == '.' && entry_.empty()) entry_ = "0";
        entry_ += key;
        shown_ = entry_;
    } else if (key == 'b') {
        if (entry_.empty()) return;
        entry_.pop_back();
        shown_ = entry_.empty() ? "0" : entry_;
    } else if (key == '%') {
        // The share of what's been entered so far, so 300 + 10 % adds 30.
        const double percent = entered() / 100.0;
        entry_.clear();
        char text[32];
        snprintf(text, sizeof(text), "%.10g", pending_ == '+' || pending_ == '-' ? value_ * percent : percent);
        entry_ = text;
        shown_ = entry_;
    } else if (key == '+' || key == '-' || key == '*' || key == '/' || key == '=') {
        if (!entry_.empty() || pending_ == 0) work_out();
        entry_.clear();
        pending_ = key == '=' ? 0 : key;
        if (failed_) shown_ = "Error";
        else show(value_);
    }
}

// The number on a line of a unit file, which starts with its key unless it's been commented out.
float number_after(const std::string& text, const char* key) {
    const size_t length = strlen(key);
    for (size_t line = 0; line < text.size();) {
        const size_t found_end = text.find('\n', line);
        const size_t end = found_end == std::string::npos ? text.size() : found_end;
        const size_t at = text.find_first_not_of(" \t", line);
        if (at < end && text.compare(at, length, key) == 0) {
            const size_t after = text.find_first_not_of(" \t", at + length);
            if (after < end && text[after] == ':') return static_cast<float>(atof(text.c_str() + after + 1));
        }
        line = end + 1;
    }
    return 0.0f;
}

float solve(const Gun& gun, float yards) {
    alignas(8) char solver[kSolverSize];
    const float target[3] = {0.0f, 0.0f, yards};
    float moa = 0.0f;
    __try {
        reinterpret_cast<SolverFn>(g_game + kSolverStart)(solver);
        field<float>(solver, kSolverBallistic) = gun.ballistic;
        field<float>(solver, kSolverMuzzle) = gun.muzzle;
        field<float>(solver, kSolverWeight) = gun.weight;
        field<float>(solver, kSolverSightHeight) = gun.sight_height;
        reinterpret_cast<SolveFn>(g_game + kSolve)(solver, 0, static_cast<unsigned>(-1), target);
        moa = field<float>(solver, kSolverElevation) * 60.0f;
        reinterpret_cast<SolverFn>(g_game + kSolverEnd)(solver);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
    return moa;
}

void load_guns() {
    g_guns.clear();
    g_page = 0;
    char** state = *reinterpret_cast<char***>(g_game + kGameState);
    const char* section_name = "free_hunt";
    void* section = reinterpret_cast<SectionFn>(g_game + kProfileSection)(state[1], nullptr, &section_name);
    if (!section) return;
    for (unsigned slot = 0; slot < kMostGuns; ++slot) {
        const char* value = engine<GetValueFn>(kGetValue)(section, nullptr, "weapon", slot);
        if (!value || !*value) break;
        const std::string choice = value;
        const std::string name = choice.substr(0, choice.find('|'));
        char* def = engine<FindInPoolFn>(kFindInPool)(state[1], nullptr, name.c_str(), static_cast<unsigned>(-1), false);
        if (!def) continue;
        const auto text = [&](const char* of) {
            const char* found = engine<GetValueFn>(kGetValue)(def, nullptr, of, static_cast<unsigned>(-1));
            return std::string(found ? found : "");
        };
        Gun gun;
        const std::string raw_name = text("short");
        const char* raw = raw_name.c_str();
        const char* const* shown = engine<TranslateFn>(kTranslate)(&raw);
        gun.name = shown && *shown ? *shown : name;
        gun.fixed_sights = !engine<GetBoolFn>(kGetBool)(def, nullptr, "adjustable_sights", static_cast<unsigned>(-1));
        const int zero = engine<GetIntFn>(kGetInt)(def, nullptr, "zero_range", static_cast<unsigned>(-1));
        gun.zero_yards = zero < kLeastZero ? kLeastZero : zero > kMostZero ? kMostZero : zero;
        const bool flat = engine<GetBoolFn>(kGetBool)(def, nullptr, "zero_sights", static_cast<unsigned>(-1));
        gun.sight_height = flat ? 0.0f : kSightHeight;
        std::string ammo;
        if (layout_read_file(text("ammo").c_str(), &ammo)) {
            gun.ballistic = number_after(ammo, "bc");
            gun.muzzle = number_after(ammo, "muzzle");
            gun.weight = number_after(ammo, "weight");
        }
        logf("paper: %s, %d yd zero, bc %.3f, %.0f fps, %.0f gr", gun.name.c_str(), gun.zero_yards, gun.ballistic, gun.muzzle, gun.weight);
        g_guns.push_back(gun);
    }
}

void fill_in_rows() {
    const double started = now_seconds();
    const size_t guns = g_guns.size();
    for (size_t i = 0; i < guns; ++i) {
        Gun& gun = g_guns[(g_page + i) % guns];
        if (gun.fixed_sights || gun.muzzle <= 0.0f) continue;
        if (!gun.zero_done) {
            gun.zero_moa = solve(gun, static_cast<float>(gun.zero_yards));
            gun.zero_done = true;
        }
        while (gun.rows_done < kRows) {
            if (now_seconds() - started > kSecondsPerFrame) return;
            gun.moa[gun.rows_done] = solve(gun, static_cast<float>((gun.rows_done + 1) * kStepYards));
            ++gun.rows_done;
        }
    }
}

template <typename Fn>
bool load(Fn* out, const char* name) {
    *out = reinterpret_cast<Fn>(graphics_gl_function(name));
    return *out != nullptr;
}

bool load_gl() {
    if (g_gl_ready) return true;
    if (g_gl_failed) return false;
    g_gl_ready = load(&g_gl.Begin, "glBegin") && load(&g_gl.End, "glEnd") && load(&g_gl.Vertex2f, "glVertex2f") &&
                 load(&g_gl.TexCoord2f, "glTexCoord2f") && load(&g_gl.Color4f, "glColor4f") &&
                 load(&g_gl.Enable, "glEnable") && load(&g_gl.Disable, "glDisable") &&
                 load(&g_gl.BlendFunc, "glBlendFunc") && load(&g_gl.MatrixMode, "glMatrixMode") &&
                 load(&g_gl.LoadIdentity, "glLoadIdentity") && load(&g_gl.PushMatrix, "glPushMatrix") &&
                 load(&g_gl.PopMatrix, "glPopMatrix") && load(&g_gl.Ortho, "glOrtho") &&
                 load(&g_gl.BindTexture, "glBindTexture") && load(&g_gl.GenTextures, "glGenTextures") &&
                 load(&g_gl.DeleteTextures, "glDeleteTextures") && load(&g_gl.TexImage2D, "glTexImage2D") &&
                 load(&g_gl.TexParameteri, "glTexParameteri") && load(&g_gl.PixelStorei, "glPixelStorei") &&
                 load(&g_gl.GetIntegerv, "glGetIntegerv") && load(&g_gl.PushAttrib, "glPushAttrib") &&
                 load(&g_gl.PopAttrib, "glPopAttrib") && load(&g_gl.Viewport, "glViewport") &&
                 load(&g_gl.ColorMask, "glColorMask") && load(&g_gl.TexEnvi, "glTexEnvi") &&
                 load(&g_gl.ActiveTexture, "glActiveTexture") && load(&g_gl.UseProgram, "glUseProgram");
    g_gl_failed = !g_gl_ready;
    if (g_gl_failed) logf("paper: OpenGL functions not found, so nothing can be drawn");
    return g_gl_ready;
}

// Windows draws the letters, so nothing has to be shipped, and each one goes in a texture once.
bool make_font(Font* font, int pixels) {
    if (font->texture) g_gl.DeleteTextures(1, &font->texture);
    font->texture = 0;
    const int cell = pixels + pixels / 2;
    const int rows = kGlyphs / kAtlasColumns + 1;
    const int width = cell * kAtlasColumns;
    const int height = cell * rows;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT gdi_font = CreateFontW(-pixels, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!dc || !bitmap || !gdi_font || !bits) {
        if (gdi_font) DeleteObject(gdi_font);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HGDIOBJ old_font = SelectObject(dc, gdi_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    font->height = static_cast<float>(metrics.tmHeight);
    for (int i = 0; i < kGlyphs; ++i) {
        const wchar_t letter = static_cast<wchar_t>(kFirstGlyph + i);
        const int x = (i % kAtlasColumns) * cell;
        const int y = (i / kAtlasColumns) * cell;
        TextOutW(dc, x, y, &letter, 1);
        SIZE size = {};
        GetTextExtentPoint32W(dc, &letter, 1, &size);
        font->glyphs[i] = {static_cast<float>(x) / width, static_cast<float>(y) / height, static_cast<float>(x + size.cx) / width,
                           static_cast<float>(y + metrics.tmHeight) / height, static_cast<float>(size.cx)};
    }
    GdiFlush();
    std::vector<unsigned char> alpha(static_cast<size_t>(width) * height);
    const auto* pixels_in = static_cast<const unsigned char*>(bits);
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = pixels_in[i * 4 + 1];

    g_gl.GenTextures(1, &font->texture);
    g_gl.BindTexture(kTexture2d, font->texture);
    int alignment = 0;
    g_gl.GetIntegerv(kUnpackAlignment, &alignment);
    g_gl.PixelStorei(kUnpackAlignment, 1);
    g_gl.TexParameteri(kTexture2d, kMinFilter, kLinear);
    g_gl.TexParameteri(kTexture2d, kMagFilter, kLinear);
    g_gl.TexImage2D(kTexture2d, 0, kAlpha, width, height, 0, kAlpha, kUnsignedByte, alpha.data());
    g_gl.PixelStorei(kUnpackAlignment, alignment);

    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(gdi_font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return true;
}

void fill(const Box& box, const Colour& colour) {
    g_gl.Color4f(colour.r, colour.g, colour.b, colour.a);
    g_gl.Begin(kQuads);
    g_gl.Vertex2f(box.left, box.top);
    g_gl.Vertex2f(box.right, box.top);
    g_gl.Vertex2f(box.right, box.bottom);
    g_gl.Vertex2f(box.left, box.bottom);
    g_gl.End();
}

float text_width(const Font& font, const std::string& text) {
    float width = 0.0f;
    for (const char letter : text) {
        const int index = letter - kFirstGlyph;
        if (index >= 0 && index < kGlyphs) width += font.glyphs[index].width;
    }
    return width;
}

void write(const Font& font, const std::string& text, float x, float y, const Colour& colour) {
    g_gl.Enable(kTexture2d);
    g_gl.BindTexture(kTexture2d, font.texture);
    g_gl.Color4f(colour.r, colour.g, colour.b, colour.a);
    g_gl.Begin(kQuads);
    for (const char letter : text) {
        const int index = letter - kFirstGlyph;
        if (index < 0 || index >= kGlyphs) continue;
        const Glyph& glyph = font.glyphs[index];
        g_gl.TexCoord2f(glyph.left, glyph.top);
        g_gl.Vertex2f(x, y);
        g_gl.TexCoord2f(glyph.right, glyph.top);
        g_gl.Vertex2f(x + glyph.width, y);
        g_gl.TexCoord2f(glyph.right, glyph.bottom);
        g_gl.Vertex2f(x + glyph.width, y + font.height);
        g_gl.TexCoord2f(glyph.left, glyph.bottom);
        g_gl.Vertex2f(x, y + font.height);
        x += glyph.width;
    }
    g_gl.End();
    g_gl.Disable(kTexture2d);
}

void write_centred(const Font& font, const std::string& text, const Box& box, const Colour& colour) {
    write(font, text, (box.left + box.right - text_width(font, text)) * 0.5f, (box.top + box.bottom - font.height) * 0.5f, colour);
}

void write_right(const Font& font, const std::string& text, float right, float y, const Colour& colour) {
    write(font, text, right - text_width(font, text), y, colour);
}

Box centred_box(float width, float height, int screen_width, int screen_height) {
    const float scale = screen_height / kReferenceHeight;
    const float left = (screen_width - width * scale) * 0.5f;
    const float top = (screen_height - height * scale) * 0.5f;
    return {left, top, left + width * scale, top + height * scale};
}

Box calculator_key(const Box& body, float scale, int row, int column, int span) {
    const float gap = kKeyGap * scale;
    const float top = body.top + 170.0f * scale;
    const float size_x = (body.right - body.left - gap * (kKeyColumns + 1)) / kKeyColumns;
    const float size_y = (body.bottom - top - gap * kKeyRows) / kKeyRows;
    const float left = body.left + gap + column * (size_x + gap);
    const float y = top + row * (size_y + gap);
    return {left, y, left + size_x * span + gap * (span - 1), y + size_y};
}

Box gun_tab(const Box& sheet, float scale, int gun, int guns) {
    const float gap = 10.0f * scale;
    const float margin = 30.0f * scale;
    const float width = (sheet.right - sheet.left - margin * 2.0f - gap * (guns - 1)) / guns;
    const float left = sheet.left + margin + gun * (width + gap);
    return {left, sheet.bottom - 62.0f * scale, left + width, sheet.bottom - 18.0f * scale};
}

const char* key_label(char key) {
    switch (key) {
        case 'c': return "C";
        case 'b': return "<-";
        case '%': return "%";
        case '/': return "/";
        case '*': return "x";
        case '-': return "-";
        case '+': return "+";
        case '=': return "=";
        case '.': return ".";
        default: return nullptr;
    }
}

void draw_calculator(int width, int height) {
    const float scale = height / kReferenceHeight;
    const Box body = centred_box(kCaseWidth, kCaseHeight, width, height);
    fill(body, kCaseColour);
    const Box screen = {body.left + 20.0f * scale, body.top + 24.0f * scale, body.right - 20.0f * scale, body.top + 130.0f * scale};
    fill(screen, kScreenColour);
    write_right(g_large, g_calculator.display(), screen.right - 14.0f * scale, (screen.top + screen.bottom - g_large.height) * 0.5f, kInk);
    write_centred(g_small, kPutAwayHint, {body.left, screen.bottom, body.right, body.top + 170.0f * scale}, kKeyInk);
    for (int row = 0; row < kKeyRows; ++row) {
        for (int column = 0; column < kKeyColumns; ++column) {
            const char key = kKeys[row][column];
            if (column > 0 && kKeys[row][column - 1] == key) continue;
            const int span = column + 1 < kKeyColumns && kKeys[row][column + 1] == key ? 2 : 1;
            const Box box = calculator_key(body, scale, row, column, span);
            const bool lit = box.holds(g_cursor_x, g_cursor_y);
            const bool op = key == '/' || key == '*' || key == '-' || key == '+' || key == '=';
            fill(box, op ? (lit ? kOpKeyLit : kOpKeyColour) : (lit ? kKeyLit : kKeyColour));
            char digit[2] = {key, 0};
            const char* label = key_label(key);
            write_centred(g_medium, label ? label : digit, box, kKeyInk);
        }
    }
}

void draw_gun_rows(const Gun& gun, const Box& sheet, float scale, float top) {
    const float yards_width = 150.0f * scale;
    const float moa_width = 200.0f * scale;
    const float columns[2] = {sheet.left + 40.0f * scale, sheet.right - 40.0f * scale - yards_width - moa_width};
    for (int side = 0; side < 2; ++side) {
        write_right(g_small, "Yards", columns[side] + yards_width, top, kFaintInk);
        write_right(g_small, "MOA", columns[side] + yards_width + moa_width, top, kFaintInk);
    }
    const float first = top + g_small.height + 6.0f * scale;
    for (int row = 0; row < kRows; ++row) {
        const int side = row < kRows / 2 ? 0 : 1;
        const int line = row % (kRows / 2);
        const float y = first + line * kRowHeight * scale;
        if (line % 2 == 0) {
            fill({columns[side] - 10.0f * scale, y, columns[side] + yards_width + moa_width + 10.0f * scale, y + kRowHeight * scale}, kPaperBand);
        }
        const float text_y = y + (kRowHeight * scale - g_medium.height) * 0.5f;
        char text[32];
        snprintf(text, sizeof(text), "%d", (row + 1) * kStepYards);
        write_right(g_medium, text, columns[side] + yards_width, text_y, kInk);
        if (row >= gun.rows_done) continue;
        const float moa = gun.moa[row];
        if (fabsf(moa) >= kMostMoa) snprintf(text, sizeof(text), "-");
        else snprintf(text, sizeof(text), "%.1f", moa);
        write_right(g_medium, text, columns[side] + yards_width + moa_width, text_y, kInk);
    }
}

void draw_sheet(int width, int height) {
    const float scale = height / kReferenceHeight;
    const Box sheet = centred_box(kSheetWidth, kSheetHeight, width, height);
    fill(sheet, kPaperColour);
    const float margin = sheet.left + 40.0f * scale;
    write(g_large, "RANGE SHEET", margin, sheet.top + 18.0f * scale, kInk);
    write_right(g_small, kPutAwayHint, sheet.right - 40.0f * scale, sheet.top + 34.0f * scale, kFaintInk);
    if (g_guns.empty()) {
        write(g_medium, "No guns in your loadout.", margin, sheet.top + 120.0f * scale, kInk);
        return;
    }
    const int page = g_page < static_cast<int>(g_guns.size()) ? g_page : 0;
    const Gun& gun = g_guns[page];
    write(g_medium, gun.name, margin, sheet.top + 76.0f * scale, kInk);
    if (gun.fixed_sights) {
        write(g_medium, "This one has fixed sights, so there's", margin, sheet.top + 150.0f * scale, kInk);
        write(g_medium, "nothing to adjust. Aim off for range.", margin, sheet.top + 190.0f * scale, kInk);
    } else {
        char line[96];
        snprintf(line, sizeof(line), "Reset sights: %.1f MOA, zeroed at %d yd.  Sights move 0.5 MOA a click.", gun.zero_moa, gun.zero_yards);
        write(g_small, line, margin, sheet.top + 112.0f * scale, kFaintInk);
        draw_gun_rows(gun, sheet, scale, sheet.top + 142.0f * scale);
    }
    const int guns = static_cast<int>(g_guns.size());
    for (int i = 0; i < guns; ++i) {
        const Box tab = gun_tab(sheet, scale, i, guns);
        const bool lit = i == page || tab.holds(g_cursor_x, g_cursor_y);
        fill(tab, lit ? kPaperBand : kPaperColour);
        write_centred(g_small, g_guns[i].name, tab, i == page ? kInk : kFaintInk);
    }
}

void draw_cursor(float scale) {
    const float size = 22.0f * scale;
    const auto arrow = [&](float grow, const Colour& colour) {
        g_gl.Color4f(colour.r, colour.g, colour.b, colour.a);
        g_gl.Begin(kTriangles);
        g_gl.Vertex2f(g_cursor_x - grow, g_cursor_y - grow * 2.0f);
        g_gl.Vertex2f(g_cursor_x + size * 0.72f + grow * 2.0f, g_cursor_y + size * 0.72f + grow);
        g_gl.Vertex2f(g_cursor_x - grow, g_cursor_y + size + grow * 2.0f);
        g_gl.End();
    };
    arrow(1.5f * scale, kCursorEdge);
    arrow(0.0f, kCursor);
}

void press_key_on_screen(int width, int height) {
    const float scale = height / kReferenceHeight;
    if (g_open == Paper::Calculator) {
        const Box body = centred_box(kCaseWidth, kCaseHeight, width, height);
        for (int row = 0; row < kKeyRows; ++row) {
            for (int column = 0; column < kKeyColumns; ++column) {
                const char key = kKeys[row][column];
                const int span = column + 1 < kKeyColumns && kKeys[row][column + 1] == key ? 2 : 1;
                if (calculator_key(body, scale, row, column, span).holds(g_cursor_x, g_cursor_y)) {
                    g_calculator.press(key);
                    return;
                }
            }
        }
    } else if (g_open == Paper::Sheet) {
        const Box sheet = centred_box(kSheetWidth, kSheetHeight, width, height);
        const int guns = static_cast<int>(g_guns.size());
        for (int i = 0; i < guns; ++i) {
            if (gun_tab(sheet, scale, i, guns).holds(g_cursor_x, g_cursor_y)) g_page = i;
        }
    }
}

bool key_pressed(int code) {
    const bool down = game_has_focus() && (GetAsyncKeyState(code) & 0x8000) != 0;
    const bool pressed = down && !g_keys_down[code];
    g_keys_down[code] = down;
    return pressed;
}

// The number pad only, as the row above the letters has its own jobs in the game.
void read_number_pad() {
    struct PadKey {
        int code;
        char key;
    };
    static constexpr PadKey kPad[] = {
        {VK_NUMPAD0, '0'}, {VK_NUMPAD1, '1'},  {VK_NUMPAD2, '2'},  {VK_NUMPAD3, '3'},   {VK_NUMPAD4, '4'}, {VK_NUMPAD5, '5'},
        {VK_NUMPAD6, '6'}, {VK_NUMPAD7, '7'},  {VK_NUMPAD8, '8'},  {VK_NUMPAD9, '9'},   {VK_DECIMAL, '.'}, {VK_ADD, '+'},
        {VK_SUBTRACT, '-'}, {VK_MULTIPLY, '*'}, {VK_DIVIDE, '/'},  {VK_RETURN, '='},    {VK_BACK, 'b'},    {VK_DELETE, 'c'},
    };
    for (const PadKey& pad : kPad) {
        if (key_pressed(pad.code)) g_calculator.press(pad.key);
    }
}

void read_page_keys() {
    const int guns = static_cast<int>(g_guns.size());
    if (key_pressed(VK_LEFT) && guns) g_page = (g_page + guns - 1) % guns;
    if (key_pressed(VK_RIGHT) && guns) g_page = (g_page + 1) % guns;
}

bool __fastcall hook_map_init(void* item, void* unused, void* def) {
    const bool ok = g_real_map_init(item, unused, def);
    const char* paper = engine<GetValueFn>(kGetValue)(def, nullptr, kPaperKey, static_cast<unsigned>(-1));
    if (!paper) g_papers.erase(item);
    else if (!strcmp(paper, kCalculatorName)) g_papers[item] = Paper::Calculator;
    else if (!strcmp(paper, kSheetName)) g_papers[item] = Paper::Sheet;
    return ok;
}

// The map opens its window when raised and shuts it when lowered, which are the moments for these too.
void __fastcall hook_map_stage(void* item, void* unused, int stage, bool entered) {
    const auto paper = g_papers.find(item);
    if (stage != kStageRaised || paper == g_papers.end()) {
        g_real_map_stage(item, unused, stage, entered);
        return;
    }
    g_open = entered ? paper->second : Paper::None;
    g_held = entered ? item : nullptr;
    g_clicked = g_put_away = false;
    if (!entered) return;
    g_cursor_x = g_mode.width * 0.5f;
    g_cursor_y = g_mode.height * 0.5f;
    if (g_open == Paper::Sheet) load_guns();
}

void lower_held_item() {
    void** methods = *static_cast<void***>(g_held);
    reinterpret_cast<LowerFn>(methods[kMethodLower / sizeof(void*)])(g_held);
}

void begin_drawing(int width, int height, int* program) {
    g_gl.GetIntegerv(kCurrentProgram, program);
    g_gl.PushAttrib(kAllAttribBits);
    g_gl.UseProgram(0);
    for (const unsigned state : kStatesOff) g_gl.Disable(state);
    int units = 1;
    g_gl.GetIntegerv(kMostTextureUnits, &units);
    for (int unit = units - 1; unit >= 0; --unit) {
        g_gl.ActiveTexture(kTexture0 + unit);
        for (const unsigned target : kTextureTargets) g_gl.Disable(target);
    }
    g_gl.TexEnvi(kTextureEnv, kTextureEnvMode, kModulate);
    g_gl.ColorMask(1, 1, 1, 1);
    g_gl.Viewport(0, 0, width, height);
    g_gl.Enable(kBlend);
    g_gl.BlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
    for (const unsigned matrix : {kTextureMatrix, kProjection, kModelview}) {
        g_gl.MatrixMode(matrix);
        g_gl.PushMatrix();
        g_gl.LoadIdentity();
        if (matrix == kProjection) g_gl.Ortho(0.0, width, height, 0.0, -1.0, 1.0);
    }
}

void end_drawing(int program) {
    for (const unsigned matrix : {kModelview, kProjection, kTextureMatrix}) {
        g_gl.MatrixMode(matrix);
        g_gl.PopMatrix();
    }
    g_gl.UseProgram(program);
    g_gl.PopAttrib();
}

bool have_fonts(int height) {
    void* context = reinterpret_cast<CurrentContextFn>(graphics_gl_function("wglGetCurrentContext"))();
    if (context != g_context) {
        g_context = context;
        g_fonts_for_height = 0;
        g_small.texture = g_medium.texture = g_large.texture = 0;
    }
    if (g_fonts_for_height == height) return true;
    const float scale = height / kReferenceHeight;
    if (!make_font(&g_large, static_cast<int>(46.0f * scale)) || !make_font(&g_medium, static_cast<int>(26.0f * scale)) ||
        !make_font(&g_small, static_cast<int>(20.0f * scale))) {
        logf("paper: couldn't make its fonts");
        g_gl_failed = true;
        g_gl_ready = false;
        return false;
    }
    g_fonts_for_height = height;
    return true;
}

void draw_frame(int width, int height) {
    if (g_clicked) {
        g_clicked = false;
        press_key_on_screen(width, height);
    }
    fill({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)}, kShade);
    if (g_open == Paper::Calculator) {
        draw_calculator(width, height);
    } else {
        draw_sheet(width, height);
    }
    draw_cursor(height / kReferenceHeight);
}

}  // namespace

bool paper_available() { return g_game != nullptr; }

bool paper_takes_mouse() { return g_open != Paper::None && view_hunt_running(); }

void paper_mouse_moved(long dx, long dy) {
    const float speed = g_mode.height / kReferenceHeight;
    const float x = g_cursor_x + dx * speed;
    const float y = g_cursor_y + dy * speed;
    g_cursor_x = x < 0.0f ? 0.0f : x > g_mode.width ? static_cast<float>(g_mode.width) : x;
    g_cursor_y = y < 0.0f ? 0.0f : y > g_mode.height ? static_cast<float>(g_mode.height) : y;
}

void paper_mouse_down(int button) {
    if (button == 0) g_clicked = true;
    else g_put_away = true;
}

void paper_draw() {
    if (g_open == Paper::None) return;
    // Leaving a hunt doesn't lower what's in your hands first.
    if (!walking_player()) {
        g_open = Paper::None;
        g_held = nullptr;
        return;
    }
    if (!view_hunt_running()) return;
    if (g_put_away) {
        g_put_away = false;
        lower_held_item();
        return;
    }
    if (!load_gl()) return;
    if (g_open == Paper::Calculator) {
        read_number_pad();
    } else {
        read_page_keys();
        fill_in_rows();
    }
    int program = 0;
    begin_drawing(g_mode.width, g_mode.height, &program);
    if (have_fonts(g_mode.height)) draw_frame(g_mode.width, g_mode.height);
    end_drawing(program);
}

std::string paper_add_items(const std::string& equipment) { return equipment + kNewItems; }

namespace {

constexpr int kTestSize = 1080;

bool looks_like(const unsigned char* pixel, const Colour& colour) {
    const auto near_to = [](unsigned char have, float want) { return fabsf(have / 255.0f - want) < 0.03f; };
    return near_to(pixel[0], colour.r) && near_to(pixel[1], colour.g) && near_to(pixel[2], colour.b);
}

// Draws each screen on the real driver, and looks for the paper, the ink and the keys in the result.
int draw_screen(Paper paper, const Colour& background, const Colour& print) {
    using ReadPixelsFn = void(WINAPI*)(int, int, int, int, unsigned, unsigned, void*);
    const auto read_pixels = reinterpret_cast<ReadPixelsFn>(graphics_gl_function("glReadPixels"));
    if (!read_pixels) return 2;
    g_open = paper;
    int program = 0;
    begin_drawing(kTestSize, kTestSize, &program);
    const bool fonts = have_fonts(kTestSize);
    if (fonts) draw_frame(kTestSize, kTestSize);
    end_drawing(program);
    if (!fonts) return 3;
    std::vector<unsigned char> pixels(static_cast<size_t>(kTestSize) * kTestSize * 4);
    read_pixels(0, 0, kTestSize, kTestSize, 0x1908, kUnsignedByte, pixels.data());
    int behind = 0, printed = 0;
    for (size_t at = 0; at < pixels.size(); at += 4) {
        behind += looks_like(&pixels[at], background);
        printed += looks_like(&pixels[at], print);
    }
    logf("paper: test draw has %d background and %d print pixels", behind, printed);
    if (behind < kTestSize * kTestSize / 50) return 4;
    if (printed < 500) return 5;
    return 0;
}

int draw_test() {
    int failed = 1;
    graphics_in_context(kTestSize, 0, [&] {
        if (!load_gl()) return;
        Gun gun;
        gun.name = "Test rifle";
        gun.zero_yards = 100;
        gun.zero_moa = 4.5f;
        gun.rows_done = kRows;
        for (int row = 0; row < kRows; ++row) gun.moa[row] = row * 3.0f - 2.0f;
        gun.moa[kRows - 1] = kMostMoa;
        g_guns = {gun, gun};
        g_guns[1].fixed_sights = true;
        failed = draw_screen(Paper::Sheet, kPaperColour, kInk);
        if (!failed) failed = draw_screen(Paper::Calculator, kCaseColour, kKeyInk);
        g_open = Paper::None;
        g_guns.clear();
    });
    return failed;
}

}  // namespace

void paper_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    const bool init = vtable_patch(reinterpret_cast<void**>(g_game + kMapInitSlot), reinterpret_cast<void*>(&hook_map_init),
                                   reinterpret_cast<void**>(&g_real_map_init));
    const bool stage = vtable_patch(reinterpret_cast<void**>(g_game + kMapStageSlot), reinterpret_cast<void*>(&hook_map_stage),
                                    reinterpret_cast<void**>(&g_real_map_stage));
    if (!init || !stage) g_game = nullptr;
    logf("paper: map item hooks %s", init && stage ? "installed" : "FAILED");
}

int paper_test() {
    const auto typed = [](const char* keys) {
        Calculator calculator;
        for (const char* key = keys; *key; ++key) calculator.press(*key);
        return calculator.display();
    };
    if (typed("") != "0") return 1;
    if (typed("12") != "12") return 2;
    if (typed("12+3=") != "15") return 3;
    if (typed("2+3*4=") != "20") return 4;                 // one step at a time, like a pocket calculator
    if (typed("10-4=") != "6") return 5;
    if (typed("7/2=") != "3.5") return 6;
    if (typed("1.5+1.25=") != "2.75") return 7;
    if (typed("1..5") != "1.5") return 8;                  // a second point is ignored
    if (typed(".5") != "0.5") return 9;
    if (typed("123b") != "12") return 10;
    if (typed("12c") != "0") return 11;
    if (typed("5/0=") != "Error") return 12;
    if (typed("5/0=3") != "3") return 13;                  // and the next key starts again
    if (typed("2+3=+4=") != "9") return 14;                // the total carries on
    if (typed("2+3=7") != "7") return 15;                  // or a new number starts over
    if (typed("6*") != "6") return 16;
    if (typed("1234567890123456") != "123456789012") return 17;  // only so many digits fit
    if (typed("200+10%=") != "220") return 18;
    if (typed("50%=") != "0.5") return 19;
    // Working out 437 yards, 12 past the row for 425, where the rows differ by 1.5 MOA.
    if (typed("12/25*1.5+12.4=") != "13.12") return 20;

    if (number_after("projectile {\n bc: 0.475\n muzzle: 2670\n weight: 168\n", "bc") != 0.475f) return 21;
    if (number_after("bc: 0.475\n muzzle: 2670\n", "muzzle") != 2670.0f) return 22;
    if (number_after(" abc: 3\n bc : 2\n", "bc") != 2.0f) return 23;     // not part of another word, and spaces are fine
    if (number_after(" muzzle_smoke: \"x\"\n", "muzzle") != 0.0f) return 24;  // and not a longer key
    if (number_after(" #weight: 147\n weight: 30\n", "weight") != 30.0f) return 28;  // or a line taken out
    const std::string with_items = paper_add_items("def a {\n}\n");
    if (with_items.find("def a {") != 0) return 25;
    if (with_items.find(std::string("def ") + kCalculatorName + ":eqitem_base") == std::string::npos) return 26;
    if (with_items.find(std::string("def ") + kSheetName + ":eqitem_base") == std::string::npos) return 27;
    return draw_test();
}

}  // namespace longshot
