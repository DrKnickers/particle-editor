// window-title.test.ts — title format contract for the Win32 titlebar
// mirror (spec §3.2). Four cases: clean/dirty × named/untitled, plus
// basename handling for both path separator styles.
import { describe, expect, test } from "vitest";
import { formatWindowTitle } from "../window-title";

describe("formatWindowTitle", () => {
  test("clean + named: basename — app name", () => {
    expect(formatWindowTitle("C:\\Mods\\fx\\plasma_blast.alo", false)).toBe(
      "plasma_blast.alo — Particle Editor",
    );
  });

  test("dirty + named: leading ● before basename", () => {
    expect(formatWindowTitle("C:\\Mods\\fx\\plasma_blast.alo", true)).toBe(
      "● plasma_blast.alo — Particle Editor",
    );
  });

  test("clean + untitled: Untitled.alo placeholder", () => {
    expect(formatWindowTitle(null, false)).toBe(
      "Untitled.alo — Particle Editor",
    );
  });

  test("dirty + untitled: ● Untitled.alo", () => {
    expect(formatWindowTitle(null, true)).toBe(
      "● Untitled.alo — Particle Editor",
    );
  });

  test("forward-slash paths split correctly", () => {
    expect(formatWindowTitle("C:/Temp/title-test.alo", false)).toBe(
      "title-test.alo — Particle Editor",
    );
  });

  test("bare filename (no separator) passes through", () => {
    expect(formatWindowTitle("loose.alo", false)).toBe(
      "loose.alo — Particle Editor",
    );
  });
});
