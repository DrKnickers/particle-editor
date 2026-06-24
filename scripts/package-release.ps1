<#
.SYNOPSIS
    Stage the Particle Editor release-zip layout.

.DESCRIPTION
    Assembles the exact directory tree the host expects to launch from, ready
    to zip into a GitHub Release artifact.

    WHY THIS LAYOUT IS LOAD-BEARING:
    The host computes the web-bundle path at runtime by walking UP three parent
    directories from the running .exe, then descending into web\apps\editor\dist
    (see src/host/HostWindow.cpp ComputeEditorDistPath:

        auto root = p.parent_path().parent_path().parent_path();
        return root / "web" / "apps" / "editor" / "dist";

    So from  <STAGE>\x64\Release\ParticleEditor.exe  the three .parent_path()
    hops land back on <STAGE>, and the bundle must live at
    <STAGE>\web\apps\editor\dist. Flatten the exe or move the bundle and the
    WebView shows ERR_NAME_NOT_RESOLVED for app.local at launch.

    Authoritative staged tree:

        <STAGE>\
          x64\Release\
            ParticleEditor.exe
            d3dx9_43.dll          # bundled beside the exe (x64 build)
          web\apps\editor\dist\
            index.html
            assets\
            fonts\

    d3dx9_43.dll SOURCE (architecture matters): the editor is x64, so the DLL
    must be the x64 build. This script copies it from $env:WINDIR\System32
    (on 64-bit Windows that is the x64 copy). It NEVER copies from SysWOW64
    (the 32-bit copy, which will not load into the x64 exe). If System32 has
    no copy, the script notes the DXSDK CAB expand alternative and continues.

.PARAMETER Stage
    Destination directory for the staged tree. Defaults to .\release-stage
    (relative to the current working directory). Re-running overwrites files.

.PARAMETER RepoRoot
    Repository root to copy the build artifacts from. Defaults to the parent
    of this script's directory (scripts\.. = repo root).

.EXAMPLE
    pwsh scripts\package-release.ps1
    pwsh scripts\package-release.ps1 -Stage C:\tmp\pe-release
#>

[CmdletBinding()]
param(
    [string] $Stage = (Join-Path (Get-Location) 'release-stage'),
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

Write-Host "Particle Editor -- release staging" -ForegroundColor Cyan
Write-Host "  RepoRoot : $RepoRoot"
Write-Host "  Stage    : $Stage"
Write-Host ""

# --- Resolve and validate the source artifacts --------------------------------

$exeSource  = Join-Path $RepoRoot 'x64\Release\ParticleEditor.exe'
$distSource = Join-Path $RepoRoot 'web\apps\editor\dist'

if (-not (Test-Path -LiteralPath $exeSource -PathType Leaf)) {
    throw "Built exe not found: $exeSource`n" +
          "Build x64 Release first: msbuild ParticleEditor.sln /p:Configuration=Release /p:Platform=x64"
}
if (-not (Test-Path -LiteralPath $distSource -PathType Container)) {
    throw "Web bundle not found: $distSource`n" +
          "Build the web bundle first (in web\): pnpm install, then pnpm --filter ./apps/editor build"
}

# --- Prepare the staged tree --------------------------------------------------

$stageExeDir  = Join-Path $Stage 'x64\Release'
$stageDistDir = Join-Path $Stage 'web\apps\editor\dist'

New-Item -ItemType Directory -Force -Path $stageExeDir  | Out-Null
New-Item -ItemType Directory -Force -Path $stageDistDir | Out-Null

# --- Copy the exe -------------------------------------------------------------

Copy-Item -LiteralPath $exeSource -Destination $stageExeDir -Force -ErrorAction Stop
Write-Host "  [ok] ParticleEditor.exe -> $stageExeDir"

# --- Copy the web bundle (index.html, assets\, fonts\) ------------------------

# Copy the *contents* of dist into the staged dist dir so the tree matches
# web\apps\editor\dist\{index.html, assets\, fonts\} exactly.
Copy-Item -LiteralPath (Join-Path $distSource '*') -Destination $stageDistDir -Recurse -Force -ErrorAction Stop
Write-Host "  [ok] web bundle -> $stageDistDir"

# --- Bundle the x64 d3dx9_43.dll beside the exe -------------------------------

$dllName    = 'd3dx9_43.dll'
$dllStage   = Join-Path $stageExeDir $dllName
$dllSystem  = Join-Path $env:WINDIR "System32\$dllName"   # x64 copy on 64-bit Windows
$dllSysWow  = Join-Path $env:WINDIR "SysWOW64\$dllName"   # 32-bit copy -- DO NOT USE

if (Test-Path -LiteralPath $dllSystem -PathType Leaf) {
    Copy-Item -LiteralPath $dllSystem -Destination $dllStage -Force -ErrorAction Stop
    Write-Host "  [ok] $dllName (x64, from System32) -> $stageExeDir"
}
else {
    Write-Host "  [!!] $dllName not found in System32 ($dllSystem)." -ForegroundColor Yellow
    Write-Host "       Do NOT copy from SysWOW64 ($dllSysWow) -- that is the 32-bit copy" -ForegroundColor Yellow
    Write-Host "       and will not load into the x64 exe." -ForegroundColor Yellow
    Write-Host "       Expand the x64 DLL from the DirectX SDK (June 2010) CAB instead:" -ForegroundColor Yellow
    Write-Host '         expand "$env:DXSDK_DIR\Redist\Jun2010_d3dx9_43_x64.cab" -F:d3dx9_43.dll <dest>' -ForegroundColor Yellow
    Write-Host "       (use the _x64.cab, NOT the _x86.cab), then copy it to:" -ForegroundColor Yellow
    Write-Host "         $stageExeDir" -ForegroundColor Yellow
}

# --- Report -------------------------------------------------------------------

Write-Host ""
Write-Host "Staged tree under: $Stage" -ForegroundColor Cyan
Get-ChildItem -LiteralPath $Stage -Recurse -File |
    ForEach-Object { Write-Host ("  " + $_.FullName.Substring($Stage.Length).TrimStart('\')) }

Write-Host ""
Write-Host "Next: zip the staged tree (its CONTENTS become the zip root), e.g.:" -ForegroundColor Cyan
Write-Host "  Compress-Archive -Path `"$Stage\*`" -DestinationPath ParticleEditor-vX.Y.Z.zip" -ForegroundColor Cyan
Write-Host "Then attach the zip to the GitHub Release (see VERSIONING.md -> Cutting a release)." -ForegroundColor Cyan
