"""Checks every game symbol and offset Longshot relies on against a game install.

The source is the only place these facts live, so this reads them from there rather than
repeating them, then confirms each against the real binaries. Needs: pip install pefile capstone

    python tools/verify_targets.py <game folder>
"""
import re
import struct
import sys
import zipfile
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
        for m in re.finditer(r"constexpr (?:uint32_t|uintptr_t|size_t) (\w+) = (0x[0-9a-fA-F]+);", text)
    }


def wildlife_bird_kinds(text):
    return int(re.search(r"kBirdKinds = (\d+)", text).group(1))


def wildlife_lure_goal(text):
    return int(re.search(r"kLureGoal = (\d+)", text).group(1))


def wildlife_goal(text, name):
    return int(re.search(name + r" = (\d+)", text).group(1))


def wildlife_water_geom(text):
    return int(re.search(r"kWaterGeom = (\d+)", text).group(1))


def wildlife_vulture(text):
    return int(re.search(r"kVulture = (\d+)", text).group(1))


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
        ("gl.dll", "gdi32.dll", "SetPixelFormat"),
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

    print("--- frame timing")
    profile = constants(source("profile.cpp"))
    profile_names = {
        m.group(1): m.group(2) for m in re.finditer(r'constexpr char (\w+)\[\] =\s*"([^"]+)"', source("profile.cpp"))
    }
    timed = bins["game.dll"]
    frame_slot = profile["kGameVtable"] + profile["kMethodFrame"]
    report(struct.unpack_from("<I", timed.image, frame_slot)[0] == timed.base + 0x1034c0, "the game object's frame method is where expected")
    render_call = timed.disasm(profile["kRenderCall"], 5)[0]
    report(render_call.mnemonic == "call" and render_call.size == 5, "the frame calls its render function here")
    report(int(render_call.op_str, 0) == timed.base + profile["kRender"], "and that is kRender")
    report(
        any(f"0x{timed.imports[('p3shared.dll', names['kUiProcess'])]:x}" in i.op_str for i in timed.disasm(profile["kRender"], 0x400)),
        "the render function draws the UI",
    )
    sound_name = bytes(timed.image).find(b"snd_update_state\0")
    binding = [i for i in timed.disasm(0x100ec0, 0x40)]
    report(
        any(i.op_str == f"dword ptr [{hex(timed.base + profile['kSlotSoundUpdate'])}], eax" for i in binding) and sound_name > 0,
        "kSlotSoundUpdate holds snd_update_state",
    )
    for key in ("kFsOpen", "kFileRead", "kFileBlockRead"):
        report(profile_names[key] in bins["p3core.dll"].exports, f"p3core exports {key}")
    gl_text = bytes(bins["gl.dll"].image)
    for name in (b"glTexImage2D", b"glTexSubImage2D", b"glCompressedTexImage2DARB", b"glBufferDataARB",
                 b"glEnable", b"glDisable", b"glTexParameterf", b"glTexParameteri"):
        report(name + b"\0" in gl_text, f"gl.dll looks up {name.decode()}")

    print("--- world draw")
    view_values = constants(source("view.cpp"))
    world_game = bins["game.dll"]
    draw_call = world_game.disasm(view_values["kWorldDrawCall"], 5)[0]
    report(
        draw_call.mnemonic == "call" and draw_call.op_str == hex(world_game.base + view_values["kDrawQueues"]),
        "the frame draws the world where expected",
    )

    print("--- weapons")
    weapon_values = constants(source("weapons.cpp"))
    weapon_image = bytes(world_game.image)
    def int_at(offset):
        return struct.unpack_from("<I", weapon_image, offset)[0]
    def call_at(offset):
        return world_game.disasm(offset, 5)[0]
    reads = world_game.disasm(0xda69, 16)
    report(
        any(i.op_str == f"dword ptr [{world_game.base + weapon_values['kSlotImpulseActivated']:#x}]" for i in reads),
        "the player reads presses through the slot weapons.cpp replaces",
    )
    report(call_at(weapon_values["kPlayerInputCall"]).op_str == hex(world_game.base + weapon_values["kPlayerInput"]),
           "and reads its input from the call it replaces")
    for name in ("kRifleStepCall", "kBowStepCall", "kItemStepCall"):
        report(call_at(weapon_values[name]).op_str == hex(world_game.base + weapon_values["kModelStep"]),
               f"{name} steps a model's animation")
    report(int_at(weapon_values["kRifleListenerVtable"]) - world_game.base == 0xaad30, "the rifle's event handler is in place")
    event_code = world_game.disasm(0xaaf9a, 0x20)
    report(
        any(i.op_str == f"al, byte ptr [esi + {weapon_values['kRifleScopeFitted'] - weapon_values['kRifleListener']:#x}]" for i in event_code),
        "it reads whether a scope is fitted where expected",
    )
    update_code = world_game.disasm(0xa66ea, 0x20)
    report(any(i.op_str == f"al, byte ptr [esi + {weapon_values['kRifleLowerWanted']:#x}]" for i in update_code),
           "a wanted lower is kept where expected")
    report(any(i.op_str == "eax, dword ptr [ecx + 0x58]" for i in world_game.disasm(0xa24b0, 4)), "weapon state is where expected")
    report(any(i.op_str == f"byte ptr [esi + {weapon_values['kRifleStopReload']:#x}], 1" for i in world_game.disasm(0xaaafa, 8)),
           "stopping a reload is where expected")

    print("--- walking")
    walk_values = constants(source("walking.cpp"))
    walk_call = call_at(walk_values["kCreateContactCall"])
    report(walk_call.op_str == hex(world_game.base + walk_values["kCreateContact"]), "contacts become joints where expected")
    report(
        any(f"{world_game.base + walk_values['kAvatarListenerVtable']:#x}" in i.op_str for i in world_game.disasm(0x124000, 0x3000)),
        "the avatar's listener is where expected",
    )

    print("--- vehicle")
    vehicle_text = source("vehicle.cpp")
    tuned = re.findall(r"\{(0x[0-9a-f]+), (0x[0-9A-F]+), (0x[0-9A-F]+)\}", vehicle_text)
    report(len(tuned) == 8, "all eight ATV values were read from vehicle.cpp")
    for at, stock, _strong in tuned:
        report(int_at(int(at, 16)) == int(stock, 16), f"the ATV value at {at} is the game's")
    for site in (0x9b21c, 0x9b229):
        report(world_game.disasm(site, 5)[0].mnemonic == "push", f"the suspension is pushed at {site:#x}")
    vehicle_values = constants(vehicle_text)
    report(world_game.disasm(0x9b26f, 2)[0].op_str == str(vehicle_values["kVehicleGroup"]), "the wheels' group is the one expected")
    report(world_game.disasm(0x9b293, 2)[0].op_str == str(vehicle_values["kTyreMaterial"]), "the wheels' material is the one expected")
    report(
        world_game.disasm(0x153bd0, 3)[0].op_str == f"eax, dword ptr [ecx + {vehicle_values['kShapeMaterial']:#x}]",
        "a shape's material is where expected",
    )
    report(
        world_game.disasm(0x153bf0, 3)[0].op_str == f"eax, dword ptr [ecx + {vehicle_values['kShapeGroup']:#x}]",
        "a shape's group is where expected",
    )
    report(
        world_game.disasm(0x106e72, 3)[0].op_str == f"eax, dword ptr [esi + {vehicle_values['kGeomShape']:#x}]",
        "a geom's shape is where expected",
    )
    report(world_game.disasm(0x1290b6, 4)[0].op_str == "byte ptr [edi + 0x54], 0x10", "the physics reads soft CFM from the mode")
    report(world_game.disasm(0x129067, 2)[0].op_str == "al, 8", "the physics reads soft ERP from the mode")
    report(world_game.disasm(0x1290bf, 3)[0].op_str == "eax, dword ptr [edi + 0x6c]", "soft CFM is where expected")
    report(world_game.disasm(0x12906d, 3)[0].op_str == "dword ptr [edi + 0x68]", "soft ERP is where expected")
    wheel_height = world_game.disasm(vehicle_values["kWheelHeight"], 9)
    report(
        len(wheel_height) == 2 and wheel_height[0].mnemonic == "fld" and wheel_height[0].op_str == "dword ptr [esi + 0x54]",
        "a wheel starts from its suspension travel",
    )
    report(
        len(wheel_height) == 2 and wheel_height[1].op_str == f"dword ptr [{world_game.base + vehicle_values['kHalf']:#x}]",
        "and halves it",
    )
    report(int_at(vehicle_values["kHalf"]) == 0x3F000000, "half is 0.5")
    crash_check = world_game.disasm(0x993d1, 6)[0]
    report(
        crash_check.mnemonic == "fcomp" and crash_check.op_str == f"dword ptr [{world_game.base + 0x19aed4:#x}]",
        "the crash handler checks its speed where expected",
    )
    report(int_at(0x19aed4) == 0x3F800000, "against 1")
    report(world_game.disasm(0x106ea2, 3)[0].op_str == f"eax, dword ptr [esi + {vehicle_values['kGeomBody']:#x}]", "a geom's body is where expected")
    for site, what in ((0x12567f, "a plain capsule"), (0x125699, "the geom around an off-centre capsule")):
        report(
            world_game.disasm(site, 6)[0].op_str == f"dword ptr [esi + {vehicle_values['kShapeGeom']:#x}], eax",
            f"a shape holds {what} where expected",
        )
    report(world_game.disasm(0x10b0a2, 6)[0].op_str == f"eax, [esi + {vehicle_values['kBodyPosition']:#x}]", "a body's position is where expected")
    report(world_game.disasm(0x10b102, 6)[0].op_str == f"eax, [esi + {vehicle_values['kBodyVelocity']:#x}]", "a body's velocity is where expected")
    report(world_game.disasm(0x10b132, 6)[0].op_str == f"eax, [esi + {vehicle_values['kBodySpin']:#x}]", "a body's spin is where expected")
    with zipfile.ZipFile(game / "base.scs") as base:
        for corner in ("fl", "fr", "rl", "rr"):
            pose = base.read(f"models/vehicles/atv/atv_{corner}.pma")
            report(pose[0x37:0x37 + 168] == pose[0x37 + 168:0x37 + 336], f"the {corner} wheel's suspension animation has no travel")
    for site, target, mnemonic in ((0x9888d, 0x1a277c, "fcomp"), (0x988a0, 0x19bfe0, "fld")):
        grip_check = world_game.disasm(site, 6)[0]
        report(
            grip_check.mnemonic == mnemonic and grip_check.op_str == f"dword ptr [{world_game.base + target:#x}]",
            f"the tyres' grip is checked at {site:#x}",
        )

    vehicle_all = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"constexpr (?:uint32_t|uintptr_t|size_t) (\w+) = (0x[0-9a-fA-F]+|\d+);", vehicle_text)}
    small = lambda n: f"{n:#x}" if n > 9 else str(n)  # as capstone writes them
    world_image = bytes(world_game.image)
    def method(at):
        return struct.unpack_from("<I", world_image, 0x1b1ad8 + at)[0] - world_game.base
    def code(at, count=8):
        return [f"{i.mnemonic} {i.op_str}" for i in world_game.disasm(at, count * 8)[:count]]
    actor_body = f"mov eax, dword ptr [ecx + {vehicle_all['kActorBody']:#x}]"
    report(f"mov dword ptr [esi], {world_game.base + 0x1b1ad8:#x}" in code(method(0)) and code(method(8))[0] == actor_body, "a dynamic actor holds ODE's body")
    report(method(vehicle_all["kMethodVelocity"]) == 0x1245a0 and code(0x1245a0)[0] == actor_body and f"call {world_game.base + 0x10b0e0:#x}" in code(0x1245a0) and code(0x10b102, 1)[0] == f"lea eax, [esi + {vehicle_all['kBodyVelocity']:#x}]", "its velocity is its body's")
    report(f"call {world_game.base + 0x10b110:#x}" in code(method(vehicle_all["kMethodSpin"])) and code(0x10b132, 1)[0] == f"lea eax, [esi + {vehicle_all['kBodySpin']:#x}]", "and so is its spin")
    report(f"call {world_game.base + 0x10b030:#x}" in code(method(vehicle_all["kMethodSetSpin"]), 12) and f"mov dword ptr [esi + {vehicle_all['kBodySpin']:#x}], eax" in code(0x10b030, 16), "which it sets straight")
    report("call dword ptr [edx + 0x80]" in code(vehicle_all["kSetVelocity"], 40) and f"call {world_game.base + 0x10afe0:#x}" in code(method(0x80), 12), "setting its velocity goes the same way")
    report(code(0x10c786, 1)[0] == "lea ebp, [esi + 0xd8]" and code(0x10c794, 1)[0] == f"lea eax, [esi + {vehicle_all['kBodyRotation']:#x}]", "a body's rotation is just before its quaternion")
    report(code(0x10b552, 2) == [f"mov eax, dword ptr [esi + {vehicle_all['kBodyFlags']:#x}]", "shr eax, 2"] and vehicle_all["kBodyDisabled"] == 4, "and it's disabled by its flags' 4")
    report(code(0x10bb54, 2) == ["mov dword ptr [esi + 0x20], edi", "mov dword ptr [esi + 0x2c], ebx"] and code(0x10bb5c, 2) == [f"mov edx, dword ptr [edi + {small(vehicle_all['kBodyJoints'])}]", "lea eax, [esi + 0x28]"] and 0x2c - 0x28 == vehicle_all["kJointOtherBody"], "each of its joints names the body at the other end")
    report(code(0x9a928, 1)[0] == "push 0x43960000" and code(0x9a94b, 1)[0] == f"call {world_game.base + 0x97c50:#x}", "the ATV is 300 kg")
    view_update = struct.unpack_from("<I", world_image, vehicle_all["kVehicleViewUpdateSlot"])[0] - world_game.base
    report(view_update == 0x15360 and code(0x15421, 1)[0] == f"call {world_game.base + vehicle_all['kVehicleViewTarget']:#x}" and code(0x15157, 3) == ["mov ecx, dword ptr [edi + 0x160]", "mov eax, dword ptr [ecx]", "call dword ptr [eax + 0x3c]"], "a vehicle's view finds where the vehicle is each update")
    report(code(0x15426, 2) == ["fld dword ptr [esp + 0x34]", f"fmul dword ptr [{world_game.base + 0x19b7cc:#x}]"] and int_at(0x19b7cc) == 0x40800000 and code(0x15445, 1)[0] == f"fmul dword ptr [esi + {vehicle_all['kViewFollowing']:#x}]", "and eases toward it by 4 times the frame's seconds")
    report(code(0x151e0, 1)[0] == f"lea eax, [ecx + {vehicle_all['kViewFollowing']:#x}]" and "call dword ptr [eax + 0x50]" in code(0x14cb0, 20), "which is what the view goes round")
    report(code(0x1549a, 1)[0] == "ret 4" and code(0x151da, 1)[0] == "ret 4", "its update takes the frame's seconds, and finding the vehicle takes where to put it")

    print("--- distances")
    detail_text = source("detail.cpp")
    detail = constants(detail_text)
    distance_image = bytes(bins["game.dll"].image)
    glare_call = world_game.disasm(detail["kGlareFovCall"] - 2, 6)[0]
    report(
        glare_call.mnemonic == "call" and glare_call.op_str == f"dword ptr [{world_game.base + detail['kGetFovSlot']:#x}]",
        "the sun's glare reads the field of view where expected",
    )
    report(
        ("p3core.dll", "?get_fov@frustum_t@prism@@QBEMXZ") in world_game.imports
        and world_game.imports[("p3core.dll", "?get_fov@frustum_t@prism@@QBEMXZ")] == world_game.base + detail["kGetFovSlot"],
        "through the game's import of get_fov",
    )
    def float_at(offset):
        return struct.unpack_from("<f", distance_image, offset)[0]
    report(distance_image[detail["kDetailScale"]:].startswith(bytes.fromhex("d944240483c148")),
           "the detail scale starts as expected")
    report(float_at(detail["kTileRadiusBase"]) == 10.0, "the tree tile radius starts from 10")
    for name in ("kGrassFadeX", "kGrassFadeZ"):
        report(abs(float_at(detail[name]) - 1 / 30) < 1e-6, f"{name} is the fade's 1/30")
    rows = re.findall(r"\{(0x[0-9a-f]+), \{([^}]*)\}, \{([^}]*)\}, (\d)\}", detail_text)
    report(len(rows) == 6, "every grass patch was read from detail.cpp")
    for at, stock, wide, size in rows:
        stock_bytes = bytes(int(value, 16) for value in stock.split(","))
        wide_bytes = bytes(int(value, 16) for value in wide.split(","))
        report(distance_image[int(at, 16):].startswith(stock_bytes), f"grass bytes at {at} are the game's")
        report(len(stock_bytes) == len(wide_bytes) == int(size), f"and its patch is {size} bytes")

    print("--- walking")
    walking = constants(source("walking.cpp"))
    walk_game = bins["game.dll"]
    walk_image = bytes(walk_game.image)
    def floats_at(offset, count):
        return struct.unpack_from(f"<{count}f", walk_image, offset)
    def close(values, wanted):
        return len(values) == len(wanted) and all(abs(a - b) < 1e-5 for a, b in zip(values, wanted))
    def code_refs(low, high):
        """Every address from low up to high that the code mentions directly."""
        text = next(s for s in walk_game.pe.sections if s.Name.startswith(b".text"))
        data = text.get_data()
        found = []
        for at in range(0, len(data) - 3):
            value = struct.unpack_from("<I", data, at)[0] - walk_game.base
            if low <= value < high:
                found.append((value, text.VirtualAddress + at))
        return found
    picks = walk_game.disasm(0x13501, 18)
    report(
        picks[0].op_str == "dword ptr [esp + 0x50], 0x40900000"
        and picks[2].op_str == "dword ptr [esp + 0x50], 0x3fcccccd"
        and picks[2].address - walk_game.base + 4 == walking["kWalkSpeed"],
        "the walking speed is the 1.6 picked next to running's 4.5",
    )
    report(close(floats_at(0x19c20c, 5), (0, 1, 0.5, 0.5, 0.5)), "moving shares are still, forward, back, left, right")
    report(walking["kMoveShares"] == 0x19c20c + 8, "and the shares set are the last three")
    report(code_refs(0x19c20c, 0x19c220) == [(0x19c20c, 0x1351f)], "and only the speed picks read them")
    report(close(floats_at(0x19c1e8, 5), (0.5, 1, 1, 1.5, 1.5)), "step rates follow the same order")
    report(walking["kStepRates"] == 0x19c1e8 + 4, "and the rates set are forward and backward")
    report(code_refs(0x19c1e8, 0x19c1fc) == [(0x19c1e8, 0x13660)], "and only the head bob reads them")
    report(close(floats_at(0x19c1fc, 3), (1.0, 0.5, 0.2)), "stances change speed as expected")
    volumes = walk_game.disasm(0xcd68, 24)
    report(
        volumes[0].op_str == "dword ptr [esp + 4], 0x3f000000" and volumes[2].op_str == "dword ptr [esp + 4], 0x3f4ccccd",
        "footsteps are 0.5 crouched and 0.8 otherwise",
    )
    def call_at(site, target):
        code = walk_game.disasm(site, 5)
        return bool(code) and code[0].mnemonic == "call" and code[0].op_str == f"{walk_game.base + target:#x}"
    with zipfile.ZipFile(game / "base.scs") as base:
        materials = re.findall(r"(\d+):(\w+)", base.read("material/material.db").decode("latin-1"))
        player_defs = base.read("def/player.def").decode("latin-1")
    footsteps = player_defs[player_defs.index("def footsteps"):player_defs.index("def touch_brush")]
    with_sound = set(re.findall(r"def (\w+) \{\s*sound00", footsteps)) - set(re.findall(r"def (\w+) \{\s*\}", footsteps))
    listed = re.search(r"kGroundHasSteps\[\] = \{(.*?)\};", source("walking.cpp"), re.S).group(1)
    has_steps = [word == "true" for word in re.findall(r"true|false", listed)]
    report([int(number) for number, _ in materials] == list(range(len(has_steps))), "every kind of ground is listed, in order")
    report([name in with_sound for _, name in materials] == has_steps, "and the list of which have footsteps matches the game's")
    report(dict((name, int(number)) for number, name in materials)["grass"] == 5, "grass is the kind expected")
    report(walk_image[0xd2b10:].startswith(bytes.fromhex("8bc133c98908")), "the search for what's under the player starts from 0")
    report(call_at(walking["kFootstepCall"], walking["kFootstep"]), "the footstep call is where expected")
    report(call_at(walking["kSetHearingCall"], walking["kSetHearing"]), "and so is the one that tells animals what they can hear")
    report(walk_image[walking["kSetHearing"]:].startswith(bytes.fromhex("8b442404894118c20400")), "which stores the third sense")
    report(walk_image[0xb5a37:0xb5a3c] == bytes.fromhex("6800009643"), "smell reaches 300 m")
    report(walk_image[0xb5a5a:0xb5a5f] == bytes.fromhex("6800007a43"), "and hearing 250 m")
    report(close(floats_at(0x19ba08, 3), (0.7, 0.3, 0.2)), "the game's noise for each stance is as noted")
    report(walk_image[walking["kPlayerRunning"]:].startswith(bytes.fromhex("568bf18a8699000000")), "the running check starts as expected")
    report(walk_game.disasm(0xbcf3, 12)[0].op_str == f"{walk_game.base + walking['kVehicleVisorClass']:#x}", "the vehicle's visor class is the one doubled for noise")

    print("--- weapon sway")
    weapons = constants(source("weapons.cpp"))
    report(call_at(weapons["kShotSwayCall"], weapons["kSwayOffset"]), "a shot reads the sway offset")
    report(call_at(weapons["kModelSwayCall"], weapons["kSwayOffset"]), "and so does a rifle's model")
    report(call_at(weapons["kViewSwayCall"], weapons["kViewSway"]), "a rifle's view reads the camera's sway")
    report(walk_image[weapons["kViewSwayCheck"]:weapons["kViewSwayCheck"] + 3] == bytes.fromhex("750857"), "behind the check for a drawn gun")
    report(struct.unpack_from("<I", walk_image, weapons["kRifleVtable"] + 0x5c)[0] - walk_game.base == 0xaa910, "and that view belongs to rifles")
    report(close(floats_at(weapons["kAimedGunLag"], 1), (0.12,)), "the aimed gun's lag is the expected limit")
    sway_setter = walk_game.disasm(0xa8fde, 7)[0]
    report(sway_setter.op_str == f"byte ptr [esi + {weapons['kRifleModelShown']:#x}], 1", "the model-shown flag is where expected")
    report(walk_game.disasm(0xa641c, 6)[0].op_str == f"dword ptr [esi + {weapons['kRifleSwayYaw']:#x}]", "sway's first angle is where expected")
    report(walk_game.disasm(0xa63f4, 6)[0].op_str == f"dword ptr [esi + {weapons['kRifleSwayPitch']:#x}]", "and its second")

    print("--- free hunt")
    free_text = source("freehunt.cpp")
    free = constants(free_text)
    def text_at(offset):
        end = walk_image.index(b"\0", offset)
        return walk_image[offset:end].decode()
    def pair_at(offset):
        name, value = struct.unpack_from("<II", walk_image, offset)
        return (text_at(name - walk_game.base), text_at(value - walk_game.base)) if name else None
    report(pair_at(free["kDefaults"]) == ("alberta_last_density00", "2"), "a new profile's settings start as expected")
    def ops(at, count):
        return [f"{i.mnemonic} {i.op_str}" for i in walk_game.disasm(at, count * 8)[:count]]
    free_all = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"constexpr (?:uintptr_t|size_t|int) (\w+) = (0x[0-9a-fA-F]+|\d+);", free_text)}
    report(ops(0x92c66, 2) == [f"lea eax, [ebp + {free_all['kHuntGoals']:#x}]", f"lea esi, [ebp + {free_all['kHuntGoalLists']:#x}]"] and ops(0x92c73, 1) == [f"mov dword ptr [esp + 0x1c], {free_all['kGoals']:#x}"] and ops(0x92ce0, 1) == [f"add edx, {free_all['kGoalSize']}"] and ops(0x92ce3, 1) == [f"add esi, {free_all['kGoalListSize']:#x}"], "a hunt has 11 goals, each with a list")
    report(ops(0x92c84, 3) == ["mov eax, dword ptr [ecx]", "dec eax", "je 0x10092c90".replace("0x10092c90", f"{walk_game.base + 0x92c90:#x}")] and ops(0x92c90, 2) == [f"call {walk_game.base + 0xbab80:#x}", "mov eax, dword ptr [eax + 8]"] and ops(0x92c9b, 1) == [f"call {walk_game.base + free_all['kResizeBytes']:#x}"], "and one of kind 1 is sized to the animals at the start")
    report(ops(0x8fb4e, 1) == ["mov eax, dword ptr [edi + 0x10]"] and ops(0x8fb5f, 1) == ["mov eax, dword ptr [edi + 0x88]"] and ops(0x8fb43, 2) == [f"call {walk_game.base + 0xad680:#x}", "mov esi, dword ptr [eax + 0x114]"], "a hit looks up the first goal's list by the animal's place")
    report(ops(0xbd7c6, 1) == [f"call {walk_game.base + free_all['kAddAnimal']:#x}"] and ops(0xbd5b8, 2) == ["sub esi, edx", "sar esi, 2"], "which is where adding it put it in the manager's list")
    report(ops(0x7e46, 1) == ["mov dword ptr [esi + 8], ebx"] and ops(0x7e4e, 1) == ["ret 4"] and ops(0x7e7d, 1) == ["mov dword ptr [esi + 8], ebx"], "resizing a list of bytes just sets its count")
    report(ops(free_all["kAccomplishedText"] - 4, 1) == [f"mov dword ptr [esp + 0x40], {walk_game.base + free_all['kGameAccomplished']:#x}"] and text_at(free_all["kGameAccomplished"]) == "@@g_lc_accomplished@@", "the hunt says its goals are accomplished from one place")
    first = walk_game.disasm(free["kDefaultsFirst"] - 1, 5)[0]
    walk = walk_game.disasm(free["kDefaultsWalk"] - 1, 5)[0]
    defaults_at = f"{walk_game.base + free['kDefaults']:#x}"
    report(first.op_str == f"eax, dword ptr [{defaults_at}]" and walk.op_str == f"esi, {defaults_at}", "and the list is read from one place")
    default_names = set()
    at = free["kDefaults"]
    while pair_at(at):
        default_names.add(pair_at(at)[0])
        at += 8
    overrides = re.findall(r'\{"([a-z0-9_]+)", "([^"]+)"\}', free_text)
    report(len(overrides) == 24, "all of the new profile's settings were read from freehunt.cpp")
    report({"weapon00", "item00", "item01", "item02", "alberta_last_slot00", "alberta_last_time"} <= default_names, "and the ones replaced are the game's")
    with zipfile.ZipFile(game / "base.scs") as base:
        definitions = base.read("def/game.def").decode("latin-1")
        equipment = base.read("def/equipment.def").decode("latin-1")
        strings = base.read("script/en/local.def").decode("latin-1")
    kinds = re.findall(r"^\tdef ([a-z]+):/species_kind", definitions, re.M)
    wanted = ["wtail", "buffalo", "goat", "mule", "grizzly", "elk"]
    chosen = dict(overrides)
    report([kinds[int(chosen[f"alberta_last_slot0{i}"])] for i in range(6)] == wanted, "the six slots are the animals asked for, in order")
    report([chosen[f"alberta_last_density0{i}"] for i in range(6)] == ["3", "2", "2", "1", "1", "1"], "at high, medium, medium, low, low, low")
    allowed = re.search(r'def alberta:/map_area \{.*?freehunt_species:"([^"]+)"', definitions, re.S).group(1).split(",")
    report(set(wanted) <= set(allowed), "and all of them live in the Northern Rockies")
    report('df_area_alberta:"Northern Rockies"' in strings, "which is what the game calls alberta")
    for name, value in overrides:
        if name.startswith(("weapon", "item")):
            for part in value.split("|"):
                report(re.search(rf"^def {re.escape(part)}:", equipment, re.M) is not None, f"{part} exists")
    opening = walk_game.disasm(free["kOpeningCheck"] - 2, 8)
    report(opening[0].op_str == "ecx, ebx" and opening[1].op_str == f"{walk_game.base + 0x3caf:#x}", "the opening shot only runs in a free hunt")
    glimpse = walk_image.find(b"@@g_ht_glimpseanimal@@\0")
    report(struct.pack("<I", walk_game.base + glimpse) in walk_image[0x3630:0x3cb0], "and it's the shot that names a distance")
    report(walk_game.disasm(free["kReadDangerousCall"], 6)[0].op_str == f"dword ptr [{walk_game.base + free['kGetBool']:#x}]", "the hunt reads dangerous animals where expected")
    report(walk_game.imports.get(("p3shared.dll", "?get_bool@def_obj_t@prism@@QBE_NQBDI@Z")) == walk_game.base + free["kGetBool"], "through get_bool")
    report(call_at(free["kCreateAnimalsCall"], free["kCreateAnimals"]), "and creates its animals where expected")
    imports = {
        "kGetValue": "?get_value@def_obj_t@prism@@QBEPBDQBDI@Z",
        "kGetInt": "?get_int@def_obj_t@prism@@QBEHQBDI@Z",
        "kGetPoint": "?get_point@def_obj_t@prism@@QBE?AV?$vec_t@M$02@2@QBDI@Z",
        "kSetPoint": "?set_point@def_obj_t@prism@@QAEXPBDABV?$vec_t@M$02@2@I@Z",
        "kFindInPool": "?find@def_pool_t@prism@@QBEPAVdef_obj_t@2@PBDI_N@Z",
        "kMakePool": "??0def_pool_t@prism@@QAE@_N@Z",
        "kClearPool": "?clear@def_pool_t@prism@@QAEXXZ",
        "kCopyString": "?copy@mstring_t@prism@@QAE_NABVstring@2@III@Z",
        "kRotate": "?rotate@quat_t@prism@@QBE?AV?$vec_t@M$02@2@ABV32@@Z",
    }
    for constant, name in imports.items():
        address = next((a for (dll, n), a in walk_game.imports.items() if n == name), None)
        report(address == walk_game.base + free[constant], f"{constant} is the game's import")
    report(text_at(free["kNameFormat"]) == "%s_%s%.2i", "animals are named as expected")
    name_push = walk_game.disasm(free["kNameFormatPush"] - 1, 5)[0]
    report(name_push.op_str == f"{walk_game.base + free['kNameFormat']:#x}", "and it's the generator that does it")
    report(call_at(0x27b5f, free["kGenerateAnimals"]), "the free hunt menu calls the generator")
    report(call_at(0xbd7c6, free["kAddAnimal"]), "animals are added where expected")
    report(call_at(0xbe3b4, free["kReadStartPoints"]), "start points are read where expected")
    report(call_at(0xbe3da, free["kFreeStartPoints"]), "and let go of where expected")
    report(struct.unpack_from("<I", walk_image, 0xbe39e)[0] - walk_game.base == free["kStartPointListClass"], "with the list class expected")
    report(walk_game.disasm(0xbd86d, 3)[0].op_str == f"dword ptr [eax + {free['kMethodInit']:#x}]", "an animal's init is the method expected")
    report(walk_game.disasm(0xbd890, 3)[0].op_str == f"dword ptr [eax + {free['kMethodStart']:#x}]", "and its start")
    report(walk_game.disasm(0xbc77c, 6)[0].op_str == f"dword ptr [eax + {free['kAnimalPosition']:#x}]", "an animal's position is where expected")
    report(walk_game.disasm(0xc3e33, 6)[0].op_str == f"al, byte ptr [esi + {free['kAnimalDead']:#x}]", "and whether it's dead")
    report(walk_game.disasm(0xbd838, 6)[0].op_str == f"eax, dword ptr [esi + {free['kAnimalNameText']:#x}]", "and its name")
    report(walk_game.disasm(0xa74b8, 3)[0].op_str == f"dword ptr [edx + {free['kHuntView']:#x}]", "the hunt gives its view where a shot reads it")
    report(walk_game.disasm(0xa74bf, 3)[0].op_str == f"dword ptr [edx + {free['kViewPlace']:#x}]", "and the view its place")
    report(walk_game.disasm(0xa74c2, 3)[0].op_str == f"eax, {free['kPlacePosition']:#x}", "at the offset expected")
    report(walk_game.disasm(0xaa4f9, 3)[0].op_str == f"ecx, dword ptr [eax + {free['kPlaceRotation']:#x}]", "with its rotation after")
    report(walk_game.disasm(0x81ad5, 4)[0].op_str == "dword ptr [eax + 0x24], 1", "mode 1 is the free hunt")
    near_herds = walk_game.disasm(0xbe426, 8)[0]
    report(near_herds.op_str == "dword ptr [esp + 0x38], 0x447a0000", "herds count as near up to 1000 m")
    closest_herd = walk_game.disasm(free["kClosestHerd"] - 4, 8)[0]
    report(closest_herd.op_str == "dword ptr [esp + 0x28], 0x432f0000", "and start no closer than 175 m")
    report(call_at(0xbe996, 0xbe2d0), "the first herd takes the spot nearest the hunter")
    stress_push = walk_game.disasm(free["kStartingStress"] - 1, 5)[0]
    report(stress_push.op_str == "0x3e99999a", "a generated animal starts with stress of 0.3")
    report(text_at(struct.unpack_from("<I", walk_image, free["kStartingStress"] + 5)[0] - walk_game.base) == "val_stress", "and that is its stress")
    report(call_at(free["kWeighNeedsCall"], free["kWeighNeeds"]), "an animal weighs its needs where expected")
    danger = walk_game.disasm(0xb6728, 24)
    report([i.op_str for i in danger[:3]] == [f"dword ptr [esi + {free[k]:#x}]" for k in ("kBrainStress", "kBrainEnemyFear", "kBrainHunterFear")],
           "and danger is stress plus fear of animals and of the hunter")
    report(walk_game.disasm(0xb5ee3, 6)[0].op_str == "eax, dword ptr [esi + 0x150]", "danger is compared with the other needs")
    report(walk_game.disasm(0xb67f1, 6)[0].op_str == f"dword ptr [esi + {free['kBrainClock']:#x}]", "an animal's clock is where expected")
    report(walk_game.disasm(0xc3e64, 6)[0].op_str == f"ebx, [esi + {free['kAnimalBrain']:#x}]", "and its thinking part")
    report(walk_image[0x1310:0x1314] == bytes.fromhex("d94140c3"), "the hunt's clock is where expected")
    report(free["kHuntClock"] == 0x40, "at the offset read")
    report(walk_game.disasm(0x4f20, 6)[0].op_str == f"eax, dword ptr [ecx + {free['kHuntScene']:#x}]", "the hunt holds its scene where expected")
    report(walk_game.disasm(0x3696, 6)[0].op_str == f"ecx, dword ptr [esi + {free['kHuntPlayer']:#x}]", "and its player")
    report(walk_game.disasm(0x369e, 3)[0].op_str == f"dword ptr [edx + {free['kMethodPosition']:#x}]", "whose position comes from the method expected")
    report(walk_game.disasm(0x3753, 3)[0].op_str == f"dword ptr [eax + {free['kMethodMoveTo']:#x}]", "and is set by the method expected")
    report(struct.unpack_from("<I", walk_image, free["kOpeningSlot"])[0] - walk_game.base == 0x3630, "the opening is in the slot expected")
    report(call_at(0xdd73, free["kFloorAt"]), "the player reads the ground and water where expected")
    report(walk_image[free["kFloorAt"]:].startswith(bytes.fromhex("8b44240883ec58")), "and it starts as expected")
    floor = walk_game.disasm(0xd47bd, 11)
    report([i.mnemonic for i in floor] == ["fld", "fst", "fsub", "fstp"] and floor[1].op_str == "dword ptr [esi]" and floor[3].op_str == "dword ptr [esi + 4]",
           "water sits first in the floor, and its depth second")
    report(walk_game.disasm(0xd459f, 3)[0].op_str == "dword ptr [esi + 8], eax", "with the ground third")
    report(call_at(free["kMarkerSpotsCall"], free["kMarkerSpots"]), "the free hunt menu reads its start points where expected")
    report(call_at(0x25b9a, free["kAddMarkerSpot"]), "and adds each one where expected")
    report(walk_game.disasm(0x26305, 3)[0].op_str == f"esi, {free['kMarkerSpotSize']:#x}", "at the size expected")
    report(walk_game.disasm(0x262e4, 6)[0].op_str == "esi, dword ptr [ebx + 0xc4]" and "kListItems = 4" in free_text, "with its items where expected")
    report(walk_game.disasm(0x262de, 6)[0].op_str == "eax, dword ptr [ebx + 0xc8]" and "kListCount = 8" in free_text, "and its count")
    every_spot = walk_game.disasm(0x26201, 3)[0]
    report(every_spot.op_str == "dword ptr [eax + 0x18], esi", "the menu numbers every spot itself")
    report(walk_image[free["kReleaseResource"]:].startswith(bytes.fromhex("578bf98b4704")), "resources are let go of where expected")

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

    # Terrain chunks. The cache's limit of 9 stands in for the heap's limit of 255 pieces.
    terrain_text = source("terrain.cpp")
    terrain = constants(terrain_text)
    core_dll = bins["p3core.dll"]
    keep_physics = walk_game.disasm(terrain["kKeepPhysics"] - 4, 5)[0]
    report(keep_physics.mnemonic == "cmp" and keep_physics.op_str == "dword ptr [esp + 0x10], 9", "9 chunks keep their physics")
    keep_triangles = walk_game.disasm(terrain["kKeepTriangles"] - 4, 5)[0]
    report(keep_triangles.mnemonic == "cmp" and keep_triangles.op_str == "dword ptr [esp + 0x14], 9", "and their triangles")
    report(walk_image[0xd8eac] == 0x73 and walk_image[0xd8f5c] == 0x73, "both past an unsigned test")
    report(call_at(0xd8ee7, 0xdb0d0) and call_at(0xd8f8b, terrain["kDropTriangles"]), "with the rest let go of")
    sites = [int(at, 16) for at in re.findall(r'\{(0x[0-9a-f]+), "\w+"\}', terrain_text)]
    report(len(sites) == 4 and all(call_at(site, terrain["kUseTriangles"]) for site in sites), "the four places that load a chunk's triangles are as expected")
    every_use = [
        at for at in range(0x1000, 0x180000)
        if walk_image[at] == 0xe8 and at + 5 + struct.unpack_from("<i", walk_image, at + 1)[0] == terrain["kUseTriangles"]
    ]
    report(sorted(every_use) == sorted(sites), "and nothing else does")
    report(walk_game.disasm(0xd8f84, 6)[0].op_str == f"eax, [esi + {terrain['kLandscapeHeap']:#x}]", "the landscape holds its heap where expected")
    report(walk_game.disasm(0xd8fd1, 3)[0].op_str == f"edi, dword ptr [esi + {terrain['kLandscapeChunks']:#x}]", "and its chunks")
    report(walk_game.disasm(0xd8fd4, 3)[0].op_str == f"ecx, dword ptr [esi + {terrain['kLandscapeChunkCount']:#x}]", "and how many")
    report(walk_game.disasm(0xda5ab, 3)[0].op_str == f"eax, dword ptr [ebp + {terrain['kChunkMesh']:#x}]", "a chunk holds its mesh where expected")
    report(walk_game.disasm(0xda5b9, 3)[0].op_str == f"al, byte ptr [eax + {terrain['kMeshFlags']}]" and walk_image[0xda5bc:0xda5bf] == bytes.fromhex("c0e807"), "whose top flag says it's alive")
    report(walk_game.disasm(0xda5ea, 3)[0].op_str == f"eax, dword ptr [eax + {terrain['kMeshModel']:#x}]", "and it holds its model")
    report(call_at(0xda609, terrain["kPieceCount"]), "which gives the pieces")
    report(walk_game.disasm(0xda5d3, 6)[0].op_str == f"dword ptr [ebp + {terrain['kChunkLastUsed']:#x}], eax", "using a chunk stamps it")
    report(walk_game.disasm(0xda5d9, 6)[0].op_str == f"al, byte ptr [ebp + {terrain['kChunkHeld']:#x}]", "and it's only loaded if it isn't held")
    report(walk_game.disasm(0xd9441, 7)[0].op_str == f"byte ptr [esi + {terrain['kChunkHeld']:#x}], 0", "letting go clears that")
    report(core_dll.disasm(0x120a9, 7)[0].op_str == f"dword ptr [edi + {terrain['kHeapSlots']}], 0xff", "the heap stops at 255 slots")
    report(core_dll.disasm(0x121ce, 3)[0].op_str == "eax, 0x18", "with a slot in the top byte of an id")
    report(core_dll.disasm(0x12176, 3)[0].op_str == f"eax, dword ptr [edi + {terrain['kHeapFreeSlots']:#x}]", "and free ones listed where expected")
    with zipfile.ZipFile(game / "base.scs") as base:
        most = max(struct.unpack("<I", base.read(name)[4:8])[0] for name in base.namelist() if re.fullmatch(r"maps/\w+/landscape_\d+\.pmg", name))
    report(most < 64, f"no chunk in any map is near that limit, the largest having {most} pieces")

    # Chunks are kept until the room is wanted, so nothing is rebuilt for going unused for a moment.
    report(walk_image[terrain["kPhysicsStale"]:terrain["kPhysicsStale"] + 2] == bytes.fromhex("770e"), "unused physics goes after 100 frames")
    report(walk_game.disasm(terrain["kPhysicsStale"], 2)[0].op_str == hex(walk_game.base + 0xd8ee0), "by jumping to where it's let go of")
    report(walk_image[terrain["kTrianglesFresh"]:terrain["kTrianglesFresh"] + 2] == bytes.fromhex("7616"), "unused triangles go the same way")
    report(walk_game.disasm(terrain["kTrianglesFresh"], 2)[0].op_str == hex(walk_game.base + 0xd8f98), "by jumping past that if they're fresh")
    report(walk_game.disasm(0xd8ecb, 3)[0].op_str == "eax, 0x64" and walk_game.disasm(0xd8f7b, 3)[0].op_str == "eax, 0x64", "and 100 frames it is")
    report(call_at(terrain["kUsePhysicsCall"], terrain["kUsePhysics"]), "a chunk's physics is loaded where expected")
    report(walk_game.disasm(0xda1b0, 6)[0].op_str == f"cl, byte ptr [esi + {terrain['kChunkPhysicsHeld']:#x}]", "only if it isn't held")
    report(any(i.mnemonic == "ret" and i.op_str == "0xc" for i in walk_game.disasm(0xda170, 0x380)), "and that function takes three values")

    # Grass, built a little at a time.
    missing = walk_game.disasm(detail["kBlockMissing"], 8)
    report([i.op_str for i in missing] == ["eax, dword ptr [esp + 0x14]", "dword ptr [esp + 0x50], ecx"], "building a block starts as expected")
    report(walk_game.disasm(0xf2799, 2)[0].op_str == "ecx, ecx", "with ecx at 0")
    report(walk_game.disasm(0xf27b8, 2)[0].op_str == hex(walk_game.base + detail["kBlockMissing"]), "reached when the walk finds no block")
    report(walk_game.disasm(0xf27c2, 6)[0].op_str == hex(walk_game.base + 0xf2944), "or a dead one")
    skipped = walk_game.disasm(detail["kBlockSkipped"], 21)
    report(skipped[0].mnemonic == "test" and skipped[0].op_str == "edi, edi" and skipped[1].op_str == "dword ptr [esp + 0x94], 0xffffffff", "and skipping it lets go of the block as usual")
    report(skipped[2].op_str == hex(walk_game.base + 0xf296b) and skipped[3].op_str == hex(walk_game.base + 0xf2956), "before moving on")
    report(call_at(detail["kBuildBlockCall"], detail["kBuildBlock"]), "a block is built where expected")
    report(any(i.mnemonic == "ret" and i.op_str == "0xc" for i in walk_game.disasm(detail["kBuildBlock"], 0x700)), "by a function that takes three values")
    jumps_in = [at for at in range(detail["kBuildBlock"], 0xf29b0)
                if detail["kBlockMissing"] < at + 2 + struct.unpack_from("<b", walk_image, at + 1)[0] < detail["kBlockMissing"] + 8
                and 0x70 <= walk_image[at] <= 0x7f]
    report(not jumps_in, "and nothing jumps into the middle of what's replaced")

    # The ATV's brake and steering.
    vehicle_values = constants(source("vehicle.cpp"))
    tick_pointer = walk_game.disasm(vehicle_values["kTickPointer"] - 1, 5)[0]
    report(tick_pointer.mnemonic == "mov" and tick_pointer.op_str == f"ecx, {walk_game.base + vehicle_values['kTick']:#x}", "the ATV's tick is handed over where expected")
    report(walk_image[0x98a1f:0x98a22] == bytes.fromhex("c20400"), "and takes one value")
    report(struct.unpack_from("<I", walk_image, vehicle_values["kControlsSlot"])[0] == walk_game.base + 0x97e70, "the ATV's controls are in the slot expected")
    report(walk_image[0x98082:0x98085] == bytes.fromhex("c20400"), "and take one value")
    report(walk_game.disasm(0x98519, 3)[0].op_str == f"al, byte ptr [edi + {vehicle_values['kAtvActive']:#x}]", "the tick checks the ATV is active")
    report(walk_game.disasm(0x98520, 7)[0].op_str == f"dword ptr [edi + {vehicle_values['kAtvRunning']:#x}], 1", "or running")
    report(walk_game.disasm(0x9852d, 6)[0].op_str == f"ecx, dword ptr [edi + {vehicle_values['kAtvBody']:#x}]", "the ATV holds its body where expected")
    report(walk_game.disasm(0x985c6, 3)[0].op_str == f"dword ptr [eax + {vehicle_values['kMethodVelocity']:#x}]", "whose velocity comes from the method expected")
    report(walk_game.disasm(0x98616, 7)[0].op_str == f"dword ptr [edi + {vehicle_values['kAtvGear']:#x}], 2", "gear 2 is no keys held")
    report(walk_game.disasm(0x97e8b, 5)[0].op_str == "eax, 2" and walk_game.disasm(0x97e90, 6)[0].op_str == f"dword ptr [esi + {vehicle_values['kAtvGear']:#x}], eax", "which it starts each frame as")
    report(walk_game.disasm(0x97ed7, 10)[0].op_str == "dword ptr [esi + 0x11c], 1" and walk_game.disasm(0x97f00, 10)[0].op_str == "dword ptr [esi + 0x11c], 0", "gear 1 is back, and 0 is forward")
    report(walk_game.disasm(0x9862d, 6)[0].op_str == f"al, byte ptr [edi + {vehicle_values['kAtvOnGround']:#x}]", "the ATV knows if it's on the ground")
    report(walk_game.disasm(0x98a10, 7)[0].op_str == "byte ptr [edi + 0x118], 0", "and that's cleared each tick")
    report(call_at(0x986cd, vehicle_values["kSetVelocity"]), "the game sets its speed where expected")
    report(call_at(0x98884, vehicle_values["kLocalVelocity"]), "and reads speed in a body's own frame where expected")
    steering = walk_game.disasm(0x18c6f0, 10)[0]
    report(steering.op_str == f"dword ptr [{walk_game.base + vehicle_values['kMostSteering']:#x}], 0x3f060a92", "steering goes up to 30 degrees")
    report(walk_game.disasm(0x97fb2, 6)[0].op_str == f"dword ptr [{walk_game.base + vehicle_values['kMostSteering']:#x}]", "and the controls read that")
    sound_shared = Binary(game / "sndshared.dll")
    report(all(("vorbisfile.dll", name) in sound_shared.imports for name in ("ov_open_callbacks", "ov_read")), "Ogg files are opened and read through vorbisfile")

    # Occlusion: both tests of a box against the view, and the terrain they're compared with.
    occlusion_text = source("occlusion.cpp")
    occlusion = constants(occlusion_text)
    box_slot = f"dword ptr [{walk_game.base + occlusion['kBoxInViewSlot']:#x}]"
    for at, what in ((occlusion["kChunkInView"], "chunk"), (occlusion["kPlantInView"], "plant")):
        call = walk_game.disasm(at - 2, 6)[0]
        report(call.mnemonic == "call" and call.op_str == box_slot, f"a {what} is tested against the view where expected")
    report(walk_game.imports[("p3core.dll", "?intersects@frustum_t@prism@@QBE_NABVaabox_t@2@@Z")] == walk_game.base + occlusion["kBoxInViewSlot"], "and that's the frustum's box test")
    report(walk_game.disasm(0xd9100, 3)[0].op_str == f"ecx, [ebp + {occlusion['kCameraFrustum']:#x}]", "on the camera's frustum, for chunks")
    report(walk_game.disasm(0xe249c, 3)[0].op_str == f"ecx, [ebp + {occlusion['kCameraFrustum']:#x}]", "and for plants")
    report(walk_game.disasm(0xd90b3, 3)[0].op_str == f"ecx, [ebp + {occlusion['kCameraPosition']:#x}]", "the camera holds its position where expected")
    report(walk_game.disasm(0xe2469, 3)[0].op_str == f"edi, [ebp + {occlusion['kCameraPosition']:#x}]", "and plants read it from there")
    loading = walk_game.disasm(occlusion["kLoadingMessage"] - 2, 6)[0]
    report(loading.mnemonic == "call" and loading.op_str == f"dword ptr [{walk_game.base + occlusion['kMessageSlot']:#x}]", "a landscape's load is announced where expected")
    report(walk_game.disasm(0xd8942, 1)[0].op_str == "eax" and text_at(struct.unpack_from("<I", walk_image, 0xd8944)[0] - walk_game.base) == "Loading landscape [%s]", "with its path")
    report(walk_game.imports[("p3core.dll", "?message@prism@@YAXQBDZZ")] == walk_game.base + occlusion["kMessageSlot"], "through the engine's message function")
    core_exports = bins["p3core.dll"].exports
    for name in re.findall(r'constexpr char kF\w+\[\] = "([^"]+)";', occlusion_text):
        report(name in core_exports, f"p3core exports {name[:24]}")
    box_contains = core_dll.disasm(0x345f0, 0x20)
    report(box_contains[1].op_str == "dword ptr [ecx + edx]" and "+ 0xc" in box_contains[6].op_str, "a box is its lowest corner, then its highest")
    magic = struct.pack("<I", occlusion["kModelMagic"])
    with zipfile.ZipFile(game / "base.scs") as base:
        names = base.namelist()
        lands = [n for n in names if re.fullmatch(r"maps/\w+/landscape\.lnd", n)]
        chunks = [n for n in names if re.fullmatch(r"maps/\w+/landscape_\d+\.pmg", n)]
        bad_models = bad_pieces = 0
        most_number = {}
        for name in chunks:
            data = base.read(name)
            pieces = struct.unpack_from("<I", data, occlusion["kModelPieceCount"])[0]
            table = struct.unpack_from("<I", data, occlusion["kModelPieces"])[0]
            bad_models += not data.startswith(magic) or table != 0x8c
            for piece in range(pieces):
                record = table + piece * occlusion["kPieceSize"]
                count, verts = struct.unpack_from("<II", data, record)
                positions = struct.unpack_from("<I", data, record + occlusion["kPiecePositions"])[0]
                indices = struct.unpack_from("<I", data, record + occlusion["kPieceIndices"])[0]
                fits = positions + verts * 12 <= len(data) and indices + count * 2 <= len(data) and count % 3 == 0
                used = struct.unpack_from(f"<{count}H", data, indices) if fits and count else (0,)
                bad_pieces += not fits or max(used) >= verts
            folder = name.rsplit("/", 1)[0]
            most_number[folder] = max(most_number.get(folder, 0), int(re.search(r"(\d+)\.pmg", name).group(1)))
        counts = {n.rsplit("/", 1)[0]: struct.unpack("<I", base.read(n)[occlusion["kLandscapeChunks"]:occlusion["kLandscapeChunks"] + 4])[0] for n in lands}
        tallest = 0.0
        model_heights = {}
        for units_name in (n for n in names if n.endswith("landscape.game.sii")):
            units = base.read(units_name).decode("latin-1")
            models = dict(re.findall(r'plant_desc : (\S+) \{\s*model_file: "([^"]+)"', units))
            for desc, size in re.findall(r"plant_inst : \S+ \{\s*body_desc: (\S+)\s*placement: \([^)]+\) (\S+) \(", units):
                model = models.get(desc, "").lstrip("/").rsplit(".", 1)[0] + ".pmg"
                if model not in model_heights:
                    head = base.open(model).read(0x3c) if model in names else bytes(0x3c)
                    model_heights[model] = struct.unpack_from("<f", head, occlusion["kModelBox"] + 16)[0]
                scale = struct.unpack(">f", bytes.fromhex(size[1:]))[0] if size.startswith("&") else float(size)
                tallest = max(tallest, model_heights[model] * scale)
    report(bad_models == 0, f"all {len(chunks)} chunks of every map have the layout expected")
    report(bad_pieces == 0, "and every piece's positions and indices are where expected")
    report(all(most_number[f] < counts[f] for f in most_number), "and every chunk's number is under the count in its .lnd")
    taller = float(re.search(r"kChunkTaller = ([\d.]+)f", occlusion_text).group(1))
    report(tallest <= taller, f"chunks allow for the tallest plant on any map, {tallest:.1f} m as placed")

    print("--- the player")
    walking_values = constants(source("walking.cpp"))
    report(walk_game.disasm(0xe398, 6)[0].op_str == f"dword ptr [esi], {walk_game.base + walking_values['kHunterTable']:#x}", "a hunter has the table of methods expected")
    hunter_class = struct.unpack_from("<I", walk_image, 0x19ba44 + 4)[0] - walk_game.base
    report(hunter_class == 0xe510 and call_at(0xe54b, 0xe360), "and is what that class's descriptor makes")
    report(walk_game.disasm(0x3fae, 5)[0].op_str == f"{walk_game.base + 0x19ba44:#x}" and walk_game.disasm(0x3fcf, 6)[0].op_str == "dword ptr [esi + 0x100], eax", "which the hunt keeps as its player")
    report(walk_game.disasm(0x9256, 6)[0].op_str == f"dword ptr [esi], {walk_game.base + 0x19b5b0:#x}" and walk_game.disasm(0x924b, 5)[0].op_str == f"{walk_game.base + 0x19bc2c:#x}", "while another mode keeps something else there")
    report(call_at(0xe828, 0xe730) and walk_game.disasm(0xe767, 6)[0].op_str == f"dword ptr [esi], {walk_game.base + 0x19bc48:#x}", "which isn't a hunter")
    hunter_tables = [i for i in walk_game.disasm(0xe2a0, 0x8000) if i.mnemonic == "mov" and i.op_str.endswith(f", {walk_game.base + walking_values['kHunterTable']:#x}")]
    report(len(hunter_tables) == 1, "and nothing else starts out as one, so no other class is a kind of hunter")

    print("--- wildlife")
    wildlife_text = source("wildlife.cpp")
    wildlife = constants(wildlife_text)
    impact_entries = struct.unpack_from("<14I", walk_image, wildlife["kImpactTable"])
    report(impact_entries[8 - 3] == walk_game.base + wildlife["kNoImpact"], "a bullet hitting a tree throws nothing up")
    report(walk_game.disasm(wildlife["kDirtImpact"], 5)[0].op_str == f"esi, {walk_game.base + 0x1a85a4:#x}" and text_at(0x1a85a4) == "emitter.throw.dirt", "and dirt has its own puff")
    report(walk_game.disasm(0xd67dc, 7)[0].op_str == f"dword ptr [eax*4 + {walk_game.base + wildlife['kImpactTable']:#x}]" and walk_game.disasm(0xd67cc, 3)[0].op_str == "eax, -3", "the table starts at surface 3")
    report(walk_game.disasm(0xd6fd2, 8)[0].op_str == "dword ptr [esp + 0x1c], 8", "and surface 8 is what a plant gives")
    report(walk_game.disasm(0xd6fbc, 5)[0].op_str == f"{walk_game.base + 0x1a9320:#x}", "going by its class")
    report(walk_game.disasm(wildlife["kMostAnimalsCheck"] - 2, 3)[0].op_str == "eax, 0x28", "the generator stops at 40 animals")
    report(walk_game.disasm(wildlife["kTrimTotal"] - 1, 5)[0].op_str == "edi, 0x28", "and trims down to that")
    report(walk_game.disasm(wildlife["kTrimScale"] - 2, 6)[0].op_str == f"dword ptr [{walk_game.base + wildlife['kFortyFloat']:#x}]", "as a float too")
    report(struct.unpack_from("<f", walk_image, wildlife["kFortyFloat"])[0] == 40.0, "which is 40")
    group_call = walk_game.disasm(wildlife["kGroupRollCall"] - 2, 6)[0]
    report(group_call.mnemonic == "call" and group_call.op_str == f"dword ptr [{walk_game.base + wildlife['kRandom']:#x}]", "a group's size is rolled where expected")
    report(walk_game.imports[("p3core.dll", "?rand@randomizer_t@prism@@QAEMXZ")] == walk_game.base + wildlife["kRandom"], "with the engine's random numbers")
    group_ops = walk_game.disasm(wildlife["kGroupRollCall"] + 4, 16)
    report(group_ops[0].op_str == f"dword ptr [{walk_game.base + 0x1a5b48:#x}]" and group_ops[1].op_str == f"dword ptr [{walk_game.base + 0x1a5b44:#x}]", "and scaled as expected")
    report(abs(struct.unpack_from("<f", walk_image, 0x1a5b48)[0] - 0.95) < 1e-3 and abs(struct.unpack_from("<f", walk_image, 0x1a5b44)[0] - 0.05) < 1e-3, "by 0.95, plus 0.05")
    report(call_at(wildlife["kFireCall"], wildlife["kFire"]), "a shot is fired where expected")
    report(next(i for i in walk_game.disasm(wildlife["kFire"], 0x340) if i.mnemonic == "ret").op_str == "0x14", "with 5 values")
    report(walk_game.disasm(0xc8e0d, 6)[0].op_str == "dword ptr [edi + 0x84]" and walk_game.disasm(0xc8e15, 6)[0].op_str == "dword ptr [edi + 0x88]", "birds appear between the distances in their file")
    report(walk_game.disasm(0xc920e, 3)[0].op_str == f"dword ptr [ebp + {wildlife['kBirdTimer']:#x}]" and walk_game.disasm(0xc9211, 3)[0].op_str == f"dword ptr [ebp + {wildlife['kBirdPeriod']:#x}]", "when a timer reaches their period")
    report(walk_game.disasm(0xc921e, 6)[0].op_str == f"eax, dword ptr [ebp + {wildlife['kBirdMost']:#x}]", "unless there are enough of them")
    report(walk_game.disasm(0xc8570, 3)[0].op_str == f"eax, dword ptr [edi + {wildlife['kBirdChanceCount']:#x}]" and walk_game.disasm(0xc859c, 3)[0].op_str == f"eax, dword ptr [edi + {wildlife['kBirdChances']:#x}]", "and the kind is rolled from a table")
    report(walk_game.disasm(0xbd9cd, 3)[0].op_str == f"esi, dword ptr [ebp + {wildlife['kManagerBirds']:#x}]", "the animal manager holds the birds")
    report(walk_game.disasm(0xbb310, 6)[0].op_str == f"eax, dword ptr [esi + {wildlife['kManagerAreas']:#x}]" and walk_game.disasm(0xbb2e4, 6)[0].op_str == f"eax, dword ptr [esi + {wildlife['kManagerAreaCount']:#x}]", "and the food and water areas")
    report(walk_game.disasm(0xe28f0, 3)[0].op_str == f"eax, dword ptr [ecx + {wildlife['kAreaResource']:#x}]", "whose kind is where expected")
    report(walk_game.disasm(0xbb36e, 5)[0].mnemonic == "call" and walk_game.disasm(0xbb373, 3)[0].op_str == "eax, 1", "with 1 for water")
    report(walk_game.disasm(0xbb389, 3)[0].op_str == "ecx, [ebp + 8]" and "kAreaPosition = 8;" in wildlife_text, "and whose place is where expected")
    report(walk_game.disasm(0xc5811, 6)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalMate']:#x}], eax", "an animal keeps its mate where expected")
    report(walk_game.disasm(0xb5f29, 6)[0].op_str == f"al, byte ptr [ecx + {wildlife['kAnimalHurt']:#x}]", "and whether it's hurt")
    report(walk_game.disasm(0xb5ef2, 6)[0].op_str == f"ecx, dword ptr [esi + {wildlife['kBrainAnimal']:#x}]", "a brain knows its animal")
    weigh_ops = "\n".join(i.op_str for i in walk_game.disasm(0xb66a0, 0x9a))
    for name, what in (("kBrainThirst", "thirst"), ("kBrainTiredness", "tiredness"), ("kBrainHunger", "hunger"), ("kBrainPrey", "the pull of prey"), ("kBrainHunterFear", "fear of the hunter"), ("kBrainEnemyFear", "fear of other animals")):
        report(f"[esi + {wildlife[name]:#x}]" in weigh_ops, f"needs are worked out from {what}")
    with zipfile.ZipFile(game / "base.scs") as base:
        species_text = base.read("def/game.def").decode("latin-1")
        kinds = re.findall(r'\{"(\w+)", \d+, \d+, \d+, \d+, [\d.]+f, (?:true|false), (?:true|false), (?:true|false)\}', wildlife_text)
        game_kinds = re.findall(r"def (\w+):/species_kind", species_text)
        report(sorted(kinds) == sorted(game_kinds), f"all {len(game_kinds)} species have numbers")
        missing_young = [name for name in game_kinds if not re.search(rf'def {name}:/species_kind.*?gender:"3"', species_text, re.S)]
        report(not missing_young, "and all of them have a young type")
        horned_young = re.findall(r'gender:"3"[^}]*?antlers:"([^"]+)"|antlers:"([^"]+)"[^}]*?gender:"3"', species_text)
        horn_files = sorted({(a or b).lstrip("/") for a, b in horned_young})
        horn_names = re.search(r'kYoungHorns\[\] = "simple_beam_part(\d+)"', wildlife_text)
        report(len(horn_files) >= 4 and horn_names.group(1) == "0" and all(".smplbeam0" in base.read(n).decode("latin-1") for n in horn_files), f"all {len(horn_files)} kinds of young with horns have the shape they're given")
        report(species_text.count(':0.3:food"') == 85, "hunters are drawn to carcasses by the amount expected")
        report('effect_range:"400"' in base.read("def/basedefs.def").decode("latin-1"), "from the range expected")
        bird_files = [n for n in base.namelist() if n.endswith("/ambient_animals.sii")]
        bird_lines = ("spawn_frequency_seconds: 7.0f", "max_simultaneous_animals: 5", "min_spawn_distance: 10.0f", "max_spawn_distance: 20.0f")
        report(all(all(line in base.read(n).decode("latin-1") for line in bird_lines) for n in bird_files), f"all {len(bird_files)} bird files say what's expected")
        links = re.findall(r"species_links\[\]: (\S+)", base.read(bird_files[0]).decode("latin-1"))
        report(len(links) == wildlife_bird_kinds(wildlife_text) and links[wildlife_vulture(wildlife_text)] == "ambient.animal.vulture", "and vultures are where expected in their list")

    report(wildlife["kMusicSlot"] == 0x19b098 + 0x30 and struct.unpack_from("<I", walk_image, wildlife["kMusicSlot"])[0] == walk_game.base + 0x2ed0, "the hunt plays its stings through the method expected")
    report(walk_image[0x2f1c:0x2f1f] == bytes.fromhex("c20800"), "which takes two values")
    music_call = walk_game.disasm(0xb2797, 14)
    report(music_call[0].op_str == "0x3f800000" and music_call[1].op_str == "1" and music_call[3].op_str == "dword ptr [edx + 0x30]", "and a hunter starting on an animal plays sting 1")
    report(walk_game.disasm(0xb15ba, 2)[0].op_str == "2", "where one starting on the hunter plays sting 2")
    report(text_at(struct.unpack_from("<I", walk_image, 0x1e315c)[0] - walk_game.base) == "animal_attacks_animal_filename", "and the file names that one")
    weigh_goal = [i for i in walk_game.disasm(0xb66a0, 0x134) if i.mnemonic == "call"]
    report(any(i.op_str == f"{walk_game.base + 0xb4bc0:#x}" for i in weigh_goal), "goals start while needs are being weighed")

    print("--- day and night")
    daytime_text = source("daytime.cpp")
    daytime = constants(daytime_text)
    environment_ops = walk_game.disasm(daytime["kEnvironment"], 12)
    report(environment_ops[0].op_str == f"{walk_game.base + 0x4f50:#x}" and environment_ops[2].op_str == f"{walk_game.base + 0x4f30:#x}", "the environment comes from the hunt")
    report(walk_game.disasm(0x4f30, 6)[0].op_str == "eax, dword ptr [ecx + 0xb0]", "where expected")
    report(walk_game.disasm(0xe6e29, 6)[0].op_str == f"al, byte ptr [esi + {daytime['kEnvironmentFrozen']:#x}]", "its clock can be frozen")
    report(walk_game.disasm(0xe6e3d, 6)[0].op_str == f"dword ptr [esi + {daytime['kEnvironmentClock']:#x}]", "and is in seconds")
    report(walk_game.disasm(0xe6775, 6)[0].op_str == f"dword ptr [{walk_game.base + 0x1a4408:#x}]" and struct.unpack_from("<f", walk_image, 0x1a4408)[0] == 3600.0, "with 3600 to the hour")
    report(walk_game.disasm(0xe6e49, 3)[0].op_str == f"ecx, dword ptr [esi + {daytime['kEnvironmentSun']:#x}]", "the environment holds the sun")
    report(walk_game.disasm(0x153c40, 6)[0].op_str == "dword ptr [ecx + 0x84]" and call_at(0xec960, 0x153c40), "which reads the clock itself")
    report(walk_game.disasm(0xec050, 3)[0].op_str == f"eax, dword ptr [ecx + {daytime['kSunAmbient']:#x}]" and walk_game.disasm(0xec053, 3)[0].op_str == f"edx, dword ptr [ecx + {daytime['kSunAmbientCount']:#x}]", "its ambient light by the hour is where expected")
    report(walk_game.disasm(0xec057, 3)[0].op_str == "edx, 4", "with four floats to an hour")
    report(walk_game.disasm(0xec067, 6)[0].op_str == f"dword ptr [{walk_game.base + 0x1a88c8:#x}]" and abs(struct.unpack_from("<f", walk_image, 0x1a88c8)[0] * 255 - 1) < 1e-3, "scaled down to 0 to 1 as they load")
    report(struct.unpack_from("<I", walk_image, 0x1e3388 - 12)[0] == 0x20, "and that's the file's ambient_levels")
    report(walk_game.disasm(0xec4b9, 6)[0].op_str == f"dword ptr [esi + {daytime['kSunSeen']:#x}]", "how much of the sun shows is where expected")
    report(walk_game.disasm(0xec6d0, 10)[0].op_str == f"dword ptr [esi + {daytime['kSunDazzle']:#x}], 0" and walk_game.disasm(0xec6c6, 10)[0].op_str == f"dword ptr [esi + {daytime['kSunDazzleWanted']:#x}], 0", "and so is its dazzle")
    tick_slots = [int(value, 16) for value in re.findall(r"0x[0-9a-f]+", re.search(r"kTickSlots\[\] = \{([^}]+)\}", wildlife_text).group(1))]
    report(all(struct.unpack_from("<I", walk_image, slot)[0] == walk_game.base + 0xc3f40 for slot in tick_slots), f"all {len(tick_slots)} kinds of animal share one tick")
    report(len(re.findall(re.escape(struct.pack("<I", walk_game.base + 0xc3f40)), bytes(walk_image))) == len(tick_slots), "and that's every one")
    report(next(i for i in walk_game.disasm(0xc3f40, 0x340) if i.mnemonic == "ret").op_str == "4", "which takes one value")
    report(walk_game.disasm(0xc3f5a, 4)[0].op_str == "edi, dword ptr [esp + 0x58]" and walk_game.disasm(0xc4034, 4)[0].op_str == "dword ptr [esp + 0x58]", "the time since the last one, divided by")

    print("--- music")
    report(walk_image[0x93ca2:0x93ca4] == bytes.fromhex("6a00") and walk_game.disasm(0x93b16, 6)[0].op_str == f"dword ptr [{walk_game.base + 0x1a2044:#x}]", "a target within 200 m plays music 0, seen or not")
    report(struct.unpack_from("<f", walk_image, 0x1a2044)[0] == 40000.0, "which is 200 m squared")
    report(walk_image[0xb279c:0xb279e] == bytes.fromhex("6a01"), "an animal going after anything plays music 1")
    report(walk_image[0xb15ba:0xb15bc] == bytes.fromhex("6a02") and walk_game.disasm(0xb1522, 10)[0].op_str == "dword ptr [esi + 0x474], 2", "and one going after the hunter plays music 2")
    attack_goal = int(re.search(r"kAttackHunterGoal = (\d+)", wildlife_text).group(1))
    report(walk_game.disasm(0xb5cf5, 6)[0].op_str == f"dword ptr [ebx], {attack_goal:#x}", "that's the goal danger can start")
    report(walk_game.disasm(0xb5ce7, 6)[0].op_str == f"dword ptr [esi + {wildlife['kBrainAggression']:#x}]" and walk_image[0xb5cf2] == 0x7a, "when a random number comes in under the animal's aggression")
    report(walk_game.disasm(0xb5ed0, 6)[0].op_str == "dword ptr [esi + 0x150]" and abs(struct.unpack_from("<f", walk_image, 0x19bfe0)[0] - 0.8) < 1e-6, "danger always wins past 0.8")
    attack_stub = struct.unpack_from("<I", walk_image, 0xb4d20 + attack_goal * 4)[0] - walk_game.base
    report(walk_game.disasm(attack_stub + 12, 6)[0].op_str == "dword ptr [edx + 0x90]" and all(struct.unpack_from("<I", walk_image, table + 0x90)[0] == walk_game.base + 0xb1500 for table in (0x1a4c70, 0x1a55e8)), "and it's the method that plays the music")

    print("--- water")
    report(call_at(wildlife["kBulletRayCall"], wildlife["kRayCast"]) and struct.unpack_from("<I", walk_image, 0x1a8480)[0] == walk_game.base + 0xd4920, "a bullet's flight casts a ray in its first method")
    report(walk_game.disasm(0xd4527, 3)[0].op_str == "eax, dword ptr [edi + 8]" and walk_image[0xd4531:0xd4534] == bytes.fromhex("c21400"), "which gives back how many things it hit")
    report(walk_game.disasm(0xd4a6c, 2)[0].op_str == "0xa" and walk_game.disasm(0xd4a48, 4)[0].op_str == "dword ptr [esp + 0x38], ebx" and walk_game.disasm(0xd4a94, 4)[0].op_str == "esi, dword ptr [esp + 0x38]", "listed from +4, counted at +8")
    report(walk_game.disasm(0xd4a98, 3)[0].op_str == "eax, 5" and walk_game.disasm(0xd4ab0, 3)[0].op_str == f"ecx, dword ptr [esi + {wildlife['kHitGeom']:#x}]", "0x20 bytes each, with what was hit at +0x1c")
    report(walk_game.disasm(0x153bf0, 3)[0].op_str == f"eax, dword ptr [ecx + {wildlife['kGeomKind']:#x}]", "which says what kind of thing it is")
    water_case = struct.unpack_from("<I", walk_image, 0xd7058 + wildlife_water_geom(wildlife_text) * 4)[0] - walk_game.base
    report(walk_image[water_case:water_case + 2] == bytes.fromhex("6a10") and "water" in text_at(0x1a84cc), "and that kind of hit only throws up water")
    report(call_at(wildlife["kWaterTargetCall"], wildlife["kAreaTarget"]) and walk_game.disasm(0xb1c7b, 6)[0].op_str == f"edx, [esi + {wildlife['kAnimalTarget']:#x}]", "a thirsty animal's target is filled in from the area")
    report(walk_game.disasm(0xbb415, 3)[0].op_str == f"edx, [ebp + {wildlife['kTargetPlace']:#x}]" and walk_game.disasm(0xbb455, 3)[0].op_str == f"dword ptr [ebp + {re.search(r'kTargetNear = (\d+)', wildlife_text).group(1)}], ecx" and walk_image[0xbb45a:0xbb45d] == bytes.fromhex("c20800"), "with its place, and how close counts as there")

    print("--- running and resting")
    report(all(call_at(call, wildlife["kFleeTarget"]) for call in (0xb201b, 0xb20b9)) and walk_image[0xb1ec4:0xb1ec7] == bytes.fromhex("c20800"), "running away picks its place in one function, twice")
    report(abs(struct.unpack_from("<f", walk_image, 0x1a397c)[0] - 45.0) < 1e-6 and abs(struct.unpack_from("<f", walk_image, 0x1a4bfc)[0] - 50.0) < 1e-6, "up to 45 degrees off, 50 to 100 m away")
    report(abs(struct.unpack_from("<f", walk_image, 0x1a4d28)[0] - 250000.0) < 1, "and it stops 500 m from what it's running from")
    flee_step = float(re.search(r"kFleeStep = ([\d.]+)f", wildlife_text).group(1))
    report(walk_game.disasm(0xb1ffe, 10)[0].op_str == "dword ptr [esi + 0x4b0], 0x42dc0000" and walk_game.disasm(0xb1ff4, 10)[0].op_str == "dword ptr [esi + 0x478], 0x41a00000", "running away paths to places from 20 to 110 m off")
    report(walk_game.disasm(0xc1334, 6)[0].op_str == "dword ptr [esi + 0x4b4]" and walk_game.disasm(0xc1350, 10)[0].op_str == "dword ptr [esi + 0x428], 1" and walk_image[0xb2114] == 0, "past that it asks for a new place")
    report(20.0 < flee_step < 110.0, "so a step along a line stays inside that")
    report(walk_game.disasm(0xc57e0, 2)[0].op_str == f"{walk_game.base + 0xc5817:#x}" and walk_game.disasm(0xc57f8, 10)[0].op_str == "dword ptr [esi + 0x43c], 0", "an empty mate is skipped, and an unknown one is none")
    report(walk_game.disasm(0xb5d03, 6)[0].op_str == f"dword ptr [ebx], {wildlife_goal(wildlife_text, 'kFleeGoal'):#x}", "danger runs from the hunter as goal 11")
    report(walk_game.disasm(0xb5d76, 4)[0].op_str == f"dword ptr [ebp + 4], {wildlife_goal(wildlife_text, 'kGoalDone')}", "a goal at stage 3 has another picked")
    report(walk_game.disasm(0xb5f97, 7)[0].op_str == f"dword ptr [ebp], {wildlife_goal(wildlife_text, 'kRestGoal')}" and walk_image[0xb12ff:0xb1301] == bytes.fromhex("6a00") and text_at(struct.unpack_from("<I", walk_image, 0x1e1560)[0] - walk_game.base) == "rest", "tiredness is goal 6, which rests")
    report(walk_game.disasm(0xb5f29, 6)[0].op_str == "al, byte ptr [ecx + 0xbe6]", "being hurt is read as the goal's picked")
    report(call_at(0xb59e2, 0xb8fc0) and walk_game.disasm(0xb8fc0, 3)[0].op_str == f"dword ptr [ecx + {wildlife['kSensesHearing']:#x}]", "hearing is the senses' third number")
    report(walk_game.disasm(0xb5a83, 6)[0].op_str == f"dword ptr [esi + {wildlife['kBrainHearing']:#x}]", "and what it heard is kept in the brain")

    print("--- calls")
    report(walk_game.disasm(wildlife["kPlayCall"] + 3, 6)[0].op_str == "al, byte ptr [esi + 0x381]" and walk_game.disasm(0xae1c1, 7)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalSoundCount']:#x}], 0xc" and walk_game.disasm(0xae1ea, 6)[0].op_str == f"eax, dword ptr [esi + {wildlife['kAnimalSounds']:#x}]", "an animal's call is its 13th sound, played where it is")
    report(walk_game.disasm(wildlife["kCallHeardWithin"] - 2, 6)[0].op_str == f"dword ptr [{walk_game.base + wildlife['kTwoHundredSquared']:#x}]" and walk_game.disasm(0xae345, 6)[0].op_str == "byte ptr [esi + 0x381], al", "if it's within 200 m")
    report(abs(struct.unpack_from("<f", walk_image, wildlife["kTwoHundredSquared"])[0] - 40000.0) < 1, "which is 40000 squared")
    report(walk_game.disasm(0xb04b9, 3)[0].op_str == "ebx, 0xc" and struct.unpack_from("<f", walk_image, wildlife["kCallFadesBy"])[0] == 200.0, "and its sound is heard to 200 m")
    report(walk_game.disasm(0xb6c1c, 2)[0].op_str == f"{int(re.search(r'kCallAnimation = (\w+)', wildlife_text).group(1), 0):#x}" and call_at(0xb6c24, wildlife["kPlayAnimation"]) and call_at(0xb6c2f, wildlife["kPlayCall"]), "answering a call plays its animation and sound")
    report(walk_image[0xb36d6:0xb36d9] == bytes.fromhex("c20c00") and walk_game.disasm(0xb6bec, 6)[0].op_str == f"eax, dword ptr [esi + {wildlife['kBrainNeed']:#x}]", "with three values, unless its need is danger")
    with zipfile.ZipFile(game / "base.scs") as base:
        names = set(base.namelist())
    pointed_at = [path for path in re.findall(r'"(/sounds/[^"]+\.ogg)"', wildlife_text) if "an_rd_" not in path]
    report(all(path.lstrip("/") in names for path in pointed_at), "every sound it points at exists")
    report(not any(name.startswith("sounds/animals/an_rd_") for name in names), "while red deer point at calls that don't")

    print("--- running")
    report(call_at(wildlife["kRootMotionCall"], wildlife["kRootMotion"]) and walk_game.disasm(0xc3fec, 6)[0].op_str == "dword ptr [esi + 0x344]" and walk_image[0x173f74:0x173f77] == bytes.fromhex("c20800"), "an animal moves as far as its animation's root, times its size")
    report(walk_game.disasm(0xc41d0, 6)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalAnimationRate']:#x}]" and walk_game.disasm(0xc531a, 6)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalAnimationRate']:#x}]", "with its animation at a rate set as it's made")
    report(walk_image[wildlife["kTurnCall"] - 2:wildlife["kTurnCall"]] == bytes.fromhex("ff15") and struct.unpack_from("<I", walk_image, wildlife["kTurnCall"])[0] == walk_game.base + wildlife["kSlerpImport"], "turning goes through one call")
    report(walk_game.disasm(0xc4091, 6)[0].op_str == f"ebx, [esi + {wildlife['kAnimalRotation']:#x}]" and walk_game.disasm(0xc4128, 8)[0].op_str == "dword ptr [esp + 0x10], 0x3f800000" and walk_game.disasm(0xc41af, 4)[0].op_str == "dword ptr [esp + 0x64]", "easing its facing round by the seconds passed")
    import_names = {imp.address - walk_game.base: imp.name.decode() for entry in walk_game.pe.DIRECTORY_ENTRY_IMPORT for imp in entry.imports if imp.name}
    report("slerp_generic" in import_names.get(wildlife["kSlerpImport"], ""), "which is slerp_generic")
    def attack_ops(at, count=1):
        return [f"{i.mnemonic} {i.op_str}" for i in walk_game.disasm(at, count * 8)[:count]]
    charge_at = attack_ops(wildlife["kChargeFromUse"] - 2)[0]
    report(charge_at == f"fcomp dword ptr [{walk_game.base + wildlife['kGameChargeFrom']:#x}]" and struct.unpack_from("<f", walk_image, wildlife["kGameChargeFrom"])[0] == 75.0, "an attack charges within 75 m")
    report(attack_ops(0xb2686, 2)[1] == f"fcomp dword ptr [{walk_game.base + 0x1a4bd4:#x}]" and attack_ops(0xb2700) == ["push 3"] and attack_ops(0xb2776) == ["push 4"] and attack_ops(0xb26c1) == [f"fmul dword ptr [{walk_game.base + 0x1a4d2c:#x}]"], "walking slower as it nears, until then, and giving up past 300 m")
    report(attack_ops(0xb0edf) == [f"lea ecx, [esi + {wildlife['kAnimalAnimations']:#x}]"] and f"lea ebx, [esi + {wildlife['kAnimalBrain']:#x}]" in attack_ops(0xb0ee5, 3), "an animal's brain is part of it")
    report(attack_ops(0xc1267) == [f"lea ecx, [esi + {wildlife['kAnimalGoalPlace']:#x}]"] and attack_ops(0xc1298, 5)[1:] == ["fmul dword ptr [esp + 0xc]", "faddp st(1)", "fcom dword ptr [esi + 0x47c]", "fnstsw ax"] and attack_ops(0xc12b3) == ["fcomp dword ptr [esi + 0x4ac]"], "running to a place, it's reached counting height, within the near distance")
    report(attack_ops(0xc11f8) == [f"lea ecx, [esi + {wildlife['kAnimalGoalPlace']:#x}]"] and attack_ops(0xc1229) == ["fcomp dword ptr [esi + 0x4ac]"], "and walking, within 1.1 m")
    report(attack_ops(0xb200d) == [f"lea ecx, [esi + {wildlife['kAnimalGoalPlace']:#x}]"] and attack_ops(0xb20a0) == [f"lea ecx, [esi + {wildlife['kAnimalGoalPlace']:#x}]"], "which is where running away puts its place")
    report(attack_ops(0xb2022, 3) == ["push 0", "mov ecx, esi", f"call {walk_game.base + wildlife['kRunTo']:#x}"] and attack_ops(wildlife["kRunTo"] + 0x36, 2)[0] == "mov dword ptr [ecx + 0x428], 3" and attack_ops(0xc10a4) == ["ret 4"], "and then runs to it, by setting its movement going again")
    report(attack_ops(0xc2c15, 2) == ["fsqrt ", f"fcom dword ptr [{walk_game.base + 0x1a5fb4:#x}]"] and struct.unpack_from("<f", walk_image, 0x1a5fb4)[0] == 90.0 and attack_ops(0xb5a37) == ["push 0x43960000"] and attack_ops(0xb5a5a) == ["push 0x437a0000"], "an animal sees 90 m, smells 300 m and hears 250 m")
    processors = [text_at(struct.unpack_from("<I", walk_image, 0x1e1560 + 4 * i)[0] - walk_game.base) for i in range(8)]
    report(processors[wildlife_goal(wildlife_text, "kAttackProcessor")] == "attack" and processors[wildlife_goal(wildlife_text, "kRunAwayProcessor")] == "run_away", "attacking and running away are its galloping states")
    report(walk_game.disasm(0xb29f7, 3)[0].op_str == "dword ptr [esi + 4], eax", "kept at +4 of what plays its animations")
    report(walk_game.disasm(0xb1fb1, 10)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalMoveMode']:#x}], {wildlife_goal(wildlife_text, 'kFleeMode')}" and walk_game.disasm(0xb1fc2, 6)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalThreat']:#x}], ecx", "running away keeps what it's running from")
    report(walk_game.disasm(0xb1f26, 2)[0].op_str == "eax, dword ptr [ecx]" and walk_game.disasm(0xb1f2e, 3)[0].op_str == "dword ptr [eax + 4]", "which says where it is through its second method")
    report(walk_game.disasm(0xb4a40, 6)[0].op_str == f"dword ptr [edi], {wildlife_goal(wildlife_text, 'kHuntPreyGoal')}", "hunting prey is goal 4")

    print("--- blood and tracks")
    report(call_at(wildlife["kDripCall"], wildlife["kDropBlood"]) and walk_image[0xc1724:0xc1726] == bytes.fromhex("6a00") and walk_image[0xad1d5:0xad1d8] == bytes.fromhex("c20400"), "a wounded animal drips one stain at a time")
    report(walk_game.disasm(0xc16b4, 6)[0].op_str == f"al, byte ptr [esi + {wildlife['kAnimalBleeds']:#x}]", "only while it's bleeding")
    report(walk_game.disasm(0xc16d1, 6)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalBleeding']:#x}]" and walk_game.disasm(wildlife["kDripEveryUse"] - 2, 6)[0].op_str == f"dword ptr [{walk_game.base + wildlife['kTen']:#x}]" and struct.unpack_from("<f", walk_image, wildlife["kTen"])[0] == 10.0, "every 10 over its bleeding, times 2 to 4, seconds")
    report(call_at(wildlife["kHitBloodCall"], wildlife["kDropBlood"]) and walk_image[0xc6541:0xc6543] == bytes.fromhex("6a02"), "and one where it's hit")
    report(call_at(wildlife["kStampCall"], wildlife["kStamp"]) and walk_image[0xf36b0:0xf36b3] == bytes.fromhex("c21c00"), "a print goes down through one call")
    feet = [walk_game.disasm(at, 6)[0].op_str for at in (0xacf15, 0xacf2c, 0xacf63, 0xacf69, 0xad06c, 0xad075, 0xad089)]
    report(feet == [f"dword ptr [esi + {wildlife['kFeetMoving']:#x}]", f"eax, dword ptr [esi + {wildlife['kFeetGround']:#x}]", f"dword ptr [esi + {wildlife['kFeetTo']:#x}]", f"dword ptr [esi + {wildlife['kFeetFrom']:#x}]", f"edx, dword ptr [esi + {wildlife['kFeetScale']:#x}]", f"eax, [esi + {wildlife['kFeetWay']:#x}]", f"ecx, dword ptr [esi + {wildlife['kFeetTrail']:#x}]"], "reading only these of what leaves it")
    report(walk_game.disasm(0xacfc9, 6)[0].op_str == f"dword ptr [esi + {wildlife['kAnimalPrintGap']:#x}]" and walk_game.disasm(0xacf37, 3)[0].op_str == f"eax, {wildlife_goal(wildlife_text, 'kSoftGround')}", "with soft ground among those that take prints")
    report(call_at(0xb05fe, wildlife["kStickers"]) and call_at(0xb0605, wildlife["kStickerNamed"]) and call_at(0xb05f7, wildlife["kCurrentHunt"]), "and a trail found by name")
    with zipfile.ZipFile(game / "base.scs") as base:
        stickers = base.read("def/sticker.def").decode("latin-1")
        basedefs = base.read("def/basedefs.def").decode("latin-1")
        blood_mats = [base.read(f"material/decal/{name}.mat").decode("latin-1") for name in ("blood1", "blood2", "blood3", "nogore_blood1", "nogore_blood2", "nogore_blood3")]
        state = base.read("effect/generic/dry/nc_xx_x_decal_mult.rs").decode("latin-1")
        mult_effect = "effect/generic/dry_tex/dry.tex.fog.nc.decal.mult.rfx" in set(base.namelist())
    report('limits:"50:200:400"' in stickers and "def hunter_trail" in stickers and 'limits:"300:300:600"' in basedefs, "the trails' limits are as expected, and hunter_trail is there")
    report(all("dry.tex.fog.nc.decal.over" in mat for mat in blood_mats) and mult_effect and "dst : zero" in state, "blood is drawn over, unlit, and the multiplied state is there to use")

    print("--- stash")
    stash_text = source("stash.cpp")
    stash = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"constexpr (?:uintptr_t|size_t) (\w+) = (0x[0-9a-fA-F]+|\d+);", stash_text)}
    operand = lambda n: f"{n:#x}" if n > 9 else str(n)  # as capstone writes them
    def first(at):
        code = walk_game.disasm(at, 16)
        return f"{code[0].mnemonic} {code[0].op_str}" if code else ""
    absolute = lambda rva: f"{walk_game.base + rva:#x}"
    report(first(stash["kCurrentGame"]) == f"mov ecx, dword ptr [{absolute(0x1e9d60)}]" and first(stash["kHuntOf"]) == "mov eax, dword ptr [ecx + 0xa8]", "the hunt is the current game's +0xa8")
    report(first(0x929dc) == "lea esi, [ecx + 0x140]" and first(0x92a02) == f"lea edi, [esi + {operand(stash['kHuntActionsHead'] - 0x140)}]" and first(0x929f2) == "mov dword ptr [esp + 0x10], 0xff7fffff" and first(0x92a2e) == "ret 0x10", "an action goes on its list with a hidden return, then 4 more")
    report(first(0x93057) == f"mov ecx, dword ptr [esi + {operand(stash['kActionProvider'])}]" and first(0x9308f) == "call dword ptr [eax + 4]" and first(0x93092) == "fstp dword ptr [esi + 0xc]", "every frame each one's +4 scores it")
    report(first(0x92946) == f"push {absolute(0x8ef30)}" and first(0x8ee62) == "fld dword ptr [edx + 4]" and first(0x8ee65) == "fcomp dword ptr [ecx + 4]" and first(0x8ee6f) == "mov eax, 0xffffffff", "the lowest score at or above 0 goes first")
    report(first(0x93140) == "mov eax, dword ptr [ecx + 0x144]" and first(0x9314a) == f"mov edx, dword ptr [ecx + {stash['kHuntActionsHead']:#x}]" and first(0x9316b) == "call dword ptr [eax + 8]", "and the interact key does the first one's +8")
    report(first(0x13244) == "push 0x8f" and call_at(0x1325d, stash["kHuntOf"]) and call_at(0x13264, 0x93140), "on foot, through the game's own key")
    report(first(0x101d40) == f"mov eax, dword ptr [{absolute(stash['kProfiles'])}]" and first(0x101d45) == "mov eax, dword ptr [eax + 4]", "the profile is the profiles' +4")
    report(first(0x40a78) == f"mov ebp, dword ptr [eax + {stash['kProfileTags']:#x}]" and call_at(0x40ab3, stash["kTakeFromTag"]) and first(0x40acd) == f"or dl, {0x40:#x}" and first(0x40ad0) == f"inc word ptr [ecx + {operand(stash['kUnitUses'])}]", "the trophy room takes each trophy out of its tag, and holds it")
    report(first(0x96df9) == f"cmp ebx, dword ptr [esi + {operand(stash['kListCount'])}]" and first(0x96e24) == f"mov eax, dword ptr [esi + {operand(stash['kListItems'])}]" and first(0x96e3a) == "ret 8" and first(0x97060) == f"mov edx, dword ptr [ecx + {operand(stash['kTagTrophy'])}]", "a tag's trophy is its +4, and taking it out takes where to put it, then which")
    report(first(0x3ff62) == f"mov ebp, dword ptr [eax + {stash['kProfileTrophies']:#x}]" and first(0x3ff7c) == f"lea ecx, [ebp + {stash['kTrophyList']:#x}]" and call_at(0x3ff7f, stash["kKeepTrophy"]) and first(0xf3ea6) == "ret 4", "moving one to the trophy room keeps it in the profile's")
    report(call_at(0x3c994, stash["kSaveProfile"]) and call_at(0x922b1, stash["kSaveProfile"]) and first(0xd246c) == "mov eax, dword ptr [edi + 0xb8]", "the profile's saved closing the room, and during a hunt")
    report(call_at(0x90645, stash["kHudOf"]) and first(0x9064a) == "mov eax, dword ptr [eax + 4]" and first(0x9064d) == "push 0xbf800000" and call_at(0x9065e, stash["kShowMessage"]) and call_at(0x92ffc, stash["kReleaseResource"]), "a message goes up the way the bag's own do")
    with zipfile.ZipFile(game / "base.scs") as base:
        txt = base.read("def/ui/txt.def").decode("latin-1")
        names = set(base.namelist())
        worlds = {n.split("/")[1]: base.read(n).decode("latin-1") for n in names if re.fullmatch(r"maps/\w+/world\.sii", n)}
        hunting = [m for m in ("africa", "alaska", "alberta", "arizona", "bavaria", "colorado", "minnesota", "montana", "oregon", "pennsylvania", "texas")]
        unit_files = {m: re.search(r'world_units_filename: "/([^"]+)"', worlds.get(m, "")) for m in hunting}
        units = {m: base.read(f.group(1)).decode("latin-1") if f and f.group(1) in names else "" for m, f in unit_files.items()}
    numbers = [int(n) for n in re.findall(r"def template(\d+)", txt)]
    report(numbers == list(range(len(numbers))) and "material/ui/ingame/x_claimanimal.mat" in names and "material/ui/ingame/x_ok.mat" in names, f"the HUD's {len(numbers)} labels run in order, so stashing's is {len(numbers)}")
    models = re.findall(r'"(\w+)"', re.search(r"kCampModels\[\] = \{([^}]*)\}", stash_text).group(1))
    camps = {}
    for m, text in units.items():
        descs = {d for d, model in re.findall(r'\nbody_desc : (\S+) \{\s*model_file: "/models/objects/(\w+)/', text) if model in models}
        camps[m] = sum(1 for d in re.findall(r"\nbody_inst : \S+ \{\s*body_desc: (\S+)", text) if d in descs)
    report(all(f for f in unit_files.values()) and all(camps.values()), "every hunting map has a camp: " + ", ".join(f"{m} {c}" for m, c in camps.items()))
    with zipfile.ZipFile(game / "base.scs") as base:
        boxes = {m: struct.unpack_from("<6f", base.read(f"models/objects/{m}/object.pmg"), stash["kGeometryBox"]) for m in models}
    report(all(b[0] <= b[3] and b[1] <= b[4] and b[2] <= b[5] and max(b[3] - b[0], b[5] - b[2]) < 60 for b in boxes.values()), "every camp's model gives its size: " + ", ".join(f"{m} {b[3] - b[0]:.0f}x{b[5] - b[2]:.0f} m" for m, b in boxes.items()))

    print("--- hour names")
    day = constants(source("daytime.cpp"))
    for word, table, read in (("hour", day["kHourNames"], day["kHourNamesRead"]), ("clock", day["kClockNames"], day["kClockNamesRead"])):
        report(walk_game.disasm(read - 3, 7)[0].op_str.endswith(f"[eax*4 + {walk_game.base + table:#x}]"), f"the {word} names are read in one place")
        last = struct.unpack_from("<I", walk_image, table + 21 * 4)[0] - walk_game.base
        report(text_at(last) == f"@@g_{word}_21@@@@g_{word}_22@@@@g_{word}_23@@", "and 21 to 23 are one string")
    with zipfile.ZipFile(game / "base.scs") as base:
        local = base.read("script/en/local.def").decode("latin-1")
    report(all(f"g_hour_{hour:02}:" in local and f"g_clock_{hour:02}:" in local for hour in range(24)), "the text for every hour exists")

    print("--- night")
    report(call_at(wildlife["kSensesCall"], wildlife["kSenses"]) and walk_game.disasm(0xb6861, 2)[0].op_str == "ecx, esi", "an animal senses the hunter in one call")
    report(walk_game.disasm(0xb59ce, 3)[0].op_str == f"edi, {wildlife['kManagerSenses']:#x}", "from the manager's numbers for what can be sensed")
    report(call_at(0xb59ed, 0xb8f80) and walk_game.disasm(0xb8f80, 3)[0].op_str == f"dword ptr [ecx + {wildlife['kSensesSmell']:#x}]", "smell is one")
    report(call_at(0xb59f8, 0xb8fa0) and walk_game.disasm(0xb8fa0, 3)[0].op_str == f"dword ptr [ecx + {wildlife['kSensesSight']:#x}]", "and sight the next")
    report(call_at(0xb5a11, 0xe6020) and walk_game.disasm(0xe6020, 6)[0].op_str == "dword ptr [ecx + 0x94]", "which the game scales by the light level")
    report(walk_game.disasm(0xb5a45, 6)[0].op_str == f"dword ptr [esi + {wildlife['kBrainSight']:#x}]", "and keeps in the brain")
    report(walk_game.disasm(0xb671c, 6)[0].op_str == f"dword ptr [esi + {wildlife['kBrainLure']:#x}]", "a mating lure's pull goes into what the animal wants")
    report(walk_game.disasm(0xb4b1b, 6)[0].op_str == f"dword ptr [esi + {wildlife['kBrainLure']:#x}]" and walk_game.disasm(0xb4b2a, 6)[0].op_str == f"dword ptr [edi], {wildlife_lure_goal(wildlife_text)}" and walk_game.disasm(0xb4b30, 7)[0].op_str == "dword ptr [edi + 8], 1", "beating a random number starts walking to it")
    report(walk_game.disasm(0xb0fd6, 7)[0].op_str == "ecx, [esi + eax*4 + 0x44c]" and wildlife["kAnimalLurePlace"] == 0x44c + 12, "the mating lure's place is the second of two")
    report(struct.unpack_from("<I", walk_image, 0xb4d20 + 3 * 4)[0] == walk_game.base + 0xb4c03 and walk_game.disasm(0xb4c0f, 3)[0].op_str == "dword ptr [edx + 0x78]", "that goal is an animal's method")
    report(all(struct.unpack_from("<I", walk_image, table + 0x78)[0] == walk_game.base + 0xb0f20 for table in (0x1a4c70, 0x1a55e8)), "and both kinds of animal share it")
    report(walk_game.disasm(0xb6742, 6)[0].op_str == f"eax, dword ptr [esi + {wildlife['kBrainGoal']:#x}]" and walk_game.disasm(0xb6792, 6)[0].op_str == f"edx, dword ptr [esi + {wildlife['kBrainGoalStage']:#x}]", "the goal and its stage are in the brain")

    print("--- lights")
    lights_text = source("lights.cpp")
    lights = constants(lights_text)
    daytime_curfew = constants(source("daytime.cpp"))["kCurfewCheck"]
    curfew = walk_game.disasm(daytime_curfew, 6)[0]
    report(curfew.mnemonic == "jne" and curfew.op_str == f"{walk_game.base + 0x93d11:#x}", "the game checks the hour before calling a hunt off")
    curfew_bytes = {
        name: bytes(int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]{2}", re.search(name + r"\[\] = \{(.*?)\}", source("daytime.cpp")).group(1)))
        for name in ("kCurfewOn", "kCurfewOff")
    }
    report(walk_image[daytime_curfew:daytime_curfew + len(curfew_bytes["kCurfewOn"])] == curfew_bytes["kCurfewOn"], "with these exact bytes")
    report(curfew_bytes["kCurfewOff"][1:] == curfew_bytes["kCurfewOn"][1:] and curfew_bytes["kCurfewOff"][0] == 0xeb, "and the same jump always taken instead")
    report(walk_game.disasm(0x93cd8, 6)[0].op_str == f"dword ptr [{walk_game.base + 0x1a01c8:#x}]" and struct.unpack_from("<f", walk_image, 0x1a01c8)[0] == 21.0, "against 21:00")
    report(text_at(0x1a2014) == "@@g_lc_huntoverquestion@@" and walk_game.disasm(0x93cef, 5)[0].op_str == f"{walk_game.base + 0x1a2014:#x}", "and that's the question it asks")
    code_part = next(part for part in walk_game.pe.sections if part.Characteristics & 0x20000000)
    code_start = code_part.VirtualAddress
    code_end = code_start + code_part.Misc_VirtualSize - 5
    def calls_to(target):
        return {at for at in range(code_start, code_end) if walk_image[at] == 0xe8 and at + 5 + struct.unpack_from("<i", walk_image, at + 1)[0] == target}
    def listed(name):
        return {int(call, 16) for call in re.findall(r"0x[0-9a-f]+", re.search(name + r"\[\] = \{(.*?)\}", lights_text).group(1))}
    report(calls_to(lights["kChooseItem"]) == listed("kChooseItemCalls") | {0xde1c}, "every way of choosing an item is caught")
    report(calls_to(lights["kUseItem"]) == listed("kUseItemCalls") | {0xddf3}, "and of using the selected one")
    report(walk_image[0xd92d:0xd930] == bytes.fromhex("c20400"), "choosing takes one value")
    report(walk_image[0xd870] == 0xc3, "and using takes none")
    report(call_at(0xd927, lights["kSelectItem"]) and walk_image[0xa126f:0xa1271] == bytes.fromhex("b001"), "choosing selects the item by name")
    report(walk_game.disasm(0xa2507, 3)[0].op_str == f"eax, [ecx + {lights['kItemName']:#x}]" and walk_game.disasm(0xa27fb, 3)[0].op_str == "ebp, [esi + 0x2c]", "and an item's name is its def's")
    report(walk_game.disasm(0xd8e3, 3)[0].op_str == f"eax, dword ptr [esi + {lights['kHunterWaiting']:#x}]" and walk_game.disasm(0xd8ea, 6)[0].op_str == f"al, byte ptr [esi + {lights['kHunterHasHands']:#x}]", "after checking the hunter isn't busy")
    report(walk_game.disasm(0xcfe3, 6)[0].op_str == f"ecx, dword ptr [esi + {lights['kHunterItems']:#x}]" and walk_game.disasm(0xcfe9, 3)[0].op_str == f"eax, dword ptr [ecx + {lights['kItemsSelected']:#x}]", "and what's selected")
    report(walk_game.disasm(0xcff0, 3)[0].op_str == f"al, byte ptr [eax + {lights['kItemFlags']}]" and walk_image[0xcff3:0xcff6] == bytes.fromhex("c0e807"), "still exists")
    report(walk_game.disasm(0xa24b0, 3)[0].op_str == f"eax, dword ptr [ecx + {lights['kItemState']:#x}]" and walk_game.disasm(0xd014, 3)[0].op_str == "eax, " + re.search(r"kItemDown = (\d+)", lights_text).group(1), "and is down")
    report(walk_game.disasm(0xd90b, 7)[0].op_str == "dword ptr [esi + 0x58], 2", "and left alone, it waits for that to go away")
    click = walk_game.disasm(0x16516, 7)[0]
    report(click.op_str == f"dword ptr [esp], {walk_game.base + 0x19c818:#x}" and text_at(0x19c818) == "/sounds/ui/click.ogg" and call_at(0x16529, lights["kPlaySound"]), "the menu plays a sound by its path")
    report(walk_image[0x1648d:0x16490] == bytes.fromhex("c20400"), "which is one value")
    report(walk_game.disasm(lights["kPlaySound"], 6)[0].op_str == f"al, byte ptr [ecx + {lights['kMenuQuiet']:#x}]", "and it only wants a menu to see if it's quiet")
    report(walk_game.disasm(0xe6050, 6)[0].op_str == f"eax, [ecx + {lights['kEnvironmentAmbient']:#x}]" and walk_game.disasm(0xe6060, 6)[0].op_str == f"eax, [ecx + {lights['kEnvironmentDiffuse']:#x}]", "the light right now is in the environment")
    view_text = source("view.cpp")
    world_call = constants(view_text)["kWorldDrawCall"]
    report(world_call == 0x102f99 and call_at(0x102f99, 0x1560a0) and call_at(0x102fea, 0x1560a0), "the world is drawn before the weapon, in a call of its own")
    with zipfile.ZipFile(game / "base.scs") as base:
        names = set(base.namelist())
        stock_equipment = base.read("def/equipment.def").decode("latin-1")
        stock_map = re.search(r"def map:eqitem_base \{(.*?)\}", stock_equipment, re.S).group(1)
        items_text = re.search(r"kLightItems\[\] =(.*?);\s*\n", lights_text, re.S).group(1)
        for key in ("script", "pba", "category"):
            value = re.search(rf'{key}:"([^"]+)"', stock_map).group(1)
            report(items_text.count(f'{key}:\\"{value}\\"') == 2, f"both lights match the map's {key}")
        for icon in set(re.findall(r'(?:icon|glow):\\"/([^\\]+)\\"', items_text)):
            report("i_predatorcall" in icon and icon in names, f"{icon} exists")
        report(not re.search(r"^def (flashlight|lantern):", stock_equipment, re.M), "and neither name is taken")
        report(re.search(r'kSwitchSound\[\] = "/([^"]+)"', lights_text).group(1) in names, "the switch's sound exists")

    print("--- freelook")
    freelook_text = source("freelook.cpp")
    freelook = constants(freelook_text)
    foot_word = struct.unpack_from("<I", walk_image, freelook["kFootViewClass"] + 0x14)[0]
    report(foot_word == walk_game.base + 0x1e9db0, "the view on foot has the class expected")
    frustum_update = walk_game.disasm(freelook["kUpdateFrustum"], 0x12)
    report(frustum_update[0].op_str == f"eax, [ecx + {freelook['kPlaceRotation']:#x}]" and frustum_update[1].op_str == "edx, [ecx + 0x20]", "a view's frustum comes from its position and rotation")
    report(walk_game.imports[("p3core.dll", "?update@frustum_t@prism@@QAEXABV?$vec_t@M$02@2@ABVquat_t@2@@Z")] == walk_game.base + 0x19a4ec, "through the engine's frustum update")
    report(walk_game.disasm(0x12ba2, 6)[0].op_str == f"dword ptr [esi], {walk_game.base + 0x19c240:#x}", "the view on foot has the table of methods expected")
    report(freelook["kFollowBodySlot"] == 0x19c240 + 0x30 and struct.unpack_from("<I", walk_image, freelook["kFollowBodySlot"])[0] == walk_game.base + 0x12c00, "and catches up with the body in the method expected")
    report(walk_game.disasm(0x12c52, 3)[0].op_str == "dword ptr [edx + 0x40]" and walk_game.disasm(0x12c57, 3)[0].op_str == f"edi, [esi + {freelook['kPlaceRotation']:#x}]", "which is where it takes the body's rotation")
    report(call_at(0x12d14, freelook["kUpdateFrustum"]) and walk_game.disasm(0x12d1f, 1)[0].mnemonic == "ret", "and ends by updating the frustum, taking no values")
    camera_calls = walk_game.disasm(0x5db5, 12)
    report(camera_calls[0].op_str == "dword ptr [eax + 0x50]" and camera_calls[-1].op_str == "dword ptr [edx + 0x30]", "the world draw has the camera catch up as it starts")
    report(struct.unpack_from("<I", walk_image, 0x19b098 + 0x50)[0] == walk_game.base + 0x30d0, "with the hunt's camera")
    report(call_at(0x5e1c, 0x5580), "before it sets up the camera for drawing")
    shot_view = walk_game.disasm(0xa74b8, 13)
    report(shot_view[0].op_str == "dword ptr [edx + 0x58]" and shot_view[3].op_str == "dword ptr [edx + 0x50]" and shot_view[4].op_str == "eax, 0x20", "a shot takes its place from the player's view")
    report(walk_game.disasm(0xabaf3, 3)[0].op_str == "dword ptr [edx + 0x50]" and walk_game.disasm(0xabaf6, 3)[0].op_str == f"ecx, dword ptr [eax + {freelook['kPlaceRotation']:#x}]", "and the gun takes its rotation from the camera")
    report(walk_game.disasm(0x131a2, 3)[0].op_str == "dword ptr [eax + 0x40]" and walk_game.disasm(0x13224, 3)[0].op_str == "dword ptr [ebx + 0x48]", "while turning starts from the hunter's own rotation, so the head never reaches the body")
    for name in re.findall(r'constexpr char kAttitude\w+\[\] = "([^"]+)";', freelook_text):
        report(name in bins["p3core.dll"].exports, f"p3core exports {name[:24]}")

    print("--- tree stand")
    stand_text = source("stand.cpp")
    stand = constants(stand_text)
    report(call_at(stand["kSetSmellCall"], stand["kSetSmell"]), "the hunter's smell is set where expected")
    report(call_at(stand["kSetSightCall"], stand["kSetSight"]), "and so is their sight")
    ladder_word = struct.unpack_from("<I", walk_image, stand["kLadderViewClass"] + 0x14)[0]
    report(ladder_word == walk_game.base + 0x1e9db4, "the view on a ladder has the class expected")
    report(any(f"[esi + {stand['kLadderViewOffset']:#x}]" in i.op_str for i in walk_game.disasm(0x139cd, 6)), "and holds its view offset where expected")
    report(text_at(struct.unpack_from("<I", walk_image, 0x1e28e4)[0] - walk_game.base) == "view_offset" and struct.unpack_from("<I", walk_image, 0x1e28d8)[0] == 0x40, "a ladder top's view offset is where expected")
    report(struct.unpack_from("<f", walk_image, 0x1a96b8)[0] == 0.0, "and it's 0 unless a script says otherwise")
    with zipfile.ZipFile(game / "base.scs") as base:
        offsets = {}
        for name in base.namelist():
            if name.endswith(".sii"):
                data = base.read(name).decode("latin-1")
                if "ladder_top" in data:
                    found = re.search(r"view_offset\s*:\s*([\d.]+)", data)
                    offsets[name] = float(found.group(1)) if found else 0.0
        report(offsets.pop("script/item/treestand.sii", None) == 0.0, "the tree stand gives no view offset")
        report(len(offsets) >= 4 and all(value > 0.0 for value in offsets.values()), f"and all {len(offsets)} other stands and ladders do")
        leaf_line = re.search(r'kLeafAlpha\[\] = "([^"]+)"', stand_text).group(1)
        leaf_source = re.search(r'kLeafSource\[\] = "([^"]+)"', stand_text).group(1)
        leaf_program = base.read("effect/hu4/dif_opac/arb2.fp").decode("latin-1")
        report(leaf_program.count(leaf_line) == 1, "the leaf program has the line that's changed")
        report(leaf_source in leaf_program, "and says what it's built from")

    print("--- calculator and range sheet")
    paper_text = source("paper.cpp")
    paper = constants(paper_text)
    map_table = walk_game.base + 0x1a2690
    report(walk_game.disasm(0x97829, 6)[0].op_str == f"dword ptr [esi], {map_table:#x}", "the map item has the table of methods expected")
    report(paper["kMapInitSlot"] == 0x1a2690 + 0x30 and struct.unpack_from("<I", walk_image, paper["kMapInitSlot"])[0] == walk_game.base + 0x97530, "which is where it reads its definition")
    report(walk_image[0x976bf:0x976c2] == bytes.fromhex("c20400"), "and that takes one value")
    report(walk_game.disasm(0x975ab, 5)[0].op_str == f"{walk_game.base + 0x1a267c:#x}" and text_at(0x1a267c) == "use_sound", "the definition, as it reads use_sound from it")
    report(paper["kMapStageSlot"] == 0x1a2690 + 0x58 and struct.unpack_from("<I", walk_image, paper["kMapStageSlot"])[0] == walk_game.base + 0x972c0, "and where it's told about raising and lowering")
    report(walk_image[0x97321:0x97324] == bytes.fromhex("c20800"), "and that takes two values")
    report(walk_game.disasm(0x972c4, 3)[0].op_str == "eax, 1", "stage 1 is what shows and hides the window")
    report(walk_game.imports[("p3shared.dll", "?ui_wshow@prism@@YAXPAVc_item_t@1@I@Z")] == walk_game.base + 0x19ab00, "with the engine's window show")
    report(walk_game.imports[("p3shared.dll", "?ui_whide@prism@@YAXPAVc_item_t@1@@Z")] == walk_game.base + 0x19aaa0, "and hide")
    report(struct.unpack_from("<I", walk_image, 0x1a2690 + paper["kMethodLower"])[0] == walk_game.base + 0xa2b80, "lowering an item is the method expected")
    report(walk_game.disasm(0xa2ba0, 3)[0].op_str == "eax, 1", "which only acts on one that's raised")
    report(walk_game.disasm(0xa29c1, 3)[0].op_str == f"dword ptr [eax + {paper['kMethodLower']:#x}]", "and takes no values")
    report(walk_game.disasm(0x9764c, 6)[0].op_str == f"ecx, dword ptr [{walk_game.base + paper['kGameState']:#x}]", "the game's state is where expected")
    report(call_at(0x24438, paper["kProfileSection"]), "a profile's sections are found where expected")
    report(text_at(0x19cc98) == "free_hunt" and text_at(0x19cee0) == "weapon", "with the free hunt loadout under the names expected")
    for slot, name in ((paper["kGetValue"], "?get_value@def_obj_t@prism@@QBEPBDQBDI@Z"), (paper["kGetBool"], "?get_bool@def_obj_t@prism@@QBE_NQBDI@Z"),
                       (paper["kGetInt"], "?get_int@def_obj_t@prism@@QBEHQBDI@Z"), (paper["kFindInPool"], "?find@def_pool_t@prism@@QBEPAVdef_obj_t@2@PBDI_N@Z")):
        report(walk_game.imports[("p3shared.dll", name)] == walk_game.base + slot, f"{name[1:name.index('@@')]} is in the slot expected")
    translate = [name for dll, name in walk_game.imports if "lcl_translate_string" in name]
    report(len(translate) == 1 and walk_game.imports[next(key for key in walk_game.imports if key[1] == translate[0])] == walk_game.base + paper["kTranslate"], "text is translated through the slot expected")
    report(walk_game.disasm(0xa8c37, 3)[0].op_str == "eax, 5" and walk_game.disasm(0xa8c55, 5)[0].op_str == "eax, 0x12c", "a gun's zero range is kept between 5 and 300")
    report(walk_game.disasm(0x180a, 2)[0].op_str == "1", "files are opened from the realm expected")

    solver_start = walk_game.disasm(paper["kSolverStart"], 0x39)
    solver_text = "\n".join(i.op_str for i in solver_start)
    report("dword ptr [eax + 4], 0x3f000000" in solver_text, "the solver starts with a ballistic coefficient of 0.5")
    report("dword ptr [eax + 8], 0x453b8000" in solver_text, "and a muzzle speed of 3000")
    report("dword ptr [eax + 0xc], 0x43160000" in solver_text, "and a bullet of 150 grains")
    report("dword ptr [eax + 0x18], 0x3fc00000" in solver_text, "and sights 1.5 above the bore")
    report(solver_start[-1].mnemonic == "ret", "and takes no values to start")
    report(call_at(0xa6857, paper["kSolve"]), "the game solves its sights where expected")
    report(walk_game.disasm(paper["kSolverEnd"], 3)[0].op_str == "ecx, 0x20", "and clears up after")
    report(paper["kSolverElevation"] == 0x10 and paper["kSolverSize"] == 0x30, "the answer is where expected")

    with zipfile.ZipFile(game / "base.scs") as base:
        equipment = base.read("def/equipment.def").decode("latin-1")
        names = set(base.namelist())
        game_defs = base.read("def/game.def").decode("latin-1")
        stock_map = re.search(r"def map:eqitem_base \{(.*?)\}", equipment, re.S).group(1)
        new_items = re.search(r"kNewItems\[\] =(.*?);\s*\n", paper_text, re.S).group(1)
        for key in ("script", "pba", "camo_look", "area_variant", "use_sound", "category"):
            value = re.search(rf'{key}:"([^"]+)"', stock_map).group(1)
            report(new_items.count(f'{key}:\\"{value}\\"') == 2, f"both items match the map's {key}")
        for icon in re.findall(r'(?:icon|glow):\\"/([^\\]+)\\"', new_items):
            report(icon in names, f"{icon} exists")
        report('class00:"eqitem_base"' in game_defs, "items of that kind are listed in the loadout")
        report(re.search(r"def eqitems \{.*?def misc \{", game_defs, re.S) is not None, "with a misc category")
        report(not re.search(r"^def (calculator|range_sheet):", equipment, re.M), "and neither name is taken")

        # The equipment grid: 68 unit cells with a gap of 6, in 4 columns, and rows that start from the top.
        layout_text = source("layout.cpp")
        grid_layout = base.read(re.search(r'kGearGrid\[\] = "([^"]+)"', layout_text).group(1)).decode("latin-1")
        grid_coords = {key: re.search(rf'{key}\[\] = "\\"coords\\" : \\"([\d ]+)\\""', layout_text).group(1) for key in ("kGearListWas", "kGearList", "kGearHeadingWas", "kGearHeading")}
        report(grid_layout.count(f'"coords" : "{grid_coords["kGearListWas"]}"') == 1, "the equipment list is where expected")
        report(grid_layout.count(f'"coords" : "{grid_coords["kGearHeadingWas"]}"') == 1, "and so is its heading")
        list_values = re.search(r'"item_gap" : "([\d.]+)".*?"item_width" : "([\d.]+)".*?"item_height" : "([\d.]+)"', grid_layout, re.S)
        cell_gap, cell_width, cell_height = (float(value) for value in list_values.groups())
        report((cell_gap, cell_width, cell_height) == (6.0, 68.0, 68.0), "its cells are 68 units with gaps of 6")
        frame = (16, 69, 337, 673)
        report(grid_layout.count('"coords" : "16 69 337 673"') == 1, "and its frame is where expected")
        left, bottom, right, top = (int(value) for value in grid_coords["kGearList"].split())
        old_left, old_bottom, old_right, old_top = (int(value) for value in grid_coords["kGearListWas"].split())
        report((left, right) == (old_left, old_right), "the list stays 4 columns wide")
        report(frame[1] <= bottom and top <= frame[3], "and stays inside its frame")
        rows = (top - bottom + cell_gap) // (cell_height + cell_gap)
        report(rows == 8 and (top - bottom) // (cell_height + cell_gap) == 8, "with room for exactly 8 rows")
        report((old_top - old_bottom) // (cell_height + cell_gap) == 7, "where it had 7")
        stock_parents = dict(re.findall(r"(?m)^\s*def (\w+):(\w+)", base.read("def/basedefs.def").decode("latin-1")))
        stock_parents.update(re.findall(r"(?m)^def (\w+):(\w+)", equipment))

        def is_gear(name):
            while name:
                if name == "eqitem_base":
                    return True
                name = stock_parents.get(name)
            return False

        gear_count = sum(is_gear(name) for name in re.findall(r"(?m)^def (\w+):", equipment))
        report(gear_count == 28, f"the game has {gear_count} gear items, which is what filled it")
        report(gear_count + 4 <= 4 * rows, "and all of them fit with the four added")
        heading = [int(value) for value in grid_coords["kGearHeading"].split()]
        report(heading[1] >= frame[3] and heading[3] <= 768, "the heading sits between the frame and the top of the screen")
        ammo_files = sorted(set(re.findall(r'^\s*ammo:"([^"]+\.sii)"', equipment, re.M)))
        bad_ammo = []
        for path in ammo_files:
            text = base.read(path.lstrip("/")).decode("latin-1")
            for key in ("bc", "muzzle", "weight"):
                if len(re.findall(rf"^\s*{key}\s*:\s*[\d.]+", text, re.M)) != 1:
                    bad_ammo.append((path, key))
        report(len(ammo_files) > 10 and not bad_ammo, f"all {len(ammo_files)} kinds of ammo give each number once")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
