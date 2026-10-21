#
# Copyright 2026 sendspin-cpp-cli Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Packages a Windows build as the release zip: one folder holding sendspin-cli.exe, its docs
# and BUILD-INFO.txt, asserted to hold exactly that.
#
# Usage: package_windows.ps1 -Binary <path to sendspin-cli.exe> [-Leg windows-x86_64] [-OutDir .]
#
# Prints the archive's name, and writes name= to $GITHUB_OUTPUT when that is set.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [string]$Leg = 'windows-x86_64',
    [string]$OutDir = '.'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "package_windows: FAIL: $Message"
    exit 1
}

$Root = Split-Path -Parent $PSScriptRoot
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$OutDir = (New-Item -ItemType Directory -Force -Path $OutDir).FullName

$reported = @(& $Binary --version)
$version = ($reported[0] -split '\s+')[1]
$libTag = ($reported[1] -split '\s+')[1]
if (-not $version -or -not $libTag) {
    Fail "'$Binary --version' did not report a version and a library tag: $($reported -join ' / ')"
}
$name = "sendspin-cli-$version-$Leg"

$stage = Join-Path $OutDir "stage\$name"
if (Test-Path -LiteralPath $stage) {
    Fail "$stage already exists; package into an empty directory"
}
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item -LiteralPath $Binary -Destination $stage
foreach ($doc in 'README.md', 'LICENSE', 'contributors.md', 'packaging\sendspin-cli.conf.example') {
    Copy-Item -LiteralPath (Join-Path $Root $doc) -Destination $stage
}

$commit = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { 'a local build' }
@"
sendspin-cli $version for $Leg
Built from commit $commit
Linked against sendspin-cpp $libTag

Runtime packages this binary needs:
  none. PortAudio is linked in, and DNS-SD is part of Windows 10 1809 and later.

The quickest way to install it is the guided installer, which verifies the release, installs
it for your user, and starts it when you log on:

  https://github.com/Sendspin/sendspin-cpp-cli/blob/main/scripts/get_started_windows.ps1

Or run it from where you unpacked it:

  .\$name\sendspin-cli.exe -l          (the devices it can play through)
  .\$name\sendspin-cli.exe             (play through the default device)

This binary is unsigned, so Windows SmartScreen may warn the first time it runs, and a zip
downloaded through a browser marks its files as coming from the internet. Check the zip
against SHA256SUMS first:

  Get-FileHash $name.zip -Algorithm SHA256

then unblock what you unpacked:

  Get-ChildItem .\$name | Unblock-File

A server finds the player over mDNS and then connects to it on port 8928, which Windows
Firewall blocks until a rule allows it. The guided installer offers to add one.
"@ | Set-Content -LiteralPath (Join-Path $stage 'BUILD-INFO.txt') -Encoding ascii

# Exactly these, so nothing stray ships.
$expected = @('BUILD-INFO.txt', 'LICENSE', 'README.md', 'contributors.md', 'sendspin-cli.conf.example',
    'sendspin-cli.exe') | Sort-Object
$actual = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object { $_.Name }) | Sort-Object
if (Compare-Object $expected $actual) {
    Fail "the staged payload holds '$($actual -join ', ')', not '$($expected -join ', ')'"
}

$zip = Join-Path $OutDir "$name.zip"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$stream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
$writer = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $stage -File) {
        # Entry names spelled with '/': Windows PowerShell's CreateFromDirectory() writes '\'.
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($writer, $file.FullName,
            "$name/$($file.Name)", [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

# Again from the zip itself, which is what ships.
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $inZip = @($archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName }) | Sort-Object
} finally {
    $archive.Dispose()
}
$wanted = @($expected | ForEach-Object { "$name/$_" }) | Sort-Object
if (Compare-Object $wanted $inZip) {
    Fail "the zip holds '$($inZip -join ', ')', not what was staged"
}

Write-Host $zip
if ($env:GITHUB_OUTPUT) {
    "name=$name" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
