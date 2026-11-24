# Scoring hook for the csv_parse task.  $cfg replaces the scorer defaults.
$cfg = @{
    Sources            = @('src\strutil.c')
    IncludeDir         = 'include'
    VisibleTest        = 'tests\test_visible.c'
    HiddenTest         = 'test_hidden.c'
    VisiblePassPattern = 'all visible tests passed'
    HiddenScorePattern = 'HIDDEN SCORE:\s*(\d+)/(\d+)'
    Required           = @('Makefile', 'include\strutil.h', 'tests\test_visible.c', 'run_tests.bat')
    Integrity          = @(
        @{ Workspace = 'include\strutil.h'; Pristine = 'pristine_strutil.h' }
    )
}
