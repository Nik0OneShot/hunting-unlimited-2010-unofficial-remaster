// Finds the game, starts it suspended, and arranges for longshot_core.dll to load before
// any game code runs. The game's own files are never modified on disk.

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAppName[] = L"Longshot";
constexpr wchar_t kAppId[] = L"12690";

// String 114 in hu2010.exe. prism3d.exe is a generic host: told nothing, it starts up, finds
// no game to run, and exits with code 0.
constexpr wchar_t kEngineArgs[] = L"-load lib/ds8 lib/loaders lib/gl lib/di8 -exec lib/game";

// Quitting a real game session takes longer than this, so it means something went wrong.
constexpr ULONGLONG kEarlyExitSeconds = 5;

// ---- Injection stub ------------------------------------------------------------------------

#pragma pack(push, 1)
struct StubData {
    FARPROC load_library;
    FARPROC get_proc_address;
    FARPROC virtual_protect;
    BYTE* entry;
    DWORD old_protect;
    BYTE original[5];
    char init_name[32];
    wchar_t dll_path[1024];
};
#pragma pack(pop)

constexpr DWORD kMagicEntry = 0x11111111;
constexpr DWORD kMagicData = 0x22222222;
const BYTE kEndMarker[8] = {'L', 'S', 'H', 'O', 'T', 'E', 'N', 'D'};

// Runs in the game process in place of its entry point. By then the loader has finished,
// so the core DLL can load normally. The stub then restores the entry point and goes there.
__declspec(naked) void stub_code() {
    __asm {
        // Literals, because inline asm treats named constants as memory operands.
        push 0x11111111  // kMagicEntry: becomes the return address to the real entry point
        pushfd
        pushad
        mov ebx, 0x22222222  // kMagicData
        cld

        lea eax, [ebx]StubData.dll_path
        push eax
        call [ebx]StubData.load_library
        test eax, eax
        jz restore

        lea ecx, [ebx]StubData.init_name
        push ecx
        push eax
        call [ebx]StubData.get_proc_address
        test eax, eax
        jz restore
        call eax

    restore:
        lea eax, [ebx]StubData.old_protect
        push eax
        push PAGE_EXECUTE_READWRITE
        push 5
        push [ebx]StubData.entry
        call [ebx]StubData.virtual_protect

        mov edi, [ebx]StubData.entry
        lea esi, [ebx]StubData.original
        mov ecx, 5
        rep movsb

        lea eax, [ebx]StubData.old_protect
        push eax
        push [ebx]StubData.old_protect
        push 5
        push [ebx]StubData.entry
        call [ebx]StubData.virtual_protect

        popad
        popfd
        ret

        _emit 'L'
        _emit 'S'
        _emit 'H'
        _emit 'O'
        _emit 'T'
        _emit 'E'
        _emit 'N'
        _emit 'D'
    }
}

std::vector<BYTE> stub_bytes() {
    auto* start = reinterpret_cast<const BYTE*>(&stub_code);
    for (size_t i = 0; i < 4096; ++i) {
        if (memcmp(start + i, kEndMarker, sizeof(kEndMarker)) == 0) {
            return std::vector<BYTE>(start, start + i);
        }
    }
    return {};
}

bool patch_dword(std::vector<BYTE>& code, DWORD magic, DWORD value) {
    for (size_t i = 0; i + 4 <= code.size(); ++i) {
        if (memcmp(&code[i], &magic, 4) == 0) {
            memcpy(&code[i], &value, 4);
            return true;
        }
    }
    return false;
}

std::wstring g_log_path;

void launcher_log(const std::wstring& line) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_log_path.c_str(), L"a, ccs=UTF-8") == 0 && file) {
        fwprintf(file, L"%ls\n", line.c_str());
        fclose(file);
    }
}

std::wstring last_error(const wchar_t* what) {
    return std::wstring(what) + L" failed (error " + std::to_wstring(GetLastError()) + L")";
}

bool inject(const PROCESS_INFORMATION& pi, const std::wstring& dll, std::wstring* error) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_INTEGER;
    if (!GetThreadContext(pi.hThread, &context)) {
        *error = last_error(L"GetThreadContext");
        return false;
    }

    // At process start, ebx holds the PEB, and the PEB holds the image base at +8.
    DWORD image_base = 0;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!ReadProcessMemory(pi.hProcess, reinterpret_cast<void*>(context.Ebx + 8), &image_base,
                           sizeof(image_base), nullptr) ||
        !ReadProcessMemory(pi.hProcess, reinterpret_cast<void*>(image_base), &dos, sizeof(dos),
                           nullptr) ||
        !ReadProcessMemory(pi.hProcess, reinterpret_cast<void*>(image_base + dos.e_lfanew), &nt,
                           sizeof(nt), nullptr)) {
        *error = last_error(L"Reading the game's headers");
        return false;
    }
    auto* entry = reinterpret_cast<BYTE*>(image_base + nt.OptionalHeader.AddressOfEntryPoint);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    StubData data{};
    data.load_library = GetProcAddress(kernel32, "LoadLibraryW");
    data.get_proc_address = GetProcAddress(kernel32, "GetProcAddress");
    data.virtual_protect = GetProcAddress(kernel32, "VirtualProtect");
    data.entry = entry;
    strcpy_s(data.init_name, "longshot_init");
    wcsncpy_s(data.dll_path, dll.c_str(), _TRUNCATE);
    if (!ReadProcessMemory(pi.hProcess, entry, data.original, sizeof(data.original), nullptr)) {
        *error = last_error(L"Reading the game's entry point");
        return false;
    }

    std::vector<BYTE> code = stub_bytes();
    if (code.empty()) {
        *error = L"Internal error: stub not found";
        return false;
    }

    auto* remote = static_cast<BYTE*>(VirtualAllocEx(pi.hProcess, nullptr, sizeof(data) + code.size(),
                                                     MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE));
    if (!remote) {
        *error = last_error(L"VirtualAllocEx");
        return false;
    }
    BYTE* remote_code = remote + sizeof(data);
    if (!patch_dword(code, kMagicEntry, reinterpret_cast<DWORD>(entry)) ||
        !patch_dword(code, kMagicData, reinterpret_cast<DWORD>(remote))) {
        *error = L"Internal error: stub markers not found";
        return false;
    }
    if (!WriteProcessMemory(pi.hProcess, remote, &data, sizeof(data), nullptr) ||
        !WriteProcessMemory(pi.hProcess, remote_code, code.data(), code.size(), nullptr)) {
        *error = last_error(L"WriteProcessMemory");
        return false;
    }

    BYTE jump[5] = {0xE9};
    const DWORD rel = reinterpret_cast<DWORD>(remote_code) - reinterpret_cast<DWORD>(entry) - 5;
    memcpy(jump + 1, &rel, 4);

    DWORD old = 0;
    if (!VirtualProtectEx(pi.hProcess, entry, sizeof(jump), PAGE_EXECUTE_READWRITE, &old) ||
        !WriteProcessMemory(pi.hProcess, entry, jump, sizeof(jump), nullptr)) {
        *error = last_error(L"Patching the game's entry point in memory");
        return false;
    }
    VirtualProtectEx(pi.hProcess, entry, sizeof(jump), old, &old);
    FlushInstructionCache(pi.hProcess, entry, sizeof(jump));
    return true;
}

// ---- Finding the game ----------------------------------------------------------------------

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_game_dir(const std::wstring& dir) {
    return file_exists(dir + L"\\prism3d.exe") && file_exists(dir + L"\\base.scs");
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    wide.resize(size - 1);
    return wide;
}

std::string read_text(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Reads the quoted value following "key" in Valve's KeyValues text.
std::vector<std::string> vdf_values(const std::string& text, const std::string& key) {
    std::vector<std::string> values;
    const std::string quoted = "\"" + key + "\"";
    for (size_t at = text.find(quoted); at != std::string::npos; at = text.find(quoted, at + 1)) {
        size_t open = text.find('"', at + quoted.size());
        if (open == std::string::npos) break;
        std::string value;
        size_t i = open + 1;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            value += text[i];
        }
        values.push_back(value);
    }
    return values;
}

std::wstring steam_path() {
    wchar_t value[MAX_PATH];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                     nullptr, value, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring path = value;
    for (wchar_t& c : path) {
        if (c == L'/') c = L'\\';
    }
    return path;
}

std::wstring find_steam_install() {
    const std::wstring steam = steam_path();
    if (steam.empty()) return {};

    std::vector<std::wstring> libraries{steam};
    const std::string folders = read_text(steam + L"\\steamapps\\libraryfolders.vdf");
    for (const std::string& path : vdf_values(folders, "path")) {
        libraries.push_back(utf8_to_wide(path));
    }

    const std::wstring manifest_name = std::wstring(L"\\steamapps\\appmanifest_") + kAppId + L".acf";
    for (const std::wstring& library : libraries) {
        const std::wstring manifest = library + manifest_name;
        if (!file_exists(manifest)) continue;
        const auto install = vdf_values(read_text(manifest), "installdir");
        if (install.empty()) continue;
        const std::wstring dir = library + L"\\steamapps\\common\\" + utf8_to_wide(install[0]);
        if (is_game_dir(dir)) return dir;
    }
    return {};
}

std::wstring configured_game_dir(const std::wstring& ini) {
    wchar_t value[MAX_PATH] = {};
    GetPrivateProfileStringW(L"paths", L"game_dir", L"", value, MAX_PATH, ini.c_str());
    return value;
}

// With "%command%" in its launch options, Steam adds the command it would have run. That's some
// file in the game's folder, so wherever it points is the copy Steam wants started.
std::wstring game_dir_in_command(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring path = argv[i];
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) continue;
        path.resize(slash);
        if (is_game_dir(path)) return path;
    }
    return {};
}

std::wstring find_game(const std::wstring& root, const std::wstring& ini,
                       const std::wstring& from_args) {
    if (!from_args.empty()) return from_args;
    const std::wstring configured = configured_game_dir(ini);
    if (!configured.empty()) return configured;
    const std::wstring steam = find_steam_install();
    if (!steam.empty()) return steam;
    for (const wchar_t* relative : {L"\\game", L"\\..\\game"}) {
        const std::wstring dir = root + relative;
        if (is_game_dir(dir)) return dir;
    }
    return {};
}

// ---- Entry ---------------------------------------------------------------------------------

std::wstring executable_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    return path;
}

int fail(const std::wstring& message) {
    launcher_log(L"error: " + message);
    MessageBoxW(nullptr, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
    return 1;
}

int run(const std::wstring& exe, const std::wstring& dir, std::wstring command_line,
        const std::wstring& dll, DWORD flags, bool quiet) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | flags, nullptr, dir.c_str(), &startup, &process)) {
        const std::wstring error = last_error(L"Starting the game");
        launcher_log(L"error: " + error);
        return quiet ? 2 : fail(error);
    }

    std::wstring error;
    if (!inject(process, dll, &error)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        launcher_log(L"error: " + error);
        return quiet ? 3 : fail(error);
    }

    launcher_log(L"started: " + command_line);
    const ULONGLONG started = GetTickCount64();
    AllowSetForegroundWindow(process.dwProcessId);
    ResumeThread(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    const ULONGLONG seconds = (GetTickCount64() - started) / 1000;
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    launcher_log(L"exited with code " + std::to_wstring(exit_code) + L" after " +
                 std::to_wstring(seconds) + L" seconds");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (!quiet && seconds < kEarlyExitSeconds) {
        return fail(L"The game closed right after starting (exit code " +
                    std::to_wstring(exit_code) +
                    L").\n\nPlease send the files in Longshot's logs folder.");
    }
    return static_cast<int>(exit_code);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring game_dir_arg, test_target;
    bool find_only = false;
    for (int i = 1; argv && i < argc; ++i) {
        if (!wcscmp(argv[i], L"--find-only")) find_only = true;
        else if (i + 1 == argc) break;
        else if (!wcscmp(argv[i], L"--game-dir")) game_dir_arg = argv[++i];
        else if (!wcscmp(argv[i], L"--test-target")) test_target = argv[++i];
    }

    const std::wstring root = executable_dir();
    const std::wstring ini = root + L"\\longshot.ini";
    const std::wstring dll = root + L"\\longshot_core.dll";
    CreateDirectoryW((root + L"\\logs").c_str(), nullptr);
    g_log_path = root + L"\\logs\\launcher.log";
    DeleteFileW(g_log_path.c_str());
    if (!file_exists(ini)) CopyFileW((root + L"\\longshot.default.ini").c_str(), ini.c_str(), TRUE);

    if (!test_target.empty()) {
        // Injects into a stand-in program so the launcher can be tested without the game.
        return run(test_target, root, L"\"" + test_target + L"\"", dll, CREATE_NO_WINDOW, true);
    }

    if (!file_exists(dll)) return fail(L"longshot_core.dll is missing from the Longshot folder.");

    wchar_t steam_app[32] = {};
    if (GetEnvironmentVariableW(L"SteamAppId", steam_app, 32)) {
        launcher_log(std::wstring(L"started by Steam, app ") + steam_app);
    }
    if (game_dir_arg.empty() && argv) game_dir_arg = game_dir_in_command(argc, argv);

    const std::wstring game_dir = find_game(root, ini, game_dir_arg);
    if (game_dir.empty() || !is_game_dir(game_dir)) {
        return fail(
            L"Could not find Hunting Unlimited 2010.\n\n"
            L"Make sure it is installed through Steam, or set game_dir under [paths] in "
            L"longshot.ini to the folder that contains prism3d.exe.");
    }

    const std::wstring exe = game_dir + L"\\prism3d.exe";
    const std::wstring engine_log = root + L"\\logs\\engine.log";
    const std::wstring command =
        L"\"" + exe + L"\" " + kEngineArgs + L" -log \"" + engine_log + L"\"";

    launcher_log(L"game folder: " + game_dir);
    if (find_only) {
        launcher_log(L"would run: " + command);
        return 0;
    }
    return run(exe, game_dir, command, dll, 0, false);
}
