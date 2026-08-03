<#
.SYNOPSIS
    Stage (and optionally zip) the Particle Editor release layout, self-asserting completeness.

.DESCRIPTION
    Assembles the exact directory tree the host launches from and FAILS LOUDLY if any required
    runtime file is missing -- so a release archive can never be produced with a broken/incomplete
    bundle.

    THE RELEASE IS TWO FILES. The React UI is compiled INTO the exe as RCDATA (the vcxproj
    GenerateEmbeddedWebAssets target embeds web/apps/editor/dist; HostWindow.cpp serves it on
    app.local via a WebResourceRequested handler), and the WebView2 loader is statically linked
    (WebView2LoaderPreference=Static). So there is no separate web/ folder and no WebView2Loader.dll
    to ship -- just the exe and the vendored D3DX9 runtime beside it:

        <STAGE>/
          x64/Release/
            ParticleEditor.exe     # self-contained UI (embedded bundle) + static WebView2 loader.
                                    # The WebView2 RUNTIME is a separate machine component; when it
                                    # is absent the app opens https://aka.ms/webview2 for the user
                                    # (OfferWebView2Install in HostWindow.cpp) rather than bundling
                                    # Microsoft's ~2 MB bootstrapper.
            d3dx9_43.dll           # x64 D3DX9 runtime, vendored at libs/redist/

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
$d3dxSource   = [System.IO.Path]::Combine($RepoRoot, 'libs', 'redist', 'd3dx9_43.dll')
$sourceRoots  = @(
    [pscustomobject]@{ Label = 'native release output'; Path = [System.IO.Path]::Combine($RepoRoot, 'x64', 'Release') }
    [pscustomobject]@{ Label = 'vendored D3DX redist'; Path = [System.IO.Path]::Combine($RepoRoot, 'libs', 'redist') }
)

foreach ($sourceRoot in $sourceRoots) {
    if (Test-PathsOverlap $Stage $sourceRoot.Path) {
        throw "[package-release] Refusing -Stage '$Stage' because it overlaps required source: $($sourceRoot.Label) -> $($sourceRoot.Path). Use a dedicated staging directory."
    }
}

Test-RequiredSource 'ParticleEditor.exe'      $exeSource    'Leaf'
Test-RequiredSource 'd3dx9_43.dll (vendored)' $d3dxSource   'Leaf'
# The React UI is EMBEDDED in the exe (RCDATA) and the WebView2 loader is STATICALLY linked
# (vcxproj WebView2LoaderPreference=Static), so neither a web/ folder nor WebView2Loader.dll ships.
# The WebView2 RUNTIME is a separate machine component (present on Windows 11, and Windows 10 with
# Edge; absent on stripped/LTSC images); when it is absent the editor opens https://aka.ms/webview2
# for the user (OfferWebView2Install in HostWindow.cpp) rather than bundling Microsoft's bootstrapper.

# --- Clear + prepare the staged tree -----------------------------------------
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
$stageExeDir  = [System.IO.Path]::Combine($Stage, 'x64', 'Release')
New-Item -ItemType Directory -Force -Path $stageExeDir  | Out-Null

# --- Copy native artifacts ----------------------------------------------------
Copy-Item -LiteralPath $exeSource  -Destination $stageExeDir -Force
Copy-Item -LiteralPath $d3dxSource -Destination $stageExeDir -Force
Write-Host "  [ok] ParticleEditor.exe, d3dx9_43.dll -> $stageExeDir"

# --- Post-stage assertions (fail loud) ---------------------------------------
# The stage is a freshly-cleared dir populated by exactly the two explicit Copy-Item calls above, so
# "nothing extra shipped" is guaranteed by construction -- there is no directory copy to carry a stray
# artifact in (the pre-2026-08 shape allowlist guarded a whole-dir copy of web/apps/editor/dist that
# no longer exists). Present-checks here + the exact zip==stage equality below pin the two-file shape.
Assert-Staged ([System.IO.Path]::Combine('x64', 'Release', 'ParticleEditor.exe'))
Assert-Staged ([System.IO.Path]::Combine('x64', 'Release', 'd3dx9_43.dll'))

# The exe must actually EMBED the web bundle -- this release ships no web/ folder, so the
# UI lives inside the exe as RCDATA. A stale pre-embed (folder-mapping) exe, or one built
# before the bundle existed, would stage "successfully" here yet show ERR_NAME_NOT_RESOLVED
# on launch. Confirm the embedded index.html is present via its stable React mount sentinel
# (id="root"); ASCII.GetString leaves that ASCII substring intact wherever it sits in the
# binary, and works identically under Windows PowerShell 5.1 and pwsh 7.
$stagedExe = [System.IO.Path]::Combine($Stage, 'x64', 'Release', 'ParticleEditor.exe')
$exeText   = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($stagedExe))
if (-not $exeText.Contains('id="root"')) {
    throw "[package-release] ParticleEditor.exe does not embed the web bundle (React mount sentinel not found) -- rebuild x64 Release after building web/apps/editor/dist."
}

$stagedCount = @(Get-ChildItem -LiteralPath $Stage -Recurse -File).Count
Write-Host "  [ok] staged tree verified ($stagedCount files; exe embeds the web bundle)"

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
    # Only FILE entries count -- a bare directory entry (ends in '/') must not satisfy a check.
    $fileEntries = @($entries | Where-Object { -not $_.EndsWith('/') })

    # EXACT-SET contract, not a subset check. A subset check would let a zip carry an extra file
    # (a stray canary, a leftover .pdb, an unrelated dll) or be missing a required artifact and still
    # pass -- both demonstrated by the 2026-07 audit's negative controls. The zip must equal the
    # staged tree exactly: nothing added between staging and compression, nothing dropped. The staged
    # tree is itself asserted above.
    $stagedRel = @(
        Get-ChildItem -LiteralPath $Stage -Recurse -File |
            ForEach-Object { $_.FullName.Substring($Stage.Length).TrimStart([char]'\', [char]'/') -replace '\\', '/' }
    )
    $zipOnly   = @($fileEntries | Where-Object { $stagedRel -notcontains $_ })
    $stageOnly = @($stagedRel   | Where-Object { $fileEntries -notcontains $_ })
    if ($zipOnly.Count -gt 0) {
        throw "[package-release] Zip contains entries absent from the staged tree: $($zipOnly -join ', ')"
    }
    if ($stageOnly.Count -gt 0) {
        throw "[package-release] Zip is MISSING staged files: $($stageOnly -join ', ')"
    }

    # Every shipped artifact named explicitly. The UI is embedded and the WebView2 loader is static,
    # so the release payload is exactly the exe and the vendored d3dx9_43.dll beside it.
    $requiredEntries = @(
        'x64/Release/ParticleEditor.exe'
        'x64/Release/d3dx9_43.dll'
    )
    foreach ($r in $requiredEntries) {
        if ($fileEntries -notcontains $r) { throw "[package-release] Missing zip entry: $r" }
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
    Write-Host "Then attach the zip to the GitHub Release." -ForegroundColor Cyan
}
