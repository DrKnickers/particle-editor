// [houdini] The pinned Houdini build and the environment its children get
// (plan decision 1, §3.3).
//
// The binary paths are constants, not configuration: §24.2 forbids running an
// executable a caller names. SideFX does not Authenticode-sign these binaries,
// so each one is pinned by its SHA-256 instead of a signature, and a Houdini
// upgrade is a deliberate edit here that also changes every cache key.
//
// The child environment is built from an allowlist. Subtracting variables
// would let HSITE, PYTHONPATH, OCIO, the *_DSO_PATH family and the owner's
// HOUDINI_* settings through; here they are absent by construction.
import * as fs from "node:fs";
import { createHash } from "node:crypto";
import { win32 } from "node:path";

export const HOUDINI_BUILD = "19.5.435";
export const HOIIOTOOL_VERSION = "OpenImageIO 2.3.14";
export const HFS = "C:\\Program Files\\Side Effects Software\\Houdini 19.5.435";
export const THREADS = 4;

const bin = (name) => win32.join(HFS, "bin", name);
export const PINNED = {
  hython: { path: bin("hython.exe"), sha256: "56d5a51a5ec6f630b0a8eb44712550c7bb5ade3afa71d94b6d4d1d396a58e13c" },
  hoiiotool: { path: bin("hoiiotool.exe"), sha256: "95ef11d97c76c0187d4ab0d34d4a66b0933d4067fe59d88b04e56e8f4fb2c47d" },
  // hython starts these itself; pin them so a swapped renderer is caught too.
  mantra: { path: bin("mantra.exe"), sha256: "209bdda045212d2927965ae61a2c884561d4e0edc9c5364546ce4373acac0b8b" },
  husk: { path: bin("husk.exe"), sha256: "a2a09b5601b70cb6a0bebd1396e454c4ae5a0657dce6ea3f0d0333d9d309afcb" },
};

// DDS entries are compressed by the DirectX SDK's texconv, the SDK the build
// already needs for d3dx9. The compression runs in d3dx9_43.dll, which
// texconv loads from System32 (the SDK's bin folder has none); it is the
// same file as libs/redist/d3dx9_43.dll. Both are pinned, so a changed
// encoder is caught, and texconv's hash is part of a DDS entry's cache key.
export const TEXCONV_BUILD = "DirectX SDK (June 2010)";
export const DXSDK = "C:\\Program Files (x86)\\Microsoft DirectX SDK (June 2010)";
export const TEXCONV = {
  texconv: { path: win32.join(DXSDK, "Utilities", "bin", "x64", "texconv.exe"),
    sha256: "0121bcf916a91a3267fc16af09f4932da2f2cb5165fad4d47821551732d18a21" },
  d3dx9: { path: "C:\\Windows\\System32\\d3dx9_43.dll",
    sha256: "84b900dbd7fa978d6e0caee26fc54f2f61d92c9c75d10b35f00e3e82cd1d67b4" },
};

const fail = (message, exitCode = 1) => Object.assign(new Error(message), { exitCode });

export function verifyPinned({ io = fs, pins = PINNED, build = `Houdini ${HOUDINI_BUILD}` } = {}) {
  for (const [name, pin] of Object.entries(pins)) {
    let bytes;
    try { bytes = io.readFileSync(pin.path); }
    catch { throw fail(`${build} ${name} is missing: ${pin.path}`); }
    const actual = createHash("sha256").update(bytes).digest("hex");
    if (actual !== pin.sha256) throw fail(`${name} differs from the pinned ${build} build`);
  }
}

export const verifyTexconv = ({ io = fs } = {}) => verifyPinned({ io, pins: TEXCONV, build: TEXCONV_BUILD });

// `stage` is the run's working folder; everything Houdini writes lands there.
export function childEnvironment(stage, { inherit = process.env, threads = THREADS } = {}) {
  const systemRoot = inherit.SystemRoot;
  if (!systemRoot) throw fail("SystemRoot is not set");
  return {
    SystemRoot: systemRoot,
    SystemDrive: inherit.SystemDrive ?? systemRoot.slice(0, 2),
    NUMBER_OF_PROCESSORS: inherit.NUMBER_OF_PROCESSORS ?? "1",
    TEMP: stage, TMP: stage, HOUDINI_TEMP_DIR: stage, HOME: stage,
    PATH: `${win32.join(systemRoot, "System32")};${win32.join(HFS, "bin")}`,
    // Without __HVER__ Houdini ignores the variable and reads the owner's prefs.
    HOUDINI_USER_PREF_DIR: win32.join(stage, "prefs", "houdini__HVER__"),
    HOUDINI_NO_ENV_FILE: "1",
    HOUDINI_PACKAGE_SKIP: "1",
    HOUDINI_MAXTHREADS: String(threads),
    PYTHONNOUSERSITE: "1",
    PYTHONDONTWRITEBYTECODE: "1",
  };
}
