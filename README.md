# Cafe Engine — Wii U, media, particles, and 3D

Cafe Engine is a small, modular 2D/3D game runtime with its own declarative UML scene
format, CafeStyle visual language, and CafeScript gameplay language. It is not a
browser and does not embed HTML, CSS, or JavaScript.

It includes a 60 FPS Wii U loop, dual-screen TV/GamePad scenes, Wii Remote
pointing, particles, cameras, lit 3D cubes and OBJ meshes, PNG/SVG images,
WAV/OGG audio, GamePad UI navigation, and an embedded TrueType font rasterized
in software.

Text uses [Atkinson Hyperlegible](https://brailleinstitute.org/freefont) Regular
(SIL Open Font License 1.1), baked into the binary and drawn with `stb_truetype`.
CafeStyle `color` and `font-size` still control ink and size.

## Build and run on macOS

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/cafe examples/hello_world/main.uml
```

The resizable window is TV on the left and GamePad on the right. Each pane
preserves the game's aspect ratio while the window changes size. Devices are split:

| Device | Role |
|---|---|
| Mouse + keyboard | Wii Remote (TV) |
| Xbox / other GCController | Wii U GamePad (right pane) |

Keyboard: WASD/arrows move the player, Z/Space/Enter or left-click the TV is A, X/Esc or right-click the TV is B. Point at the left pane to see the red IR cursor. Click the right pane or use the Xbox D-pad/A to work the HUD.

A one-shot stacked preview (TV above GamePad) is still available:

```sh
./build/cafe examples/hello_world/main.uml --click 320 360 --preview
```

## Wii U homebrew

Needs [devkitPro](https://devkitpro.org) with the `wiiu-dev` and `wut-tools` packages. Then:

```sh
./scripts/build-wiiu.sh
```

Every folder under `examples/` that contains `main.uml` is packed as both:

- an Aroma **WUHB** (Wii U Menu)
- a Homebrew Launcher **folder** (RPX + `main.uml` + assets)

Copy `dist/wiiu` to the SD card root:

```text
sd:/wiiu/apps/
  hello_world.wuhb          ← Aroma / Wii U Menu
  hello_world/
    hello_world.rpx         ← Homebrew Launcher
    meta.xml
    main.uml
    main.cstyle
    main.cafe
    assets/
```

**Aroma:** put `hello_world.wuhb` in `sd:/wiiu/apps/` and launch it from the Wii U Menu (needs the Homebrew on Menu plugin). Homebrew Launcher does not list `.wuhb` files.

**Homebrew Launcher / Tiramisu:** open the `hello_world` folder, not the `.wuhb`.

The RPX loads `/vol/content/main.uml` inside a WUHB, or `main.uml` next to the RPX. If neither is found it stays running and prints the error on both screens.

### Cemu

Cemu 2.0 or newer can load the bundle:

1. **File → Load** `dist/wiiu/apps/hello_world.wuhb` (prefer the `.wuhb`, not the `.rpx`).
2. Turn on the GamePad window (Options / Display → Separate GamePad view).
3. In **Options → Input settings**, set Controller 1 to **Wii U GamePad** and
   choose your connected controller. The demo accepts the D-pad and left stick
   for movement; A activates the focused GamePad button.

If you load `hello_world.rpx` instead, set Cemu’s SD card folder to `dist` so `sd:/wiiu/apps/hello_world/main.uml` exists. Loading only the RPX with no SD card leaves both screens empty.

Optional files in a game folder:

| File | Purpose |
|---|---|
| `wuhb.meta` | `name=`, `short_name=`, `author=` |
| `icon.png` | 128×128 icon |
| `tv.png` | 1280×720 TV splash |
| `drc.png` | 854×480 GamePad splash |

A loose RPX is also written to `dist/wiiu/apps/cafe_engine/` if you still want that name.

CafeScript save files are SD-only on Wii U. `Storage.write("slot-1.txt", data)`
writes beneath `sd:/wiiu/apps/cafe-engine/saves/`. Cafe Engine never falls back
to MLC, SLC, or another NAND volume; if the SD card cannot be mounted, the call
returns `false` and the game continues without saving.

The Wii U backend sends ordinary sprites, sprite batches, glyph quads, and plain rectangles through GX2. Generated and decorated shapes are cached as textures. Unsupported 2D drawing remains in ordered, cropped software layers so existing effects keep their scene order. TV and GamePad retain independent scenes and frame reuse. Meshes and particles use GX2 directly. The loop targets 60 FPS, subject to workload and hardware performance; it does not guarantee 60 FPS. The canvas is aspect-fitted independently to each real output, so 4:3 and 16:9 screens keep their proportions with centered letterboxing instead of stretching. GamePad touch and desktop pointer coordinates are mapped back through that fitted viewport. VPAD feeds `GamePad`; KPAD/WPAD feeds `WiiRemote`, including IR (`WiiRemote.valid`, `WiiRemote.x`, `WiiRemote.y`) and Nunchuk.

Cached Wii U UI panels and glyphs rasterize at the larger fitted pixel density
of the TV and GamePad outputs (1.5× for a 720p TV and 2.25× for a 1080p TV
with a 640×480 logical canvas). Their positions and input coordinates remain
in the original logical layout; the lower-density screen samples the same
cached detail. Hue and saturation attributes on images and sprites are
applied to cached GX2 texture variants. For example, `hue="190"
saturation="1.45"` recolors an image without changing its alpha or tint.

Projects can opt into a full-screen 2D canvas with `<main viewport="expand">`.
The Wii U renderer keeps the original vertical scale, reveals more scene area
at the sides of wide displays, and maps touch/IR input back to logical scene
coordinates. A 640×480 scene spans about −107 to 747 horizontally on the
854×480 GamePad. Full-screen 640×480 backgrounds and scrims cover the whole
output; other sprites keep their proportions. CafeScript exposes `Screen.left`,
`Screen.right`, `Screen.width`, `Screen.height`, `Screen.tvWidth`, and
`Screen.gamePadWidth` for camera culling and spawn placement. TV and GamePad
render separately when their visible widths differ substantially. Without the
attribute, existing aspect-fit rendering remains in effect.
UI leaves can use `screen-anchor="left"`, `"right"`, or `"stretch"` to keep
their original edge spacing on each display. Rendering, button hit tests, and
GamePad focus navigation use the same anchored bounds. Centered elements need
no attribute. A progress fill can use `screen-anchor="stretch-progress"` and
`screen-base-width="578"`; its current width remains proportional to the
matching stretched track.
The Wii U loop also checks the TV and GamePad colour-buffer dimensions before
input each frame. If either buffer changes, it rebuilds viewport coordinates,
refreshes the live CafeScript `Screen` object, invalidates retained display
frames, and rerasterizes cached UI art when pixel density changes. This reacts
to output-buffer changes; resizing an emulator window without changing its
emulated GX2 buffers is handled by the emulator's own scaling.

The Cemu-compatible presentation pass uses an embedded open-source GX2 texture
shader based on [gx2textureShader](https://github.com/rw-r-r-0644/gx2textureShader).

Rounded panels keep inset shadows clipped to their inner radius. `sawtooth-edge`
supports `top`, `right`, `bottom`, and `left`; teeth inherit the element's fill,
gradients, background patterns, border, opacity, and outer shadow. Borders are
silhouette strokes: they follow rounded curves and every sawtooth slope and
valley without leaving a straight seam where the teeth meet the body. Image
textures are clipped to the same silhouette and cannot paint over the outline.
Outset shadows use a real distance falloff around that silhouette, so the blur
radius in `outer-shadow: x y blur color` softens curves and sawtooth edges too.

For a manual cross-build:

```sh
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake \
  -S . -B build-wiiu -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-wiiu
cmake --install build-wiiu --prefix dist/wiiu/apps
```

## CafeScript additions

```js
Scene.open("world", "tv")
Scene.gamepad.open("hud")
spawn("spark", "world-layer", x, y)
destroy(sparkId)
overlaps(player, goal)
emitParticles(sparks, 50)
const voice = Audio.play("assets/chime.ogg", { loop: false, volume: 0.5 })
Audio.pause(voice)
Audio.resume(voice)
Audio.setVolume(voice, 0.35)
Audio.setPitch(voice, 1.1)
Audio.stop(voice)
GamePad.up / GamePad.pressed.A / GamePad.leftX
WiiRemote.A / WiiRemote.valid / WiiRemote.x / WiiRemote.nunchuk.x
Keyboard.w / Keyboard.pressed.escape
Mouse.x / Mouse.left / Mouse.pressed.left / Mouse.released.left

Storage.write("slot-1.txt", "quests=4")
const save = Storage.read("slot-1.txt", "quests=0")
const timer = setTimeout(() => console.log("ready"), 500)

Collision.pointIn(player, Mouse.x, Mouse.y)
Collision.circles(x1, y1, radius1, x2, y2, radius2)
Collision.alpha("assets/hit-mask.png", pixelX, pixelY, 0.1)

const speeds = [80, 120, 160];
const doubled = speeds.map(speed => speed * 2);
for (const speed of doubled) {
  if (speed < 200) continue;
  console.log(`speed=${speed}`);
}

if (doubled.length > 2) {
  console.log("The condition controls this whole block");
  console.log("Add as many statements as you need");
} else if (doubled.length === 2) {
  console.log("Exactly two values");
} else {
  console.log("Fewer than two values");
}
```

CafeScript also supports `let`/`const`/`var`, arrow and anonymous functions,
closures, template literals with `${expressions}`, array indexing and mutation,
object shorthand, ternaries, `===`/`!==`, `**`, `+=`/`-=`/`*=`/`/=`,
JavaScript-style multi-statement `if`/`else if`/`else` blocks, `for...of`,
`break`, `continue`, `null`, `undefined`, and `typeof`. Built-ins
include common Array methods (`push`, `pop`, `map`, `filter`, `reduce`, `find`,
`findIndex`, `some`, `every`, `slice`, `reverse`, `forEach`, `includes`,
`indexOf`, `join`, `splice`, `concat`, `fill`), String helpers (`trim`, `split`, `slice`, `substring`),
`Object.keys`/`values`/`entries`/`assign`, a larger `Math`, `console.log`, and
`Number`/`String`/`Boolean` conversions. Nodes expose `z`, `rotationX/Y/Z`,
`scaleX/Y/Z`, `depth`, `fov`, and particle controls in addition to their 2D fields.

Groups apply position, rotation, and scale to their descendants. This provides a
cheap 2D camera: move one world group while the HUD remains in a separate `ui`.
For dense fields, `<spritebatch>` stores up to thousands of tinted, rotated
instances without creating thousands of scene nodes:

```xml
<group id="world-camera">
  <spritebatch id="fragments" src="assets/fragment.png" max="7000" cache="true" />
</group>
```

```js
const index = fragments.add({x: 40, y: 80, width: 16, height: 16,
  rotation: 30, opacity: 0.8, tint: "#d65cffff"});
fragments.set(index, {x: 44, rotation: 36});

// For hundreds of moving instances, cross the script/native boundary once.
setSprites(fragments, movingFragments);
```

Retained batches containing 256 or more sprites are raster-cached automatically
and the cache is shared by the TV and GamePad. Add `cache="true"` to smaller
static batches to opt them in too. Any batch mutation, transform, tint, or
opacity change invalidates the cache; continuously moving batches should use
`setSprites` so their full instance list crosses into the engine once per frame.

Vector primitives use `<shape kind="circle|ellipse|line|polygon|arc">` and
accept the same fills, alpha gradients, borders, and blend modes as other 2D
nodes. Project fonts can be selected with
`font-source: url("assets/Handlee-Regular.ttf")` or a text node's `font=` attribute.

## Media, particles, and 3D

Images can use PPM, PNG, or SVG. Audio can use PCM WAV or Vorbis OGG. External
3D geometry uses triangulated or polygonal Wavefront OBJ files; faces are
triangulated while loading.

Image sources can also use seeded [procedural generation](docs/procedural-images.md):
simplex noise, terrain with caves, and edge-decorated masks. Existing An EgG
cave sources remain compatible and render at higher resolution.

OBJ normals are automatic when needed. Explicit `vn` data is preserved,
`s off` creates hard per-face normals, and numbered or named `s` groups receive
area-weighted averaged normals across shared positions and UV seams. If an OBJ
contains no `s` statements and omits normals, Cafe Engine enables one automatic
smoothing group. Use `shading: auto`, `smooth`, or `gouraud` to render those
normals; use `shading: flat` to keep a faceted look regardless of OBJ normals.

```xml
<scene3d id="world" display="tv">
  <camera id="camera" z="-6" fov="60" near="0.1" far="100" />
  <cube id="box" class="box" x="-1" z="2"
        width="1.5" height="1.5" depth="1.5" />
  <mesh id="ship" class="ship" src="assets/ship.obj" x="1" z="3" />
  <particles id="sparks" x="320" y="420" rate="30" lifetime="1.2"
             speed="90" spread="60" gravity="80" particle-size="7"
             max="200" color="#ffd45c" />
  <image id="logo" src="assets/logo.svg" x="20" y="20" width="96" height="96" />
  <audio id="music" src="assets/music.ogg" autoplay="true" loop="true" volume="0.4" />
</scene3d>
```

Use `rotationX`, `rotationY`, and `rotationZ` from CafeScript to animate 3D
nodes. A scene may mix 2D UI/images, particle emitters, and 3D content.

### Materials, blending, and particles

The same blend setting works on sprites, images, text, panels, buttons,
particles, and 3D meshes:

```css
.energy   { blend-mode: additive; }
.glass    { blend-mode: screen; }
.stain    { blend-mode: multiply; }
.eraser   { blend-mode: subtract; }
.solid    { blend-mode: normal; }

.smooth-model { shading: smooth; }  /* Gouraud vertex lighting */
.low-poly     { shading: flat; }
.glow-model   { shading: unlit; }

.alpha-ramp {
  fill: linear-gradient(90deg, rgba(255, 80, 20, 0.15), #ffe060ee);
}
```

Particle colors and sizes can change across their lifetime. Emitters support
native GX2 circles or rotating rectangles:

```xml
<particles id="fire" class="energy" x="320" y="420"
  shape="circle" particle-size="10" end-size="2"
  start-color="#fff4aaff" end-color="#ff4b1000"
  rate="35" lifetime="1.4" speed="95" spread="70" />

<particles id="confetti" class="glass" x="320" y="420"
  shape="rectangle" particle-width="12" particle-height="4"
  spin="220" start-color="#58d8ffff" end-color="#8c5cffff" />
```

CafeScript can change `particleShape`, `particleSize`, `endSize`,
`particleAspect`, and `spin` at runtime. Supported blend modes are `normal`,
`additive`, `multiply`, `screen`, and `subtract`; shading modes are `flat`,
`auto`/`auto-smooth`/`smooth`/`gouraud`, and `unlit`.

Particle emitters accept `camera="true"` (the default) or `camera="false"`.
With `true`, moving or rotating the emitter moves its existing particles too.
With `false`, each particle stays in screen space at the position and direction
where it was emitted, while the emitter can move to create new particles elsewhere.
`spawn-width="38"` spreads each new particle across a 38-pixel line centered on
the emitter, perpendicular to its emission direction. `spin="0"` keeps each
particle's own orientation fixed as it travels.

UML scenes take `display="tv"`, `display="gamepad"`, or `display="both"`.

Styles and scripts may be external, embedded, or combined. Inline blocks are
read as raw CafeStyle/CafeScript, so operators such as `<` do not look like UML
tags. When a block also has `src`, the external file is loaded first and the
inline content extends it:

```xml
<style>
  .energy { blend-mode: additive; fill: rgba(255, 120, 20, 0.7); }
</style>

<script>
  let speed = 2;
  function update(dt) {
    if (hero.x < 600) hero.x += speed * dt;
  }
</script>
```

The existing self-closing form remains supported:

```xml
<style src="main.cstyle" />
<script src="main.cafe" />
```

## Project layout

- `src/core` — engine orchestration and scene graph
- `src/uml` — custom XML-like project parser
- `src/cstyle` — selectors and visual value parsing
- `src/cafescript` — lexer, parser, evaluator, events, engine APIs
- `src/renderer` — software rasterizer, stb_truetype, embedded Atkinson Hyperlegible
- `src/assets` — PNG/SVG/PPM, WAV/OGG, OBJ, and font loading
- `src/audio` — SDL-backed audio mixer
- `src/platform/desktop` — live macOS window
- `src/platform/wiiu` — VPAD + KPAD homebrew main
- `include/cafe/input` — platform-neutral input state
- `examples/hello_world` — dual-screen demo (also packed as `hello_world.wuhb`)
