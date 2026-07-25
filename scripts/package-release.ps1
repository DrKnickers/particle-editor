<#
.SYNOPSIS
    Stage (and optionally zip) the Particle Editor release layout, self-asserting completeness.

.DESCRIPTION
    Assembles the exact directory tree the host launches from and FAILS LOUDLY if any required
    runtime file is missing -- so a release archive can never be produced with a broken/incomplete
    bundle.

    WHY THIS LAYOUT IS LOAD-BEARING:
    The host computes the web-bundle path at runtime by walking UP three parent directories from the
    running .exe, then descending into web/apps/editor/dist (HostWindow.cpp ComputeEditorDistPath):

        auto root = p.parent_path().parent_path().parent_path();
        return root / "web" / "apps" / "editor" / "dist";

    So from  <STAGE>/x64/Release/ParticleEditor.exe  the three hops land back on <STAGE>, and the
    bundle must live at  <STAGE>/web/apps/editor/dist . Flatten the exe or move the bundle and the
    WebView shows ERR_NAME_NOT_RESOLVED for app.local at launch.

    Authoritative staged tree:

        <STAGE>/
          x64/Release/
            ParticleEditor.exe
            WebView2Loader.dll     # WebView2 runtime loader (dynamic dependency)
            d3dx9_43.dll           # x64 D3DX9 runtime, vendored at libs/redist/
          web/apps/editor/dist/
            index.html
            assets/                # built JS/CSS (must be non-empty)
            fonts/

    d3dx9_43.dll is vendored at  libs/redist/d3dx9_43.dll  (x64) and copied from there, so packaging
    is deterministic on any machine or CI runner (a per-machine OS copy is absent on a clean install).

    CROSS-PLATFORM: this script is written to run under Windows PowerShell 5.1 AND PowerShell 7
    (pwsh) on Linux, so the node --test packaging smoke test can exercise it on the CI runner. Keep
    all paths built via [System.IO.Path]::Combine / Join-Path -- no embedded backslash path literals.

.PARAMETER Stage
    Destination directory for the staged tree. Defaults to ./release-stage. CLEARED and rebuilt each
    run so stale files from a prior run never survive into the archive.

.PARAMETER RepoRoot
    Repository root to copy build artifacts from. Defaults to the parent of this script's directory.

.PARAMETER OutZip
    Optional. When set, after staging the script creates this zip from the staged tree AND asserts the
    zip's entries include every required artifact -- the final "inspect the archive" gate.

.EXAMPLE
    pwsh scripts/package-release.ps1
.EXAMPLE
    powershell -File scripts/package-release.ps1 -OutZip ParticleEditor-v0.3.0.zip
#>

[CmdletBinding()]
param(
    [string] $Stage    = (Join-Path (Get-Location) 'release-stage'),
    [string] $RepoRoot = '',
    [string] $OutZip
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $scriptRoot = $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($scriptRoot) -and $MyInvocation.MyCommand.Path) {
        $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    if ([string]::IsNullOrWhiteSpace($scriptRoot)) {
        throw "[package-release] Could not resolve script directory; pass -RepoRoot explicitly."
    }
    $RepoRoot = Split-Path -Parent $scriptRoot
}

# Normalize to absolute paths and REFUSE a destructive -Stage (the stage is Remove-Item'd below):
# never the filesystem root, the repo root, or an ancestor of the repo.
$Stage    = [System.IO.Path]::GetFullPath($Stage)
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
$sep       = [System.IO.Path]::DirectorySeparatorChar
$stageNorm = $Stage.TrimEnd([char]'\', [char]'/')
$repoNorm  = $RepoRoot.TrimEnd([char]'\', [char]'/')
if ([string]::IsNullOrWhiteSpace([System.IO.Path]::GetFileName($stageNorm)) -or
    $stageNorm -eq $repoNorm -or
    $repoNorm.StartsWith($stageNorm + $sep, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "[package-release] Refusing -Stage '$Stage' (filesystem root, the repo root, or an ancestor of it). Use a dedicated staging directory."
}

function Test-RequiredSource {
    param([string]$Label, [string]$Path, [ValidateSet('Leaf','Container')][string]$Kind)
    if (-not (Test-Path -LiteralPath $Path -PathType $Kind)) {
        throw "[package-release] Missing required source: $Label -> $Path"
    }
}
function Assert-Staged {
    param([string]$Relative)
    $p = [System.IO.Path]::Combine($Stage, $Relative)
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) {
        throw "[package-release] Missing staged artifact: $Relative"
    }
    if ((Get-Item -LiteralPath $p).Length -le 0) {
        throw "[package-release] Empty staged artifact: $Relative"
    }
}
function Test-PathIsOrContains {
    param([string]$Parent, [string]$Child)
    $parentNorm = [System.IO.Path]::GetFullPath($Parent).TrimEnd([char]'\', [char]'/')
    $childNorm  = [System.IO.Path]::GetFullPath($Child).TrimEnd([char]'\', [char]'/')
    return ($childNorm -eq $parentNorm -or
        $childNorm.StartsWith($parentNorm + $sep, [System.StringComparison]::OrdinalIgnoreCase))
}
function Test-PathsOverlap {
    param([string]$A, [string]$B)
    return ((Test-PathIsOrContains $A $B) -or (Test-PathIsOrContains $B $A))
}

Write-Host "Particle Editor -- release staging" -ForegroundColor Cyan
Write-Host "  RepoRoot : $RepoRoot"
Write-Host "  Stage    : $Stage"
if ($OutZip) { Write-Host "  OutZip   : $OutZip" }
Write-Host ""

# --- Resolve + validate source artifacts -------------------------------------
$exeSource    = [System.IO.Path]::Combine($RepoRoot, 'x64', 'Release', 'ParticleEditor.exe')
$loaderSource = [System.IO.Path]::Combine($RepoRoot, 'x64', 'Release', 'WebView2Loader.dll')
$d3dxSource   = [System.IO.Path]::Combine($RepoRoot, 'libs', 'redist', 'd3dx9_43.dll')
$wv2Source    = [System.IO.Path]::Combine($RepoRoot, 'libs', 'redist', 'MicrosoftEdgeWebview2Setup.exe')
$distSource   = [System.IO.Path]::Combine($RepoRoot, 'web', 'apps', 'editor', 'dist')
$distIndex    = [System.IO.Path]::Combine($distSource, 'index.html')
$distAssets   = [System.IO.Path]::Combine($distSource, 'assets')
$sourceRoots  = @(
    [pscustomobject]@{ Label = 'native release output'; Path = [System.IO.Path]::Combine($RepoRoot, 'x64', 'Release') }
    [pscustomobject]@{ Label = 'vendored D3DX redist'; Path = [System.IO.Path]::Combine($RepoRoot, 'libs', 'redist') }
    [pscustomobject]@{ Label = 'web bundle dist'; Path = $distSource }
)

foreach ($sourceRoot in $sourceRoots) {
    if (Test-PathsOverlap $Stage $sourceRoot.Path) {
        throw "[package-release] Refusing -Stage '$Stage' because it overlaps required source: $($sourceRoot.Label) -> $($sourceRoot.Path). Use a dedicated staging directory."
    }
}

Test-RequiredSource 'ParticleEditor.exe'      $exeSource    'Leaf'
Test-RequiredSource 'WebView2Loader.dll'      $loaderSource 'Leaf'
Test-RequiredSource 'd3dx9_43.dll (vendored)' $d3dxSource   'Leaf'
# The WebView2 LOADER beside the exe is not the RUNTIME. The runtime is a machine
# component: present on Windows 11 and on Windows 10 with Edge, absent on stripped
# or LTSC images. Without it the editor starts and immediately tells the user to go
# install something else -- the same dead end the vendored d3dx9_43.dll exists to
# avoid. Ship Microsoft's ~2 MB bootstrapper so the editor can offer to install it
# (see the WebView2 failure path in src/host/HostWindow.cpp), and treat it as
# REQUIRED so a release can never quietly ship without it.
Test-RequiredSource 'MicrosoftEdgeWebview2Setup.exe (vendored)' $wv2Source 'Leaf'
Test-RequiredSource 'web bundle (dist)'       $distSource   'Container'
Test-RequiredSource 'web bundle index.html'   $distIndex    'Leaf'
Test-RequiredSource 'web bundle assets/'      $distAssets   'Container'

# --- Clear + prepare the staged tree -----------------------------------------
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
$stageExeDir  = [System.IO.Path]::Combine($Stage, 'x64', 'Release')
$stageDistDir = [System.IO.Path]::Combine($Stage, 'web', 'apps', 'editor', 'dist')
New-Item -ItemType Directory -Force -Path $stageExeDir  | Out-Null
New-Item -ItemType Directory -Force -Path $stageDistDir | Out-Null

# --- Copy native artifacts ----------------------------------------------------
Copy-Item -LiteralPath $exeSource    -Destination $stageExeDir -Force
Copy-Item -LiteralPath $loaderSource -Destination $stageExeDir -Force
Copy-Item -LiteralPath $d3dxSource   -Destination $stageExeDir -Force
Copy-Item -LiteralPath $wv2Source    -Destination $stageExeDir -Force
Write-Host "  [ok] ParticleEditor.exe, WebView2Loader.dll, d3dx9_43.dll -> $stageExeDir"

# --- Copy the web bundle CONTENTS (index.html, assets/, fonts/) ---------------
Get-ChildItem -LiteralPath $distSource -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stageDistDir -Recurse -Force
}
Write-Host "  [ok] web bundle -> $stageDistDir"

# --- Post-stage assertions (fail loud) ---------------------------------------
Assert-Staged ([System.IO.Path]::Combine('x64', 'Release', 'ParticleEditor.exe'))
Assert-Staged ([System.IO.Path]::Combine('x64', 'Release', 'WebView2Loader.dll'))
Assert-Staged ([System.IO.Path]::Combine('x64', 'Release', 'd3dx9_43.dll'))
Assert-Staged ([System.IO.Path]::Combine('web', 'apps', 'editor', 'dist', 'index.html'))
$stagedAssets = [System.IO.Path]::Combine($stageDistDir, 'assets')
if (-not (Test-Path -LiteralPath $stagedAssets -PathType Container) -or
    -not (Get-ChildItem -LiteralPath $stagedAssets -Recurse -File)) {
    throw "[package-release] Staged web bundle has no assets: $stagedAssets"
}
$stagedFonts = [System.IO.Path]::Combine($stageDistDir, 'fonts')
if (-not (Test-Path -LiteralPath $stagedFonts -PathType Container) -or
    -not (Get-ChildItem -LiteralPath $stagedFonts -Recurse -File)) {
    throw "[package-release] Staged web bundle has no fonts: $stagedFonts"
}
Write-Host "  [ok] staged tree verified"

# --- Optional: create + verify the release zip -------------------------------
if ($OutZip) {
    if (Test-Path -LiteralPath $OutZip) { Remove-Item -LiteralPath $OutZip -Force }
    Compress-Archive -Path ([System.IO.Path]::Combine($Stage, '*')) -DestinationPath $OutZip -Force

    # Load the zip-reader type only if it isn't already available (PS 5.1 needs the assembly;
    # pwsh 7 has it loaded). A genuine load failure must surface, not be swallowed.
    if (-not ('System.IO.Compression.ZipFile' -as [type])) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
    }
    $zip = [System.IO.Compression.ZipFile]::OpenRead($OutZip)
    try {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
    } finally {
        $zip.Dispose()
    }
    # Only FILE entries count — a bare directory entry (ends in '/') must not satisfy a check.
    $fileEntries = @($entries | Where-Object { -not $_.EndsWith('/') })

    $requiredEntries = @(
        'x64/Release/ParticleEditor.exe'
        'x64/Release/WebView2Loader.dll'
        'x64/Release/d3dx9_43.dll'
        'web/apps/editor/dist/index.html'
    )
    foreach ($r in $requiredEntries) {
        if ($fileEntries -notcontains $r) { throw "[package-release] Missing zip entry: $r" }
    }
    if (-not ($fileEntries | Where-Object { $_ -like 'web/apps/editor/dist/assets/*' })) {
        throw "[package-release] Missing zip entry (file under): web/apps/editor/dist/assets/"
    }
    if (-not ($fileEntries | Where-Object { $_ -like 'web/apps/editor/dist/fonts/*' })) {
        throw "[package-release] Missing zip entry (file under): web/apps/editor/dist/fonts/"
    }
    Write-Host "  [ok] zip verified -> $OutZip" -ForegroundColor Cyan
}

# --- Report -------------------------------------------------------------------
Write-Host ""
Write-Host "Staged tree under: $Stage" -ForegroundColor Cyan
Get-ChildItem -LiteralPath $Stage -Recurse -File |
    ForEach-Object { Write-Host ("  " + $_.FullName.Substring($Stage.Length).TrimStart([char]'\', [char]'/')) }

if (-not $OutZip) {
    Write-Host ""
    Write-Host "Next: re-run with -OutZip to create AND self-verify the release archive, e.g.:" -ForegroundColor Cyan
    Write-Host "  pwsh scripts/package-release.ps1 -OutZip ParticleEditor-vX.Y.Z.zip" -ForegroundColor Cyan
    Write-Host "Then attach the zip to the GitHub Release (see VERSIONING.md -> Cutting a release)." -ForegroundColor Cyan
}
