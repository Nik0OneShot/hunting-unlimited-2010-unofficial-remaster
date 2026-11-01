"""Checks every game symbol and offset Longshot relies on against a game install.

The source is the only place these facts live, so this reads them from there rather than
repeating them, then confirms each against the real binaries. Needs: pip install pefile capstone

    python tools/verify_targets.py <game folder>
"""
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
failures = 0


def report(ok, what):
    global failures
    print(f"{'ok  ' if ok else 'FAIL'}  {what}")
    failures += 0 if ok else 1


def source(name):
    return (ROOT / "src" / "core" / name).read_text(encoding="utf-8")


class Binary:
    def __init__(self, path):
        self.pe = pefile.PE(str(path))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image = self.pe.get_memory_mapped_image()
        self.imports = {}
        for desc in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = desc.dll.decode().lower()
            for item in desc.imports:
                if item.name:
                    self.imports[(dll, item.name.decode())] = item.address
        self.exports = set()
        if hasattr(self.pe, "DIRECTORY_ENTRY_EXPORT"):
            self.exports = {
                e.name.decode() for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name
            }

    def disasm(self, rva, size):
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        return list(md.disasm(self.image[rva:rva + size], self.base + rva))


def string_table(path):
    """Every string in an executable's string table resources."""
    pe = pefile.PE(str(path))
    image = pe.get_memory_mapped_image()
    found = []
    for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if kind.id != pefile.RESOURCE_TYPE["RT_STRING"]:
            continue
        for block in kind.directory.entries:
            entry = block.directory.entries[0].data.struct
            data = image[entry.OffsetToData:entry.OffsetToData + entry.Size]
            at = 0
            while at < len(data):
                length = struct.unpack_from("<H", data, at)[0]
                at += 2
                found.append(data[at:at + length * 2].decode("utf-16le"))
                at += length * 2
    return found


def constants(text):
    return {
        m.group(1): int(m.group(2), 16)
        for m in re.finditer(r"constexpr (?:uint32_t|size_t) (\w+) = (0x[0-9a-fA-F]+);", text)
    }


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    game = Path(sys.argv[1])

    bins = {
        "prism3d.exe": Binary(game / "prism3d.exe"),
        "p3core.dll": Binary(game / "p3core.dll"),
        "p3shared.dll": Binary(game / "p3shared.dll"),
        "gl.dll": Binary(game / "lib" / "gl.dll"),
        "di8.dll": Binary(game / "lib" / "di8.dll"),
        "game.dll": Binary(game / "lib" / "game.dll"),
    }

    print("--- launch arguments")
    launcher = (ROOT / "src" / "launcher" / "main.cpp").read_text(encoding="utf-8")
    arguments = re.search(r'kEngineArgs\[\] = L"([^"]+)"', launcher).group(1)
    report(arguments in string_table(game / "hu2010.exe"), "launcher matches the original launcher")

    print("--- builds")
    for name, stamp, size in re.findall(
        r'constexpr KnownBuild \w+ = \{"([\w.]+)", (0x[0-9A-F]+), (0x[0-9A-F]+)\}',
        source("core.cpp"),
    ):
        b = bins[name]
        report(
            b.pe.FILE_HEADER.TimeDateStamp == int(stamp, 16)
            and b.pe.OPTIONAL_HEADER.SizeOfImage == int(size, 16),
            f"{name} fingerprint",
        )

    print("--- imports the hooks patch")
    view = source("view.cpp")
    names = {m.group(1): m.group(2) for m in re.finditer(r'constexpr char (\w+)\[\] =\s*"([^"]+)"', view)}
    for owner, dll, symbol in (
        ("prism3d.exe", "user32.dll", "CreateWindowExA"),
        ("p3core.dll", "kernel32.dll", "LoadLibraryA"),
        ("gl.dll", "user32.dll", "ChangeDisplaySettingsA"),
        ("gl.dll", "gdi32.dll", "SwapBuffers"),
        ("gl.dll", "kernel32.dll", "GetProcAddress"),
        ("di8.dll", "dinput8.dll", "DirectInput8Create"),
        ("game.dll", "p3shared.dll", names["kUiInit"]),
        ("game.dll", "p3core.dll", names["kSetFov"]),
        ("game.dll", "p3core.dll", names["kProjection"]),
        ("game.dll", "p3shared.dll", names["kUiRef2View"]),
        ("game.dll", "p3shared.dll", names["kUiWinsert"]),
        ("game.dll", "p3shared.dll", names["kUiWshow"]),
        ("game.dll", "p3shared.dll", names["kUiWremove"]),
    ):
        report((dll, symbol) in bins[owner].imports, f"{owner} imports {symbol[:60]}")

    print("--- exports called directly")
    timing = re.search(r'kTmrRead\[\] = "([^"]+)"', source("timing.cpp")).group(1)
    report(timing in bins["p3core.dll"].exports, "p3core exports tmr_read")
    for key in ("kCvarSet", "kUiSetRect", "kUiReferenceMode"):
        report(names[key] in bins["p3shared.dll"].exports, f"p3shared exports {key}")
    for name in ("inp_event", "inp_get_mouse_sensitivity"):
        report(name in bins["prism3d.exe"].exports, f"prism3d exports {name}")

    print("--- cvars in game.dll")
    game_dll = bins["game.dll"]
    expected = {
        "kCvarQuality": "g_quality", "kCvarGrass": "g_grass_density",
        "kCvarTreeLod": "g_tree_lodswitch", "kCvarZclip": "g_zclip",
        "kCvarShadows": "g_shadows", "kCvarWater": "g_water_quality",
        "kCvarParticles": "g_particles", "kCvarUiMap": "g_ui_map",
        "kCvarMode": "r_mode", "kCvarFullscreen": "r_fullscreen",
    }
    view_constants = constants(view)
    for key, name in expected.items():
        rva = view_constants[key]
        actual = bytes(game_dll.image[rva:rva + len(name) + 1])
        report(actual == name.encode() + b"\0", f"{name} at +0x{rva:x}")

    print("--- focus handling in prism3d.exe")
    host = bins["prism3d.exe"]
    wndproc = host.disasm(0xc970, 96)
    report(
        any(i.mnemonic == "cmp" and i.op_str == "esi, 0x1c" for i in wndproc),
        "window procedure handles WM_ACTIVATEAPP",
    )
    # A window that never gets focus also never gets told it is active.
    report(host.image[0x49ed0] == 1, "the game starts out marked active")

    print("--- scope zoom in game.dll")
    zoom = game_dll.disasm(0x8ae80, 0x14a)
    report(
        any(i.op_str == "dword ptr [eax + 0x68], ecx" for i in zoom),
        "zoom step writes the field of view directly",
    )
    report(
        any(i.op_str == "dword ptr [esi + 0xf0], 0x3d8f5c29" for i in zoom),
        "zoom step starts the 0.07 second lockout the wheel pacing waits out",
    )

    print("--- intro in game.dll")
    menu = constants(source("menu.cpp"))
    guard = menu["kIntroFirstLogoGuard"]
    report(bytes(game_dll.image[guard:guard + 2]) == b"\x76\x12", "first logo guard is where expected")
    intro = game_dll.disasm(menu["kIntroUpdateBegin"], menu["kIntroUpdateEnd"] - menu["kIntroUpdateBegin"])
    trigger_slot = game_dll.base + menu["kSlotTriggerByName"]
    report(
        any(i.mnemonic == "call" and f"0x{trigger_slot:x}" in i.op_str for i in intro),
        "the intro asks for its trigger through kSlotTriggerByName",
    )

    print("--- menu lists in game.dll")
    table = game_dll.base + menu["kListVtable"]
    def method(offset):
        return struct.unpack_from("<I", game_dll.image, table - game_dll.base + offset)[0]
    report(method(menu["kMethodAction"]) == 0x10166640, "list vtable holds the wheel handler")
    report(method(0) == 0x10162950, "list vtable starts where expected")
    factory = struct.unpack_from("<I", game_dll.image, 0x1ae554 + 0x18)[0]
    constructor = int(
        [i for i in game_dll.disasm(factory - game_dll.base, 80) if i.mnemonic == "call"][1].op_str, 0
    )
    writes = game_dll.disasm(constructor - game_dll.base, 32)
    report(any(f"0x{table:x}" in i.op_str for i in writes), "ui.comp.common.list uses that vtable")

    print("--- hunt tracking in game.dll")
    enter = view_constants["kEnterGameplayReturn"]
    wshow_slot = game_dll.imports[("p3shared.dll", names["kUiWshow"])]
    call = game_dll.disasm(enter - 6, 6)[0]
    report(
        call.mnemonic == "call" and f"0x{wshow_slot:x}" in call.op_str,
        "kEnterGameplayReturn follows a ui_wshow call",
    )
    report(
        bytes(game_dll.image).count(struct.pack("<I", game_dll.base + 0x3630)) == 1,
        "enter-gameplay is one virtual method, on the hunt class only",
    )

    print("--- main viewport in game.dll")
    getter = game_dll.disasm(0x1024f0, 8)[0]
    report(
        getter.mnemonic == "lea" and f"0x{view_constants['kMainViewport']:x}" in getter.op_str,
        "viewport getter returns kMainViewport",
    )
    # A menu handler fetches the game object immediately before calling that getter.
    fetch = game_dll.disasm(0x1953f, 6)[0]
    report(
        f"0x{game_dll.base + view_constants['kGameObject']:x}" in fetch.op_str,
        "game object lives at kGameObject",
    )

    print("--- input offsets in game.dll")
    input_constants = constants(source("input.cpp"))
    begin, end = input_constants["kLookBegin"], input_constants["kLookEnd"]
    code = game_dll.disasm(begin, end - begin)
    slot = game_dll.base + input_constants["kSlotImpulseValue"]
    ids = []
    for i, ins in enumerate(code):
        if ins.mnemonic == "call" and f"0x{slot:x}" in ins.op_str:
            # The argument push can sit several FPU instructions before the call.
            for previous in reversed(code[max(0, i - 8):i]):
                if previous.mnemonic == "call":
                    break
                if previous.mnemonic == "push" and previous.op_str.startswith("0x"):
                    ids.append(int(previous.op_str, 0))
                    break
    report({0x6e, 0x6f, 0x70, 0x71} <= set(ids), "look function reads all four mouse impulses")
    report(any(i.mnemonic == "sub" and i.op_str == "esp, 0x40" for i in code[:1]), "look function starts where expected")

    # The caller must pass sys_get_lastdelta as the look function's argument.
    caller = game_dll.disasm(0x137d4, 32)
    delta_slot = game_dll.base + input_constants["kSlotLastDelta"]
    report(f"0x{delta_slot:x}" in caller[0].op_str, "look function is fed sys_get_lastdelta")
    report(
        caller[3].mnemonic == "fstp"
        and caller[4].mnemonic == "call"
        and int(caller[4].op_str, 0) == game_dll.base + begin,
        "that value goes straight into the look function",
    )

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
