"""Checks Longshot's layout rewrites against the real files in a game install, without running it.

Extracts the layouts to a temporary folder, has the self-test rewrite them for each screen shape and
HUD setting, then confirms each result is still sound. Build Longshot first.

    python tools/check_layouts.py <game folder>
"""
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCOPES = ["gscope", "gscope_r1", "gscope_r1x", "gscope_r2", "gscope_r2x", "gscope_r3", "gscope_r3x",
          "gscope_r4", "gscope_r4x"]
LAYOUTS = [f"ui/{name}.esc" for name in SCOPES + ["gbinoc", "geyes", "gflash", "ghud"]]
TEMPLATES = "def/ui/txt.def"
ASPECTS = {"16x10": 16 / 10, "16x9": 16 / 9, "ultrawide": 3440 / 1440, "32x9": 32 / 9}
HUDS = {0: "edges", 1: "16:9", 2: "4:3"}

failures = 0


def report(ok, what):
    global failures
    if not ok:
        print(f"FAIL  {what}")
        failures += 1


def parse(text):
    """Every entity in a layout, in file order, with its values and parent."""
    entities, stack = [], []
    for raw in text.splitlines():
        line = raw.strip()
        if line.endswith("{"):
            match = re.match(r'entity "([^"]+)" \{', line)
            if match:
                owners = [x for x in stack if x is not None]
                entity = {"type": match.group(1), "values": {}, "parent": owners[-1] if owners else None}
                entities.append(entity)
                stack.append(entity)
            else:
                stack.append(None)
        elif line == "}":
            stack.pop()
        else:
            match = re.match(r'"(\w+)" : "(.*)"$', line)
            if match:
                owner = [x for x in stack if x is not None][-1]
                owner["values"].setdefault(match.group(1), match.group(2))
    report(not stack, "braces balance")
    return entities


def coords(entity):
    text = entity["values"].get("coords")
    return [int(x) for x in text.split()] if text else None


def screen(aspect):
    visible = 768 * aspect
    return (1024 - visible) / 2, (1024 + visible) / 2


def by_id(entities, wanted):
    return next(e for e in entities if e["values"].get("id") == str(wanted))


def inside(entities, parent):
    found = []
    for entity in entities:
        walker = entity
        while walker is not None:
            if walker is parent:
                found.append(entity)
                break
            walker = walker["parent"]
    return found


def check_window(entities, label):
    window = entities[0]
    report(coords(window) == [0, 0, 1024, 768], f"{label}: the window stays at 0,0")
    report(window["values"].get("clip_children") == "false", f"{label}: the window stops clipping")


def check_scope(before, after, aspect, label):
    check_window(after, label)
    left, right = screen(aspect)
    bars = [e for e in after if "FF000000" in e["values"].get("text", "")]
    report(len(bars) == 2, f"{label}: two black bars")
    report(len(after) == len(before) + 2, f"{label}: nothing else added or lost")
    for bar in bars:
        at = coords(bar)
        report(at[1] == 0 and at[3] == 768, f"{label}: bars stay in the window's height")
    spans = sorted((coords(e)[0], coords(e)[2]) for e in bars)
    report(spans[0][0] <= left and spans[0][1] >= 0, f"{label}: left bar covers the left side")
    report(spans[1][0] <= 1024 and spans[1][1] >= right, f"{label}: right bar covers the right side")


def check_binoculars(before, after, aspect, label):
    check_window(after, label)
    left, right = screen(aspect)
    report(len(after) == len(before), f"{label}: nothing added or lost")
    for old, new in zip(before, after):
        if old is before[0]:
            continue
        was, now = coords(old), coords(new)
        if "binocs_corner" in new["values"].get("text", ""):
            report(now[1] == was[1] and now[3] == was[3], f"{label}: mask height unchanged")
            if was[0] == 0:
                report(now[0] <= left and now[2] == 512, f"{label}: left half reaches the edge")
            else:
                report(now[0] == 512 and now[2] >= right, f"{label}: right half reaches the edge")
        else:
            report(was == now, f"{label}: reticle unchanged")


def check_eyes(after, templates, aspect, label):
    check_window(after, label)
    left, right = screen(aspect)
    half = [int(x) for x in re.findall(r"lid_top\.mat[^>]*?width=(\d+)", templates)]
    report(len(half) == 4 and len(set(half)) == 1, f"{label}: all lid images get one width")
    for lid in after[1:]:
        at = coords(lid)
        report(at[0] <= left and at[2] >= right, f"{label}: lids reach both edges")
        report(at[2] - at[0] == 2 * half[0], f"{label}: two lid images exactly fill the width")


def check_flash(after, aspect, label):
    check_window(after, label)
    left, right = screen(aspect)
    at = coords(after[1])
    report(at[0] <= left and at[2] >= right, f"{label}: flash covers the screen")


def is_strip(entity):
    groups = {"5", "6", "11"}
    text = entity["values"].get("text", "")
    return entity["values"].get("id") in groups or "top.mat" in text or "bottom.mat" in text


def check_strip(entity, old, aspect, label):
    left, right = screen(aspect)
    now = coords(entity)
    report(now[1] == old[1] and now[3] == old[3], f"{label}: strip keeps its height")
    report(now[0] <= left and now[2] >= right, f"{label}: strip reaches both edges")
    text = entity["values"].get("text", "")
    if text:
        widths = {int(x) for x in re.findall(r"width=(\d+)", text)}
        report(widths == {now[2] - now[0]}, f"{label}: strip images fill it exactly")


def check_hud(before, after, aspect, hud, label):
    check_window(after, label)
    report(len(after) == len(before), f"{label}: nothing added or lost")
    left, right = screen(aspect)
    if hud == 0:
        area = (left, right)
    elif hud == 1:
        wide = 768 * 16 / 9
        area = (max(left, (1024 - wide) / 2), min(right, (1024 + wide) / 2))
    else:
        area = (0, 1024)

    was = dict(zip(map(id, after), before))
    expected = {}
    for group, dx in ((88, round(area[0])), (86, round(area[0])), (40, round(area[1]) - 1024)):
        for entity in inside(after, by_id(after, group)):
            expected[id(entity)] = dx

    equipment = by_id(after, 31)
    bar = coords(equipment)
    report(bar[0] >= 0 and bar[2] <= 1024, f"{label}: equipment bar still slides up")
    report(bar[1] == -124 and bar[3] == 0, f"{label}: equipment bar height unchanged")
    if hud == 2:
        report(bar[0] == 382, f"{label}: equipment bar stays")
    else:
        report(abs(bar[0] + bar[2] - 1024) <= 1, f"{label}: equipment bar is centred")
    bar_shift = bar[0] - coords(was[id(equipment)])[0]
    for entity in inside(after, equipment):
        expected[id(entity)] = bar_shift

    for entity in after[1:]:
        old, now = coords(was[id(entity)]), coords(entity)
        if old is None or now is None:
            report(old == now, f"{label}: entity kept its coords")
        elif is_strip(entity):
            check_strip(entity, old, aspect, label)
        else:
            dx = expected.get(id(entity), 0)
            report(now == [old[0] + dx, old[1], old[2] + dx, old[3]],
                   f"{label}: {entity['values'].get('name')} moved by {dx}, got {now} from {old}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    game = Path(sys.argv[1])
    launcher = ROOT / "dist" / "longshot.exe"
    selftest = ROOT / "dist" / "longshot_selftest.exe"

    with zipfile.ZipFile(game / "base.scs") as archive, tempfile.TemporaryDirectory() as folder:
        work = Path(folder)
        sources = {}
        lines = []
        for index, path in enumerate(LAYOUTS + [TEMPLATES]):
            name = f"file{index}"
            data = archive.read(path)
            sources[path] = (name, data.decode("latin-1"))
            (work / name).write_bytes(data)
            lines.append(f"{name} /{path}")
        (work / "list.txt").write_text("\n".join(lines) + "\n")

        result = subprocess.run(
            [str(launcher), "--test-target", str(selftest)],
            env={**os.environ, "LONGSHOT_LAYOUT_DIR": str(work)},
        )
        report(result.returncode == 0, f"self-test passes (exit code {result.returncode})")

        def rewritten(path, aspect, hud):
            return (work / f"{sources[path][0]}.{aspect}.{hud}").read_bytes().decode("latin-1")

        checked = 0
        for aspect_name, aspect in ASPECTS.items():
            for hud, hud_name in HUDS.items():
                label = f"{aspect_name} {hud_name}"
                for path in LAYOUTS:
                    text = rewritten(path, aspect_name, hud)
                    report(bool(text), f"{label}: {path} rewritten")
                    if not text:
                        continue
                    before = parse(sources[path][1])
                    after = parse(text)
                    file_label = f"{label} {path}"
                    if "gscope" in path:
                        check_scope(before, after, aspect, file_label)
                    elif "gbinoc" in path:
                        check_binoculars(before, after, aspect, file_label)
                    elif "geyes" in path:
                        check_eyes(after, rewritten(TEMPLATES, aspect_name, hud), aspect, file_label)
                    elif "gflash" in path:
                        check_flash(after, aspect, file_label)
                    else:
                        check_hud(before, after, aspect, hud, file_label)
                    checked += 1

    print(f"{checked} rewritten files checked, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
