#include "longshot.h"

#include <dbghelp.h>

namespace longshot {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

LONG g_dumped = 0;
LONG g_reports = 0;

bool is_fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

void module_of(void* address, char* name, DWORD size, uintptr_t* offset) {
    name[0] = 0;
    *offset = reinterpret_cast<uintptr_t>(address);
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &mod)) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* slash = strrchr(path, '\\');
            strncpy_s(name, size, slash ? slash + 1 : path, _TRUNCATE);
        }
        *offset -= reinterpret_cast<uintptr_t>(mod);
    }
}

// System DLLs probe bad pointers on purpose (IsBadReadPtr), and catch their own faults.
bool is_system_probe(const char* module) {
    return !_stricmp(module, "kernelbase.dll") || !_stricmp(module, "kernel32.dll") ||
           !_stricmp(module, "ntdll.dll");
}

void write_dump(EXCEPTION_POINTERS* info) {
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return;
    auto write =
        reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!write) return;

    const std::wstring path = logs_dir() + L"\\crash.dmp";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION details{GetCurrentThreadId(), info, FALSE};
    write(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithIndirectlyReferencedMemory,
          &details, nullptr, nullptr);
    CloseHandle(file);
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!is_fatal(code)) return EXCEPTION_CONTINUE_SEARCH;

    char module[64];
    uintptr_t offset = 0;
    module_of(info->ExceptionRecord->ExceptionAddress, module, sizeof(module), &offset);
    if (is_system_probe(module)) return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedIncrement(&g_reports) <= 8) {
        const CONTEXT* c = info->ContextRecord;
        logf("exception: code=%08lX at %s+0x%X (flags=%lu, access=%lu, addr=%p)", code, module,
             static_cast<unsigned>(offset), info->ExceptionRecord->ExceptionFlags,
             info->ExceptionRecord->NumberParameters > 0
                 ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[0])
                 : 0ul,
             info->ExceptionRecord->NumberParameters > 1
                 ? reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1])
                 : nullptr);
        logf("exception: eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX "
             "esp=%08lX",
             c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    }
    if (InterlockedExchange(&g_dumped, 1) == 0) write_dump(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// A vectored handler, because the engine installs its own unhandled-exception filter.
void crash_install() { AddVectoredExceptionHandler(1, on_exception); }

}  // namespace longshot
