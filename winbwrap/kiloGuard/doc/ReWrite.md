# 驱动重写任务

原来的驱动源码为 KiloGuard.c 只有一个 c 文件，以及它包含的头文件，它已经具备沙箱的基础功能

目前显著的发现这样两个事实：
1，假设有一个进程 A，如果它不会启动任何子进程的话，通过 bwrap 这个程序将其放进沙箱，确实可以做到限制该进程只能对指定目录的读写，
对于 level = 0 的目录，它不可以访问，不可以枚举目录结构，对于 level = 1 的目录，它做到读取其中文件，执行其中的程序，
对于 level = 2 的目录它可以做完完全不受限制，等同于没有进入沙箱
2，但是如果这个进程在它的执行过程中，存在创建任意的子进程的行为，不管它创建这个子进程的方式是挂起创建再执行，还是直接执行，
都会在子进程初始化的时候，发生死锁，现象就是子进程走不到 main 函数，后续也不再发起任何 IRP 消息，父进程 A 进程，直接卡死

所以我决定对这个驱动程序做一次彻底的重写，就在当前目录下，我的思路是这样的，保持现有驱动目录下所有文件结构不变，
KiloGuard.c 这个文件也不再进行改写，放这一边作为参考

在它的边上重新写 FileGuard.c + Domain.c + Pidmap.c + ProcessGuard.c 文件
然后在这些空白文件上开始搭建骨架，保持和原来的 KiloGuard.c 一样的接口以及调用方式
编译的和部署的产物也是 KiloGuard.sys，应用程序 bwrap.exe 不需要知道底层的驱动发生了变更

为了表示区分，后续我将使用 KiloGuard 特意指代 KiloGuard.c 这个原驱动
FileGuard 指代 FileGuard.c + Domain.c + Pidmap.c + ProcessGuard.c 整个集合

后续每一次修改重新部署驱动，都需要跑若干次测试用例，看看测试的稳定性和效果是否达标
依据测试结果，将 KiloGuard.c 的业务功能，填充到 FileGuard.c 这个里面
再次重新部署，测试，循环

测试用例必须覆盖到这几个维度，（如下我用 A -> B -> C 简略表示进程的父，子，孙关系，L0,L1,L2 表示目录权限）
1，A 进程如果在 L0 目录，无法启动
2，A 进程如果在 L1,L2 目录，但是它依赖的 dll 或其他加载项依然在 L0 目录，无法启动
3，A 进程可以正常启动，如果它希望启动子进程 B，此时进程 B 的启动规则遵守前面的 1 和 2
4，B 进程可以正常启动，如果它希望启动子进程 C，此时进程 C 的启动规则遵守前面的 1 和 2
5，进程 A 启动进程 B ，或者进程 B 启动进程 C，只有两个结果，可以启动或者不可以启动，但是不能发生死锁
6，无论进程 A,B,C 都不可以对 L0,L1 目录进行写入文件的操作，只能对 L2 的目录进行写入文件
   它们只能对 L1 和 L2 的目录执行读取操作和执行操作
7，windows 的编程规则，在 main 函数执行执行之前，是可以对文件进行 IO 操作的，这个 load 阶段
   原则上，也禁止对 L0 读取，对 L0 和 L1 写入，但是现阶段已经发生了死锁，就是在 load 阶段
   所以，阶段可以对 main 函数之前的 load 阶段放行，后续如果有机会就解决
8，windows 的应用程序在启动的时候会静态加载 dll，大多数情况下，这些 dll 会在 system32 目录下，或者和他们相同目录
   但是也无法避免有些应用程序，在启动的时候是通过 bat 或者 cmd 文件启动的，不是直接启动
   这个时候，它们完全可以通过在脚本中设置 PATH 路径，附加它们依赖的 dll 路径，然后通过脚本启动
   所以这种时候，无论这个脚本或者应用程序，它们是进程 A 还是进程 B或C，结果只有两种，成功启动或者不能启动
   绝对避免他们的启动的时候发生死锁
9，一个比较典型的例子是 svn 程序，它依赖的 dll 除了在 system32 目录和它自身所在目录之外，需要读取
   Windows\Globalization 目录下的国际化相关的文件，否则无法启动
   另外一个例子是 git.exe 它依赖的 dll 有些是在它自身目录下，结果在将它所在的目录设置为 L0 之后
   它会因为无法读取相同目录下的 dll 文件，于是加载了 system32 目录下的同名文件，依然可以启动成功

# FileGuard 设计总结（与 KiloGuard.c 的差异及理由）

以下是在重写过程中，经评审讨论后确认的设计决策记录。

## 1. IRP 回调独立设计（放弃中央决策入口）

KiloGuard.c 将多个 IRP 的回调统一收敛到 `KgCheckFileIrp` → `KgCheckFileAccess` 做中央决策。
FileGuard 则让每个 IRP 回调（PreCreate、PreWrite、PreSetInfo、PreCleanup 等）各自独立完成
路径解析、规则查找和权限裁决，彼此不共享代码路径。

**理由：**
- 不同 IRP 的发生时机和频率各不相同，用同一套规则限制反而引入耦合风险
- 各回调独立后，出现问题时更容易定位到具体的 IRP 回调，修改单个回调不影响其他功能
- 代价是 PreCreate / PreWrite / PreSetInfo / PreCleanup 中存在相似的代码结构，但这是有意为之的隔离设计

## 2. ObRegisterCallbacks 句柄权限剥离 → 放弃

（SVN r315 添加，r318 删除）

FileGuard 的 ProcessGuard.c 中 KiloPreOperation 始终返回 OB_PREOP_SUCCESS，
不对沙箱进程的 OpenProcess 做任何权限裁剪。

**理由：**
- ObRegisterCallbacks 的 OB_OPERATION_HANDLE_CREATE 同时覆盖 OpenProcess（打开已有进程）
  和 NtCreateUserProcess（创建子进程），两者参数完全一致，无法区分
- 若在此剥离危险权限（PROCESS_TERMINATE、PROCESS_VM_WRITE 等），则沙箱父进程创建子进程时，
  子进程句柄的权限也会被剥离，导致子进程无法正常启动
- 这正是原版 KiloGuard.c 导致子进程卡死/死锁的根本原因之一
- 当前保留 ObRegisterCallbacks 注册和 KgRegisterProcessCallbacks 调用框架，
  但 KiloPreOperation 作为空桩，以备将来若找到区分方案时可直接启用

## 3. 决策缓存 → 放弃

KiloGuard.c 使用 KgHashPath + gDecisionCache（128 槽轮转）缓存文件访问决策结果。
FileGuard 不做任何决策缓存，每次回调都实时查规则链表。

**理由：**
- 每次 IRP 回调中 80-90% 的开销来自 FltGetFileNameInformation（走整个文件系统栈获取路径），
  决策缓存只能跳过最后的规则链查找（约 0.5-2μs），收益极低
- 原版的决策缓存存在跨域缓存冲突 bug：不同沙箱域的 ruleEpoch 初始值均为 0，
  不同域的进程访问同一路径时可能误命中对方的缓存结果
- 典型工作负载（编译/构建）中同一文件通常只访问一次，缓存命中率极低
- 为了极低收益引入 epoch 维护和正确性隐患，不值得

## 4. gShutdown → 放弃

KiloGuard.c 定义了 gShutdown 标志（DriverEntry 置 0，DriverUnload 置 1），
但没有任何代码实际读取检查该标志，属于死代码，FileGuard 不再保留。

## 5. 增强：重命名目标路径权限检查

FileGuard 的 PreSetInfo 在处理 IRP_MJ_SET_INFORMATION 的 FileRenameInformation 时，
不仅检查源路径的权限等级，还提取目标路径并独立查规则。
仅当源路径和目标路径均 ≥ L2 时才允许重命名。KiloGuard.c 没有此项检查。

## 6. 修复：PreCreate 中绑定根祖先遍历放行范围

KiloGuard.c 的 PreCreate 在非纯遍历路径下也会无条件放行绑定根祖先目录，
这意味着即使是 WRITE / DELETE 访问，只要路径是绑定根的父目录就会绕过权限检查。
FileGuard 修正了此问题：绑定根祖先的遍历放行仅在纯遍历（FILE_TRAVERSE）时提前返回，
在其他路径中该放行仅授予 L1（只读）级别，后续仍走正常的 level-vs-operation 裁决。
