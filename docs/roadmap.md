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
- whether world-to-UI overlays follow the wider UI rect
- aim speed outside walking: the frame rate fix covers the player look function at `game.dll+0x13070`,
  but `+0x119d0`, `+0x13a10` and `+0x14e10` also read the mouse, so vehicles and other cameras may
  still change speed with frame rate

## 2. Full player settings

Done:

- HUD at the screen edges, in a centred 16:9 area, or in a centred 4:3 area (`hud_layout`)
- Scope and binocular masks, flashes and eyelids that reach the screen edges

Still to do:

- Menu backgrounds that fill the screen
- An in-game settings screen, instead of only `longshot.ini`
- Sensitivity as cm/360 from a mouse DPI setting
- Pausing while the game is in the background. The original stopped when it lost focus because it
  tore down its video. Longshot keeps the window alive, so the hunt currently keeps running.

## 3. Performance and stability

Driven by the logs from stage 1, since those show where the time goes:

- fix the crash causes
- reduce hitching from synchronous loading and from audio
- reduce draw calls in dense forest
- large address aware support, if memory limits turn out to matter

## 4. New renderer

Replace `lib/gl.dll`, the swappable OpenGL plugin, with a modern renderer that keeps the game's
art. It starts by matching the original image, then adds optional effects, each with a toggle:

- proper anti-aliasing and anisotropic filtering
- longer draw distance and better shadows
- ambient occlusion
- better water and lighting

The renderer will be built to stand on its own, so it can carry over to stage 5.

## 5. Standalone engine

Replace `game.dll` and the rest of the engine, reading the original `base.scs` directly. This is by
far the largest stage: about 1.7 MB of compiled game logic, including animal AI, ballistics, missions
and UI, to rebuild and match. Stages 1 to 4 will show which parts are worth tackling first.
