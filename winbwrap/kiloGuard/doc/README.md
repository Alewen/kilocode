# KiloGuard.sys 驱动

## 重要说明

- **部署驱动必须使用 `deploy.ps1`** — 自动完成证书检查/生成、编译、签名、证书安装、复制驱动、创建服务与注册表、启动、验证全流程。
- **卸载驱动必须使用 `cleanup.ps1`** — 正确卸载 minifilter（`fltmc unload`）、删除服务、清理注册表、删除驱动文件、清理构建产物。
- 手动执行上述步骤极易遗漏或出错（例如 minifilter 不能用 `sc stop` 卸载），请始终使用脚本。

## `build.ps1` 与 `deploy.ps1` 的关系

- `build.ps1` — **仅编译**。调用 MSVC + WDK 编译 `KiloGuard.c` 并链接生成 `KiloGuard.sys`。
- `deploy.ps1` — **全流程部署**。内部自动调用 `build.ps1`，并在编译成功后继续完成签名、证书安装、复制驱动、创建服务、启动、验证。
- 单独运行 `build.ps1` 的场景：仅需生成 `.sys` 文件（如调试/静态分析），不涉及安装和运行。
- 日常使用直接运行 `deploy.ps1` 即可，无需手动执行 `build.ps1`。

## 部署

```pwsh
.\deploy.ps1
```

脚本自动完成：证书检查/生成 → 编译 → 签名 → 证书安装 → 复制驱动 → 创建服务 + 注册表 altitude → 启动 → 验证。

## 卸载

```pwsh
.\cleanup.ps1
```

脚本自动完成：卸载 minifilter（`fltmc unload`）→ 删除服务（`sc delete`）→ 清理注册表 → 删除驱动文件 → 清理构建产物。注意，这一步执行完毕之后，可能需要重启才能生效。任何重启必须征求用户允许。

## 验证

```cmd
sc query KiloGuard       :: STATE: 4 RUNNING
fltmc filters            :: 应列出 KiloGuard
```

## 文件清单

| 文件 | 说明 |
|------|------|
| `KiloGuard.c` | 驱动源码 |
| `build.ps1` | 编译脚本（仅编译，被 `deploy.ps1` 自动调用） |
| `deploy.ps1` | 全流程自动化部署脚本（编译 → 签名 → 安装 → 启动） |
| `cleanup.ps1` | 全流程自动化卸载脚本（卸载 → 删除 → 清理） |
| `KiloGuardTest.pfx` | 测试签名证书（密码: test） |
| `driver_dev_env.md` | 开发环境检查报告 |
| `README.md` | 本文档 |
