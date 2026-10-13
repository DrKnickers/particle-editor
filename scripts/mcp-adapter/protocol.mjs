// [mcp-adapter] The MCP server side of JSON-RPC 2.0: initialize, ping,
// tools/list, tools/call and cancellation. Transport-free, so the tests drive
// it with plain objects; server.mjs carries it over stdio.
export const PROTOCOL_VERSIONS = ["2025-06-18", "2025-03-26", "2024-11-05"];
export const SERVER_INFO = { name: "particle-editor", title: "Particle Editor", version: "1.0.0" };

const error = (id, code, message) => ({ jsonrpc: "2.0", id, error: { code, message } });
const reply = (id, result) => ({ jsonrpc: "2.0", id, result });

// Tool calls run one at a time: observe drives the GPU, and none of them is
// meant to race another over the same files.
export function createProtocol(tools, { log = () => {} } = {}) {
  const inFlight = new Map();
  let queue = Promise.resolve();

  async function handleOne(message) {
    const isObject = message !== null && typeof message === "object" && !Array.isArray(message);
    // A response to a request: the server sends none, so there is nothing to match.
    if (isObject && !("method" in message) && ("result" in message || "error" in message)) return undefined;
    if (!isObject || message.jsonrpc !== "2.0" || typeof message.method !== "string")
      return error(isObject ? message.id ?? null : null, -32600, "invalid request");
    const { id, method, params } = message;
    const notification = !("id" in message);
    if (notification) {
      if (method === "notifications/cancelled") inFlight.get(params?.requestId)?.abort();
      return undefined;
    }
    switch (method) {
      case "initialize": {
        const asked = params?.protocolVersion;
        return reply(id, { protocolVersion: PROTOCOL_VERSIONS.includes(asked) ? asked : PROTOCOL_VERSIONS[0],
          capabilities: { tools: { listChanged: false } }, serverInfo: SERVER_INFO,
          instructions: "Particle Editor's window-free tools over one task root. Paths are relative to that root; " +
            "patch writes a candidate and never the source. Start with particle_inspect to get sourceDocumentHash." });
      }
      case "ping": return reply(id, {});
      case "tools/list": return reply(id, { tools: tools.definitions });
      case "tools/call": {
        if (typeof params?.name !== "string") return error(id, -32602, "tools/call needs a tool name");
        if (!tools.definitions.some((d) => d.name === params.name)) return error(id, -32602, `unknown tool: ${params.name}`);
        const controller = new AbortController();
        inFlight.set(id, controller);
        const run = queue.then(() => controller.signal.aborted ? null
          : tools.call(params.name, params.arguments ?? {}, controller.signal));
        queue = run.catch(() => {});
        try {
          const outcome = await run;
          // A cancelled request gets no response (MCP cancellation).
          return controller.signal.aborted ? undefined : reply(id, outcome);
        } catch (e) {
          log(`${params.name} failed: ${e.stack ?? e.message}`);
          return error(id, -32603, `internal error: ${e.message}`);
        } finally {
          inFlight.delete(id);
        }
      }
      default: return error(id, -32601, `method not found: ${method}`);
    }
  }

  // One message or a batch (2025-03-26 allows batches); undefined when
  // nothing is owed (notifications only).
  async function handle(message) {
    if (!Array.isArray(message)) return handleOne(message);
    if (!message.length) return error(null, -32600, "empty batch");
    const replies = (await Promise.all(message.map(handleOne))).filter(Boolean);
    return replies.length ? replies : undefined;
  }
  // The client went away (stdin closed): stop every call, running or queued.
  handle.abortAll = () => { for (const controller of inFlight.values()) controller.abort(); };
  return handle;
}
