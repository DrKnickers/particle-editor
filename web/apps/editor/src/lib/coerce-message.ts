/** Coerce a host message (object or JSON string) to a record, or null. */
export function coerceMessage(data: unknown): Record<string, unknown> | null {
  if (typeof data === "string") {
    try {
      return JSON.parse(data) as Record<string, unknown>;
    } catch {
      return null;
    }
  }
  return data && typeof data === "object" ? (data as Record<string, unknown>) : null;
}
