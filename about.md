# 项目背景

- 当前项目是 vscode 的 kilo 插件源代码，它可以跨平台编译生成 vsix 的插件，然后被安装在 vscode 上面
- kilo 插件的作用是提供用户和 AI 的沟通渠道，让用户可以在 ai 协助下进行软件开发
- 该项目从开源 GitHub 获取，用户新建了分支，从 67b815466c 开始修改
- 可以通过 git 查看提交日志，获知自 67b815466c 以来，用户的重要功能修改

# 直接通过如下命令查询，自 67b815466c 以来的文件修改名单

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

# 配置文件路径
- ~/.config/kilo/kilo.jsonc

# skill 路径
 /home/shaoke/.kilo/skills
 /home/shaoke/.kilo -- 可读可写

# 数据库路劲
 /home/shaoke/.local/share/kilo/
 /home/shaoke/.local/share/kilo -- 只读
