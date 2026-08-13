/** Path display and comparison helpers for host-supplied Windows-style paths. */

type BasenameOptions = {
  /** Ignore repeated leading/trailing separators when choosing the final segment. */
  normalizeSeparators?: boolean;
};

/** Return a path's final segment, accepting either slash style without changing case. */
export function basename(path: string, options: BasenameOptions = {}): string {
  if (options.normalizeSeparators) {
    const parts = path.split(/[\\/]+/).filter((part) => part.length > 0);
    return parts.length > 0 ? parts[parts.length - 1]! : path;
  }
  const index = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return index >= 0 ? path.slice(index + 1) : path;
}

/** Compare canonical Windows paths case-insensitively, ignoring trailing slashes. */
export function eqPath(a: string, b: string): boolean {
  return a.replace(/[\\/]+$/, "").toLowerCase() === b.replace(/[\\/]+$/, "").toLowerCase();
}
