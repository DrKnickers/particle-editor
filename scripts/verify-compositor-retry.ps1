# verify-compositor-retry.ps1 -- fault-injection regression check for the
# Compositor::Init DirectComposition retry (tasks/2026-07-07-compositor-init-retry-plan.md).
# ASCII only (PS 5.1 mojibakes non-ASCII).
#
# Proves the retry LOOP MECHANICS on any box: with ALO_COMP_INIT_FAIL_FIRST=2 the
# first two DCompositionCreateDevice2 attempts are forced to fail with the real
# CRD HRESULT (0x8007139F), then the third attempt does the REAL create, which
# succeeds -- so the editor recovers instead of going fatal on the first failure.
#
# NOTE: this proves the loop + logging + recovery-after-transient. It does NOT
# prove a REAL 0x8007139F clears on retry (that needs the failure to actually
# manifest under CRD; efficacy is unverified -- the retry ships as a mitigation).
#
# Run in ISOLATION (a normal launch writes canonical host.log; don't run
# concurrently with the gate's other exe-launching lanes). The env var is set
# only for the child launched here -- NEVER export it into a gate/capture shell,
# whose environment is inherited by spawned hosts.
#
# Requires: x64\Release\ParticleEditor.exe (Compositor::Init runs during WebView2
# env setup, before app-content load, so a web bundle is not required).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "x64\Release\ParticleEditor.exe"
if (-not (Test-Path $exe)) { Write-Error "exe not found: $exe (build Release)"; exit 2 }
$log = Join-Path $env:LOCALAPPDATA "AloParticleEditor\host.log"

Write-Host "[compositor-retry] launching with ALO_COMP_INIT_FAIL_FIRST=2 ..."
$env:ALO_COMP_INIT_FAIL_FIRST = "2"
try {
    $p = Start-Process $exe -PassThru
    Start-Sleep -Seconds 10
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:\ALO_COMP_INIT_FAIL_FIRST -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 1

if (-not (Test-Path $log)) { Write-Host "  FAIL: host.log not found at $log"; exit 1 }
# host.log is truncated per launch, so its current contents are this run's lines.
$lines = Get-Content $log

# Expected: fault-injection banner, two injected failures, then a real recovery.
$checks = @(
    @{ pat = 'fault-injection active: fail first 2';                          desc = 'fault-injection activated' },
    @{ pat = 'COMP-fail\] DCompositionCreateDevice2 hr=0x8007139F \(attempt 1/4\)'; desc = 'attempt 1 failed 0x8007139F' },
    @{ pat = 'COMP-fail\] DCompositionCreateDevice2 hr=0x8007139F \(attempt 2/4\)'; desc = 'attempt 2 failed 0x8007139F' },
    @{ pat = 'COMP-init\] DComp V1 device created \(attempt 3/4\)';           desc = 'attempt 3 recovered (real create succeeded)' }
)
$fails = 0
foreach ($c in $checks) {
    if ($lines | Select-String -Pattern $c.pat -Quiet) {
        Write-Host "  ok: $($c.desc)"
    } else {
        Write-Host "  FAIL: expected in host.log -> $($c.desc)  [/$($c.pat)/]"
        $fails++
    }
}
# Guard: the app must NOT have gone fatal (retry recovered before the fallback).
if ($lines | Select-String -Pattern 'FATAL: composition unavailable' -Quiet) {
    Write-Host "  FAIL: FATAL composition path was hit despite a recoverable attempt"
    $fails++
}

if ($fails -eq 0) { Write-Host "[compositor-retry] ALL PASS"; exit 0 }
Write-Host "[compositor-retry] $fails check(s) FAILED"; exit 1
