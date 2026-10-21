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
# Installs a released sendspin-cli for this Windows user and starts it at every logon.
# Verifies SHA256SUMS, installs under LocalAppData, writes a config and registers a scheduled
# task. Nothing needs administrator rights except the optional firewall rule, asked for apart.
#
# Usage: get_started_windows.ps1 [-Version <tag>] [-Yes] [-Firewall]
#
#   -Version <tag>  install this release instead of the latest, e.g. -Version v0.5.0
#   -Yes            do not ask before installing. Required when there is no console to ask on
#   -Firewall       also add the firewall rule without asking (still needs a UAC prompt)
#
# Environment:
#
#   SENDSPIN_CLI_ZIP  install this zip instead of downloading a release. The escape hatch for a
#                     locally built archive, and the only way to exercise this script before a
#                     release exists. It is NOT checksummed, and the script says so loudly.

[CmdletBinding()]
param(
    [string]$Version = '',
    [switch]$Yes,
    [switch]$Firewall,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1's download progress bar slows Invoke-WebRequest to a crawl.
$ProgressPreference = 'SilentlyContinue'

# Fixed so this script can never install someone else's binary.
$Repo = 'Sendspin/sendspin-cpp-cli'
$Leg = 'windows-x86_64'
$TaskName = 'sendspin-cli'
$FirewallRule = 'sendspin-cli'
# DNS-SD and AF_UNIX both arrived by Windows 10 1809.
$MinimumBuild = 17763

function Fail([string]$Message) {
    Write-Host "get_started_windows: FAIL: $Message" -ForegroundColor Red
    exit 1
}

function Say([string]$Message = '') {
    Write-Host $Message
}

function Step([string]$Message) {
    Write-Host ''
    Write-Host "==> $Message"
}

# Runs a native command, discarding its stderr. Windows PowerShell turns stderr into error
# records, which the script's 'Stop' preference would throw on.
function Invoke-Native([scriptblock]$Command) {
    $ErrorActionPreference = 'Continue'
    & $Command 2>$null
}

function Test-CanAsk {
    return [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
}

function Confirm-Action([string]$Question) {
    $answer = Read-Host "$Question [y/N]"
    return $answer -match '^(y|yes)$'
}

if ($Help) {
    # The header's usage block, from '# Usage:' to the end of the comment.
    $inUsage = $false
    foreach ($line in Get-Content -LiteralPath $PSCommandPath) {
        if ($line -match '^# Usage:') {
            $inUsage = $true
        }
        if ($inUsage) {
            if ($line -notmatch '^#') {
                break
            }
            Write-Host ($line -replace '^# ?', '')
        }
    }
    exit 0
}

# Is this a host this can work on at all

if ([Environment]::OSVersion.Platform -ne 'Win32NT') {
    Fail "Windows only. On Linux use scripts/get_started_linux.sh; on macOS take the .pkg from https://github.com/$Repo/releases"
}
$Build = [Environment]::OSVersion.Version.Build
if ($Build -lt $MinimumBuild) {
    Fail "this is Windows build $Build, and sendspin-cli needs $MinimumBuild (Windows 10 1809) or later for its control socket and mDNS"
}
# x64 builds also run on Windows on ARM, through its x64 emulation.
$Arch = $env:PROCESSOR_ARCHITECTURE
if ($Arch -ne 'AMD64' -and $Arch -ne 'ARM64') {
    Fail "no release is built for a '$Arch' Windows -- the archive is $Leg. Build from source instead: https://github.com/$Repo#build"
}

$LocalAppData = [Environment]::GetFolderPath('LocalApplicationData')
$RoamingAppData = [Environment]::GetFolderPath('ApplicationData')
$InstallDir = Join-Path $LocalAppData 'Programs\sendspin-cli'
$Binary = Join-Path $InstallDir 'sendspin-cli.exe'
$ConfigDir = Join-Path $RoamingAppData 'sendspin-cli'
$Config = Join-Path $ConfigDir 'config'
$LogDir = Join-Path $LocalAppData 'sendspin-cli'
$LogFile = Join-Path $LogDir 'sendspin-cli.log'
$PidFile = Join-Path $LogDir 'sendspin-cli.pid'
$User = [Security.Principal.WindowsIdentity]::GetCurrent().Name

$WorkDir = Join-Path ([IO.Path]::GetTempPath()) ("sendspin-cli-setup-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $WorkDir | Out-Null

try {
    Say 'sendspin-cli getting started'
    Say "  host:         Windows build $Build, $Arch$(if ($Arch -eq 'ARM64') { ' (running the x64 build under emulation)' })"
    Say "  release leg:  $Leg"
    Say "  user:         $User"

    # The payload

    if ($env:SENDSPIN_CLI_ZIP) {
        Step 'Using the payload you supplied'
        if (-not (Test-Path -LiteralPath $env:SENDSPIN_CLI_ZIP -PathType Leaf)) {
            Fail "SENDSPIN_CLI_ZIP names '$env:SENDSPIN_CLI_ZIP', which is not a file"
        }
        $Zip = (Resolve-Path -LiteralPath $env:SENDSPIN_CLI_ZIP).Path
        Say "  $Zip"
        Say ''
        Say '  !! NOT VERIFIED. This is your own archive, so there is no published SHA256SUMS to'
        Say '  !! check it against, and nothing here has established that it is what you think.'
        Say '  !! Take a release instead of setting SENDSPIN_CLI_ZIP to get that check.'
    } else {
        Step 'Finding the release to install'
        # Windows PowerShell 5.1 may not offer TLS 1.2 by default.
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor
            [Net.SecurityProtocolType]::Tls12

        if ($Version) {
            $Tag = $Version
        } else {
            try {
                $latest = Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$Repo/releases/latest"
            } catch {
                $status = $_.Exception.Response.StatusCode.value__
                if ($status -eq 404) {
                    Fail "$Repo has published no releases yet, so there is nothing to download. Build from source -- https://github.com/$Repo#build -- or set SENDSPIN_CLI_ZIP to a zip you built"
                }
                Fail "could not ask github.com for the latest release of $Repo -- check this machine's network and try again ($($_.Exception.Message))"
            }
            $Tag = $latest.tag_name
        }
        # The archives are named for the version, which is the tag without its 'v'.
        $Archive = "sendspin-cli-$($Tag.TrimStart('v'))-$Leg.zip"
        Say "  $Tag  ->  $Archive"

        Step 'Downloading and verifying'
        $Base = "https://github.com/$Repo/releases/download/$Tag"
        $Zip = Join-Path $WorkDir $Archive
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$Base/$Archive" -OutFile $Zip
        } catch {
            Fail "no $Archive in release $Tag of $Repo. Check the tag exists and carries a Windows build: https://github.com/$Repo/releases"
        }
        $SumsFile = Join-Path $WorkDir 'SHA256SUMS'
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$Base/SHA256SUMS" -OutFile $SumsFile
        } catch {
            Fail "$Tag carries $Archive but no SHA256SUMS, so there is nothing to verify it against. Refusing to install an unverified binary"
        }

        # A SHA256SUMS that omits this archive verifies nothing, so check it is listed.
        $listed = Get-Content $SumsFile | Where-Object { ($_ -split '\s+', 2)[1] -eq $Archive } |
            Select-Object -First 1
        if (-not $listed) {
            Fail "the SHA256SUMS published with $Tag does not list $Archive, so there is no checksum to verify it against. Nothing has been installed"
        }
        $expected = ($listed -split '\s+')[0].ToLowerInvariant()
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            Fail "$Archive does not match the checksum $Tag publishes for it. Nothing has been installed. Download it again; if it fails a second time, say so on the issue tracker rather than installing it anyway"
        }
        Say "  ${Archive}: SHA256 matches SHA256SUMS"
    }

    # What is in it

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $entries = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $names = @($entries.Entries | ForEach-Object { $_.FullName })
    } finally {
        $entries.Dispose()
    }
    $roots = @($names | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique)
    if ($roots.Count -ne 1) {
        Fail "'$Zip' has $($roots.Count) top-level entries, and a payload has one folder holding sendspin-cli.exe"
    }
    $PayloadRoot = $roots[0]
    if ($names -notcontains "$PayloadRoot/sendspin-cli.exe") {
        Fail "'$Zip' holds no $PayloadRoot/sendspin-cli.exe -- a Linux or macOS archive would look exactly like this. Take the $Leg one"
    }

    # The plan

    $SeedConfig = -not (Test-Path -LiteralPath $Config)
    $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $AddToPath = -not (($UserPath -split ';') -contains $InstallDir)
    $HaveRule = $false
    try {
        $HaveRule = [bool](Get-NetFirewallRule -DisplayName $FirewallRule -ErrorAction Stop)
    } catch {
        $HaveRule = $false
    }

    Step 'This is what it will do, as you, with no administrator rights'
    Say ''
    Say "  install   $InstallDir\  (sendspin-cli.exe and its README, LICENSE and config example)"
    if ($SeedConfig) {
        Say "  config    $Config  (from the example; every line in it is commented out)"
    } else {
        Say "  config    $Config  (exists; left as it is)"
    }
    if ($AddToPath) {
        Say "  PATH      add $InstallDir to your user PATH, so 'sendspin-cli status' works anywhere"
    }
    Say "  task      '$TaskName' in Task Scheduler: start the player, without a window, when you"
    Say "            log on, logging to $LogFile; it checks every minute"
    Say '            and starts the player again if it has stopped'
    Say '  start     the task, now'
    Say ''
    Say 'A running player is stopped first, so re-running this upgrades it.'
    Say 'With nothing in the config, it plays through your default output device, follows it when'
    Say 'you change it, and advertises itself so a Sendspin server on this network can find it.'

    if (-not $Yes) {
        if (-not (Test-CanAsk)) {
            Fail 'there is no console to confirm that with. Re-run with -Yes if you have read it and want it done'
        }
        if (-not (Confirm-Action "`nGo ahead?")) {
            Fail 'nothing was installed'
        }
    }

    # Install

    Step 'Installing'
    # Disabled first, so its once-a-minute check cannot start the old binary mid-copy.
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Disable-ScheduledTask -TaskName $TaskName | Out-Null
    }
    # Any player run from the install folder holds the binary open.
    Get-CimInstance Win32_Process -Filter "Name = 'sendspin-cli.exe'" |
        Where-Object { $_.ExecutablePath -eq $Binary } |
        ForEach-Object {
            Say "  stopping the running player (pid $($_.ProcessId))"
            # Without /F: its close handler says goodbye to the server first.
            $target = $_.ProcessId
            Invoke-Native { & taskkill.exe /PID $target } | Out-Null
            Wait-Process -Id $_.ProcessId -Timeout 10 -ErrorAction SilentlyContinue
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }

    $Unpacked = Join-Path $WorkDir 'unpacked'
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Unpacked)
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Path (Join-Path (Join-Path $Unpacked $PayloadRoot) '*') -Destination $InstallDir -Recurse -Force
    # Downloaded files carry the internet zone mark; this one was verified above.
    Get-ChildItem -LiteralPath $InstallDir -File | Unblock-File
    Say "  $Binary"
    & $Binary --version | ForEach-Object { Say "  $_" }

    if ($SeedConfig) {
        New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
        Copy-Item -LiteralPath (Join-Path $InstallDir 'sendspin-cli.conf.example') -Destination $Config
        Say "  $Config  (from the example; everything in it is commented out)"
    }

    if ($AddToPath) {
        $newPath = if ($UserPath) { "$UserPath;$InstallDir" } else { $InstallDir }
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
        Say "  PATH: added $InstallDir (new terminals pick it up)"
    }

    # The task

    Step 'Starting it at logon'
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    # -z detaches into a hidden console of its own, which taskkill /PID stops cleanly; the
    # headless conhost spares the short-lived parent a window. While a player holds the
    # pidfile, the minute's attempt refuses before opening the log and exits.
    $action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\conhost.exe" `
        -Argument "--headless `"$Binary`" -z -P `"$PidFile`" -f `"$LogFile`"" -WorkingDirectory $InstallDir
    $atLogon = New-ScheduledTaskTrigger -AtLogOn -User $User
    # A logon trigger's own repetition only starts at the next logon, so a second trigger checks.
    $everyMinute = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(10) `
        -RepetitionInterval (New-TimeSpan -Minutes 1)
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $atLogon, $everyMinute `
        -Settings $settings -Principal $principal -Description 'Sendspin audio player (sendspin-cli)' `
        -Force | Out-Null
    Say "  task: '$TaskName' starts the player when $User logs on, and again within a minute if it stops"

    Start-ScheduledTask -TaskName $TaskName
    Start-Sleep -Seconds 4
    $running = Get-CimInstance Win32_Process -Filter "Name = 'sendspin-cli.exe'" |
        Where-Object { $_.ExecutablePath -eq $Binary }
    if ($running) {
        Step 'sendspin-cli is running'
        if (Test-Path -LiteralPath $LogFile) {
            Get-Content -LiteralPath $LogFile -Tail 4 | ForEach-Object { Say "  $_" }
        }
    } else {
        Step 'sendspin-cli was started and is NOT running'
        Say ''
        Say '  What it said:'
        if (Test-Path -LiteralPath $LogFile) {
            Get-Content -LiteralPath $LogFile -Tail 15 | ForEach-Object { Say "    $_" }
        } else {
            Say "    (no log at $LogFile)"
        }
        Say ''
        Say "  '$Binary -l' lists the devices this machine has."
    }

    # The firewall

    Step 'Letting a server reach it'
    if ($HaveRule) {
        Say "  The '$FirewallRule' firewall rule is already there."
    } else {
        Say '  A server finds the player over mDNS and then connects to it on port 8928. Windows'
        Say '  Firewall blocks that connection until a rule allows it, and adding one needs'
        Say '  administrator rights. The rule this adds lets sendspin-cli.exe accept connections'
        Say '  on private networks only:'
        Say ''
        Say "    New-NetFirewallRule -DisplayName $FirewallRule -Direction Inbound -Action Allow -Program '$Binary' -Protocol TCP -Profile Private"
        Say ''
        $addRule = $Firewall
        if (-not $addRule -and -not $Yes -and (Test-CanAsk)) {
            $addRule = Confirm-Action 'Add it now? Windows asks for administrator approval'
        }
        if ($addRule) {
            $command = "New-NetFirewallRule -DisplayName '$FirewallRule' -Direction Inbound -Action Allow " +
                "-Program '$Binary' -Protocol TCP -Profile Private | Out-Null"
            try {
                $elevated = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru `
                    -ArgumentList '-NoProfile', '-NonInteractive', '-Command', $command
                if ($elevated.ExitCode -eq 0) {
                    Say "  Added the '$FirewallRule' rule."
                } else {
                    Say "  The rule was not added (exit $($elevated.ExitCode)); run the command above as administrator."
                }
            } catch {
                Say '  Administrator approval was declined, so the rule was not added. Run the command above'
                Say '  from an administrator PowerShell when you want it; until then, dial a server'
                Say "  yourself with 'server = <host>' in the config."
            }
        } else {
            Say '  Not added. Until it is, a server cannot dial in: dial out instead with'
            Say "  'server = <host>' in the config, or run the command above as administrator."
        }
    }

    # What to do next

    Step 'Next'
    Say ''
    Say '  Ask it what it is doing (in a new terminal, so PATH is up to date):'
    Say ''
    Say '    sendspin-cli status'
    Say ''
    Say '  Pair it from your Sendspin server: when the server asks for a code, it is in the log.'
    Say ''
    Say "    Get-Content '$LogFile' -Wait -Tail 20"
    Say ''
    Say "  Change what it plays through, or anything else, in $Config --"
    Say "  'sendspin-cli -l' lists the devices -- then stop it, and the task starts it again"
    Say '  within a minute with the new settings:'
    Say ''
    Say "    taskkill /PID (Get-Content '$PidFile')"
    Say ''
    Say '  To keep it stopped, disable the task first:'
    Say ''
    Say "    Disable-ScheduledTask $TaskName; taskkill /PID (Get-Content '$PidFile')"
    Say ''
} finally {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}
