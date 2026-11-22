# Package LuxEditor into a standalone portable ZIP distribution
param(
    [string]$BuildDir = "build/local",
    [string]$OutputDir = "dist",
    [string]$Version = "0.1.0",
    [string]$Configuration = "Release"
)

. (Join-Path $PSScriptRoot "Utf8.ps1")
. (Join-Path $PSScriptRoot "WindowsRuntime.ps1")
$ErrorActionPreference = "Stop"
$root = (Get-Item $PSScriptRoot).Parent.FullName
Set-Location $root

if ($Configuration -ne "Release" -and $Configuration -ne "Debug") {
    throw "Packaging failure: Unsupported configuration '$Configuration'. Must be 'Release' or 'Debug'."
}

$suffix = if ($Configuration -eq "Release") { "" } else { "-Debug" }
$qtPostfix = if ($Configuration -eq "Debug") { "d" } else { "" }
$outDirResolved = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $root $OutputDir }
$package = New-LuxPackageOutput -OutputDir $outDirResolved -Name "LuxEditor-Portable-$Version$suffix-x64"
$editorStaging = $package.Staging
$editorZip = $package.Zip
Write-Host "=== Packaging LuxEditor Standalone Portable (v$Version, Configuration=$Configuration) ==="

try {

    $buildDirResolved = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
    $binSource = Join-Path $buildDirResolved "bin/$Configuration"
    $editorExe = Join-Path $binSource "LuxEditor.exe"
    if (-not (Test-Path -LiteralPath $editorExe -PathType Leaf)) {
        throw "Packaging failure: $Configuration executable LuxEditor.exe not found in $binSource. Build $Configuration first. Silent fallback to other configurations is forbidden for public release packaging."
    }

    if ($Configuration -eq "Release") {
        $debugDlls = Get-ChildItem -LiteralPath $binSource -Filter "*d.dll" -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -match "^(vcruntime.*d|msvcp.*d|ucrtbase.*d|ucrtbased|Qt6.*d|SDL3d)\.dll$"
        }
        if ($debugDlls) {
            throw "Packaging failure: Debug runtime DLLs detected in Release binary directory: $($debugDlls.Name -join ', ')"
        }
    }

    # Copy executables
    Copy-Item -LiteralPath $editorExe -Destination $editorStaging -Force
    if (Test-Path -LiteralPath (Join-Path $binSource "LuxPlayer.exe") -PathType Leaf) {
        Copy-Item -LiteralPath (Join-Path $binSource "LuxPlayer.exe") -Destination $editorStaging -Force
    }

    # Copy the Forge renderer, native shaders, GPU configuration and C# host.
    Copy-LuxWindowsRuntime -Source $binSource -Destination $editorStaging

    # Search locations for Qt runtime and tools (vcpkg, standalone Qt, CI environment)
    $qtRoots = @()
    if ($env:QT_ROOT_DIR) { $qtRoots += $env:QT_ROOT_DIR }
    if ($env:Qt6_DIR) {
        $qtRoots += (Split-Path (Split-Path (Split-Path $env:Qt6_DIR)))
    }
    $qtRoots += "C:\vcpkg\installed\x64-windows"
    $qtRoots += "C:\vcpkg\installed\x64-windows\Qt6"
    $qtRoots += "C:\Qt"

    # Copy Qt platform plugin
    $platformsDir = Join-Path $editorStaging "platforms"
    New-Item -ItemType Directory -Path $platformsDir -Force | Out-Null
    if (Test-Path -LiteralPath (Join-Path $binSource "platforms") -PathType Container) {
        Get-ChildItem -LiteralPath (Join-Path $binSource "platforms") -Filter "*.dll" -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $platformsDir -Force
        }
    }
    $offscreenPlugin = "qoffscreen$qtPostfix.dll"
    if (-not (Test-Path -LiteralPath (Join-Path $platformsDir $offscreenPlugin) -PathType Leaf)) {
        foreach ($rootCand in $qtRoots) {
            if (Test-Path -LiteralPath $rootCand -PathType Container) {
                $candOffscreen = Get-ChildItem -LiteralPath $rootCand -Filter $offscreenPlugin -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($candOffscreen) {
                    Copy-Item -LiteralPath $candOffscreen.FullName -Destination $platformsDir -Force
                    break
                }
            }
        }
    }

    # Copy licenses
    $licensesDir = Join-Path $editorStaging "licenses"
    New-Item -ItemType Directory -Path $licensesDir -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $root 'docs/licenses') | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $licensesDir -Recurse -Force
    }

    # Write Standalone Distribution README
    $editorReadme = @"
# LuxEditor — Standalone Portable Edition (v$Version)

LuxEngine 2D/3D Game Editor portable package for Windows 10/11 x64.
No external installation of Qt, CMake, or Visual Studio is required to run the editor.

## System Requirements
- Windows 10 64-bit or Windows 11 64-bit
- Direct3D 12 compatible GPU and up-to-date display driver (NVIDIA GeForce, AMD Radeon, or Intel Arc/UHD)
- 4 GB RAM minimum (8 GB recommended)

## How to Run
Double-click `LuxEditor.exe` to launch the editor.

## Command Line Usage
- `LuxEditor.exe` : Launch graphical editor workspace
- `LuxEditor.exe --version` : Print editor and engine version
- `LuxEditor.exe --smoke-test` : Run headless initialization verification

## Third-Party Licenses
See the `licenses/` directory for full license texts and dependency details.
- Qt 6 (LGPLv3)
- SDL3 (zlib)
- Jolt Physics (MIT)
- libzip (3-clause BSD)
- The Forge (Apache-2.0)
- .NET 10 runtime is required to execute C# game scripts
"@
    $editorReadme | Out-File -LiteralPath (Join-Path $editorStaging "README.md") -Encoding utf8

    # Check for windeployqt to deploy all required Qt runtime dependencies
    $windeployqt = $null
    $deployQtRoot = $null
    foreach ($rootCand in $qtRoots) {
        if (Test-Path -LiteralPath $rootCand -PathType Container) {
            $candWindeployqt = Get-ChildItem -LiteralPath $rootCand -Filter "windeployqt.exe" -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($candWindeployqt) {
                $windeployqt = $candWindeployqt.FullName
                $deployQtRoot = $rootCand
                break
            }
        }
    }
    if (-not $windeployqt) {
        $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
        if ($cmd) { $windeployqt = $cmd.Source }
    }
    $qtRuntimeFiles = @("Qt6Core$qtPostfix.dll", "Qt6Gui$qtPostfix.dll", "Qt6Widgets$qtPostfix.dll",
                        "platforms/qwindows$qtPostfix.dll")
    $readyDebugQt = $Configuration -eq 'Debug' -and $deployQtRoot -and
        (Test-Path -LiteralPath (Join-Path $deployQtRoot 'debug/bin/Qt6Widgetsd.dll') -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-Path $deployQtRoot 'bin/Qt6Widgetsd.dll') -PathType Leaf) -and
        @($qtRuntimeFiles | Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $editorStaging $_) -PathType Leaf)
        }).Count -eq 0
    if ($readyDebugQt) {
        Write-Host "Using the Debug Qt runtime copied from the build output."
    } elseif ($windeployqt -and (Test-Path -LiteralPath $windeployqt -PathType Leaf)) {
        Write-Host "Running windeployqt ($windeployqt) to ensure all required Qt dependencies are bundled..."
        $deployConfiguration = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
        & $windeployqt $deployConfiguration --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $editorStaging "LuxEditor.exe")
        if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
    }

    # Verify essential files strictly (throw immediately on missing file)
    $requiredEditorFiles = @(
        "LuxEditor.exe",
        "SDL3.dll",
        "licenses/LICENSE-LuxEngine.txt",
        "licenses/LICENSE-Qt6.txt",
        "licenses/LICENSE-LGPLv3.txt",
        "licenses/DEPENDENCIES.md"
    ) + $qtRuntimeFiles

    foreach ($f in $requiredEditorFiles) {
        $targetPath = Join-Path $editorStaging $f
        if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
            throw "Packaging integrity failure: required Editor file missing: $f"
        }
    }

    Assert-LuxWindowsRuntime -Directory $editorStaging

    # Verify no debug runtime DLLs are accidentally packaged in Release build
    if ($binSource -like "*Release*") {
        $debugDlls = Get-ChildItem -LiteralPath $editorStaging -Filter "*d.dll" -Recurse | Where-Object {
            $_.Name -match "^(ucrtbase|vcruntime140|msvcp140|Qt6Core|Qt6Gui|Qt6Widgets|SDL3)d\.dll$"
        }
        if ($debugDlls -and $debugDlls.Count -gt 0) {
            throw "Packaging integrity failure: Debug DLLs found in Release Editor package: $($debugDlls.Name -join ', ')"
        }
    }

    # Create ZIP archive
    Write-Host "Creating Editor archive: $editorZip"
    Compress-LuxPackageOutput -Package $package
    Publish-LuxPackageOutput -Package $package
    $editorZip = $package.PublishedZip

    $zipSize = (Get-Item -LiteralPath $editorZip).Length
    $zipHash = (Get-FileHash -LiteralPath $editorZip -Algorithm SHA256).Hash
    Write-Host "=== Editor Packaging Complete ==="
    Write-Host "Package: $editorZip"
    Write-Host "Size   : $([math]::Round($zipSize / 1MB, 2)) MB ($zipSize bytes)"
    Write-Host "SHA256 : $zipHash"
} finally {
    Remove-LuxPackageWork -Package $package
}
