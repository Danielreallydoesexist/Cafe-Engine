# Procedural images

An image `src` may start with `procedural:`. Cafe Engine generates the pixels
when a source string first appears, then caches the result. The same source
always produces the same image. On Wii U, the result is uploaded to a GX2
texture and sampled as an ordinary sprite. Sources can be changed from
CafeScript, for example to display a different terrain chunk.

Dimensions are in pixels and are limited to 16–1024 per side. `seed` selects
the deterministic variation. Colors use `#RRGGBB` or `#RRGGBBAA`.

## Simplex color field

```xml
<image id="clouds" src="procedural:simplex:seed=42;width=768;height=512;scale=110;octaves=4;low=#263c73;high=#d4e8dd"
       x="0" y="0" width="640" height="480" />
```

`scale` is the feature size in pixels. `octaves` is 1–6. `low` and `high` are
the two colors. Optional `cutoff=0.45` makes pixels below the threshold
transparent, creating a mask. `originX` and `originY` shift the field so
adjacent chunks can sample the same noise without visible seams.

## Terrain with caves

```xml
<image id="terrain" src="procedural:terrain:seed=9;width=768;height=768;scale=120;surface=0.4;roughness=0.17;caveScale=45;caveThreshold=0.25;solid=#36344d;edge=stones;edgeColor=#b5a58b;edgeWidth=3"
       x="0" y="0" width="500" height="500" />
```

Pixels above the noisy `surface` and inside the caves are transparent. `solid`
colors the remaining terrain. `caveScale` controls the size of cave pockets;
`caveThreshold` controls how open they are. `originX` and `originY` select a
region of the same noise field. Surface height is measured against the image
height in world pixels, so moving `originY` also moves above or below ground.

## Masks

```xml
<image id="island" src="procedural:mask:shape=ellipse;seed=3;width=768;height=768;roughness=0.12;solid=#425b65;edge=grass;edgeColor=#7ebd6b;edgeWidth=4"
       x="0" y="0" width="500" height="500" />
```

`shape` may be `ellipse` or `rectangle`. `roughness` perturbs the boundary
using simplex noise. For a smooth shape, use `roughness=0`.
Rectangular masks have square corners by default. Add `roundedCorners=true`
to round them, with optional `cornerRadius=24` (source pixels); use
`roundedCorners=false` for square corners.

## Edge decoration

All generators accept `edge=outline`, `edge=stones`, or `edge=grass`.
Decoration follows the boundary between opaque and transparent pixels, so it
also works along cave openings and simplex cutoff masks. Use `edgeColor`,
`edgeWidth` (1–12), and `edgeSpacing` (4–48). `edge=none` is the default.
Decorations are drawn on the opaque side of the boundary and become part of
the cached image; they do not add a per-frame draw call.

To place **your own sprites or game objects** along the edge, CafeScript can
request placement points from any image with transparency:

```js
const points = Mask.edgePoints(island.src, 16);
for (const p of points) {
  // p.x and p.y are 0..1 positions in the image; p.nx and p.ny
  // point out from its opaque region. Map them to your image node.
  spawnRock(island.x + p.x * island.width,
            island.y + p.y * island.height);
}
```

The second argument is spacing in source pixels (4–128). An optional third
argument is the alpha threshold (default 0.5). Points are generated once per
image revision and spacing, then cached. The result is capped at 2048 points.
This API also accepts regular PNG and SVG images; it is not tied to the cave
generator or a biome system.

The older `procedural:cave:bendH:bendV:exits:mouths:corners:style` source
remains supported for existing games. It preserves its 500-unit layout and
rasterizes at 768×768 pixels by default. Other games can choose a rectangular
mask with `;width=1024;height=512`, then display it at any size with the image
node's own `width` and `height`. Edge options may be appended after a `;`,
for example `;width=1024;height=512;edge=stones;edgeColor=#888888`.
The legacy `corners` bitmask remains the default. Append
`;roundedCorners=true` to round all four outer corners or
`;roundedCorners=false` to leave them square, regardless of that bitmask.

Cafe Engine retains up to 24 recent procedural images per AssetManager. When
a source is evicted, its stable slot is reused and its GX2 texture is updated.
Use a finite set of source strings for rapidly animated effects; generating a
new large image every frame still takes CPU time and texture bandwidth.
