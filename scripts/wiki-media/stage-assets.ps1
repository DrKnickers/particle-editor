# stage-assets.ps1 - build the wiki-media tutorial fixture tree (pipeline spec section 6).
#
# Creates <focMods>\ParticleTutorial\Data\Art\Models, copies the two vanilla source
# effects in as loose overrides, then runs the builder timelines (record runs whose
# frames are discarded; each ends in a confined file/save) to derive the tutorial
# stage files under <focMods>\ParticleTutorial\_stages.
#
# IP guard: every write target must resolve OUTSIDE the repo - vanilla-derived .alo
# files never enter git. Hard-fails otherwise.
#
# Usage:
#   powershell -File scripts\wiki-media\stage-assets.ps1                 # stage everything
#   powershell -File scripts\wiki-media\stage-assets.ps1 -Check         # verify, write nothing
#   powershell -File scripts\wiki-media\stage-assets.ps1 -SkipBuilders  # copies only
param(
    [string]$Config = "$PSScriptRoot\config.local.json",
    [switch]$Check,
    [switch]$SkipBuilders
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Fail([string]$msg) { Write-Host "[stage-assets] FAIL: $msg"; exit 1 }

if (-not (Test-Path $Config)) { Fail "config not found: $Config (copy config.example.json to config.local.json and localize paths)" }
$cfg = Get-Content -Raw $Config | ConvertFrom-Json
foreach ($key in @('focMods', 'extraction', 'exe')) {
    if (-not $cfg.$key) { Fail "config missing '$key'" }
}

$exe = if ([System.IO.Path]::IsPathRooted($cfg.exe)) { $cfg.exe } else { Join-Path $repoRoot $cfg.exe }
$base = Join-Path $cfg.focMods 'ParticleTutorial'
$models = Join-Path $base 'Data\Art\Models'
$stages = Join-Path $base '_stages'

# -- IP guard: no stage target may live inside the repo ---------------------
$baseFull = [System.IO.Path]::GetFullPath($base)
if ($baseFull.ToLowerInvariant().StartsWith($repoRoot.ToLowerInvariant())) {
    Fail "stage base '$baseFull' resolves inside the repo '$repoRoot' - game-derived .alo files must not enter git"
}

# -- Source copies (pristine each run) ---------------------------------------
$sources = @('P_HP_IMPERIAL_DAMAGE.ALO', 'P_SHIELD_BLAST_LARGE00.ALO')
$srcDir = Join-Path $cfg.extraction 'ART\MODELS'

# -- Repo example stages (Tutorial 5): the two hand-authored explosion example
# files ship IN the repo (site/guide/downloads) and are the SOURCE OF TRUTH for
# the Tutorial-5 clips. Unlike the vanilla $sources (copied from the game
# extraction), these come from the repo and are copied verbatim into _stages under
# a clip-facing name. Ordered map: repo-relative source -> staged filename.
$repoExamples = @(
    @{ src = 'site/guide/downloads/P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO'; dst = 't5-example-flipbook.alo' },
    @{ src = 'site/guide/downloads/P_EXPLOSION_EXAMPLE.ALO';          dst = 't5-example.alo' }
)

if ($Check) {
    $bad = 0
    foreach ($name in $sources) {
        $src = Join-Path $srcDir $name
        $dst = Join-Path $models $name
        if (-not (Test-Path $src)) { Write-Host "[stage-assets] CHECK MISS: source $src"; $bad++; continue }
        if (-not (Test-Path $dst)) { Write-Host "[stage-assets] CHECK MISS: staged $dst"; $bad++; continue }
        $a = Get-FileHash $src -Algorithm SHA256
        $b = Get-FileHash $dst -Algorithm SHA256
        if ($a.Hash -ne $b.Hash) { Write-Host "[stage-assets] CHECK DRIFT: $name (staged copy != extraction)"; $bad++ }
    }
    foreach ($ex in $repoExamples) {
        $src = Join-Path $repoRoot $ex.src
        $dst = Join-Path $stages $ex.dst
        if (-not (Test-Path $src)) { Write-Host "[stage-assets] CHECK MISS: repo example $($ex.src)"; $bad++; continue }
        if (-not (Test-Path $dst)) { Write-Host "[stage-assets] CHECK MISS: staged $($ex.dst)"; $bad++; continue }
        $a = Get-FileHash $src -Algorithm SHA256
        $b = Get-FileHash $dst -Algorithm SHA256
        if ($a.Hash -ne $b.Hash) { Write-Host "[stage-assets] CHECK DRIFT: $($ex.dst) (staged copy != repo example)"; $bad++ }
    }
    if (Test-Path $stages) {
        Get-ChildItem $stages -Filter '*.alo' | ForEach-Object { Write-Host "[stage-assets] stage present: $($_.Name)" }
    }
    if ($bad -gt 0) { exit 1 } else { Write-Host '[stage-assets] CHECK OK'; exit 0 }
}

New-Item -ItemType Directory -Force $models | Out-Null
New-Item -ItemType Directory -Force $stages | Out-Null

foreach ($name in $sources) {
    $src = Join-Path $srcDir $name
    if (-not (Test-Path $src)) { Fail "vanilla source missing: $src (point config 'extraction' at your base-game extraction)" }
    Copy-Item $src (Join-Path $models $name) -Force
    Write-Host "[stage-assets] staged copy: $name"
}

foreach ($ex in $repoExamples) {
    $src = Join-Path $repoRoot $ex.src
    if (-not (Test-Path $src)) { Fail "repo example missing: $($ex.src)" }
    Copy-Item $src (Join-Path $stages $ex.dst) -Force
    Write-Host "[stage-assets] staged example: $($ex.dst)"
}

# -- Builder timelines (ordered - later stages consume earlier ones) ---------
if (-not $SkipBuilders) {
    if (-not (Test-Path $exe)) { Fail "exe not found: $exe" }
    $builderDir = Join-Path $repoRoot 'tasks\wiki-media\tutorials\_stage'
    $builders = @(
        'build-t1-green.timeline.json',
        'build-t2-polished.timeline.json',
        'build-t3-core-glow.timeline.json',
        'build-t3-full.timeline.json',
        'build-t4-purple.timeline.json',
        # Tutorial 5 teardown builders. These run AFTER the $repoExamples copy above
        # (each opens one of the staged examples and deletes down to the subset its
        # clip needs), so they must stay after t1-t4 in this ordered list. The old
        # from-scratch build-t5-explosion.timeline.json is RETIRED: it produced a toy
        # soft-dot explosion unrelated to the guide's example files.
        # All open t5-example-flipbook.alo EXCEPT build-t5-fire-layered, which opens the
        # PLAIN t5-example.alo — §4 Option B is the layered fireball, and only the plain
        # example has the Fire + Fire Details pair.
        'build-t5-flash-shockwave.timeline.json',
        'build-t5-smoke-fire.timeline.json',
        'build-t5-fire-only.timeline.json',
        'build-t5-fire-layered.timeline.json',
        'build-t5-single-debris.timeline.json'
    )
    foreach ($b in $builders) {
        $tl = Join-Path $builderDir $b
        if (-not (Test-Path $tl)) { Write-Host "[stage-assets] builder not present yet, skipping: $b"; continue }
        Write-Host "[stage-assets] builder: $b"
        $p = Start-Process -FilePath $exe -ArgumentList '--record', "`"$tl`"" -Wait -PassThru -WorkingDirectory $repoRoot
        if ($p.ExitCode -ne 0) { Fail "builder $b exited $($p.ExitCode)" }
    }
}

Write-Host '[stage-assets] OK'
exit 0
