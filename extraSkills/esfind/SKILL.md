---
name: esfind
description: Windows 下通过 Everything NTFS 索引引擎（es.exe）按文件名/路径即时搜索文件，比 glob/grep 快 10-100 倍。当用户需要查找特定名称的文件、不确定文件位置、或需要跨磁盘搜索时使用。
---

# Esfind

## Overview

利用 Everything 维护的 NTFS 索引数据库，通过 `scripts/es.exe` 实现亚秒级文件名搜索。搜索范围覆盖所有 NTFS 卷，不受工作目录限制。

**适用场景：**
- 已知文件名或部分名称，但不确定文件在哪
- 需要跨磁盘/全盘快速查找文件
- 需要比 `glob` 更快的大范围文件名匹配
- 需要组合文件名、路径、日期、大小等条件筛选

**不适用的场景：**
- 搜索文件内容中的文本 → 使用 `grep` 工具
- 需要搜索 Linux/Mac 系统 → Everything 仅支持 Windows NTFS

## Decision Tree

```
用户要找文件？
├── 知道文件名/部分名称 → 使用 es.exe
│   ├── 需要精确定位特定目录 → 加 -path <路径>
│   ├── 需要精确全词匹配 → 加 -w
│   ├── 结果太多 → 加 -n <数量> 或加更多关键词
│   ├── 需要按日期/大小排序 → 加 -sort <字段>
│   └── 需要查看额外属性 → 加 -size -date-modified 等列参数
├── 要找文件内容中的文本 → 使用 grep（不适用 esfind）
└── 想在工作区按模式浏览 → 使用 glob（不适用 esfind）
```

## 调用方式

es.exe 在此 skill 的 `scripts/es.exe` 路径。从 skill 目录执行时使用相对路径，从其他目录执行时使用完整路径。

```powershell
# 从 esfind skill 目录执行：
scripts\es.exe <搜索词>

# 从任意目录执行（需完整路径）：
& "<KILO_CONFIG>\skills\esfind\scripts\es.exe" <搜索词>
```

调用示例：

```powershell
scripts\es.exe setLineEnd
```

## 搜索语法详解

### 基础搜索

按文件名搜索（不区分大小写，默认匹配文件名部分）：

```powershell
scripts\es.exe setLineEnd
```

### 通配符搜索

支持 `*`（任意字符）和 `?`（单个字符）：

```powershell
# 所有 .ps1 文件
scripts\es.exe *.ps1

# 以 set 开头的文件
scripts\es.exe set*

# 类似 pattern 匹配
scripts\es.exe es*.exe
```

### 多关键词搜索（隐含 AND）

多个词之间用空格分隔，全部匹配才返回：

```powershell
scripts\es.exe set line
```

### Everything 原生搜索语法

es.exe 支持 Everything 搜索语法，可在搜索文本中使用：

```powershell
# 按扩展名筛选（分号表示 OR，即 .ps1 或 .exe）
scripts\es.exe ext:ps1;exe

# 关键词 + 扩展名组合
scripts\es.exe setLineEnd ext:ps1;cmd

# 排除模式（! 前缀表示 NOT）
scripts\es.exe *.md !readme

# 路径 + 扩展名 + 关键词组合
scripts\es.exe -path C:\Share_Kilo ext:ts
```

### 精确文件名匹配

使用 `nopath:` 和 `wholefilename:` 语法实现精确匹配文件名（避免匹配子串）：

```powershell
# 精确匹配名为 .git 的文件夹（不会匹配 .github、kilocode.git 等）
scripts\es.exe /ad nopath: wholefilename:.git

# 精确统计名为 .git 的文件夹数量
scripts\es.exe -get-result-count /ad nopath: wholefilename:.git

# 精确匹配文件名为 package.json 的文件
scripts\es.exe /a-d nopath: wholefilename:package.json
```

**说明：**
- `nopath:` - 只匹配文件名部分，不匹配路径
- `wholefilename:` - 完全匹配整个文件名
- 两者结合使用可以实现最精确的文件名匹配

### 路径过滤

限定搜索范围到指定目录树：

```powershell
# 仅在 C:\Share_Kilo 下搜索 .ts 文件
scripts\es.exe -path C:\Share_Kilo -n 10 *.ts

# 指定父路径
scripts\es.exe -parent-path C:\Share_Kilo\kilocode -n 5 SKILL.md
```

| 参数 | 说明 |
|---|---|
| `-path <dir>` | 搜索指定路径及其子目录 |
| `-parent-path <dir>` | 搜索指定路径的父目录及其子目录 |
| `-parent <dir>` | 搜索指定路径的直接子目录 |

### 全词匹配

将搜索词作为完整单词匹配（而非子串）：

```powershell
# 匹配 "setLineEnd" 但不匹配 "batchsetLineEnd"
scripts\es.exe -w setLineEnd
```

### 大小写敏感搜索

默认不区分大小写，用 `-case`（或 `-i`）启用：

```powershell
scripts\es.exe -case -n 5 SETLINEEND
```

## 结果控制

### 限制结果数量

```powershell
scripts\es.exe -n 5 *.ps1
```

### 获取结果统计（不列出文件）

```powershell
# 仅返回匹配数量
scripts\es.exe -get-result-count *.ps1

# 仅返回匹配文件的总字节数
scripts\es.exe -get-total-size *.ps1
```

## 显示列控制

默认只显示完整路径。可额外添加列：

```powershell
# 仅显示文件名（不含路径）
scripts\es.exe -name -n 5 *.ps1

# 显示文件大小 + 最后修改日期
scripts\es.exe -size -date-modified -n 5 *.ps1

# 显示扩展名
scripts\es.exe -extension -n 5 *.ps1

# 显示创建日期
scripts\es.exe -date-created -n 5 *.ps1
```

可用显示列：`-name`, `-path-column`, `-full-path-and-name`（默认）, `-size`, `-extension`, `-date-created`/`-dc`, `-date-modified`/`-dm`, `-date-accessed`/`-da`, `-attributes`/`-attribs`, `-run-count`, `-date-run`, `-date-recently-changed`/`-rc`

## 排序

```powershell
# 按名称排序
scripts\es.exe -sort name -n 5 *.ps1

# 按大小降序
scripts\es.exe -sort size-descending -size -n 5 *.iso

# 按创建日期升序
scripts\es.exe -sort date-created-ascending -n 5 *.ps1

# 按修改时间（最近修改的排前面）
scripts\es.exe -sort date-modified -n 8 *.md
```

可用排序字段：`name`, `path`, `size`, `extension`, `date-created`, `date-modified`, `date-accessed`, `attributes`, `file-list-file-name`, `run-count`, `date-recently-changed`, `date-run`

排序方向：追加 `-ascending` 或 `-descending` 后缀（默认升序）。

## 文件类型过滤

### 仅文件 / 仅文件夹

```powershell
# 仅文件
scripts\es.exe /a-d -path C:\Share_Kilo -n 5 *.ps1

# 仅文件夹
scripts\es.exe /ad -path C:\Share_Kilo -n 10 *kilo*
```

### 属性过滤（DIR 风格）

```powershell
# 隐藏文件
scripts\es.exe /ah -path C:\Share_Kilo -n 5 .gitignore

# 系统文件
scripts\es.exe /as ... ...
```

属性标志：R=只读, H=隐藏, S=系统, D=目录, A=归档。前缀 `-` 表示排除，如 `/a-d` 排除目录（即仅文件）。

## 输出格式

```powershell
# CSV 格式（带 "Filename" 头，双引号包裹）
scripts\es.exe -csv -n 5 *.ps1

# TSV 格式（制表符分隔，带列头）
scripts\es.exe -tsv -size -dm -n 3 *.ps1

# 双引号包裹路径
scripts\es.exe -double-quote -n 3 setLineEnd
```

## 正则搜索

`-r` 启用正则匹配。正则匹配的是 **完整路径**（不限于文件名）：

```powershell
# 路径中包含 exe 或 ps1 的文件
scripts\es.exe -r -n 5 "exe|ps1"
```

**注意：** 简单文件名匹配用通配符 `*` 即可，无需正则。

## 完整参数参考

### 搜索选项

| 参数 | 别名 | 说明 |
|---|---|---|
| `-r <text>` | `-regex` | 正则表达式搜索（匹配完整路径） |
| `-i` | `-case` | 大小写敏感（默认不敏感） |
| `-w` | `-whole-word` | 全词匹配 |
| `-p` | `-match-path` | 搜索词匹配完整路径+文件名（默认仅匹配文件名） |
| `-path <dir>` | | 限定搜索目录树 |
| `-parent-path <dir>` | | 限定父目录树 |
| `-parent <dir>` | | 限定直接父目录 |
| `-n <num>` | `-max-results` | 最大返回条数 |
| `/ad` | | 仅文件夹 |
| `/a-d` | | 仅文件 |
| `/a<flags>` | | 属性过滤 |
| `ext:` | （搜索语法） | 按扩展名筛选 |
| `!` | （搜索语法） | 排除 |

### 排序选项

| 参数 | 说明 |
|---|---|
| `-sort <field>[-ascending\|-descending]` | 按字段排序 |
| `-sort-ascending` / `-sort-descending` | 设置排序方向 |

### 显示选项

| 参数 | 说明 |
|---|---|
| `-name` | 仅显示文件名（不含路径） |
| `-size` | 显示文件大小 |
| `-date-created` / `-dc` | 显示创建日期 |
| `-date-modified` / `-dm` | 显示修改日期 |
| `-date-accessed` / `-da` | 显示访问日期 |
| `-extension` / `-ext` | 显示扩展名 |
| `-attributes` / `-attribs` | 显示文件属性 |
| `-csv` | CSV 格式输出 |
| `-tsv` | TSV 格式输出 |
| `-double-quote` | 路径用双引号包裹 |

## 资源清单

| 文件 | 说明 |
|---|---|
| `scripts/es.exe` | Everything 命令行客户端 v1.1.0.30（依赖后台 Everything v1.4.1.1029 服务） |

## 注意事项

- `es.exe` 依赖 **Everything 服务**（`Everything.exe`）在后台运行。如果搜索无结果返回，先用 Everything GUI 确认索引是否正常工作
- 首次安装 Everything 后可能需要几分钟建立索引
- 搜索结果基于 NTFS 的实时 USN 日志，新建/修改文件秒级可见
- **仅支持 NTFS 卷**。FAT32/exFAT 卷不在 Everything 索引范围内
- 从本 skill 目录执行时使用相对路径 `scripts/es.exe`；从其他目录使用完整路径
- AI 调用时优先使用 `bash` 工具直接执行 es.exe，配合 `-path -n -sort` 等参数精确定位
