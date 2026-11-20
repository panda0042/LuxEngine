# Verify SDK Relocatability & Consumer Build in Debug and Release
param(
    [string]$BuildDir = "build/local",
    [string]$SdkPrefix = "",
    [string]$SdkArchive = "",
    [string]$DepsDir = "C:/LuxDeps"
)

. (Join-Path $PSScriptRoot "Utf8.ps1")
. (Join-Path $PSScriptRoot "WindowsRuntime.ps1")
$ErrorActionPreference = "Stop"
$root = (Get-Item $PSScriptRoot).Parent.FullName
Set-Location $root

Write-Host "=== Testing LuxEngine SDK Relocation & Consumer Build ==="

$cmakeExe = "cmake"
$vsCMake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (Test-Path $vsCMake) { $cmakeExe = $vsCMake }

$isUserSpecifiedPrefix = -not [string]::IsNullOrWhiteSpace($SdkPrefix)
$shouldCleanupSdk = -not $isUserSpecifiedPrefix

if (-not $isUserSpecifiedPrefix) {
    # If no SDK prefix provided, test relocatability using a dedicated path with spaces
    $tempBase = [System.IO.Path]::GetTempPath()
    $SdkPrefix = Join-Path $tempBase "LuxSDK Relocated Path $(Get-Random)"
    New-Item -ItemType Directory -Path $SdkPrefix -Force | Out-Null
} else {
    # User specified an SdkPrefix: NEVER delete or overwrite existing user directories!
    if (Test-Path $SdkPrefix) {
        $existingItems = Get-ChildItem -Path $SdkPrefix -Force -ErrorAction SilentlyContinue
        if ($existingItems -and $existingItems.Count -gt 0) {
            throw "Target SdkPrefix directory already exists and is not empty: '$SdkPrefix'. VerifySdkRelocation refuses to overwrite or delete existing user directories to prevent data loss. Please specify an empty directory or omit -SdkPrefix to use a dedicated isolated path."
        }
    } else {
        New-Item -ItemType Directory -Path $SdkPrefix -Force | Out-Null
    }
}

Write-Host "SDK Relocation Target: $SdkPrefix"

$consumerTestDir = Join-Path ([System.IO.Path]::GetTempPath()) "LuxConsumerTest-$(Get-Random)"
New-Item -ItemType Directory -Path $consumerTestDir -Force | Out-Null

try {
    if (-not $SdkArchive) {
        $packages = Join-Path $consumerTestDir "packages"
        $packageArguments = @{ BuildDir = $BuildDir; DepsDir = $DepsDir; OutputDir = $packages; CMakeExe = $cmakeExe }
        & (Join-Path $PSScriptRoot "PackageSdk.ps1") @packageArguments
        $SdkArchive = Join-Path $packages "LuxEngine-SDK-0.1.0-x64.zip"
    }
    Write-Host "1. Extracting SDK ZIP to relocated path..."
    Expand-Archive -LiteralPath $SdkArchive -DestinationPath $SdkPrefix

    $sdkCmake = Join-Path $SdkPrefix "lib/cmake/LuxEngine"
    if (-not (Test-Path (Join-Path $sdkCmake "LuxEngineConfig.cmake"))) {
        throw "LuxEngineConfig.cmake missing in $sdkCmake"
    }
    Assert-LuxWindowsRuntime -Directory (Join-Path $SdkPrefix "bin")

    # 2. Test MinimalGame Consumer against Relocated SDK in Release & Debug
    Copy-Item -Path "$root/samples/MinimalGame/*" -Destination $consumerTestDir -Recurse -Force

    $consumerBuild = Join-Path $consumerTestDir "build"
    Write-Host "2. Configuring MinimalGame against relocated SDK..."
    $sdlCmake = Join-Path $SdkPrefix "share/sdl3"
    $joltCmake = Join-Path $SdkPrefix "lib/cmake/Jolt"
    foreach ($file in @("$sdlCmake/SDL3Config.cmake", "$joltCmake/JoltConfig.cmake")) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "SDK dependency package missing: $file"
        }
    }
    $configureArgs = @("-DLuxEngine_DIR=$sdkCmake", "-DSDL3_DIR=$sdlCmake", "-DJolt_DIR=$joltCmake",
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE")
    & $cmakeExe -S $consumerTestDir -B $consumerBuild -A x64 @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for consumer" }

    # Build & Run Release
    Write-Host "3. Building & running MinimalGame (Release)..."
    & $cmakeExe --build $consumerBuild --config Release
    if ($LASTEXITCODE -ne 0) { throw "CMake build (Release) failed for consumer" }

    $releaseExe = Join-Path $consumerBuild "Release/MinimalGame.exe"
    if (-not (Test-Path $releaseExe)) {
        $releaseExe = Join-Path $consumerBuild "bin/Release/MinimalGame.exe"
    }
    if (-not (Test-Path $releaseExe)) {
        throw "MinimalGame.exe (Release) not found in $consumerBuild"
    }
    Assert-LuxWindowsRuntime -Directory (Split-Path $releaseExe)
    Write-Host "Running $releaseExe..."
    & $releaseExe
    if ($LASTEXITCODE -ne 0) { throw "MinimalGame.exe (Release) execution failed: exit code $LASTEXITCODE" }

    # Build & Run Debug
    Write-Host "4. Building & running MinimalGame (Debug)..."
    & $cmakeExe --build $consumerBuild --config Debug
    if ($LASTEXITCODE -ne 0) { throw "CMake build (Debug) failed for consumer" }

    $debugExe = Join-Path $consumerBuild "Debug/MinimalGame.exe"
    if (-not (Test-Path $debugExe)) {
        $debugExe = Join-Path $consumerBuild "bin/Debug/MinimalGame.exe"
    }
    if (-not (Test-Path $debugExe)) {
        throw "MinimalGame.exe (Debug) not found in $consumerBuild"
    }
    Assert-LuxWindowsRuntime -Directory (Split-Path $debugExe)
    Write-Host "Running $debugExe..."
    & $debugExe
    if ($LASTEXITCODE -ne 0) { throw "MinimalGame.exe (Debug) execution failed: exit code $LASTEXITCODE" }

    Write-Host "`n=== SDK Relocation & MinimalGame Verification: ALL PASSED ==="
}
finally {
    if (Test-Path $consumerTestDir) {
        Remove-Item -Recurse -Force $consumerTestDir -ErrorAction SilentlyContinue
    }
    # NEVER delete user-specified directories! Only clean up if auto-generated by the script.
    if ($shouldCleanupSdk -and (Test-Path $SdkPrefix)) {
        Remove-Item -Recurse -Force $SdkPrefix -ErrorAction SilentlyContinue
    }
}
