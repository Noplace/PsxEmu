# Fetches NVIDIA's Streamline SDK - DLSS's headers, and its DLLs - into Temp\streamline\, which git
# ignores (Docs/DLSS-Plan.md, phase 4). Run from anywhere; running it again does nothing.
#
# The DLLs are NVIDIA's, under NVIDIA's own licence (the SDK's license.txt): they may ship inside
# the application, with NVIDIA's marks in its About box, but not under this repository's MIT
# licence - so they are fetched, never committed. The build copies the ones it needs beside the
# executable.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest's progress bar makes it crawl

$Version = '2.14.1'
$Name = "streamline-sdk-v$Version.zip"
$Url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$Version/$Name"
$Size = 275994000   # as GitHub's release lists it

$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$Folder = Join-Path $Root 'Temp\streamline'
$Zip = Join-Path $Folder $Name
$Sdk = Join-Path $Folder "v$Version"

if (Test-Path -LiteralPath (Join-Path $Sdk 'include\sl.h')) {
    "Streamline $Version is already in $Sdk"
    exit 0
}
New-Item -ItemType Directory -Force $Folder | Out-Null

if (-not (Test-Path -LiteralPath $Zip) -or (Get-Item -LiteralPath $Zip).Length -ne $Size) {
    "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing
}
$Got = (Get-Item -LiteralPath $Zip).Length
if ($Got -ne $Size) {
    throw "$Name is $Got bytes, not the $Size GitHub lists - not unpacked"
}

"Unpacking into $Sdk"
if (Test-Path -LiteralPath $Sdk) { Remove-Item -Recurse -Force -LiteralPath $Sdk }
Expand-Archive -LiteralPath $Zip -DestinationPath $Sdk
# Some releases put everything one folder down; bring it up so include\ is at the top.
$Inner = Get-ChildItem -LiteralPath $Sdk -Directory
if ($Inner.Count -eq 1 -and -not (Test-Path -LiteralPath (Join-Path $Sdk 'include'))) {
    Get-ChildItem -LiteralPath $Inner[0].FullName | Move-Item -Destination $Sdk
    Remove-Item -LiteralPath $Inner[0].FullName
}
if (-not (Test-Path -LiteralPath (Join-Path $Sdk 'include\sl.h'))) {
    throw "unpacked, but no include\sl.h in $Sdk"
}
"Streamline $Version is in $Sdk"
