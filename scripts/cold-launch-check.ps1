# cold-launch-check.ps1 -- automated cold-launch gate for the packaged release.
# ASCII only (PS 5.1 mojibakes non-ASCII).
#
# Proves the ONE thing no gate lane covers: the artifact a user downloads -- the
# zip built by scripts/package-release.ps1, extracted somewhere that is not the
# repo, on a profile with NO dev state -- starts, serves its embedded UI, and can
# open and render a real game .alo. This is the release-readiness Phase 2 exit
# criterion (tasks/2026-07-25-v030-cold-launch-procedure.md), automated.
#
# What it does, in order:
#   1. (unless -SkipBuild) builds the web bundle then the Release exe -- web
#      FIRST, because the exe embeds web/apps/editor/dist.
#   2. Packages the zip via scripts/package-release.ps1 (its own asserts run:
#      two-file shape, embedded-bundle sentinel).
#   3. Extracts to a temp dir OUTSIDE the repo whose path contains spaces (the
#      classic quoting-bug surface).
#   4. Moves the editor's per-user state aside (HKCU key exported+deleted,
#      %LOCALAPPDATA% and %TEMP% profile dirs renamed) so the launch is truly
#      cold. EVERYTHING is restored in a finally block, pass or fail.
#   5. Phase A: launches the extracted exe interactively (game path passed on
#      the command line -- see MANUAL-ONLY below) and asserts via host.log:
#      NavigationCompleted success=1 + app/ready (React first paint), and no
#      ERR_NAME_NOT_RESOLVED / FATAL.
#   6. Phase B: runs the extracted exe with --drive: opens a real game .alo,
#      asserts a non-black viewport and a live production-postMessage bridge
#      (bridge-selftest). Exit 0 required.
#
# MANUAL-ONLY surface deliberately NOT covered here (record in the procedure doc
# when checked by hand):
#   - the first-run folder-picker itself (a truly pathless interactive launch
#     shows SHBrowseForFolder; automation cannot answer a native modal, so the
#     game path is supplied via argv -- everything after the picker is covered)
#   - the upgrade path (0.2.0 settings/autosave under the new build)
#   - the missing-WebView2-runtime dialog (needs a machine without the runtime)
#
# Usage:
#   powershell -File scripts/cold-launch-check.ps1                 # full: build + package + check
#   powershell -File scripts/cold-launch-check.ps1 -SkipBuild      # reuse existing build outputs
#   powershell -File scripts/cold-launch-check.ps1 -KeepArtifacts  # keep the zip/extract dirs for inspection

param(
    [string]$GameDataPath = "",
    [string]$AloPath = "",
    [switch]$SkipBuild,
    [switch]$KeepArtifacts,
    [int]$LaunchTimeoutSec = 90
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$script:fails = 0
function Ok($m)   { Write-Host "  ok: $m" }
function Fail($m) { Write-Host "  FAIL: $m" -ForegroundColor Red; $script:fails++ }
function Die($m)  { Write-Host "ERROR: $m" -ForegroundColor Red; exit 2 }

# --- Preflight ---------------------------------------------------------------
# A running editor would share the profile dirs this script moves aside.
if (Get-Process ParticleEditor -ErrorAction SilentlyContinue) {
    Die "ParticleEditor.exe is running -- close it first (this script moves the editor's profile aside)."
}

# Capture the game path BEFORE the state-aside deletes the registry key.
if (-not $GameDataPath) {
    $GameDataPath = (Get-ItemProperty "HKCU:\Software\AloParticleEditor" -ErrorAction SilentlyContinue).GameDataPath
}
if (-not $GameDataPath -or -not (Test-Path -LiteralPath $GameDataPath)) {
    Die "no usable game data path. Pass -GameDataPath <dir> (the FoC 'corruption' folder)."
}
# Strip any trailing backslash BEFORE quoting into an argument list: a path
# ending in \ followed by the closing quote is parsed by CommandLineToArgvW as
# an ESCAPED quote, mangling the argument -- the app then sees no game path and
# (interactively) blocks on the first-run folder picker. Caught by this very
# script's first run. The app re-appends the separator itself.
$GameDataPath = $GameDataPath.TrimEnd('\')

# A real game particle file for Phase B; fall back to the repo fixture.
if (-not $AloPath) {
    $candidate = Join-Path $GameDataPath "Data\Art\Models\P_BOBA_JETPACK.ALO"
    if (Test-Path -LiteralPath $candidate) { $AloPath = $candidate }
    else { $AloPath = Join-Path $root "web\apps\editor\tests\fixtures\a11y-base-state.alo" }
}
if (-not (Test-Path -LiteralPath $AloPath)) { Die "Phase B .alo not found: $AloPath" }

Write-Host "cold-launch-check" -ForegroundColor Cyan
Write-Host "  repo      : $root"
Write-Host "  game path : $GameDataPath"
Write-Host "  .alo      : $AloPath"

# --- 1. Build (web FIRST -- the exe embeds dist) ------------------------------
if (-not $SkipBuild) {
    Write-Host "[build] web bundle"
    Push-Location (Join-Path $root "web")
    try {
        & pnpm --filter ./apps/editor build
        if ($LASTEXITCODE -ne 0) { Die "web build failed ($LASTEXITCODE)" }
    } finally { Pop-Location }

    Write-Host "[build] x64 Release"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = $null
    if (Test-Path $vswhere) {
        $msbuild = (& $vswhere -latest -find "MSBuild\**\Bin\MSBuild.exe" |
            Where-Object { $_.Trim().EndsWith("MSBuild.exe") } | Select-Object -First 1)
    }
    if (-not $msbuild) {
        foreach ($c in @(
            "C:\Program Files (x86)\<path>",
            "C:\Program Files (x86)\<path>"
        )) { if (Test-Path $c) { $msbuild = $c; break } }
    }
    if (-not $msbuild) { Die "MSBuild not found (vswhere + known paths)" }
    $out = & $msbuild (Join-Path $root "ParticleEditor.sln") /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo 2>&1
    # The arrow line is the proof the exe actually relinked -- MSBuild under a
    # mangled invocation can no-op and still exit 0 (repo gotcha).
    $arrow = $out | Select-String "-> .*ParticleEditor\.exe"
    if ($LASTEXITCODE -ne 0 -or -not $arrow) {
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
        Die "Release build failed (exit $LASTEXITCODE, arrow line present: $([bool]$arrow))"
    }
}

# --- 2. Package ---------------------------------------------------------------
$work  = Join-Path $env:TEMP "pe-coldlaunch-$PID"
if (Test-Path $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$zip   = Join-Path $work "ParticleEditor-cold.zip"
$stage = Join-Path $work "stage"
Write-Host "[package] $zip"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "scripts\package-release.ps1") `
    -RepoRoot $root -Stage $stage -OutZip $zip
if ($LASTEXITCODE -ne 0) { Die "package-release.ps1 failed ($LASTEXITCODE)" }

# --- 3. Extract to a spaced path outside the repo -----------------------------
$extract = Join-Path $env:TEMP "PE cold launch $PID"
if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
$coldExe = Join-Path $extract "x64\Release\ParticleEditor.exe"
if (-not (Test-Path -LiteralPath $coldExe)) { Die "extracted exe missing: $coldExe" }
$fileCount = @(Get-ChildItem -LiteralPath $extract -Recurse -File).Count
if ($fileCount -ne 2) { Fail "extracted tree has $fileCount files, expected exactly 2 (exe + d3dx9)" }
else { Ok "extracted 2-file tree at a spaced path" }

# --- 4. State aside + phases, with guaranteed restore -------------------------
$aloKeyPs = "HKCU:\Software\AloParticleEditor"
$aloKeyCmd = "HKCU\Software\AloParticleEditor"
$regBak = Join-Path $work "alo-settings-backup.reg"
$laDir  = Join-Path $env:LOCALAPPDATA "AloParticleEditor"
$laBak  = "$laDir.coldbak$PID"
$tmpDir = Join-Path $env:TEMP "AloParticleEditor"
$tmpBak = "$tmpDir.coldbak$PID"
$keyExisted = Test-Path $aloKeyPs
$procA = $null
$procB = $null

try {
    if ($keyExisted) {
        & reg.exe export $aloKeyCmd $regBak /y | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $regBak)) { Die "registry backup failed -- aborting before touching state" }
        & reg.exe delete $aloKeyCmd /f | Out-Null
    }
    if (Test-Path -LiteralPath $laDir)  { Move-Item -LiteralPath $laDir  $laBak  -Force }
    if (Test-Path -LiteralPath $tmpDir) { Move-Item -LiteralPath $tmpDir $tmpBak -Force }
    Ok "profile state moved aside (registry exported, dirs renamed)"

    # --- Phase A: interactive cold launch ------------------------------------
    Write-Host "[phase A] interactive cold launch"
    $hostLog = Join-Path $laDir "host.log"
    $procA = Start-Process -FilePath $coldExe -ArgumentList ('"' + $GameDataPath + '"') `
        -WorkingDirectory $extract -PassThru
    $deadline = (Get-Date).AddSeconds($LaunchTimeoutSec)
    $navOk = $false; $ready = $false; $fatal = $null
    while ((Get-Date) -lt $deadline) {
        if ($procA.HasExited) { $fatal = "process exited early (code $($procA.ExitCode))"; break }
        $txt = ""
        if (Test-Path -LiteralPath $hostLog) {
            # NOT [IO.File]::ReadAllText: it opens with FileShare.Read, which
            # conflicts with the app's live write handle (host.log is open
            # _SH_DENYNO) and throws a sharing violation on every poll.
            # Get-Content opens with ReadWrite sharing and reads fine.
            try { $txt = (Get-Content -LiteralPath $hostLog -Raw -ErrorAction Stop) } catch { $txt = "" }
        }
        if ($txt -match "ERR_NAME_NOT_RESOLVED") { $fatal = "ERR_NAME_NOT_RESOLVED in host.log"; break }
        if ($txt -match "FATAL")                 { $fatal = "FATAL in host.log"; break }
        if ($txt -match "NavigationCompleted \(success=1") { $navOk = $true }
        if ($txt -match "app/ready received")             { $ready = $true }
        if ($navOk -and $ready) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($fatal) { Fail "phase A: $fatal" }
    elseif (-not ($navOk -and $ready)) {
        Fail "phase A: timeout after ${LaunchTimeoutSec}s (NavigationCompleted=$navOk app/ready=$ready)"
        if (Test-Path -LiteralPath $hostLog) {
            # Full log to the (auto-kept) work dir; PERF spam floods a tail view.
            Copy-Item -LiteralPath $hostLog (Join-Path $work "phaseA-host.log") -Force
            Write-Host "    full log: $(Join-Path $work 'phaseA-host.log')"
            Get-Content -LiteralPath $hostLog |
                Where-Object { $_ -notmatch "^\[PERF|^\[COMP-engine" } |
                Select-Object -First 40 | ForEach-Object { Write-Host "    $_" }
        }
    } else { Ok "phase A: embedded UI served (NavigationCompleted success=1, React first paint)" }
    if ($procA -and -not $procA.HasExited) {
        $procA.CloseMainWindow() | Out-Null
        if (-not $procA.WaitForExit(10000)) { $procA.Kill(); $procA.WaitForExit(5000) | Out-Null }
    }

    # --- Phase B: --drive opens a real game .alo ------------------------------
    Write-Host "[phase B] --drive real-.alo open + render + bridge"
    $driveJson = Join-Path $work "cold-drive.json"
    # Set a bright background before the non-black probe: a COLD profile's
    # defaults (no persisted skydome/ground) can legitimately render a dim
    # scene, so probing the effect alone is scene-dependent. Flooding the
    # background first makes non-black prove the render+composite pipeline
    # (drive-smoke's baseline does the same); file/open + bridge-selftest
    # prove the .alo loaded and the production bridge is live.
    @{ steps = @(
        @{ kind = "file/open"; params = @{ path = $AloPath } },
        @{ settle = 800 },
        @{ kind = "engine/set/background"; params = @{ rgb = 3829413 } },
        @{ settle = 400 },
        @{ "assert-viewport-nonblack" = @{} },
        @{ "bridge-selftest" = @{ kind = "emitters/list" } }
    ) } | ConvertTo-Json -Depth 6 | Set-Content -Path $driveJson -Encoding ASCII
    $procB = Start-Process -FilePath $coldExe `
        -ArgumentList ('--drive "' + $driveJson + '" "' + $GameDataPath + '"') `
        -WorkingDirectory $extract -PassThru
    if (-not $procB.WaitForExit(120000)) {
        $procB.Kill(); $procB.WaitForExit(5000) | Out-Null
        Fail "phase B: --drive timed out"
    } elseif ($procB.ExitCode -ne 0) {
        Fail "phase B: --drive exit $($procB.ExitCode)"
        $dl = Join-Path $laDir ("host-drive-" + $procB.Id + ".log")
        if (Test-Path -LiteralPath $dl) {
            Copy-Item -LiteralPath $dl (Join-Path $work "phaseB-drive.log") -Force
            Write-Host "    full log: $(Join-Path $work 'phaseB-drive.log')"
            Get-Content -LiteralPath $dl |
                Where-Object { $_ -notmatch "^\[PERF|^\[COMP-engine" } |
                Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        }
    } else { Ok "phase B: real .alo opened, viewport non-black, bridge live (exit 0)" }
}
finally {
    # Restore the machine no matter what happened above. NOTHING in this block
    # may terminate it early: under EAP=Stop, PS 5.1 turns a native command's
    # stderr line (reg.exe writes status there) into a TERMINATING error, which
    # on this script's first run aborted the restore mid-way and stranded the
    # user's real profile in the .coldbak dirs. Hence: EAP=Continue for the
    # whole block, no stderr redirection on natives, and per-step try/catch.
    $ErrorActionPreference = "Continue"
    $restoreFailed = $false
    function Restore-Step([string]$what, [scriptblock]$action) {
        try { & $action; Write-Host "  restore: $what" }
        catch { Write-Host "  RESTORE FAILED: $what -- $($_.Exception.Message)" -ForegroundColor Red; $script:restoreFailed = $true }
    }
    foreach ($p in @($procA, $procB)) {
        if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {} }
    }
    Start-Sleep -Milliseconds 500
    # Drop state the cold run created, then put the originals back.
    Restore-Step "cold-run LOCALAPPDATA profile removed" {
        if (Test-Path -LiteralPath $laDir) { Remove-Item -LiteralPath $laDir -Recurse -Force -Confirm:$false -ErrorAction Stop } }
    Restore-Step "cold-run TEMP autosave dir removed" {
        if (Test-Path -LiteralPath $tmpDir) { Remove-Item -LiteralPath $tmpDir -Recurse -Force -Confirm:$false -ErrorAction Stop } }
    Restore-Step "cold-run registry key removed" {
        if (Test-Path $aloKeyPs) { & reg.exe delete $aloKeyCmd /f | Out-Null } }
    if ($keyExisted) {
        Restore-Step "registry settings imported from backup" {
            if (-not (Test-Path $regBak)) { throw "backup file missing: $regBak" }
            & reg.exe import $regBak | Out-Null
            if (-not (Test-Path $aloKeyPs)) { throw "key absent after import" } }
    }
    Restore-Step "LOCALAPPDATA profile moved back" {
        if (Test-Path -LiteralPath $laBak) { Move-Item -LiteralPath $laBak $laDir -Force } }
    Restore-Step "TEMP autosave dir moved back" {
        if (Test-Path -LiteralPath $tmpBak) { Move-Item -LiteralPath $tmpBak $tmpDir -Force } }
    if ($restoreFailed) {
        Write-Host "STATE RESTORE INCOMPLETE -- backups preserved: $regBak ; $laBak ; $tmpBak" -ForegroundColor Red
        $script:fails++
    } elseif ($script:fails -gt 0) {
        # Keep evidence on any failure -- the copied logs live in $work.
        Write-Host "failure artifacts kept for diagnosis: $work ; $extract"
    } elseif (-not $KeepArtifacts) {
        try { Remove-Item -LiteralPath $extract -Recurse -Force -Confirm:$false -ErrorAction Stop } catch {}
        try { Remove-Item -LiteralPath $work    -Recurse -Force -Confirm:$false -ErrorAction Stop } catch {}
    } else {
        Write-Host "artifacts kept: $work ; $extract"
    }
}

# --- Summary ------------------------------------------------------------------
if ($script:fails -eq 0) {
    Write-Host "COLD LAUNCH: PASS (packaged zip, clean profile, spaced path)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "COLD LAUNCH: $script:fails FAILURE(S)" -ForegroundColor Red
    exit 1
}
