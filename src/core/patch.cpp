#include "longshot.h"

#include <cstring>

namespace longshot {

namespace {

const IMAGE_NT_HEADERS* nt_headers(HMODULE mod) {
    auto* base = reinterpret_cast<const BYTE*>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

bool write_pointer(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

bool iat_patch(HMODULE mod, const char* dll, const char* func, void* hook, void** original) {
    auto* nt = nt_headers(mod);
    if (!nt) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto* base = reinterpret_cast<BYTE*>(mod);
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<char*>(base + desc->Name), dll) != 0) continue;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(by_name->Name, func) != 0) continue;

            auto** slot = reinterpret_cast<void**>(&slots->u1.Function);
            if (original) *original = *slot;
            return write_pointer(slot, hook);
        }
    }
    return false;
}

bool vtable_patch(void** slot, void* hook, void** original) {
    if (original) *original = *slot;
    return write_pointer(slot, hook);
}

bool patch_bytes(void* at, const void* expected, const void* replacement, size_t size) {
    if (memcmp(at, expected, size) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, replacement, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

bool write_jump(void* at, void* to) {
    DWORD old = 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    auto* code = static_cast<BYTE*>(at);
    const auto rel = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(at) - 5;
    code[0] = 0xE9;
    memcpy(code + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

bool module_matches(HMODULE mod, uint32_t timestamp, uint32_t image_size) {
    auto* nt = nt_headers(mod);
    return nt && nt->FileHeader.TimeDateStamp == timestamp &&
           nt->OptionalHeader.SizeOfImage == image_size;
}

}  // namespace longshot
