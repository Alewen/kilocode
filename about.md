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

# 自 8882b523e3 以来的功能变更清单

| 功能 | 修改位置 |
|------|----------|
| 1. 限制 AI 对工作区外文件进行操作（write/edit/bash/apply_patch） | packages/opencode/src/kilocode/session/prompt.ts |
|  | packages/opencode/src/session/prompt.ts |
| 2. Linux 下限制危险命令（echo/printf/tee/touch） | packages/opencode/src/kilocode/session/prompt.ts |
| 3. Bwrap 沙箱隔离 bash 执行 | packages/opencode/src/tool/shell.ts |
|  | packages/opencode/src/config/config.ts |
|  | packages/opencode/test/config/global-config-init.test.ts |
|  | bwrap-example.json |
| 4. Windows 下识别 pwsh 路径别名 | packages/opencode/src/util/which.ts |
|  | packages/opencode/test/util/which.test.ts |
| 5. opencode 工具加载失败不中断对话（容错） | packages/opencode/src/tool/registry.ts |
| 6. 禁止 plan 模式自动退出 | packages/opencode/src/kilocode/plan-followup.ts |
| 7. 禁止插件主动关闭侧栏 | packages/kilo-vscode/src/extension.ts |
| 8. Edit 工具增加编码检测提示 | packages/opencode/src/tool/edit.txt |
| 9. 调整编译与发布流程 | .husky/pre-push |
|  | packages/opencode/script/build.ts |
|  | bun.lock |
|  | packages/kilo-vscode/.gitignore |
|  | kilo-compile-linux-64-vsix.sh |
|  | kilo-compile-windows-x64-vsix.ps1 |
| 10. 大模型收藏夹排序与显示优化 | packages/kilo-vscode/webview-ui/src/components/shared/ModelSelector.tsx |
| 11. UI 显示问题修复（自动滚屏冲突 / Windows 输出乱码 / 推理显示不完全） | packages/kilo-ui/src/hooks/create-auto-scroll.tsx |
|  | packages/opencode/src/tool/shell.ts |
|  | packages/opencode/src/kilocode/tool/shell-output.ts |
|  | packages/opencode/test/kilocode/shell-output.test.ts |
|  | packages/ui/src/context/marked.tsx |
| 12. soul.txt / default.txt 双语化 | packages/opencode/src/kilocode/soul.txt |
|  | packages/opencode/src/session/prompt/default.txt |
| 13. README 补充说明 | README.md |
| 14. kiloHistory（离线查看对话记录） | kiloHistory/ |
| 15. extraSkills（自定义技能扩展） | extraSkills/ |
| 16. kilocli（手工工具集，可追溯执行会话中的 edit/write） | kilocli/ |
| 17. Git 换行符自动转换关闭（避免跨平台文件差异） | .gitattributes |
| 18. Fork 仓库地址更新 | packages/kilo-vscode/package.json |
|  | packages/plugin/package.json |
|  | packages/sdk/js/package.json |
| 19. SDK 事件类型扩展（AgentManager / Indexing 等 Kilo 特有事件） | packages/sdk/js/src/v2/gen/types.gen.ts |
