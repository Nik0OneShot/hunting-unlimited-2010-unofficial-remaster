# Engine notes

Reverse-engineering notes for Hunting Unlimited 2010, Steam build 252065. Offsets are relative to
each module's base. `tools/verify_targets.py` checks the ones Longshot depends on.

## Layout

32-bit, built June 2009 with MSVC 7.1. No DRM, packers or Steam API. No ASLR, and `prism3d.exe`
has no relocations, so it always loads at `0x400000`.

| File | Role |
| --- | --- |
| `hu2010.exe` | Launcher (`istart`). Picks settings, then starts `prism3d.exe` with the arguments below |
| `prism3d.exe` | Generic host. Exports 261 engine functions by name |
| `p3core.dll` | Core: strings, files, maths, timers |
| `p3shared.dll` | Shared: cvars, UI, resources, video process |
| `gfxshared.dll`, `inpshared.dll`, `sndshared.dll` | Base classes for the plugins |
| `lib/gl.dll` | OpenGL renderer plugin (loads `opengl32` dynamically) |
| `lib/di8.dll` | DirectInput 8 plugin |
| `lib/ds8.dll` | DirectSound plugin |
| `lib/loaders.dll` | Image loaders |
| `lib/game.dll` | All game logic, about 1.7 MB of code |

Plugins export one function, `<name>_get_dl_ops`. They bind the engine API by name, using
`GetProcAddress` on the host, into pointer slots in their own data.

`prism3d.exe` runs nothing by default. Without `-exec` it finishes startup, logs
`[sys] process shutdown` and exits with code 0. The arguments it needs are string 114 in `hu2010.exe`:

    -load lib/ds8 lib/loaders lib/gl lib/di8 -exec lib/game

`-log <file>` takes a normal Windows path. Each option's arguments run up to the next `-`.

Besides those arguments, `hu2010.exe` writes settings to a config file. It sets `r_device`, `r_path`,
`r_mode`, `r_fullscreen`, `s_acceleration`, `s_mix_rate`, `s_device` and `g_quality`. Starting the
engine directly works without it: the engine creates its own profile folder, a device name that
doesn't match falls back to the first device, and `base.scs` has defaults for the rest, apart from
`g_quality`, which Longshot sets. Render paths are in `specs.settings`, and the default of 5 is `ARB2`.

Nearly all calls between modules go through import tables or those slots, so most fixes need no
code patching at all.

## Assets

`base.scs` is a plain uncompressed ZIP of 17,347 files. It mounts alongside optional `patch.scs` and
`secure.scs`, and the home folder is under Documents.

Config order is `autoexec.cfg`, then `default.cfg`, `config.cfg`, `/home/config.cfg`.

Formats seen: `.pmd/.pmg/.pma/.pmc` models, `.dds/.tga/.tobj` textures, `.mat` materials, `.rfx` effects,
`.vp/.fp` ARB shaders, `.vgl/.fgl` GLSL shaders, `.nvrc/.nvfp/.atifs` vendor shaders, `.esc` UI layouts,
`.sii` definitions, `.bhv` animal behaviour, `.lnd` and `.ai` map data, `.ogg` audio.

## Timing

`p3core!tmr_read` is `timeGetTime() * 1000`. `sys_frame` uses it for all timing, then clamps each
frame delta to 0..0.1 s. So every delta is a whole number of milliseconds: at 165 Hz, 6.06 ms frames
come out as 6 or 7 ms.

Longshot replaces the function with a jump to a QueryPerformanceCounter version.

## Mouse

`di8.dll` opens the mouse with `DISCL_EXCLUSIVE | DISCL_FOREGROUND`, with a 128-event buffer. Each axis
change is one event, so fast mice can fill it during a hitch. It reads up to 128 events per frame, sums
them, and posts one `inp_event` of type 5 carrying the total.

The event handler only processes offsets up to `DIMOFS_BUTTON2`, so `mouse4` and up never fire even though
the engine defines those triggers. Button events are type 3 (down) or 2 (up), with `code + 0x106` as the
trigger. Engine codes: 0 left, 1 middle, 2 right.

The look function at `game.dll+0x13070` receives `sys_get_lastdelta()` as its argument and does:

    yaw = (impulse(mleft) - impulse(mright)) * aim_scale * 0.872665 * delta

Keys and joysticks need that `delta`, because they hold a rate. Mouse values are already a total for
the frame, so the multiplication makes aim speed depend on frame rate: at 165 FPS the same movement
turns about 2.75 times less than at 60 FPS.

`aim_scale` is `state + 0x70`, where `state` comes from the pointer at `game.dll+0x1e9d68`. It is 1.0
normally and lower while looking down a sight.

The options slider stores `position * 0.033`, and its lowest position is 5, so `m_sensitivity` bottoms out
at 0.165. At 60 FPS the stock formula then gives 0.14 degrees per count, or about 3.6 pixels at the centre
of a 3440 wide screen at the Hor+ view. Steps that size look choppy during slow aiming, even with perfect
frame pacing, so Longshot's reference rate is high enough to bring that step under a pixel.

Longshot swaps the `inp_impulse_value` slot at `game.dll+0x2d08e8`. When the caller is inside the look
function and the impulse is one of the four mouse look ids (`0x6e..0x71`), it scales the value by
`(1 / reference_hz) / delta`, which cancels the multiplication. Bindings, inversion and the in-game
slider all keep working.

Not changed: the camera function at `game.dll+0x119d0`, which also reads those impulses. Its purpose
and whether it needs the same fix are still unknown.

## Field of view

`frustum_t` layout: `+0` fov, `+4` aspect, `+8` near, `+0xc` far, `+0xe8` dirty flag.

The FOV is horizontal. `r_frustum_projection_matrix(matrix, rect, near, far, fov, aspect)` computes
`x = near * tan(fov/2)` and `y = x / aspect`, so wider screens lose vertical view. Passing `aspect` as 0
means use `rect`, where `rect` is left, right, bottom, top.

Hor+ widens `tan(fov/2)` by `aspect / (4/3)`, which keeps the vertical view exactly as it was at 4:3.

Longshot hooks `set_fov` and `r_frustum_projection_matrix` in `game.dll`'s imports. The first drives
culling and the second the actual projection, so they have to agree.

The frustum planes use `aspect * tan(fov/2)` for the vertical extent. With `aspect` as width over height,
that is wider than the view, which is safe but culls less than it could.

## Intro

The menu class's update at `game.dll+0x93f0` steps through four screens: `esrb` for 4 seconds, then
`publisher`, `scs_icon` and `gametitle` for 2 seconds each. Each has a fade in, a hold and a fade out,
tracked by the logo index at `+0x104`, the phase at `+0x108` and a timer at `+0x10c`. The menu map starts
loading when the last screen appears.

If the `esc` trigger is held, it adds 1000 seconds to the timer each frame, which ends the current phase. A
`jbe` at `+0x9440` stops that on the first screen.

Longshot removes that `jbe`, and answers the `esc` lookup from that function with a held trigger once any key
or button has been pressed, which runs through every screen in a few frames.

## Scope zoom

Scopes are defined in `def/equipment.def` with `zoom_min`, `zoom_max` and `zoom_step`, usually a step of 1.
The view through a scope is the main camera at a narrower field of view.

The wheel is bound to the `scope_zoomin` and `scope_zoomout` impulses (`0x87`, `0x88`). Two functions act on
them, at `game.dll+0xa3d10` and `+0xa9890`, and both call the zoom step at `+0x8ae80`. That function:

- changes the zoom level by one step
- writes `2 * atan(tan(base) / zoom)` straight into the field of view at `state + 0x68`
- writes `scale / zoom^k` to `state + 0x70`, the aim scale, so it varies with zoom and isn't a fixed 0.2
- sets a 0.07 second lockout at `+0xf0`, during which further steps are ignored

The lockout stops the two callers from stepping twice for one notch, and makes a held key repeat at a
sensible rate, so it has to stay. Wheel notches that land inside it are lost, though.

Longshot leaves the game's zoom levels alone. It glides the field of view it passes on to `set_fov` and the
projection toward the game's value, and its mouse layer queues wheel notches while looking down a sight,
releasing one each time the lockout ends.

## Display

Modes are enumerated on demand with `EnumDisplaySettingsA`, so there is no fixed mode list to overflow.
`r_mode` looks like `1024x768x32x75`.

`r_mode` and `r_fullscreen` are cvars inside `game.dll` at `+0x1e5398` and `+0x1e5468`. `game.dll` reads them
immediately after `ui_init` returns, once the config files have loaded.

A `cvar_t` starts with its name, and has a read-only flag at `+0x69`, the current value at `+0xb4`, the
default at `+0xb8`, and a link at `+0xbc`.

The fullscreen window path already creates a `WS_POPUP` at the origin, topmost. So borderless only needs
`ChangeDisplaySettingsA` skipped and the topmost flag removed.

`gl.dll` calls `SwapBuffers` through its import table. It can also use `wglSwapBuffers`, fetched with
`GetProcAddress`.

## Focus

The window procedure is at `prism3d.exe+0xc970`. `WM_ACTIVATEAPP` sets the application active flag at
`+0x49ed0`, which starts at 1. `game.dll+0x103649` compares that flag with its last value every frame, and
any change restarts the whole video system: window, GL context and resources. That made sense when losing
focus meant losing an exclusive display mode. In the log it shows up as repeating `[gl] CDS init` and
`[gl] CDS shutdown` pairs, with a new window each time.

`WM_ACTIVATE` is separate, and only stops input and resets impulses.

Longshot swallows `WM_ACTIVATEAPP` with focus lost, and only in borderless. The flag stays 1, so the game
keeps its window, and it sleeps a little each frame while in the background.

Windows on editions without Xbox Game Bar can show an `ms-gamingoverlay` link error when a game takes
focus. Before this fix, that dialog alone was enough to set off the restart.

## UI

Layouts are text files with coordinates in a 1024x768 space, from `ui_get_reference_mode`.

The UI object holds a viewport rect at `+0x1f0` (left, right, bottom, top), set by `ui_set_viewport_rect`.
Both drawing and cursor mapping read it, so widening it keeps the layout centred and the cursor aligned.
The engine itself never calls that setter.

`ui_view_t::uiref2view(out, in, width, height)` turns a UI rectangle into pixels. It scales by
`width / 1024` directly, ignoring the rect above, so Longshot corrects its output. Five places in
`game.dll` use it: two 3D preview views, and three menu handlers that write the result into the main
world viewport at `game object + 0x2a4`, which is how the world appears in a menu panel. The game object
is the pointer at `game.dll+0x1e9d60`.

The world camera builds its projection from the full window size, never from that viewport, so a menu
panel gets the full window's aspect. Stock 4:3 hid this because the panel is nearly 4:3 too. Longshot
passes the viewport's aspect explicitly instead.

Still to check: other `ui_get_reference_mode` users in `game.dll` convert between world and UI space with
the plain reference size, for example at `+0xabb0`, so anything using them may not follow the wider rect.

The engine never clears the colour buffer, since the world or a UI background always covers it. It also
renders environment cube map faces into the back buffer's bottom-left corner, then copies them out. With a
centred UI, both show in the side bars during menus, so Longshot clears the bars just before each swap.

The world filling the screen isn't enough to tell a hunt from a menu. The menu map starts during the last
intro screen and draws full-screen behind the black 4:3 splash, then the main menu moves it into a panel.
So the bars only show the world during a hunt. A hunt starts at the virtual method at `game.dll+0x3630`,
which hides the pointer and shows the HUD. That method is on the hunt class only: the menu class uses the
same `+0x104` field for its logo index. A hunt ends when the HUD window is removed or the world viewport
becomes a panel. Frames without the world, such as loading, don't end it, in case a hunt screen pauses
world drawing.

### Lists

UI components are in `game.dll`. Each has a descriptor with its name, then its size at `+0x14` and factory
at `+0x18`. `ui_view_t` virtual methods are `handler` at `+0x54`, `handle_event` at `+0x78`, `action` at
`+0x7c`, `offset` at `+0x80` and `changed` at `+0x9c`.

`ui.comp.common.list` has its vtable at `+0x1be468`. The wheel reaches `action` as type `0x12` or `0x13`,
and lists are the only UI component that handles those. The list moves its slider, which comes back as
action type 2 and calls `+0x1664f0` with an item index.

A list keeps a fixed set of row views in a `vptrlist_t` at `+0x100`. They are positioned once, when created,
at `top - index * (item_height + item_gap)`. Scrolling only sets the first item index at `+0x114` and refills
the rows, so there is no per-pixel scrolling to drive.

Longshot hooks those two vtable entries. After a wheel scroll it shifts the rows back by the distance
scrolled, then eases them to their real positions from the list's own `handler`, so the animation stops by
itself if the list goes away. The shift is capped at one row, because rows aren't clipped to the list and
would otherwise draw over nearby UI.

## Ruled out

- Extension string overflow: `message()` formats into allocated memory, so a very long GL extension list is safe there.
- Display mode list overflow: there is no fixed list.
