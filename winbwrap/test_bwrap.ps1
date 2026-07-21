param(
    [int]$Rounds = 5,
    [switch]$Cmd,
    [switch]$Pwsh,
    [switch]$CmdPwsh,
    [switch]$NoInject,
    [switch]$Verbose,
    [string]$Bind = "$PSScriptRoot\.."
)

$ErrorActionPreference = "Continue"
$bwrap = "$PSScriptRoot\bwrap\x64\Release\bwrap.exe"
$aliasPwsh = "$env:LOCALAPPDATA\Microsoft\WindowsApps\pwsh.exe"
$bindResolved = (Resolve-Path $Bind).Path
$tempBind = "$env:LOCALAPPDATA\Temp"

if (-not (Test-Path $bwrap)) { throw "bwrap.exe not found: $bwrap" }

$runCmd = $Cmd -or (-not $Pwsh -and -not $CmdPwsh)
$runPwsh = $Pwsh -or (-not $Cmd -and -not $CmdPwsh)
$runCmdPwsh = $CmdPwsh -or (-not $Cmd -and -not $Pwsh)

Write-Host "==================== bwrap stability test ====================" -ForegroundColor Magenta
Write-Host "bwrap  : $bwrap" -ForegroundColor DarkGray
Write-Host "bind   : $bindResolved" -ForegroundColor DarkGray
Write-Host "rounds : $Rounds" -ForegroundColor DarkGray
$mode = "$(if ($runCmd) {'cmd '})$(if ($runPwsh) {'pwsh '})$(if ($runCmdPwsh) {'cmd-pwsh '})".Trim()
Write-Host "mode   : $mode" -ForegroundColor DarkGray
Write-Host ""

function Test-Command($label, $exe, $extra, $extraBinds) {
    Write-Host "=== $label x$Rounds ===" -ForegroundColor Cyan
    $ok = 0; $crash = 0; $fail = 0
    $crashCodes = @{}
    for ($i = 1; $i -le $Rounds; $i++) {
        Write-Host "--- [$i/$Rounds] ---" -ForegroundColor DarkGray

        $bwrapArgs = @("--bind", $bindResolved)
        if ($extraBinds) { $bwrapArgs += $extraBinds }
        if ($NoInject) { $bwrapArgs += "--no-inject" }
        if ($Verbose) { $bwrapArgs += "--showConsole" }
        $bwrapArgs += @("--", $exe) + $extra

        if ($Verbose) {
            $out = & $bwrap @bwrapArgs 2>&1
            $ec = $LASTEXITCODE
            if ($out) {
                foreach ($line in $out) { Write-Host "    $line" -ForegroundColor Gray }
            }
        } else {
            & $bwrap @bwrapArgs | Out-Host
            $ec = $LASTEXITCODE
        }

        if ($ec -eq 0) {
            $ok++
            Write-Host "  [$i] OK" -ForegroundColor Green
        } elseif ($ec -eq -1073740791) {
            $crash++
            $crashCodes['0xC0000409'] = ($crashCodes['0xC0000409'] + 1)
            Write-Host "  [$i] CRASH (0xC0000409 STATUS_STACK_BUFFER_OVERRUN)" -ForegroundColor Red
        } elseif ($ec -lt -100) {
            $crash++
            $code = "0x$($ec.ToString('X8'))"
            $crashCodes[$code] = ($crashCodes[$code] + 1)
            Write-Host "  [$i] CRASH ($code)" -ForegroundColor Red
        } else {
            $fail++
            Write-Host "  [$i] FAIL exit=$ec" -ForegroundColor Yellow
        }
        Start-Sleep -Milliseconds 200
    }
    Write-Host ""
    Write-Host "  => ok=$ok crash=$crash fail=$fail" -ForegroundColor White
    if ($crashCodes.Count -gt 0) {
        foreach ($key in $crashCodes.Keys) {
            Write-Host "     $key x$($crashCodes[$key])" -ForegroundColor Red
        }
    }
    Write-Host ""
    @{ ok = $ok; crash = $crash; fail = $fail }
}

$results = @{}
if ($runCmd) {
    $results['cmd'] = Test-Command "cmd.exe" "cmd.exe" @("/d", "/c", "echo hello") @("--showbox")
}

if ($runPwsh) {
    if (Test-Path $aliasPwsh) {
        $results['pwsh'] = Test-Command "pwsh (direct)" $aliasPwsh @("-NoLogo", "-NoProfile", "-Command", "Write-Host hello") @("--bind", $tempBind, "--showbox")
    } else {
        Write-Host "pwsh alias not found: $aliasPwsh" -ForegroundColor Yellow
    }
}

if ($runCmdPwsh) {
    if (Test-Path $aliasPwsh) {
        $results['cmd-pwsh'] = Test-Command "pwsh via cmd" "cmd.exe" @("/d", "/c", "`"$aliasPwsh`" -NoLogo -NoProfile -Command Write-Host hello") @("--bind", $tempBind, "--showbox")
    } else {
        Write-Host "pwsh alias not found: $aliasPwsh" -ForegroundColor Yellow
    }
}

Write-Host "============================================================"
foreach ($key in $results.Keys) {
    $r = $results[$key]
    Write-Host " $($key.PadRight(8)) => ok=$($r.ok) crash=$($r.crash) fail=$($r.fail)"
}
Write-Host "============================================================"
