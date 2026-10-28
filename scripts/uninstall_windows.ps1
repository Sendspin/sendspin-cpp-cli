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
# Removes what get_started_windows.ps1 installed: the scheduled task, the running player, the
# install folder, its PATH entry and the firewall rule. Config and remembered state are kept
# unless -Purge. Shows what it will do and asks first, unless -Yes.
#
# Usage: uninstall_windows.ps1 [-Purge] [-Yes]
#
#   -Purge  also remove what is otherwise kept: the config in AppData\Roaming\sendspin-cli, and
#           the log and remembered state (identity, pairings, volume) in AppData\Local\sendspin-cli
#   -Yes    do not ask anything: do it as shown, keeping config and state unless -Purge.
#           Required when there is no console to ask on. Removing the firewall rule still
#           brings up a UAC prompt

[CmdletBinding()]
param(
    [switch]$Purge,
    [switch]$Yes,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'

# Keep in step with get_started_windows.ps1.
$TaskName = 'sendspin-cli'
$FirewallRule = 'sendspin-cli'

function Fail([string]$Message) {
    Write-Host "uninstall_windows: FAIL: $Message" -ForegroundColor Red
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

# Players running from the installed binary, not some other copy.
function Get-RunningPlayers {
    return @(Get-CimInstance Win32_Process -Filter "Name = 'sendspin-cli.exe'" |
            Where-Object { $_.ExecutablePath -eq $Binary })
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

if ([Environment]::OSVersion.Platform -ne 'Win32NT') {
    Fail 'Windows only. On Linux use scripts/uninstall_linux.sh'
}

$LocalAppData = [Environment]::GetFolderPath('LocalApplicationData')
$RoamingAppData = [Environment]::GetFolderPath('ApplicationData')
$InstallDir = Join-Path $LocalAppData 'Programs\sendspin-cli'
$Binary = Join-Path $InstallDir 'sendspin-cli.exe'
$ConfigDir = Join-Path $RoamingAppData 'sendspin-cli'
$DataDir = Join-Path $LocalAppData 'sendspin-cli'

# What is here

$Task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
$Running = @(Get-RunningPlayers)
$HaveInstall = Test-Path -LiteralPath $InstallDir
$UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$OnPath = ($UserPath -split ';') -contains $InstallDir
$HaveRule = $false
try {
    $HaveRule = [bool](Get-NetFirewallRule -DisplayName $FirewallRule -ErrorAction Stop)
} catch {
    $HaveRule = $false
}
$Kept = @($ConfigDir, $DataDir | Where-Object { Test-Path -LiteralPath $_ })
# What players leave per control port, e.g. sendspin-cli-8928.sock.lock.
$Leftovers = @(Get-ChildItem -LiteralPath $LocalAppData -Filter 'sendspin-cli-*.sock*' -Force -ErrorAction SilentlyContinue)

if (-not $Purge -and -not $Yes -and $Kept -and (Test-CanAsk)) {
    Step 'Your config and what the player remembered'
    Say ''
    $Kept | ForEach-Object { Say "  $_" }
    Say ''
    Say "  These are kept by default, so installing again keeps the player's identity and its"
    Say '  pairings. Without them, a server sees a new, unpaired player.'
    Say ''
    $Purge = Confirm-Action '  Remove them too?'
}

$Plan = @()
if ($Task) {
    $Plan += "task      unregister '$TaskName' from Task Scheduler"
}
foreach ($process in $Running) {
    $Plan += "stop      the running player (pid $($process.ProcessId))"
}
if ($HaveInstall) {
    $Plan += "remove    $InstallDir\"
}
if ($OnPath) {
    $Plan += "PATH      remove $InstallDir from your user PATH"
}
if ($HaveRule) {
    $Plan += "firewall  remove the '$FirewallRule' rule (Windows asks for administrator approval)"
}
if ($Purge) {
    $Kept | ForEach-Object { $Plan += "remove    $_\" }
}
if ($Leftovers) {
    $Plan += "tidy      $($Leftovers.Count) control-socket file(s) players left in $LocalAppData"
}

if (-not $Plan) {
    Say 'sendspin-cli is not installed here; there is nothing to remove.'
    if ($Kept) {
        Say "Left in place (-Purge removes them): $($Kept -join ', ')"
    }
    exit 0
}

Step 'This is what it will do'
Say ''
$Plan | ForEach-Object { Say "  $_" }
Say ''
if (-not $Purge -and $Kept) {
    Say "Kept: $($Kept -join ', ')"
    Say '-Purge removes those as well.'
}

if (-not $Yes) {
    if (-not (Test-CanAsk)) {
        Fail 'there is no console to confirm that with. Re-run with -Yes if you have read it and want it done; nothing was removed'
    }
    if (-not (Confirm-Action "`nGo ahead?")) {
        Fail 'nothing was removed'
    }
}

Step 'Removing'
if ($Task) {
    # Disabled first, so its once-a-minute check cannot start the player again.
    Disable-ScheduledTask -TaskName $TaskName | Out-Null
}
# Asked again, not taken from the plan: the task may have started one while it was on screen.
foreach ($process in (Get-RunningPlayers)) {
    Say "  stopping the running player (pid $($process.ProcessId))"
    # Without /F: its close handler says goodbye to the server first.
    $target = $process.ProcessId
    Invoke-Native { & taskkill.exe /PID $target } | Out-Null
    Wait-Process -Id $target -Timeout 10 -ErrorAction SilentlyContinue
    Stop-Process -Id $target -Force -ErrorAction SilentlyContinue
}
if ($Task) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Say "  task '$TaskName' unregistered"
}
if ($OnPath) {
    $remaining = ($UserPath -split ';' | Where-Object { $_ -and $_ -ne $InstallDir }) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $remaining, 'User')
    Say "  PATH: removed $InstallDir"
}
if ($HaveInstall) {
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
    Say "  removed $InstallDir"
}
if ($HaveRule) {
    $command = "Remove-NetFirewallRule -DisplayName '$FirewallRule'"
    try {
        $elevated = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru `
            -ArgumentList '-NoProfile', '-NonInteractive', '-Command', $command
        if ($elevated.ExitCode -eq 0) {
            Say "  removed the '$FirewallRule' firewall rule"
        } else {
            Say "  The rule was not removed (exit $($elevated.ExitCode)); run this as administrator:"
            Say "    $command"
        }
    } catch {
        Say '  Administrator approval was declined, so the firewall rule is still there. Run this'
        Say '  from an administrator PowerShell to remove it:'
        Say "    $command"
    }
}
if ($Purge) {
    foreach ($path in $Kept) {
        Remove-Item -LiteralPath $path -Recurse -Force
        Say "  removed $path"
    }
}
if ($Leftovers) {
    # A player still running, from anywhere, may be using one of them.
    if (Get-Process -Name 'sendspin-cli' -ErrorAction SilentlyContinue) {
        Say "  another sendspin-cli is still running, so the control-socket files in $LocalAppData"
        Say '  are left; run this again once it has stopped'
    } else {
        $Leftovers | Remove-Item -Force -ErrorAction SilentlyContinue
        Say "  tidied the control-socket files in $LocalAppData"
    }
}
Say ''
Say 'Done.'
