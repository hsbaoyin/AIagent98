# Scoring hook for the multifile task: two implementation files.
$cfg = @{
    Sources            = @('src\idset.c', 'src\idfmt.c')
    IncludeDir         = 'include'
    VisibleTest        = 'tests\test_visible.c'
    HiddenTest         = 'test_hidden.c'
    VisiblePassPattern = 'all visible tests passed'
    HiddenScorePattern = 'HIDDEN SCORE:\s*(\d+)/(\d+)'
    Required           = @('Makefile', 'include\idset.h', 'src\idset.c', 'src\idfmt.c', 'tests\test_visible.c', 'run_tests.bat')
    Integrity          = @(
        @{ Workspace = 'include\idset.h'; Pristine = 'pristine_idset.h' }
    )
}
