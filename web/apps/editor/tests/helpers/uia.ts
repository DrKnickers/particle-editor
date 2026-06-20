import { spawn } from "node:child_process";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import type { UIANode } from "./a11y-normalizer";

// ESM-equivalent of __dirname (package is "type": "module").
const __dirname = path.dirname(fileURLToPath(import.meta.url));

// Path to the T3 C++ inspector executable.
// tests/helpers/ → 5 levels of ".." → repo root → x64/<Debug|Release>/
const INSPECTOR_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "..",
  "..",
  "..",
  "x64",
  process.env.A11Y_BUILD_CONFIG ?? "Debug",
  "uia_inspector.exe"
);

export async function captureUIA(
  hwnd: bigint | number,
  surfaceId: string,
  options?: { depth?: number; timeoutMs?: number }
): Promise<UIANode> {
  const hex = "0x" + BigInt(hwnd).toString(16);
  // Default depth 20: the React DOM in WebView2 HWND mode sits at depth
  // 11+ from AloHostMain (Chrome_WidgetWin_1 → BrowserRootView →
  // NonClientView → EmbeddedBrowserFrameView → BrowserView →
  // SidebarContentsSplitView(×2) → View → MultiContentsView → View →
  // React root → component tree). Depth 8 (the prior default) cut off
  // before the React content; 20 gives ~9 levels of component-tree
  // headroom above the WebView2 chrome layers.
  const args = [
    "--hwnd", hex,
    "--capture", surfaceId,
    "--depth", String(options?.depth ?? 20),
  ];
  return new Promise((resolve, reject) => {
    const child = spawn(INSPECTOR_PATH, args, { stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    const timeout = setTimeout(() => {
      child.kill();
      reject(new Error(
        `uia_inspector timeout (${options?.timeoutMs ?? 15000}ms) for surface=${surfaceId}`
      ));
    }, options?.timeoutMs ?? 15000);
    child.stdout.on("data", (chunk) => { stdout += chunk.toString("utf8"); });
    child.stderr.on("data", (chunk) => { stderr += chunk.toString("utf8"); });
    child.on("close", (code) => {
      clearTimeout(timeout);
      if (code !== 0) {
        reject(new Error(
          `uia_inspector exited ${code} for surface=${surfaceId}: ${stderr}`
        ));
        return;
      }
      try {
        resolve(JSON.parse(stdout) as UIANode);
      } catch (e) {
        reject(new Error(
          `uia_inspector produced invalid JSON for surface=${surfaceId}: ` +
          `${(e as Error).message}\nstdout: ${stdout.slice(0, 500)}`
        ));
      }
    });
  });
}

export async function discoverHostHwnd(
  options?: { windowClassName?: string; timeoutMs?: number }
): Promise<bigint> {
  // The harness launches ParticleEditor.exe with stdio:ignore and a VISIBLE
  // window (windowsHide removed in T9.3 — SW_HIDE suppresses the
  // WebView2 UIA provider, preventing UIA from traversing into the React DOM).
  //
  // Strategy: FindWindow("AloHostViewport") reliably finds the D3D popup child
  // (WS_POPUP|WS_VISIBLE), then GetParent() returns the AloHostMain top-level
  // HWND (the correct root for UIA capture). Using the viewport→parent path
  // is more robust than .NET Process.MainWindowHandle and avoids a .NET
  // dependency in this Node-ESM context.
  //
  // Class names: src/host/HostWindow.cpp:73-74
  //   kHostWindowClassName   = L"AloHostMain"     (the root we want)
  //   kHostViewportClassName = L"AloHostViewport" (the popup child, reliably found)
  const viewportClass = options?.windowClassName ?? "AloHostViewport";
  const cmd = [
    `Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices;`,
    `public class Win32HwndDiscover {`,
    `  [DllImport("user32.dll", CharSet=CharSet.Unicode)]`,
    `  public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);`,
    `  [DllImport("user32.dll")]`,
    `  public static extern IntPtr GetParent(IntPtr hWnd);`,
    `}' -ErrorAction SilentlyContinue;`,
    `$viewport = [Win32HwndDiscover]::FindWindow("${viewportClass}", $null);`,
    `if ($viewport -eq [IntPtr]::Zero) { "" }`,
    `else { $parent = [Win32HwndDiscover]::GetParent($viewport);`,
    `  if ($parent -ne [IntPtr]::Zero) { $parent.ToInt64() }`,
    `  else { $viewport.ToInt64() } }`,
  ].join(" ");
  return new Promise((resolve, reject) => {
    const ps = spawn("powershell.exe", ["-NoProfile", "-Command", cmd], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    let out = "";
    let err = "";
    const t = setTimeout(() => {
      ps.kill();
      reject(new Error(`discoverHostHwnd timeout for viewport class ${viewportClass}`));
    }, options?.timeoutMs ?? 10_000);
    ps.stdout.on("data", (c) => { out += c.toString("utf8"); });
    ps.stderr.on("data", (c) => { err += c.toString("utf8"); });
    ps.on("close", () => {
      clearTimeout(t);
      const v = out.trim();
      if (!v || v === "0") {
        reject(new Error(
          `Could not find window with class "${viewportClass}". ` +
          `Is the editor running with --test-host? stderr: ${err}`
        ));
      } else {
        resolve(BigInt(v));
      }
    });
  });
}
