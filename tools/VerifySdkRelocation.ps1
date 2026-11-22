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
if (Test-Path -LiteralPath $vsCMake -PathType Leaf) { $cmakeExe = $vsCMake }

$isUserSpecifiedPrefix = -not [string]::IsNullOrWhiteSpace($SdkPrefix)
$shouldCleanupSdk = -not $isUserSpecifiedPrefix
$tempBase = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath([IO.Path]::GetTempPath()))

if (-not $isUserSpecifiedPrefix) {
    # If no SDK prefix provided, test relocatability using a dedicated path with spaces
    $SdkPrefix = Join-Path $tempBase "LuxSDK Relocated Path $([guid]::NewGuid())"
} else {
    # User specified an SdkPrefix: NEVER delete or overwrite existing user directories!
    $SdkPrefix = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SdkPrefix)
    if (Test-Path -LiteralPath $SdkPrefix) {
        if (-not (Test-Path -LiteralPath $SdkPrefix -PathType Container)) {
            throw "SdkPrefix must be a directory: '$SdkPrefix'."
        }
        if (Get-ChildItem -LiteralPath $SdkPrefix -Force | Select-Object -First 1) {
            throw "Target SdkPrefix directory already exists and is not empty: '$SdkPrefix'. VerifySdkRelocation refuses to overwrite or delete existing user directories to prevent data loss. Please specify an empty directory or omit -SdkPrefix to use a dedicated isolated path."
        }
    }
}

Write-Host "SDK Relocation Target: $SdkPrefix"

$consumerTestDir = Join-Path $tempBase "LuxConsumerTest-$([guid]::NewGuid())"
[IO.Directory]::CreateDirectory($consumerTestDir) | Out-Null

try {
    if (-not $SdkArchive) {
        $packages = Join-Path $consumerTestDir "packages"
        $packageArguments = @{ BuildDir = $BuildDir; DepsDir = $DepsDir; OutputDir = $packages; CMakeExe = $cmakeExe }
        & (Join-Path $PSScriptRoot "PackageSdk.ps1") @packageArguments
        $SdkArchive = Join-Path $packages "LuxEngine-SDK-0.1.0-x64.zip"
    }
    Write-Host "1. Extracting SDK ZIP to relocated path..."
    $archivePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SdkArchive)
    [IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $SdkPrefix)

    $sdkCmake = Join-Path $SdkPrefix "lib/cmake/LuxEngine"
    if (-not (Test-Path -LiteralPath (Join-Path $sdkCmake "LuxEngineConfig.cmake") -PathType Leaf)) {
        throw "LuxEngineConfig.cmake missing in $sdkCmake"
    }
    Assert-LuxWindowsRuntime -Directory (Join-Path $SdkPrefix "bin")

    # 2. Test MinimalGame Consumer against Relocated SDK in Release & Debug
    Get-ChildItem -LiteralPath (Join-Path $root 'samples/MinimalGame') | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $consumerTestDir -Recurse -Force
    }

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
    if (-not (Test-Path -LiteralPath $releaseExe -PathType Leaf)) {
        $releaseExe = Join-Path $consumerBuild "bin/Release/MinimalGame.exe"
    }
    if (-not (Test-Path -LiteralPath $releaseExe -PathType Leaf)) {
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
    if (-not (Test-Path -LiteralPath $debugExe -PathType Leaf)) {
        $debugExe = Join-Path $consumerBuild "bin/Debug/MinimalGame.exe"
    }
    if (-not (Test-Path -LiteralPath $debugExe -PathType Leaf)) {
        throw "MinimalGame.exe (Debug) not found in $consumerBuild"
    }
    Assert-LuxWindowsRuntime -Directory (Split-Path $debugExe)
    Write-Host "Running $debugExe..."
    & $debugExe
    if ($LASTEXITCODE -ne 0) { throw "MinimalGame.exe (Debug) execution failed: exit code $LASTEXITCODE" }

    Write-Host "`n=== SDK Relocation & MinimalGame Verification: ALL PASSED ==="
}
finally {
    Remove-LuxPackageDirectory -Path $consumerTestDir -OutputDir $tempBase
    # NEVER delete user-specified directories! Only clean up if auto-generated by the script.
    if ($shouldCleanupSdk) {
        Remove-LuxPackageDirectory -Path $SdkPrefix -OutputDir $tempBase
    }
}
