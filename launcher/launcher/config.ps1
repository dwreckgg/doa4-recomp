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
}
