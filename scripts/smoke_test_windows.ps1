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
# smoke_test.sh's ground on Windows, where its kill/wait handling does not apply: --version,
# the control socket round trip, a clean stop on taskkill, and -z with its pidfile.
#
# Usage: smoke_test_windows.ps1 -Binary <path to sendspin-cli.exe>

[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Binary)

$ErrorActionPreference = 'Stop'
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$Work = Join-Path ([IO.Path]::GetTempPath()) ("sendspin-smoke-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Work | Out-Null
# Every run names NUL as its config, so no config on the machine leaks in.
$Common = @('--config', 'NUL', '-o', 'null', '--no-mdns')
$failures = 0

function Pass([string]$What) {
    Write-Host "PASS: $What"
}

function Check([bool]$Ok, [string]$What, [string]$Detail = '') {
    if ($Ok) {
        Pass $What
    } else {
        Write-Host "FAIL: $What$(if ($Detail) { " -- $Detail" })"
        $script:failures++
    }
}

# Until the condition holds or the deadline passes; generous for loaded runners.
function Wait-For([scriptblock]$Condition, [int]$Seconds = 20) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return [bool](& $Condition)
}

# A native command's output with its stderr merged in, as strings. Windows PowerShell turns
# stderr into error records, which the script's 'Stop' preference would throw on.
function Invoke-Native([scriptblock]$Command) {
    $ErrorActionPreference = 'Continue'
    & $Command 2>&1 | ForEach-Object { "$_" }
}

function Read-Log([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        return (Get-Content -LiteralPath $Path -Raw)
    }
    return ''
}

try {
    # Basics
    $version = @(& $Binary --version)
    Check ($LASTEXITCODE -eq 0 -and $version[0] -match '^sendspin-cli \S+$' -and $version[1] -match '^sendspin-cpp \S+$') `
        '--version names this player and the library it links' ($version -join ' / ')
    & $Binary --help @Common | Out-Null
    Check ($LASTEXITCODE -eq 0) '--help exits 0'

    # A foreground player, its control socket, and a clean stop. Its own hidden console, so
    # taskkill without /F delivers the close event.
    $log = Join-Path $Work 'foreground.log'
    $port = 39311
    $player = Start-Process -FilePath $Binary -PassThru -WindowStyle Hidden -ArgumentList (@(
            '--port', $port, '-f', "`"$log`"", '--state-dir', "`"$Work\state`"") + $Common)
    Check (Wait-For { (Read-Log $log) -match "listening on port $port" }) 'the player starts and listens' (Read-Log $log)

    $status = @(Invoke-Native { & $Binary status --port $port --config NUL })
    Check ($LASTEXITCODE -eq 0 -and ($status -join "`n") -match '(?m)^name: ') `
        'status answers over the control socket' ($status -join ' / ')
    Invoke-Native { & $Binary pause --port $port --config NUL } | Out-Null
    Check ($LASTEXITCODE -eq 4) 'pause with no server is refused as not connected (exit 4)' "exit $LASTEXITCODE"
    Invoke-Native { & $Binary status --port ($port + 1) --config NUL } | Out-Null
    Check ($LASTEXITCODE -eq 3) 'status with nothing listening exits 3' "exit $LASTEXITCODE"

    & taskkill.exe /PID $player.Id | Out-Null
    Check ($player.WaitForExit(10000)) 'taskkill (no /F) stops it'
    Check ((Read-Log $log) -match 'Shutting down') 'and it shut down cleanly' (Read-Log $log)

    # -z: the parent returns, the pidfile names a live copy, and a second -z is refused.
    $zlog = Join-Path $Work 'daemon.log'
    $pidfile = Join-Path $Work 'daemon.pid'
    $zport = 39313
    & $Binary -z -P $pidfile -f $zlog --no-control --port $zport --state-dir "$Work\zstate" @Common
    Check ($LASTEXITCODE -eq 0) '-z returns 0 once detached'
    Check (Wait-For { (Read-Log $zlog) -match "listening on port $zport" }) 'the detached copy starts' (Read-Log $zlog)
    $daemonPid = [int]((Get-Content -LiteralPath $pidfile -ErrorAction SilentlyContinue) | Select-Object -First 1)
    $daemon = Get-Process -Id $daemonPid -ErrorAction SilentlyContinue
    Check ([bool]$daemon) '-P names a live process' "pidfile holds '$daemonPid'"

    $second = @(Invoke-Native { & $Binary -z -P $pidfile --no-control --port ($zport + 1) --state-dir "$Work\zstate2" @Common })
    Check ($LASTEXITCODE -ne 0 -and ($second -join "`n") -match 'already running') `
        'a second -z on the same pidfile is refused' ($second -join ' / ')

    if ($daemon) {
        & taskkill.exe /PID $daemonPid | Out-Null
        Check ($daemon.WaitForExit(10000)) 'taskkill stops the detached copy'
        Check (-not (Test-Path -LiteralPath $pidfile)) 'and its pidfile is removed'
    }
} finally {
    Get-Process -Name sendspin-cli -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $Binary } | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Host "$failures check(s) failed"
    exit 1
}
Write-Host 'Every smoke check passed'
