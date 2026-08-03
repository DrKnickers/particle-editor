# Blend Modes

The blend mode decides how a particle mixes with whatever is behind it. It is one of the biggest
levers on how an effect reads, so it is worth knowing what each option does. Set it in the Property
Panel under Appearance, in the Rendering section, with the Blend mode field.

The two you will use most are Additive and Transparent. Additive brightens whatever is behind the
particle and is right for anything glowing. Transparent is ordinary see-through blending and is
right for anything solid-looking that fades, such as smoke.

> **Additive ignores alpha.** An additive particle's Alpha track does nothing — brightness comes
> entirely from its color. Fade an additive glow by dropping its **color** toward black (or shrinking
> its Scale), never with the Alpha track. This one fact trips up almost every newcomer.

<!-- Media: ref-blend-mode-grid -->

## The Modes

| Mode | What it does | Reach for it when |
|------|--------------|-------------------|
| None | No blending. The particle is drawn opaque and simply covers what is behind it; its alpha is ignored. | You genuinely want a solid, fully opaque sprite. Rare for particles. |
| Additive | Adds the particle's color to the scene, so it only ever brightens. Dark parts of the texture become invisible, and the **alpha channel is ignored** — brightness comes entirely from color, so fade an additive effect with its color or scale, never with alpha. | Glows, fire, plasma, energy, sparks — anything that gives off light. |
| Transparent | Ordinary alpha blending: the particle's alpha controls how much you see it over the background. | Smoke, dust, debris, and most soft opaque effects that fade in and out. |
| Inverse | Multiplies the scene by the particle's color and texture, so it darkens instead of brightening. White leaves the scene unchanged; black drives it toward black. | Scorch marks, dark smoke, shadowy or draining effects. |
| Depth additive | Additive, but depth-aware (see below). | An additive effect that overlaps scene geometry and should intersect it correctly instead of popping in front of it. |
| Depth transparent | Transparent, but depth-aware. | Alpha-blended smoke or dust that overlaps terrain or a model and should meet it per-pixel rather than at the flat quad's edge. |
| Depth inverse | Inverse, but depth-aware. | A darkening effect that meets solid geometry and should depth-test against it correctly, per pixel. |
| Diffuse transparent | Alpha-blended like Transparent, but scene lighting adds shade and highlights to the particle instead of letting it look flat or self-lit. | A soft effect that should sit in the scene's light rather than glowing on its own. |
| Bump map | A lit particle: it uses the normal texture to catch scene lighting, alpha-blended over the background. | Effects that should look shaded and three-dimensional rather than flat and self-lit. |
| Decal bump map | A decal-style blend that can lighten or darken the surface behind it, so the mark looks lit rather than like a flat stain. | Marks laid onto a surface — burns, stains, and impact decals. |

## About the "Depth" Modes

The Depth additive, Depth transparent, and Depth inverse modes read a depth texture so that
different parts of one particle can pass correctly in front of or behind solid surfaces, instead of
the whole flat sprite popping wholly in front of or behind them. Use a depth mode when an effect
sits against or passes through scene geometry; use the plain mode when it floats in open space,
since the plain mode is cheaper.

<!-- Media: ref-depth-mode-compare -->

## The Bump Modes and Their Textures

Bump map gets its lighting shape from the emitter's **normal** texture — the image that controls
how light falls across the particle. Decal bump map gets that shape from the **color** texture
instead. If the texture a mode relies on is missing or has no useful variation, the expected
shading simply will not appear.

Selecting Bump map also shows the editor's "Always face camera" checkbox as checked and disabled.
Treat that as a hint, not a setting change: the emitter keeps whatever orientation you assigned it,
and the box only signals that bump-mapped particles are usually meant to be viewed head-on.

## Modes the Editor Does Not Expose

The underlying engine defines a few more blend modes — stencil-based darkening, a heat-shimmer mode,
and a scanline mode — that this editor intentionally does not list, matching the original tool. The
ten modes above are the full set you can choose from in the UI.
