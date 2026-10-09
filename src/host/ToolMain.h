#ifndef TOOL_MAIN_H
#define TOOL_MAIN_H

// `ParticleEditor.exe --tool <verb> ...`: the window-free authoring tool
// (tasks/2026-10-07-phase4b-tool-verbs-plan.md). It runs before any window,
// D3D, WebView2 or game-data work and returns the process exit code.
//
//   --tool inspect  <in.alo> --out <inspect.json>
//   --tool validate <in.alo> --out <report.json>
//
// `validate` always writes its report. It exits 0 when the source loads
// exactly as written (the report also says whether a save reproduces its
// bytes, and lists warnings and advisories), and 3 when it does not: a load
// failure, a spawn link the loader cleared, or a value inspect cannot report.
//
// Results go to files, never stdout (the exe is a Windows-subsystem program,
// and the loader prints diagnostics to stdout); errors go to stderr.
// Exit codes, shared by every verb:
//   0  ok                2  usage, or an output path that names an input
//   3  the source is invalid: it does not load, holds a non-finite float or
//      a string that is not UTF-8
//   13 an output could not be written      14 internal error

namespace host
{
// True when the command line asks for the tool (any argument is `--tool`).
bool IsToolCommandLine();

// Runs the tool and returns the exit code.
int RunToolMain();
}

#endif // TOOL_MAIN_H
