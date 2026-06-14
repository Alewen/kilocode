# 项目背景

- 当前项目是 vscode 的 kilo 插件源代码，它可以跨平台编译生成 vsix 的插件，然后被安装在 vscode 上面
- kilo 插件的作用是提供用户和 AI 的沟通渠道，让用户可以在 ai 协助下进行软件开发
- 该项目已被纳入 git 管理，从版本 8882b52 至今，已经有多次迭代
- 8882b52 版本以及之前属于开源项目，之后的修改是用户个人使用的修改
- 可以通过 git diff --name-only 比较最新的版本和最初的版本 8882b52 查看用户历次文件修改
- 用户对这个源码，自 8882b52 以来，所作的修改，主要是限制 ai 对工作区以外的文件进行操作
- 用户修改也限制 ai 对 write / edit / bash 等工具的调用范围，但是没有限制 read

# 直接通过如下命令查询，自 8882b52 以来的文件修改名单

- 需要排除的文件夹有 kilocli/
- 需要排除的文件夹有 kiloHistory/
- 需要排除的文件夹有 extraSkills/
- 需要排除的文件有 ChineseChangLog.md
- 需要排除的文件有 about.md
- 需要排除的文件有 kilo-compile-linux-64-vsix.sh
- 需要排除的文件有 kilo-compile-windows-x64-vsix.ps1，
- 需要排除的文件有 bwrap-example.json

- git diff --name-status 8882b52 HEAD -- . \
':!kilocli/**' \
':!kiloHistory/**' \
':!extraSkills/**' \
':!ChineseChangLog.md' \
':!about.md' \
':!kilo-compile-linux-64-vsix.sh' \
':!kilo-compile-windows-x64-vsix.ps1' \
':!bwrap-example.json'
