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

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
