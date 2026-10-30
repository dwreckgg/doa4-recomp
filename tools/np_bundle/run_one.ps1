# Helper of run_check.bat (Windows PowerShell 5.1, which comes with Windows 10 and 11). Three jobs, one per switch:
#   -Check      the setup can work: your game folder, the exe, and folder names the game can read
#   -Name ...   one replay: starts the exe, waits at most -Timeout seconds, ends it if it is still running, and writes
#               <Run>\out\run_result.txt the way tools\np_run.py (judge) does, plus command.txt and pid.txt
#   -Sysinfo F  writes the OS and CPU of this PC to the file F
#   -Zip F      makes the zip F of the results: the folder out and the files dpps.txt, sysinfo.txt, recordings.txt of this
#               folder, entry names with '/' (written here so that no tar or zip tool on the PC is needed)
# The game's environment (DOA4_NP*, SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT) is set by run_check.bat and inherited here.
# Nothing here limits which CPUs the game may use.
param(
    [switch]$Check,
    [string]$Sysinfo,
    [string]$Zip,
    [string]$Name,
    [string]$Game,
    [string]$Exe,
    [string]$Run,
    [string]$Cache,
    [int]$Timeout = 900
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$notPlain = '[^\x20-\x7E]'

function Get-Verdict([string[]]$lines, [int]$code, [bool]$timedOut, [string]$outDir) {
    # tools\np_run.py judge(): ok only if the game exited 0 by itself after the session quit with "end of the recording"
    # and the log holds no WARN line; a refusal is never ok, and its reason is the quit text.
    $refusal = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].StartsWith('refused: ', [StringComparison]::Ordinal) -or $lines[$i].StartsWith('cannot replay ', [StringComparison]::Ordinal)) {
            $refusal = $i
            break
        }
    }
    if ($refusal -ge 0) {
        $flaws = @()
        for ($j = $refusal + 1; $j -lt $lines.Count; $j++) {
            if ($lines[$j].StartsWith('session on', [StringComparison]::Ordinal)) {
                $flaws += 'a session started after it'
                break
            }
        }
        if (Test-Path -LiteralPath (Join-Path $outDir 'digest.d4d')) { $flaws += 'digest.d4d was written' }
        if ($timedOut) { $flaws += 'the run had to be ended after the timeout' } elseif ($code -ne 0) { $flaws += "exit $code" }
        if ($flaws.Count -gt 0) { return @('failed', ('refusal not honoured (' + ($flaws -join '; ') + '): ' + $lines[$refusal])) }
        return @('failed', $lines[$refusal])
    }
    $quitLine = $null
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        if ($lines[$i].StartsWith('quit at frame ', [StringComparison]::Ordinal)) {
            $quitLine = $lines[$i]
            break
        }
    }
    $warn = $null
    foreach ($l in $lines) {
        if ($l.StartsWith('WARN ', [StringComparison]::Ordinal)) {
            $warn = $l
            break
        }
    }
    if ($null -ne $warn) {
        $q = 'no quit line'
        if ($null -ne $quitLine) { $q = $quitLine }
        return @('failed', "$warn ($q)")
    }
    $reason = $null
    if ($null -ne $quitLine) {
        $at = $quitLine.IndexOf(': ', [StringComparison]::Ordinal)
        if ($at -ge 0) { $reason = $quitLine.Substring($at + 2) }
    }
    $shown = 'none'
    if ($null -ne $quitLine) { $shown = $quitLine }
    if ((-not $timedOut) -and ($code -eq 0) -and ($reason -ceq 'end of the recording')) { return @('ok', $shown) }
    return @('failed', $shown)
}

if ($Check) {
    $problems = @()
    if ($PSScriptRoot -cmatch $notPlain) {
        $problems += "This folder has a letter that is not plain English (a-z, 0-9) in its path: $PSScriptRoot . Move the whole folder to a plain path such as C:\doa4test and run it again; the game cannot read other paths."
    }
    if ($Game -cmatch $notPlain) {
        $problems += "The GAME folder has a letter that is not plain English (a-z, 0-9) in its path: $Game . Copy the game folder to a plain path such as C:\DOA4 and use that."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Game 'default.xex'))) {
        $problems += "default.xex was not found in the GAME folder ""$Game"". Set the GAME line in run_check.bat to your DOA4 game folder (the folder that has default.xex in it)."
    }
    if (-not (Test-Path -LiteralPath $Exe)) {
        $problems += "The game exe $Exe was not found. Extract the whole zip again."
    }
    foreach ($p in $problems) { Write-Host $p }
    if ($problems.Count -gt 0) { exit 1 }
    exit 0
}

if ($Sysinfo) {
    $lines = @()
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $lines += "OS: $($os.Caption.Trim()) $($os.Version) build $($os.BuildNumber)"
    } catch { $lines += "OS: unknown ($($_.Exception.Message))" }
    try {
        foreach ($c in @(Get-CimInstance Win32_Processor)) {
            $lines += "CPU: $($c.Name.Trim()), $($c.NumberOfCores) cores, $($c.NumberOfLogicalProcessors) logical"
        }
    } catch { $lines += "CPU: unknown ($($_.Exception.Message))" }
    try {
        $mem = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
        $lines += ('RAM: {0:N1} GB' -f ($mem / 1GB))
    } catch { $lines += "RAM: unknown ($($_.Exception.Message))" }
    try {
        foreach ($g in @(Get-CimInstance Win32_VideoController)) { $lines += "GPU: $($g.Name.Trim()), driver $($g.DriverVersion)" }
    } catch { $lines += "GPU: unknown ($($_.Exception.Message))" }
    try {
        $lines += "UCRT: " + (Get-Item (Join-Path $env:SystemRoot 'System32\ucrtbase.dll')).VersionInfo.FileVersion
    } catch { $lines += "UCRT: unknown ($($_.Exception.Message))" }
    $lines += "PowerShell: $($PSVersionTable.PSVersion)"
    [System.IO.File]::WriteAllLines($Sysinfo, [string[]]$lines, $utf8)
    exit 0
}

if ($Zip) {
    try {
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $root = $PSScriptRoot.TrimEnd('\')
        $stream = [System.IO.File]::Open($Zip, [System.IO.FileMode]::Create)
        $archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($item in @('out', 'dpps.txt', 'sysinfo.txt', 'recordings.txt')) {
                $path = Join-Path $root $item
                if (-not (Test-Path -LiteralPath $path)) { continue }
                $files = @(Get-Item -LiteralPath $path)
                if (Test-Path -LiteralPath $path -PathType Container) { $files = @(Get-ChildItem -LiteralPath $path -Recurse -File) }
                foreach ($f in $files) {
                    $entry = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
                    try {
                        # opened for sharing: a file a stray process of the game still has open (stdout.txt, game.log) is read, not a reason to stop
                        $in = [System.IO.File]::Open($f.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                        try {
                            $dest = $archive.CreateEntry($entry, [System.IO.Compression.CompressionLevel]::Optimal).Open()
                            try { $in.CopyTo($dest) } finally { $dest.Dispose() }
                        } finally { $in.Dispose() }
                    } catch {
                        Write-Host ("left out " + $f.FullName + ": " + ($_.Exception.Message -replace '[\r\n]+', ' '))
                    }
                }
            }
        } finally {
            $archive.Dispose()
            $stream.Dispose()
        }
    } catch {
        Write-Host ("could not make the zip: " + ($_.Exception.Message -replace '[\r\n]+', ' '))
        exit 1
    }
    exit 0
}

if (-not $Name) {
    Write-Host 'run_one.ps1: nothing to do (-Check, -Sysinfo FILE, -Zip FILE or -Name NAME ...)'
    exit 2
}

$outDir = Join-Path $Run 'out'
$resultPath = Join-Path $outDir 'run_result.txt'
try {
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    $argList = @("--game_data_root=$Game", "--user_data_root=$(Join-Path $Run 'ud')", "--log_file=$(Join-Path $Run 'game.log')",
                 '--headless=true', "--cache_path=$Cache")
    $cmdLine = ($argList | ForEach-Object { '"' + $_ + '"' }) -join ' '
    # command.txt as np_run.py writes it: the command line, then the DOA4_ variables and the SDL one
    $shown = @($Exe + ' ' + ($argList -join ' '))
    foreach ($v in @(Get-ChildItem Env: | Where-Object { $_.Name -like 'DOA4_*' -or $_.Name -eq 'SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT' } | Sort-Object Name)) {
        $shown += "$($v.Name)=$($v.Value)"
    }
    [System.IO.File]::WriteAllLines((Join-Path $Run 'command.txt'), [string[]]$shown, $utf8)

    $started = Get-Date
    $p = Start-Process -FilePath $Exe -ArgumentList $cmdLine -WorkingDirectory (Split-Path -Parent $Exe) `
        -RedirectStandardOutput (Join-Path $Run 'stdout.txt') -RedirectStandardError (Join-Path $Run 'stderr.txt') -PassThru
    $null = $p.Handle   # keep the process handle: without it ExitCode can read empty after the process has ended
    [System.IO.File]::WriteAllText((Join-Path $Run 'pid.txt'), [string]$p.Id, $utf8)
    $timedOut = $false
    if (-not $p.WaitForExit($Timeout * 1000)) {
        $timedOut = $true
        Write-Host "timeout after $Timeout s: ending pid $($p.Id)"
        try { $p.Kill() } catch { }
        $p.WaitForExit()
    }
    $code = [int]$p.ExitCode

    $logLines = @()
    $logPath = Join-Path $outDir 'np_log.txt'
    if (Test-Path -LiteralPath $logPath) {
        $logLines = [string[]][System.IO.File]::ReadAllLines($logPath)
    } else {
        Write-Host 'no np_log.txt: the session did not start'
        if ($code -eq -1073741515) {
            Write-Host 'The game could not start: a Windows component is missing. Install the Microsoft Visual C++ 2015-2022 x64 runtime (vc_redist.x64.exe from Microsoft) and run this again.'
        }
    }
    $status, $quit = Get-Verdict $logLines $code $timedOut $outDir
    if ($timedOut) { $status = 'timeout' }
    $text = "status $status`nexit $code`ntimeout $([int]$timedOut)`nquit $quit`n"
    [System.IO.File]::WriteAllText($resultPath, $text, $utf8)
    $seconds = ((Get-Date) - $started).TotalSeconds
    Write-Host ('{0}: status {1} (exit {2} after {3:N0} s)' -f $Name, $status, $code, $seconds)
    if ($status -ne 'ok') { Write-Host "  $quit" }
    if ($status -eq 'ok') { exit 0 }
    exit 1
} catch {
    $msg = ($_.Exception.Message -replace '[\r\n]+', ' ')
    Write-Host "script error: $msg"
    try { [System.IO.File]::WriteAllText($resultPath, "status failed`nexit -1`ntimeout 0`nquit script error: $msg`n", $utf8) } catch { }
    exit 1
}
