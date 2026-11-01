. (Join-Path $PSScriptRoot 'TestHelpers.ps1')

# Release mode: Install and Update download the newest netplay release's files, check each one
# against the release's SHA256SUMS.txt and swap the release folder whole. The GitHub API, the
# downloads, the running-game query and the folder renames go through the command table, so
# these tests replace them (TestHelpers.ps1: New-FakeHttp, New-TReleaseTable); the real entries
# are checked at the end against a local HTTP server, this process and a real folder.

$script:Src = Join-Path $PSScriptRoot '..'
$script:Three = @('dead_or_alive_4.exe', 'rexruntime.dll', 'TracyClient.dll')

# The brief's list (draft v1.0.9, another channel's v1.0.8, netplay-v1.0.8 without
# TracyClient.dll, netplay-v1.0.7 complete, netplay-v1.0.6) plus the releases on which the
# likeliest wrong rules pick something else: a tag without a channel, a channel that only
# begins with "netplay", a newer release without SHA256SUMS.txt, and complete releases both
# before and after v1.0.7 in the list (first-complete picks v1.0.6, last-complete v1.0.5).
function Get-TStandardSpecs {
    @(
        @{ Tag = 'netplay-v1.0.6' },
        @{ Tag = 'netplay-v1.0.9'; Draft = $true },
        @{ Tag = 'v1.0.8' },
        @{ Tag = 'nightly-v1.0.8' },
        @{ Tag = 'netplay-nightly-v1.0.11' },
        @{ Tag = 'netplay-v1.0.8'; Omit = @('TracyClient.dll') },
        @{ Tag = 'netplay-v1.1.0'; Omit = @('SHA256SUMS.txt') },
        @{ Tag = 'netplay-v1.0.7' },
        @{ Tag = 'netplay-v1.0.5' }
    )
}

# A DOA4 folder in TestDrive with a release-mode context over a fake GitHub.
function New-TRel {
    param([string]$Name, [object[]]$Specs = (Get-TStandardSpecs), [string]$Tag = '', [string[]]$Running = @(),
        [string]$FailRename = '', [int]$ListStatus = 200, [string]$ListText = '', [string]$ShortcutFolder = '')
    $set = New-TReleaseSet $Specs
    $http = New-FakeHttp -ListJson $set.Json -Files $set.Files -ListStatus $ListStatus -ListText $ListText
    $root = Join-Path $TestDrive $Name
    $o = [pscustomobject]@{ Root = $root; Release = (Join-Path $root 'release'); Http = $http; Set = $set; Ctx = $null; Cmd = $null }
    $o.Cmd = New-TReleaseTable -Http $http -Running $Running -FailRename $FailRename
    $o.Ctx = New-DOA4Context -Root $root -Config (New-TReleaseConfig -Tag $Tag) -Cmd $o.Cmd -ShortcutFolder $ShortcutFolder -Action 'install'
    return $o
}

# Another context on the same folder and fake GitHub: another pin, a running game, a failing rename.
function New-TRelCtx {
    param($R, [string]$Tag = '', [string[]]$Running = @(), [string]$FailRename = '')
    $cmd = New-TReleaseTable -Http $R.Http -Running $Running -FailRename $FailRename
    $ctx = New-DOA4Context -Root $R.Root -Config (New-TReleaseConfig -Tag $Tag) -Cmd $cmd -Action 'update'
    return [pscustomobject]@{ Ctx = $ctx; Cmd = $cmd }
}

# Every file under a folder with its SHA-256, sorted; '<absent>' when the folder is missing.
function Get-TTree {
    param([string]$Dir)
    if (-not (Test-Path -LiteralPath $Dir)) { return '<absent>' }
    $full = [IO.Path]::GetFullPath($Dir).TrimEnd('\')
    return (@(Get-ChildItem -LiteralPath $Dir -Recurse -Force -File | ForEach-Object {
                $_.FullName.Substring($full.Length + 1) + '|' + (Get-TBytesHash $_.FullName)
            } | Sort-Object) -join "`n")
}

function Get-TFileHex { param([string]$Path) return (Get-TSha256Hex ([IO.File]::ReadAllBytes($Path))) }

# settings.json read by an independent reader (not Get-DOA4Settings).
function Read-TSettingsJson {
    param([string]$Root)
    return ([IO.File]::ReadAllText((Join-Path $Root 'launcher\settings.json')) | ConvertFrom-Json)
}

Describe 'Get-DOA4Release: the newest complete release of the channel' {
    It 'the tests'' SHA-256 oracle is SHA-256 (FIPS 180-2 example: "abc")' {
        Get-TSha256Hex ([Text.Encoding]::ASCII.GetBytes('abc')) | Should BeExactly 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
    }
    It 'picks netplay-v1.0.7: not the draft v1.0.9, no other channel, not the v1.0.8 without TracyClient.dll or the v1.1.0 without SHA256SUMS.txt, not the first or last complete one' {
        $r = New-TRel 'pick'
        $p = Get-DOA4Release -Ctx $r.Ctx
        $p.Tag | Should BeExactly 'netplay-v1.0.7'
        @($p.Assets.Keys | Sort-Object) -join ',' | Should BeExactly 'dead_or_alive_4.exe,rexruntime.dll,SHA256SUMS.txt,TracyClient.dll'
        foreach ($n in ($script:Three + 'SHA256SUMS.txt')) { $p.Assets[$n] | Should BeExactly (Get-TAssetUrl 'netplay-v1.0.7' $n) }
        @($r.Http.State.Calls | ForEach-Object { $_.Url }) -join ' ' | Should BeExactly $script:TReleaseListUrl
    }
    It 'compares versions as numbers: netplay-v1.0.10 is newer than netplay-v1.0.9' {
        $r = New-TRel 'numeric' -Specs @(@{ Tag = 'netplay-v1.0.9' }, @{ Tag = 'netplay-v1.0.10' }, @{ Tag = 'netplay-v1.0.2' })
        (Get-DOA4Release -Ctx $r.Ctx).Tag | Should BeExactly 'netplay-v1.0.10'
    }
    It 'a pinned ReleaseTag (config.ps1) picks that release, netplay-v1.0.6, with its own files' {
        $r = New-TRel 'pin' -Tag 'netplay-v1.0.6'
        $p = Get-DOA4Release -Ctx $r.Ctx
        $p.Tag | Should BeExactly 'netplay-v1.0.6'
        foreach ($n in ($script:Three + 'SHA256SUMS.txt')) { $p.Assets[$n] | Should BeExactly (Get-TAssetUrl 'netplay-v1.0.6' $n) }
    }
    It '-Tag pins as the config does' {
        $r = New-TRel 'pinparam'
        (Get-DOA4Release -Ctx $r.Ctx -Tag 'netplay-v1.0.5').Tag | Should BeExactly 'netplay-v1.0.5'
    }
    It 'a pinned release that lacks a file is refused by its name' {
        $r = New-TRel 'pinbad' -Tag 'netplay-v1.0.8'
        { Get-DOA4Release -Ctx $r.Ctx } | Should Throw 'netplay-v1.0.8'
    }
    It 'no release of the channel yet (the fork answers [] today): a readable error' {
        $r = New-TRel 'none' -Specs @()
        $r.Set.Json | Should BeExactly '[]'
        { Get-DOA4Release -Ctx $r.Ctx } | Should Throw 'No netplay release'
    }
    It 'an API error names the HTTP status and GitHub''s message' {
        $r = New-TRel 'apierr' -ListStatus 403 -ListText '{"message":"API rate limit exceeded for 192.0.2.1.","documentation_url":"https://docs.github.com/rest"}'
        { Get-DOA4Release -Ctx $r.Ctx } | Should Throw 'HTTP 403'
        { Get-DOA4Release -Ctx $r.Ctx } | Should Throw 'API rate limit exceeded'
    }
    It 'no answer at all (status 0) is readable too' {
        $r = New-TRel 'noanswer' -ListStatus 0 -ListText 'The remote name could not be resolved: ''api.github.com'''
        { Get-DOA4Release -Ctx $r.Ctx } | Should Throw 'could not be resolved'
    }
}

Describe 'Install on an empty DOA4 folder (release mode, through Invoke-DOA4Install)' {
    $shortcuts = Join-Path $TestDrive 'inst-shortcuts'
    [void](New-Item -ItemType Directory -Force -Path $shortcuts)
    $r = New-TRel 'inst' -ShortcutFolder $shortcuts
    $sourceBefore = Get-DOA4Source -Ctx $r.Ctx
    $res = Invoke-DOA4Install -Ctx $r.Ctx

    It 'the fresh folder is in release mode (config default), and Install succeeds' {
        $sourceBefore | Should BeExactly 'release'
        $res.Ok | Should Be $true
        $res.Message | Should Match 'netplay-v1\.0\.7'
    }
    It 'release\ holds the three files of netplay-v1.0.7 byte for byte, and its SHA256SUMS.txt, nothing else' {
        foreach ($n in $script:Three) {
            Get-TFileHex (Join-Path $r.Release $n) | Should BeExactly (Get-TSha256Hex (Get-TAssetBytes 'netplay-v1.0.7' $n))
        }
        @(Get-ChildItem -LiteralPath $r.Release -Force | ForEach-Object { $_.Name } | Sort-Object) -join ',' |
            Should BeExactly 'dead_or_alive_4.exe,rexruntime.dll,SHA256SUMS.txt,TracyClient.dll'
    }
    It 'leaves no release_new or release_old' {
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_old') | Should Be $false
    }
    It 'records source = release and lastRelease (tag, time) in settings.json' {
        $j = Read-TSettingsJson $r.Root
        $j.source | Should BeExactly 'release'
        $j.lastRelease.tag | Should BeExactly 'netplay-v1.0.7'
        [string]$j.lastRelease.time | Should Not BeNullOrEmpty
        (Get-DOA4Settings (Join-Path $r.Root 'launcher\settings.json')).lastRelease.tag | Should BeExactly 'netplay-v1.0.7'
    }
    It 'asked for the list once and for each of the four files of netplay-v1.0.7 once, into release_new; never the zips' {
        $calls = @($r.Http.State.Calls)
        $calls[0].Url | Should BeExactly $script:TReleaseListUrl
        $files = @($calls | Select-Object -Skip 1)
        @($files | ForEach-Object { Split-Path -Leaf $_.Url } | Sort-Object) -join ',' | Should BeExactly 'dead_or_alive_4.exe,rexruntime.dll,SHA256SUMS.txt,TracyClient.dll'
        foreach ($c in $files) {
            $c.Url | Should BeExactly (Get-TAssetUrl 'netplay-v1.0.7' (Split-Path -Leaf $c.Url))
            $c.OutFile | Should BeExactly (Join-Path (Join-Path $r.Root 'release_new') (Split-Path -Leaf $c.Url))
        }
    }
    It 'ran no build tool (the fakes of git, cmd, winget, clang and vswhere throw)' {
        $log = [IO.File]::ReadAllText($res.LogPath)
        $log | Should Not Match 'fake (git|cmd|winget|clang|vswhere)'
    }
    It 'the log names the tag' {
        [IO.File]::ReadAllText($res.LogPath) | Should Match 'netplay-v1\.0\.7'
    }
    It 'made the desktop shortcut to the launcher' {
        Test-Path -LiteralPath (Join-Path $shortcuts 'Dead or Alive 4.lnk') | Should Be $true
    }
}

Describe 'A broken download installs nothing' {
    It 'a checksum mismatch on rexruntime.dll: Install-DOA4Release throws naming it; no release\, no release_new\' {
        $r = New-TRel 'sum1' -Specs @(@{ Tag = 'netplay-v1.0.7'; Corrupt = @('rexruntime.dll') })
        { Install-DOA4Release -Ctx $r.Ctx } | Should Throw 'rexruntime.dll'
        Test-Path -LiteralPath $r.Release | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'launcher\settings.json') | Should Be $false
    }
    It 'the same through Invoke-DOA4Install: not Ok, the Checksums step, the file named' {
        $r = New-TRel 'sum2' -Specs @(@{ Tag = 'netplay-v1.0.7'; Corrupt = @('rexruntime.dll') })
        $res = Invoke-DOA4Install -Ctx $r.Ctx
        $res.Ok | Should Be $false
        $res.Step | Should BeExactly 'Checksums'
        $res.Message | Should Match 'rexruntime\.dll'
        $res.Message | Should Not Match 'dead_or_alive_4\.exe'
        Test-Path -LiteralPath $r.Release | Should Be $false
    }
    It 'a SHA256SUMS.txt without a line for TracyClient.dll is refused by that name' {
        $r = New-TRel 'sum3' -Specs @(@{ Tag = 'netplay-v1.0.7'; NoSum = @('TracyClient.dll') })
        { Install-DOA4Release -Ctx $r.Ctx } | Should Throw 'TracyClient.dll has no line in SHA256SUMS.txt'
        Test-Path -LiteralPath $r.Release | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
    }
    It 'a file that answers HTTP 404 is refused by name, nothing left behind' {
        $r = New-TRel 'dl404' -Specs @(@{ Tag = 'netplay-v1.0.7'; Missing = @('TracyClient.dll') })
        { Install-DOA4Release -Ctx $r.Ctx } | Should Throw 'TracyClient.dll'
        { Install-DOA4Release -Ctx $r.Ctx } | Should Throw 'HTTP 404'
        Test-Path -LiteralPath $r.Release | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
    }
    It 'an Update whose download is broken leaves the installed release byte for byte' {
        $r = New-TRel 'sum4' -Specs @(@{ Tag = 'netplay-v1.0.6' }, @{ Tag = 'netplay-v1.0.7'; Corrupt = @('TracyClient.dll') }) -Tag 'netplay-v1.0.6'
        (Invoke-DOA4Install -Ctx $r.Ctx).Ok | Should Be $true
        Write-TFile (Join-Path $r.Release 'dead_or_alive_4.toml') "fullscreen = true`n"
        $before = Get-TTree $r.Release
        $before | Should Match 'dead_or_alive_4\.exe'
        $u = New-TRelCtx $r
        $res = Invoke-DOA4Update -Ctx $u.Ctx
        $res.Ok | Should Be $false
        $res.Message | Should Match 'TracyClient\.dll'
        Get-TTree $r.Release | Should BeExactly $before
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
        (Read-TSettingsJson $r.Root).lastRelease.tag | Should BeExactly 'netplay-v1.0.6'
    }
}

Describe 'Update over an installed release' {
    $r = New-TRel 'upd' -Tag 'netplay-v1.0.6'
    $inst = Invoke-DOA4Install -Ctx $r.Ctx
    $installedDll = Get-TFileHex (Join-Path $r.Release 'rexruntime.dll')
    # The game's own files beside the exe: a toml with a BOM and CRLF, and its logs.
    $toml = Join-Path $r.Release 'dead_or_alive_4.toml'
    [IO.File]::WriteAllBytes($toml, [byte[]](0xEF, 0xBB, 0xBF) + [Text.Encoding]::UTF8.GetBytes("doa4_game_files = `"D:/Games/DOA4`"`r`nfullscreen = true`r`n"))
    Write-TFile (Join-Path $r.Release 'logs\doa4 session.log') "old session`r`n"
    Write-TFile (Join-Path $r.Release 'logs\np\peer.txt') "peer`n"
    Write-TFile (Join-Path $r.Release 'imgui.ini') "[Window][x]`n"
    $tomlHash = Get-TBytesHash $toml
    $logsBefore = Get-TTree (Join-Path $r.Release 'logs')
    $u = New-TRelCtx $r
    $res = Invoke-DOA4Update -Ctx $u.Ctx

    It 'precondition: v1.0.6 was installed (pinned)' {
        $inst.Ok | Should Be $true
        $installedDll | Should BeExactly (Get-TSha256Hex (Get-TAssetBytes 'netplay-v1.0.6' 'rexruntime.dll'))
    }
    It 'installs netplay-v1.0.7''s files' {
        $res.Ok | Should Be $true
        foreach ($n in $script:Three) {
            Get-TFileHex (Join-Path $r.Release $n) | Should BeExactly (Get-TSha256Hex (Get-TAssetBytes 'netplay-v1.0.7' $n))
        }
        (Read-TSettingsJson $r.Root).lastRelease.tag | Should BeExactly 'netplay-v1.0.7'
    }
    It 'keeps the game''s toml byte for byte and its logs folder' {
        Get-TBytesHash $toml | Should BeExactly $tomlHash
        Get-TTree (Join-Path $r.Release 'logs') | Should BeExactly $logsBefore
        $logsBefore | Should Match 'peer\.txt'
    }
    It 'names in the log what it does not carry over (imgui.ini)' {
        [IO.File]::ReadAllText($res.LogPath) | Should Match 'imgui\.ini'
    }
    It 'leaves no release_new or release_old' {
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_old') | Should Be $false
    }
    It 'Update when netplay-v1.0.7 is installed: "nothing new", one API call and nothing else, nothing changed' {
        $before = Get-TTree $r.Release
        $n = $r.Http.State.Calls.Count
        $again = Invoke-DOA4Update -Ctx (New-TRelCtx $r).Ctx
        $again.Ok | Should Be $true
        $again.Message | Should BeExactly 'nothing new: netplay-v1.0.7 is installed'
        $r.Http.State.Calls.Count - $n | Should Be 1
        $r.Http.State.Calls[$n].Url | Should BeExactly $script:TReleaseListUrl
        Get-TTree $r.Release | Should BeExactly $before
    }
    It 'the same tag with a file missing from release\ is downloaded again, not "nothing new"' {
        Remove-Item -LiteralPath (Join-Path $r.Release 'TracyClient.dll')
        $again = Invoke-DOA4Update -Ctx (New-TRelCtx $r).Ctx
        $again.Ok | Should Be $true
        $again.Message | Should BeExactly 'Updated to netplay-v1.0.7.'
        Get-TFileHex (Join-Path $r.Release 'TracyClient.dll') | Should BeExactly (Get-TSha256Hex (Get-TAssetBytes 'netplay-v1.0.7' 'TracyClient.dll'))
        Get-TBytesHash $toml | Should BeExactly $tomlHash
    }
    It 'Update with nothing installed says to run Install, and asks GitHub nothing' {
        $e = New-TRel 'upd-empty'
        $x = Invoke-DOA4Update -Ctx $e.Ctx
        $x.Ok | Should Be $false
        $x.Message | Should BeExactly 'Not installed yet: run Install first.'
        # The release path's refusal, not the source path's (same words, step 'Tools'): the
        # probe's dispatch-removed mutant passed this check until the step was asserted.
        $x.Step | Should BeExactly 'Release'
        $e.Http.State.Calls.Count | Should Be 0
    }
}

Describe 'A running game blocks Install and Update' {
    $r = New-TRel 'run' -Tag 'netplay-v1.0.6'
    $inst = Invoke-DOA4Install -Ctx $r.Ctx
    $exe = Join-Path $r.Release 'dead_or_alive_4.exe'

    It 'precondition: v1.0.6 is installed' {
        $inst.Ok | Should Be $true
        Get-TFileHex $exe | Should BeExactly (Get-TSha256Hex (Get-TAssetBytes 'netplay-v1.0.6' 'dead_or_alive_4.exe'))
    }
    It 'Update while the release''s exe runs (its path in other letter case) is refused; nothing changes, GitHub is not asked' {
        $before = Get-TTree $r.Release
        $n = $r.Http.State.Calls.Count
        $u = New-TRelCtx $r -Running @('C:\Elsewhere\dead_or_alive_4.exe', $exe.ToUpperInvariant())
        $res = Invoke-DOA4Update -Ctx $u.Ctx
        $res.Ok | Should Be $false
        $res.Message | Should Match 'running'
        Get-TTree $r.Release | Should BeExactly $before
        $r.Http.State.Calls.Count | Should Be $n
        @($u.Cmd.Renames).Count | Should Be 0
    }
    It 'Install-DOA4Release throws the same' {
        $u = New-TRelCtx $r -Running @($exe)
        { Install-DOA4Release -Ctx $u.Ctx } | Should Throw 'running'
    }
    It 'a dead_or_alive_4.exe running from another folder does not block' {
        $u = New-TRelCtx $r -Running @('C:\Elsewhere\dead_or_alive_4.exe', (Join-Path $r.Root 'doa4-recomp\out\build\win-amd64-release\dead_or_alive_4.exe'))
        $res = Invoke-DOA4Update -Ctx $u.Ctx
        $res.Ok | Should Be $true
        Get-TFileHex $exe | Should BeExactly (Get-TSha256Hex (Get-TAssetBytes 'netplay-v1.0.7' 'dead_or_alive_4.exe'))
    }
}

Describe 'The swap: a failed rename puts the old release back' {
    It 'the second rename (release_new -> release) failing leaves the old release\ in place, byte for byte' {
        $r = New-TRel 'swap1' -Tag 'netplay-v1.0.6'
        (Invoke-DOA4Install -Ctx $r.Ctx).Ok | Should Be $true
        Write-TFile (Join-Path $r.Release 'dead_or_alive_4.toml') "fullscreen = false`n"
        $before = Get-TTree $r.Release
        $u = New-TRelCtx $r -FailRename 'release_new'
        $res = Invoke-DOA4Update -Ctx $u.Ctx
        $res.Ok | Should Be $false
        $res.Message | Should Match 'previous release'
        @($u.Cmd.Renames) -join '; ' | Should BeExactly 'release -> release_old; release_new -> release; release_old -> release'
        Get-TTree $r.Release | Should BeExactly $before
        Test-Path -LiteralPath (Join-Path $r.Root 'release_old') | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
        (Read-TSettingsJson $r.Root).lastRelease.tag | Should BeExactly 'netplay-v1.0.6'
    }
    It 'the first rename (release -> release_old) failing changes nothing' {
        $r = New-TRel 'swap2' -Tag 'netplay-v1.0.6'
        (Invoke-DOA4Install -Ctx $r.Ctx).Ok | Should Be $true
        $before = Get-TTree $r.Release
        $u = New-TRelCtx $r -FailRename 'release'
        $res = Invoke-DOA4Update -Ctx $u.Ctx
        $res.Ok | Should Be $false
        Get-TTree $r.Release | Should BeExactly $before
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
    }
    It 'an earlier swap cut off after its first rename (release_old\, no release\) is put back before anything else' {
        $r = New-TRel 'swap3' -Tag 'netplay-v1.0.6'
        (Invoke-DOA4Install -Ctx $r.Ctx).Ok | Should Be $true
        $before = Get-TTree $r.Release
        [IO.Directory]::Move($r.Release, (Join-Path $r.Root 'release_old'))
        Write-TFile (Join-Path $r.Root 'release_new\dead_or_alive_4.exe') 'half a download'
        $u = New-TRelCtx $r -Tag 'netplay-v1.0.6'
        $res = Invoke-DOA4Update -Ctx $u.Ctx
        $res.Ok | Should Be $true
        $res.Message | Should BeExactly 'nothing new: netplay-v1.0.6 is installed'
        Get-TTree $r.Release | Should BeExactly $before
        Test-Path -LiteralPath (Join-Path $r.Root 'release_old') | Should Be $false
        Test-Path -LiteralPath (Join-Path $r.Root 'release_new') | Should Be $false
    }
    It 'a release_old\ left beside release\ (a delete that failed earlier) is removed, release\ untouched' {
        $r = New-TRel 'swap4' -Tag 'netplay-v1.0.6'
        (Invoke-DOA4Install -Ctx $r.Ctx).Ok | Should Be $true
        $before = Get-TTree $r.Release
        Write-TFile (Join-Path $r.Root 'release_old\dead_or_alive_4.exe') 'stale'
        $res = Invoke-DOA4Update -Ctx (New-TRelCtx $r -Tag 'netplay-v1.0.6').Ctx
        $res.Ok | Should Be $true
        Get-TTree $r.Release | Should BeExactly $before
        Test-Path -LiteralPath (Join-Path $r.Root 'release_old') | Should Be $false
    }
}

Describe 'Existing source installs keep building from source' {
    $t = New-TInstalled -Base (Join-Path $TestDrive 'srcinst')
    # settings.json as the launcher before release mode wrote it: no "source".
    $settings = Join-Path $t.Root 'launcher\settings.json'
    $old = [IO.File]::ReadAllText($settings) | ConvertFrom-Json
    [IO.File]::WriteAllText($settings, (ConvertTo-Json -InputObject ([ordered]@{ gameFiles = ''; lastBuild = $old.lastBuild }) -Depth 4))
    $cfg = $t.Config.Clone()
    $cfg.DefaultSource = 'release'
    $http = New-FakeHttp -ListJson (New-TReleaseSet (Get-TStandardSpecs)).Json
    $cmd = New-FakeTable $t.Tools -Cmd $t.Build -RealGit
    $cmd.http = $http.Invoke
    $cmd.processPaths = { throw 'fake processPaths: the source path does not ask' }
    $cmd.rename = { throw 'fake rename: the source path does not rename' }
    $ctx = New-DOA4Context -Root $t.Root -Config $cfg -Cmd $cmd -Action 'update'

    It 'precondition: installed from source, settings.json has no source, the config default is release' {
        $t.Install.Ok | Should Be $true
        Test-Path -LiteralPath (Join-Path $t.Root 'doa4-recomp\.git') | Should Be $true
        ([IO.File]::ReadAllText($settings) | ConvertFrom-Json).PSObject.Properties['source'] | Should BeNullOrEmpty
        $cfg.DefaultSource | Should BeExactly 'release'
    }
    It 'Get-DOA4Source says source' {
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'source'
    }
    It 'Update takes the git path ("Updated and rebuilt."), with no request to GitHub and no release folder' {
        $r = Invoke-DOA4Update -Ctx $ctx
        $r.Message | Should BeExactly 'Updated and rebuilt.'
        $r.Ok | Should Be $true
        [IO.File]::ReadAllText($r.LogPath) | Should Match '\[Sources\] done'
        $http.State.Calls.Count | Should Be 0
        Test-Path -LiteralPath (Join-Path $t.Root 'release') | Should Be $false
    }
    It 'Play still starts the built exe from the build folder' {
        Get-DOA4ExePath -Ctx $ctx | Should BeExactly $ctx.ExePath
        # An upstream build (no first-run setup) needs the game files folder.
        $psi = Get-DOA4GameStartInfo -Ctx $ctx -GameFiles 'D:\Games\DOA4'
        $psi.FileName | Should BeExactly $ctx.ExePath
        $psi.WorkingDirectory | Should BeExactly $ctx.BuildDir
    }
}

Describe 'Get-DOA4Source and Set-DOA4Source' {
    It 'a fresh folder with the shipped config.ps1 is in release mode' {
        $cfg = & (Join-Path $script:Src 'launcher\config.ps1')
        $ctx = New-DOA4Context -Root (Join-Path $TestDrive 'src-shipped') -Config $cfg -Cmd @{}
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'release'
    }
    It 'a config.ps1 from before release mode (no DefaultSource) builds from source' {
        $cfg = New-TestConfig
        $cfg.Remove('DefaultSource')
        $ctx = New-DOA4Context -Root (Join-Path $TestDrive 'src-oldcfg') -Config $cfg -Cmd @{}
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'source'
    }
    It 'the saved choice wins over a doa4-recomp clone: source = release' {
        $ctx = New-DOA4Context -Root (Join-Path $TestDrive 'src-a') -Config (New-TestConfig) -Cmd @{}
        [void](New-Item -ItemType Directory -Force -Path (Join-Path $ctx.GamePath '.git'))
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'source'
        Set-DOA4Source -Ctx $ctx -Source 'release'
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'release'
    }
    It 'the saved choice wins over the config default: source = source on a fresh folder' {
        $ctx = New-DOA4Context -Root (Join-Path $TestDrive 'src-b') -Config (New-TReleaseConfig) -Cmd @{}
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'release'
        Set-DOA4Source -Ctx $ctx -Source 'source'
        Get-DOA4Source -Ctx $ctx | Should BeExactly 'source'
        (Read-TSettingsJson $ctx.Root).source | Should BeExactly 'source'
    }
    It 'Set-DOA4Source keeps the other settings' {
        $ctx = New-DOA4Context -Root (Join-Path $TestDrive 'src-c') -Config (New-TReleaseConfig) -Cmd @{}
        Save-DOA4Settings -Path $ctx.SettingsPath -Settings @{ gameFiles = 'D:\Games\DOA4'; lastBuild = @{ game = 'g1'; sdk = 's1'; time = 't1' } }
        Set-DOA4Source -Ctx $ctx -Source 'release'
        $j = Read-TSettingsJson $ctx.Root
        $j.source | Should BeExactly 'release'
        $j.gameFiles | Should BeExactly 'D:\Games\DOA4'
        $j.lastBuild.game | Should BeExactly 'g1'
    }
    It 'Set-DOA4Source refuses any other word' {
        $ctx = New-DOA4Context -Root (Join-Path $TestDrive 'src-d') -Config (New-TReleaseConfig) -Cmd @{}
        # The message, not just any throw: a missing command throws too (this check passed in RED on that).
        { Set-DOA4Source -Ctx $ctx -Source 'releases' } | Should Throw 'does not belong to the set'
        Test-Path -LiteralPath $ctx.SettingsPath | Should Be $false
        Set-DOA4Source -Ctx $ctx -Source 'release'
        (Read-TSettingsJson $ctx.Root).source | Should BeExactly 'release'
    }
}

Describe 'Play and the folders in release mode' {
    $r = New-TRel 'play'
    $notYet = Get-DOA4BuildStatus -Ctx $r.Ctx
    $playBefore = $null
    try { Start-DOA4Game -Ctx $r.Ctx | Out-Null } catch { $playBefore = $_.Exception.Message }
    Install-DOA4Release -Ctx $r.Ctx | Out-Null

    It 'Play before Install says to run Install' {
        $playBefore | Should Match 'run Install'
    }
    It 'Get-DOA4ExePath and the start info use release\ (exe and working folder), no game path' {
        Get-DOA4ExePath -Ctx $r.Ctx | Should BeExactly (Join-Path $r.Root 'release\dead_or_alive_4.exe')
        $psi = Get-DOA4GameStartInfo -Ctx $r.Ctx
        $psi.FileName | Should BeExactly (Join-Path $r.Root 'release\dead_or_alive_4.exe')
        $psi.WorkingDirectory | Should BeExactly (Join-Path $r.Root 'release')
        $psi.UseShellExecute | Should Be $true
        $psi.Arguments | Should BeExactly (ConvertTo-DOA4ArgumentString @('--user_data_root=' + (Join-Path $r.Root 'userdata')))
    }
    It 'a release has the first-run setup (there is no doa4-recomp folder at all)' {
        Test-Path -LiteralPath (Join-Path $r.Root 'doa4-recomp') | Should Be $false
        Test-DOA4HasFirstRunSetup $r.Ctx | Should Be $true
    }
    It 'the status is not built before Install and built after' {
        $notYet | Should BeExactly 'not built'
        Get-DOA4BuildStatus -Ctx $r.Ctx | Should BeExactly 'built'
    }
    It 'the folders: logs and the photo folder setting come from release\' {
        Write-TFile (Join-Path $r.Release 'dead_or_alive_4.toml') "doa4_photo_dir = `"D:/Shots`"`n"
        $f = Get-DOA4FolderTargets -Ctx $r.Ctx
        $f.Photos | Should BeExactly 'D:/Shots'
        $f.Logs | Should BeExactly (Join-Path $r.Root 'release\logs')
        $f.Saves | Should BeExactly (Join-Path $r.Root 'userdata')
    }
    $realXex = if ($env:DOA4_ROOT) { Join-Path $env:DOA4_ROOT 'xex\DOA4' } else { Join-Path $PSScriptRoot '..\..\..\xex\DOA4' }
    $realXex = [IO.Path]::GetFullPath($realXex)
    if (Test-Path -LiteralPath (Join-Path $realXex 'default.xex')) {
        It 'Change game files writes release\dead_or_alive_4.toml (real game folder, read only)' {
            Set-DOA4GameFiles -Ctx $r.Ctx -Folder $realXex | Should BeExactly 'toml'
            Get-DOA4TomlString -Path (Join-Path $r.Release 'dead_or_alive_4.toml') -Key 'doa4_game_files' | Should BeExactly $realXex.Replace('\', '/')
            Test-Path -LiteralPath $r.Ctx.TomlPath | Should Be $false
        }
    }
}

Describe 'The shipped config.ps1 (release mode)' {
    It 'names the fork, the netplay channel, the three files, no pin, and release as the default' {
        $c = & (Join-Path $script:Src 'launcher\config.ps1')
        $c.ReleaseRepo | Should BeExactly 'Dwreckgg/doa4-recomp'
        $c.ReleaseChannel | Should BeExactly 'netplay'
        @($c.ReleaseAssets) -join ',' | Should BeExactly 'dead_or_alive_4.exe,rexruntime.dll,TracyClient.dll'
        $c.ReleaseTag | Should BeExactly ''
        $c.DefaultSource | Should BeExactly 'release'
    }
}

Describe 'The real command table (release mode entries)' {
    $t = New-DOA4CommandTable

    It 'has http, processPaths and rename' {
        foreach ($k in @('http', 'processPaths', 'rename')) { $t.ContainsKey($k) | Should Be $true }
    }
    It 'processPaths finds this PowerShell process by its path' {
        $mine = (Get-Process -Id $PID).Path
        $mine | Should Not BeNullOrEmpty
        $name = [IO.Path]::GetFileNameWithoutExtension($mine)
        @(& $t.processPaths $name) -contains $mine | Should Be $true
        @(& $t.processPaths 'no-such-process-doa4-test').Count | Should Be 0
    }
    It 'rename moves a folder with its files' {
        $a = Join-Path $TestDrive 'mv a'
        Write-TFile (Join-Path $a 'x.txt') 'x'
        & $t.rename $a (Join-Path $TestDrive 'mv b')
        Test-Path -LiteralPath $a | Should Be $false
        [IO.File]::ReadAllText((Join-Path $TestDrive 'mv b\x.txt')) | Should BeExactly 'x'
    }
    It 'http (a local HTTP server): text with the launcher''s User-Agent, a binary file, a 404 without throwing, a redirect followed' {
        $server = {
            param($prefix, $count, $seen)
            $l = New-Object System.Net.HttpListener
            $l.Prefixes.Add($prefix)
            $l.Start()
            try {
                for ($i = 0; $i -lt $count; $i++) {
                    $ar = $l.BeginGetContext($null, $null)
                    if (-not $ar.AsyncWaitHandle.WaitOne(15000)) { break }
                    $c = $l.EndGetContext($ar)
                    [void]$seen.Add(('{0}|{1}' -f $c.Request.Url.AbsolutePath, $c.Request.UserAgent))
                    $resp = $c.Response
                    $body = [byte[]]@()
                    switch ($c.Request.Url.AbsolutePath) {
                        '/list' { $resp.StatusCode = 200; $body = [Text.Encoding]::UTF8.GetBytes('[{"tag_name":"netplay-v1.0.7"}]') }
                        '/file' { $resp.StatusCode = 200; $body = [byte[]](0, 1, 2, 255, 13, 10, 0) }
                        '/moved' { $resp.StatusCode = 302; $resp.RedirectLocation = $prefix + 'file' }
                        default { $resp.StatusCode = 404; $body = [Text.Encoding]::UTF8.GetBytes('{"message":"Not Found"}') }
                    }
                    $resp.ContentLength64 = $body.Length
                    if ($body.Length -gt 0) { $resp.OutputStream.Write($body, 0, $body.Length) }
                    $resp.Close()
                }
            } finally { $l.Close() }
        }
        $seen = [System.Collections.ArrayList]::Synchronized((New-Object System.Collections.ArrayList))
        $port = Get-Random -Minimum 30000 -Maximum 60000
        $prefix = "http://localhost:$port/"
        $ps = [PowerShell]::Create()
        [void]$ps.AddScript($server).AddArgument($prefix).AddArgument(5).AddArgument($seen)
        $h = $ps.BeginInvoke()
        try {
            Start-Sleep -Milliseconds 500
            $text = & $t.http ($prefix + 'list') ''
            $out = Join-Path $TestDrive 'http-file.bin'
            $file = & $t.http ($prefix + 'file') $out
            $missing = & $t.http ($prefix + 'missing') ''
            $out2 = Join-Path $TestDrive 'http-moved.bin'
            $moved = & $t.http ($prefix + 'moved') $out2
            $text.Status | Should Be 200
            $text.Text | Should BeExactly '[{"tag_name":"netplay-v1.0.7"}]'
            $file.Status | Should Be 200
            ([IO.File]::ReadAllBytes($out) -join ',') | Should BeExactly '0,1,2,255,13,10,0'
            $missing.Status | Should Be 404
            $missing.Text | Should Match 'Not Found'
            $moved.Status | Should Be 200
            ([IO.File]::ReadAllBytes($out2) -join ',') | Should BeExactly '0,1,2,255,13,10,0'
            @($seen | Where-Object { $_ -notmatch '\|DOA4-launcher$' }).Count | Should Be 0
            @($seen).Count | Should Be 5
        } finally {
            if (-not $h.IsCompleted) { $ps.Stop() } else { [void]$ps.EndInvoke($h) }
            $ps.Dispose()
        }
    }
}

Disable-TFileProtocol
