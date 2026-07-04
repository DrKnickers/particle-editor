#pragma once
// HeadlessComposite — the CPU alpha-composite for the headless `--record`
// capture path (PE_RECORD_HEADLESS). Extracted to a header so the pixel math
// is unit-testable without the GUI (tests/test_headless_composite.cpp); the
// full path is also covered end-to-end by scripts/clip-verify/headless-golden.mjs.
//
// Layers: engine UNDER (opaque, from AlphaCompositor::CaptureSnapshotBgra at its
// scene-rect origin) and UI OVER (from CapturePreview, STRAIGHT — not
// premultiplied — alpha; the viewport region comes back alpha=0). The output
// frame is the UI's dimensions (the UI is the full window client content); the
// engine shows through only where the UI is transparent (alpha < 255):
//   out.rgb = ui.a·ui.rgb + (255−ui.a)·eng.rgb   (÷255, per channel, rounded)
// EncodeBgraToPng writes 32bppRGB (alpha ignored downstream), so only RGB is
// composited. Both layers are straight alpha — never mix premultiplied here.

#include <cstdint>
#include <vector>

namespace host {

// Composite `ui` (uiW×uiH, BGRA, straight alpha) over `eng` (eW×eH, BGRA,
// opaque) placed at (eX,eY) in the UI frame. Writes the uiW×uiH result to
// `out`. `eng` may be empty (then `out` == `ui`). Pixels of the engine layer
// that fall outside the UI frame are clipped.
inline void CompositeUiOverEngine(
    const std::vector<unsigned char>& ui, int uiW, int uiH,
    const std::vector<unsigned char>& eng, int eW, int eH, int eX, int eY,
    std::vector<unsigned char>& out, int& outW, int& outH)
{
    outW = uiW; outH = uiH;
    out = ui;   // opaque panels pass straight through; blend only the hole
    if (eng.empty() || eW <= 0 || eH <= 0 || uiW <= 0 || uiH <= 0) return;
    for (int y = 0; y < eH; ++y)
    {
        const int fy = eY + y;
        if (fy < 0 || fy >= uiH) continue;
        for (int x = 0; x < eW; ++x)
        {
            const int fx = eX + x;
            if (fx < 0 || fx >= uiW) continue;
            const size_t o = (static_cast<size_t>(fy) * uiW + fx) * 4u;
            const uint8_t a = ui[o + 3];
            if (a == 255) continue;   // opaque UI → engine fully hidden
            const size_t e = (static_cast<size_t>(y) * eW + x) * 4u;
            for (int c = 0; c < 3; ++c)
                out[o + c] = static_cast<uint8_t>(
                    (ui[o + c] * a + eng[e + c] * (255 - a) + 127) / 255);
        }
    }
}

}  // namespace host
