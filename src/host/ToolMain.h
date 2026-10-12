#ifndef TOOL_MAIN_H
#define TOOL_MAIN_H

// `ParticleEditor.exe --tool <verb> ...`: the window-free authoring tool
// (tasks/2026-10-07-phase4b-tool-verbs-plan.md). It runs before any window,
// D3D, WebView2 or game-data work and returns the process exit code.
//
//   --tool inspect  <in.alo> --out <inspect.json>
//   --tool validate <in.alo> --out <report.json>
//   --tool patch    <in.alo> <patch.json> --out <candidate.alo> --report <report.json>
//   --tool reorder  <in.alo> <reorder.json> --out <candidate.alo> --report <report.json>
//   --tool index    --out <records.json> [--layer <dir>]... <game-root>...
//
// `validate` always writes its report. It exits 0 when the source loads
// exactly as written (the report also says whether a save reproduces its
// bytes, and lists warnings and advisories), and 3 when it does not: a load
// failure, a spawn link the loader cleared, or a value inspect cannot report.
//
// `patch` (ToolPatch.h) writes its report for every outcome once its four
// paths are distinct; --out is replaced only by a verified candidate. `reorder`
// (ToolReorder.h) takes the same four paths and moves one root emitter; it
// authors the defect benchmark's draw-order cases.
//
// `index` (ToolIndex.h) lists every particle system the content stack reaches
// (the game roots, as --observe takes them, under the mod layers, highest
// first) and every texture its emitters name, resolved and hashed as the
// editor resolves them: the records the asset index is built from.
//
// Results go to files, never stdout (the exe is a Windows-subsystem program,
// and the loader prints diagnostics to stdout); errors go to stderr.
// Exit codes, shared by every verb:
//   0  ok                2  usage, or an output path that names an input
//   3  the source is invalid: it does not load, holds a non-finite float or
//      a string that is not UTF-8 (patch: or the loader repaired it)
//   4  selector refusal     5  rule refusal     6  postcondition failure
//   13 an output could not be written      14 internal error

namespace host
{
// True when the command line asks for the tool (any argument is `--tool`).
bool IsToolCommandLine();

// Runs the tool and returns the exit code.
int RunToolMain();
}

#endif // TOOL_MAIN_H
