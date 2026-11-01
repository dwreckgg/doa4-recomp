#Requires -Version 5.1
# DOA4 launcher: the setup and the game's launcher in one window (WinForms).
# All logic is in DOA4Setup.psm1; this file only wires buttons to it. Each button runs the
# same module function as the matching non-interactive action:
#   DOA4Launcher.ps1                         the window
#   DOA4Launcher.ps1 -DryRun [-AssumeMissing git,llvm,vs]
#                                            print what Install would install and do
#   DOA4Launcher.ps1 -Action Install|Update|Play|SetGameFiles|Status [-ShortcutFolder <dir>]
#                    [-GameFiles <dir>]      no window; output on the console; exit code 0/1.
#                                            Install never installs tools here (no one to ask)
#                                            and makes a shortcut only with -ShortcutFolder.
# Keep this file ASCII only (Windows PowerShell 5.1 reads it in the ANSI code page).
param(
    [ValidateSet('', 'Install', 'Update', 'Play', 'SetGameFiles', 'Status', 'DryRun')]
    [string]$Action = '',
    [switch]$DryRun,
    [string[]]$AssumeMissing = @(),
    [string]$ShortcutFolder = '',
    [string]$GameFiles = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path -Parent $here
Import-Module (Join-Path $here 'DOA4Setup.psm1') -Force -DisableNameChecking
$config = & (Join-Path $here 'config.ps1')

if ($DryRun -and $Action -and $Action -ne 'DryRun') {
    # Never run a real action while the caller believes it is a dry run.
    [Console]::Out.WriteLine("-DryRun cannot be combined with -Action $Action. Use -DryRun alone to see what Install would do.")
    exit 1
}
if ($DryRun -and -not $Action) { $Action = 'DryRun' }
# With -File, "-AssumeMissing git,llvm" arrives as one string.
$AssumeMissing = @($AssumeMissing | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# One launcher per DOA4 folder (the window and every action that changes something; the
# read-only DryRun and Status need no lock). Held until this process ends.
$script:rootLock = $null
if ($Action -ne 'DryRun' -and $Action -ne 'Status') {
    $script:rootLock = Enter-DOA4RootLock -Root $root
    if (-not $script:rootLock) {
        $busy = "The DOA4 launcher is already running for this folder ($root). Use that window, or wait until it has finished."
        if ($Action) { [Console]::Out.WriteLine($busy); exit 1 }
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show($busy, 'Dead or Alive 4', 'OK', 'Information')
        exit 1
    }
}

# ------------------------------------------------------------------------------------------
# Non-interactive mode
# ------------------------------------------------------------------------------------------
if ($Action) {
    $sink = { param($line) [Console]::Out.WriteLine($line) }
    $ctx = New-DOA4Context -Root $root -Config $config -Sink $sink -ShortcutFolder $ShortcutFolder -Action $Action
    $code = 0
    try {
        switch ($Action) {
            'DryRun' { Write-DOA4DryRun -Ctx $ctx -AssumeMissing $AssumeMissing | Out-Null }
            'Install' {
                $r = Invoke-DOA4Install -Ctx $ctx
                Write-DOA4Log $ctx $r.Message
                if (-not $r.Ok) { $code = 1 }
            }
            'Update' {
                $r = Invoke-DOA4Update -Ctx $ctx
                Write-DOA4Log $ctx $r.Message
                if (-not $r.Ok) { $code = 1 }
            }
            'Play' {
                $p = Start-DOA4Game -Ctx $ctx -GameFiles $GameFiles
                Write-DOA4Log $ctx ('started process {0}' -f $p.Id)
            }
            'SetGameFiles' {
                if (-not $GameFiles) { throw '-GameFiles <folder> is required.' }
                $where = Set-DOA4GameFiles -Ctx $ctx -Folder $GameFiles
                Write-DOA4Log $ctx "saved to $where"
            }
            'Status' {
                $src = Get-DOA4Source -Ctx $ctx
                $line = 'status: ' + (Get-DOA4BuildStatus -Ctx $ctx) + '; source: ' + $src
                $lr = (Get-DOA4Settings $ctx.SettingsPath).lastRelease
                if ($src -eq 'release' -and $lr -and $lr.tag) { $line += '; release ' + $lr.tag }
                Write-DOA4Log $ctx $line
            }
        }
    } catch {
        Write-DOA4Log $ctx ('error: ' + $_.Exception.Message)
        $code = 1
    }
    Write-DOA4Log $ctx "log: $($ctx.LogPath)"
    exit $code
}

# ------------------------------------------------------------------------------------------
# The window
# ------------------------------------------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$queue = New-Object 'System.Collections.Concurrent.ConcurrentQueue[object]'
$script:job = $null
$script:iconSet = $false
$script:syncingSource = $false
$installSteps = @('Folder', 'Tools', 'Disk space', 'Code', 'Fixes', 'Build', 'Shortcut')
$updateSteps = @('Folder', 'Tools', 'Sources', 'Local changes', 'Update', 'Build')
$releaseUpdateSteps = @('Folder', 'Running game', 'Release', 'Download', 'Checksums', 'Swap')
$releaseInstallSteps = $releaseUpdateSteps + @('Shortcut')

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Dead or Alive 4'
$form.Size = New-Object System.Drawing.Size(820, 600)
$form.MinimumSize = New-Object System.Drawing.Size(640, 420)
$form.StartPosition = 'CenterScreen'

$status = New-Object System.Windows.Forms.Label
$status.Dock = 'Top'
$status.Height = 28
$status.TextAlign = 'MiddleLeft'
$status.Padding = New-Object System.Windows.Forms.Padding(8, 0, 0, 0)

$buttons = New-Object System.Windows.Forms.FlowLayoutPanel
$buttons.Dock = 'Top'
$buttons.Height = 40
$buttons.Padding = New-Object System.Windows.Forms.Padding(4)

function New-Button([string]$text) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text
    $b.AutoSize = $true
    $buttons.Controls.Add($b)
    return $b
}
$bInstall = New-Button 'Install'
$bPlay = New-Button 'Play'
$bUpdate = New-Button 'Update && rebuild'
$bGameFiles = New-Button 'Change game files...'
$bPhotos = New-Button 'Open photos'
$bSaves = New-Button 'Open saves'
$bLogs = New-Button 'Open logs'

# Where Install and Update get the game from, saved in settings.json ("source").
$sourcePanel = New-Object System.Windows.Forms.FlowLayoutPanel
$sourcePanel.Dock = 'Top'
$sourcePanel.Height = 30
$sourcePanel.Padding = New-Object System.Windows.Forms.Padding(8, 4, 4, 0)
$sourceLabel = New-Object System.Windows.Forms.Label
$sourceLabel.Text = 'Get the game from:'
$sourceLabel.AutoSize = $true
$sourceLabel.Padding = New-Object System.Windows.Forms.Padding(0, 4, 0, 0)
$rbRelease = New-Object System.Windows.Forms.RadioButton
$rbRelease.Text = 'Releases (recommended)'
$rbRelease.AutoSize = $true
$rbSource = New-Object System.Windows.Forms.RadioButton
$rbSource.Text = 'Build from source (advanced)'
$rbSource.AutoSize = $true
$sourcePanel.Controls.AddRange(@($sourceLabel, $rbRelease, $rbSource))

$steps = New-Object System.Windows.Forms.ListView
$steps.View = 'Details'
$steps.Dock = 'Top'
$steps.Height = 150
$steps.FullRowSelect = $true
$steps.HeaderStyle = 'Nonclickable'
[void]$steps.Columns.Add('Step', 120)
[void]$steps.Columns.Add('Status', 640)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Both'
$log.WordWrap = $false
$log.Dock = 'Fill'
$log.Font = New-Object System.Drawing.Font('Consolas', 9)

$form.Controls.Add($log)
$form.Controls.Add($steps)
$form.Controls.Add($buttons)
$form.Controls.Add($sourcePanel)
$form.Controls.Add($status)

function Add-LogLine([string]$line) { $log.AppendText($line + [Environment]::NewLine) }

function Get-UiContext([string]$name) {
    $sink = { param($line) Add-LogLine $line }
    New-DOA4Context -Root $root -Config $config -Sink $sink -Action $name
}

function Show-Steps([string[]]$names) {
    $steps.Items.Clear()
    foreach ($n in $names) {
        $item = New-Object System.Windows.Forms.ListViewItem($n)
        [void]$item.SubItems.Add('waiting')
        [void]$steps.Items.Add($item)
    }
}

function Set-StepRow([string]$name, [string]$state, [string]$detail) {
    $text = $state
    if ($detail) { $text = "$state - $detail" }
    foreach ($item in $steps.Items) {
        if ($item.Text -eq $name) { $item.SubItems[1].Text = $text; return }
    }
    $item = New-Object System.Windows.Forms.ListViewItem($name)
    [void]$item.SubItems.Add($text)
    [void]$steps.Items.Add($item)
}

function Get-UiSource {
    return (Get-DOA4Source -Ctx (New-DOA4Context -Root $root -Config $config -Action 'status'))
}

function Update-Status {
    $ctx = New-DOA4Context -Root $root -Config $config -Action 'status'
    $src = Get-DOA4Source -Ctx $ctx
    $s = Get-DOA4BuildStatus -Ctx $ctx
    if ($src -eq 'release') {
        $lr = (Get-DOA4Settings $ctx.SettingsPath).lastRelease
        if ($s -eq 'not built') { $status.Text = 'Game: not installed yet (from the releases)' }
        elseif ($lr -and $lr.tag) { $status.Text = "Game: release $($lr.tag) (this build asks for its game files itself)" }
        else { $status.Text = 'Game: a release (this build asks for its game files itself)' }
        $bUpdate.Text = 'Update'
    } else {
        $setup = ''
        if (Test-Path -LiteralPath $ctx.GamePath) {
            if (Test-DOA4HasFirstRunSetup $ctx) { $setup = ' (this build asks for its game files itself)' }
            else { $setup = ' (game files: ' + $(if ((Get-DOA4Settings $ctx.SettingsPath).gameFiles) { (Get-DOA4Settings $ctx.SettingsPath).gameFiles } else { 'not chosen yet' }) + ')' }
        }
        $status.Text = "Game: $s$setup"
        $bUpdate.Text = 'Update && rebuild'
    }
    # The radio buttons show the mode in use; setting them here must not save a choice.
    $script:syncingSource = $true
    try {
        $rbRelease.Checked = ($src -eq 'release')
        $rbSource.Checked = ($src -eq 'source')
    } finally { $script:syncingSource = $false }
    $bPlay.Enabled = ($s -ne 'not built') -and -not $script:job
    if ($s -ne 'not built' -and -not $script:iconSet) {
        try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon((Get-DOA4ExePath -Ctx $ctx)); $script:iconSet = $true } catch { }
    }
}

function Set-UiSource([string]$source) {
    if ($script:syncingSource -or $script:job) { return }
    try {
        Set-DOA4Source -Ctx (Get-UiContext 'source') -Source $source
        if ($source -eq 'release') { Add-LogLine 'Install and Update now get the game from the netplay releases.' }
        else { Add-LogLine 'Install and Update now build the game from source (they install Git, LLVM and the Visual Studio Build Tools when missing).' }
    } catch {
        [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Dead or Alive 4', 'OK', 'Warning')
    }
    Update-Status
}
$rbRelease.Add_CheckedChanged({ if ($rbRelease.Checked) { Set-UiSource 'release' } })
$rbSource.Add_CheckedChanged({ if ($rbSource.Checked) { Set-UiSource 'source' } })

function Set-Busy([bool]$busy) {
    foreach ($b in @($bInstall, $bUpdate, $bGameFiles, $rbRelease, $rbSource)) { $b.Enabled = -not $busy }
    $bPlay.Enabled = -not $busy
    if (-not $busy) { Update-Status }
}

# Runs one module action in a background runspace; progress comes back through $queue.
$background = {
    param($here, $root, $queue, $kind, $options)
    Import-Module (Join-Path $here 'DOA4Setup.psm1') -DisableNameChecking
    $config = & (Join-Path $here 'config.ps1')
    $sink = { param($line) $queue.Enqueue(@{ Kind = 'log'; Text = $line }) }.GetNewClosure()
    $onStep = { param($n, $s, $d) $queue.Enqueue(@{ Kind = 'step'; Name = $n; State = $s; Detail = $d }) }.GetNewClosure()
    $ctx = New-DOA4Context -Root $root -Config $config -Sink $sink -OnStep $onStep -ShortcutFolder $options.ShortcutFolder -Action $kind
    try {
        switch ($kind) {
            'plan' { $queue.Enqueue(@{ Kind = 'plan'; Plan = (Get-DOA4InstallPlan -Ctx $ctx).Plan }) }
            'install' { $queue.Enqueue(@{ Kind = 'done'; Result = (Invoke-DOA4Install -Ctx $ctx -ToolsApproved:([bool]$options.Approved)) }) }
            'update' { $queue.Enqueue(@{ Kind = 'done'; Result = (Invoke-DOA4Update -Ctx $ctx) }) }
        }
    } catch {
        $queue.Enqueue(@{ Kind = 'done'; Result = [pscustomobject]@{ Ok = $false; Step = ''; Message = $_.Exception.Message; LogPath = $ctx.LogPath } })
    }
}

function Start-Background([string]$kind, [hashtable]$options) {
    $ps = [PowerShell]::Create()
    [void]$ps.AddScript($background).AddArgument($here).AddArgument($root).AddArgument($queue).AddArgument($kind).AddArgument($options)
    $script:job = @{ PS = $ps; Handle = $ps.BeginInvoke() }
    Set-Busy $true
}

function Stop-BackgroundJob {
    if ($script:job) {
        try { [void]$script:job.PS.EndInvoke($script:job.Handle) } catch { Add-LogLine ('error: ' + $_.Exception.Message) }
        $script:job.PS.Dispose()
        $script:job = $null
    }
}

function On-Plan($plan) {
    Stop-BackgroundJob
    if ($plan.Blocked) {
        Set-Busy $false
        [void][System.Windows.Forms.MessageBox]::Show($plan.Blocked, 'Dead or Alive 4', 'OK', 'Warning')
        return
    }
    $approved = $false
    if ($plan.Items.Count -gt 0) {
        $list = ($plan.Items | ForEach-Object { '  - {0} ({1})' -f $_.Name, $_.Size }) -join "`r`n"
        $answer = [System.Windows.Forms.MessageBox]::Show(
            "These tools are missing and will be installed with winget:`r`n`r`n$list`r`n`r`nVisual Studio shows its own progress window and asks once for administrator rights. Install them now?",
            'Dead or Alive 4 setup', 'YesNo', 'Question')
        if ($answer -ne 'Yes') {
            Add-LogLine 'Nothing was installed.'
            Set-Busy $false
            return
        }
        $approved = $true
    }
    Show-Steps $installSteps
    Start-Background 'install' @{ Approved = $approved; ShortcutFolder = [Environment]::GetFolderPath('Desktop') }
}

function On-Done($result) {
    Stop-BackgroundJob
    Set-Busy $false
    if ($result.Ok) {
        Add-LogLine $result.Message
    } else {
        $msg = "$($result.Message)`r`n`r`nLog: $($result.LogPath)"
        [void][System.Windows.Forms.MessageBox]::Show($msg, 'Dead or Alive 4', 'OK', 'Warning')
    }
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 150
$timer.Add_Tick({
        $item = $null
        $n = 0
        while ($n -lt 500 -and $queue.TryDequeue([ref]$item)) {
            $n++
            switch ($item.Kind) {
                'log' { Add-LogLine $item.Text }
                'step' { Set-StepRow $item.Name $item.State $item.Detail }
                'plan' { On-Plan $item.Plan }
                'done' { On-Done $item.Result }
            }
        }
    })

function Select-GameFolder {
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Choose the folder that holds default.xex (the extracted game files).'
    if ($dlg.ShowDialog($form) -ne 'OK') { return $null }
    return $dlg.SelectedPath
}

$upstreamNote = 'This build cannot extract a disc image itself: extract it first (README, section 5 "The game files"), then choose the extracted folder.'

function Invoke-ChangeGameFiles {
    $folder = Select-GameFolder
    if (-not $folder) { return $false }
    $ctx = Get-UiContext 'gamefiles'
    try {
        $where = Set-DOA4GameFiles -Ctx $ctx -Folder $folder
        if ($where -eq 'toml') { Add-LogLine 'Saved in the game''s settings; used from the next start.' }
        Update-Status
        return $true
    } catch {
        $text = $_.Exception.Message
        if (-not (Test-DOA4HasFirstRunSetup $ctx)) { $text += "`r`n`r`n" + $upstreamNote }
        [void][System.Windows.Forms.MessageBox]::Show($text, 'Dead or Alive 4', 'OK', 'Warning')
        return $false
    }
}

$bInstall.Add_Click({
        if ((Get-UiSource) -eq 'release') {
            # No tools to install: straight to the download.
            Show-Steps $releaseInstallSteps
            Start-Background 'install' @{ Approved = $false; ShortcutFolder = [Environment]::GetFolderPath('Desktop') }
            return
        }
        Show-Steps @('Tools')
        Start-Background 'plan' @{ ShortcutFolder = '' }
    })
$bUpdate.Add_Click({
        if ((Get-UiSource) -eq 'release') { Show-Steps $releaseUpdateSteps } else { Show-Steps $updateSteps }
        Start-Background 'update' @{ ShortcutFolder = '' }
    })
$bGameFiles.Add_Click({ [void](Invoke-ChangeGameFiles) })
$bPlay.Add_Click({
        $ctx = Get-UiContext 'play'
        for ($attempt = 0; $attempt -lt 2; $attempt++) {
            try {
                $p = Start-DOA4Game -Ctx $ctx
                Add-LogLine ('Started (process {0}).' -f $p.Id)
                return
            } catch {
                $text = $_.Exception.Message
                if ($attempt -gt 0 -or (Test-DOA4HasFirstRunSetup $ctx) -or $text -like 'The game is not built*') {
                    [void][System.Windows.Forms.MessageBox]::Show($text, 'Dead or Alive 4', 'OK', 'Warning')
                    return
                }
                if ($text -ne 'NEED_GAME_FILES') {
                    [void][System.Windows.Forms.MessageBox]::Show($text + "`r`n`r`nChoose the game files folder again.", 'Dead or Alive 4', 'OK', 'Warning')
                }
                if (-not (Invoke-ChangeGameFiles)) { return }
            }
        }
    })
function Open-Folder([string]$path) {
    if (Test-Path -LiteralPath $path) { Start-Process -FilePath 'explorer.exe' -ArgumentList ('"' + $path + '"') }
    else { [void][System.Windows.Forms.MessageBox]::Show("$path does not exist yet.", 'Dead or Alive 4', 'OK', 'Information') }
}
$bPhotos.Add_Click({ Open-Folder (Get-DOA4FolderTargets -Ctx (Get-UiContext 'open')).Photos })
$bSaves.Add_Click({ Open-Folder (Get-DOA4FolderTargets -Ctx (Get-UiContext 'open')).Saves })
$bLogs.Add_Click({ Open-Folder (Get-DOA4FolderTargets -Ctx (Get-UiContext 'open')).Logs })

$form.Add_FormClosing({
        param($sender, $e)
        if ($script:job) {
            [void][System.Windows.Forms.MessageBox]::Show('Setup is still working. Wait until it has finished (the log shows its progress).', 'Dead or Alive 4', 'OK', 'Information')
            $e.Cancel = $true
        }
    })
$form.Add_Shown({
        Update-Status
        if ((Get-DOA4BuildStatus -Ctx (New-DOA4Context -Root $root -Config $config -Action 'status')) -eq 'not built') {
            if ((Get-UiSource) -eq 'release') {
                Add-LogLine 'Not installed yet: press Install. It downloads the newest netplay release from GitHub, checks every file against the release''s checksums and makes a desktop shortcut.'
            } else {
                Add-LogLine 'Not installed yet: press Install. It checks the tools, downloads the sources, builds the game and makes a desktop shortcut.'
            }
        }
    })

$timer.Start()
[void]$form.ShowDialog()
$timer.Stop()
