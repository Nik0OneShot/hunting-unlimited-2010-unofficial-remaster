// Stand-in for the game. The launcher injects longshot_core.dll into this program exactly as
// it does for prism3d.exe, which tests everything that does not need the game running.
// Results go to logs\selftest.txt, and the exit code is the number of failures.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

FILE* g_out = nullptr;
int g_failures = 0;

void check(const char* name, bool passed) {
    fprintf(g_out, "%s  %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++g_failures;
}

void near_check(const char* name, float actual, float expected) {
    const bool passed = fabsf(actual - expected) < 0.05f;
    fprintf(g_out, "%s  %s (got %.3f, expected %.3f)\n", passed ? "PASS" : "FAIL", name, actual,
            expected);
    if (!passed) ++g_failures;
}

using LayoutFn = int(__cdecl*)(const char*, const char*, float, int, char*, int);
using PauseFn = int(__cdecl*)(bool*, bool, bool, bool);

void pause_checks(PauseFn step) {
    bool ours = false;
    check("playing: nothing to do", step(&ours, true, true, false) == 0);
    check("tabbing out of a hunt pauses it", step(&ours, true, false, false) == 1);
    check("that happens once", step(&ours, true, false, true) == 0);
    check("coming back resumes it", step(&ours, true, true, true) == -1);
    check("and that happens once", step(&ours, true, true, false) == 0);

    check("a pause the player set survives tabbing out", step(&ours, true, false, true) == 0);
    check("and is still there on return", step(&ours, true, true, true) == 0);

    check("menus aren't paused", step(&ours, false, false, false) == 0);

    check("tabbing out again pauses", step(&ours, true, false, false) == 1);
    check("a hunt ending lets go of the pause", step(&ours, false, false, true) == -1);
}

constexpr int kEdges = 0, kWide169 = 1, kCenter43 = 2;
constexpr float kUltrawide = 3440.0f / 1440.0f;

// Made up for this test, in the format of the game's own layout files.
const char kHudLayout[] = R"(estruct {
	client {
		entities {
			entity "ui.window" {
				evalues {
					"coords" : "0 0 1024 768"
				}
				entities {
					entity "ui.comp.group" {
						evalues {
							"coords" : "14 1 643 137"
							"id" : "88"
						}
						entities {
							entity "ui.comp.text" {
								evalues {
									"coords" : "288 1 385 108"
								}
							}
						}
					}
					entity "ui.comp.group" {
						evalues {
							"coords" : "747 1 1014 517"
							"id" : "40"
						}
					}
					entity "ui.comp.group" {
						evalues {
							"coords" : "382 -124 1014 0"
							"id" : "31"
						}
					}
					entity "ui.comp.text" {
						evalues {
							"coords" : "459 677 562 701"
							"name" : "timer"
						}
					}
				}
			}
		}
	}
}
)";

const char kScopeLayout[] = R"(estruct {
	client {
		entities {
			entity "ui.window" {
				evalues {
					"coords" : "0 0 1024 768"
				}
				entities {
				}
			}
		}
	}
}
)";

const char kMenuLayout[] = R"(estruct {
	client {
		entities {
			entity "ui.window" {
				evalues {
					"clip_children" : "false"
					"coords" : "0 0 1024 768"
					"fitting" : "true"
				}
				entities {
					entity "ui.comp.text" {
						evalues {
							"text" : "<img src=/material/ui/ui_static_back.mat xscale=stretch yscale=stretch>"
							"coords" : "0 0 1024 768"
						}
					}
				}
			}
		}
	}
}
)";

bool has_coords(const char* text, const char* coords) {
    return strstr(text, (std::string("\"coords\" : \"") + coords + "\"").c_str()) != nullptr;
}

void layout_checks(LayoutFn rewrite) {
    static char out[16384];

    check("a 4:3 screen leaves the HUD alone",
          rewrite("/ui/ghud.esc", kHudLayout, 4.0f / 3.0f, kEdges, out, sizeof(out)) == 0);
    check("other files are left alone",
          rewrite("/ui/mmain.esc", kHudLayout, kUltrawide, kEdges, out, sizeof(out)) == 0);

    check("edges: HUD rewritten",
          rewrite("/ui/ghud.esc", kHudLayout, kUltrawide, kEdges, out, sizeof(out)) > 0);
    check("edges: left panel reaches the screen edge", has_coords(out, "-391 1 238 137"));
    check("edges: its contents move with it", has_coords(out, "-117 1 -20 108"));
    check("edges: right panel reaches the screen edge", has_coords(out, "1152 1 1419 517"));
    check("edges: the equipment bar is centred in the original area",
          has_coords(out, "196 -124 828 0"));
    check("edges: the timer stays put", has_coords(out, "459 677 562 701"));
    check("edges: the window stays where it was", has_coords(out, "0 0 1024 768"));
    check("edges: the window stops clipping", strstr(out, "\"clip_children\" : \"false\"") != nullptr);

    rewrite("/ui/ghud.esc", kHudLayout, kUltrawide, kWide169, out, sizeof(out));
    check("16:9: left panel at the 16:9 edge", has_coords(out, "-157 1 472 137"));
    check("16:9: right panel at the 16:9 edge", has_coords(out, "918 1 1185 517"));

    rewrite("/ui/ghud.esc", kHudLayout, kUltrawide, kCenter43, out, sizeof(out));
    check("4:3: left panel stays", has_coords(out, "14 1 643 137"));
    check("4:3: right panel stays", has_coords(out, "747 1 1014 517"));
    check("4:3: equipment bar stays", has_coords(out, "382 -124 1014 0"));

    rewrite("/ui/ghud.esc", kHudLayout, 16.0f / 10.0f, kWide169, out, sizeof(out));
    check("a 16:9 HUD never leaves a narrower screen", has_coords(out, "-88 1 541 137"));

    check("scope rewritten",
          rewrite("/ui/gscope_r2x.esc", kScopeLayout, kUltrawide, kEdges, out, sizeof(out)) > 0);
    check("scope: bar on the left", has_coords(out, "-408 0 2 768"));
    check("scope: bar on the right", has_coords(out, "1022 0 1432 768"));
    check("scope: bars are solid black", strstr(out, "color=FF000000") != nullptr);

    check("a menu without the background is left alone",
          rewrite("/ui/mcontrols.esc", kScopeLayout, kUltrawide, kEdges, out, sizeof(out)) == 0);
    check("menu rewritten",
          rewrite("/ui/moptions.esc", kMenuLayout, kUltrawide, kEdges, out, sizeof(out)) > 0);
    check("menu: panel on the left", has_coords(out, "-408 0 0 768"));
    check("menu: panel on the right", has_coords(out, "1024 0 1432 768"));
    check("menu: the left panel reflects the background",
          strstr(out, "left=0.3984 right=0.0000") != nullptr);
    check("menu: the right panel reflects it too",
          strstr(out, "left=1.0000 right=0.6016") != nullptr);
    check("menu: the original background stays", has_coords(out, "0 0 1024 768"));
    check("menu: the window stops fitting itself", strstr(out, "\"fitting\" : \"false\"") != nullptr);
    check("menu: nothing to tint here", strstr(out, "top.mat") == nullptr);

    const char bindings[] =
        "def commands {\n\tdef f10 {\n\t\tcommand:\"screenshot\"\n\t}\n\tdef f11 {\n\t\tcommand:\"bugreport\"\n\t}\n}\n";
    check("screenshot key moves on any screen",
          rewrite("/home/controls.def", bindings, 4.0f / 3.0f, kEdges, out, sizeof(out)) > 0);
    check("screenshot key is now F12",
          strstr(out, "def f12 {\n\t\tcommand:\"screenshot\"") != nullptr);
    check("F10 is free", strstr(out, "def f10") == nullptr);
    check("other commands stay", strstr(out, "def f11 {\n\t\tcommand:\"bugreport\"") != nullptr);
    check("a key the player picked stays",
          rewrite("/home/controls.def", "def commands {\n\tdef k {\n\t\tcommand:\"screenshot\"\n\t}\n}\n",
                  kUltrawide, kEdges, out, sizeof(out)) == 0);
    check("something already on F12 stays",
          rewrite("/home/controls.def",
                  "def commands {\n\tdef f10 {\n\t\tcommand:\"screenshot\"\n\t}\n}\n"
                  "def impulses {\n\tdef f12 {\n\t\timpulse:\"run\"\n\t}\n}\n",
                  kUltrawide, kEdges, out, sizeof(out)) == 0);
}

// tools/check_layouts.py extracts the real files and reads the results back. Each line of
// list.txt names a file in `folder`, then the game path it came from.
void rewrite_folder(const wchar_t* folder, LayoutFn rewrite) {
    static char text[1 << 20];
    static char out[1 << 20];
    const float aspects[] = {16.0f / 10.0f, 16.0f / 9.0f, kUltrawide, 32.0f / 9.0f};
    const wchar_t* const aspect_names[] = {L"16x10", L"16x9", L"ultrawide", L"32x9"};

    FILE* list = nullptr;
    _wfopen_s(&list, (std::wstring(folder) + L"\\list.txt").c_str(), L"r");
    check("layout list opens", list != nullptr);
    if (!list) return;

    char name[64], game_path[128];
    while (fscanf_s(list, "%63s %127s", name, 64u, game_path, 128u) == 2) {
        const std::wstring base =
            std::wstring(folder) + L"\\" + std::wstring(name, name + strlen(name));
        FILE* in = nullptr;
        _wfopen_s(&in, base.c_str(), L"rb");
        if (!in) continue;
        text[fread(text, 1, sizeof(text) - 1, in)] = 0;
        fclose(in);

        for (int aspect = 0; aspect < 4; ++aspect) {
            for (int hud = 0; hud < 3; ++hud) {
                const int size = rewrite(game_path, text, aspects[aspect], hud, out, sizeof(out));
                const std::wstring result_path = base + L"." + aspect_names[aspect] + L"." +
                                                 std::to_wstring(hud);
                FILE* result = nullptr;
                _wfopen_s(&result, result_path.c_str(), L"wb");
                if (!result) continue;
                fwrite(out, 1, size, result);
                fclose(result);
            }
        }
    }
    fclose(list);
}

}  // namespace

int main() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy_s(slash + 1, 64, L"logs\\selftest.txt");
    _wfopen_s(&g_out, path, L"w");
    if (!g_out) return 100;

    HMODULE core = GetModuleHandleW(L"longshot_core.dll");
    check("launcher loaded the core before main()", core != nullptr);
    if (!core) {
        fclose(g_out);
        return 1;
    }

    auto test_iat = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(core, "longshot_test_iat"));
    auto test_fov =
        reinterpret_cast<float(__cdecl*)(float, float)>(GetProcAddress(core, "longshot_test_fov"));
    auto test_raw = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(core, "longshot_test_rawmouse"));
    auto read_raw = reinterpret_cast<void(__cdecl*)(long*, long*)>(
        GetProcAddress(core, "longshot_test_rawmouse_read"));
    auto count_raw = reinterpret_cast<long(__cdecl*)()>(
        GetProcAddress(core, "longshot_test_rawmouse_events"));
    auto test_ui = reinterpret_cast<float(__cdecl*)(float, float, float, int)>(
        GetProcAddress(core, "longshot_test_ui_x"));
    auto test_zoom = reinterpret_cast<float(__cdecl*)(float, float, float, float)>(
        GetProcAddress(core, "longshot_test_zoom"));
    auto test_wheel = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(core, "longshot_test_wheel"));
    auto test_glide = reinterpret_cast<float(__cdecl*)(float, float, float)>(
        GetProcAddress(core, "longshot_test_glide"));
    auto test_glide_add = reinterpret_cast<float(__cdecl*)(float, float, float)>(
        GetProcAddress(core, "longshot_test_glide_add"));
    auto test_layout = reinterpret_cast<LayoutFn>(GetProcAddress(core, "longshot_test_layout"));
    auto test_pause = reinterpret_cast<PauseFn>(GetProcAddress(core, "longshot_test_pause"));
    const bool exports = test_iat && test_fov && test_raw && read_raw && count_raw && test_ui &&
                         test_zoom && test_wheel && test_glide && test_glide_add && test_layout &&
                         test_pause;
    check("test exports present", exports);
    if (!exports) {
        fclose(g_out);
        return 1;
    }

    check("import patch reports success", test_iat() == 0);
    check("patched import is what actually gets called", GetTickCount() == 0x12345678);

    near_check("FOV unchanged at 4:3", test_fov(65.0f, 4.0f / 3.0f), 65.0f);
    near_check("FOV widens at 16:9", test_fov(65.0f, 16.0f / 9.0f), 80.69f);
    near_check("FOV widens at 3440x1440", test_fov(65.0f, 3440.0f / 1440.0f), 97.56f);
    near_check("narrow viewport left alone", test_fov(65.0f, 1.0f), 65.0f);

    // The menu panel in a 3440x1440 screenshot: its frame runs from x=1420 to x=2648.
    const float rect_left = -405.33f, rect_right = 1429.33f;
    near_check("UI left edge lands after the bar", test_ui(0.0f, rect_left, rect_right, 3440), 760.0f);
    near_check("UI right edge lands before the bar", test_ui(1024.0f, rect_left, rect_right, 3440), 2680.0f);
    near_check("menu panel left edge", test_ui(352.0f, rect_left, rect_right, 3440), 1420.0f);
    near_check("menu panel right edge", test_ui(1007.0f, rect_left, rect_right, 3440), 2648.1f);

    near_check("zoom doesn't move in no time", test_zoom(20.0f, 10.0f, 0.0f, 0.06f), 20.0f);
    near_check("zoom after one time constant", test_zoom(20.0f, 10.0f, 0.06f, 0.06f), 12.919f);
    near_check("zoom after one frame at 165 Hz", test_zoom(20.0f, 10.0f, 0.00606f, 0.06f), 18.722f);
    near_check("zoom settles on the target", test_zoom(20.0f, 10.0f, 5.0f, 0.06f), 10.0f);
    near_check("zooming out works too", test_zoom(10.0f, 20.0f, 0.06f, 0.06f), 15.518f);
    check("wheel notches queue through a sight, without loss", test_wheel() == 0);

    near_check("list glide after one time constant", test_glide(30.0f, 0.05f, 0.05f), 11.036f);
    near_check("list glide after one frame", test_glide(-30.0f, 0.00606f, 0.045f), -26.222f);
    near_check("list glide snaps home when nearly there", test_glide(0.2f, 0.001f, 0.045f), 0.0f);
    near_check("list glide never overshoots a row", test_glide_add(-20.0f, -30.0f, 30.0f), -30.0f);
    near_check("list glide reverses cleanly", test_glide_add(-20.0f, 30.0f, 30.0f), 10.0f);

    pause_checks(test_pause);
    layout_checks(test_layout);
    wchar_t folder[MAX_PATH];
    if (GetEnvironmentVariableW(L"LONGSHOT_LAYOUT_DIR", folder, MAX_PATH)) {
        rewrite_folder(folder, test_layout);
    }

    check("raw mouse thread starts", test_raw() == 0);

    // Real relative mouse moves, one pixel out and back, so the whole Raw Input path runs.
    // Someone may be using the mouse during the test, and their movement can cancel ours out,
    // so this counts input events instead of adding up movement.
    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE;
    long dx = 0, dy = 0;
    const int steps[] = {1, -1};
    for (const int step : steps) {
        const long before = count_raw();
        move.mi.dx = step;
        SendInput(1, &move, sizeof(move));
        Sleep(150);
        read_raw(&dx, &dy);
        const long seen = count_raw() - before;
        fprintf(g_out, "INFO  after a move of %d: %ld events, dx=%ld dy=%ld\n", step, seen, dx, dy);
        check("raw mouse receives the input", seen > 0);
    }

    fprintf(g_out, "%d failure(s)\n", g_failures);
    fclose(g_out);
    return g_failures;
}
