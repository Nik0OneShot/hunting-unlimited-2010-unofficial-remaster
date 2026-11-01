// Stand-in for the game. The launcher injects longshot_core.dll into this program exactly as
// it does for prism3d.exe, which tests everything that does not need the game running.
// Results go to logs\selftest.txt, and the exit code is the number of failures.

#include <windows.h>

#include <cmath>
#include <cstdio>

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
    const bool exports = test_iat && test_fov && test_raw && read_raw && count_raw && test_ui &&
                         test_zoom && test_wheel && test_glide && test_glide_add;
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
