# Shared Windows runtime contract for portable packages and SDK consumers.
function Get-LuxWindowsRuntimeFiles {
    @(
        "SDL3.dll",
        "LuxForge.dll",
        "amd_ags_x64.dll",
        "dxcompiler.dll",
        "WinPixEventRuntime.dll",
        "gpu.data",
        "gpu.cfg",
        "nethost.dll",
        "managed/LuxEngine.ScriptCore.dll",
        "managed/LuxEngine.ScriptCore.runtimeconfig.json",
        "DIRECT3D12/default.rootsig",
        "DIRECT3D12/compute.rootsig",
        "DIRECT3D12/lux2d.vert",
        "DIRECT3D12/lux2d.frag",
        "DIRECT3D12/luxmesh.vert",
        "DIRECT3D12/luxmesh.frag",
        "DIRECT3D12/luxscene.vert",
        "DIRECT3D12/luxscene.frag",
        "DIRECT3D12/luxshadow.vert",
        "DIRECT3D12/luxshadow.frag",
        "DIRECT3D12/luxfullscreen.vert",
        "DIRECT3D12/luxsky.frag",
        "DIRECT3D12/luxtone.frag"
    )
    foreach ($file in @(
        "shape-cube.fbx", "shape-cube-half.fbx", "shape-cube-rounded.fbx",
        "shape-cylinder.fbx", "shape-cylinder-detailed.fbx", "shape-cylinder-half.fbx",
        "shape-hexagon.fbx", "shape-slope.fbx", "shape-triangular-prism.fbx", "floor-square.fbx",
        "Textures/colormap.png", "License.txt", "Provenance.json"
    )) {
        "Resources/Models/Primitives/KenneyPrototype/$file"
    }
}

function Assert-LuxWindowsRuntime {
    param([Parameter(Mandatory = $true)][string]$Directory)

    foreach ($relativePath in (Get-LuxWindowsRuntimeFiles)) {
        $path = Join-Path $Directory $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
            throw "Packaging integrity failure: required runtime file missing or empty: $relativePath"
        }
    }
}

function Copy-LuxWindowsRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    Assert-LuxWindowsRuntime -Directory $Source
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    # Preserve transitive dependency DLLs and optional DXIL validation support as well.
    Get-ChildItem -LiteralPath $Source -Filter "*.dll" -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
    }
    foreach ($file in @("gpu.data", "gpu.cfg")) {
        Copy-Item -LiteralPath (Join-Path $Source $file) -Destination $Destination -Force
    }
    foreach ($directory in @("DIRECT3D12", "managed", "Resources")) {
        Copy-Item -LiteralPath (Join-Path $Source $directory) -Destination $Destination -Recurse -Force
    }
    Assert-LuxWindowsRuntime -Directory $Destination
}

function New-LuxPackageOutput {
    param([string]$OutputDir, [string]$Name)

    if ($Name -notmatch '\A[A-Za-z0-9][A-Za-z0-9._+-]*\z') {
        throw "Invalid package name: $Name"
    }
    $directory = [IO.Path]::GetFullPath((Join-Path $OutputDir '.'))
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $suffix = [guid]::NewGuid().ToString('N')
    $package = @{
        Directory = $directory
        Staging = Join-Path $directory "$Name.writing-$suffix"
        Zip = Join-Path $directory "$Name.writing-$suffix.zip"
        PublishedStaging = Join-Path $directory $Name
        PublishedZip = Join-Path $directory "$Name.zip"
        PreviousStaging = Join-Path $directory "$Name.previous-$suffix"
        Lock = Join-Path $directory ".$Name.lock"
    }
    [IO.Directory]::CreateDirectory($package.Staging) | Out-Null
    return $package
}

function Compress-LuxPackageOutput {
    param([hashtable]$Package)

    [IO.Compression.ZipFile]::CreateFromDirectory(
        $Package.Staging, $Package.Zip, [IO.Compression.CompressionLevel]::Optimal, $false)
}

function Remove-LuxPackageDirectory {
    param([string]$Path, [string]$OutputDir)

    if (-not (Test-Path -LiteralPath $Path)) { return }
    $entry = Get-Item -LiteralPath $Path -Force
    if ([IO.Path]::GetDirectoryName($entry.FullName) -ne $OutputDir -or
        ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to remove package directory: $Path"
    }
    Remove-Item -LiteralPath $entry.FullName -Recurse -Force
}

function Publish-LuxPackageOutput {
    param([hashtable]$Package)

    $lock = [IO.File]::Open($Package.Lock, 'OpenOrCreate', 'ReadWrite', 'None')
    $oldMoved = $false
    $newMoved = $false
    try {
        if (Test-Path -LiteralPath $Package.PublishedZip) {
            $existingZip = Get-Item -LiteralPath $Package.PublishedZip -Force
            if ($existingZip.PSIsContainer -or
                ($existingZip.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Cannot replace package ZIP: $($Package.PublishedZip)"
            }
        }
        if (Test-Path -LiteralPath $Package.PublishedStaging) {
            $old = Get-Item -LiteralPath $Package.PublishedStaging -Force
            if (-not $old.PSIsContainer -or ($old.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Cannot replace package directory: $($Package.PublishedStaging)"
            }
            [IO.Directory]::Move($Package.PublishedStaging, $Package.PreviousStaging)
            $oldMoved = $true
        }
        [IO.Directory]::Move($Package.Staging, $Package.PublishedStaging)
        $newMoved = $true
        if ([IO.File]::Exists($Package.PublishedZip)) {
            [IO.File]::Replace($Package.Zip, $Package.PublishedZip, [NullString]::Value)
        } else {
            [IO.File]::Move($Package.Zip, $Package.PublishedZip)
        }
    } catch {
        if ($newMoved) {
            [IO.Directory]::Move($Package.PublishedStaging, $Package.Staging)
        }
        if ($oldMoved) {
            [IO.Directory]::Move($Package.PreviousStaging, $Package.PublishedStaging)
        }
        throw
    } finally {
        $lock.Dispose()
    }
    if ($oldMoved) {
        try { Remove-LuxPackageDirectory -Path $Package.PreviousStaging -OutputDir $Package.Directory }
        catch { Write-Warning "Previous package directory retained: $($Package.PreviousStaging)" }
    }
}

function Remove-LuxPackageWork {
    param([hashtable]$Package)

    Remove-LuxPackageDirectory -Path $Package.Staging -OutputDir $Package.Directory
    if ([IO.File]::Exists($Package.Zip)) {
        [IO.File]::Delete($Package.Zip)
    }
}
