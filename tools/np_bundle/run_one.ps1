# Helper of run_check.bat (Windows PowerShell 5.1, which comes with Windows 10 and 11). Three jobs, one per switch:
#   -Check      the setup can work: your game folder, the exe, and folder names the game can read
#   -Name ...   one replay: starts the exe, waits at most -Timeout seconds, ends it if it is still running, and writes
#               <Run>\out\run_result.txt the way tools\np_run.py (judge) does, plus command.txt and pid.txt
#   -Sysinfo F  writes the OS and CPU of this PC to the file F
#   -Zip F      makes the zip F of the results: the folder out and the files dpps.txt, sysinfo.txt, recordings.txt of this
#               folder, entry names with '/' (written here so that no tar or zip tool on the PC is needed)
#   -Setup      starts -Exe normally (a window) with --user_data_root=<-UserData> and a log in it, and waits for it to end: the
#               game's first-run setup screen (get_game_files.bat). Started here, not by `start`, which mangles the quotes of
#               a command line with several quoted arguments when the program is a .cmd (the tests' stand-in).
#   -FindGame   finds the game folder the game's first-run setup saved (get_game_files.bat): the line
#               doa4_game_files = "..." before the first [table] of bin\dead_or_alive_4.toml (the setup writes it there,
#               src/dead_or_alive_4_app.h SaveGameFiles, forward slashes, \ and " escaped). Exit 0: the folder holds default.xex
#               and its name is plain ASCII: it is written to game_path.txt (a line run_check.bat reads with `set /p`, which
#               only reads ASCII correctly: a folder with any other letter is never written there). Exit 1: nothing usable
#               saved (-Quiet: say nothing; -Prefix words the first line of an exit 0). Exit 2: the saved folder is fine but its name has an accent or another letter that
#               is not plain ASCII (the setup accepts any ANSI-code-page path, this test does not): the folder is named in the
#               message (printed by PowerShell itself, so the letters come out right) and nothing is written.
#               -Toml and -Result name other files (the tests).
#   -ForgetGame removes the doa4_game_files line (the top-level one only) from the toml, so that the game's first-run setup
#               screen appears again; every other byte of the file stays (BOM, line ends, other keys, tables).
# The game's environment (DOA4_NP*, SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT) is set by run_check.bat and inherited here.
# Nothing here limits which CPUs the game may use.
param(
    [switch]$Check,
    [switch]$FindGame,
    [switch]$Quiet,
    [string]$Prefix = 'Game files found',
    [switch]$ForgetGame,
    [switch]$Setup,
    [string]$UserData,
    [string]$Toml,
    [string]$Result,
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
# NP_BUNDLE_NO_WINDOW=1 is set only by this bundle's own tests (tests/tools/test_np_tools.py): Start-Process then uses the console it
# already has instead of opening a new window for a console program (the tests' stand-in game). Unset on the friend's PC: the
# game is started exactly as before.
$noWindow = @{}
if ($env:NP_BUNDLE_NO_WINDOW) { $noWindow['NoNewWindow'] = $true }
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

function Write-StartHint([int]$code) {
    # The game could not start: a Windows error status says why (a missing or old Visual C++ runtime). True if a hint was printed.
    if ($code -eq -1073741515) {
        Write-Host 'The game could not start: a Windows component is missing. Install the Microsoft Visual C++ 2015-2022 x64 runtime (vc_redist.x64.exe from Microsoft) and run this again.'
        return $true
    }
    if ($code -eq -1073741511) {
        # 0xC0000139: an old Visual C++ 2015-2022 runtime lacks a function (rexruntime.dll imports msvcp140_atomic_wait.dll)
        Write-Host 'The game could not start: your Microsoft Visual C++ 2015-2022 x64 runtime is too old. Install the newest vc_redist.x64.exe from Microsoft and run this again.'
        return $true
    }
    return $false
}

if ($Check) {
    $problems = @()
    if ($PSScriptRoot -cmatch $notPlain) {
        $problems += "This folder has a letter that is not plain English (a-z, 0-9) in its path: $PSScriptRoot . Move the whole folder to a plain path such as C:\doa4test and run it again; the game cannot read other paths."
    }
    if ($Game -cmatch $notPlain) {
        # (the folder is not echoed: a path typed into the batch file in Notepad comes out of cmd in another code page, garbled)
        $problems += "The GAME folder has a letter that is not plain English (a-z, 0-9) in its path. Copy the game folder to a plain path such as C:\DOA4 and put that in the GAME line."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Game 'default.xex'))) {
        $problems += "default.xex was not found in the GAME folder ""$Game"". Run get_game_files.bat to get your game files, or set the GAME line in run_check.bat to your DOA4 game folder (the folder that has default.xex in it)."
    }
    if (-not (Test-Path -LiteralPath $Exe)) {
        $problems += "The game exe $Exe was not found. Extract the whole zip again."
    }
    foreach ($p in $problems) { Write-Host $p }
    if ($problems.Count -gt 0) { exit 1 }
    exit 0
}

if ($Setup) {
    if ($PSScriptRoot -cmatch $notPlain) {
        Write-Host "This folder has a letter that is not plain English (a-z, 0-9) in its path: $PSScriptRoot . Move the whole folder to a plain path such as C:\doa4test and run this again; the game cannot read other paths."
        exit 1
    }
    try {
        New-Item -ItemType Directory -Force -Path $UserData | Out-Null
        $cmdLine = '"--user_data_root=' + $UserData + '" "--log_file=' + (Join-Path $UserData 'setup.log') + '"'
        $p = Start-Process @noWindow -FilePath $Exe -ArgumentList $cmdLine -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
        $null = $p.Handle
        $p.WaitForExit()
        $setupCode = [int]$p.ExitCode
    } catch {
        Write-Host ("The game could not be started: " + ($_.Exception.Message -replace '[\r\n]+', ' '))
        exit 1
    }
    # The setup is where a missing Visual C++ runtime shows first. Any other exit code is not an error here (closing the game
    # window or pressing Alt+F4 ends it with whatever code): the next step looks for the saved folder.
    if (Write-StartHint $setupCode) { exit 1 }
    exit 0
}

if ($FindGame) {
    if (-not $Toml) { $Toml = Join-Path $PSScriptRoot 'bin\dead_or_alive_4.toml' }
    if (-not $Result) { $Result = Join-Path $PSScriptRoot 'game_path.txt' }
    if (Test-Path -LiteralPath $Result) { Remove-Item -LiteralPath $Result -Force }
    $folder = $null
    $why = "the game's setup has not saved a game folder (no doa4_game_files line in $Toml)"
    if (Test-Path -LiteralPath $Toml) {
        foreach ($line in [System.IO.File]::ReadAllLines($Toml)) {
            if ($line -match '^\s*\[') { break }   # the first table header ends the top level: a key below it is another key
            if ($line -cmatch '^\s*doa4_game_files\s*=\s*"((?:[^"\\]|\\.)*)"\s*(#.*)?$') {   # TOML keys are case-sensitive
                $raw = $Matches[1]
                if ($raw -notmatch '^(?:[^\\]|\\[\\"])*$') {   # only the two escapes the setup writes (\\ and \")
                    $why = "the saved game folder uses an escape this script does not read: $raw"
                } else {
                    $folder = ($raw -replace '\\(.)', '$1').Replace('/', '\')
                }
                break
            }
        }
    }
    if ($folder -and -not (Test-Path -LiteralPath (Join-Path $folder 'default.xex'))) {
        $why = "the saved game folder ""$folder"" has no default.xex (was it moved or deleted?)"
        $folder = $null
    }
    if (-not $folder) {
        if (-not $Quiet) { Write-Host "No game files found: $why." }
        exit 1
    }
    if ($folder -cmatch $notPlain) {
        # The setup accepts any folder the ANSI code page can write; this test does not (the batch file would read the name in the
        # wrong code page, and the game decodes its arguments in the ANSI code page). Nothing is written to $Result.
        Write-Host "Your game files are in this folder: $folder"
        Write-Host 'Its name has a letter that is not plain English (an accent or another special letter), so the test cannot use it.'
        exit 2
    }
    [System.IO.File]::WriteAllText($Result, $folder + "`r`n", $utf8)
    Write-Host "${Prefix}: $folder"
    exit 0
}

if ($ForgetGame) {
    if (-not $Toml) { $Toml = Join-Path $PSScriptRoot 'bin\dead_or_alive_4.toml' }
    if (-not (Test-Path -LiteralPath $Toml)) { exit 0 }
    $bytes = [System.IO.File]::ReadAllBytes($Toml)
    $bomLength = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $bomLength = 3 }
    $text = $utf8.GetString($bytes, $bomLength, $bytes.Length - $bomLength)
    $kept = New-Object System.Text.StringBuilder
    $inTable = $false
    $removed = 0
    foreach ($m in [regex]::Matches($text, '[^\n]*\n|[^\n]+$')) {
        $line = $m.Value
        if (-not $inTable) {
            if ($line -match '^\s*\[') { $inTable = $true }
            elseif ($line -cmatch '^\s*doa4_game_files\s*=') { $removed++; continue }
        }
        [void]$kept.Append($line)
    }
    if ($removed -eq 0) { exit 0 }
    $body = $utf8.GetBytes($kept.ToString())
    $out = New-Object byte[] ($bomLength + $body.Length)
    [Array]::Copy($bytes, 0, $out, 0, $bomLength)
    [Array]::Copy($body, 0, $out, $bomLength, $body.Length)
    $tmp = $Toml + '.tmp'
    [System.IO.File]::WriteAllBytes($tmp, $out)
    Move-Item -LiteralPath $tmp -Destination $Toml -Force
    Write-Host 'The saved game folder was removed from the settings file, so the game setup will ask again.'
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
    $p = Start-Process @noWindow -FilePath $Exe -ArgumentList $cmdLine -WorkingDirectory (Split-Path -Parent $Exe) `
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
        $null = Write-StartHint $code
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
