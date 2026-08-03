# verify-headless-fatal-no-hang.ps1 -- regression guard for the headless-modal
# sweep (headless runs must exit non-zero instead of hanging on a modal).
# ASCII only (PS 5.1 mojibakes non-ASCII).
#
# Proves that a HEADLESS run whose DirectComposition init is FORCED to fail exits
# promptly instead of hanging forever on the fatal MessageBox. With
# ALO_COMP_INIT_FAIL_FIRST=4 (>= kInitAttempts) every DComp create attempt fails,
# so Compositor::Init exhausts the retry and calls FailFatalComposition during
# InitWebView2 -- BEFORE the --drive script runs. With the modal suppressed in
# non-interactive mode the process exits(1) promptly; WITHOUT the fix it would
# block on a MessageBox no user can dismiss (WaitForExit would time out -> Kill).
#
# Run in ISOLATION -- launches the exe and reads the per-PID --drive log. NEVER
# export ALO_COMP_INIT_FAIL_FIRST into a gate/capture shell (spawned hosts inherit
# it). Requires x64\Release\ParticleEditor.exe.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "x64\Release\ParticleEditor.exe"
if (-not (Test-Path $exe)) { Write-Error "exe not found: $exe (build Release)"; exit 2 }

# Minimal --drive script. Never actually run -- the forced fatal fires first --
# but the path must exist for arg parsing.
$script = Join-Path $env:TEMP "headless-fatal-drive.json"
'{ "steps": [] }' | Set-Content -Path $script -Encoding ascii
$logDir = Join-Path $env:LOCALAPPDATA "AloParticleEditor"

Write-Host "[headless-fatal] launching --drive with ALO_COMP_INIT_FAIL_FIRST=4 ..."
$env:ALO_COMP_INIT_FAIL_FIRST = "4"
$exit = $null
try {
    $p = Start-Process -FilePath $exe -ArgumentList "--drive `"$script`"" -PassThru -WindowStyle Normal
    $childPid = $p.Id
    if ($p.WaitForExit(20000)) { $exit = $p.ExitCode }
    else { $p.Kill(); $p.WaitForExit(5000) | Out-Null; $exit = -1 }
} finally {
    Remove-Item Env:\ALO_COMP_INIT_FAIL_FIRST -ErrorAction SilentlyContinue
    Remove-Item $script -ErrorAction SilentlyContinue
}

$fails = 0
# 1. Did NOT hang (the whole point): a bounded exit, not a timeout-Kill.
if ($exit -eq -1) { Write-Host "  FAIL: process hung (>20s) -- modal not suppressed in headless?"; $fails++ }
else { Write-Host "  ok: exited within bound (no modal hang)" }
# 2. Exited non-zero (took the fatal path, not a clean run).
if ($exit -eq 0) { Write-Host "  FAIL: expected non-zero exit, got 0"; $fails++ }
elseif ($exit -ne -1) { Write-Host "  ok: non-zero exit ($exit)" }
# 3. Confirm it actually reached FailFatalComposition — read THIS run's per-PID
#    --drive log exactly (host-drive-<pid>.log), never a stale/concurrent one.
$driveLog = Join-Path $logDir "host-drive-$childPid.log"
if ((Test-Path $driveLog) -and (Select-String -Path $driveLog -Pattern 'FATAL: composition unavailable' -Quiet)) {
    Write-Host "  ok: hit FailFatalComposition (fatal path exercised, modal skipped)"
} else {
    Write-Host "  FAIL: no 'FATAL: composition unavailable' in host-drive-$childPid.log (fatal path not reached?)"
    $fails++
}

if ($fails) { Write-Host "[headless-fatal] FAILED ($fails)"; exit 1 }
Write-Host "[headless-fatal] ALL PASS"
exit 0
