# Longshot

A modern runtime for **Hunting Unlimited 2010** (SCS Software, 2009). It runs your own copy
of the game with proper ultrawide support, high refresh rates and correct mouse aim.

Longshot does not include any game files, and it needs a legitimately owned copy to run.
It never changes your game folder: fixes are applied in memory while the game runs.

Not affiliated with or endorsed by SCS Software or THQ.

> **Status: early.** The game runs and plays at a locked 165 FPS at 3440x1440 on the first test
> PC, but it hasn't been tried on a wide range of hardware. See [docs/roadmap.md](docs/roadmap.md).

## What it fixes

| Problem in the original | What Longshot does |
| --- | --- |
| Aim speed changes with frame rate, because mouse movement is multiplied by frame time | Aim speed stays the same at any frame rate |
| The game clock ticks in whole milliseconds, which causes stutter at high refresh rates | Sub-millisecond clock |
| Wide screens crop the top and bottom of the view | Proper Hor+ field of view |
| Menus and HUD are stretched on wide screens | Correct proportions, with menus centred |
| The HUD can't use a wide screen | Put it at the screen edges, in a centred 16:9 area, or in a centred 4:3 area |
| Scopes and fades only cover the middle of a wide screen | Scopes, binoculars, flashes and eyelids reach the screen edges |
| Fullscreen changes the display mode and blocks alt-tab | Borderless fullscreen |
| Losing focus destroys and rebuilds the window and renderer | The game stays up, and alt-tab is instant |
| The mouse is held exclusively, and fast mice lose movement | Raw Input with no lost movement |
| Only three mouse buttons work | Side buttons can be bound in the game's controls menu |
| Fixed scoped sensitivity | Zoom-scaled sensitivity, which can be turned off |
| Scope zoom jumps between levels and loses fast wheel notches | Smooth zoom, and every notch counts |
| The first intro screen can't be skipped, and only Esc skips the rest | Any key or click skips the whole intro |
| Menu lists jump a row at a time | Lists glide when scrolled |
| Sensitivity is limited to the in-game slider | Exact, separate X and Y values |

## Using it

1. Install Hunting Unlimited 2010 through Steam.
2. Put the Longshot files in any folder, and run `longshot.exe`.

It finds the Steam install by itself. If it can't, set `game_dir` under `[paths]` in
`longshot.ini`. All settings are in that file, created on first run, and each is explained
there.

Longshot starts the game directly, so Steam won't show the game as running. To launch it from
Steam, add `longshot.exe` as a non-Steam game.

Your security software may warn about `longshot.exe`, because it loads code into another
process, and that is also what some malware does. The full source is in this repository.

## Troubleshooting

**"We can't open this 'ms-gamingoverlay' link"**

This is a Windows message, not a game error. Windows tries to open Xbox Game Bar, which isn't
included in some editions such as LTSC. It can't harm the game: click OK, then click back on the
game. Longshot never changes Windows settings for you, but you can stop the message yourself by
running these in PowerShell, then signing out and back in:

```powershell
reg add "HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\GameDVR" /v AppCaptureEnabled /t REG_DWORD /d 0 /f
```

```powershell
reg add "HKCU\System\GameConfigStore" /v GameDVR_Enabled /t REG_DWORD /d 0 /f
```

```powershell
reg add "HKCU\Software\Microsoft\GameBar" /v UseNexusForGameBarEnabled /t REG_DWORD /d 0 /f
```

## Reporting a problem

After a run, attach everything from the `logs` folder: `longshot.log`, `engine.log`,
`launcher.log`, and `crash.dmp` if there is one.

## Building

You need Visual Studio 2022 or later with C++ tools. The build must be 32-bit because the game is.

```bash
cmake -S . -B build -A Win32
```

```bash
cmake --build build --config Release
```

The result is in `dist`. These three checks need no game launch:

```bash
dist/longshot.exe --test-target "<full path>/dist/longshot_selftest.exe"
```

```bash
python tools/verify_targets.py "<game folder>"
```

```bash
python tools/check_layouts.py "<game folder>"
```

The first injects into a stand-in program and writes `dist/logs/selftest.txt`. The second confirms
every offset and symbol Longshot relies on against a game install. The third rewrites the install's
real layout files for each screen shape and HUD setting, and confirms the results are sound.

## How it works

See [docs/engine-notes.md](docs/engine-notes.md) for what was learned about the engine, and
[docs/roadmap.md](docs/roadmap.md) for what comes next.

## License

MIT. See [LICENSE](LICENSE). This covers Longshot's code only, not the game.
