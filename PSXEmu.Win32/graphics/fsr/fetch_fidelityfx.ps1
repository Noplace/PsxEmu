# Fetches AMD's signed FidelityFX DLLs - FSR Upscaling and FSR Frame Generation, and the loader that
# finds them - into Temp\fidelityfx\, which git ignores (Docs/FSR-Plan.md, phase 0). Run from
# anywhere; running it again does nothing.
#
# The DLLs are AMD's, under the SDK's MIT licence (Kits\FidelityFX\docs\license.md lists them), so
# they could be committed; at 69 MB they are fetched instead, as NVIDIA's are, and the build copies
# them beside the executable. Each is checked against the git blob id the release's tree gives it,
# so a file changed on the way is never unpacked into the build.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest's progress bar makes it crawl

$Version = '2.3.0'
$Base = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v$Version/Kits/FidelityFX/signedbin"
# Name, size and git blob id, as the v2.3.0 tree lists them.
$Files = @(
    @{ Name = 'amd_fidelityfx_loader_dx12.dll';          Size = 26376;    Blob = '144916ba922c2a66149ad5cb1037a4f86d2b6491' },
    @{ Name = 'amd_fidelityfx_upscaler_dx12.dll';        Size = 28761864; Blob = '199de3a500a1d153b7e73fa5ca0adbd2a4ac1229' },
    @{ Name = 'amd_fidelityfx_framegeneration_dx12.dll'; Size = 40085776; Blob = '1a06b72761086cb5561f8984ce0dc6a24a370e9e' }
)

$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$Folder = Join-Path $Root "Temp\fidelityfx\v$Version"

# Git's id for a file: SHA-1 over "blob <size>\0" and the bytes.
function Get-BlobId([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha = [System.Security.Cryptography.SHA1]::Create()
    $sha.TransformBlock($header, 0, $header.Length, $null, 0) | Out-Null
    $sha.TransformFinalBlock($bytes, 0, $bytes.Length) | Out-Null
    -join ($sha.Hash | ForEach-Object { $_.ToString('x2') })
}

New-Item -ItemType Directory -Force $Folder | Out-Null
foreach ($File in $Files) {
    $Path = Join-Path $Folder $File.Name
    if ((Test-Path -LiteralPath $Path) -and (Get-BlobId $Path) -eq $File.Blob) {
        "$($File.Name) is already in $Folder"
        continue
    }
    "Downloading $($File.Name) ($($File.Size) bytes)"
    $Part = "$Path.part"
    Invoke-WebRequest -Uri "$Base/$($File.Name)" -OutFile $Part -UseBasicParsing
    $Got = (Get-Item -LiteralPath $Part).Length
    if ($Got -ne $File.Size) {
        Remove-Item -LiteralPath $Part
        throw "$($File.Name) is $Got bytes, not the $($File.Size) the release lists"
    }
    $Id = Get-BlobId $Part
    if ($Id -ne $File.Blob) {
        Remove-Item -LiteralPath $Part
        throw "$($File.Name) is not the release's file (blob $Id, not $($File.Blob))"
    }
    Move-Item -Force -LiteralPath $Part -Destination $Path
}
"AMD FidelityFX $Version is in $Folder"
