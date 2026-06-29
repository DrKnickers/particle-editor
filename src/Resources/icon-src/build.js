// Particle Editor app icon — ORBITAL: a bright azure core with a tilted
// orbit and particles (an emitter/atom). Flat with consistent shading,
// transparent, no plate, tamed (no bloom). Site accent #4a8bff.
// Size ramp: full orbit + 3 particles (>=48); thicker orbit + 2 particles
// (32); a tilted orbit LINE + 2 particles through the core (16) — so it
// still reads as an orbit, not a random cluster.
const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');

const OUT = path.join(__dirname, 'out');
fs.mkdirSync(OUT, { recursive: true });
const THETA = -22;  // orbit tilt (deg)

const ptsAt = (c, rx, ry, list) => list.map(deg => {
  const t = deg*Math.PI/180, x = c + rx*Math.cos(t), y = c + ry*Math.sin(t);
  return { x, y, front: y >= c };
});
const dot = (p, r) => `<circle cx="${p.x.toFixed(2)}" cy="${p.y.toFixed(2)}" r="${r.toFixed(2)}" fill="url(#part)"/>`;

// p: { mode:'full'|'line', rx, ry, coreR, sw, pr, ts }
function iconSVG(size, p) {
  const c = size/2;
  const defs = `<defs>
    <radialGradient id="core" cx="42%" cy="36%" r="62%"><stop offset="0%" stop-color="#8cbcff"/><stop offset="60%" stop-color="#4a8bff"/><stop offset="100%" stop-color="#2f72e0"/></radialGradient>
    <radialGradient id="part" cx="40%" cy="34%" r="64%"><stop offset="0%" stop-color="#bcd8ff"/><stop offset="100%" stop-color="#5b97ff"/></radialGradient>
    <clipPath id="front"><rect x="${-size}" y="${c}" width="${3*size}" height="${size}"/></clipPath>
  </defs>`;
  if (p.mode === 'line') {
    const ps = ptsAt(c, p.rx, 0, p.ts);
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">${defs}
      <g transform="rotate(${THETA} ${c} ${c})">
        <line x1="${(c-p.rx).toFixed(2)}" y1="${c}" x2="${(c+p.rx).toFixed(2)}" y2="${c}" stroke="#4f8eee" stroke-width="${p.sw.toFixed(2)}" stroke-linecap="round"/>
        ${ps.map(q => dot(q, p.pr)).join('')}
      </g>
      <circle cx="${c}" cy="${c}" r="${p.coreR.toFixed(2)}" fill="url(#core)"/>
    </svg>`;
  }
  const ps = ptsAt(c, p.rx, p.ry, p.ts), back = ps.filter(q => !q.front), front = ps.filter(q => q.front);
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">${defs}
    <g transform="rotate(${THETA} ${c} ${c})">
      <ellipse cx="${c}" cy="${c}" rx="${p.rx.toFixed(2)}" ry="${p.ry.toFixed(2)}" fill="none" stroke="#3f7fe6" stroke-width="${p.sw.toFixed(2)}"/>
      ${back.map(q => dot(q, p.pr)).join('')}
    </g>
    <circle cx="${c}" cy="${c}" r="${p.coreR.toFixed(2)}" fill="url(#core)"/>
    <g transform="rotate(${THETA} ${c} ${c})">
      <g clip-path="url(#front)"><ellipse cx="${c}" cy="${c}" rx="${p.rx.toFixed(2)}" ry="${p.ry.toFixed(2)}" fill="none" stroke="#6ba2ff" stroke-width="${p.sw.toFixed(2)}"/></g>
      ${front.map(q => dot(q, p.pr)).join('')}
    </g>
  </svg>`;
}

function large(size) {
  return { mode:'full', rx:size*0.40, ry:size*0.155, coreR:size*0.135, sw:size*0.045, pr:size*0.075, ts:[50,150,280] };
}
const HINTED = {
  32: { mode:'full', rx:11,  ry:4.6, coreR:4.8, sw:2.4, pr:3.2, ts:[40,210] },  // thicker orbit, 2 dots
  16: { mode:'line', rx:5.2, ry:0,   coreR:3.2, sw:1.6, pr:2.2, ts:[0,180] },   // orbit line + 2 dots
};

function render(size) {
  const p = HINTED[size] || large(size);
  if (p.rx + p.pr > size/2) throw new Error(`size ${size}: rx+pr ${p.rx+p.pr} exceeds half ${size/2}`);
  return new Resvg(iconSVG(size, p), { fitTo: { mode:'width', value:size } }).render().asPng();
}

function buildIco(entries) {
  const N = entries.length, header = Buffer.alloc(6);
  header.writeUInt16LE(0,0); header.writeUInt16LE(1,2); header.writeUInt16LE(N,4);
  const dir = Buffer.alloc(16*N); let off = 6 + 16*N;
  entries.forEach((e,i)=>{ const o=i*16;
    dir[o]=e.size>=256?0:e.size; dir[o+1]=e.size>=256?0:e.size; dir.writeUInt16LE(1,o+4); dir.writeUInt16LE(32,o+6);
    dir.writeUInt32LE(e.buf.length,o+8); dir.writeUInt32LE(off,o+12); off += e.buf.length; });
  return Buffer.concat([header, dir, ...entries.map(e=>e.buf)]);
}

const sizes = [16, 32, 48, 64, 128, 256];
const pngs = {};
for (const s of sizes) { pngs[s] = render(s); fs.writeFileSync(path.join(OUT, `size_${s}.png`), pngs[s]); }
fs.writeFileSync(path.join(OUT, 'master_1024.png'), render(1024));
fs.writeFileSync(path.join(OUT, 'logo.ico'), buildIco(sizes.map(s => ({ size:s, buf:pngs[s] }))));
fs.writeFileSync(path.join(OUT, 'logo.svg'), iconSVG(256, large(256)));

const b64 = s => pngs[s].toString('base64');
const rowA = sizes.map((s,i)=>{const x=40+i*112;return `<image x="${x}" y="48" width="80" height="80" href="data:image/png;base64,${b64(s)}"/><text x="${x+40}" y="146" fill="#9aa1ad" font-size="12" text-anchor="middle" font-family="sans-serif">${s}px</text>`;}).join('');
const rowB = [16,32,48].map((s,i)=>{const x=70+i*215;return `<image x="${x}" y="196" width="168" height="168" image-rendering="pixelated" href="data:image/png;base64,${b64(s)}"/><text x="${x+84}" y="384" fill="#cdd3dd" font-size="12" text-anchor="middle" font-family="sans-serif">${s}px true pixels</text>`;}).join('');
fs.writeFileSync(path.join(OUT, 'preview_sheet.png'), new Resvg(`<svg xmlns="http://www.w3.org/2000/svg" width="730" height="410" viewBox="0 0 730 410"><rect width="730" height="410" fill="#0b0d12"/><text x="30" y="28" fill="#eef1f6" font-size="15" font-weight="700" font-family="sans-serif">logo.ico — orbital, packed sizes</text>${rowA}<line x1="30" y1="168" x2="700" y2="168" stroke="#222"/>${rowB}</svg>`, { fitTo:{mode:'width',value:1460} }).render().asPng());

const stat = f => fs.statSync(path.join(OUT, f)).size;
console.log('Wrote to', OUT);
for (const s of sizes) console.log(`  size_${s}.png ${stat('size_'+s+'.png')} B`);
console.log(`  logo.ico ${stat('logo.ico')} B (sizes ${sizes.join(', ')})`);
console.log(`  master_1024.png ${stat('master_1024.png')} B  logo.svg ${stat('logo.svg')} B`);
