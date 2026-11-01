# Roadmap

Each stage ships something playable, and later stages reuse the work before them.

## 1. Wrapper core (current)

Timing, aim speed, Raw Input, side buttons, Hor+ FOV, borderless fullscreen, centred 4:3 UI,
frame limiter, vsync control, frame-time logging and crash capture.

Running in the real game on the first test PC: 3440x1440 at a locked 165 FPS, with no crashes in a
three-minute session.

Still to confirm with longer play:

- UI scissor clipping with the widened UI rect
- whether anything in the game misbehaves at high frame rates
- whether the crashes and hitching seen on the original return
- whether terrain, grass or tree culling needs the wider FOV too

## 2. Full player settings

Done:

- HUD at the screen edges, in a centred 16:9 area, or in a centred 4:3 area (`hud_layout`)
- Scope and binocular masks, flashes and eyelids that reach the screen edges
- Pausing a hunt while the game is in the background, off by default (`pause_in_background`)
- Menu backgrounds that fill the screen
- An in-game settings tab, under Options
- Turn speed as cm/360, from a mouse DPI setting

## 3. Performance and stability

The logs now split every frame into its parts, list each slow frame, and report memory use. On the
first test PC that showed almost nothing to fix: an 11 minute hunt held 165 FPS, with about 2.5 ms of
work a frame, and every slow frame had a known cause, like a screenshot or loading a map. See
[engine-notes.md](engine-notes.md).

So the rest waits for logs from other PCs, since those will show what's worth doing:

- fix any crashes that turn up
- reduce hitching from loading and audio, if it shows on slower drives
- reduce draw calls in dense forest, if weaker PCs need it
- large address aware support, if memory limits turn out to matter

## 4. Graphics

Modern graphics that keep the game's art, each with a toggle.

Done:

- Up to 8x anti-aliasing, including the edges of foliage
- Up to 16x texture filtering
- Grass out to 85 m, and trees that keep their detail further out
- Sharpening, richer colour and bloom

Later: stronger anti-aliasing for the shimmer on distant trees and fine edges, through supersampling
or something like FSR or DLAA.

## 5. Standalone engine

Replace `game.dll` and the rest of the engine, reading the original `base.scs` directly. This is by
far the largest stage: about 1.7 MB of compiled game logic, including animal AI, ballistics, missions
and UI, to rebuild and match. Stages 1 to 4 will show which parts are worth tackling first.
