#include "longshot.h"

#include <cstdarg>
#include <cstdio>

namespace longshot {

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static ULONGLONG g_start = 0;

void log_open() {
    InitializeCriticalSection(&g_lock);
    g_start = GetTickCount64();
    CreateDirectoryW(logs_dir().c_str(), nullptr);
    const std::wstring path = logs_dir() + L"\\longshot.log";
    MoveFileExW(path.c_str(), (path + L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

void logf(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    char line[1024];
    const ULONGLONG ms = GetTickCount64() - g_start;
    int n = snprintf(line, sizeof(line), "[%6llu.%03llu] ", ms / 1000, ms % 1000);

    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);

    if (n > static_cast<int>(sizeof(line)) - 2) n = sizeof(line) - 2;
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line, n, &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

}  // namespace longshot
