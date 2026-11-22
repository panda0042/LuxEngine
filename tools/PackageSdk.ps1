# Package LuxEngine SDK into a portable ZIP distribution
param(
    [string]$BuildDir = "build/local",
    [string]$OutputDir = "dist",
    [string]$Version = "0.1.0",
    [string]$DepsDir = "C:/LuxDeps",
    [string]$CMakeExe = "cmake"
)

. (Join-Path $PSScriptRoot "Utf8.ps1")
. (Join-Path $PSScriptRoot "WindowsRuntime.ps1")
$ErrorActionPreference = "Stop"
$root = (Get-Item $PSScriptRoot).Parent.FullName
Set-Location $root

Write-Host "=== Packaging LuxEngine SDK (v$Version) ==="
$outDirResolved = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $root $OutputDir }
$package = New-LuxPackageOutput -OutputDir $outDirResolved -Name "LuxEngine-SDK-$Version-x64"
$sdkStaging = $package.Staging
$sdkZip = $package.Zip

try {

    # Keep both suffixed engine libraries, then leave shared runtime files in Release mode.
    Write-Host "Installing Debug SDK to staging..."
    & $CMakeExe --install $BuildDir --config Debug --prefix $sdkStaging
    if ($LASTEXITCODE -ne 0) { throw "cmake --install Debug failed" }

    Write-Host "Installing Release SDK to staging..."
    & $CMakeExe --install $BuildDir --config Release --prefix $sdkStaging
    if ($LASTEXITCODE -ne 0) { throw "cmake --install Release failed" }

    # Copy SDL3 runtime DLL into bin from Release build
    $buildDirResolved = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
    $sdlDll = Join-Path $buildDirResolved "bin/Release/SDL3.dll"
    if (Test-Path -LiteralPath $sdlDll -PathType Leaf) {
        Copy-Item -LiteralPath $sdlDll -Destination "$sdkStaging/bin/SDL3.dll" -Force
        Write-Host "Copied SDL3.dll to SDK bin/"
    } else {
        throw "Packaging failure: Release SDL3.dll not found in $BuildDir/bin/Release. Build Release first."
    }

    # Bundle Jolt headers, libraries and CMake configuration
    $JoltInclude = "$DepsDir/include/Jolt"
    if (Test-Path -LiteralPath $JoltInclude -PathType Container) {
        New-Item -ItemType Directory -Path "$sdkStaging/include/Jolt" -Force | Out-Null
        Get-ChildItem -LiteralPath $JoltInclude | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination "$sdkStaging/include/Jolt" -Recurse -Force
        }
        Write-Host "Bundled Jolt headers"
    }
    $JoltLibRelease = "$DepsDir/lib/Jolt.lib"
    if (Test-Path -LiteralPath $JoltLibRelease -PathType Leaf) {
        Copy-Item -LiteralPath $JoltLibRelease -Destination "$sdkStaging/lib/Jolt.lib" -Force
        Write-Host "Bundled Jolt Release lib"
    }
    $JoltLibDebug = "$DepsDir/lib/Joltd.lib"
    if (Test-Path -LiteralPath $JoltLibDebug -PathType Leaf) {
        Copy-Item -LiteralPath $JoltLibDebug -Destination "$sdkStaging/lib/Joltd.lib" -Force
        Write-Host "Bundled Jolt Debug lib"
    }
    $JoltCmakeDir = "$DepsDir/lib/cmake/Jolt"
    if (Test-Path -LiteralPath $JoltCmakeDir -PathType Container) {
        New-Item -ItemType Directory -Path "$sdkStaging/lib/cmake/Jolt" -Force | Out-Null
        Get-ChildItem -LiteralPath $JoltCmakeDir | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination "$sdkStaging/lib/cmake/Jolt" -Recurse -Force
        }
        Write-Host "Bundled Jolt CMake package"
    }

    # Bundle SDL3 headers, import library and CMake configuration
    $vcpkgBase = if ($env:VCPKG_INSTALLATION_ROOT) { $env:VCPKG_INSTALLATION_ROOT } elseif (Test-Path "C:/vcpkg") { "C:/vcpkg" } else { "" }
    $sdlInclude = if ($vcpkgBase) { "$vcpkgBase/installed/x64-windows/include/SDL3" } else { "" }
    if ($sdlInclude -and (Test-Path -LiteralPath $sdlInclude -PathType Container)) {
        New-Item -ItemType Directory -Path "$sdkStaging/include/SDL3" -Force | Out-Null
        Get-ChildItem -LiteralPath $sdlInclude | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination "$sdkStaging/include/SDL3" -Recurse -Force
        }
        Write-Host "Bundled SDL3 headers"
    }
    $sdlLib = if ($vcpkgBase) { "$vcpkgBase/installed/x64-windows/lib/SDL3.lib" } else { "" }
    if ($sdlLib -and (Test-Path -LiteralPath $sdlLib -PathType Leaf)) {
        Copy-Item -LiteralPath $sdlLib -Destination "$sdkStaging/lib/SDL3.lib" -Force
        Write-Host "Bundled SDL3 import lib"
    }
    if ($vcpkgBase) {
        $sdlRoot = Join-Path $vcpkgBase 'installed/x64-windows'
        foreach ($directory in @('share/sdl3', 'debug/lib', 'debug/bin')) {
            New-Item -ItemType Directory -Path (Join-Path $sdkStaging $directory) -Force | Out-Null
        }
        Get-ChildItem -LiteralPath (Join-Path $sdlRoot 'share/sdl3') -Filter '*.cmake' -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination "$sdkStaging/share/sdl3"
        }
        foreach ($file in @('debug/lib/SDL3.lib', 'debug/bin/SDL3.dll')) {
            Copy-Item -LiteralPath (Join-Path $sdlRoot $file) -Destination (Join-Path $sdkStaging $file)
        }
    }

    # Verify essential files strictly (throw on missing file)
    $requiredFiles = @(
        "include/LuxEngine/Application.hpp",
        "include/Jolt/Jolt.h",
        "include/SDL3/SDL.h",
        "lib/LuxEngine.lib",
        "lib/LuxRuntime.lib",
        "lib/Jolt.lib",
        "lib/Joltd.lib",
        "lib/cmake/Jolt/JoltConfig.cmake",
        "lib/cmake/Jolt/JoltConfig-debug.cmake",
        "lib/cmake/Jolt/JoltConfig-release.cmake",
        "lib/SDL3.lib",
        "bin/SDL3.dll",
        "debug/lib/SDL3.lib",
        "debug/bin/SDL3.dll",
        "share/sdl3/SDL3Config.cmake",
        "share/sdl3/SDL3ConfigVersion.cmake",
        "lib/cmake/LuxEngine/LuxEngineConfig.cmake",
        "share/LuxEngine/shaders/DIRECT3D12/default.rootsig",
        "share/LuxEngine/shaders/DIRECT3D12/lux2d.vert",
        "share/LuxEngine/shaders/DIRECT3D12/lux2d.frag",
        "licenses/LICENSE-LuxEngine.txt",
        "licenses/LICENSE-LGPLv3.txt",
        "licenses/LICENSE-Jolt.txt",
        "licenses/DEPENDENCIES.md",
        "examples/MinimalGame/CMakeLists.txt",
        "examples/MinimalGame/main.cpp"
    )

    foreach ($f in $requiredFiles) {
        $targetPath = Join-Path $sdkStaging $f
        if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
            throw "Packaging integrity failure: required SDK file missing: $f"
        }
    }

    Assert-LuxWindowsRuntime -Directory (Join-Path $sdkStaging "bin")

    # Create ZIP archive
    Write-Host "Creating SDK archive: $sdkZip"
    Compress-LuxPackageOutput -Package $package
    Publish-LuxPackageOutput -Package $package
    $sdkZip = $package.PublishedZip

    $zipSize = (Get-Item -LiteralPath $sdkZip).Length
    $zipHash = (Get-FileHash -LiteralPath $sdkZip -Algorithm SHA256).Hash
    Write-Host "=== SDK Packaging Complete ==="
    Write-Host "Package: $sdkZip"
    Write-Host "Size   : $([math]::Round($zipSize / 1MB, 2)) MB ($zipSize bytes)"
    Write-Host "SHA256 : $zipHash"
} finally {
    Remove-LuxPackageWork -Package $package
}
