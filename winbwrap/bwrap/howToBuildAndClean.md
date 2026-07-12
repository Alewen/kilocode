# 如何编译和清理

-.\build.ps1                                    # 编译 Release x64 (默认)
-.\build.ps1 -Clean                             # 清理 Release x64 (默认)
-.\build.ps1 -Config Debug -Arch Win32          # 编译 Debug Win32
-.\build.ps1 -Config Debug -Arch Win32 -Clean   # 清理 Debug Win32