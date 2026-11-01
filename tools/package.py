"""Builds the release zip from dist. Build a Release first. Only Longshot's own files go in.

    python tools/package.py
"""
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = [
    ROOT / "dist" / "longshot.exe",
    ROOT / "dist" / "longshot_core.dll",
    ROOT / "longshot.default.ini",
    ROOT / "README.md",
    ROOT / "LICENSE",
]


def main():
    project = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    version = re.search(r"project\(longshot VERSION ([\d.]+)", project).group(1)

    missing = [path for path in FILES if not path.exists()]
    if missing:
        sys.exit(f"Missing, so build first: {', '.join(str(path) for path in missing)}")

    source = max(path.stat().st_mtime for path in (ROOT / "src").rglob("*.*"))
    if (ROOT / "dist" / "longshot_core.dll").stat().st_mtime < source:
        sys.exit("The source is newer than the build, so build first.")

    output = ROOT / "release" / f"longshot-{version}.zip"
    output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in FILES:
            archive.write(path, f"longshot/{path.name}")

    print(f"{output} ({output.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
