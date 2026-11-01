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
        "ds8.dll": Binary(game / "lib" / "ds8.dll"),
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
        ("game.dll", "p3shared.dll", names["kUiProcess"]),
        ("game.dll", "p3core.dll", names["kSetFov"]),
        ("game.dll", "p3core.dll", names["kProjection"]),
        ("game.dll", "p3shared.dll", names["kUiRef2View"]),
        ("game.dll", "p3shared.dll", names["kUiWinsert"]),
        ("game.dll", "p3shared.dll", names["kUiWshow"]),
        ("game.dll", "p3shared.dll", names["kUiWremove"]),
    ):
        report((dll, symbol) in bins[owner].imports, f"{owner} imports {symbol[:60]}")

    print("--- layout files")
    layout = source("layout.cpp")
    layout_names = {
        m.group(1): m.group(2) for m in re.finditer(r'constexpr char (\w+)\[\] =\s*"([^"]+)"', layout)
    }
    shared = bins["p3shared.dll"]
    report(("p3core.dll", layout_names["kFsOpen"]) in shared.imports, "p3shared imports fs_open")
    for key in ("kMemFileBind", "kFileSize", "kFileRead"):
        report(layout_names[key] in bins["p3core.dll"].exports, f"p3core exports {key}")
    # Layouts and templates are both read by decipher_file, whose first step opens the file.
    open_slot = shared.imports[("p3core.dll", layout_names["kFsOpen"])]
    report(
        any(i.mnemonic == "call" and f"0x{open_slot:x}" in i.op_str for i in shared.disasm(0x31950, 24)),
        "decipher_file opens files through that import",
    )
    for caller, kind in ((0x1c3a, "layouts"), (0x28d9f, "templates")):
        call = shared.disasm(caller, 5)[0]
        report(int(call.op_str, 0) == shared.base + 0x31a60, f"{kind} are read with decipher_file")

    print("--- options menu")
    settings = constants(source("settings.cpp"))
    game_code = bins["game.dll"]
    table = settings["kOptionsVtable"]
    def slot(offset):
        return struct.unpack_from("<I", game_code.image, table + offset)[0] - game_code.base
    handle = slot(settings["kMethodHandle"])
    report(handle == 0x36170, "the options vtable holds its event handler")
    report(slot(settings["kMethodOpened"]) == 0x36c30, "and the method that runs when it opens")
    name_at = bytes(game_code.image).find(b"ui.handler.menu.options\0")
    descriptor = bytes(game_code.image).find(struct.pack("<I", game_code.base + name_at))
    factory = struct.unpack_from("<I", game_code.image, descriptor + 0x18)[0] - game_code.base
    report(
        any(f"0x{game_code.base + table:x}" in i.op_str for i in game_code.disasm(factory, 0x50)),
        "that vtable belongs to ui.handler.menu.options",
    )
    code = game_code.disasm(handle, 0x60)
    report(any(i.op_str == f"ecx, dword ptr [edi + {settings['kViewId']:#x}]" for i in code), "a control's id is where expected")
    report(any(i.mnemonic == "cmp" and i.op_str == "ebp, 2" for i in code), "clicks arrive as kActionClicked")
    report(any(i.mnemonic == "cmp" and i.op_str == "eax, 0xb" for i in code), "the handler stops at id 12, leaving 13 free")
    switch = game_code.disasm(0x3620d, 0x28)
    report(
        any(i.op_str == f"dword ptr [esi + {settings['kHandlerCurrentTab']:#x}], 0" for i in switch)
        and any(i.op_str == f"esi, dword ptr [esi + {settings['kHandlerTabs']:#x}]" for i in switch),
        "tabs are where expected",
    )
    calls = [i.op_str for i in switch if i.mnemonic == "call"]
    report(
        calls[:2] == [hex(game_code.base + settings["kHolderLock"]), hex(game_code.base + settings["kHolderUnlock"])],
        "tab holders are read with kHolderLock and kHolderUnlock",
    )
    report(
        any(i.op_str == f"edi, dword ptr [eax + {settings['kHandlerWindow']:#x}]" for i in switch),
        "a tab's window is where expected",
    )
    # The sound tab sets its cycle buttons the same way.
    sound_code = game_code.disasm(0x53c30, 0x30)
    definition = game_code.base + settings["kButtonValueDef"]
    setter = definition + settings["kValueDefSet"]
    report(any(i.op_str == hex(definition) for i in sound_code), "the sound tab uses that value definition")
    report(any(f"0x{setter:x}" in i.op_str for i in sound_code), "and calls its setter")
    names_of = {e.name: e.address for e in shared.pe.DIRECTORY_ENTRY_EXPORT.symbols}
    group_show = names_of[b"?show@ui_group_t@prism@@UAE_N_N@Z"]
    group_table = 0x3e158
    report(
        struct.unpack_from("<I", shared.image, group_table + settings["kViewShow"])[0] == shared.base + group_show,
        "a group's show method is where expected",
    )
    report(b"?ui_whide@prism@@YAXPAVc_item_t@1@@Z" in names_of, "p3shared exports ui_whide")
    find_child = game_code.disasm(settings["kFindChild"], 16)
    report(
        any(i.op_str == f"eax, dword ptr [ebx + {settings['kViewId']:#x}]" for i in find_child),
        "kFindChild compares that same id",
    )

    print("--- keyboard")
    input_source = constants(source("input.cpp"))
    level = input_source["kKeyboardLevel"]
    di8 = bins["di8.dll"]
    push = di8.disasm(level, 2)[0]
    report(push.mnemonic == "push" and push.op_str == "0x16", "di8 asks for the keyboard without exclusive access")
    ask = di8.disasm(level, 12)
    report(any(i.mnemonic == "call" and i.op_str.endswith("+ 0x34]") for i in ask), "that goes to SetCooperativeLevel")
    key_at = input_source["kDefaultScreenshotKey"]
    default_line = bytes(bins["game.dll"].image[key_at - 4:key_at + 28])
    report(default_line.startswith(b'def f10 { command:"screenshot"'), "the default bindings have the shot on F10")
    keys = bytes(bins["prism3d.exe"].image)
    report(b"\0f12\0" in keys, "the engine knows F12")
    # Bindings in the player's folder are read through the import the file hook replaces.
    report(
        any(i.mnemonic == "call" and f"0x{open_slot:x}" in i.op_str for i in shared.disasm(0x2be20, 32)),
        "read_from_file opens files through that import too",
    )
    addresses = {e.name: e.address for e in shared.pe.DIRECTORY_ENTRY_EXPORT.symbols}
    report(
        addresses[b"?read_from_file@def_pool_t@prism@@QAE_NABVstring@2@@Z"] == 0x2be20,
        "and that is read_from_file",
    )
    paths = bytes(bins["game.dll"].image)
    report(b"/home/controls.def\0" in paths, "the player's bindings are in controls.def")

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

    print("--- sound buffers in ds8.dll")
    sound = constants(source("sound.cpp"))
    ds8 = bins["ds8.dll"]
    for key, value in (("kBufferFlags", "kPlainFlags"), ("kBufferFlags3d", "k3dFlags")):
        ins = ds8.disasm(sound[key], 5)[0]
        report(ins.op_str == f"edi, {sound[value]:#x}", f"{key} holds those flags")
    # Those flags go into the buffer description, through edi, with no focus flag of their own.
    build = ds8.disasm(0x1191, 0x40)
    report(any(i.op_str == "eax, edi" and i.mnemonic == "or" for i in build), "they reach the buffer flags")
    report(all("0x8000" not in i.op_str and "0x4000" != i.op_str for i in build), "no focus flag is set already")

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

    print("--- what the layouts have to respect in game.dll")
    # The side bars are cleared just before this, so it has to come after the world, once a frame.
    process_slot = game_dll.imports[("p3shared.dll", names["kUiProcess"])]
    process_calls = [
        i for i in game_dll.disasm(0x102df0, 0x400)
        if i.mnemonic == "call" and f"0x{process_slot:x}" in i.op_str
    ]
    report(len(process_calls) == 1, "the frame draws the UI once")
    report(
        bytes(game_dll.image).count(struct.pack("<I", process_slot)) == 1,
        "and nothing else draws it",
    )
    # The equipment bar's slide picks its direction from where the bar sits against 0 and 1024.
    reference_slot = game_dll.imports[("p3shared.dll", names["kUiReferenceMode"])]
    report(
        any(f"0x{reference_slot:x}" in i.op_str for i in game_dll.disasm(0x7f0d0, 0x90)),
        "a slide reads the reference size",
    )
    slide_calls = [
        i for i in game_dll.disasm(0x83000, 0x4000)
        if i.mnemonic == "call" and i.op_str == hex(game_dll.base + 0x7f0d0)
    ]
    report(len(slide_calls) == 1, "only one slide is set up")
    report(
        any(i.mnemonic == "push" and i.op_str == "0x1f" for i in game_dll.disasm(0x83f6b, 8)),
        "and that slide is the equipment bar",
    )
    # The scope handler parks its window off-screen and brings it back to exactly 0,0.
    park = game_dll.disasm(0x8b4a2, 12)
    move_slot = game_dll.imports[("p3shared.dll", "?move_to@ui_view_t@prism@@QAEXMM@Z")]
    report(
        park[0].op_str == "0" and any(f"0x{move_slot:x}" in i.op_str for i in park),
        "the scope handler moves its window to 0,0",
    )

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

    print("--- pausing in game.dll")
    # The game object's constructor sets its vtable and stores itself in kGameObject.
    constructor = game_dll.disasm(0x1060f0, 0x60)
    report(
        any(f"0x{game_dll.base + view_constants['kGameObject']:x}" in i.op_str for i in constructor),
        "this constructor fills kGameObject",
    )
    game_table = game_dll.base + 0x1b06cc
    report(any(f"0x{game_table:x}" in i.op_str for i in constructor), "and sets this vtable")
    def game_method(offset):
        return struct.unpack_from("<I", game_dll.image, 0x1b06cc + offset)[0] - game_dll.base
    pause = game_dll.disasm(game_method(view_constants["kMethodPause"]), 13)
    report(
        any(i.op_str == "byte ptr [ecx + 0x550], al" for i in pause) and pause[-1].op_str == "4",
        "kMethodPause sets the flag and takes one argument",
    )
    is_paused = game_dll.disasm(game_method(view_constants["kMethodIsPaused"]), 7)
    report(is_paused[0].op_str == "al, byte ptr [ecx + 0x550]", "kMethodIsPaused reads the flag")
    # The game frame only advances the world while that flag is clear.
    guard = game_dll.disasm(0x1035d0, 10)
    report(
        guard[0].op_str == "al, byte ptr [ebp + 0x550]" and guard[2].mnemonic == "jne",
        "the game frame checks the flag",
    )

    print("--- input offsets in game.dll")
    input_constants = constants(source("input.cpp"))
    slot = game_dll.base + input_constants["kSlotImpulseValue"]
    delta_slot = game_dll.base + input_constants["kSlotLastDelta"]
    look_rate = 0.872665

    def mouse_ids(code):
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
        return set(ids)

    def uses_look_rate(code):
        for ins in code:
            found = re.search(r"\[0x([0-9a-f]+)\]", ins.op_str)
            if found and ins.mnemonic == "fld":
                at = int(found.group(1), 16) - game_dll.base
                if 0 <= at < len(game_dll.image) - 4:
                    value = struct.unpack_from("<f", game_dll.image, at)[0]
                    if abs(value - look_rate) < 0.00001:
                        return True
        return False

    # Where each look function's caller fetches the frame time to pass in.
    feeds = {0x13070: 0x137d4, 0x13a10: 0x13de1, 0x14e10: 0x150d3}
    ranges = re.findall(r"\{(0x[0-9a-f]+), (0x[0-9a-f]+)\},  // (\w+)", source("input.cpp"))
    report(len(ranges) == len(feeds), "every look function is listed")
    for start, stop, name in ranges:
        begin, end = int(start, 16), int(stop, 16)
        code = game_dll.disasm(begin, end - begin)
        report({0x6e, 0x6f, 0x70, 0x71} <= mouse_ids(code), f"{name} reads all four mouse impulses")
        report(code[0].mnemonic == "sub" and code[0].op_str.startswith("esp, "), f"{name} starts where expected")
        tail = bytes(game_dll.image[begin:end]).rstrip(b"\xcc")
        report(tail[-1] == 0xc3 or tail[-3] == 0xc2, f"{name} ends where expected")
        report(uses_look_rate(code), f"{name} scales by the look rate")

        # The caller must pass sys_get_lastdelta as the look function's argument.
        caller = game_dll.disasm(feeds[begin], 32)
        report(f"0x{delta_slot:x}" in caller[0].op_str, f"{name} is fed sys_get_lastdelta")
        report(
            caller[3].mnemonic == "fstp"
            and caller[4].mnemonic == "call"
            and int(caller[4].op_str, 0) == game_dll.base + begin,
            f"that value goes straight into {name}",
        )

    # The ATV's camera is built on third_person_visor, and calls down to it.
    report(
        game_dll.disasm(0x1536b, 5)[0].op_str == hex(game_dll.base + 0x150d0),
        "vehicle_visor uses third_person_visor's look function",
    )
    report(
        struct.unpack_from("<I", game_dll.image, 0x19c624)[0] == game_dll.base + 0x15360,
        "that method is on vehicle_visor's vtable",
    )

    # dead_visor is left out because it adds the mouse straight to its angles.
    dead = game_dll.disasm(0x119d0, 0x1a0)
    report(0x6e in mouse_ids(dead), "dead_visor reads the mouse")
    report(not uses_look_rate(dead), "and doesn't scale it by the look rate")
    report(not any(f"0x{delta_slot:x}" in i.op_str for i in dead), "or by frame time")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
