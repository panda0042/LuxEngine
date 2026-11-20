# Packaging must reject the wrong configuration and Debug CRTs in Release output.
param([string]$BuildDir = "build/local")

$ErrorActionPreference = "Stop"
$repository = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$powerShell = (Get-Process -Id $PID).Path
$testDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "LuxPackagingIntegrity-$([guid]::NewGuid())"

function Expect-PackagingFailure {
    param([string]$Script, [string[]]$Arguments, [string]$Expected)
    # Keep expected child-process errors as data, then check the actual diagnostic.
    $ErrorActionPreference = "Continue"
    $output = & $powerShell -NoProfile -File (Join-Path $repository "tools/$Script") @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $output -notmatch $Expected) {
        throw "Expected '$Expected' from $Script, exit $LASTEXITCODE. Output: $output"
    }
}

try {
    New-Item -ItemType Directory -Path $testDirectory -Force | Out-Null
    $outputDirectory = Join-Path $testDirectory "packages"
    foreach ($script in @("PackageRuntime.ps1", "PackageEditor.ps1")) {
        Expect-PackagingFailure -Script $script -Arguments @(
            "-BuildDir", $BuildDir, "-OutputDir", $outputDirectory,
            "-Configuration", "NonExistentConfig_xyz"
        ) -Expected "Unsupported configuration"
    }
    Expect-PackagingFailure -Script "PackageSdk.ps1" -Arguments @(
        "-BuildDir", (Join-Path $testDirectory "missing-build"), "-OutputDir", $outputDirectory
    ) -Expected "cmake --install (Debug|Release) failed"

    foreach ($entry in @(
        @("PackageRuntime.ps1", "LuxPlayer-Portable-0.1.0-x64", "LuxPlayer.exe not found"),
        @("PackageEditor.ps1", "LuxEditor-Portable-0.1.0-x64", "LuxEditor.exe not found"),
        @("PackageSdk.ps1", "LuxEngine-SDK-0.1.0-x64", "cmake --install (Debug|Release) failed")
    )) {
        $publishedDir = Join-Path $outputDirectory $entry[1]
        $publishedZip = "$publishedDir.zip"
        New-Item -ItemType Directory -Path $publishedDir -Force | Out-Null
        $marker = Join-Path $publishedDir "previous.txt"
        [IO.File]::WriteAllText($marker, "previous contents")
        Compress-Archive -LiteralPath $marker -DestinationPath $publishedZip
        $hash = (Get-FileHash -LiteralPath $publishedZip).Hash
        Expect-PackagingFailure -Script $entry[0] -Arguments @(
            "-BuildDir", (Join-Path $testDirectory "missing-build"), "-OutputDir", $outputDirectory
        ) -Expected $entry[2]
        if ((Get-FileHash -LiteralPath $publishedZip).Hash -ne $hash -or
            [IO.File]::ReadAllText($marker) -ne "previous contents") {
            throw "Packaging failure changed a published artifact: $($entry[0])"
        }
    }
    . (Join-Path $repository "tools/WindowsRuntime.ps1")
    $first = New-LuxPackageOutput -OutputDir $outputDirectory -Name "Atomic-Package"
    [IO.File]::WriteAllText((Join-Path $first.Staging "file.txt"), "original")
    Compress-Archive -Path "$($first.Staging)/*" -DestinationPath $first.Zip
    Publish-LuxPackageOutput -Package $first
    $candidate = New-LuxPackageOutput -OutputDir $outputDirectory -Name "Atomic-Package"
    [IO.File]::WriteAllText((Join-Path $candidate.Staging "file.txt"), "replacement")
    Compress-Archive -Path "$($candidate.Staging)/*" -DestinationPath $candidate.Zip
    $originalHash = (Get-FileHash -LiteralPath $first.PublishedZip).Hash
    $locked = [IO.File]::Open($first.PublishedZip, 'Open', 'Read', 'Read')
    $replacementFailed = $false
    try {
        Publish-LuxPackageOutput -Package $candidate
    } catch {
        $replacementFailed = $true
    } finally {
        $locked.Dispose()
    }
    if (-not $replacementFailed -or
        (Get-FileHash -LiteralPath $first.PublishedZip).Hash -ne $originalHash -or
        [IO.File]::ReadAllText((Join-Path $first.PublishedStaging "file.txt")) -ne "original") {
        throw "A locked ZIP did not preserve both published artifacts."
    }
    Publish-LuxPackageOutput -Package $candidate
    if ((Get-FileHash -LiteralPath $first.PublishedZip).Hash -eq $originalHash -or
        [IO.File]::ReadAllText((Join-Path $first.PublishedStaging "file.txt")) -ne "replacement") {
        throw "A valid replacement did not publish both artifacts."
    }

    $mockBuild = Join-Path $testDirectory "build"
    $mockBin = Join-Path $mockBuild "bin/Release"
    New-Item -ItemType Directory -Path $mockBin -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $mockBin "LuxPlayer.exe"), "mock player")
    [System.IO.File]::WriteAllText((Join-Path $mockBin "LuxEditor.exe"), "mock editor")
    [System.IO.File]::WriteAllText((Join-Path $mockBin "ucrtbased.dll"), "mock Debug CRT")
    foreach ($script in @("PackageRuntime.ps1", "PackageEditor.ps1")) {
        Expect-PackagingFailure -Script $script -Arguments @(
            "-BuildDir", $mockBuild, "-OutputDir", $outputDirectory, "-Configuration", "Release"
        ) -Expected "Debug runtime DLLs detected"
    }
    Write-Host "Packaging configuration rejection, missing-build diagnostics and Debug CRT checks passed."
}
finally {
    if (Test-Path -LiteralPath $testDirectory) {
        Remove-Item -LiteralPath $testDirectory -Recurse -Force
    }
}

# GitHub's pwsh wrapper forwards LASTEXITCODE, including expected child failures.
exit 0
