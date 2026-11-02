# Hunting Unlimited 2010 Unofficial Remaster

Longshot is a modern runtime for **Hunting Unlimited 2010** (SCS Software, 2009). It runs your own
copy of the game with proper ultrawide support, high refresh rates and correct mouse aim.

Longshot does not include any game files, and it needs a legitimately owned copy to run.
It never changes your game folder: fixes are applied in memory while the game runs.

Not affiliated with or endorsed by SCS Software or THQ.

> **Status: early.** It plays well at a locked 165 FPS at 3440x1440, but so far it has only been
> tested on that one PC. Please [report what you find](#reporting-a-problem).

## What it fixes

| Problem in the original | What Longshot does |
| --- | --- |
| Aim speed changes with frame rate, because mouse movement is multiplied by frame time | Aim speed stays the same at any frame rate |
| The game clock ticks in whole milliseconds, which causes stutter at high refresh rates | Sub-millisecond clock |
| Wide screens crop the top and bottom of the view | Proper Hor+ field of view |
| No anti-aliasing, and jagged leaves and grass | Up to 8x anti-aliasing, including foliage edges, and 16x texture filtering |
| The frame rate can fall by two thirds depending on where you look, because terrain data is rebuilt every frame | Terrain data stays loaded, and can't run out of room |
| Everything in view gets drawn, even behind a hill | Terrain and trees hidden by the ground are skipped, which looks the same and draws less |
| Grass and trees pop in close to you | Grass out to 85 m, and trees that keep their detail up to 3x further |
| A flat, soft image by modern standards | Optional sharpening, richer colour and bloom, which leave the HUD alone |
| Holding forward walks you slightly off to the side | You walk where you're heading |
| Weapons ignore or misplay presses made while they're busy | Presses wait their turn, the latest one decides, and reloads can be cut short |
| Fire, holster, and the scope comes back up first | It goes straight to cocking and holstering |
| The ATV cuts out at 25 km/h, stalls on hills, and bounces constantly | It pulls away properly, climbs without a run-up, reaches 55 km/h, and rides on its suspension |
| The ATV's brake does less than letting go, and it can't hold it on a hill | It stops from 55 km/h in about 9 m, and holds it on hills |
| The ATV washes wide in turns, and wanders on hills and over bumps | Steering suits your speed, and it holds its line |
| Walking is slow, and half that backwards or sideways | A bit faster forward, and most of that speed in every direction |
| Footsteps have two volumes, and animals don't hear how fast you go | Both follow your real speed, so creeping is quieter and running is louder |
| Footsteps go silent on some ground | Every step makes a sound |
| Ironsight shots land away from the sights | Sway moves the whole view, as in a scope, so shots go where the sights are |
| A free hunt runs out of animals, and opens by showing you where one is | New herds arrive to hold the population you chose, and you start straight at your marker |
| Free hunt animals start close by, and can bolt for no reason | They start at least 450 m away, and only run from what they've noticed |
| The free hunt map marker snaps to points about 200 m apart | It goes almost anywhere, and a hunt never starts you in water |
| Your eyelids close whenever the sun is anywhere in a wide view | Only when you're looking close to the sun |
| Menus and HUD are stretched on wide screens | Correct proportions, with menus centred and their backgrounds filling the sides |
| The HUD can't use a wide screen | Put it at the screen edges, in a centred 16:9 area, or in a centred 4:3 area |
| Scopes and fades only cover the middle of a wide screen | Scopes, binoculars, flashes and eyelids reach the screen edges |
| Fullscreen changes the display mode and blocks alt-tab | Borderless fullscreen |
| Losing focus destroys and rebuilds the window and renderer | The game stays up, and alt-tab is instant. The hunt and its sound keep running, or the hunt can pause while you're away |
| The mouse is held exclusively, and fast mice lose movement | Raw Input with no lost movement |
| Only three mouse buttons work | Side buttons can be bound in the game's controls menu |
| Global hotkeys, like a screenshot tool's Print Screen, don't work in game | They do now, and the Windows key works too |
| The game's screenshot key is F10 | It's on F12, the same as Steam's |
| Fixed scoped sensitivity | Zoom-scaled sensitivity, which can be turned off |
| Scope zoom jumps between levels and loses fast wheel notches | Smooth zoom, and every notch counts |
| The first intro screen can't be skipped, and only Esc skips the rest | Any key or click skips the whole intro |
| Menu lists jump a row at a time | Lists glide when scrolled |
| Sensitivity is limited to the in-game slider | Exact, separate X and Y values, or a turn speed in cm/360 |

## Using it

You need Windows 10 version 1803 or later, and the Steam version of the game. Other versions may
run, but the fixes that depend on the exact game build turn themselves off.

1. Install Hunting Unlimited 2010 through Steam.
2. Download the zip from [Releases](../../releases), and unzip it to any folder.
3. Run `longshot.exe`.

It finds the Steam install by itself. If it can't, set `game_dir` under `[paths]` in
`longshot.ini`. The main settings are in the game, under Options, then Longshot. All of them are
in that file, created on first run, and each is explained there.

To play through Steam, so it shows you as playing and counts your time, right-click the game in
Steam, choose Properties, and set Launch Options to this, using your own path to Longshot:

```text
"C:\full\path\to\longshot.exe" %command%
```

Then press Play in Steam as usual. Running `longshot.exe` by itself still works, but Steam won't
show the game as running.

F12 saves the game's own screenshot to `Documents\Hunting Unlimited 2010\screenshots`. You can
pick a different key in the game's controls menu, and Longshot will leave it alone.

Windows or your security software may warn about `longshot.exe`, because it isn't signed and it
loads code into another process, which is also what some malware does. The full source is in this
repository, so you can read it or build it yourself.

To remove Longshot, delete its folder. Your game stays as it was, though the game saves any change
to your key bindings in its own profile.

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

## Known gaps

- Only tested on one PC so far, with the Steam build.
- Steam's overlay loads when the game starts through Steam, but it hasn't been fully tested with Longshot.
- Distant trees and fine edges can still shimmer, even at 8x anti-aliasing.

## Reporting a problem

[Open an issue](../../issues), and attach everything from the `logs` folder: `longshot.log`,
`engine.log`, `launcher.log`, and `crash.dmp` if there is one. A screenshot helps with anything
visual. The logs show file paths, which can include your Windows user name, and a crash dump holds
a copy of the game's memory, so look through them first if that matters to you.

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
real layout files for each screen shape and HUD setting, and confirms the results are sound. The
two Python tools need `pip install pefile capstone`.

To make a release zip, build first, then:

```bash
python tools/package.py
```

## How it works

See [docs/engine-notes.md](docs/engine-notes.md) for what was learned about the engine, and
[docs/roadmap.md](docs/roadmap.md) for what comes next.

## License

MIT. See [LICENSE](LICENSE). This covers Longshot's code only, not the game.
