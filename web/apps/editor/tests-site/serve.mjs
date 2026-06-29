// Minimal dependency-free static server for the landing smoke. Serves the repo-root
// site/ dir (resolved relative to this file) on PORT. Node built-ins only (node:http/fs/path/url).
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, relative, resolve, isAbsolute } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(fileURLToPath(new URL("../../../../site", import.meta.url)));
const PORT = Number(process.env.PORT || 5175);
const TYPES = {
  ".html": "text/html", ".css": "text/css", ".js": "text/javascript",
  ".svg": "image/svg+xml", ".json": "application/json",
  ".mp4": "video/mp4", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
  ".png": "image/png", ".webp": "image/webp", ".gif": "image/gif",
};

createServer(async (req, res) => {
  try {
    let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (p === "/") p = "/index.html";
    const full = join(ROOT, p);
    const rel = relative(ROOT, full);
    if (rel.startsWith("..") || isAbsolute(rel)) { res.writeHead(403).end(); return; }
    const body = await readFile(full);
    res.writeHead(200, { "content-type": TYPES[extname(full)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404).end("not found");
  }
}).listen(PORT, () => console.log(`serving ${ROOT} on ${PORT}`));
