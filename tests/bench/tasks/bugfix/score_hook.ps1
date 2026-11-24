# Scoring hook for the bugfix task.
$cfg = @{
    Sources            = @('src\counter.c')
    IncludeDir         = 'include'
    VisibleTest        = 'tests\test_visible.c'
    HiddenTest         = 'test_hidden.c'
    VisiblePassPattern = 'all visible tests passed'
    HiddenScorePattern = 'HIDDEN SCORE:\s*(\d+)/(\d+)'
    Required           = @('Makefile', 'include\counter.h', 'tests\test_visible.c', 'run_tests.bat')
    Integrity          = @(
        @{ Workspace = 'include\counter.h'; Pristine = 'pristine_counter.h' }
    )
}
