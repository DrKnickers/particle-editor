// PrimitivesGallery.tsx — demo route for ?demo=primitives.
//
// Renders live instances of each primitive at 2-3 different configurations.
// Reachable at:
//   Browser mode:  http://localhost:5174/?demo=primitives
//   Native mode:   https://app.local/?demo=primitives
//
// This screen owns no bridge calls and supplies its own static fixture data.

import { useState } from "react";
import { Spinner } from "@/primitives/Spinner";
import { ColorButton } from "@/primitives/ColorButton";
import type { RgbColor } from "@/primitives/palette-store";

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="mb-8">
      <h2 className="mb-3 border-b border-border pb-1 text-sm font-semibold text-text-2">
        {title}
      </h2>
      <div className="space-y-4">{children}</div>
    </section>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-[10px] text-text-3">{label}</span>
      <div>{children}</div>
    </div>
  );
}

export function PrimitivesGallery() {
  // Spinner state
  const [spin1, setSpin1] = useState(0);
  const [spin2, setSpin2] = useState(12.5);
  const [spin3, setSpin3] = useState(0.001);

  // ColorButton state
  const [color1, setColor1] = useState<RgbColor>({ r: 255, g: 128, b: 0 });
  const [color2, setColor2] = useState<RgbColor>({ r: 64, g: 160, b: 255 });

  return (
    <div className="h-full w-full overflow-y-auto bg-bg text-text">
      <header className="sticky top-0 z-10 flex h-10 items-center gap-3 border-b border-border bg-bg px-6">
        <span className="font-semibold">Particle Editor</span>
        <span className="text-text-3">·</span>
        <span className="text-xs text-text-2">Primitives gallery</span>
        <span className="ml-auto text-[10px] text-text-3">?demo=primitives</span>
      </header>

      <main className="mx-auto max-w-2xl px-6 py-6">
        {/* ── Spinner ── */}
        <Section title="Spinner">
          <Row label="Integer, no unit, default density">
            <div className="w-32">
              <Spinner value={spin1} onChange={setSpin1} step={1} aria-label="Demo spinner 1" />
            </div>
            <span className="mt-0.5 text-[10px] text-text-3">value: {spin1}</span>
          </Row>
          <Row label="Float, unit='deg/s', tight density, min=-180, max=180">
            <div className="w-40">
              <Spinner
                value={spin2}
                onChange={setSpin2}
                step={0.5}
                decimals={1}
                unit="deg/s"
                min={-180}
                max={180}
                density="tight"
                aria-label="Demo spinner 2"
              />
            </div>
            <span className="mt-0.5 text-[10px] text-text-3">value: {spin2}</span>
          </Row>
          <Row label="Scientific notation, step=1e-4, loose density">
            <div className="w-40">
              <Spinner
                value={spin3}
                onChange={setSpin3}
                step={0.0001}
                decimals={4}
                density="loose"
                aria-label="Demo spinner 3"
              />
            </div>
            <span className="mt-0.5 text-[10px] text-text-3">value: {spin3}</span>
          </Row>
        </Section>

        {/* ── ColorButton ── */}
        <Section title="ColorButton">
          <Row label="Default density">
            <ColorButton value={color1} onChange={setColor1} aria-label="Demo color 1" />
            <span className="mt-0.5 text-[10px] text-text-3">
              rgb({color1.r}, {color1.g}, {color1.b})
            </span>
          </Row>
          <Row label="Tight density">
            <ColorButton
              value={color2}
              onChange={setColor2}
              density="tight"
              aria-label="Demo color 2"
            />
            <span className="mt-0.5 text-[10px] text-text-3">
              rgb({color2.r}, {color2.g}, {color2.b})
            </span>
          </Row>
          <Row label="Disabled">
            <ColorButton
              value={{ r: 128, g: 128, b: 128 }}
              onChange={() => {}}
              disabled
              aria-label="Demo color disabled"
            />
          </Row>
        </Section>

      </main>
    </div>
  );
}
