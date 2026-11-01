#include "longshot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace longshot {

namespace {

constexpr char kFsOpen[] = "?fs_open@prism@@YA?AVret_t@?$auto_ptr_t@Vfile_t@prism@@@1@ABVstring@1@W4fs_realm_t@1@@Z";
constexpr char kMemFileBind[] = "?mem_file_bind@prism@@YA?AVret_t@?$auto_ptr_t@Vfile_t@prism@@@1@QBXI@Z";
constexpr char kFileSize[] = "?size@file_t@prism@@QAE_KXZ";
constexpr char kFileRead[] = "?read@file_t@prism@@QAE_NQAXI@Z";

constexpr float kLayoutWidth = 1024.0f;
constexpr float kLayoutHeight = 768.0f;
constexpr float kBaseAspect = 4.0f / 3.0f;
constexpr float kHud169Width = kLayoutHeight * 16.0f / 9.0f;

// Enough overlap to hide any seam against the masks, and to cover the screen edge.
constexpr int kBleed = 2;

// The menu background, and two layers below everything the menus draw themselves.
constexpr char kMenuBackground[] = "ui_static_back.mat";
constexpr int kMenuBackLayer = -13;
constexpr int kMenuTintLayer = -12;

// The player's own key bindings.
constexpr char kControls[] = "home/controls.def";

// Group ids in ghud.esc.
constexpr int kHudGame = 5;
constexpr int kHudPhoto = 6;
constexpr int kHudMenu = 11;
constexpr int kHudEquipment = 31;
constexpr int kHudStatus = 40;
constexpr int kHudCompass = 86;
constexpr int kHudGear = 88;

using FsOpenFn = void**(__cdecl*)(void**, const char* const*, int);
using MemFileBindFn = void**(__cdecl*)(void**, const void*, unsigned int);
using FileSizeFn = uint64_t(__thiscall*)(void*);
using FileReadFn = bool(__thiscall*)(void*, void*, unsigned int);
using FileDeleteFn = void(__thiscall*)(void*, int);

FsOpenFn g_real_fs_open = nullptr;
MemFileBindFn g_mem_file_bind = nullptr;
FileSizeFn g_file_size = nullptr;
FileReadFn g_file_read = nullptr;

std::mutex g_cache_lock;
std::map<std::string, std::string> g_cache;

struct Entity {
    std::string type;
    std::string name;
    int parent = -1;
    int id = 0;
    int values_line = -1;
    int entities_line = -1;
    int coords_line = -1;
    int text_line = -1;
};

// An .esc file split into lines, with its entities indexed. Everything not edited stays exactly
// as it was. New lines are added onto existing ones, so line numbers never move.
class EscFile {
public:
    explicit EscFile(const std::string& text) {
        split(text);
        index();
    }

    std::string text() const {
        std::string out;
        for (const std::string& line : lines_) out += line + "\n";
        return out;
    }

    int count() const { return static_cast<int>(entities_.size()); }
    const Entity& entity(int index) const { return entities_[index]; }

    int find_by_id(int id) const {
        for (int i = 0; i < count(); ++i) {
            if (entities_[i].id == id) return i;
        }
        return -1;
    }

    int find_by_name(const char* name) const {
        for (int i = 0; i < count(); ++i) {
            if (entities_[i].name == name) return i;
        }
        return -1;
    }

    int find_by_text(const char* part) const {
        for (int i = 0; i < count(); ++i) {
            if (text_of(i).find(part) != std::string::npos) return i;
        }
        return -1;
    }

    std::string text_of(int index) const {
        const int line = entities_[index].text_line;
        return line < 0 ? std::string() : value_of(lines_[line]);
    }

    void set_text(int index, const std::string& value) {
        const int line = entities_[index].text_line;
        if (line >= 0) replace_value(line, value);
    }

    bool coords(int index, int out[4]) const {
        const int line = entities_[index].coords_line;
        if (line < 0) return false;
        return sscanf_s(value_of(lines_[line]).c_str(), "%d %d %d %d", &out[0], &out[1], &out[2],
                        &out[3]) == 4;
    }

    void set_span(int index, int left, int right) {
        int at[4];
        if (!coords(index, at)) return;
        at[0] = left;
        at[2] = right;
        write_coords(index, at);
    }

    // Moves an entity and everything inside it. Child coordinates are window-relative, not
    // parent-relative, so each one has to move.
    void shift(int index, int dx) {
        int at[4];
        if (coords(index, at)) {
            at[0] += dx;
            at[2] += dx;
            write_coords(index, at);
        }
        for (int i = index + 1; i < count(); ++i) {
            if (entities_[i].parent == index) shift(i, dx);
        }
    }

    void set_value(int index, const char* key, const char* value) {
        const int first = entities_[index].values_line;
        if (first < 0) return;
        const std::string wanted = std::string("\"") + key + "\"";
        for (int i = first + 1; i < static_cast<int>(lines_.size()); ++i) {
            const std::string line = trimmed(lines_[i]);
            if (line == "}") break;
            if (line.rfind(wanted, 0) == 0) {
                replace_value(i, value);
                return;
            }
        }
        lines_[first] += "\r\n" + wanted + " : \"" + value + "\"";
    }

    void add_child(int index, const std::string& entity_text) {
        const int line = entities_[index].entities_line;
        if (line >= 0) lines_[line] += "\r\n" + entity_text;
    }

private:
    static std::string value_of(const std::string& line) {
        const size_t colon = line.find(" : \"");
        const size_t last = line.rfind('"');
        if (colon == std::string::npos || last < colon + 4) return std::string();
        return line.substr(colon + 4, last - colon - 4);
    }

    void replace_value(int line, const std::string& value) {
        std::string& text = lines_[line];
        const size_t colon = text.find(" : \"");
        const size_t last = text.rfind('"');
        if (colon == std::string::npos || last < colon + 4) return;
        text.replace(colon + 4, last - colon - 4, value);
    }

    void write_coords(int index, const int at[4]) {
        char text[64];
        snprintf(text, sizeof(text), "%d %d %d %d", at[0], at[1], at[2], at[3]);
        replace_value(entities_[index].coords_line, text);
    }

    void split(const std::string& text) {
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                lines_.push_back(text.substr(start));
                break;
            }
            lines_.push_back(text.substr(start, end - start));
            start = end + 1;
        }
    }

    static std::string trimmed(const std::string& line) {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) return std::string();
        return line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    }

    void index() {
        enum class Block { Other, Entity, Values, Children };
        struct Open {
            Block block;
            int entity;
        };
        std::vector<Open> open;

        for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
            const std::string line = trimmed(lines_[i]);
            int owner = -1;
            for (auto it = open.rbegin(); it != open.rend() && owner < 0; ++it) owner = it->entity;

            if (line.rfind("entity \"", 0) == 0 && line.back() == '{') {
                Entity entity;
                entity.type = line.substr(8, line.find('"', 8) - 8);
                entity.parent = owner;
                entities_.push_back(entity);
                open.push_back({Block::Entity, count() - 1});
            } else if (line == "evalues {") {
                if (owner >= 0) entities_[owner].values_line = i;
                open.push_back({Block::Values, -1});
            } else if (line == "entities {") {
                if (owner >= 0) entities_[owner].entities_line = i;
                open.push_back({Block::Children, -1});
            } else if (!line.empty() && line.back() == '{') {
                open.push_back({Block::Other, -1});
            } else if (line == "}") {
                if (!open.empty()) open.pop_back();
            } else if (owner >= 0 && !open.empty() && open.back().block == Block::Values) {
                Entity& entity = entities_[owner];
                if (line.rfind("\"coords\"", 0) == 0) entity.coords_line = i;
                else if (line.rfind("\"text\"", 0) == 0) entity.text_line = i;
                else if (line.rfind("\"name\"", 0) == 0) entity.name = value_of(line);
                else if (line.rfind("\"id\"", 0) == 0) entity.id = atoi(value_of(line).c_str());
            }
        }
    }

    std::vector<std::string> lines_;
    std::vector<Entity> entities_;
};

// Whole layout units, rounded outward so nothing stops short of the screen edge.
struct Area {
    int screen_left;
    int screen_right;
    int hud_left;
    int hud_right;
};

Area area_for(float aspect, HudLayout hud) {
    const float visible = kLayoutHeight * aspect;
    const float screen_left = (kLayoutWidth - visible) * 0.5f;
    const float screen_right = (kLayoutWidth + visible) * 0.5f;

    float hud_left = 0.0f, hud_right = kLayoutWidth;
    if (hud == HudLayout::Edges) {
        hud_left = screen_left;
        hud_right = screen_right;
    } else if (hud == HudLayout::Wide169) {
        hud_left = (kLayoutWidth - kHud169Width) * 0.5f;
        hud_right = (kLayoutWidth + kHud169Width) * 0.5f;
        if (hud_left < screen_left) hud_left = screen_left;
        if (hud_right > screen_right) hud_right = screen_right;
    }

    Area area;
    area.screen_left = static_cast<int>(std::floor(screen_left));
    area.screen_right = static_cast<int>(std::ceil(screen_right));
    area.hud_left = static_cast<int>(std::lround(hud_left));
    area.hud_right = static_cast<int>(std::lround(hud_right));
    return area;
}

std::string black_bar(int left, int right) {
    char text[256];
    snprintf(text, sizeof(text),
             "entity \"ui.comp.text\" {\r\nevalues {\r\n"
             "\"text\" : \"<img src=/material/ui/color.mat xscale=stretch yscale=stretch "
             "color=FF000000>\"\r\n"
             "\"coords\" : \"%d 0 %d 768\"\r\n}\r\n}",
             left, right);
    return text;
}

void replace_all(std::string* text, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = text->find(from, at)) != std::string::npos) {
        text->replace(at, from.size(), to);
        at += to.size();
    }
}

// The scope is a circle inside the 4:3 area, with black corners that reach its sides. Black bars
// carry that on to the screen edges. The window itself has to stay at 0,0: the scope handler
// parks it off-screen and brings it back with move_to, which would otherwise shift it sideways.
void fix_scope(EscFile& esc, const Area& area) {
    esc.set_value(0, "clip_children", "false");
    esc.add_child(0, black_bar(area.screen_left - kBleed, kBleed));
    esc.add_child(0, black_bar(static_cast<int>(kLayoutWidth) - kBleed, area.screen_right + kBleed));
}

// The binocular mask is a band across the top and bottom, open at the sides, so bars would cut it
// off with a straight edge. Stretching it from the centre keeps its shape.
void fix_binoculars(EscFile& esc, const Area& area) {
    esc.set_value(0, "clip_children", "false");
    for (int i = 0; i < esc.count(); ++i) {
        if (esc.text_of(i).find("binocs_corner") == std::string::npos) continue;
        int at[4];
        if (!esc.coords(i, at)) continue;
        if (at[0] == 0) esc.set_span(i, area.screen_left - kBleed, at[2]);
        else esc.set_span(i, at[0], area.screen_right + kBleed);
    }
}

// The lids are two images side by side, so their total width has to be even. See fix_lid_template.
int lid_half_width(const Area& area) {
    const int width = area.screen_right + kBleed - (area.screen_left - kBleed);
    return (width + 1) / 2;
}

void fix_eyes(EscFile& esc, const Area& area) {
    esc.set_value(0, "clip_children", "false");
    const int left = area.screen_left - kBleed;
    for (int i = 1; i < esc.count(); ++i) esc.set_span(i, left, left + 2 * lid_half_width(area));
}

void fix_flash(EscFile& esc, const Area& area) {
    esc.set_value(0, "clip_children", "false");
    for (int i = 1; i < esc.count(); ++i) {
        esc.set_span(i, area.screen_left - kBleed, area.screen_right + kBleed);
    }
}

// A strip of images sized for the 1024 wide layout, widened to the screen.
void widen_strip(EscFile& esc, int index, const Area& area) {
    if (index < 0) return;
    const int left = area.screen_left - kBleed;
    const int right = area.screen_right + kBleed;
    char width[32];
    snprintf(width, sizeof(width), "width=%d", right - left);

    std::string text = esc.text_of(index);
    replace_all(&text, "width=1024", width);
    // The dimming behind the pause menu leaves its top and bottom at their natural width.
    replace_all(&text, "top.mat height=", std::string("top.mat ") + width + " height=");
    replace_all(&text, "bottom.mat  height=", std::string("bottom.mat ") + width + " height=");
    esc.set_text(index, text);
    esc.set_span(index, left, right);
}

void fix_hud(EscFile& esc, const Area& area) {
    esc.set_value(0, "clip_children", "false");

    widen_strip(esc, esc.find_by_name("top"), area);
    widen_strip(esc, esc.find_by_name("bottom"), area);
    widen_strip(esc, esc.find_by_text("top.mat height=64"), area);

    const int left = area.hud_left;
    const int right_shift = area.hud_right - static_cast<int>(kLayoutWidth);
    const int gear = esc.find_by_id(kHudGear);
    const int compass = esc.find_by_id(kHudCompass);
    const int status = esc.find_by_id(kHudStatus);
    const int equipment = esc.find_by_id(kHudEquipment);
    if (gear >= 0) esc.shift(gear, left);
    if (compass >= 0) esc.shift(compass, left);
    if (status >= 0) esc.shift(status, right_shift);

    // The equipment bar sits to the right only to clear the gear panel, which has now moved
    // away. It has to stay inside 0..1024: the game works out which way it slides in from that,
    // and past either side it would slide in sideways.
    int bar[4];
    if (left != 0 && equipment >= 0 && esc.coords(equipment, bar)) {
        const int width = bar[2] - bar[0];
        esc.shift(equipment, (static_cast<int>(kLayoutWidth) - width) / 2 - bar[0]);
    }

    // These fit themselves around their contents at run time. This just keeps them truthful.
    for (const int id : {kHudGame, kHudPhoto, kHudMenu}) {
        const int group = esc.find_by_id(id);
        if (group >= 0) esc.set_span(group, area.screen_left - kBleed, area.screen_right + kBleed);
    }
}

std::string panel(int left, int right, int layer, const std::string& text) {
    char coords[64];
    snprintf(coords, sizeof(coords), "%d 0 %d 768", left, right);
    return "entity \"ui.comp.text\" {\r\nevalues {\r\n\"text\" : \"" + text + "\"\r\n\"layer\" : \"" +
           std::to_string(layer) + "\"\r\n\"coords\" : \"" + coords + "\"\r\n}\r\n}";
}

// Reflects the menu background across one edge of the layout, so it carries on with no seam. The
// dark layer over it has a ragged top and bottom, so that is reflected the same way.
void add_menu_side(EscFile& esc, int left, int right, bool tinted) {
    const int width = right - left;
    const float reach = std::min(static_cast<float>(width) / kLayoutWidth, 1.0f);
    const bool left_side = right <= 0;
    char image_range[48];
    snprintf(image_range, sizeof(image_range), "left=%.4f right=%.4f", left_side ? reach : 1.0f,
             left_side ? 0.0f : 1.0f - reach);
    const std::string range = image_range;
    const std::string size = "width=" + std::to_string(width);

    esc.add_child(0, panel(left, right, kMenuBackLayer,
                           std::string("<img src=/material/ui/") + kMenuBackground +
                               " xscale=stretch yscale=stretch " + range + ">"));
    if (!tinted) return;
    esc.add_child(0, panel(left, right, kMenuTintLayer,
                           "<color value=@@rgb_bck@@ ><img src=/material/ui/top.mat " + size +
                               " height=64 " + range + "><img src=/material/ui/color.mat " + size +
                               " height=640 color=@@rgb_bck@@><img src=/material/ui/bottom.mat " +
                               size + " height=64 " + range + ">"));
}

// The window keeps its own rect, because code that moves a window goes by its left edge. So it
// has to stop fitting itself around its contents.
void fix_menu(EscFile& esc, bool tinted, const Area& area) {
    esc.set_value(0, "clip_children", "false");
    esc.set_value(0, "fitting", "false");
    add_menu_side(esc, area.screen_left - kBleed, 0, tinted);
    add_menu_side(esc, static_cast<int>(kLayoutWidth), area.screen_right + kBleed, tinted);
}

std::string fix_lid_template(std::string text, const Area& area) {
    char width[32];
    snprintf(width, sizeof(width), "width=%d", lid_half_width(area));
    replace_all(&text, "width=514", width);
    return text;
}

bool is_layout(const std::string& path, const char* prefix) {
    return path.rfind(prefix, 0) == 0 && path.size() > 4 &&
           path.compare(path.size() - 4, 4, ".esc") == 0;
}

bool is_scope(const std::string& path) { return is_layout(path, "ui/gscope"); }

bool is_menu(const std::string& path) {
    return is_layout(path, "ui/m") || path == "ui/sload.esc";
}

bool rewrites(const std::string& path) {
    return is_scope(path) || is_menu(path) || path == "ui/gbinoc.esc" || path == "ui/geyes.esc" ||
           path == "ui/gflash.esc" || path == "ui/ghud.esc" || path == "def/ui/txt.def" ||
           path == kControls;
}

// F12 is where Steam takes screenshots, so the game's own shot belongs there too, rather than on
// its original F10. A key the player picked for themselves is left alone, as is anything already
// on F12.
std::string move_screenshot_key(const std::string& text) {
    const size_t command = text.find("command:\"screenshot\"");
    if (command == std::string::npos || text.find("def f12 {") != std::string::npos) {
        return std::string();
    }
    const size_t key = text.rfind("def ", command);
    if (key == std::string::npos || text.compare(key, 8, "def f10 ") != 0) return std::string();

    std::string changed = text;
    changed.replace(key, 7, "def f12");
    return changed;
}

// With the stretched UI, layouts stay as they are, the same as on a 4:3 screen.
float layout_aspect() {
    return g_cfg.ui_layout == UiLayout::Center43 ? g_mode.aspect : kBaseAspect;
}

std::string normalised(const char* path) {
    while (*path == '/') ++path;
    return path;
}

void close_file(void* file) {
    void** table = *static_cast<void***>(file);
    reinterpret_cast<FileDeleteFn>(table[0])(file, 1);
}

// The game reads every layout through this one function, which makes it the one place to change
// a layout before anything has been built from it. The player's own files supply the content.
void** __cdecl hook_fs_open(void** out, const char* const* path, int realm) {
    g_real_fs_open(out, path, realm);
    if (!*out || !path || !*path) return out;

    const std::string name = normalised(*path);
    if (!rewrites(name)) return out;

    std::lock_guard<std::mutex> lock(g_cache_lock);
    auto cached = g_cache.find(name);
    if (cached == g_cache.end()) {
        std::string original(static_cast<size_t>(g_file_size(*out)), '\0');
        if (!g_file_read(*out, &original[0], static_cast<unsigned int>(original.size()))) {
            logf("layout: could not read %s", name.c_str());
            return out;
        }
        std::string changed = layout_rewrite(name.c_str(), original, layout_aspect(), g_cfg.hud_layout);
        logf("layout: %s %s", name.c_str(), changed.empty() ? "left as it is" : "rewritten");
        cached = g_cache.emplace(name, changed.empty() ? original : changed).first;
    }

    close_file(*out);
    *out = nullptr;
    // The engine reads from this buffer without owning it, and the cache keeps it alive for good.
    g_mem_file_bind(out, cached->second.data(), static_cast<unsigned int>(cached->second.size()));
    return out;
}

}  // namespace

std::string layout_rewrite(const char* path, const std::string& original, float aspect,
                           HudLayout hud) {
    const std::string name = normalised(path);
    if (name == kControls) return move_screenshot_key(original);
    if (aspect <= kBaseAspect + 0.01f) return std::string();
    const Area area = area_for(aspect, hud);

    if (name == "def/ui/txt.def") {
        std::string changed = fix_lid_template(original, area);
        return changed == original ? std::string() : changed;
    }

    EscFile esc(original);
    if (esc.count() == 0 || esc.entity(0).type != "ui.window") return std::string();
    if (is_scope(name)) fix_scope(esc, area);
    else if (name == "ui/gbinoc.esc") fix_binoculars(esc, area);
    else if (name == "ui/geyes.esc") fix_eyes(esc, area);
    else if (name == "ui/gflash.esc") fix_flash(esc, area);
    else if (name == "ui/ghud.esc") fix_hud(esc, area);
    else if (is_menu(name) && original.find(kMenuBackground) != std::string::npos) {
        fix_menu(esc, original.find("top.mat") != std::string::npos, area);
    } else {
        return std::string();
    }
    return esc.text();
}

void layout_install(HMODULE p3core, HMODULE p3shared) {
    g_mem_file_bind = reinterpret_cast<MemFileBindFn>(GetProcAddress(p3core, kMemFileBind));
    g_file_size = reinterpret_cast<FileSizeFn>(GetProcAddress(p3core, kFileSize));
    g_file_read = reinterpret_cast<FileReadFn>(GetProcAddress(p3core, kFileRead));
    if (!g_mem_file_bind || !g_file_size || !g_file_read) {
        logf("layout: engine file functions not found, layouts left as they are");
        return;
    }
    const bool ok = iat_patch(p3shared, "p3core.dll", kFsOpen, reinterpret_cast<void*>(&hook_fs_open),
                              reinterpret_cast<void**>(&g_real_fs_open));
    logf("layout: file hook %s", ok ? "installed" : "FAILED");
}

}  // namespace longshot
