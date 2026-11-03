# An EgG portability audit

Source audited: `/Users/lf/Desktop/weird gaem for wii U` (LÖVE 11.4 / LÖVE Potion).

An EgG can now be ported feature-for-feature to Cafe Engine, but its roughly
4,900 lines of Lua still need to be translated to CafeScript and its scene/UI
split expressed in UML/CafeStyle. Cafe Engine does not execute LÖVE Lua files.

## Engine mappings

| An EgG requirement | Cafe Engine support |
|---|---|
| 1280×720 virtual layout on TV and GamePad | independent aspect-fitted displays and pointer remapping |
| mouse, touch, keyboard, GamePad, D-pad and analog cursor | `Mouse`, named `Keyboard`, `GamePad`, and `WiiRemote` state with pressed/released edges |
| scrolling infinite desk | transformable world `group` used as a 2D camera |
| eggs, boss art and UI images | PNG/SVG sprites with rotation, scale, opacity, tint and blend modes |
| circles, ellipses, lines, polygons and arcs | `<shape>` vector primitives with CafeStyle paint/borders |
| 640/7,000 background fragments and 720/4,680 hazards | retained `<spritebatch max="7000">` instances with add/set/remove/clear |
| short-lived effects | gradient circle/rectangle particle emitters or a dynamic sprite batch |
| shockwave alpha hit mask | `Collision.alpha(path, x, y, threshold)` |
| egg/boss/zone collision | `Collision.overlaps`, `Collision.pointIn`, and `Collision.circles` |
| scripted attack clocks and delayed events | frame `update(dt)`, `Time`, `setTimeout`, and `setInterval` |
| music and sound control | OGG/WAV voices with play, pause, resume, stop, volume, pitch and playing state |
| Handlee typeface | project TTF loading through `font-source` or `font=` |
| three independent save slots | `Storage.read/write/exists/remove` |

## Save safety on Wii U

Wii U storage is intentionally SD-only:

```text
sd:/wiiu/apps/cafe-engine/saves/
```

If the SD card cannot be mounted, storage operations fail and return `false`.
There is no fallback to MLC, SLC, or another NAND path.

## Porting boundary

The remaining work is game code and content organization, not a missing engine
capability: translate Lua tables/functions to CafeScript objects/functions,
move draw calls into retained UML nodes/batches, and recreate the title, save,
desk, fight, pause and result screens as scenes/layers. The supplied OGG/WAV,
PNG and TTF assets can be reused directly.
