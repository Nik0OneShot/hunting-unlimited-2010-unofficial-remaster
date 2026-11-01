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

## Walking

Forward is built from the view with no bias, the head bob stays in the camera, and physics runs at a fixed 40 Hz. But
`avatar::update` at `+0x126dd0` never corrects sideways velocity, and the player's contacts with the ground use ODE's box
friction, with a limit of 500 per axis, on axes close to the world's own. Walking across that box at an angle, the
axes don't resist equally, so the player gets pulled toward the nearest world axis. This is a model, worked out from the
code, and the drift it predicts changes with heading.

Longshot hooks the one `dJointCreateContact` call at `+0x12765d`, and for the player's contacts it sets `dContactFDir1`
to the direction of the walk, which lines the box up with it. In the game that left a typical drift of 1 degree over 3 m,
across 57 straight stretches. It logs that drift, with the heading, and whether the correction is on.

## Vehicles

The ATV and the horse are the only vehicles. The horse moves by animation, so its speed is in its `.bhv` data.

The ATV isn't driven through its wheels, whose friction is zero. The tick at `+0x98510` pushes on the rear hubs with a
fixed force, 600 N each on 340 kg, which stalls at 21 degrees. It stops pushing altogether once the speed reaches 25 km/h.
That counts falling too, which is why it took a cliff to go fast. With no throttle, on the ground, it loses a fifth of its
speed every tick, at 40 ticks a second, which would stop it dead at any real speed. None of this depends on frame rate:
physics runs at a fixed 40 Hz. Throttle is on or off. Reverse shares the cut-out, but with only 400 N.

The tyres do hold it sideways, by cancelling sideways speed at each wheel, up to 0.8 m/s a tick. That's about 3 g, and
the ATV rolls at around 1 g, so with any real speed a hard turn flips it, and the rider dies when it lands upside down.
At 25 km/h it never mattered. The limit comes down to 0.22 m/s, about 0.8 g, so it slides first. The limit and the test
for reaching it are separate: changing only the limit leaves full grip for any slide under 0.8 m/s, so it would still
flip, and the test's values are shared with other code, so its operands get pointed at Longshot's own.

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
That's about 280 N·m at the grip limit, against the 384 N·m or so that used to roll it, and 1180 N·m to tip it on flat
ground. Sideways on a slope it tips at the game's 46 degrees.

Whether the game's tyres ever bounced can't be read from the code. A contact bounces if the product of its materials'
first fields is over 0.0001, and the setup at `+0x51c2` appears to fill in only the friction beside that field. The log
says so the first time a tyre is asked to bounce, and every 10 seconds or so how far the tyres sank.

A model of one corner over random facets at 55 km/h puts the typical jolt 30% lower and the worst 40% lower, with the wheel
off the ground 9% of the time instead of 22%. At 25 km/h that's 0.9% instead of 12%. It compares settings. It isn't a
measurement of the game.

The changes are all in `vehicle.cpp`, and the suspension needs a new ATV to take effect.

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
in it.

**Trees** are the only thing with level of detail. Everything else is always drawn. Per plant, with `v` for
`g_tree_lodswitch` and `s` for the scale below, `dist / s` under `50 * (1 + v)` is the full model, under `70 * (1 + v)`
is a crossfade, and past that is a billboard. Whole tiles go through another test first: any tile further than
`(10 + 70v) * s` draws only billboards, with no fade. That's 60 m short of where the plants finish fading, so trees
pop there. Longshot starts it from 80 instead of 10.

`s` is `tan(40) / tan(fov / 2)`, which is meant to make scopes keep their detail, and it also drives animals, whose
thresholds are 35 and 85 m. It reads the frustum's field of view, so widening that for wide screens shrinks every range:
at 3440x1440 the full model stopped at 56 m instead of 100. Longshot replaces the function at `+0xd01d0` with one that
uses the game's own field of view, and multiplies it by `lod_distance`.

## Frame timing

The engine creates no threads of its own, so a file read or a texture upload holds up the frame until it finishes.

The game object's frame method is at vtable `+0x04`. It advances the world, then calls the render function at
`game.dll+0x102df0` from `+0x10360d`, which draws the world, calls `ui_process`, and swaps. It then calls
`snd_update_state`. Other callers of the render function are the loading screens, so Longshot only replaces that
one call.

Each frame is split into `sim`, `world`, `ui`, `swap` and `sound`, and `post` sits inside `ui`. `swap` includes the frame limiter and waiting for
vsync, so a large one on its own usually means spare time, not a problem. `files` and `uploads` sit inside the others,
and are counted separately: `files` covers `fs_open`, `read` and `block_read` in every module, and `uploads` covers
`glTexImage2D`, `glTexSubImage2D`, `glCompressedTexImage2DARB` and `glBufferDataARB`.

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
| About 20 ms in `sound` | Once, starting a sound |
| Up to 25 ms in `swap` | Coming back from alt-tab |

## Ruled out

- Extension string overflow: `message()` formats into allocated memory, so a very long GL extension list is safe there.
- Display mode list overflow: there is no fixed list.
