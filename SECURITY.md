# Security policy

## Reporting a vulnerability

For exploitable issues — anything that lets a `.alo` file or a loaded mod trigger code execution, write outside its sandbox, exfiltrate data, or otherwise cross a trust boundary — **don't open a public issue**. Use GitHub's [private security advisory](https://github.com/DrKnickers/particle-editor/security/advisories/new) feature instead.

Private advisories let us discuss a fix and prepare a coordinated release before the details are publicly visible.

For non-exploitable bugs (crashes from your own input, layout glitches, rendering oddities), a public issue using the standard bug-report template is the right channel.

## Scope

The editor parses `.alo` files (a Petroglyph chunk format) and shader / texture assets from the configured EaW / FoC install. The relevant trust boundaries:

- **`.alo` file content** — the parser handles malformed input by clamping to sentinels and logging, but a crafted file that bypasses those checks and reaches memory corruption is in scope.
- **Mod assets resolved via the `FileManager`** — shaders compile through `D3DXCreateEffectFromFile` and textures load through `D3DXCreateTextureFromFile`. A crafted `.fx` or texture that exploits a `d3dx9_43.dll` vulnerability is in scope to the extent the editor surfaces the issue.
- **Registry-stored editor settings** (`HKCU\Software\AloParticleEditor`) — the editor validates length / NaN / range on load. Anything that crashes by passing those checks is in scope.

Out of scope: bugs in `d3dx9_43.dll` itself (Microsoft DXSDK), bugs in the game's own runtime, and bugs that require the user to already have arbitrary-write access to the machine (you've already lost at that point).

## What to expect

This fork is maintained as a side project. A response on a private advisory might take days. If the issue is critical, say so in the advisory title — we'll prioritise.
