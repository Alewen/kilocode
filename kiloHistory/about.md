# 项目背景

- 该项目是一个本地运行，可以查看 kilo 插件记录的，用户和 ai 全部历史对话的 web 项目
- kilo 插件是一个可以安装在 vscode 里面的 vsix 插件，它提供了用户和 ai 对话渠道
- kilo 插件会自动将用户和 ai 对话的全部信息，记录在 kilo.db 数据中
- 这个项目的作用就是原封不动的读取 kilo.db 的历史对话
- 这个项目是完全模拟，kilo 插件浏览 ai 对话历史记录的，只是把这个展示效果通过 web ，纯静态展示的
- Windows 下 kilo 插件的源码目录在 C:\Share_Kilo\kilocode
- Linux 下 kilo 插件的源码目录在 ~/kilocode

## AI 对话消息类型分类

### 1. Part Type（片段类型）— 6 种
| 类型 | 用途 |
|------|------|
| `reasoning` | AI 推理/思考过程，可折叠显示 |
| `text` | 普通文本回复（Markdown 渲染） |
| `tool` | 工具调用及结果（内部再分子类型） |
| `step-start` | 步骤开始标记（不渲染） |
| `step-finish` | 步骤结束标记（含 token 用量） |
| `compaction` | 对话压缩/摘要标记 |

### 2. Tool Type（工具子类型）— 13 种
| 工具 | 用途 | 图标 |
|------|------|------|
| `bash` | 执行 Shell 命令 | `>_` |
| `read` | 读取文件 | 📖 |
| `list` | 列出目录 | 📖 |
| `write` / `edit` / `apply_patch` | 写入/编辑文件/应用补丁 | ✎ |
| `glob` / `grep` | 搜索文件/搜索内容 | 🔍 |
| `question` | 向用户提问 | ? |
| `task` | 子任务 | ⚡ |
| `skill` | 执行技能 | 🔧 |
| `todowrite` / `todoread` | 待办事项读/写 | ◆ |

### 3. 代码位置
- Part Type 定义：`server.py:99`
- Tool Icon 映射：`index.html:449-488`
- 前端渲染入口：`index.html:862`（`renderParts()` 函数）

## web 页面可折叠的部分

AI 对话区域（右侧）共有 3 类可折叠元素，点击 header 通过 `togglePart()` 切换 `.open` 类控制显隐：

### 1. `reasoning-part` — 推理片段
- **Header**: 🧠 推理 + 前80字摘要
- **Body**: `.reasoning-body`（默认 `display: none`，`.open` 时 `display: block`）
- **代码**: `index.html:868-878`（渲染），`kiloIndex.css:297-365`（样式）

### 2. `tool-part` — 工具调用片段（除 `todowrite` 外全部工具）
- **Header**: 工具图标 + 标题 + 副标题 + ▶ 箭头
- **Body**: `.tool-body`（默认 `display: none`，`.open` 时 `display: block`）
- **覆盖工具**: `bash`、`read`、`write`、`edit`、`apply_patch`、`glob`、`grep`、`list`、`question`、`task`、`skill`、`todoread` 及其他未知工具
- **代码**: `index.html:899-913`（渲染），`kiloIndex.css:399-462`（样式）

### 3. `todo-part` — 待办事项片段（仅 `todowrite`）
- **Header**: ✓ 待办事项 + ▶ 箭头
- **Body**: `.todo-body`（默认 `display: none`，`.open` 时 `display: block`）
- **代码**: `index.html:886-898`（渲染），`kiloIndex.css:933-990`（样式）

## web 页面不可折叠的部分

### 1. `text` — 文本回复
- 直接渲染 Markdown（含代码高亮），无折叠交互
- **代码**: `index.html:879-884`

### 2. `compaction` — 对话压缩标记
- 虚线边框静态文本 `[对话压缩]`，无折叠交互
- **代码**: `index.html:923-927`

### 3. `step-start` / `step-finish` — 步骤标记
- `step-start`：不渲染
- `step-finish`：仅提取 token 用量信息，不渲染可见元素
- **代码**: `index.html:916-922`
