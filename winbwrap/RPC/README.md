# Windows RPC 开发示例

本目录包含 3 个由浅入深的 Windows RPC (Remote Procedure Call) 示例，使用 C 语言和 MIDL 编写。

---

## 核心概念：IPC、RPC、ALPC 的定义与关系

### 1. IPC — Inter-Process Communication（进程间通信）

**定义**：IPC 不是某种具体技术，而是一个**统称**，泛指所有允许不同进程之间交换数据或同步行为的机制。

**作用**：解决进程地址空间隔离问题——每个进程有独立的虚拟地址空间，无法直接访问彼此的内存，需要通过操作系统提供的机制来传递数据或信号。

**Windows 下常见的 IPC 机制**：

| 类别 | 具体机制 | 说明 |
|---|---|---|
| 数据交换 | 剪贴板、邮槽（Mailslot）、命名管道（Named Pipe）、匿名管道 | 字节流或消息式数据传递 |
| 共享内存 | File Mapping / Shared Memory | 最快的大数据 IPC，需配合同步原语 |
| 消息机制 | Windows 消息（SendMessage/PostMessage）、WM_COPYDATA | GUI 进程间通信 |
| 网络 | Socket / Winsock | 跨机器通用 |
| 高层框架 | RPC、COM/DCOM | 提供函数调用抽象 |
| 内核级 | ALPC 端口 | 高性能本地 IPC，供系统组件和 RPC 使用 |
| 同步原语 | 互斥量（Mutex）、信号量（Semaphore）、事件（Event） | 本身不传递数据，用于同步 |

> 注意：互斥量、信号量、事件等是**同步原语**，不是数据交换意义上的 IPC。它们常与共享内存等 IPC 机制配合使用，但本身不传数据。

---

### 2. RPC — Remote Procedure Call（远程过程调用）

**定义**：RPC 是一种**高层通信框架**，允许一个进程（客户端）调用另一个进程（服务端）中的函数，就像调用本地函数一样。被调用的进程可以在同一台机器上，也可以在网络另一端。

**作用**：提供**位置透明**的函数调用抽象。开发者只需编写接口定义，参数的序列化/反序列化、网络传输、错误处理等底层细节由 RPC 运行时自动完成。

**核心特性**：

- **IDL 驱动**：用接口定义语言（MIDL）描述接口，编译器自动生成客户端代理（proxy）和服务端存根（stub）
- **自动封送/散集**（Marshaling / Unmarshaling）：参数自动打包成网络字节流、解包成本地结构
- **多传输协议**：同一套接口代码可在不同协议上运行（本地 ALPC、TCP/IP、命名管道、HTTP 等）
- **内置安全**：支持认证、加密、模拟等安全特性
- **是 COM/DCOM 的基础**：Windows 的组件对象模型构建在 RPC 之上

**Windows 中的应用**：服务控制管理器（SCM）、事件日志、WMI、注册表远程访问、Active Directory、MMC 管理单元等大量系统组件均基于 RPC 实现。

---

### 3. ALPC — Advanced Local Procedure Call（高级本地过程调用）

**定义**：ALPC 是 Windows 内核提供的一种**高性能本地进程间通信机制**，仅用于同一台机器上的进程之间。它是 LPC（Local Procedure Call）的增强版，从 Windows Vista 开始引入。

**作用**：为本地进程间提供低延迟、高效率的消息传递通道，是 Windows 操作系统内部组件之间通信的基石。

**核心特性**：

- 基于**端口（Port）**内核对象，采用**请求-响应**模式
- 短消息通过系统调用复制到内核缓冲区（仅一次用户态→内核态拷贝，比传统管道少一次）
- 大消息通过**共享内存区段**（Section）映射，接收方可直接访问同一块物理内存，无需额外拷贝
- 支持**回调（Callback）**机制，服务端可反向请求客户端数据
- 支持**上下文（Connection Ports / Communication Ports）**分层模型
- 可用于**内核态 ↔ 用户态**之间、**用户态进程↔用户态进程**之间的通信
- 是 Windows 系统服务调用的底层通道之一（`NtRequestWaitReplyPort` 等）

**典型应用**：

- `ncalrpc` 协议的底层传输（本地 RPC）
- Win32k.sys 与用户态 GUI 进程之间的通信
- 系统服务（如 lsass、smss、csrss）之间的通信
- 现代 UWP 应用的 AppContainer 沙箱通信

---

### 三者的关系

**包含关系**（从大到小）：

```
IPC（进程间通信 — 统称）
 ├── 剪贴板 / 邮槽 / 管道 / Socket / 共享内存 / ...
 ├── RPC（远程过程调用 — 高层框架）
 │    └── ncalrpc（本地 RPC 传输协议）
 │         └── ALPC（内核级本地通信端口 — 底层实现）
 └── ALPC（可独立使用的内核 IPC 机制）
```

**层次关系**（从高到低）：

```
┌─────────────────────────────────────────┐
│          应用层代码（你的程序）           │
├─────────────────────────────────────────┤
│     RPC 运行时（rpcrt4.dll）            │
│     ├─ IDL 存根 / NDR 封送              │
│     ├─ 安全 / 认证 / 端点映射            │
│     └─ 多传输协议抽象                    │
├─────────────────────────────────────────┤
│  传输层: ncalrpc / ncacn_ip_tcp / ...   │
├─────────────────────────────────────────┤
│  内核层: ALPC 端口（仅本地时）           │
│  └─ 共享内存区段 + 内核消息队列          │
└─────────────────────────────────────────┘
```

**一句话总结**：

> **IPC 是总类，RPC 是其中的高层框架，ALPC 是内核级本地通信机制；当 RPC 使用本地传输（ncalrpc）时，底层走的就是 ALPC。**

---

## 前置条件

- Visual Studio 2022（含 C++ 桌面开发工作负载 / Build Tools）
- Windows SDK（含 midl.exe）

## 编译

在 PowerShell 中运行：

```powershell
# 编译所有示例
.\build.ps1

# 只编译某个示例（可输入部分名称匹配）
.\build.ps1 -Example Basic
.\build.ps1 -Example Struct
.\build.ps1 -Example Callback

# 清理
.\build.ps1 -Clean
```

每个示例的输出在各自目录的 `out\` 子目录下，包含 `server.exe` 和 `client.exe`。

## 运行

每个示例都需要先启动服务端，再启动客户端（两个终端窗口）。

### 示例 1: 01_BasicHelloWorld

最基础的 RPC 示例，展示完整的 IDL -> ACF -> MIDL 编译 -> 客户端/服务端调用流程。

```
# 终端1
01_BasicHelloWorld\out\server.exe

# 终端2
01_BasicHelloWorld\out\client.exe
```

**知识点：**
- `hello.idl` — 接口定义（UUID + 函数签名）
- `hello.acf` — 应用配置文件（指定隐式绑定句柄）
- `midl.exe` — 编译 IDL 生成头文件和存根
  - `hello.h` — 公共头文件
  - `hello_c.c` — 客户端存根（客户端编译用）
  - `hello_s.c` — 服务端存根（服务端编译用）
- `ncalrpc` 协议 — 本地 RPC（走 ALPC，性能最高）
- 链接 `rpcrt4.lib` — RPC 运行时库

### 示例 2: 02_StructAndArray

演示复杂数据类型的传递。

```
# 终端1
02_StructAndArray\out\server.exe

# 终端2
02_StructAndArray\out\client.exe
```

**知识点：**
- 结构体嵌套定义（`Point` 内嵌在 `Rect` 中）
- `[in]` / `[out]` / `[in, out]` 参数方向
- `[size_is]` —  conformant array 大小声明
- `[range]` — 参数范围校验
- 服务端分配、客户端释放的 out 数组（`GetPrimeNumbers`）
- `midl_user_allocate` / `midl_user_free` — 内存管理回调

### 示例 3: 03_CallbackRPC

演示上下文句柄（Context Handle），用于在多次调用之间保持服务端状态。

```
# 终端1
03_CallbackRPC\out\server.exe

# 终端2
03_CallbackRPC\out\client.exe
```

**知识点：**
- `[context_handle]` — 上下文句柄类型，服务端状态在多次调用间持久化
- 客户端持有不透明句柄，服务端维护真实状态结构
- `_rundown` 函数 — 客户端异常断开时服务端的清理回调
- 典型使用场景：会话、事务、大文件分块传输

## RPC 开发核心流程

```
1. 编写 IDL 文件（接口定义语言）
   ├── UUID 唯一标识接口
   ├── 版本号
   └── 函数签名 + 参数属性 [in]/[out]/[size_is] 等

2. 编写 ACF 文件（应用配置文件）
   ├── 绑定句柄类型
   ├── 编码解码特性
   └── 上下文句柄声明

3. midl.exe 编译 IDL+ACF
   ├── 生成 .h 头文件
   ├── 生成 _c.c 客户端存根（proxy）
   └── 生成 _s.c 服务端存根（stub）

4. 实现服务端
    ├── 注册协议序列（ncalrpc / ncacn_ip_tcp 等）— RpcServerUseProtseqEp
    ├── 注册接口 — RpcServerRegisterIf
    ├── 注册到端点映射器 — RpcEpRegister（可选，用于动态端点查找）
    ├── 启动监听线程 — RpcServerListen（必须调用，否则服务端不会开始监听）
    ├── 等待服务停止 — RpcMgmtWaitServerListen（仅等待，不负责启动监听）
    └── 实现 IDL 中声明的函数

5. 实现客户端
   ├── 创建绑定字符串
   ├── 绑定到服务端
   ├── 直接调用 IDL 函数（就像本地函数）
   └── 释放绑定
```

## 常见协议序列

| 协议序列 | 说明 |
|---|---|
| `ncalrpc` | 本地 RPC，底层走 ALPC，延迟最低，仅限同一台机器 |
| `ncacn_ip_tcp` | TCP/IP 面向连接 RPC，跨机器，可靠传输 |
| `ncacn_np` | 命名管道传输，跨机器或本地，Windows 原生风格 |
| `ncacn_http` | HTTP 隧道 RPC，穿越防火墙 / 代理 |
| `ncadg_ip_udp` | UDP 无连接数据报 RPC，低延迟但消息大小受限（约 4KB） |

> 注：`ncalrpc` 是本地最低延迟的 RPC 传输；但如果是纯大数据块搬运，**共享内存 + 同步原语**的吞吐更高。RPC 的优势是提供了完整的函数调用语义和类型安全。

## RPC 的多种使用方式（多维度分类）

### 按传输协议分（最常用的分类维度）

见上表。核心是：**同一套 IDL 接口代码可以在不同协议上运行**，只需在服务端注册不同的协议序列、客户端用不同的绑定字符串即可。

### 按调用模型分

- **同步 RPC** — 客户端调用后阻塞等待返回（最常用，本仓库示例都是同步）
- **异步 RPC** — 客户端调用后立即返回，通过事件/回调/轮询获取结果。通过 ACF 的 `[async]` 属性或 MIDL `/async` 开关启用

### 按调用模式 / 方向分

- **请求-响应（同步双向）** — 客户端发送请求并阻塞等待服务端返回（最常见，本仓库示例均为此模式）
- **单向调用（One-way）** — 客户端发出请求后立即返回，不等待服务端响应。需 IDL 标记 `[oneway]`，常用于日志上报、心跳通知
- **回调 RPC** — 服务端在处理过程中反向调用客户端函数。通过 `[callback]` 属性声明回调接口，客户端传入回调函数指针
- **广播 RPC** — 客户端向多个服务端广播请求（UDP 协议下可用，但现代 Windows 默认安全策略通常限制广播，需显式配置权限）

### 按状态管理分

- **无状态调用** — 每次调用独立，服务端不保留上下文
- **上下文句柄（Context Handle）** — 在多次调用之间保持服务端状态。客户端持有一个不透明句柄，服务端维护真实状态结构。客户端异常断开时会触发 `_rundown` 回调清理

### 按绑定句柄类型分

- **自动句柄（auto_handle）** — RPC 运行时自动根据接口 UUID 查找端点、建立连接，客户端完全不用管绑定
- **隐式句柄（implicit_handle）** — 全局一个绑定句柄变量，接口内所有调用共用（本仓库示例用的就是这种）
- **显式句柄（explicit / primitive handle）** — 每个函数第一个参数传绑定句柄，同一个接口可连不同服务端
- **通用句柄（generic handle）** — 用户自定义句柄类型，通过绑定/解绑回调转换成真正的 RPC 绑定，用于封装自定义连接逻辑

### 按接口定义方式分

- **IDL + MIDL 编译** — 标准方式。写 `.idl` + `.acf` 文件，用 `midl.exe` 生成头文件和存根代码
- **类型库 (TLB)** — COM 风格，用类型库描述接口，本质上还是基于 IDL 但面向 COM 组件模型
- **手写存根** — 直接调用 RPC 运行时 API 手动做 NDR 编码/解码，极少见，仅用于特殊底层场景

### 更高层的 RPC 框架

- **COM / DCOM** — 构建在 RPC 之上的组件对象模型，是 Windows 生态最核心的组件技术
- **WCF (Windows Communication Foundation)** — .NET 时代的统一通信框架，支持多种传输和协议
- **gRPC / Thrift / ZeroC Ice** — 跨平台 RPC 框架，不依赖 Windows RPC 运行时
- **消息队列（MSMQ 等）** — 异步可靠消息传递，可看作 RPC 的异步变体

## 关键头文件和库

- `rpcdce.h` — RPC 数据类型和常量
- `rpc.h` — RPC 运行时 API
- `rpcndr.h` — NDR 编码（IDL 编译器自动包含）
- `rpcrt4.lib` — 链接库
- `rpcrt4.dll` — 运行时 DLL（系统自带）
