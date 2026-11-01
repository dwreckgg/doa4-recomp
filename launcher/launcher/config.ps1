# DOA4 launcher configuration. Data only.
# To build other sources (for example your own published branches), change the repository
# URLs and refs below. A ref is a branch name, or a commit id (7 to 40 hex digits), which is
# checked out as a fixed version and never moves on Update.
@{
    # The game port. Default: upstream, its default branch (confirmed 2026-10-28).
    GameRepoUrl    = 'https://github.com/parkerallan/doa4-recomp'
    GameRef        = 'main'

    # The ReXGlue SDK. e8ce24f is the SDK version the port's code was generated with.
    SdkRepoUrl     = 'https://github.com/rexglue/rexglue-sdk'
    SdkRef         = 'e8ce24f'

    # Tools, installed with winget after one confirmation when missing.
    WingetGitId    = 'Git.Git'
    WingetLlvmId   = 'LLVM.LLVM'
    LlvmVersion    = '23.1.4'
    WingetVsId     = 'Microsoft.VisualStudio.BuildTools'
    VsWorkload     = 'Microsoft.VisualStudio.Workload.VCTools'
    VsVersionRange = '[18,19)'

    # Sizes shown before installing (installed size on a PC that has them, 2026-10-28).
    GitSize        = 'about 0.4 GB'
    LlvmSize       = 'about 3 GB'
    VsSize         = 'about 6 GB with the Windows SDK'

    # Free space needed on the DOA4 folder's drive for the sources and the build, in GB.
    # Measured 2026-10-28: about 2.0 GB after a full upstream build (sources, submodules and
    # build folders); twice that leaves room for rebuilds and logs.
    RequiredFreeGB = 4

    # Where the game comes from in a new DOA4 folder: 'release' (Install and Update download
    # the newest release's files from GitHub, check each against the release's SHA256SUMS.txt
    # and swap the release folder whole) or 'source' (clone and build, everything above).
    # The window's choice, saved in launcher\settings.json, wins; a DOA4 folder that already
    # holds a doa4-recomp clone keeps building from source.
    DefaultSource  = 'release'

    # The releases: GitHub repository, tag channel (tags '<channel>-v<a.b.c>') and the files
    # a release must have (besides SHA256SUMS.txt). Empty ReleaseTag: the newest release; a
    # tag (for example 'netplay-v1.0.7') pins that release.
    ReleaseRepo    = 'Dwreckgg/doa4-recomp'
    ReleaseChannel = 'netplay'
    ReleaseAssets  = @('dead_or_alive_4.exe', 'rexruntime.dll', 'TracyClient.dll')
    ReleaseTag     = ''
}
