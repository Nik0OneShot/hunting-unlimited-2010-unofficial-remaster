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

Cameras are the `visor` classes, and four of them read the mouse:

| Class | Look function | Multiplies by frame time |
| --- | --- | --- |
| `fps_visor`, on foot | `+0x13070` | Yes |
| `ladder_visor` | `+0x13a10` | Yes |
| `third_person_visor`, the base of `vehicle_visor` | `+0x14e10` | Yes |
| `dead_visor` | `+0x119d0` | No, it adds the mouse straight to its angles |

The first three share the same formula and are all fed `sys_get_lastdelta`, so the same scaling fixes them, and
one sensitivity applies to all of them. `dead_visor` is left alone.

## Keyboard

`di8.dll` asks for the keyboard with `DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_NOWINKEY`, at `+0x30e7`. That
last flag does more than stop the Windows key. While the game has focus, no `RegisterHotKey` hotkey fires in any
program, so screenshot tools never see Print Screen. Tested with a stand-in: with the flag, none of a set of test
hotkeys arrived, and without it, all of them did. Longshot removes the flag, so the Windows key works in game.

The game's own screenshot is the console command `screenshot`, bound to F10, and it saves numbered PNGs to
`/home/screenshots`. Print Screen isn't bound to anything. Bindings come from two places: default text in
`game.dll` at `+0x1adc98`, read with `read_from`, then `/home/controls.def` if it exists, read with
`read_from_file`. The game writes that file back, with any changes, when it exits.

Longshot moves the shot to F12, Steam's screenshot key, by changing the default text and rewriting `controls.def`
through the file hook. It only moves it from F10, and only if nothing else is on F12, so a key the player chose stays.

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

## Sound

The engine checks each sound once a frame in `snd_update_state`. A sound's tag is passed when it's created: world
sounds use `0x0a`, and menu music uses `-1`. `snd_set_current_tag` flags every sound with a different tag as silenced,
except `-1`.

A silenced sound loses its channel. If it loops, it starts again when it's allowed back. If it doesn't, the engine
forgets it, and destroys it if it's a one-shot. So switching tags can't pause a sound. Music in a hunt is exposed to
this: `/music/target_range.ogg` is a one-shot with tag `0x0a`.

`ds8.dll` builds its buffer flags at `+0x1191` from `0xa0`, or `0xb0` for 3D sounds, plus `DSBCAPS_LOCSOFTWARE`,
since `s_acceleration` defaults to 0. Neither focus flag is set, so Windows silences sounds while their window is
out of focus. They keep their place, and `GetStatus` still reports them as playing, even across a real focus change,
so the engine doesn't lose them.

With `pause_in_background` off, Longshot adds `DSBCAPS_GLOBALFOCUS`, so a running hunt stays audible.

## Pausing

The original stopped when it lost focus, because it tore down its video. Longshot keeps the window alive, so a
hunt keeps running unless `pause_in_background` is on.

The game object has `pause(bool)` at vtable `+0x28` and `is_paused` at `+0x2c`, both on the byte at `+0x550`.
While that is set, the game frame at `game.dll+0x1034c0` doesn't advance the world. The pause key (`gamepause`,
impulse `0x45`), the in-game menu and other dialogs all use it, and one dialog, at `+0x44a10`, saves the old value
before pausing.

The pause window also switches the sound tag, with `0x14` to pause and `0x0a` to resume. Longshot doesn't, because
that would destroy any one-shot that's playing. It leaves the quiet to Windows instead.

Longshot calls `pause` from its swap hook, on the game's main thread. It only does so during a hunt, and only if the
game isn't already paused, so a pause the player set is still there when they come back.

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

The camera's conversions between world and UI space, at `+0xabb0` and `+0xae30`, use the plain reference size,
so they wouldn't follow the wider rect. Only the map editor calls them, in `emainhdl.cpp`, so hunts aren't affected.

The engine never clears the colour buffer, since the world or a UI background always covers it. It also
renders environment cube map faces into the back buffer's bottom-left corner, then copies them out. With a
centred UI, both show in the side bars during menus, so Longshot clears the bars each frame.

The world filling the screen isn't enough to tell a hunt from a menu. The menu map starts during the last
intro screen and draws full-screen behind the black 4:3 splash, then the main menu moves it into a panel.
So the bars only show the world during a hunt. A hunt starts at the virtual method at `game.dll+0x3630`,
which hides the pointer and shows the HUD. That method is on the hunt class only: the menu class uses the
same `+0x104` field for its logo index. A hunt ends when the HUD window is removed or the world viewport
becomes a panel. Frames without the world, such as loading, don't end it, in case a hunt screen pauses
world drawing.

### Layout files

`.esc` files and `.def` templates are both read by `decipher_file` in `p3shared.dll`, which opens them through
that module's `fs_open` import. Longshot hooks that import. For the handful of files it changes, it reads the
player's copy, rewrites the text and hands back a `mem_file_bind` file. That file doesn't own its buffer, so
Longshot keeps the buffer. Doing this before the engine parses a file means every part of the game sees the
new positions from the start, and nothing is shipped or written to disk.

Coordinates are in the window's space, with y up, and aren't relative to the parent group, so moving a group
means moving every entity inside it. The `coords` of a `fitting` group are recalculated from its contents.

Windows clip to their own rect unless they have `"clip_children" : "false"`, so anything outside 0..1024 needs
that. The window's own rect has to stay at 0,0, though:

- `ui_view_t::move_to` offsets by the distance from the current left and bottom. The scope handler parks its
  window with `move_to(0, 1024)` and brings it back with `move_to(0, 0)`, which would shift a window that
  doesn't start at 0,0. Anything added to it must also stay within y 0..768, or it would still show while parked.
- Only one thing slides in the HUD: the equipment bar, group 31. The slide at `game.dll+0x7f0d0` picks its
  direction from where the item sits. Left of 0 or right of 1024 means sideways, and only after that does it
  check below 0, which is the case the bar relies on. So the bar has to stay within 0..1024.

The scope mask is a circle with solid corners, and its sides are fully black, so black bars continue it to
the screen edges. The binocular mask is two bands, across the top and bottom, and the view runs off both sides
between them. Bars would cut that off with a straight edge, so its halves stretch outward from the centre.

Eyelids close when the player looks at the sky. They are templates 60 and 61 in `def/ui/txt.def`, each two
images of a fixed width side by side. Those are the only uses of `width=514`. The images have to be widened
along with the rect, or they sit at its left end.

`ui/xfader.esc` isn't used by anything. `gscore`, `gmap` and `gpause` stay inside the 4:3 area.

Menu backgrounds are `ui_static_back.mat`, a leather texture stretched over the 1024 wide layout. Most menus
draw it as one full rect, and `mmain`, `mtroom` and `manimaldb` draw it as four tiles around the 3D panel. A dark
layer in `rgb_bck` goes over it, and that layer's `top.mat` and `bottom.mat` images have a ragged edge. So
Longshot adds side panels to the window, with both reflected at the layout's edges using `left` and `right`.
Reflecting means the texture joins with no seam and keeps its scale. Those windows have to drop `fitting`, or the
panels would widen the rect that `move_to` goes by.

The side bars now have to be cleared before the UI draws, or the clear would erase those panels. The game draws the
whole UI with one `ui_process` call in its frame at `game.dll+0x102df0`, after the world, so Longshot clears there.
If a frame reaches the swap without it, the bars are cleared at the swap instead.

### Options menu

`ui.handler.menu.options` has its vtable at `game.dll+0x1ac7e8`, with `handle` at `+0x6c` and the method that runs
when the window opens at `+0x80`. `handle(view, action, param)` gets a click as action 2, with the control's id at
`view + 0xdc`. Ids 1, 2 and 3 are Apply, Cancel and Defaults, and 10, 11 and 12 show the tab windows held at
`+0xbc`, tracked by the index at `+0xc8`. Tabs share the `menu_tab` window group. Defaults goes to whichever tab
that index names.

Longshot's tab isn't a window. Its button and rows go in the `moptions.esc` layout, inside the options window, so
every click on them reaches that one handler. Longshot hooks both vtable entries, and hides the game's current tab
when its own is chosen. A view's `layer` is its own, not inherited from its group, and the mouse goes to the highest
layer under it, so the rows need a layer above the window's frames, which are on 1. Rows are cycle buttons, look 26, set through the `value` definition at `+0x1e7128`, whose
setter is at `+0x34`. A setter takes the view object, the definition, an index, and the text.

Changes wait for Apply, which writes them to `longshot.ini` and reloads it, so anything read from the config each
time takes effect at once. A value edited by hand that isn't one of a row's choices is shown as it is, and only
replaced if the player clicks that row.

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

Longshot hooks those two vtable entries. After any action that changes the first item, it shifts the rows back by
the distance scrolled, then eases them to their real positions from the list's own `handler`, so the animation
stops by itself if the list goes away. The wheel moves the slider, which arrives as a second action inside the
first, so only the outermost one counts.

Rows aren't clipped to the list. The shift is capped at one row, and a row that would land outside the list moves
far off-screen until it fits again. Hiding it instead would disturb focus, and the list's own hiding of empty rows.

## Renderer

`lib/gl.dll` picks a technique for each material from `r_path`, and 5 is `ARB2`. Materials point at `.rfx` effect
files, 589 of them, which list a technique per path. For ARB2 those are `arb2.vp` and `arb2.fp`: ARB assembly compiled
from Cg, stored as plain text in `base.scs`, so the file hook can rewrite them. The folders under `effect/hu4/` cover
diffuse, specular, normal and offset mapping, environment maps, terrain with up to three layers, and water.

Nothing uses framebuffer objects. The frame goes straight to the back buffer, and environment cube faces are drawn
there and copied out. The window's pixel format comes from `ChoosePixelFormat` and `SetPixelFormat` in `gl.dll`'s
imports, with no multisampling, so the game has no anti-aliasing of its own.

Longshot replaces `SetPixelFormat`. For the game's window, and not the tiny one it makes to probe the driver, it swaps
in a multisampled format from `wglChoosePixelFormatARB`, which needs a throwaway context to fetch. Trees and grass are
cut out with the alpha test, which multisampling can't smooth, so Longshot also turns on alpha to coverage whenever the
alpha test is on and blending is off. It tracks both through `glEnable` and `glDisable`.

Texture filtering is set per texture with `GL_TEXTURE_MAX_ANISOTROPY_EXT`, and Longshot overrides that value in
`glTexParameterf` and `glTexParameteri`. All of these reach `gl.dll` through `GetProcAddress`, so they can be wrapped by name.

`ui_process` runs after the world and before the UI, with the context current, so Longshot runs its post-processing
there, on hunt frames that drew the world. It copies the frame into a texture, which resolves any multisampling, then draws over the
frame with its own shaders, so the game's drawing and the HUD are left alone. It uses framebuffer objects only for
its own bloom passes. The game keeps its state in sync by itself, so everything changed has to go back exactly:
`glPushAttrib` covers nearly all of it, and the current program and framebuffer are saved by hand. With every effect
off, none of this runs. The self-test runs the effects on the real driver, on a hidden window's back buffer with and
without multisampling, then checks the pixels and that the state came back. Sharpening has to be tested on grey,
because contrast adaptive sharpening does nothing beside pure white, by design.

A frame is drawn in this order, in `game.dll`:

1. The context clears colour, depth and stencil.
2. The sky, with its own camera (near 0.01, far 2) at the origin. Then depth and stencil are cleared.
3. The world, with the main camera. Its near and far planes vary, and the frame's first
   `r_frustum_projection_matrix` call builds it, at return address `+0x5c0e`.
4. Depth and stencil are cleared, and the weapon is drawn with a third camera (near 0.1, far 1000). This step is
   skipped for some views, and its clear with it.
5. Depth and stencil are cleared again, and `ui_process` runs.

So depth is gone by the time the UI starts. Each of those draws goes through the same nine queues, one at a time, in
`shaderlib_queue_render_and_flush` at `+0x1560a0`. Queues 0 and 1 are sorted one way, and the rest another. Materials
pick theirs in their pass's setup: `hu_dif`, `hu_dif_spec` and the terrain use queue 0, and `hu_dif_opac`,
`hu_dif_opac_attn` and `hu_water` use queue 6. That's every material with see-through edges, so grass, leaves, bushes
and water all come after everything solid.

The reflection cube at `+0x5f00` builds its camera through the same `+0x5580`, with a field of view of 90 and depth from
1 to 100, one face at a time for a run of frames after the view moves. Its rectangle is only as big as its texture,
which is how to tell it from the world's.

The camera setup at `+0x5580` doesn't run on every frame. In one test it missed 8 to 21% of them while moving and none
while standing still, so nothing that has to happen every frame can depend on it. Longshot tells that the world was
drawn from the frame's own call at `+0x102f99`.

The engine's matrices keep rows together, so element 11 is the one a column-major matrix calls 14. The projection is a
standard OpenGL one with a horizontal field of view: `away = m[11] / (ndc + m[10])`. Copying the depth buffer with
`glCopyTexSubImage2D` works on a multisampled back buffer, even with real geometry and a stencil buffer.

- Anisotropic filtering is in the Graphics menu, as `r_anisotropy_factor`, a fraction of the driver's maximum. The
  default of 0.25 is 4x on a 16x card. Longshot's own setting overrides it, without changing the game's.
- `g_shadows` is only read in two places, both testing for 1 or 3, before drawing `animal_shadow.mat` as a decal.
  No sun shadows have turned up, so there seems to be no shadow map to improve.
- `g_water_quality` turns on a reflection cube. Water doesn't show the scene around it in any other way.
- `g_zclip`, `g_tree_lodswitch` and `g_grass_density` look unused, because their readers aren't where they're used.
  Each cvar has a validator at `cvar+0x64`, which `cvar_t::set` calls, and that stores a float in a global of its own
  at `+0x1e4004`, `+0x1e4008` and `+0x1e400c`. See Distances.

## Weapons

A weapon is in one of three states at `+0x58`: 0 busy, 1 ready, 2 down. While busy, `+0x340` says what it's doing, and
nearly every action ends when its animation does, through the listener at `+0x60`, and not on a timer. Presses come from
`inp_impulse_is_activated`, which reads a byte and clears it, and the byte only lasts one input frame. Nothing reads
reload or sights while a weapon is busy, and raising or lowering reaches a toggle that does nothing, so those presses just
vanish. The one thing that remembers is `+0x1fc`, a lower request.

Firing a scoped rifle goes: shot, a short kick, a wait to watch the bullet land, cock, then ready. Firing never clears the
aim, and cocking ends by restoring the scope, which is why a holster seemed to scope back in first.

So Longshot reads the presses itself at the start of the player's input, at the call at `+0xddaf`, and holds one for
later if the weapon is busy. The latest press replaces it. Once the weapon is free the press goes through the game's own
readers, so nothing is called out of turn. Raising, lowering and the sights wait as long as it takes, because they say how
the weapon should end up, but a shot only keeps for 0.35 s, as a late shot is worse than none. With a lower waiting, or
the sights while aimed, the wait to watch the bullet is skipped, and the end of cocking doesn't put the scope back. A press during a reload sets the game's own stop flag at
`+0x34c`, which takes effect after the round it's on. Cocking can't be cut short, and neither can anything else without
leaving the weapon's state to be repaired by hand, because a forced stop raises no end event.

Animation time only moves when `+0x15a8c0` is called with some seconds, and sounds and events sit on that same clock, so
scaling those seconds speeds up an action cleanly. It's applied to raising, lowering and aiming, at the three calls that
step a held weapon or item. Items that hold for their own length, like calls, are left alone.

## Ironsights

A weapon's sway comes from three slow waves at `+0xa6260`. Sized by stance, they're ±0.15 and ±0.02 at rest, and any shot or
change of stance raises them to ±0.4 and ±0.7, which fade back over 8 seconds. There are two ways they get used, and which
one depends on `+0x2b4`, set while the gun model is drawn.

Through a scope the model is hidden, and `+0xa68c0` turns the camera by three times the sway. The camera's place is what a shot
reads, so the shot goes wherever the view is. That's why scopes feel fair.

In ironsights, `+0xa6150` gives 1.5 times the sway as an angle, which is added to the shot and turns the model. It's the same angle
for both, but it doesn't look the same: the model turns about its own centre and is drawn with its own field of view, so the
sights move a different amount on screen. Right after a shot that's up to about 1 degree, or 1.75 m at 100 m. The model also
trails a turn by up to 0.12, which the shot ignores.

Longshot zeroes that offset for rifles in ironsights, at the shot's call and the model's, and lets the camera's sway through at
half size, so it stays as large as the gun's was. The gun and the shot both hang off the camera, so they stay together. The lag
limit goes to 0 while the setting is on. Bows have their own view method and are left alone.

## Walking

Forward is built from the view with no bias, the head bob stays in the camera, and physics runs at a fixed 40 Hz. But
`avatar::update` at `+0x126dd0` never corrects sideways velocity, and the player's contacts with the ground use ODE's box
friction, with a limit of 500 per axis, on axes close to the world's own. Walking across that box at an angle, the
axes don't resist equally, so the player gets pulled toward the nearest world axis. This is a model, worked out from the
code, and the drift it predicts changes with heading.

Longshot hooks the one `dJointCreateContact` call at `+0x12765d`, and for the player's contacts it sets `dContactFDir1`
to the direction of the walk, which lines the box up with it. In the game that left a typical drift of 1 degree over 3 m,
across 57 straight stretches. It logs that drift, with the heading, and whether the correction is on.

### Who the player is

`game + 0x544` is the current game mode, and `+0x100` of it is that mode's player. It's only a hunter (table `0x19bb70`)
in a hunt, as each mode makes its own: `0x19b098` makes the hunter, `0x19b5b0` makes the one with table `0x19bc48`, and
`0x19aef8` makes another. The world draws behind the intro and menus with one of those in place, and `+0x8c` on those
isn't a view, so anything run from a draw has to check the table before going near a hunter's fields. `walking_player`
does that.

## Speed and noise

Speed on foot is picked at `+0x13501`: 1.6 m/s, or 4.5 running, times the stance (1, 0.5, 0.2) times the direction, from a table at
`+0x19c20c`, with half speed backwards and sideways. Forward wins if a sideways key is held too. Longshot changes the walking
speed in place and the three shares in the table, which nothing else reads. Steps land from the head bob, at a rate in the table at
`+0x19c1e8`, so forward and backward are raised by the square root of the change in speed, since a stride gets longer too.

Footstep volume is fixed at `+0xcd65`: 0.5 crouched and 0.8 otherwise, so running sounds like walking.

Each animal adds up three senses at `+0xb59c0`. Sight is a cone scaled by daylight and how exposed the hunter is. Smell reaches
300 m, downwind only, scaled by cover scent. Hearing reaches 250 m and fades with distance, and the hunter's half of it is set at
`+0xbee5`: 0.7 standing, 0.3 crouched or 0.2 prone, doubled when running, and 0 when still. So it already existed, but as steps.

A step is silent if its kind of ground has no sound in `player.def`, which goes for invis, thin, paper, brush, metal and cactus. The search for what's
under the player at `+0xd42e0` starts from 0, invis, and only changes it if it finds a terrain triangle there, so that also covers anything else underfoot.
Longshot gives those steps the last kind of ground that had a sound, or grass if there hasn't been one.

With `speed_noise`, Longshot works out the speed the game is asking for and gives both the footstep and the hearing a value from
a curve through the game's own numbers. The ATV and horse keep the game's value, which is checked by the class of the view.

## Free hunt

The menu writes a whole mission to `tmp_mission.def` and loads it like any other. A new profile's `free_hunt` section comes from a
list of name and value pairs at `+0x1e2020`, read at `+0xd0bef`. Longshot points that at its own copy, so existing profiles are
never touched. `last_map` is the area's place in the list, plus one, and 0 means the first visit, which picks its own loadout and
sets Alaska. Setting it skips that.

Animal slots hold a species' place in `game.def`, not in the area's list. Density is 0 off, 1 low, 2 medium or 3 high, which picks
from `freehunt_counts`, 2, 3 or 5 animals. The game's limit is 40 at once in the generator.

**The map marker** sits on the area's start points, 65 to 173 of them about 200 to 300 m apart in `landscape.start.sii`. The menu keeps them in a list
at `+0xc0` in its handler, with 0x28 bytes for each, and picks the nearest to a click. After reading the list it numbers every entry and works out
where it sits on the map, in one loop, so anything added to the list first is treated the same as the rest. Longshot hooks the read at `+0x25efe`,
and adds a spot every 20 m within 220 m of a start point. They go after the start points, so numbers saved in a profile still mean the same place.
The menu reads nothing else from the list, and the map editor's own read isn't touched.

A spot takes the height of the nearest start point, which doesn't matter: the opening at `+0x3630` drops the hunter to the ground, and that search
tries higher and further five times. The world isn't loaded in the menu, so water has to wait until then. `+0xd4540` fills in what's under a place,
with the water's surface first, its depth second and the ground third. The player calls it every frame. Longshot checks the marker with it just
before the opening runs, and if it's over 0.3 m deep it looks outward in rings, 12 places every 15 m up to 450 m, for the first dry spot.

**The opening shot** is `+0x3630`, which only runs when the mode at `+0x24` is 1, the free hunt. It points a camera at the nearest
animal within 700 m and writes "About %d yards north from you...". It's skipped by turning the check into a jump.

**Dangerous animals** has a bug in the game. The menu only writes the setting into the hunt when the box is clicked, so a box that was
already ticked when the menu opened does nothing. Longshot reads the saved value instead, at `+0x94f31`.

**Animals** are all made at load, and none ever arrive later. The generator at `+0xbe4b0` writes herds into a definition pool, at
start points from 175 m out, and `+0xbd600` turns those into animals. Longshot reuses both while a hunt runs, about every minute at
most, on the main thread at the start of the UI:

1. Count living animals within 1000 m by species, against how many the hunt started with. A dead one has `+0xbe7` set.
2. Take the species furthest short, and a start point 500 to 1000 m out, away from the view if there is one.
3. Run the generator for one herd, so sizes, antlers and moods are natural. The name format at `+0xbe874` gets an `sp12_` prefix first,
   because herd mates find each other by name and each run starts from 00 again.
4. Move the herd to that point, keeping its spacing, and do what `+0xbd600` does for each animal. That function can't be called
   itself, as its second pass pairs the list with every animal from the first. An animal's clock at `+0x754` starts at 0, which only
   suits a hunt that's loading, so it's set to the time now. Otherwise its first update would count the whole hunt as one step.

A hunt has 11 goals at `+0x10`, a kind and a number each, and a list of bytes for each at `+0x80`, 0x10 bytes apart. A goal of kind 1 keeps a byte
for every animal, by its place in the manager's list, which is the index `+0xbd480` returns and the animal's trophy keeps at `+0x114`. Those lists are
sized to the animals when the hunt starts (`+0x92c9b`), so animals brought in later are past the end. A hit on one then stopped the game, as the
hit's scoring at `+0x8fa80` looks the first goal's list up by it, and an index past the end aborts at `+0x8fb83`. After each herd arrives, Longshot
grows every kind 1 list to the animals with the game's own resize, `+0x7e00`, and zeroes the new places, as the hunt does.

With every tag used, the hunt says its goals are accomplished, and to end it (`+0x90696`). In a free hunt that's pointed at Longshot's own text,
which says the bags are full, to stash them at a camp or to leave the hunt.

It stops at 90 alive within 1000 m, or 320 listed. Animals left far behind don't count towards the first, or none would ever arrive
as the hunter moves on. If anything faults, it stops for the rest of the run rather than risk the hunt.

`game.def` is only held while a herd is made. A resource still held when the game quits stops its memory manager from shutting down, with
"[mem] Number of safe iterations exceeded", and the game crashes on the way out.

**Distance.** The generator skips start points within 175 m of the hunter, and calls those within 1000 m near. It gives the first herd the near point
closest to the hunter, on purpose, which is who the opening shot shows, and the rest random near points. Animals smell up to 300 m and hear up to 250 m,
so a herd at 175 m can sense the hunter the moment a hunt starts. Longshot moves the 175 at `+0xbe436` to 450 m. Simulated on the Northern Rockies' start points,
that leaves the nearest animals a median 465 m away, about 4 minutes on foot, with about 5 groups within 700 m. Trying 800 and 1700 m put the nearest 810 m away,
a 7 minute walk, spread too thinly to find in play.

**Why animals bolt.** Each animal picks the biggest of its needs at `+0xb5d70`, and one of those is danger, which is stress plus its fear of the hunter and
of other animals. Every generated animal starts with 0.3 stress, which takes minutes to fade. Once it has eaten and drunk, that's the biggest need,
even with nothing around. Danger then runs `+0xb5bf0`, which picks goal 0xb when fear of the hunter is low. That goal is to flee from the hunter's
position, at a gallop, however far away and whether it has noticed or not. So free hunt animals now start with no stress, and `+0xb689d` hides what stress
they gain while both fears are below 0.1, so it only counts while they're reacting to something. Fear fades slowly, so a scared animal stays wary.
Prey still runs from predators it notices within about 70 m. That's real behaviour and is left alone.

## Stashing

Bagging puts an animal's trophy (its `+0x144`) in one of the profile's tags, at `+0x90420`. The tags are a list at the profile's `+0xb0`, each with its trophy at `+4`, and once they're all used a free hunt can't bag another. After the hunt, the trophy room takes each trophy out with `+0x96df0` (at `+0x40ab3`) and holds it. Move to Trophy Room then puts it in the profile's trophies, the list at `+0x20` of its `+0xac`, with `+0xf3e70` (at `+0x3ff7f`). Closing the room saves the profile with `+0xd2450`, which the hunt calls itself too.

What the interact key can do is a list on the hunt (the current game's `+0xa8`) at `+0x140`. `+0x929d0` adds to it a provider, whose `+4` scores it and whose `+8` does it, a label, which is the number of a template in `def/ui/txt.def`, and an argument. Every frame each one's scored (`+0x93057`), the list's sorted with the lowest score at or above 0 first, and that one's label is shown. The key (`0x8f`, read at `+0x13244` on foot) does it (`+0x93140`). An animal to bag scores its reach squared less how far off it is, squared.

In a free hunt Longshot adds a stash to that list, with a label added after the game's 65. It scores a million when you're on foot at a camp with something bagged, and below 0 otherwise, so anything else on offer goes first. Camps are read as the landscape loads, from the map's `world_units_filename` in its `world.sii`: every `body_inst` whose `body_desc` is a house, cabin, hut, tent, campfire, garage, shed, barn, roofed shelter, ruined house, railway station, pickup or Land Rover. Each hunting map has from 2 to 13. A camp reaches 10 m past its model's box, turned the way the model is placed. The box is in the model's `.pmg`, at `+0x24`, after the version and the bounding sphere: a station is 9 by 33 m, a pickup 2 by 5. The lighthouse isn't a camp, as its box takes in a beam 100 m long. Stashing does what the trophy room does with each trophy, saves the profile, and puts up a message with `+0x816a0`, the way bagging does. The results at the end of the hunt only list what was bagged after the last stash.

## Wildlife

**Herds.** `freehunt_counts` is a count of animals, not herds, and `maximal_herd_size` isn't a herd's size either. The generator at `+0xbe4b0` adds
up each slot's count, and scales them all down to 40 if that's more (`+0xbe5a8`, then `+0xbdb90`). It splits each slot into males and females, then
hands them out in groups of `(0.05 + 0.95 * random) * maximal_herd_size`, rounded down and at least 1, each at its own start point on a 10 m circle. So the old 2, 3 and 5 with
the default of 4 made groups of one to three. `game.def` is rewritten as it loads with new numbers for each species, and the limit goes to 127, as its
check only has room for a byte. The roll for a group's size at `+0xbe944` is squared, which gives many small groups and the odd very large one. A count list
can be any length, and the slot's number picks from it, so the entries after the three densities are the numbers 1 to 15. That's how the spawner asks for a herd of an
exact size. It also makes that roll 1 while it does, so the herd stays in one group.

**Young.** Every species has a type with `gender` 3 and a variant in its model, but the generator gives that gender a count of 0 (`+0xbe81e`). So they're made by changing
generated animals before they're created, which is just their `type` and `weight`, plus `mate` so they follow the mother. Males with antlers are left alone. Females of buffalo, goats,
elephants and bighorns have horns, and so do their young, from `antlers_young.sii`. That's a shorter list, one shape for goats and two for elephants, and a `main_beam` past its end is an array overflow at `+0xca4dd`
as the animal is made. So young always get `simple_beam_part0`. Bears and cougars live alone or as a mother with up to two young, so anything else in their group changes to suit.

**What an animal does.** `+0xb66a0` is run for each animal. It works out its needs from the fields below, picks the largest at `+0xb5d70`, and runs the goal that goes with it.
Longshot changes those fields around the call, and puts back the difference, because the goal it runs can change them too.

| Brain offset | | Need |
| --- | --- | --- |
| `+0xdc`, `+0xe0` | Fear of the hunter, and of other animals | Danger, with `+0x124` stress |
| `+0xe4` | The pull of prey | Hunger, times `+0x110` |
| `+0x110` | Hunger | Hunger |
| `+0x114` | Thirst | Thirst, as it is |
| `+0x118` | Tiredness | Rest |

**Water.** Thirst is goal 5, and hunger goal 2. Both ask `+0xbb460` for the best of the map's `attract_area` units, kept at `+0x8c` in the animal manager, and walk there. Each area has a
resource at `+0x2c`, 0 for food and 1 for water, and scores 50 times the animal's hunger or thirst, over how far away it is. An area counts as food or water depending on which the animal wants. So animals have always
gone to water, but thirst starts at 0.1 to 0.4 and has to beat everything else. Longshot gives each herd turns of being thirsty, using the mate's address so they go together, with hunger held at 0 so nothing else wins and the score
goes to water. Every map has water areas but Alaska, which has none, so animals there keep to food. New herds arrive near water half the time, if there's anywhere to.

**The truce.** A hunter that's thirsty, and for 150 seconds after, has its hunger, the pull of prey and both fears hidden. Hunger alone doesn't start a chase, but the pull of prey being at least `+0x104` does, at `+0xb4a2d`. Danger is what starts an
attack, at `+0xb5bf0`, so hiding the fears stops that. It ends for 90 seconds if the animal's hurt (`+0xbe6`), a shot goes off within 60 m, or the hunter's within 15 m. With nothing to do, it wanders, which is
what moves it on from the water.

**Time of day** follows the clock, below. At dawn (5 to 9) and dusk (16 to 20), hunger counts for 1.4 times and tiredness half, and animals are thirsty for longer. In the middle of the day (11 to 14) hunger
counts for 0.6 and tiredness twice, and turns at the water are further apart. At night (21 to 5) it splits: grazers get 0.5 and 2.5 with the longer gap at the water, and hunters 1.6 and 0.5. Most birds roost, leaving 3.

**Cost.** The manager's tick at `+0xbcd50` gives two animals a heavier look each frame, then calls every animal's `vf34`, `+0xc3f40`, whatever its distance. On one PC 114 animals took `sim` from about 0.5 ms to between 3 and 8 ms, while `world` didn't change, as most are out of
view. That tick takes the seconds passed, and everything in it is worked out from that, so one that's skipped can catch up. So beyond 130 m it runs 30 times a second, beyond 300 m 15 times, and beyond 600 m 8 times, or 30 for anything in view through a scope. They're all in the three
tables with that method, `+0x1a4ca4`, `+0x1a561c` and `+0x1a6184`. Every 30 seconds the log says how many ran and how many were saved.

**Panic.** An animal that isn't a hunter, with fear of the hunter over 0.35, raises an alarm once a second, at 0.75 of its fear. Any other within 60 m takes that on as fear of the hunter. So it spreads through a herd, fading as it goes, and dies out after a few animals in a row. Danger
then sends them running from the hunter's position. Fear of other animals isn't passed on, as it would become fear of the hunter, and `+0xb5bf0` lets anything with aggression at `+0xb8` attack the hunter on that, from any distance. Past 100 m that aggression is hidden, so it runs. An animal being hurt (`+0xbe6`) or killed (`+0xbe7`, looked for twice a second) raises an alarm at full strength.

Danger wins at `+0xb5ed0` when it's the biggest need or past 0.8. Being thirsty is 0.95, so fear that's been passed on, which is 0.75 at most, never beat it. So any animal that's frightened, or a hunter that's provoked, stays away from the water for 5 minutes.

**Night.** The senses at `+0xb59c0` multiply what they find by three numbers in the object at `+0x10` in the animal manager: smell at `+0x10`, sight at `+0x14` and hearing at `+0x18`. The call at `+0xb6868` is wrapped, and changes those around it for each animal. Darkness goes from 0 to 1 between 19:00 and 21:00, and back between 5:00 and 7:00. Smell fades with it, leaving hearing as it was. Sight only reaches 18 m in the dark, or 30 m if the hunter has a light on. The game already scales sight by the light level, which at night is close to nothing, so up close that's divided back out, and an animal sees about as well as it would by day.

**Lures.** Calls, scents, decoys and carcasses all work the same way. `+0xba910` reads a list of who's drawn from a def, `+0xba1f0` adds it to the list at `+0x40` in the animal manager, and each animal adds them up at `+0xbc9c0`. That gives an appetite for food at `+0xfc` and mating at `+0x100`, smoothed into `+0x104` and `+0x108`, and the strongest one's place at `+0x44c` and `+0x458`. A mating need is `+0x108` added on, and over a random number, `+0xb4b00` starts goal 3 with a 1, which walks to that place. That goal is shared by every animal, predators included. A visual lure stops at 90 m, and lives on an item, so a light skips all that and writes the pull and the place itself, around `+0xb66a0` like everything else here. It works from 21:00 to 5:00, within 100 m of a lantern, or 150 m and within 25 degrees of a beam. The goal only reads the place as it starts, so its stage is set back to 0 every 4 seconds for it to look again.

An animal being drawn in that has any sight of the hunter at `+0x130` gets real fear of 1, so it runs, and raises the alarm through its herd, and won't come again for two minutes. On a tree stand sight is 0, so they don't. A predator takes that fear the way it takes any other, which can mean attacking.

**Running away.** Danger from the hunter is goal 11, which is goal 12, running from something, aimed at the hunter. It asks `+0xb1d90` where to go, from `+0xb201b` and again at `+0xb20b9` whenever it gets there or gets stuck, and then starts a new path.
That path is only made for a place within 110 m (`+0x4b0`, squared at `+0xc1334`). Past that the movement says 1, which asks again, every update, so a place further off has it turning on the spot and costs a new path each time. That picks 50 to 100 m off, up to 45 degrees either side of straight away, fresh each time, which is the zigzagging and circling, and with rocks and trees in the way it could end up heading back past the hunter. It stops 500 m away. Longshot answers instead: each herd gets one line, kept for 25 seconds at a time unless the threat moves 40 m. It tries straight away, then 20 degree steps to each side up to 80, and throws out any that are over 0.4 m deep 60 or 120 m along. Of the rest, climbing counts for a little against being straight away, as `+0xd4540` gives the ground's height too. The place is 90 m along that line from where each animal is. A herd with no dry line is cornered. One that can fight, within 40 m, has its aggression at `+0xb8` set to 1 and its goal's stage at `+0x248` set to 3, which makes `+0xb5d70` pick again, and `+0xb5bf0` then attacks. Anything else goes straight away regardless. None of them heads for the hunter to get away.

**Attacking.** Goal 13 goes after the hunter from `+0xb25a9`. Within 75 m (`+0xb265c`) it charges, with the `attack` processor. Further off it
walks with `move`, at a pace of its distance over 300 m, so slower as it closes: a quarter at 75 m. Past 300 m it gives up. Longshot starts attacks
within 100 m, so every one crept the first stretch. It charges from 150 m instead.

**Lines.** A herd keeps one line, but an animal it would take towards what it's running from, which is one on the far side, takes its own. The log
counts those, and, for anything running, full circles within 20 seconds and running at the hunter from within 60 m without attacking, with the
goal, stage, processor and movement mode of the latest, and for a circle, what it's running from and how far, how many new places to run to it was given meanwhile, and how far off the one it's heading for is. The first hunt with this put every circle on animals running away, 400 to 600 m from the hunter. Speed is measured over half a second, as distant animals only move on their own ticks.

**Places to go to.** A place to run to is at `+0x49c`. Running away (movement mode 1, from `+0xc125b`), it's reached within the near distance,
20 m for running away, and walking (mode 0, `+0xc11ec`) within 1.1 m, both measured in three dimensions, height included. Longshot's places
along a line were left at the height the animal started from, so 90 m along a mountainside one could be 20 m above the ground or under it. It
could never get close enough, never asked for another, and ran round and round underneath. The second hunt with the circle log showed it: every
circle was running away, with no new place asked for during it. Places to run to and beds now take the ground's height there, from `+0xd4540`
looked at from 120 m above.

That wasn't all of it. The next hunt still had goats running from predators round places 50 to 80 m off, on the ground, and never reaching
them. So anything running away that turns three quarters of a circle without being given a new place is given one itself: 40 m off, about
60 degrees to one side of its line away, alternating sides. It's written to `+0x49c`, and `+0xc1050` sets the movement going again, as
the goal does after picking a place itself. The log counts how many went round in circles and how many were sent somewhere else.

**Senses.** `+0xb59c0` works out sight, smell and hearing of the hunter, and adds them, by the seconds passed, to how aware the animal is. Sight
stops at 90 m (`+0xc2c17`), smell at 300 m and hearing at 250 m, so an animal further off than that can't notice the hunter at all. What
sets one off is a predator, a shot, a bullet going past, or fear passed along its herd. Anything that starts running away while it's in
the binoculars' view gets a line in the log, with its fears of the hunter and of other animals, and what it's running from and how far.

**Mountain goats.** They live up high. As the hunt starts, each goat herd moves to the best ground within 200 m of where the generator put it,
and one that arrives later to the best within 250 m of its spot that's still 500 m from the hunter. Best is highest, with steepness worth 25 m of
height for a slope of 1 in 1, measured 10 m round each place, looking at 16 directions every 25 m out. Each goat keeps its home, which moves to
anywhere at least as high that it stands calmly. While calm, not thirsty and not drawn to anything, one more than 70 m from home walks back. Goats
drink a third as often as anything else, and running away, climbing is worth 1.2 against 0.35 for everything else.

**Drinking.** Bears have no animation for drinking or grazing, so at the water they stand. They drink for 40 seconds at a time. A hunter drinking,
or wandering after, ignores the hunter unless provoked, and seeing or hearing the hunter within 60 m now provokes it.

**Sleep.** Tiredness is goal 6, `+0xb1290`, which stays put with the `rest` processor (index 0 of the names at `+0x1e1560`). From 21:00 to 5:00 a grazer that's calm picks a bed within 50 m, dry, higher and further from the hunter, or one around wherever the animal it follows chose. It walks there the same way it would to a light, then rests with its tiredness held at 0.9 and no turns at the water. After 30 seconds still on that goal it's asleep. The senses at `+0xb59c0` get no sight from it, and hearing (`+0x18` of the manager's object, and kept at `+0x138` of the brain) at a third. What it hears each second wakes it a little, a shot by how close it was, up to 400 m, and anything that frightens it, hurts it or has a bullet pass within 8 m wakes it at once. A bullet passing that close frightens anything. The animal a herd follows stays awake.

**Wounds.** Hurt is `+0xbe6`, which `+0xb5f29` reads as the goal's picked, and then nothing else gets a look in. So it's hidden while picking, around `+0xb66a0` like the rest, and put back. A wounded animal runs with full fear until it's 150 to 300 m from the hunter, then lies up with its fears hidden. Coming within 45 m, being seen, or being heard at more than 0.3 sends it on another 120 m.

**Young.** A bear or cougar with young following it, within 40 m of the hunter or of them, gets 0.9 fear of the hunter and aggression 1, so it attacks. Danger only picks between running and attacking when it's first picked, and at 0.9 it always wins, so her goal's stage is set to 3 until she's attacking, or one she was already running away on would carry on. The generator only sets `mate` on the animals after the first in each group (`+0xbe91b`), and `+0xc57e0` skips an empty one, but the first in Longshot's family may not have been the first in the generator's. So hers is cleared, or she'd look like one of her own young.

**Contact.** Each frame Longshot works out how fast each animal's moving from where it was. One within 1.6 m that's coming at 2 m/s or more knocks the view along its way, harder up to 8 m/s, tipping it back for a knock from the front and rolling it for one from the side. The foot view's `+0x20` and `+0x2c` take it after the head turn, on springs that settle in about half a second.

**Running.** An animal moves by its animation. Each tick (`+0xc3f40`) takes how far the animation's root moved (`+0x173f00`, at `+0xc3fe7`), times the animal's size at `+0x344`, turns it to face the animal's way, and adds it to where it is. The animation goes on by the seconds passed times `+0x3b0`, which is 0.9 to 1.2, picked as it's made (`+0xc531a`). A `.pma` ends with the root's move for each frame, and from those, running is slow: a whitetail's run covers 5.9 m/s, an elk's 4.8, a bison's 3.4, a black bear's 3.5 and a mountain goat's 3.3. While it's in the `attack` or `run_away` state (4 and 5 of the names at `+0x1e1560`, kept at `+4` of `+0x75c`), Longshot raises `+0x3b0` for the tick, up to 2.2 times, and stretches the root's move past that up to 1.6 times, to reach a top speed for each kind: 13 m/s for deer, 20 for pronghorn, 17 for cougars, 18 for lions, 12 to 14 for bears and bison, and 15 for wolves. Nothing's made slower.

Turning is `slerp_generic`, at `+0xc41c0`, from the way it faces to the way it's going, by the seconds passed (1.5 times that in one state). That's about a second to mostly come round. It's the only call through that import here, so it goes through Longshot, which takes 2.5 to 8 times as much off each tick running, the most for cats and wolves, and less of that walking.

Each kind can sprint flat out for 12 seconds (cougars) to 60 (wolves and pronghorn), slows down to 0.6 of its top speed as that runs out, and gets it back resting over 45 seconds. A hunter that's run itself out gives up on you or its prey for 40 seconds. A wounded animal runs at 0.7 and tires faster, and young at 0.8, which their mothers keep to. Agile kinds cut 70 degrees off to the side for about a second when what they're running from (at `+0x480`, whose second method says where it is) is within 25 m. Bears, bison, moose and elephants sometimes bluff, stopping short or veering off 8 to 20 m away and leaving you alone for 30 seconds.

**Blood.** A wounded animal drips at `+0xc16b0`: while `+0xbe9` is set, every 10 over its bleeding at `+0xbf0`, times 2 to 4, seconds (or 20 when barely bleeding), it calls `+0xad0c0`, which drops one stain within 0.4 m, onto the ground. A hit drops one more (`+0xc6561`). The stains are `blood_trail` in `sticker.def`, 0.16 to 0.25 m, at most 50, 200 and 400 of each. Longshot drips with 4 instead of the 10 (`+0xc16e6`), drops 1 to 6 at a time by how hard it bleeds, so lung shots leave the most, throws 6 where it's hit, and 10 for a pool where a wounded animal lies up (2 more every 20 seconds) or something dies. The stains are 1.6 times as big and three times as many are kept.

Blood used `dry.tex.fog.nc.decal.over`, blended over the ground with no lighting, so it shone at night. Only the 6 blood materials use it. They're given `dry.tex.fog.nc.decal.mult` instead, whose state, `nc_xx_x_decal_mult.rs`, nothing else in the game uses. That's rewritten to multiply twice, `dst_color` by `src_color`, and to drop what's under half opaque, as the stains' clear texels are dark red rather than black. So a stain is the ground made red, as dark as the ground. Materials and effects are opened by `gfxshared.dll` and `loaders.dll`, which get the same file hook as `p3shared.dll`.

A bleeding animal marks where it is every 2 seconds. A hunter that isn't busy goes for the newest mark it can smell, within 250 m and 3 minutes, until it's within 25 m, when its own hunting takes over.

**Tracks.** Each tick adds how far it moved to `+0x2e4`, and at `spoor_dist` times its size (`+0x2e8`), `+0xacf00` puts a print down, on soft ground only, through `+0xf3670` at `+0xad0a4`, from the trail at `+0x2f0`. Longshot stamps each one twice, which darkens it more as they're subtracted, makes them 1.7 times as big, puts them 0.6 of the way apart, keeps three times as many, and spaces them 1.8 times as far running. The hunter leaves prints with `hunter_trail`, which is in `sticker.def` but never used: `+0xacf00` only reads a handful of fields, so it's handed a block with just those.

**Calls.** An animal has 13 sounds, loaded at `+0xb02f4` from the keys at `+0x1e13c0` into the list at `+0x31c`: two footsteps for each of dirt, grass, leaves, snow and water, two for falling, and `s_specific`, its call. `+0xae1b0` plays the call where the animal is, if `+0x381` is set, which `+0xae2e0` sets when it's within 200 m (40000 at `+0x1a2044`, compared at `+0xae329`). The call is heard from 25 m down to nothing at 200 m (`+0xb04f2`). The only time the game plays it is answering a call item, at `+0xb6c2f` and `+0xb6de8`, with the `call` animation (0x28, through `+0xb3680`), with the species' `call_back` chance from `brain+0x74`. So without calls nothing's ever heard. Boars, cougars, elephants, kudu, lions and wolves have `an_missing.ogg`, which is a second of silence, and red deer point at two files that don't exist. Both the distance check and the sound reach 400 m now. Wolves get the howls from the north maps' ambience, and red deer the elk's calls. Then each species with a voice calls in alarm, once per herd every 30 seconds, as it's hit, as a bear comes for you or defends its young, and now and then to keep in touch, anywhere from every 2 minutes for bison to every 15 for deer. Males of elk, red deer, moose and turkeys call four times as often at dawn and dusk. At night a wolf leading a pack, or alone, howls every few minutes, and the rest join in within 4 seconds. Nothing calls while asleep or in danger.

**Music.** The hunt's `vf30` at `+0x2ed0` plays one of five stings from `script/scenic_music`: 0 target sighted, 1 an animal attacking an animal, 2 an animal attacking the hunter, 3 and 4 for openers. The game plays 1 at `+0xb27a0` whenever any hunter starts on anything, with no check of how far away that is.
It always did that, but it took a lot more animals for it to come up every few minutes. Longshot drops that sting, so the chase music is 2, at `+0xb15be`, which only plays for the hunter.

That one comes at the start of goal 13, which `+0xb5bf0` can pick for anything with any aggression at `+0xb8` and more than 0.2 fear of the hunter, however far away. Hearing reaches 250 m, so that can be something a long way off in the dark, and the music has nothing to do with what you can see. So the sting's held back past 80 m, and played if that animal gets within 80 m still on that goal. If it gives up, it never plays.

Once a second, `+0x939f2` looks for any living animal within 200 m (40000 at `+0x93b16`) that's on the hunt's list of targets, and 0 plays at `+0x93ca6` as that goes from none to some, with no check of whether it can be seen. With this many animals that can be the first second of a hunt, so it's dropped in free hunts too. Missions keep it. 4, the opener at `+0x95172`, is left alone. Every sting goes in the log, with the animal and how far away it was. Birds aren't involved in any of this, as they're `ambient_animal_bird_u`, run by their own server, with none of the needs and goals of an `ai_animal_u`.

**Carcasses.** Each species' `carcass` section lists who it draws, and `effect_range` in `carcass_base` how far. Those go from 0.3 to 0.9 and 400 to 900 m. Vultures come from the birds: their chances are a running total at `+0x58` in
the server, and the rest are scaled down while the hunter's within 400 m of something that's been dead for under 5 minutes.

**Birds.** Each map's `ambient_animals.sii` sets how many, how often and how far away. The server's at `+0x30` in the animal manager, with a timer at `+0x68` that runs to the period at `+0x64`, and the most allowed at `+0x80`. A shot, an alarm nearby or
the hunter moving at 7 m/s shortens that for 1.5 seconds. These go up in the air where the server would have put them anyway, so it isn't birds lifting off the ground around you.
They're rewritten by the same hook as everything else, but `load_unit_tree` opens files from game.dll, so game.dll's import has to be patched too.

**Water.** A bullet's flight is the first method of its listener at `+0x1a8480`, `+0xd4920`, which casts a ray at `+0xd4a83` with `+0xd4430` and goes through what it hit, nearest first. Some kinds are passed through, and anything else stops it. A hit's 0x20 bytes have what was hit at `+0x1c`, and `+0x6c` of that is its kind. Kind 1 is water, which `+0xd6f6b` only throws up a splash for. So water's taken out of that list, and the bullet goes on to whatever's in or under it. The rangefinder's ray, at `+0xd4f03`, is left alone.

A thirsty animal walks to `+0x8` of the water area, and stops within its `+0x14`, both filled in by `+0xbb3d0` at `+0xb1c8a`. That's often well into a lake. When it's deeper than 0.4 m there, the place is moved back along the line to the animal, a metre at a time, to the first spot that isn't, and it walks right up to that.

**Trees.** A bullet that hits an object gets surface 15, or 8 if it's a plant (`+0xd6fd2`). The table of impacts at `+0xd68b8` has nothing for 8, so it points at dirt now.

## Day and night

The game has a whole clock. The environment (`+0xe6080`) keeps seconds into the day at `+0x84`, and its tick adds ten times real time unless `+0x88` is set, which some kinds of hunt do as they load (`+0x95228`, depending on the mode at `+0x24`). Everything else already reads that clock each frame:

- The sun (`+0x68` in the environment) takes the time from it at `+0xec960`, and interpolates its `ambient_levels`, `diffuse_levels` and `specular_levels` by the minute. These are 24 entries of 4 floats each at `+0x24`, `+0x34` and `+0x44`.
- `+0xec210` sets how high it is, from 20 to 60 degrees between 5:00 and 12:00 and back by 21:00. It never goes lower.
- The sky is ordinary `hu.dif` material, so it's lit by those levels like anything else, and fog is multiplied by them.
- `+0x94` in the environment is how much light there is, from the fog. Animal sight is scaled by it at `+0xb5a11`, so they already see less at dark.

So in free hunts Longshot sets `+0x88`, so the two can't add up, and writes `+0x84` itself to take `free_hunt_day_minutes`. A game minute is the smallest step the light takes, so at the default that's every 2 seconds, too small to see.

Hours 22 to 4 have no light at all, which would be pure black, so ambient gets a floor of moonlight once the hunt starts. Only ambient, as diffuse light would come from a sun that's still 20 degrees up. That's still up there at night and would show as a faint disc, as `+0xac` in the sun, how much of it is drawn,
comes from the light level. So from 21:00 to 5:00 that and its dazzle, `+0xcc` and `+0xd0`, go to 0 just before each frame is drawn, after the tick has set them.

`@include`d files are read inside p3core, not through the imports Longshot hooks, which is why this works on the loaded table and not on `sun_normal.sui`.

The hunt's tick also ends things at 21:00, with "Hunting time over" at `+0x93cef`, which is checked at `+0x93cd8`. In a free hunt that branch at `+0x93ce3` always jumps past it, and it's put back for any other kind of hunt.

Nothing reached 21:00 before, so a bug in the game never came up. Its lists of names for each hour, at `+0x1e57c0` and `+0x1e5818`, are missing the commas after 21, so `@@g_clock_21@@@@g_clock_22@@@@g_clock_23@@` is one string and each list is 22 long. So 21:00 translated as three times run together, with a stray number filling in the second `%02d`, 22:00 read "clear" out of the next table, and 23:00 read a float as a pointer. Each list is only read in one place, `+0x101ca4` and `+0x101d04`, so both read Longshot's lists instead.

## Lights

The engine has one light, the sun, with no way to add another. So the flashlight and lantern are a pass over the finished picture, like the rest of post-processing. It multiplies each pixel, by how much light a surface has now against what it would have by day, so dark textures stay dark, and by day there's nothing to add.
Dividing by the moonlight's colour takes out its blue, too. The depth of the world says how far each pixel is. A beam fades with its angle from the middle of the view and with distance, and a lantern only with distance. Nothing about the game's materials had to change, so it lights grass, trees and animals the same. As it has no normals, surfaces don't catch the light any differently by which way they face.

The depth has to be copied at the world's own flush of the queues, `+0x102f99`, because the weapon's at `+0x102fea` comes after a clear. The projection is the last one set up on a full window rectangle by then, for the same reason.

Items take two steps. Choosing one is `+0xd8e0`, with the item's name, which is the name of its def. The hud calls it from `+0x81453`, `+0x88072` and `+0x88425`, and `+0xe262` is the same thing unless that item's already selected, in which case it's the second step. Choosing puts down any item that's up, comes back from `+0xde1c` once it's down, and selects the new one with `+0xa1130`, which also tells the hud. Nothing's raised yet. That's `+0xd740`, using the selected item, from the item key at `+0xdb42` or `+0xe255`. It puts the gun away first, coming back from `+0xddf3`, then calls the item's `+0x3c`. `+0xd930` looks like choosing, but it's for weapons.

The hooks look for the two names on both steps, and flip the light without letting the game go any further, so no item is raised, the gun doesn't move, and a light stays on through binoculars or anything else. Choosing a light still selects it, so the item key goes on flipping it, unless another item is up. Taking that out of your hands would strand it.

`+0x16420` plays a sound for a menu, by its path. It takes the menu in `ecx`, but only reads `+0x80` of it to see if it's been made quiet, so an empty block will do. It doesn't check that the sound loaded, so the path has to exist. The switch uses the .44's misfire, as `click.ogg` is also what you hear cycling through your items.

## Vehicles

The ATV and the horse are the only vehicles. The horse moves by animation, so its speed is in its `.bhv` data.

The ATV isn't driven through its wheels, whose friction is zero. The tick at `+0x98510` pushes on the rear hubs with a
fixed force, 600 N each on 340 kg, which stalls at 21 degrees. It stops pushing altogether once the speed reaches 25 km/h.
That counts falling too, which is why it took a cliff to go fast. With no throttle, on the ground, it loses a fifth of its
speed every tick, at 40 ticks a second, which would stop it dead at any real speed. None of this depends on frame rate:
physics runs at a fixed 40 Hz. Throttle is on or off. Reverse shares the cut-out, but with only 400 N.

The tyres do hold it sideways, by cancelling sideways speed at each wheel, up to 0.8 m/s a tick, as a force on the wheel of
the chassis's mass times that speed times 10. With zero friction, that's all that holds it sideways, on hillsides and over
bumps as well as in turns. Up to 2.4 g of it goes into cornering at full lock, so with any real speed a hard turn flips it,
and the rider dies when it lands upside down. At 25 km/h it never mattered. Limiting it to 0.22 m/s, 0.65 g, stopped that,
but left the ATV loose. A 30 degree hillside takes 0.5 g of that just to hold still, and a sideways kick from a bump took
twice as long to settle. It's now 0.4 m/s, 1.2 g, and what stops the flipping is steering that asks for less, below. The limit
and the test for reaching it are separate: changing only the limit leaves full grip for any slide under 0.8 m/s, and the
test's values are shared with other code, so its operands get pointed at Longshot's own.

Steering goes to 30 degrees at 50 degrees a second, whatever the speed. The wheelbase is 1.12 m, so that's a 1.9 m circle, or
12 g at the top speed. The fronts can't give that, so they wash out and it runs wide. The rears have the longer lever, so it
doesn't spin. The controls method, in the slot at `+0x1a28a8`, clamps to the float at `+0x1ee5e0`, so Longshot sets that for
the length of the call, to the angle at which a turn takes 9 m/s2. That's all of it below 15 km/h, 12 degrees at 25 km/h and
2.5 degrees at 55 km/h. The handlebars and rider read the same float elsewhere, which is why it's put back afterwards. A model from
above at 40 Hz, with these rules, gives a 30 m circle at the top speed where it was 41 m, at 25% of the load that tips it on
flat ground, against 19% before and 66% for the game's grip, which flipped.

The controls start each frame in gear 2, for no keys. Forward is gear 0 and back is gear 1. Slowing with no throttle only
happens in gear 2, so holding back turns it off, and leaves reverse's 800 N, or nothing above the cut-out. That's 2.4 m/s2, so
back took 48 m to stop from 55 km/h where letting go took 18 m, and on a slope of over 14 degrees it couldn't hold the ATV at
all. With zero friction it also never stops rolling downhill: at 2% a tick with no keys, it settles at 12 m/s times the sine
of the slope. Longshot replaces the pointer to the tick, handed over at `+0x9a62e`, and first takes speed off anything on
the ground: 10 m/s2 for a key against the way it's moving at over 0.5 m/s, and 2 m/s2 with no keys, which stops it on 12 degrees.
Below 0.5 m/s back is reverse again. That stops it from 55 km/h in 1.2 s and 9 m, and down a 30 degree slope in 2 s and 15 m.
Speed is taken off the body's whole velocity through `+0xbf30`, as the game does, so it can't pitch the ATV over its nose.

It bounces for three reasons, which are worse the faster it goes. The suspension, set when the ATV is made at `+0x9b21c`,
is ERP 0.7 and CFM 0.0007, which comes to a 40000 N/m spring with 429 N·s/m of damping. On 85 kg a corner that shakes about
3.5 times a second with 12% of the damping it takes to settle, so it's still going when the next bump arrives. The wheels
are hard balls on hard ground: the game never touches the world's contact settings, which stay at ODE's defaults, ERP 0.2,
no limit on the correcting speed and no surface layer. At 55 km/h they cover 38 cm a tick over ground made of flat facets,
so any ridge throws one off the ground. And nothing is left to soak it up, since the tyres are the same material as a rock
to the solver.

Strong ATV gives the suspension 15000 N/m and 1500 N·s/m, about 2 shakes a second at 66%, which settles within one swing.
It also makes each tyre contact give, at 20000 N/m and 1200 N·s/m, with the bounce flag cleared. Tyres are told apart by
material 5 and group 6: a shape's material is at `+0x68` and its group at `+0x6c`, and the map's own shapes can have
material 5, but in group 0. The contact hook in `walking.cpp` passes them to `vehicle.cpp`.

Softer springs sit lower, and that has its own cost. The chassis collision is one capsule, from `vehicle.pmc`, whose
underside is level with the hubs, 28.5 cm above the ground with 0.28 m tyres. The game's spring used 2 cm of that at rest,
but the soft spring and tyres use 10 cm, and a hard hit took the rest. Only the chassis is wired to the handler at
`+0x99370`, which plays `coll.ogg`, `coll0.ogg` or `coll1.ogg` and raises dust on any contact above 1 m/s, so it came out as
a smack and a puff of smoke.

So the wheels start 10 cm below where they belong, which preloads the springs: the ride doesn't change, and the ATV rests
3 cm above the game's height, with the worst dips in the model no lower than the game's own suspension reached. A wheel's
height is `y0 + travel * 0.5` at `+0x9b11f`, from a `.pma` with two frames, but all four animations have identical frames,
so the travel is zero and changing the 0.5 would do nothing. The load and the multiply get replaced with a load of the drop.
The arms read `y0` and the travel on their own, so they don't care. A bump stop in the tyres also kept the chassis up, but
gave back most of the jolts.

That cut the touches but couldn't end them, and firmer springs don't either. A side-on model of the whole ATV puts nearly
all of them at the nose, where the capsule reaches 40 cm past the front axle. The chassis has the inertia of a box 4 m by
3 m, 231 kg·m² in pitch, so it's slow to follow the ground. Doubling the spring rate barely changed how often it touched,
and the game's own suspension touched about as often at the same speed. What gives them away is the handler: it goes by
the ATV's speed, so at 55 km/h a graze counts as a crash, and the model's typical touch closes on the ground at under 1 m/s.
So the check at `+0x993d1` reads a value that Longshot sets for each contact. It stays at 1 unless the contact faces at
least halfway up and the chassis is closing on it at under 5 m/s, measured at the contact from the body's velocity and
spin, in which case nothing plays. Trees and walls crash as they always did. The log has a line for each touch. A minute
of driving at speed gave 43 touches, 38 of them ahead of the centre, closing at 0.9 m/s in the middle case, and only one
over the limit.

The capsule is off-centre, so the game makes it a geom inside a transform geom, and contacts name the inner one, which has
no body. Only the outer one has it, so the speed comes through the shape's own pointer at `+0x88`. If it can't be read,
the way the contact faces still decides, so a bad read can't turn a graze back into a crash.

Lifting it doesn't bring the rolling back, because the tyres have no friction and the side grip is a force on the wheel
bodies, so it acts at hub height, not at the ground. Its lever is the hub's depth below the centre of mass, about 10 cm.
That's about 300 N·m in a turn at 0.9 g, against the 1180 N·m it takes to tip it on flat ground. Sideways on a slope it tips
at the game's 46 degrees.

The ATV's body is one of the scene's dynamic actors, whose methods are at `+0x1b1ad8`, and it holds ODE's body at `+0xb8`. Its `+0x60` and `+0x64` hand back ODE's velocity and spin, and `+0x84` sets the spin. ODE's body has its rotation at `+0xa8`, as three rows of four, just before its quaternion, and its joints as a list at `+0x14`, each node naming the body at the joint's other end at `+4` (`+0x10bb54`). So a wheel's first joint names the chassis. The chassis is a 300 kg box, 4 m by 0.5 m by 3 m, with the inertia that goes with that, facing down its z axis.

It went over because nothing holds it in the air. A crest at speed throws it up, it keeps turning the way it was, and it lands on its nose or its back. The game's flag at `+0x118` only says the chassis touched something (`+0x99393`), so Longshot counts tyre contacts itself, front and back by which side of the chassis's centre they're on. With no wheel down the ATV falls at 2 g, so a bump throws it half as high. Its spin comes round in about 0.1 s to what would level it within 0.3 s, at up to 3 rad/s: nose along the way it's flying, at 0.6 of that and within 20 degrees, and level side to side. On one axle, the end that's up can't keep rising, and past 45 degrees of pitch it's turned back. On four wheels it goes where the ground takes it. The log says how many jumps lasted over a quarter second, the longest, and how often it was held on the ground.

The view from behind a vehicle is `vehicle_visor`, whose methods are at `+0x19c5e8`. Each frame its `+0x3c` (`+0x15360`) finds where the vehicle is (`+0x15150`) and eases a point at its `+0x164` toward it, by 4 times the frame's seconds, and the view goes round that point at its distance. That leaves the view a quarter of a second behind: nearly 4 m at 55 km/h, so speeding up pulled the view well back. Longshot moves the point 0.9 of the way the vehicle went across the ground before each update, which leaves 40 cm at the top speed. Rising and falling are left to the game's easing, which is what smooths the bumps.

Whether the game's tyres ever bounced can't be read from the code. A contact bounces if the product of its materials'
first fields is over 0.0001, and the setup at `+0x51c2` appears to fill in only the friction beside that field. The log
says so the first time a tyre is asked to bounce, and every 10 seconds or so how far the tyres sank.

A model of one corner over random facets at 55 km/h puts the typical jolt 30% lower and the worst 40% lower, with the wheel
off the ground 9% of the time instead of 22%. At 25 km/h that's 0.9% instead of 12%. It compares settings. It isn't a
measurement of the game.

The changes are all in `vehicle.cpp`, and the suspension needs a new ATV to take effect.

## Freelook

The camera on foot is the player's view (`hunt vf58` gives the player, and its `vf50` the view), holding a position at
`+0x20`, a rotation at `+0x2c` and a frustum at `+0x48`, and `0xd0160` rebuilds the frustum from the other two.

Writing to it before a frame does nothing, as the world draw has the camera catch up with the body first (`vf30`, called
at `0x5dbe`, which is `0x12c00` on foot). That copies the body's position and rotation back over the view, adds bob and
sway, and rebuilds the frustum. So freelook wraps that slot, and after it runs the head's yaw and pitch go on through
the engine's own attitude class and the frustum is rebuilt.

The turn stays on the view until the next time it catches up. A shot copies its place from the view (`+0xa74bf`) and the
gun takes its rotation from the camera (`+0xabaf3`), so both go where you look. Nothing that moves or turns the body reads
the view: walking and the look function (`+0x131a2`) start from the hunter's own rotation, so the head can't reach the
body or build up over time.

The mouse reaches it in `hook_impulse_value`, which gives the look impulses back as 0 while freelook has them. It works
only with the foot view (`0x19c220`). Pitch stops at the game's own 70 degrees for wherever the view ends up.

Tapping alt on its own opens the window's system menu, which freezes the game until the next key, so `WM_SYSCOMMAND`
with `SC_KEYMENU` is dropped.

## Tree stands

The hunter's update sets what animals can smell and see with `0xb8f70`, called at `+0xbd5e`, and `0xb8f90` at `+0xbe41`.
Sight already gets 0.25x on any ladder view, which covers tree stands, tripods and blinds alike. Hooking both calls to
give 0 hides the player from both senses, and hearing, at `+0xbee5`, is left as it is.

Tree stands are told from the rest by the view offset, at `+0x144` of the ladder view (`0x19c2e8`), read from `ladder_top`.
Its default is 0, kept with its limits at `0x1a96b0`, and `treestand.sii` is the only script with a `ladder_top` that
doesn't set one. The blind has 0.3, the tripod and ground blind 0.52, and ladders 0.5.

All leaves come from one fragment program, `effect/hu4/dif_opac/arb2.fp`, so it's changed as it reaches
`glProgramStringARB`. Its fog coordinate is the distance in metres. The change reads a value from `program.env[20]`,
which only `stand_before_world` sets, so nothing changes unless the player is up a tree. The game's alpha test stays at
0.2, so leaves thin out with alpha to coverage when anti-aliasing is on, or a fixed dither with `KIL` when it's off.
If the driver refuses the changed program, the original goes in instead.

## Calculator and range sheet

Both are extra entries in `def/equipment.def`, added as the file loads, with the same fields as the map. Anything
based on `eqitem_base` shows up in the loadout by its `category`, so the game needs nothing else to list them.

The game's 28 items exactly fill the equipment grid in `ui/meq_items.esc`, 4 columns by 7 rows, and new ones go after
them on a row it has no room to show. An eighth row fits with the same size cells once the heading, which is in the
frame, moves up above it. The handler just adds every item to the list, so it needs no changes.

They're `area_map` items (`/script/item/map.sii`), with the table of methods at `0x1a2690`. `+0x30` is given the
definition as the item loads, which is where the `paper` key added to each is read, as the item doesn't keep it. `+0x58`
is told about stages. It gets `(1, true)` once the raise animation ends, `(1, false)` as it starts lowering, and
`(2, false)` as it leaves the lowered stage, which is where the map plays its sound. Stage 1 is what shows and hides
the map's window, so for these two that part never reaches the game, and the overlay goes up instead. `+0x38` lowers
any raised item, which is how right click puts one away.

The overlay is drawn just before the swap, with everything of the game's state in `glPushAttrib` and the matrix stacks,
so it needs no hook in the UI. Windows draws the text into a texture for each of three sizes. The mouse is taken at
`GetDeviceData`, and only presses are held back, so a button the game saw go down still gets let go.

Leaving a hunt doesn't lower what's in your hands, so `paper_draw` drops the overlay once there's no player.

### Bullet flight

The sheet uses the game's own solver, so it can't disagree with the game.

| Offset | |
| --- | --- |
| `0xf5880` | Sets up a 0x30 byte solver: `+0x4` ballistic coefficient 0.5, `+0x8` muzzle speed 3000, `+0xc` weight 150, `+0x18` sight height 1.5 |
| `0xf58d0` | Solves it: `(0, -1, target)`, where `target` is `{0, 0, yards}`. Up to 10 passes, and it gives up at once if muzzle speed is 60 or less |
| `0xf58c0` | Clears up after |

The answer is at `+0x10` in degrees, and 60x that is the MOA the game shows, which is what lets the sheet's zero be
checked against the game's own message when sights are reset. The game calls this at `0xa6857`. Sight height stays at
1.5 unless the gun has `zero_sights`, and then it's 0. Sights move in steps of 0.00833333 degrees, or 0.5 MOA, and stop
at 10 degrees either way.

Ammo files give `bc`, `muzzle` and `weight`. `9mm.sii` has a second `weight` commented out above the real one, which is
why they're read a line at a time. A loadout is in the profile's `free_hunt` section as indexed `weapon` entries, each a
gun's name, then `|` and its scope. Tournaments don't use that section.

## Eyelids

`ui/geyes.esc` is two strips of black, and `+0x7f3f0` moves them each frame by one number, called the glare here, which
sets both how far they close and how opaque they are. It's worked out at `+0xec6b0`: the angle between the view and the
sun, as a share of half the field of view, scaled by the weather, and zero if the sun is hidden. So the wider the field
of view, the further from the sun you can look and still be squinting. At 3440x1440 that was over 60 degrees. Longshot
gives that one reading a field of view of its own, of 50 degrees or the game's own if that's less, as it is through a
scope. The sky has nothing to do with it: that's just where the sun usually is.

## Distances

**View distance** is `g_zclip`, and the far plane is `(1 - z) * 300 + z * 1700`. The slider works, tops out at 1700 m,
and has nothing to do with grass or trees.

**Grass** is drawn from 20 m blocks, walking 3 blocks either way around the camera and skipping any over 62 m away. But
what actually limits it is a shader. Every grass material is `hu.dif_opac_attn`, which projects `attn.png` over a disc
30 m in radius, and the alpha test cuts it at 0.3, which is at 24 m. The 1/30 lives in two floats in `.data`, each with
one reader, so they can be changed while the game runs. Past 47 m the square edges of the blocks would show, so the
walk and the 62 m have to grow too. Blocks are found by key and not by position in a grid, so that can't overflow.
They're built on the main thread, and kept in a cache of 98, which has to grow with them. `grass.sii` has no distance
in it. The walk builds every block it finds missing, in that frame. Longshot steps in at `+0xf27c8`, where it has just found one, and once 2 ms of a frame has gone
on building, sends the rest to `+0xf2933`, the walk's own way of moving on. They come up on the next frame. This takes an assembly stub, as it's in the
middle of a function, and it's the only one in the project.

**Trees** are the only thing with level of detail. Everything else is always drawn. Per plant, with `v` for
`g_tree_lodswitch` and `s` for the scale below, `dist / s` under `50 * (1 + v)` is the full model, under `70 * (1 + v)`
is a crossfade, and past that is a billboard. Whole tiles go through another test first: any tile further than
`(10 + 70v) * s` draws only billboards, with no fade. That's 60 m short of where the plants finish fading, so trees
pop there. Longshot starts it from 80 instead of 10.

`s` is `tan(40) / tan(fov / 2)`, which is meant to make scopes keep their detail, and it also drives animals, whose
thresholds are 35 and 85 m. It reads the frustum's field of view, so widening that for wide screens shrinks every range:
at 3440x1440 the full model stopped at 56 m instead of 100. Longshot replaces the function at `+0xd01d0` with one that
uses the game's own field of view, and multiplies it by `lod_distance`. Zooming in already scales the ranges by the magnification,
so the two multiplied went too far: `lod_distance` 3 through 8x binoculars put full trees out to a kilometre, at over 5,000 draws
against 1,500. Together they stop at 8, but never below the zoom alone or `lod_distance` alone.

## Terrain chunks

A map is about 100 chunks, each 350 m square, and each chunk's physics and triangles are built when something first needs them. The landscape tick at
`+0xd8e60` trims both caches every frame: it sorts the chunks by when they were last used, and lets go of any past the 9th, or not used for 100 frames.
Anything that asks for more than 9 in a frame rebuilds the difference every frame, which `engine.log` reports as "Chunk physics data trashing" or
"Chunk triangle heap data trashing". It only says so when the frame before let go of something too, so a chunk here and there doesn't show.

That was the worst slowdown in a hunt. Physics at 15 chunks a frame cost 17 ms, which took 165 FPS down to 44, and triangles at 22 chunks a frame cost 7 ms,
at about 2.7 ms and 0.5 ms for each chunk rebuilt. Both can turn on and off as you turn around. Every animal is tested against the view each frame
at `+0xae2e0`, and drops to state 0 when out of it, so looking towards animals costs more even when they're too far away to see. A ray at `+0xd3740`
also asks for every chunk touching the box around it, so a long ray at a diagonal asks for far more chunks than one along a map axis. What asks for
the most chunks hasn't been pinned down.

The 9 isn't arbitrary, because triangles have a harder limit. They go in a `triangle_heap_t`, which has 255 slots, with one for each piece of a chunk's model,
and the slot number sits in the top byte of each triangle's id. Chunks have up to 43 pieces, and Arizona's largest 9 have exactly 255. `append` refuses
once the slot count reaches 255, and it checks that before it looks at the free list, while `remove` never lowers the count. So holding 255 pieces once, even
briefly, stops any more being added for the rest of the map. `+0xda590` ignores the failure and marks the chunk as held anyway, so ground queries there find
nothing. That happened in play: grass gets its ground from here too, and a hunt that thrashed for minutes had none left around the player, then came back
in the same spot once this was fixed. Physics has no such limit.

Longshot raises both caches to 127 chunks, the most that `cmp` can hold. Nothing else calls `+0xda590` but the four sites it hooks, for ground, shadows,
collisions and grass. Before a chunk loads, the hook lets go of the chunks used longest ago until its pieces fit in 254 slots, so the heap never reaches 255.

The 100 frame rule goes too. It's half a second at 200 FPS, and was over a second and a half at the 60 FPS the game was made for. Animals look at the ground only now and
then, so with that alone the log still showed 30 to 50 chunks reloading every 10 seconds, with none needed for room, and single frames of 11 to 13 ms in `sim`.

Every 10 seconds, if anything was loaded, the log gets a `terrain:` line saying how much, for which site, and how many had to make room. Some are normal the
first time a chunk is needed. Once a map has been driven around, a lot every 10 seconds means more pieces are wanted at once than fit.

## Culling

The game tests everything against the view, but not against anything in front of it. Each chunk's box is tested at `+0xd9103`, and only then is what's in it
looked at. Chunks within `(10 + 70v) * s` of the camera then test each plant's box at `+0xe249f`, and further chunks draw a batch of billboards, which `+0xdc000`
rebuilds whenever the view swings past about 30 degrees. Grass blocks and animals test a sphere.

Northern Rockies has 24,938 plants, and all the maps together have 206,521. They're plain text in `landscape.game.sii`, as `plant_inst` with a position and a
scale, and a chunk's triangles are in its `.pmg`, so what the ground hides can be measured without running the game. Reading both, and looking from all 146 start
points in 8 directions at `lod_distance` 3, a view holds a median of 228 full trees and 4,604 billboards, and up to 10,000.

With the margins below, the ground hides this much:

| | Waterfall | Northern Rockies | Montana | Texas | Arizona |
| --- | --- | --- | --- | --- | --- |
| Full models | 36% | 17% | 19% | 11% | 23% |
| Fading | 35% | 37% | 39% | 28% | 49% |
| Whole chunks | 17% | 12% | | | |

Longshot reads the terrain when a map loads, from the `Loading landscape` message at `+0xd894a`, which gives the path whatever kind of hunt it is. Each chunk's `.pmg`
goes into a height map of 8 m cells, read every 4 m, and each cell keeps the lowest surface in it. So rocks and water don't count as cover, and the map can only be lower
than the real ground, which hides less. Some maps skip a chunk number, and Texas starts from 1, so it asks `fs_file_exists` first.

From that it builds a skyline: for each of 720 directions and every 8 m out to 1700 m, how high the ground has risen so far, as a slope. It's rebuilt when the camera moves 1 m, and
what's hidden is then one look-up, so it needs no copy of a plant's result and works from any view. A box is hidden if the skyline short of its nearest edge is above its top
in every direction it covers. The two calls in that first paragraph go through a pointer, so swapping the pointer in each gets the game's own test first, and this
only runs on what's in view.

The margins all hide less. The camera counts as 1 m higher and every box as 3 m taller, ground within 24 m hides nothing as it changes too fast as you move, and nothing
within 40 m is hidden. A chunk's box is only its terrain, so it counts as 60 m taller and 15 m wider. The tallest plant as placed on any map is a 44.5 m poplar at 1.3 times, 57.6 m.
A second camera in a frame gets no culling, rather than the time for another skyline.

Trees aren't all that's left. Billboards in a chunk that's partly hidden are still drawn, and so is grass, and animals. `occlusion_culling=0` turns it off.

## Frame timing

The engine creates no threads of its own, so a file read or a texture upload holds up the frame until it finishes.

The game object's frame method is at vtable `+0x04`. It advances the world, then calls the render function at
`game.dll+0x102df0` from `+0x10360d`, which draws the world, calls `ui_process`, and swaps. It then calls
`snd_update_state`. Other callers of the render function are the loading screens, so Longshot only replaces that
one call.

Each frame is split into `sim`, `world`, `ui`, `swap` and `sound`, and `post` sits inside `ui`. `swap` includes the frame limiter and waiting for
vsync, so a large one on its own usually means spare time, not a problem. The rest sit inside those, and are counted separately: `files` covers `fs_open`,
`read` and `block_read` in every module, `uploads` covers `glTexImage2D`, `glTexSubImage2D`, `glCompressedTexImage2DARB` and `glBufferDataARB`, `grass` is
blocks being built, `terrain` is chunks being loaded, `decode` is `ov_open_callbacks` and `ov_read`, `queues` is the time from `+0x1560a0` on, which is what's in `world`
after the game has worked out what to draw, and `skyline` is occlusion's. Each line ends with the draw calls and triangles, per frame on the 10 second lines.

`snd_update_state` runs twice a frame, once from the hunt's update at `+0x4344`, so `sound` can land inside `sim`. Streaming sounds refill a quarter of a second at a
time, which is too little to explain the stalls in the table below. Neither sound module has a thread, so decoding holds up the frame, and `decode` is there to show whether that's what they are.

Every 10 seconds the log gets the averages and the memory in use, and any frame over twice the usual length gets
its own line, with `other` for time outside every section.

An 11 minute hunt at 3440x1440 and 165 FPS averaged about 0.3 ms in `sim`, 2 ms in `world` and 0.1 ms in `ui`, and
touched almost no files once it had loaded, because everything is loaded up front. Each known cause of a slow frame:

| Frame | Cause |
| --- | --- |
| Up to 230 ms | Loading a map, with most of it in `files` |
| About 135 ms in `ui` | The Sound tab listing audio devices, logged as `[snd] device count` |
| About 65 ms, then up to 25 ms in `swap` | The screenshot command reading the frame back and saving it, uncompressed |
| About 40 ms in `world` | Once, 10 seconds into a hunt |
| 8 to 35 ms in `sound` | Every 40 seconds or so, cause not known yet |
| Up to 25 ms in `swap` | Coming back from alt-tab |

## Ruled out

- Extension string overflow: `message()` formats into allocated memory, so a very long GL extension list is safe there.
- Display mode list overflow: there is no fixed list.
