// PrimitivesGallery.tsx — demo route for ?demo=primitives.
//
// Renders live instances of each primitive at 2-3 different configurations.
// Reachable at:
//   Browser mode:  http://localhost:5174/?demo=primitives
//   Native mode:   https://app.local/?demo=primitives
//
// This screen owns no bridge calls and supplies its own static fixture data.
// Removed once Screens 4/5/6/8 ship and provide real consumption sites.

import { useState } from "react";
import { Spinner } from "@/primitives/Spinner";
import { ColorButton } from "@/primitives/ColorButton";
import { TexturePalette } from "@/primitives/TexturePalette";
import { RandomParam } from "@/primitives/RandomParam";
import type { RgbColor } from "@/primitives/palette-store";
import type { RandomParamValue } from "@/primitives/RandomParam";
import type { TextureItem } from "@/primitives/TexturePalette";

// Minimal 1×1 placeholder data URIs for demo thumbnails.
// The 1×1 PNGs have distinct hues so cells are visually distinct.
const THUMB_RED =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8/5+hHgAHggJ/PchI6QAAAABJRU5ErkJggg==";
const THUMB_GREEN =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";
const THUMB_BLUE =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

const DEMO_TEXTURES: TextureItem[] = [
  { path: "textures/fire_01.tga",   label: "fire_01",   thumbnailSrc: THUMB_RED   },
  { path: "textures/smoke_02.tga",  label: "smoke_02",  thumbnailSrc: THUMB_GREEN },
  { path: "textures/spark_03.tga",  label: "spark_03",  thumbnailSrc: THUMB_BLUE  },
  { path: "textures/missing_04.tga",label: "missing_04",thumbnailSrc: null        },
  { path: "textures/ember_05.tga",  label: "ember_05",  thumbnailSrc: THUMB_RED   },
];

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

  // TexturePalette state
  const [texVal, setTexVal] = useState<string | null>("textures/fire_01.tga");

  // RandomParam state
  const [rp1, setRp1] = useState<RandomParamValue>({ mode: "Constant", value: 1 });
  const [rp2, setRp2] = useState<RandomParamValue>({ mode: "UniformRange", min: 0.5, max: 2.0 });
  const [rp3, setRp3] = useState<RandomParamValue>({ mode: "Normal", mean: 1.0, sigma: 0.25 });

  return (
    <div className="h-full w-full overflow-y-auto bg-bg text-text">
      <header className="sticky top-0 z-10 flex h-10 items-center gap-3 border-b border-border bg-bg px-6">
        <span className="font-semibold">AloParticleEditor</span>
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

        {/* ── TexturePalette ── */}
        <Section title="TexturePalette">
          <Row label="5 items (1 missing), cellSize=64">
            <TexturePalette
              items={DEMO_TEXTURES}
              value={texVal}
              onChange={setTexVal}
              cellSize={64}
              onBrowse={(p) => console.log("[demo] browse", p)}
              onClear={(p) => console.log("[demo] clear", p)}
              onReveal={(p) => console.log("[demo] reveal", p)}
            />
            <span className="mt-1 text-[10px] text-text-3">selected: {texVal ?? "(none)"}</span>
          </Row>
          <Row label="Empty palette">
            <TexturePalette
              items={[]}
              value={null}
              onChange={() => {}}
            />
          </Row>
          <Row label="Small cells (cellSize=40), no callbacks → context items disabled">
            <TexturePalette
              items={DEMO_TEXTURES.slice(0, 3)}
              value={null}
              onChange={() => {}}
              cellSize={40}
            />
          </Row>
        </Section>

        {/* ── RandomParam ── */}
        <Section title="RandomParam">
          <Row label="Starts Constant">
            <div className="w-52">
              <RandomParam value={rp1} onChange={setRp1} step={0.1} decimals={2} />
            </div>
            <span className="mt-0.5 text-[10px] text-text-3">
              {JSON.stringify(rp1)}
            </span>
          </Row>
          <Row label="Starts UniformRange">
            <div className="w-64">
              <RandomParam value={rp2} onChange={setRp2} step={0.1} decimals={2} unit="s" />
            </div>
            <span className="mt-0.5 text-[10px] text-text-3">
              {JSON.stringify(rp2)}
            </span>
          </Row>
          <Row label="Starts Normal">
            <div className="w-64">
              <RandomParam value={rp3} onChange={setRp3} step={0.05} decimals={2} />
            </div>
            <span className="mt-0.5 text-[10px] text-text-3">
              {JSON.stringify(rp3)}
            </span>
          </Row>
        </Section>
      </main>
    </div>
  );
}
