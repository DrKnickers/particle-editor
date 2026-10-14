// [houdini] The in-game review kit (plan §3.7): a throwaway mod that shows one
// generated texture in Forces of Corruption.
//   node scripts/houdini/review-kit.mjs --entry <cache entry> [--exe <ParticleEditor.exe>]
//   node scripts/houdini/review-kit.mjs --record <kit> --status passed|failed --notes <text>
//
// The game spawns the move-order click marker, P_MOUSE_CLICK.ALO, wherever a
// player right-clicks a move order. The kit replaces it with the editor's
// review fixture for the texture's shape (one motionless particle at scale 40,
// white tint, lifted 40 units so the terrain doesn't hide its lower half), and
// lets the marker live long enough for the fixture to play out: 150 frames
// instead of 30 for a 4 s flipbook, and as much longer as a longer one needs.
// It is staged under <root>\review\<id>-<hash8>\:
//   Data\Art\Textures\<texture>     the entry's texture, byte for byte
//   Data\Art\Models\P_MOUSE_CLICK.ALO
//   Data\XML\PARTICLES.XML          the game's own, read from its install
//   REVIEW.md                       how to launch it and what to look for
//   review.json                     what the kit shows, and gameVerification
// The kit is never written into the game or a mod (the root's own rules), and
// an existing kit is refused. --record writes the reviewer's verdict into
// review.json: the cache entry is never rewritten.
//
// Prints one `[houdini-review]` JSON line. Exit 0 ok, 1 failed (the editor or
// the game install), 2 bad input, 4 the entry is not valid.
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { readStockFile, resolveReplayGameRoots } from "../replay-proof.mjs";
import { fail } from "./rules.mjs";
import { sha256 } from "./cache.mjs";
import { kindFor, lifetimeTicks, openEntry } from "./validate.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
// src/effect/HoudiniValidationFixture.h: kReviewLift.
export const REVIEW_LIFT = 40;
export const MARKER = "P_MOUSE_CLICK.ALO";
// Observed in game: 150 frames held the 4 s fireball to its last cell. A
// longer fixture gets as many more, in proportion.
export const MARKER_FRAMES = 150;
const MARKER_SECONDS = 4;
export const markerFrames = (kind) => Math.max(MARKER_FRAMES, Math.ceil(MARKER_FRAMES * lifetimeTicks(kind) / 60 / MARKER_SECONDS));
export const STATUSES = ["passed", "failed"];
const MAX_NOTES = 2000;

export function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i], value = argv[i + 1];
    if (!["--entry", "--exe", "--record", "--status", "--notes"].includes(flag) || value === undefined ||
        value.startsWith("--") || flag.slice(2) in args)
      throw fail("usage: review-kit.mjs --entry <cache entry> [--exe <ParticleEditor.exe>] | " +
        "--record <kit> --status passed|failed --notes <text>", 2);
    args[flag.slice(2)] = ["--status", "--notes"].includes(flag) ? value : resolve(value);
  }
  if (args.record !== undefined) {
    if (args.entry !== undefined || args.exe !== undefined) throw fail("--record takes only --status and --notes", 2);
    if (!STATUSES.includes(args.status)) throw fail(`--status must be ${STATUSES.join(" or ")}`, 2);
    if (typeof args.notes !== "string" || !args.notes.trim() || args.notes.length > MAX_NOTES)
      throw fail(`--notes must be 1 to ${MAX_NOTES} characters`, 2);
  } else if (args.entry === undefined || args.status !== undefined || args.notes !== undefined) {
    throw fail("--entry is required; --status and --notes go with --record", 2);
  }
  return args;
}

// The game's PARTICLES.XML with only the click marker's lifetime changed. The
// marker must be the model the kit replaces, or the kit would show nothing.
export function patchParticlesXml(bytes, lifetimeFrames = MARKER_FRAMES) {
  const xml = bytes.toString("latin1");
  const blocks = [...xml.matchAll(/<Particle\s+Name="GUI_Move_Command_Particle"\s*>[\s\S]*?<\/Particle>/g)];
  if (blocks.length !== 1) throw fail(`PARTICLES.XML has ${blocks.length} GUI_Move_Command_Particle entries, not one`, 1);
  const [block] = blocks;
  const model = block[0].match(/<Land_Model_Name>\s*([^<]*?)\s*<\/Land_Model_Name>/);
  if (model?.[1].toUpperCase() !== MARKER) throw fail(`GUI_Move_Command_Particle shows ${model?.[1] ?? "no land model"}, not ${MARKER}`, 1);
  const frames = [...block[0].matchAll(/<Particle_Lifetime_Frames>\s*\d+\s*<\/Particle_Lifetime_Frames>/g)];
  if (frames.length !== 1) throw fail("GUI_Move_Command_Particle has no single Particle_Lifetime_Frames", 1);
  const patched = block[0].replace(frames[0][0], `<Particle_Lifetime_Frames>${lifetimeFrames}</Particle_Lifetime_Frames>`);
  return Buffer.from(xml.slice(0, block.index) + patched + xml.slice(block.index + block[0].length), "latin1");
}

// The kit launches Forces of Corruption, so its XML comes from that install.
export function focRoot(roots) {
  const root = roots.find((path) => basename(path).toLowerCase() === "corruption");
  if (!root) throw fail("the review kit needs the Forces of Corruption install (its corruption folder) as game data", 1);
  return root;
}

export function checklist({ kit, game, provenance, kind, lifetime }) {
  const { name } = provenance.texture;
  const launcher = game ? join(game, "swfoc.exe") : "swfoc.exe";
  return [
    `# In-game review: ${provenance.generatorId}`,
    "",
    `Texture \`${name}\` (${provenance.outputProfile}, ${provenance.tier} tier, seed ${provenance.seed}),`,
    `shown by the \`${kind}\` review fixture for ${lifetime} s each time a move order is given.`,
    "",
    "## Launch",
    "",
    "1. Set the window size in the game's registry profile,",
    "   `HKCU\\Software\\Petroglyph\\StarWars FOC\\Profiles\\<profile>\\Player`, values",
    "   `WindowWidth` and `WindowHeight`. The game ignores `-W`/`-H`. In the same key, set",
    "   `WidescreenAspectRatio` to the window's width over its height: the game builds its 3D view",
    "   at that aspect and stretches it to the window, so any other value draws every sprite",
    "   stretched. It is a DWORD holding the float's bits; in PowerShell, for a 1280 x 720 window,",
    "   `[BitConverter]::ToInt32([BitConverter]::GetBytes([single](1280 / 720)), 0)`. Note the old",
    "   values and put them back afterwards.",
    "2. Start the game windowed, with this kit as its mod:",
    "",
    "   ```text",
    `   "${launcher}" LANGUAGE=ENGLISH -WINDOWED Modpath=${kit}`,
    "   ```",
    "",
    "3. Start a land skirmish. Select some units and right-click open ground: each move order",
    `   spawns the texture above the click point for ${lifetime} s. Repeat at a few zoom levels.`,
    "4. Keep the game in front. In the background it stops rendering, and a recording of it",
    "   freezes.",
    "",
    "## What to look for",
    "",
    "- a black or white box around the texture: the alpha is wrong;",
    "- a dark fringe at its edge;",
    "- for a flipbook: the frames in order, no flicker, no neighbouring cell showing;",
    "- for an additive texture: how much it blooms;",
    "- bleed or shimmer when zoomed out.",
    "",
    "The fixture holds the particle still, untinted and unfaded, so it shows the texture as",
    "it is. It is not an effect: a pop in and out is the fixture.",
    "",
    "## Record the verdict",
    "",
    "```text",
    `node scripts/houdini/review-kit.mjs --record "${kit}" --status passed|failed --notes "<what you saw>"`,
    "```",
    "",
    "`passed` means the texture arrives in game as intended. It says nothing about whether an",
    "effect built on it is finished.",
    "",
  ].join("\r\n");
}

export function stage({ entry, exe = join(repoRoot, "x64", "Release", "ParticleEditor.exe") }, {
  io = fs, roots = () => resolveReplayGameRoots({}), fixture = writeFixture, stockFile = readStockFile, open = openEntry,
} = {}) {
  const { provenance, root, published } = open(entry, io);
  const { name, grid, blend } = provenance.texture;
  const kind = kindFor({ blend, grid }), lifetimeFrames = markerFrames(kind);
  const hash8 = name.slice(-12, -4);
  const kit = join(root, "review", `${provenance.generatorId}-${hash8}`);
  if (provenance.tier !== "final") throw fail(`the review kit shows a final-tier texture; this entry is ${provenance.tier}`, 2);
  const game = focRoot(roots());
  const xml = stockFile([game], "XML\\PARTICLES.XML");
  if (!xml) throw fail(`no PARTICLES.XML in the game data at ${game}`, 1);
  const patched = patchParticlesXml(xml, lifetimeFrames);

  io.mkdirSync(dirname(kit), { recursive: true });
  try { io.mkdirSync(kit); }
  catch (error) { throw error.code === "EEXIST" ? fail(`the kit already exists; delete it to stage it again: ${kit}`, 2) : error; }
  try {
    const data = (...parts) => { const path = join(kit, "Data", ...parts); io.mkdirSync(dirname(path), { recursive: true }); return path; };
    io.writeFileSync(data("Art", "Textures", name), published);
    if (sha256(io.readFileSync(data("Art", "Textures", name))) !== provenance.contentHash) throw fail("the kit's texture copy differs", 1);
    const shown = fixture(exe, kind, name, data("Art", "Models", MARKER));
    if (shown.reloaded !== true || shown.colorTexture !== name || shown.lift !== REVIEW_LIFT || shown.kind !== kind)
      throw fail(`the review fixture does not show ${name} lifted ${REVIEW_LIFT}: ${JSON.stringify(shown)}`, 1);
    io.writeFileSync(data("XML", "PARTICLES.XML"), patched);
    const review = {
      schemaVersion: 1, generatorId: provenance.generatorId, generatorVersion: provenance.generatorVersion,
      seed: provenance.seed, parameters: provenance.parameters, tier: provenance.tier, cacheKey: provenance.cacheKey,
      contentHash: provenance.contentHash, texture: name, fixture: { kind, lifetime: shown.lifetime, lift: shown.lift },
      markerFrames: lifetimeFrames, gameVerification: "not-run",
    };
    io.writeFileSync(join(kit, "review.json"), `${JSON.stringify(review, null, 2)}\n`);
    io.writeFileSync(join(kit, "REVIEW.md"), checklist({ kit, game, provenance, kind, lifetime: shown.lifetime }));
  } catch (error) {
    io.rmSync(kit, { recursive: true, force: true });
    throw error;
  }
  return { ok: true, exitCode: 0, entry, kit, texture: name, kind, contentHash: provenance.contentHash };
}

// The reviewer's verdict, against the bytes the kit still holds.
export function recordVerdict({ record: kit, status, notes }, { io = fs, today = () => new Date().toISOString().slice(0, 10) } = {}) {
  let review;
  try { review = JSON.parse(io.readFileSync(join(kit, "review.json"), "utf8")); }
  catch { throw fail(`not a review kit: ${kit}`, 2); }
  if (review?.schemaVersion !== 1 || typeof review.texture !== "string" || !/^[a-z0-9_-]+\.(tga|dds)$/.test(review.texture))
    throw fail(`not a review kit: ${kit}`, 2);
  let texture;
  try { texture = io.readFileSync(join(kit, "Data", "Art", "Textures", review.texture)); }
  catch { throw fail(`the kit lost its texture: ${review.texture}`, 4); }
  if (sha256(texture) !== review.contentHash) throw fail(`the kit's ${review.texture} no longer matches ${review.contentHash}`, 4);
  review.gameVerification = { status, date: today(), notes: notes.trim() };
  io.writeFileSync(join(kit, "review.json"), `${JSON.stringify(review, null, 2)}\n`);
  return { ok: true, exitCode: 0, kit, texture: review.texture, gameVerification: review.gameVerification };
}

function writeFixture(exe, kind, name, path) {
  const r = spawnSync(exe, ["--gen-houdini-review-fixture", kind, name, path], { windowsHide: true, encoding: "utf8", timeout: 60000 });
  const line = r.stdout?.match(/^\[houdini-fixture\] (.+)$/m)?.[1];
  if (r.status !== 0 || !line || !fs.existsSync(path)) throw fail(`the review fixture flag failed (exit ${r.status}): ${r.stderr}`, 1);
  return JSON.parse(line);
}

function main() {
  let record;
  try {
    const args = parseArgs(process.argv.slice(2));
    record = args.record !== undefined ? recordVerdict(args) : stage(args);
  } catch (error) { record = { ok: false, exitCode: error.exitCode ?? 1, error: error.message }; }
  console.log(`[houdini-review] ${JSON.stringify(record)}`);
  process.exitCode = record.exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) main();
