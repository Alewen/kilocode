& git diff --name-only refs/tags/v7.4.5 HEAD | Where-Object { $_ -notmatch '^winbwrap/' }

& git diff --name-status refs/tags/v7.4.5 HEAD | Where-Object { $_ -notmatch '\twinbwrap/' }

& git branch --list v7.4.5
& git tag --list v7.4.5 
& git log --oneline -3 --decorate v7.4.5
& git log --oneline -3 --decorate refs/tags/v7.4.5
& git log --oneline -3 --decorate refs/heads/v7.4.5

# 切换到准确的分支，而不是标签
# & git checkout refs/heads/v7.4.5
