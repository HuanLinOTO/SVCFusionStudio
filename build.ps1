<#
PowerShell build helper for SVCFusion Studio
Usage examples:
  .\build.ps1                      # configure+build DirectML (Release), will try to find CMake
  .\build.ps1 -Configuration Debug -Parallel 4
  .\build.ps1 -Generator "Visual Studio 17 2022" -Platform x64
  .\build.ps1 -VsPath "F:\vs"        # your Visual Studio root (default)
  .\build.ps1 -CMakeArgs @('-DUSE_DIRECTML=OFF')  # override the local DirectML default
#>
[CmdletBinding()]
param(
    [string] $SourceDir = (Get-Location).Path,
    [string] $BuildDir  = "",
    [string] $Generator = "Ninja",
    [string] $Platform  = "",
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string] $Configuration = "Release",
    [int] $Parallel = 8,
    [string] $VsPath = 'F:\\vs',
    [string[]] $CMakeArgs = @()
)
if (-not $BuildDir) { $BuildDir = Join-Path $SourceDir 'build' }

function Find-CMake {
    # 1) on PATH
    $cm = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cm) { return $cm.Path }

    # 2) common Program Files location(s)
    $candidates = @(
        "$Env:ProgramFiles\CMake\bin\cmake.exe",
        "$Env:ProgramFiles(x86)\CMake\bin\cmake.exe",
        (Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
    )

    foreach ($p in $candidates) { if ($p -and (Test-Path $p)) { return $p } }

    # 3) quick recursive lookup inside provided VS root (safe-limited)
    try {
        if (Test-Path $VsPath) {
            $found = Get-ChildItem -Path $VsPath -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue -Force -Depth 5 | Select-Object -First 1
            if ($found) { return $found.FullName }
        }
    } catch { }

    return $null
}

$cmakePath = Find-CMake
if (-not $cmakePath) {
    Write-Host "CMake not found on PATH or under '$VsPath'." -ForegroundColor Yellow
    Write-Host "Install CMake or add it to PATH, then re-run this script." -ForegroundColor Yellow
    Write-Host 'Windows install options: winget install --id Kitware.CMake -e --source winget' -ForegroundColor Gray
    exit 1
}

Write-Host "Using CMake: $cmakePath" -ForegroundColor Green
Write-Host "Source: $SourceDir" -ForegroundColor Gray
Write-Host "Build dir: $BuildDir" -ForegroundColor Gray

# Prepare configure args. The helper defaults local builds to DirectML; CI invokes
# cmake directly with explicit provider flags, so this does not affect workflows.
$cfgArgs = @(
    "-S", "$SourceDir",
    "-B", "$BuildDir",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DUSE_DIRECTML=ON",
    "-DUSE_BUNDLED_DIRECTML_RUNTIME=ON"
)
if ($Generator) { $cfgArgs += ("-G", "$Generator") }
if ($Generator -and $Platform) { $cfgArgs += ("-A", "$Platform") }
if ($CMakeArgs) { $cfgArgs += $CMakeArgs }

# Run configure
$proc = & $cmakePath @cfgArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

# Build
$buildArgs = @("--build", "$BuildDir", "--config", "$Configuration", "--parallel", "$Parallel")
$proc = & $cmakePath @buildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

# Locate binary
# Ensure BuildDir is a single string (avoid accidental arrays from callers)
$BuildDir = [string]$BuildDir
$possible = @(
    [System.IO.Path]::Combine($BuildDir, 'bin', $Configuration, 'SVCFusionStudio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'bin', $Configuration, 'SVCFusion Studio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'SVCFusionStudio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'SVCFusion Studio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'SVCFusionStudio_artefacts', $Configuration, 'SVCFusionStudio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'SVCFusionStudio_artefacts', $Configuration, 'SVCFusion Studio.exe'),
    [System.IO.Path]::Combine($BuildDir, $Configuration, 'SVCFusionStudio.exe'),
    [System.IO.Path]::Combine($BuildDir, $Configuration, 'SVCFusion Studio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'Release', 'SVCFusionStudio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'Release', 'SVCFusion Studio.exe'),
    [System.IO.Path]::Combine($BuildDir, 'SVCFusionStudio_artefacts', 'Release', 'SVCFusionStudio.exe')
    [System.IO.Path]::Combine($BuildDir, 'SVCFusionStudio_artefacts', 'Release', 'SVCFusion Studio.exe')
)
foreach ($p in $possible) {
    if ($p -and (Test-Path $p)) {
        Write-Host "Build succeeded — executable: $p" -ForegroundColor Green
        exit 0
    }
}

Write-Host "Build finished (no EXE found in expected locations). Check build output." -ForegroundColor Yellow
exit 0
