# run-perf-audit.ps1 -- reproducible local performance audit runner.
# ASCII only: keep PS 5.1 output readable.
#
# Requires: x64\Release\ParticleEditor.exe and web/apps/editor/dist.
# This script does not build or commit. It runs supplied --drive workflows with
# perf tracing enabled, stores raw artifacts, and summarizes each trace.

param(
    [string]$Exe = "",
    [string]$OutDir = "",
    [string[]]$DriveScript = @(),
    [switch]$NullSink,
    [switch]$ForceOverwrite,
    [int]$RepeatCount = 1,
    [switch]$ReuseWebViewProfile,
    [int]$TimeoutMs = 120000
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root "x64\Release\ParticleEditor.exe" }
if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
    $OutDir = Join-Path $root "tasks\perf-audit-2026-06-28\runs\$stamp-$PID"
}
if (-not (Test-Path $Exe)) { Write-Error "exe not found: $Exe"; exit 2 }
if ($RepeatCount -lt 1) { Write-Error "-RepeatCount must be >= 1"; exit 2 }

if (Test-Path $OutDir) {
    $existing = @(Get-ChildItem -LiteralPath $OutDir -Force -ErrorAction SilentlyContinue)
    if ($existing.Count -gt 0 -and -not $ForceOverwrite) {
        Write-Error "output directory is not empty; pass -ForceOverwrite or choose another -OutDir: $OutDir"
        exit 2
    }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Write-JsonFile([string]$path, $value) {
    $value | ConvertTo-Json -Depth 8 | Set-Content -Path $path -Encoding UTF8
}

function Get-Sha256([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-GitValue([string[]]$GitArgs) {
    try {
        $value = & git -C $root @GitArgs 2>$null
        if ($LASTEXITCODE -eq 0) { return [string]::Join("`n", @($value)) }
    } catch {}
    return $null
}

function Snapshot-Directory([string]$dir) {
    $result = [ordered]@{
        path = $dir
        exists = [bool](Test-Path -LiteralPath $dir)
        fileCount = 0
        totalBytes = 0
        newestWriteUtc = $null
    }
    if (-not $result.exists) { return $result }
    $newest = $null
    foreach ($f in Get-ChildItem -LiteralPath $dir -File -Recurse -Force -ErrorAction SilentlyContinue) {
        $result.fileCount++
        $result.totalBytes += [int64]$f.Length
        if ($null -eq $newest -or $f.LastWriteTimeUtc -gt $newest) { $newest = $f.LastWriteTimeUtc }
    }
    if ($null -ne $newest) { $result.newestWriteUtc = $newest.ToString("o") }
    return $result
}

function Snapshot-State([string]$path) {
    $localAppData = Join-Path $env:LOCALAPPDATA "AloParticleEditor"
    $tempAutosave = Join-Path $env:TEMP "AloParticleEditor"
    $state = [ordered]@{
        timestampUtc = (Get-Date).ToUniversalTime().ToString("o")
        localAppData = $localAppData
        tempAutosave = $tempAutosave
        registry = @{}
        filesystem = [ordered]@{
            localAppData = Snapshot-Directory $localAppData
            tempAutosave = Snapshot-Directory $tempAutosave
        }
    }
    try {
        $p = Get-ItemProperty "HKCU:\Software\AloParticleEditor" -ErrorAction Stop
        foreach ($n in $p.PSObject.Properties.Name) {
            if ($n -match '^PS') { continue }
            $v = $p.$n
            if ($v -is [byte[]]) { $state.registry[$n] = [BitConverter]::ToString($v) }
            else { $state.registry[$n] = [string]$v }
        }
    } catch {}
    Write-JsonFile $path $state
}

function Compare-State([string]$beforePath, [string]$afterPath, [string]$outPath) {
    $before = Get-Content -Path $beforePath -Raw | ConvertFrom-Json
    $after = Get-Content -Path $afterPath -Raw | ConvertFrom-Json
    $keys = @{}
    foreach ($p in $before.registry.PSObject.Properties) { $keys[$p.Name] = $true }
    foreach ($p in $after.registry.PSObject.Properties) { $keys[$p.Name] = $true }
    $changes = @()
    foreach ($key in ($keys.Keys | Sort-Object)) {
        $a = $before.registry.$key
        $b = $after.registry.$key
        if ([string]$a -ne [string]$b) {
            $changes += [ordered]@{ key = $key; before = $a; after = $b }
        }
    }
    $fsChanges = @()
    foreach ($name in @("localAppData", "tempAutosave")) {
        $a = $before.filesystem.$name
        $b = $after.filesystem.$name
        if ([string]($a | ConvertTo-Json -Compress) -ne [string]($b | ConvertTo-Json -Compress)) {
            $fsChanges += [ordered]@{ key = $name; before = $a; after = $b }
        }
    }
    Write-JsonFile $outPath ([ordered]@{ registryChanges = $changes; filesystemChanges = $fsChanges })
}

function Write-Manifest([string]$runDir) {
    $resolvedRunDir = [IO.Path]::GetFullPath($runDir).TrimEnd('\')
    $prefix = $resolvedRunDir + '\'
    $files = Get-ChildItem -LiteralPath $runDir -File -Recurse |
        Where-Object { $_.Name -ne "artifact-manifest.json" } |
        Sort-Object FullName |
        ForEach-Object {
            $full = [IO.Path]::GetFullPath($_.FullName)
            [ordered]@{
                path = $full.Substring($prefix.Length)
                bytes = $_.Length
                sha256 = Get-Sha256 $_.FullName
            }
        }
    Write-JsonFile (Join-Path $runDir "artifact-manifest.json") ([ordered]@{ files = @($files) })
}

function Write-RunMetadata([string]$runDir, [string]$scriptPath, [string]$profile) {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
    $gpu = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -First 4
    $exeItem = Get-Item -LiteralPath $Exe
    $distDir = Join-Path $root "web\apps\editor\dist"
    $distFiles = @()
    if (Test-Path -LiteralPath $distDir) {
        $distFiles = @(Get-ChildItem -LiteralPath $distDir -File -Recurse -ErrorAction SilentlyContinue)
    }
    Write-JsonFile (Join-Path $runDir "run-metadata.json") ([ordered]@{
        timestampUtc = (Get-Date).ToUniversalTime().ToString("o")
        gitBranch = (Get-GitValue -GitArgs @("branch", "--show-current"))
        gitHead = (Get-GitValue -GitArgs @("rev-parse", "HEAD"))
        gitDirtyPorcelain = (Get-GitValue -GitArgs @("status", "--short"))
        exe = [ordered]@{
            path = $Exe
            bytes = $exeItem.Length
            sha256 = Get-Sha256 $Exe
            lastWriteUtc = $exeItem.LastWriteTimeUtc.ToString("o")
        }
        webDist = [ordered]@{
            path = $distDir
            fileCount = $distFiles.Count
            totalBytes = ($distFiles | Measure-Object -Property Length -Sum).Sum
        }
        fixture = [ordered]@{
            path = $scriptPath
            sha256 = Get-Sha256 $scriptPath
        }
        profile = [ordered]@{
            path = $profile
            reused = [bool]$ReuseWebViewProfile
        }
        environment = [ordered]@{
            machine = $env:COMPUTERNAME
            user = $env:USERNAME
            osCaption = if ($os) { $os.Caption } else { $null }
            osVersion = if ($os) { $os.Version } else { $null }
            totalVisibleMemoryBytes = if ($os) { [int64]$os.TotalVisibleMemorySize * 1024 } else { $null }
            cpuName = if ($cpu) { $cpu.Name } else { $null }
            cpuCores = if ($cpu) { $cpu.NumberOfCores } else { $null }
            cpuLogicalProcessors = if ($cpu) { $cpu.NumberOfLogicalProcessors } else { $null }
            gpu = @($gpu | ForEach-Object { [ordered]@{ name = $_.Name; driverVersion = $_.DriverVersion } })
        }
    })
}

function Clear-RunDirForOverwrite([string]$runDir, [string]$outRoot) {
    if (-not (Test-Path -LiteralPath $runDir)) { return }
    $resolvedRun = [IO.Path]::GetFullPath($runDir)
    $resolvedRoot = [IO.Path]::GetFullPath($outRoot).TrimEnd('\') + '\'
    if (-not $resolvedRun.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Error "refusing to clear run directory outside output root: $resolvedRun"
        exit 2
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force
}

function Get-RequiredEvents([string]$scriptPath) {
    $text = Get-Content -LiteralPath $scriptPath -Raw
    $events = @()
    try {
        $json = $text | ConvertFrom-Json
        if ($json.requiredEvents) {
            foreach ($event in $json.requiredEvents) { $events += [string]$event }
        }
    } catch {}
    if ($events.Count -eq 0) {
        $events = @("clock_calibration", "renderer.bridge.request:1", "bridge.dispatch:1")
    }
    if ($text -match '"textures/get-preview"') {
        $events += @("texture.preview:1", "bridge.texture_preview:1")
    }
    return $events
}

function Invoke-PerfDrive([string]$scriptPath, [int]$index, [int]$repeatIndex) {
    if (-not (Test-Path $scriptPath)) { Write-Error "drive script not found: $scriptPath"; exit 2 }
    $name = [IO.Path]::GetFileNameWithoutExtension($scriptPath)
    $runName = if ($RepeatCount -gt 1) {
        "r{0:00}-{1:00}-{2}" -f $repeatIndex, $index, $name
    } else {
        "{0:00}-{1}" -f $index, $name
    }
    $runDir = Join-Path $OutDir $runName
    if (Test-Path $runDir) {
        $existing = @(Get-ChildItem -LiteralPath $runDir -Force -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0 -and -not $ForceOverwrite) {
            Write-Error "run directory is not empty; pass -ForceOverwrite or choose another -OutDir: $runDir"
            exit 2
        }
        if ($existing.Count -gt 0 -and $ForceOverwrite) {
            Clear-RunDirForOverwrite $runDir $OutDir
        }
    }
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item $scriptPath (Join-Path $runDir "drive-script.json") -Force

    $trace = Join-Path $runDir "perf-trace.ndjson"
    $profile = if ($ReuseWebViewProfile) {
        Join-Path $OutDir "shared-webview-profile"
    } else {
        Join-Path $runDir "webview-profile"
    }
    $stateBefore = Join-Path $runDir "state-before.json"
    $stateAfter = Join-Path $runDir "state-after.json"
    Snapshot-State $stateBefore

    $args = @("--drive", "`"$scriptPath`"", "--perf-artifact-dir", "`"$runDir`"", "--perf-webview-profile", "`"$profile`"")
    if ($NullSink) {
        $args += @("--perf-trace-mode", "null")
    } else {
        $args += @("--perf-trace", "`"$trace`"")
    }

    Write-Host "[perf] $name"
    Write-JsonFile (Join-Path $runDir "command.json") ([ordered]@{
        exe = $Exe
        args = $args
        timeoutMs = $TimeoutMs
        nullSink = [bool]$NullSink
        repeatIndex = $repeatIndex
        repeatCount = $RepeatCount
        reuseWebViewProfile = [bool]$ReuseWebViewProfile
    })
    Write-RunMetadata $runDir $scriptPath $profile

    $timedOut = $false
    $killSucceeded = $null
    $exitCode = 0
    $p = Start-Process -FilePath $Exe -ArgumentList ($args -join " ") -PassThru -WindowStyle Normal
    try {
        if (-not $p.WaitForExit($TimeoutMs)) {
            $timedOut = $true
            try { $p.Kill() } catch {}
            $p.WaitForExit(5000) | Out-Null
            $killSucceeded = [bool]$p.HasExited
            "timeout" | Set-Content -Path (Join-Path $runDir "exit.txt") -Encoding ASCII
            if (-not $killSucceeded) {
                "process did not exit after Kill()" | Set-Content -Path (Join-Path $runDir "kill-failed.txt") -Encoding ASCII
                $exitCode = 6
            } else {
                $exitCode = 5
            }
        } else {
            $exitCode = [int]$p.ExitCode
            $p.ExitCode | Set-Content -Path (Join-Path $runDir "exit.txt") -Encoding ASCII
        }
    } finally {
        if (-not $p.HasExited) { try { $p.Kill() } catch {} }
        Snapshot-State $stateAfter
        Compare-State $stateBefore $stateAfter (Join-Path $runDir "state-diff.json")
        Write-JsonFile (Join-Path $runDir "cleanup.json") ([ordered]@{
            timedOut = $timedOut
            killSucceeded = $killSucceeded
            processExited = [bool]$p.HasExited
        })
    }

    if (-not $NullSink -and $exitCode -eq 0) {
        if (-not (Test-Path $trace) -or (Get-Item -LiteralPath $trace).Length -eq 0) {
            Write-JsonFile (Join-Path $runDir "analyzer-status.json") ([ordered]@{
                status = "missing-trace"
                trace = $trace
            })
            Write-Manifest $runDir
            return 7
        }
        $analyzerArgs = @($trace, "--out", $runDir)
        foreach ($required in Get-RequiredEvents $scriptPath) {
            $analyzerArgs += @("--require-event", $required)
        }
        node (Join-Path $root "scripts\analyze-perf-trace.mjs") @analyzerArgs | Out-Host
        $analyzerExit = $LASTEXITCODE
        Write-JsonFile (Join-Path $runDir "analyzer-status.json") ([ordered]@{
            status = $(if ($analyzerExit -eq 0) { "ok" } else { "failed" })
            exitCode = $analyzerExit
            trace = $trace
        })
        if ($analyzerExit -ne 0) {
            Write-Manifest $runDir
            return 8
        }
    }

    Write-Manifest $runDir
    return $exitCode
}

if ($DriveScript.Count -eq 0) {
    $defaultDir = Join-Path $root "tasks\perf-audit-2026-06-28\fixtures\drive"
    if (Test-Path $defaultDir) {
        $DriveScript = @(Get-ChildItem $defaultDir -Filter "*.json" | Sort-Object Name | ForEach-Object { $_.FullName })
    }
}
if ($DriveScript.Count -eq 0) {
    Write-Error "no drive scripts supplied; pass -DriveScript or add tasks\perf-audit-2026-06-28\fixtures\drive\*.json"
    exit 2
}

$fail = 0
for ($r = 1; $r -le $RepeatCount; $r++) {
    for ($i = 0; $i -lt $DriveScript.Count; $i++) {
        $code = Invoke-PerfDrive $DriveScript[$i] ($i + 1) $r
        if ($code -ne 0) { $fail++ }
    }
}

Write-Host "[perf] artifacts: $OutDir"
if ($fail -gt 0) { exit 1 }
exit 0
