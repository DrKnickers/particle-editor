# Blend Modes

The blend mode decides how a particle mixes with whatever is behind it. It is one of the biggest
levers on how an effect reads, so it is worth knowing what each option does. Set it in the Property
Panel under Appearance, in the Rendering section, with the Blend mode field.

The two you will use most are Additive and Transparent. Additive adds light to the scene and is right
for anything glowing. Transparent is ordinary see-through blending and is right for anything solid-
looking that fades, such as smoke.

<!-- Media: ref-blend-mode-grid -->

## The Modes

| Mode | What it does | Reach for it when |
|------|--------------|-------------------|
| None | No blending. The particle is drawn opaque and simply covers what is behind it; its alpha is ignored. | You genuinely want a solid, fully opaque sprite. Rare for particles. |
| Additive | Adds the particle's color to the scene, so it only ever brightens. Dark parts of the texture become invisible, and the **alpha channel is ignored** — brightness comes entirely from color, so fade an additive effect with its color or scale, never with alpha. | Glows, fire, plasma, energy, sparks — anything that gives off light. |
| Transparent | Ordinary alpha blending: the particle's alpha controls how much you see it over the background. | Smoke, dust, debris, and most soft opaque effects that fade in and out. |
| Inverse | Multiplies the scene by the particle's color, so it darkens instead of brightening. White leaves the scene unchanged; black drives it toward black. | Scorch marks, dark smoke, shadowy or draining effects. |
| Depth additive | Additive, but depth-aware (see below). | An additive effect that overlaps scene geometry and should intersect it correctly instead of popping in front of it. |
| Depth transparent | Transparent, but depth-aware. | Alpha-blended smoke or dust that overlaps terrain or a model and should meet it per-pixel rather than at the flat quad's edge. |
| Depth inverse | Inverse, but depth-aware. | A darkening effect that meets solid geometry and should sort against it correctly. |
| Diffuse transparent | Alpha-blended like Transparent, but the particle's own color is applied more strongly. | A transparent effect where you want the color tracks to read boldly rather than washed out. |
| Bump map | A lit particle: it uses the normal texture to catch scene lighting, alpha-blended over the background. | Effects that should look shaded and three-dimensional rather than flat and self-lit. |
| Decal bump map | A multiply-style blend that darkens and tints the surface behind it, like a stamped decal. | Marks laid onto a surface — burns, stains, and impact decals. |

## About the "Depth" Modes

The Depth additive, Depth transparent, and Depth inverse modes pair each blend with a *depth sprite*:
they read the emitter's depth texture to give every particle real per-pixel depth instead of the flat
billboard quad's single depth value. That lets a particle meet solid geometry correctly — sorting and
occluding against it pixel by pixel, rather than the whole quad popping wholly in front of or behind
the surface. Use a depth mode when an effect sits against or passes through scene geometry; use the
plain mode when it floats in open space, since the plain mode is cheaper.

<!-- Media: ref-depth-mode-compare -->

## The Bump Modes Need the Normal Texture

Bump map and Decal bump map read the emitter's normal texture to work. If that texture is missing or
flat, these modes have nothing to shade against and will not look as intended. Selecting Bump map also
forces the editor's "Always face camera" option on — the checkbox shows checked and disabled —
reflecting that bump-mapped particles are meant to be viewed head-on.

## Modes the Editor Does Not Expose

The underlying engine defines a few more blend modes — stencil-based darkening, a heat-shimmer mode,
and a scanline mode — that this editor intentionally does not list, matching the original tool. The
ten modes above are the full set you can choose from in the UI.
