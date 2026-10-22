# Fetches ONNX Runtime with its DirectML execution provider, and DirectML itself, from nuget.org into
# Temp\onnxruntime\, which git ignores (Docs/Texture-Upscaling-Plan.md, phase 0). Run from
# anywhere; running it again does nothing.
#
# Three DLLs are kept from the two packages: onnxruntime.dll and onnxruntime_providers_shared.dll
# (MIT) and DirectML.dll (Microsoft's DirectML licence, which allows shipping it inside a Windows
# application or game). At 36 MB they are fetched rather than committed, as AMD's and NVIDIA's
# are, and build_tools.bat copies them beside the tools. Each package is checked against the
# SHA-256 it had when phase 0 read it, and each DLL against Microsoft's Authenticode signature.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest's progress bar makes it crawl

$Base = 'https://api.nuget.org/v3-flatcontainer'
$Packages = @(
    @{ Id = 'microsoft.ml.onnxruntime.directml'; Version = '1.24.4'; Size = 12458649;
       Sha256 = '57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681';
       Files = @('runtimes/win-x64/native/onnxruntime.dll',
                 'runtimes/win-x64/native/onnxruntime_providers_shared.dll',
                 'LICENSE', 'ThirdPartyNotices.txt') },
    @{ Id = 'microsoft.ai.directml'; Version = '1.15.4'; Size = 202292617;
       Sha256 = '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9';
       Files = @('bin/x64-win/DirectML.dll', 'LICENSE.txt') }
)
# What each kept file is called in the folder: the two licences would otherwise collide.
$Rename = @{ 'LICENSE' = 'onnxruntime.license.txt'; 'ThirdPartyNotices.txt' = 'onnxruntime.notices.txt';
             'LICENSE.txt' = 'directml.license.txt' }

# The repository that has EmuLib as its submodule, where the build looks for Temp\ (Emu.Frontend.Win32.props);
# EmuLib's own when it stands alone.
$Root = $null
try { $Root = git -C $PSScriptRoot rev-parse --show-superproject-working-tree 2>$null } catch { }
if (-not $Root) { $Root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..') }
$Folder = Join-Path $Root 'Temp\onnxruntime\1.24.4'
$Dlls = @('onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll')

function Test-Signed([string]$Path) {
    $Signature = Get-AuthenticodeSignature -LiteralPath $Path
    $Signature.Status -eq 'Valid' -and $Signature.SignerCertificate.Subject -match 'O=Microsoft Corporation'
}

$Have = $true
foreach ($Dll in $Dlls) {
    $Path = Join-Path $Folder $Dll
    if (-not (Test-Path -LiteralPath $Path) -or -not (Test-Signed $Path)) { $Have = $false }
}
if ($Have) {
    "ONNX Runtime 1.24.4 and DirectML 1.15.4 are already in $Folder"
    exit 0
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force $Folder | Out-Null
foreach ($Package in $Packages) {
    $Name = "$($Package.Id).$($Package.Version).nupkg"
    $Part = Join-Path $Folder "$Name.part"
    "Downloading $Name ($($Package.Size) bytes)"
    Invoke-WebRequest -Uri "$Base/$($Package.Id)/$($Package.Version)/$Name" -OutFile $Part -UseBasicParsing
    $Got = (Get-Item -LiteralPath $Part).Length
    $Hash = (Get-FileHash -LiteralPath $Part -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Got -ne $Package.Size -or $Hash -ne $Package.Sha256) {
        Remove-Item -LiteralPath $Part
        throw "$Name is not the package phase 0 read ($Got bytes, SHA-256 $Hash)"
    }
    $Zip = [System.IO.Compression.ZipFile]::OpenRead($Part)
    try {
        foreach ($Wanted in $Package.Files) {
            $Entry = $Zip.Entries | Where-Object { $_.FullName -eq $Wanted }
            if ($null -eq $Entry) { throw "$Name has no $Wanted" }
            $Leaf = Split-Path $Wanted -Leaf
            if ($Rename.ContainsKey($Leaf)) { $Leaf = $Rename[$Leaf] }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, (Join-Path $Folder $Leaf), $true)
        }
    } finally {
        $Zip.Dispose()
        Remove-Item -LiteralPath $Part
    }
}
foreach ($Dll in $Dlls) {
    if (-not (Test-Signed (Join-Path $Folder $Dll))) { throw "$Dll is not signed by Microsoft" }
}
"ONNX Runtime 1.24.4 and DirectML 1.15.4 are in $Folder"
