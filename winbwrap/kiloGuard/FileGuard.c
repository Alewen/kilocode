#include <fltKernel.h>
#include <wdmsec.h>
#include "KiloGuardSecret.h"
#include "Domain.h"
#include "ProcessGuard.h"
#include "PidPortMap.h"
#include "RegGuard.h"

///////////////////////////////////////////////////////////////////////////////
// 设备对象 + IOCTL — 兼容 bwrap.exe 的 CreateFile + DeviceIoControl 方式
//     bwrap.exe 通过 CreateFile("\\\\.\\KiloGuard") 打开设备
//     通过 DeviceIoControl 发送 6 个 IOCTL 进行认证、策略、绑定等操作
//
// bwrap.exe 调用顺序:
//   1. AUTHENTICATE     — 启动时认证，发送共享密钥，加入受信白名单（调用1次）
//   2. CONTROL_SANDBOX  — 创建沙箱域，返回 KG_SANDBOX_ID；退出时销毁沙箱域（调用2次）
//   3. SET_POLICY_BATCH — 批量设置路径规则（哪个目录→哪个沙箱域→什么权限）（调用1次）
//   4. ATTACH_PROCESS   — 将子进程 PID 绑定到沙箱域（调用1次）
//   5. QUERY_EVENTS     — 轮询沙箱域内进程启动失败事件，每500ms调用一次（反复调用）
//   6. DEAUTHENTICATE   — 退出时从受信白名单移除自己（调用1次）
//   *. QUERY_STATS      — 预留，当前 bwrap.exe 未使用

///////////////////////////////////////////////////////////////////////////////
// Macros
///////////////////////////////////////////////////////////////////////////////

#define IOCTL_KG_SET_POLICY_BATCH \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_KG_ATTACH_PROCESS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_KG_CONTROL_SANDBOX \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_KG_QUERY_STATS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_KG_AUTHENTICATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_KG_DEAUTHENTICATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_KG_QUERY_EVENTS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_KG_QUERY_DENY_EVENTS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_READ_DATA)

#define KG_MAX_IOCTL_SIZE 65536
#define KG_MAX_TRUSTED_PIDS 16

///////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// 流上下文（Stream Context）
// 预留结构，用于在文件流上挂载自定义数据
// 当前 IOCTL 通信模式下暂未启用
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_STREAM_CONTEXT {
    ULONG Placeholder; // 预留，当前未使用
} KG_STREAM_CONTEXT;

////////////////////////////////////////////////////////////////////////////////
// 受信 PID 白名单条目
// 记录通过 AUTHENTICATE 认证的 bwrap.exe 进程
// Pid   - 受信进程 PID
// Valid - 条目是否有效
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_TRUSTED_PID {
    HANDLE Pid;
    BOOLEAN Valid;
} KG_TRUSTED_PID;

////////////////////////////////////////////////////////////////////////////////
// 文件操作类型枚举
// 用于在 minifilter 回调中统一描述文件访问意图
// KgOpNone      - 未分类操作（如 Cleanup）
// KgOpCreate    - 创建/打开文件
// KgOpRead      - 读取文件内容
// KgOpWrite     - 写入文件内容（含创建/覆盖）
// KgOpDelete    - 删除文件
// KgOpRename    - 重命名文件
// KgOpSetInfo   - 修改文件属性/大小/时间
// KgOpEnumerate - 枚举目录内容
// KgOpExecute   - 执行文件（映射为可执行节）
////////////////////////////////////////////////////////////////////////////////
typedef enum _KG_OPERATION_TYPE {
    KgOpNone = 0,
    KgOpCreate,
    KgOpRead,
    KgOpWrite,
    KgOpDelete,
    KgOpRename,
    KgOpSetInfo,
    KgOpEnumerate,
    KgOpExecute,
} KG_OPERATION_TYPE;

////////////////////////////////////////////////////////////////////////////////
// 进程沙箱状态快照
// 在 minifilter 回调入口处无锁捕获，用于后续快速判断
// SlotIndex    - 沙箱域槽位索引（若在沙箱内）
// PidMapEpoch  - 捕获时的 PidMap 版本号
// IsSandboxed  - 进程是否在沙箱内
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_SNAPSHOT {
    ULONG SlotIndex;
    LONG PidMapEpoch;
    BOOLEAN IsSandboxed;
} KG_SNAPSHOT;

///////////////////////////////////////////////////////////////////////////////
// Global variables
///////////////////////////////////////////////////////////////////////////////

static PFLT_FILTER gFilter = NULL;
static PDEVICE_OBJECT gDeviceObject = NULL;
static KG_TRUSTED_PID gTrustedPids[KG_MAX_TRUSTED_PIDS];
static KSPIN_LOCK gTrustedLock;
static PFLT_PORT gServerPort = NULL;
static PFLT_PORT gClientPort = NULL;

static BOOLEAN KgCaptureSnapshot(KG_SYSTEM_STATE* state, HANDLE pid, KG_SNAPSHOT* snap);
static PCSTR KgOpToString(KG_OPERATION_TYPE op);

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 8. Altitude
//      决定驱动在过滤器栈中的位置
//      不在代码中设置，而是通过注册表配置：
//      HKLM\SYSTEM\CurrentControlSet\Services\<ServiceName>\Instances\<InstanceName>
//      deploy.ps1 中已设置 Altitude = 360000
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 7. 实例回调
//      InstanceSetupCallback — 卷挂载时初始化
//      InstanceQueryTeardownCallback / InstanceTeardownStartCallback — 卷卸载时清理
///////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// 功能：实例设置回调 — 卷挂载时由 Filter Manager 调用
//       当前驱动注册到所有卷的所有文件系统类型
// 参数：
//   FltObjects          - 相关对象（卷、实例等）
//   Flags               - 设置标志
//   VolumeDeviceType    - 卷设备类型
//   VolumeFilesystemType - 文件系统类型
// 返回值：
//      STATUS_SUCCESS（始终附加到所有卷）
// 业务概念 - 实例建立:
//      当一个卷被挂载（如插入 U 盘、系统启动时挂载 C 盘），Filter Manager 会遍历所有已注册的 minifilter，
//      为每个 minifilter 在该卷上创建一个实例。当前实现：直接返回 STATUS_SUCCESS，表示"我同意附加到这个卷"。
//      如果返回 STATUS_FLT_DO_NOT_ATTACH：Filter Manager 就不会在这个卷上创建实例，该卷上的所有文件 IO 
//      都不会经过这个 minifilter。
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS
KgInstanceSetup(PCFLT_RELATED_OBJECTS FltObjects, FLT_INSTANCE_SETUP_FLAGS Flags,
                DEVICE_TYPE VolumeDeviceType, FLT_FILESYSTEM_TYPE VolumeFilesystemType)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);
    UNREFERENCED_PARAMETER(VolumeFilesystemType);
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：查询是否可以卸载实例的回调
//       当前始终允许卸载
// 参数：
//   FltObjects - 相关对象
//   Flags      - 查询拆卸标志
// 返回值：
//      STATUS_SUCCESS（允许拆卸）
//  业务概念 - 查询是否可拆卸:
//      当有人请求卸载实例时（如 fltmc detach、卷被弹出），Filter Manager 先调用此回调询问
//      minifilter 是否同意。当前实现：直接返回 STATUS_SUCCESS，表示"同意拆卸"。
//      如果返回 STATUS_FLT_DO_NOT_DETACH：拆卸请求被拒绝，实例保持附加状态。
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS
KgInstanceQueryTeardown(PCFLT_RELATED_OBJECTS FltObjects, FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：实例拆卸开始回调
//       当前为占位实现，无特殊清理操作
// 参数：
//      FltObjects - 相关对象
//      Flags      - 拆卸标志
// 返回值：
//      无
// 业务概念 - 拆卸开始:
//      拆卸已确认，Filter Manager 开始拆卸流程时调用。此时实例仍在附加状态，但不再接收新的 IRP 回调。
////////////////////////////////////////////////////////////////////////////////
static VOID
KgInstanceTeardownStart(PCFLT_RELATED_OBJECTS FltObjects, FLT_INSTANCE_TEARDOWN_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：实例拆卸完成回调
//       当前为占位实现，无特殊清理操作
// 参数：
//   FltObjects - 相关对象
//   Flags      - 拆卸标志
// 返回值：
//      无
// 业务概念 - 拆卸完成:
//      实例已从卷上完全分离，所有未完成的 IRP 回调都已处理完毕。
//      对 KiloGuard 的实际意义
////////////////////////////////////////////////////////////////////////////////
static VOID
KgInstanceTeardownComplete(PCFLT_RELATED_OBJECTS FltObjects, FLT_INSTANCE_TEARDOWN_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
}

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 6. 上下文管理
//      FltAllocateContext / FltSetStreamHandleContext 等
//      在文件/流/实例上挂载自定义数据，跨回调共享状态
//
// ★ 以下 Stream Context 代码专为 minifilter 通信端口架构预留。
//    当前 bwrap 使用 IOCTL 方式通信，Stream Context 暂未接入。
//    注意：KgContextCleanup 中 ExFreePoolWithTag 有 bug（FM 自行管理 Context 内存），
//    将来启用前需修复。
///////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// 功能：流上下文清理回调
//       当 minifilter 释放 FLT_STREAM_CONTEXT 时由 Filter Manager 调用
//       注意：当前使用 ExFreePoolWithTag 直接释放，
//       但 Filter Manager 自己管理 Context 内存，
//       此处的释放方式存在 bug，启用前需修复
// 参数：
//   Context     - 待释放的上下文对象
//   ContextType - 上下文类型（当前未使用）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID
KgContextCleanup(PFLT_CONTEXT Context, FLT_CONTEXT_TYPE ContextType)
{
    UNREFERENCED_PARAMETER(ContextType);
    ExFreePoolWithTag(Context, KG_POOL_TAG);
}

////////////////////////////////////////////////////////////////////////////////
// 上下文类型注册表
// 向 Filter Manager 声明驱动支持的上下文类型（当前仅注册 STREAM_CONTEXT）
// 当前 IOCTL 通信模式下暂未接入，为通信端口架构预留
////////////////////////////////////////////////////////////////////////////////
static const FLT_CONTEXT_REGISTRATION ContextRegistration[] = {
    { FLT_STREAM_CONTEXT, 0, KgContextCleanup, sizeof(KG_STREAM_CONTEXT), KG_POOL_TAG },
    { FLT_CONTEXT_END }
};

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 5. 用户态通信
//      通过 FltCreateCommunicationPort 创建通信端口
//      用户态用 FilterConnectCommunicationPort 连接
//      用于接收用户态的策略配置等控制命令
//
// ★ 以下通信端口代码专为 minifilter 通信端口架构预留。
//    当前 bwrap 使用 CreateFile + DeviceIoControl (IOCTL) 方式通信，
//    不走 FilterConnectCommunicationPort，KgMessageNotify / KgDisconnectNotify 暂未接入。
///////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// 功能：通信端口消息通知回调
//       当用户态通过 FilterSendMessage 发送消息时调用
//       当前为占位实现，bwrap 使用 IOCTL 方式通信，此回调暂未接入
// 参数：
//   PortCookie              - 端口标识
//   InputBuffer             - 输入缓冲区
//   InputBufferLength        - 输入缓冲区长度
//   OutputBuffer             - 输出缓冲区
//   OutputBufferLength       - 输出缓冲区长度
//   ReturnOutputBufferLength - 返回的输出数据长度
// 返回值：STATUS_SUCCESS
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS
KgMessageNotify(PVOID PortCookie, PVOID InputBuffer, ULONG InputBufferLength,
                PVOID OutputBuffer, ULONG OutputBufferLength, PULONG ReturnOutputBufferLength)
{
    UNREFERENCED_PARAMETER(PortCookie);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ReturnOutputBufferLength);
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：通信端口断开连接通知回调
//       当用户态断开通信端口连接时，关闭客户端端口句柄
//       当前 bwrap 使用 IOCTL 方式通信，此回调暂未接入
// 参数：
//   ConnectionCookie - 连接标识（当前未使用）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS
KgConnectNotify(PFLT_PORT ClientPort, PVOID ServerPortCookie,
                PVOID ConnectionContext, ULONG SizeOfContext,
                PVOID *ConnectionPortCookie)
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    // Accept unconditionally. SID lookup / slot assignment is handled
    // via the IOCTL channel (CONTROL_SANDBOX + ATTACH_PROCESS).
    // This port is used ONLY for push notifications.
    *ConnectionPortCookie = (PVOID)(ULONG_PTR)1;
    gClientPort = ClientPort;
    KG_LOG("FileGuard: Port Connect accepted\n");

    return STATUS_SUCCESS;
}

static VOID KgDisconnectNotify(PVOID ConnectionCookie)
{
    UNREFERENCED_PARAMETER(ConnectionCookie);
    KG_LOG("FileGuard: Port disconnect\n");
    FltCloseClientPort(gFilter, &gClientPort);
}

VOID KgSendProcessEvent(ULONG slotIndex, ULONG pid, ULONG msgType, PUNICODE_STRING imageName)
{
    UNREFERENCED_PARAMETER(slotIndex);

    PFLT_PORT port = (PFLT_PORT)InterlockedCompareExchangePointer((PVOID*)&gClientPort, NULL, NULL);
    if (!port) return;

    KG_SANDBOX_ID sid = 0;
    KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)InterlockedCompareExchangePointer((PVOID*)&gState, NULL, NULL);
    if (state && slotIndex < KG_MAX_DOMAIN && state->Domain[slotIndex].Active)
        sid = state->Domain[slotIndex].Id;

    KG_PORT_MESSAGE msg = {};
    msg.MsgType = msgType;
    msg.Pid = pid;
    msg.SID = sid;
    if (imageName && imageName->Buffer)
        wcsncpy_s(msg.ImageName, 260, imageName->Buffer, _TRUNCATE);

    LARGE_INTEGER timeout;
    timeout.QuadPart = -200 * 10000;

    NTSTATUS status = FltSendMessage(gFilter, &port, &msg, sizeof(msg), NULL, NULL, &timeout);
    if (status == STATUS_TIMEOUT)
        KG_LOG("FileGuard: Port Send timeout pid=%lu\n", pid);
    else if (!NT_SUCCESS(status))
        KG_LOG("FileGuard: Port Send failed pid=%lu status=%08lX\n", pid, status);
    else
        KG_LOG("FileGuard: Port Sent pid=%lu type=%lu sid=%lu\n", pid, msgType, sid);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：初始化受信 PID 白名单
//       初始化保护白名单的自旋锁，将所有 16 个槽位标记为无效
// 参数：无
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID KgInitTrustedPids(VOID)
{
    KeInitializeSpinLock(&gTrustedLock);
    for (ULONG i = 0; i < KG_MAX_TRUSTED_PIDS; i++)
        gTrustedPids[i].Valid = FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：检查当前调用者是否在受信 PID 白名单中
//       只有通过 AUTHENTICATE 验证的 bwrap.exe 进程才被认为是受信的
//       受信进程允许执行 SET_POLICY_BATCH、ATTACH_PROCESS 等特权 IOCTL
// 参数：无
// 返回值：TRUE=受信调用者，FALSE=未经认证
////////////////////////////////////////////////////////////////////////////////
static BOOLEAN KgIsTrustedCaller(VOID)
{
    HANDLE pid = PsGetCurrentProcessId();
    KIRQL oldIrql;
    KeAcquireSpinLock(&gTrustedLock, &oldIrql);
    for (ULONG i = 0; i < KG_MAX_TRUSTED_PIDS; i++) {
        if (gTrustedPids[i].Valid && gTrustedPids[i].Pid == pid) {
            KeReleaseSpinLock(&gTrustedLock, oldIrql);
            return TRUE;
        }
    }
    KeReleaseSpinLock(&gTrustedLock, oldIrql);
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：将指定 PID 加入受信白名单
//       在 IOCTL_KG_AUTHENTICATE 验证共享密钥成功后调用
// 参数：
//   pid - 要加入白名单的进程 PID
// 返回值：STATUS_SUCCESS 成功；STATUS_TOO_MANY_COMMANDS 白名单已满
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS KgAddTrustedPid(HANDLE pid)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&gTrustedLock, &oldIrql);
    for (ULONG i = 0; i < KG_MAX_TRUSTED_PIDS; i++) {
        if (!gTrustedPids[i].Valid) {
            gTrustedPids[i].Pid = pid;
            gTrustedPids[i].Valid = TRUE;
            KeReleaseSpinLock(&gTrustedLock, oldIrql);
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&gTrustedLock, oldIrql);
    return STATUS_TOO_MANY_COMMANDS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：从受信 PID 白名单中移除指定进程
//       在进程退出时（process notify 退出路径）或 DEAUTHENTICATE 时调用
// 参数：
//   pid - 要移除的进程 PID
// 返回值：无（未找到时静默返回）
////////////////////////////////////////////////////////////////////////////////
VOID KgRemoveTrustedPid(HANDLE pid)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&gTrustedLock, &oldIrql);
    for (ULONG i = 0; i < KG_MAX_TRUSTED_PIDS; i++) {
        if (gTrustedPids[i].Valid && gTrustedPids[i].Pid == pid) {
            gTrustedPids[i].Valid = FALSE;
            KeReleaseSpinLock(&gTrustedLock, oldIrql);
            return;
        }
    }
    KeReleaseSpinLock(&gTrustedLock, oldIrql);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：清空整个受信 PID 白名单
//       在驱动卸载时调用，移除所有受信进程条目
// 参数：无
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID KgClearAllTrustedPids(VOID)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&gTrustedLock, &oldIrql);
    for (ULONG i = 0; i < KG_MAX_TRUSTED_PIDS; i++)
        gTrustedPids[i].Valid = FALSE;
    KeReleaseSpinLock(&gTrustedLock, oldIrql);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：设备创建/关闭分发例程（IRP_MJ_CREATE / IRP_MJ_CLOSE）
//       用户态 bwrap.exe 通过 CreateFile 打开设备时调用
//       直接返回成功，不接受额外参数验证
// 参数：
//   DeviceObject - 设备对象
//   Irp          - IO 请求包
// 返回值：STATUS_SUCCESS
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS KgCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：设备 IO 控制分发例程（IRP_MJ_DEVICE_CONTROL）
//       处理 bwrap.exe 通过 DeviceIoControl 发送的 6 个 IOCTL：
//       AUTHENTICATE / DEAUTHENTICATE / SET_POLICY_BATCH /
//       ATTACH_PROCESS / CONTROL_SANDBOX / QUERY_STATS / QUERY_EVENTS
//       所有 IOCTL 使用 METHOD_BUFFERED 方式
// 参数：
//   DeviceObject - 设备对象
//   Irp          - IO 请求包（含 IOCTL 码和输入/输出缓冲区）
// 返回值：STATUS_SUCCESS 或相应错误码
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS KgDeviceIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inSize = stack->Parameters.DeviceIoControl.InputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;

    if (!buf || inSize == 0 || inSize > KG_MAX_IOCTL_SIZE) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    switch (code)
    {
    case IOCTL_KG_AUTHENTICATE:
    {
        if (inSize < KG_TOKEN_SIZE) { status = STATUS_INVALID_PARAMETER; break; }
        if (RtlCompareMemory(buf, gExpectedToken, KG_TOKEN_SIZE) != KG_TOKEN_SIZE) {
            status = STATUS_ACCESS_DENIED; break;
        }
        status = KgAddTrustedPid(PsGetCurrentProcessId());
        break;
    }

    case IOCTL_KG_DEAUTHENTICATE:
    {
        KgRemoveTrustedPid(PsGetCurrentProcessId());
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_KG_SET_POLICY_BATCH:
    {
        if (!KgIsTrustedCaller()) { status = STATUS_ACCESS_DENIED; break; }
        if (inSize < sizeof(KG_POLICY_BATCH_INPUT)) { status = STATUS_INVALID_PARAMETER; break; }
        KG_POLICY_BATCH_INPUT* in = (KG_POLICY_BATCH_INPUT*)buf;
        ULONG count = in->RuleCount;
        if (count == 0 || count > 256 ||
            inSize < sizeof(KG_POLICY_BATCH_INPUT) + (count - 1) * sizeof(KG_POLICY_RULE_ENTRY)) {
            status = STATUS_INVALID_PARAMETER; break;
        }

        KG_SYSTEM_STATE* s = KgAcquireState();
        if (!s) { status = STATUS_UNSUCCESSFUL; break; }

        KIRQL oldIrql;
        KeAcquireSpinLock(&gStateLock, &oldIrql);

        for (ULONG i = 0; i < count; i++) {
            UNICODE_STRING path;
            in->Rules[i].Path[1023] = L'\0';
            RtlInitUnicodeString(&path, in->Rules[i].Path);
            KG_SANDBOX_ID sid = in->Rules[i].Sid;
            ULONG slot = KgFindDomainSlot(s, sid);

            if (slot == (ULONG)-1) {
                InterlockedIncrement(&s->DroppedRuleCount);
                continue;
            }

            KG_POLICY_DOMAIN* domain = &s->Domain[slot];
            KIRQL dIrql;
            KeAcquireSpinLock(&domain->Lock, &dIrql);
            NTSTATUS r = KgAddPathRule(domain, &path, in->Rules[i].Level);
            if (NT_SUCCESS(r))
                InterlockedIncrement(&domain->RuleEpoch);
            else if (r != STATUS_TOO_MANY_COMMANDS)
                InterlockedIncrement(&s->DroppedRuleCount);
            KeReleaseSpinLock(&domain->Lock, dIrql);
        }

        KeReleaseSpinLock(&gStateLock, oldIrql);
        KgReleaseState(s);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_KG_ATTACH_PROCESS:
    {
        if (!KgIsTrustedCaller()) { status = STATUS_ACCESS_DENIED; break; }
        if (inSize < sizeof(KG_ATTACH_PROCESS_INPUT)) { status = STATUS_INVALID_PARAMETER; break; }
        KG_ATTACH_PROCESS_INPUT* in = (KG_ATTACH_PROCESS_INPUT*)buf;

        KG_SYSTEM_STATE* s = KgAcquireState();
        if (!s) { status = STATUS_UNSUCCESSFUL; break; }

        KIRQL oldIrql;
        KeAcquireSpinLock(&gStateLock, &oldIrql);

        KG_PID_MAP* newMap = KgClonePidMap(s->PidMap);
        if (!newMap) { KeReleaseSpinLock(&gStateLock, oldIrql); KgReleaseState(s); status = STATUS_INSUFFICIENT_RESOURCES; break; }

        ULONG attachedSlot = (ULONG)-1;
        BOOLEAN netBlockEnabled = FALSE;
        if (in->Detach) {
            KgRemovePid(newMap, in->Pid);
            KgSetPidNetCache(in->Pid, KG_NET_ALLOW);
            status = STATUS_SUCCESS;
        } else {
            attachedSlot = KgFindDomainSlot(s, in->Sid);
            if (attachedSlot != (ULONG)-1) {
                status = KgSetPidSlot(newMap, in->Pid, attachedSlot);
                netBlockEnabled = s->Domain[attachedSlot].NetBlockEnabled;
            } else {
                status = STATUS_NOT_FOUND;
            }
        }

        if (NT_SUCCESS(status)) {
            KG_PID_MAP* oldMap = KgSwapPidMap(newMap);
            KeReleaseSpinLock(&gStateLock, oldIrql);
            KgReleaseState(s);
            KgFreeOldPidMap(oldMap);

            if (!in->Detach && attachedSlot != (ULONG)-1) {
                gBwrapFast[(ULONG)(ULONG_PTR)in->Pid & 0xFFFF] = 1;
                PEPROCESS process = NULL;
                if (NT_SUCCESS(PsLookupProcessByProcessId(in->Pid, &process))) {
                    PUNICODE_STRING imageName = NULL;
                    SeLocateProcessImageName(process, &imageName);
                    KIRQL tIrql;
                    KeAcquireSpinLock(&gTrackerLock, &tIrql);
                    KgAllocTracker(in->Pid, attachedSlot, imageName);
                    KeReleaseSpinLock(&gTrackerLock, tIrql);

                    UCHAR netStatus = KG_NET_BLOCK;
                    if (imageName && netBlockEnabled) {
                        KG_SYSTEM_STATE* s2 = KgAcquireState();
                        if (s2 && attachedSlot < KG_MAX_DOMAIN && s2->Domain[attachedSlot].Active) {
                            if (KgIsNetExeInList(&s2->Domain[attachedSlot], imageName))
                                netStatus = KG_NET_ALLOW;
                        }
                        if (s2) KgReleaseState(s2);
                    }
                    KgSetPidNetCache(in->Pid, netStatus);

                    if (imageName) ExFreePool(imageName);
                    ObDereferenceObject(process);
                } else {
                    KgSetPidNetCache(in->Pid, KG_NET_BLOCK);
                }
            }
        } else {
            KgFreePidMap(newMap);
            KeReleaseSpinLock(&gStateLock, oldIrql);
            KgReleaseState(s);
        }
        break;
    }

    case IOCTL_KG_CONTROL_SANDBOX:
    {
        if (!KgIsTrustedCaller()) { status = STATUS_ACCESS_DENIED; break; }
        if (inSize < sizeof(KG_CONTROL_SANDBOX)) { status = STATUS_INVALID_PARAMETER; break; }
        KG_CONTROL_SANDBOX* in = (KG_CONTROL_SANDBOX*)buf;

        KG_SYSTEM_STATE* s = KgAcquireState();
        if (!s) { status = STATUS_UNSUCCESSFUL; break; }

        KIRQL oldIrql;
        KeAcquireSpinLock(&gStateLock, &oldIrql);

        if (in->Destroy) {
            ULONG slot = KgFindDomainSlot(s, in->Sid);
            if (slot == (ULONG)-1 || slot == 0) {
                KeReleaseSpinLock(&gStateLock, oldIrql);
                KgReleaseState(s);
                status = STATUS_NOT_FOUND;
                break;
            }

            // Remove all PIDs belonging to this sandbox from PidMap,
            // gPidSlotFast, and gPidNetCache before deleting the domain.
            KG_PID_MAP* newMap = KgClonePidMap(s->PidMap);
            if (newMap) {
                for (ULONG b = 0; b < KG_PID_BUCKETS; b++) {
                    PLIST_ENTRY e = newMap->Buckets[b].Flink;
                    while (e != &newMap->Buckets[b]) {
                        KG_PID_ENTRY* entry = CONTAINING_RECORD(e, KG_PID_ENTRY, Link);
                        PLIST_ENTRY next = e->Flink;
                        if (entry->SlotIndex == slot) {
                            HANDLE pid = entry->Pid;
                            ULONG fastIdx = (ULONG)(ULONG_PTR)pid & 0xFFFF;
                            RemoveEntryList(&entry->Link);
                            ExFreePoolWithTag(entry, KG_POOL_TAG);
                            gPidSlotFast[fastIdx] = KG_PID_SLOT_EMPTY;
                            gPidNetCache[fastIdx] = KG_NET_ALLOW;
                            gBwrapFast[fastIdx] = 0;
                        }
                        e = next;
                    }
                }
            }

            KG_POLICY_DOMAIN* domain = &s->Domain[slot];
            KIRQL dIrql;
            KeAcquireSpinLock(&domain->Lock, &dIrql);
            KgFreePathRules(domain);
            domain->Active = FALSE;
            InterlockedIncrement(&domain->RuleEpoch);
            KeReleaseSpinLock(&domain->Lock, dIrql);

            KG_PID_MAP* oldMap = NULL;
            if (newMap) {
                oldMap = KgSwapPidMap(newMap);
            }

            KeReleaseSpinLock(&gStateLock, oldIrql);

            // Free old PidMap outside spinlock (ExWaitForRundownProtectionRelease may block)
            KgFreeOldPidMap(oldMap);

            KgReleaseState(s);
            status = STATUS_SUCCESS;
        } else {
            KG_SANDBOX_ID sid;
            do {
                sid = (KG_SANDBOX_ID)InterlockedIncrement((volatile LONG*)&gNextSid);
            } while (sid == 0 || sid == (KG_SANDBOX_ID)-1);

            status = STATUS_INSUFFICIENT_RESOURCES;
            for (ULONG i = 1; i < KG_MAX_DOMAIN; i++) {
                if (!s->Domain[i].Active) {
                    s->Domain[i].Id = sid;
                    s->Domain[i].Active = TRUE;
                    KeInitializeSpinLock(&s->Domain[i].Lock);
                    InitializeListHead(&s->Domain[i].RuleList);
                    s->Domain[i].RuleCount = 0;
                    s->Domain[i].RuleEpoch = 0;
                    KeInitializeSpinLock(&s->Domain[i].EventRing.Lock);
                    s->Domain[i].EventRing.WriteIndex = 0;
                    s->Domain[i].EventRing.ReadIndex = 0;
                    KeInitializeSpinLock(&s->Domain[i].DenyRing.Lock);
                    s->Domain[i].DenyRing.WriteIndex = 0;
                    s->Domain[i].DenyRing.ReadIndex = 0;
                    s->Domain[i].NetBlockEnabled = FALSE;
                    s->Domain[i].NetExeCount = 0;
                    status = STATUS_SUCCESS;
                    break;
                }
            }
            if (NT_SUCCESS(status)) {
                in->Sid = sid;
                info = sizeof(KG_CONTROL_SANDBOX);
            }
            KeReleaseSpinLock(&gStateLock, oldIrql);
            KgReleaseState(s);
        }
        break;
    }

    case IOCTL_KG_QUERY_STATS:
    {
        if (inSize < sizeof(KG_STATS)) { status = STATUS_INVALID_PARAMETER; break; }
        KG_SYSTEM_STATE* s = KgAcquireState();
        ((KG_STATS*)buf)->DroppedRuleCount = s ? s->DroppedRuleCount : 0;
        if (s) KgReleaseState(s);
        info = sizeof(KG_STATS);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_KG_QUERY_EVENTS:
    {
        if (!KgIsTrustedCaller()) { status = STATUS_ACCESS_DENIED; break; }
        if (inSize < sizeof(KG_QUERY_EVENTS_INPUT)) { status = STATUS_INVALID_PARAMETER; break; }
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KG_QUERY_EVENTS_OUTPUT)) {
            status = STATUS_BUFFER_TOO_SMALL; break;
        }
        KG_QUERY_EVENTS_INPUT* in = (KG_QUERY_EVENTS_INPUT*)buf;

        KG_SYSTEM_STATE* s = KgAcquireState();
        if (!s) { status = STATUS_UNSUCCESSFUL; break; }

        ULONG slot = KgFindDomainSlot(s, in->Sid);
        if (slot == (ULONG)-1) { KgReleaseState(s); status = STATUS_NOT_FOUND; break; }

        KG_POLICY_DOMAIN* domain = &s->Domain[slot];
        KG_QUERY_EVENTS_OUTPUT* out = (KG_QUERY_EVENTS_OUTPUT*)buf;
        out->EventCount = 0;

        KIRQL oldIrql;
        KeAcquireSpinLock(&domain->EventRing.Lock, &oldIrql);

        LONG readIdx = domain->EventRing.ReadIndex;
        LONG writeIdx = domain->EventRing.WriteIndex;

        while (readIdx != writeIdx && out->EventCount < KG_EVENT_BATCH_SIZE) {
            RtlCopyMemory(&out->Events[out->EventCount],
                          &domain->EventRing.Events[readIdx],
                          sizeof(KG_FAILURE_EVENT));
            out->EventCount++;
            readIdx = (readIdx + 1) % KG_EVENT_RING_SIZE;
        }

        domain->EventRing.ReadIndex = readIdx;

        KeReleaseSpinLock(&domain->EventRing.Lock, oldIrql);
        KgReleaseState(s);

        info = sizeof(KG_QUERY_EVENTS_OUTPUT);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_KG_QUERY_DENY_EVENTS:
    {
        if (inSize < sizeof(KG_QUERY_EVENTS_INPUT)) { status = STATUS_INVALID_PARAMETER; break; }
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KG_QUERY_DENY_EVENTS_OUTPUT)) {
            status = STATUS_BUFFER_TOO_SMALL; break;
        }
        KG_QUERY_EVENTS_INPUT* in = (KG_QUERY_EVENTS_INPUT*)buf;

        KG_SYSTEM_STATE* s = KgAcquireState();
        if (!s) { status = STATUS_UNSUCCESSFUL; break; }

        ULONG slot = KgFindDomainSlot(s, in->Sid);
        if (slot == (ULONG)-1) { KgReleaseState(s); status = STATUS_NOT_FOUND; break; }

        KG_POLICY_DOMAIN* domain = &s->Domain[slot];
        KG_QUERY_DENY_EVENTS_OUTPUT* out = (KG_QUERY_DENY_EVENTS_OUTPUT*)buf;
        out->EventCount = 0;

        KIRQL oldIrql;
        KeAcquireSpinLock(&domain->DenyRing.Lock, &oldIrql);

        LONG readIdx = domain->DenyRing.ReadIndex;
        LONG writeIdx = domain->DenyRing.WriteIndex;

        while (readIdx != writeIdx && out->EventCount < KG_DENY_EVENT_BATCH_SIZE) {
            RtlCopyMemory(&out->Events[out->EventCount],
                          &domain->DenyRing.Events[readIdx],
                          sizeof(KG_DENY_EVENT));
            out->EventCount++;
            readIdx = (readIdx + 1) % KG_DENY_RING_SIZE;
        }

        domain->DenyRing.ReadIndex = readIdx;

        KeReleaseSpinLock(&domain->DenyRing.Lock, oldIrql);
        KgReleaseState(s);

        info = sizeof(KG_QUERY_DENY_EVENTS_OUTPUT);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_KG_SET_NET_EXE_LIST:
    {
        if (!KgIsTrustedCaller()) { status = STATUS_ACCESS_DENIED; break; }
        if (inSize < sizeof(KG_NET_EXE_LIST_INPUT)) { status = STATUS_INVALID_PARAMETER; break; }
        KG_NET_EXE_LIST_INPUT* in = (KG_NET_EXE_LIST_INPUT*)buf;
        if (in->Count > KG_MAX_NET_EXE) { status = STATUS_INVALID_PARAMETER; break; }

        KG_SYSTEM_STATE* s = KgAcquireState();
        if (!s) { status = STATUS_UNSUCCESSFUL; break; }

        ULONG slot = KgFindDomainSlot(s, in->Sid);
        if (slot == (ULONG)-1) { KgReleaseState(s); status = STATUS_NOT_FOUND; break; }

        KG_POLICY_DOMAIN* domain = &s->Domain[slot];
        KIRQL oldIrql;
        KeAcquireSpinLock(&domain->Lock, &oldIrql);
        domain->NetExeCount = in->Count;
        for (ULONG i = 0; i < in->Count; i++) {
            in->Paths[i][259] = L'\0';
            wcsncpy_s(domain->NetExeList[i], 260, in->Paths[i], _TRUNCATE);
        }
        domain->NetBlockEnabled = TRUE;
        KeReleaseSpinLock(&domain->Lock, oldIrql);

        KgReleaseState(s);
        status = STATUS_SUCCESS;
        break;
    }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 3. 路径解析
//      通过 FltGetFileNameInformation 从 PFLT_CALLBACK_DATA 获取文件完整路径
//      这是做任何文件级决策的前提
///////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// 功能：从 PFLT_CALLBACK_DATA 中获取文件完整路径（规范化格式）
//       先通过 FltGetFileNameInformation 获取文件名信息，
//       再通过 FltParseFileNameInformation 解析为结构化格式
// 参数：
//   Data     - 回调数据
//   NameInfo - 输出参数，接收文件名信息指针
// 返回值：TRUE=路径解析成功，FALSE=解析失败
////////////////////////////////////////////////////////////////////////////////
static BOOLEAN
KgNormalizePath(PFLT_CALLBACK_DATA Data, PFLT_FILE_NAME_INFORMATION* NameInfo)
{
    NTSTATUS status;
    status = FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT, NameInfo);
    if (!NT_SUCCESS(status)) return FALSE;
    status = FltParseFileNameInformation(*NameInfo);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(*NameInfo);
        return FALSE;
    }
    return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 4. 决策与拦截
//      根据路径 + 操作类型决定放行或拒绝
//      拒绝时返回 FLT_PREOP_COMPLETE 并设置 IoStatus.Status = STATUS_ACCESS_DENIED
///////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// 功能：检查沙箱内进程的文件访问是否允许
//       流程：获取系统状态→拍摄 PID 快照→判断是否在沙箱内→
//       解析文件路径→输出日志
//       当前始终返回 TRUE（放行），后续在此处添加实际策略决策
// 参数：
//   Data - 回调数据（用于提取文件路径等信息）
//   Op   - 操作类型（Create/Read/Write/Delete 等）
// 返回值：TRUE=放行，FALSE=拒绝（当前始终返回 TRUE）
////////////////////////////////////////////////////////////////////////////////
/*
 * 文件访问检查（KgCheckFileAccess）
 *
 * 任意进程对文件的任意访问（读取、写入、创建、删除、枚举等），
 * 无论该进程是否在沙箱内，都会进入 minifilter 回调并调用此函数。
 *
 * 执行流程：
 *   1. 获取系统状态（KgAcquireState）
 *   2. 对当前 PID 拍摄快照（KgCaptureSnapshot），快速获知该进程
 *      是否在沙箱内以及所属的域
 *   3. 如果不在沙箱内，直接返回 TRUE，不做任何处理
 *   4. 如果在沙箱内，获取文件路径（KgNormalizePath）
 *   5. 输出日志：域ID、PID、文件路径、操作类型
 *   6. 返回 TRUE（放行），后续在此处添加实际的策略决策逻辑
 *
 * 返回值：TRUE = 放行，FALSE = 拒绝（当前始终返回 TRUE）
 */
static BOOLEAN KgCheckFileAccess(PFLT_CALLBACK_DATA Data, KG_OPERATION_TYPE Op)
{
    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) return TRUE;

    HANDLE pid = PsGetCurrentProcessId();
    ULONG slot = KgIsPidInSandBox(pid);
    if (slot == (ULONG)-1) {
        KgReleaseState(state);
        return TRUE;
    }

    PFLT_FILE_NAME_INFORMATION nameInfo;
    if (!KgNormalizePath(Data, &nameInfo)) {
        KgReleaseState(state);
        return TRUE;
    }

    KG_POLICY_DOMAIN* domain = &state->Domain[slot];
    KG_SANDBOX_LEVEL level = KgFindPathRule(domain, &nameInfo->Name);

    PCSTR opStr = KgOpToString(Op);
    if (opStr[0] != '\0') {
        KG_LOG("FileGuard: DomainID=%lu PID=%lu FileName=%wZ | %s | L%hhu\n",
                 domain->Id,
                 (ULONG)(ULONG_PTR)pid,
                 &nameInfo->Name,
                 opStr,
                 level);
    } else {
        KG_LOG("FileGuard: DomainID=%lu PID=%lu FileName=%wZ | L%hhu\n",
                 domain->Id,
                 (ULONG)(ULONG_PTR)pid,
                 &nameInfo->Name,
                 level);
    }

    FltReleaseFileNameInformation(nameInfo);
    KgReleaseState(state);
    return TRUE;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：拒绝文件访问 — 设置 STATUS_ACCESS_DENIED 并完成请求
//       当前预留，尚未实际投入使用
// 参数：
//   Data - 回调数据（设置 IoStatus 后完成）
// 返回值：FLT_PREOP_COMPLETE（请求已处理，下层不再处理）
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS KgDenyAccess(PFLT_CALLBACK_DATA Data)
{
    Data->IoStatus.Status = STATUS_ACCESS_DENIED;
    Data->IoStatus.Information = 0;
    return FLT_PREOP_COMPLETE;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：将 IRP 主功能号和小功能号映射为统一的 KG_OPERATION_TYPE
//       特别是对 IRP_MJ_SET_INFORMATION 按 FileInformationClass
//       细分为 Delete/Rename/SetInfo 三种语义
// 参数：
//   Iopb - IO 参数块（含 MajorFunction 和细分类信息）
// 返回值：对应的操作类型枚举值
////////////////////////////////////////////////////////////////////////////////
/*
 * 操作规范化（KgNormalizeOperation）
 *
 * 将 IRP 主功能号（MajorFunction）和小功能号（minor 信息类）映射为
 * 统一的 KG_OPERATION_TYPE 操作类型，供后续策略决策使用。
 *
 * 核心场景：IRP_MJ_SET_INFORMATION 的细分
 *   同一个 IRP_MJ_SET_INFORMATION 主功能号下，通过 FileInformationClass
 *   区分不同语义的操作：
 *     - FileDispositionInformation / FileDispositionInformationEx → KgOpDelete
 *       （标记文件待删除，句柄关闭时真正删除）
 *     - FileRenameInformation / FileRenameInformationEx → KgOpRename
 *       （重命名文件）
 *     - 其他信息类（FileBasicInformation、FileEndOfFileInformation 等）
 *       → KgOpSetInfo（修改属性/大小/时间戳）
 */
static KG_OPERATION_TYPE KgNormalizeOperation(PFLT_IO_PARAMETER_BLOCK Iopb)
{
    switch (Iopb->MajorFunction) {
    case IRP_MJ_CREATE:
        return KgOpCreate;

    case IRP_MJ_READ:
        return KgOpRead;

    case IRP_MJ_WRITE:
        return KgOpWrite;

    case IRP_MJ_SET_INFORMATION: {
        FILE_INFORMATION_CLASS infoClass =
            Iopb->Parameters.SetFileInformation.FileInformationClass;
        switch (infoClass) {
        case FileDispositionInformation:
        case FileDispositionInformationEx:
            return KgOpDelete;
        case FileRenameInformation:
        case FileRenameInformationEx:
            return KgOpRename;
        default:
            return KgOpSetInfo;
        }
    }

    case IRP_MJ_DIRECTORY_CONTROL:
        return KgOpEnumerate;

    case IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION:
        return KgOpExecute;

    default:
        return KgOpNone;
    }
}

////////////////////////////////////////////////////////////////////////////////
// 功能：将 IRP_MJ_CREATE 的 DesiredAccess 和 CreateOptions
//       翻译为统一的 KG_OPERATION_TYPE 操作类型
//       区分打开读取、打开写入、打开删除、打开执行等语义
//       没有此翻译 PreCreate 只能笼统传递 KgOpCreate
// 参数：
//   DesiredAccess - 请求的访问掩码
//   CreateOptions - 文件创建选项（含处置方式）
// 返回值：对应的操作类型枚举值
////////////////////////////////////////////////////////////////////////////////
/*
 * Create 访问翻译（KgTranslateCreateAccess）
 *
 * 将 IRP_MJ_CREATE 的 DesiredAccess（期望访问掩码）和 CreateOptions
 * （创建选项）翻译为实际的 KG_OPERATION_TYPE 操作意图。
 *
 * 同一个 IRP_MJ_CREATE 可能对应多种语义：
 *   - 进程以 FILE_WRITE_DATA 或 FILE_APPEND_DATA 权限打开文件
 *     → KgOpWrite（实际要写入）
 *   - 进程以 DELETE 权限打开文件 → KgOpDelete（实际要删除）
 *   - 进程以 FILE_EXECUTE 权限打开文件 → KgOpExecute（实际要执行）
 *   - 进程指定 FILE_CREATE / FILE_SUPERSEDE / FILE_OVERWRITE 等
 *     创建/覆盖类处置方式 → KgOpWrite（即使未显式请求写权限）
 *   - 其他情况 → KgOpCreate（单纯打开/创建，无特殊意图）
 *
 * 没有这个翻译，PreCreate 只能笼统地传 KgOpCreate，无法区分
 * "打开文件读取"和"打开文件写入"——后者在 L1 目录应该被拒绝。
 */
static KG_OPERATION_TYPE KgTranslateCreateAccess(ACCESS_MASK DesiredAccess, ULONG CreateOptions)
{
    ULONG disposition = CreateOptions & 0x00FFFFFF;

    if (DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA))
        return KgOpWrite;
    if (DesiredAccess & DELETE)
        return KgOpDelete;
    if (DesiredAccess & FILE_EXECUTE)
        return KgOpExecute;

    switch (disposition) {
    case FILE_CREATE:
    case FILE_SUPERSEDE:
        return KgOpWrite;
    case FILE_OVERWRITE:
    case FILE_OVERWRITE_IF:
        return KgOpWrite;
    }

    return KgOpCreate;
}

static BOOLEAN KgIsPureTraversal(ACCESS_MASK desiredAccess)
{
    ACCESS_MASK realAccess = desiredAccess &
        ~(FILE_TRAVERSE | SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_READ_EA);
    return (realAccess == 0) && (desiredAccess & FILE_TRAVERSE);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：将 KG_OPERATION_TYPE 枚举值转为可读的 ASCII 字符串
//       用于日志输出，方便调试和问题排查
// 参数：
//   op - 操作类型枚举值
// 返回值：操作类型字符串指针；未知类型返回空字符串
////////////////////////////////////////////////////////////////////////////////
/*
 * 操作类型转字符串（KgOpToString）
 *
 * 将 KG_OPERATION_TYPE 枚举值转为可读的 ASCII 字符串，
 * 用于日志输出，方便调试和问题排查。
 */
static PCSTR KgOpToString(KG_OPERATION_TYPE op)
{
    switch (op) {
    case KgOpCreate:    return "Create";
    case KgOpRead:      return "Read";
    case KgOpWrite:     return "Write";
    case KgOpDelete:    return "Delete";
    case KgOpRename:    return "Rename";
    case KgOpSetInfo:   return "SetInfo";
    case KgOpEnumerate: return "Enumerate";
    case KgOpExecute:   return "Execute";
    default:            return "";
    }
}

////////////////////////////////////////////////////////////////////////////////
// 功能：无锁快照捕获 — 判断当前 PID 是否在沙箱内并获取槽位索引
//       通过两轮读取 gPidMapEpoch 比较检测并发修改，
//       若 epoch 不一致则重试，确保读取的 SlotIndex 原子一致
//       整个过程中不获取任何锁，避免在分页 IO 路径中死锁
// 参数：
//   state - 系统状态（含 PidMap 指针）
//   pid   - 目标进程 PID
//   snap  - 输出参数，接收快照结果（SlotIndex、IsSandboxed、PidMapEpoch）
// 返回值：TRUE 表示快照捕获成功
////////////////////////////////////////////////////////////////////////////////
/*
 * 快照捕获（KgCaptureSnapshot）
 *
 * 任意进程对文件的任意访问（读取、写入、创建、删除、枚举等），
 * 无论该进程是否在沙箱内，都会触发 KgCheckFileAccess 的执行。
 * KgCheckFileAccess 首先调用 KgCaptureSnapshot，用不加锁的方式
 * 对 PidMap 快速拍摄一份"快照"，目的是在执行完捕获后，通过
 * snap.IsSandboxed 获知当前 PID 是否为沙箱内的进程。
 *
 * 技术实现：
 *   通过读取 gPidMapEpoch 两次 + 比较，检测 PidMap 在捕获期间
 *   是否被并发修改（process notify 的创建/退出回调会替换 PidMap）。
 *   若 epoch 不一致则重试，确保读取的 SlotIndex 是原子一致的快照。
 *   整个过程中不获取任何锁，避免在分页 IO 路径中死锁。
 */

static BOOLEAN KgCaptureSnapshot(KG_SYSTEM_STATE* state, HANDLE pid, KG_SNAPSHOT* snap)
{
    for (;;) {
        LONG epoch1 = gPidMapEpoch;
        KG_PID_MAP* pidMap = state->PidMap;
        snap->SlotIndex = (ULONG)-1;
        snap->IsSandboxed = FALSE;

        if (pidMap && ExAcquireRundownProtection(&pidMap->RundownRef)) {
            LONG epoch2 = gPidMapEpoch;
            if (epoch1 == epoch2) {
                snap->PidMapEpoch = epoch1;
                snap->SlotIndex = KgLookupSlotByPid(pidMap, pid);
                snap->IsSandboxed = (snap->SlotIndex != (ULONG)-1);
                ExReleaseRundownProtection(&pidMap->RundownRef);
                return TRUE;
            }
            ExReleaseRundownProtection(&pidMap->RundownRef);
        } else {
            snap->PidMapEpoch = gPidMapEpoch;
            return TRUE;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// 目录枚举过滤 — 类型与辅助函数
////////////////////////////////////////////////////////////////////////////////

typedef struct _KG_DIR_ENUM_CTX {
    UNICODE_STRING DirectoryPath; // Pre 回调中保存的当前枚举目录路径
    PEPROCESS RequestorProcess;   // 发起目录枚举请求的进程 EPROCESS
    WCHAR Buffer[1];              // 柔性数组，实际存放目录路径字符串
} KG_DIR_ENUM_CTX;

typedef struct _KG_DIR_ENTRY_OFFSETS {
    FILE_INFORMATION_CLASS InfoClass;     // 目录信息结构类型（如 FileDirectoryInformation）
    USHORT FileNameLengthOffset;          // 该结构中 FileNameLength 字段的偏移量
    USHORT FileNameOffset;                // 该结构中 FileName 字段的偏移量
} KG_DIR_ENTRY_OFFSETS;

static const KG_DIR_ENTRY_OFFSETS KgDirEntryOffsets[] = {
    { FileDirectoryInformation,          FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileNameLength),
                                         FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) },
    { FileFullDirectoryInformation,      FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileNameLength),
                                         FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) },
    { FileBothDirectoryInformation,      FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileNameLength),
                                         FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) },
    { FileNamesInformation,              FIELD_OFFSET(FILE_NAMES_INFORMATION, FileNameLength),
                                         FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) },
    { FileIdBothDirectoryInformation,    FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileNameLength),
                                         FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName) },
    { FileIdFullDirectoryInformation,    FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileNameLength),
                                         FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName) },
};

static BOOLEAN
KgLookupDirEntryOffsets(FILE_INFORMATION_CLASS infoClass, USHORT* fnLenOffset, USHORT* fnOffset)
{
    for (ULONG i = 0; i < sizeof(KgDirEntryOffsets) / sizeof(KgDirEntryOffsets[0]); i++) {
        if (KgDirEntryOffsets[i].InfoClass == infoClass) {
            *fnLenOffset = KgDirEntryOffsets[i].FileNameLengthOffset;
            *fnOffset = KgDirEntryOffsets[i].FileNameOffset;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
KgValidateDirEntry(PUCHAR entry, ULONG entrySize, USHORT fnLenOffset)
{
    if (entrySize < sizeof(ULONG))
        return FALSE;
    if (fnLenOffset + sizeof(ULONG) > entrySize)
        return FALSE;
    ULONG fnLen = *(ULONG*)(entry + fnLenOffset);
    if (fnLenOffset + sizeof(ULONG) + fnLen > entrySize)
        return FALSE;
    return TRUE;
}

static BOOLEAN
KgCheckEnumVisibility(HANDLE pid, PUNICODE_STRING path)
{
    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) return TRUE;

    ULONG slot = KgIsPidInSandBox(pid);
    if (slot == (ULONG)-1) {
        KgReleaseState(state);
        return TRUE;
    }

    KG_POLICY_DOMAIN* domain = &state->Domain[slot];
    KG_SANDBOX_LEVEL level = 0;
    USHORT bestLen = 0;

    for (PLIST_ENTRY e = domain->RuleList.Flink; e != &domain->RuleList; e = e->Flink) {
        KG_PATH_RULE* rule = CONTAINING_RECORD(e, KG_PATH_RULE, Link);
        if (rule->PathPrefix.Length <= path->Length &&
            RtlPrefixUnicodeString(&rule->PathPrefix, path, TRUE) &&
            rule->PathPrefix.Length > bestLen)
        {
            if (rule->PathPrefix.Length < path->Length) {
                WCHAR nextChar = path->Buffer[rule->PathPrefix.Length / sizeof(WCHAR)];
                if (nextChar != L'\\')
                    continue;
            }
            level = rule->Level;
            bestLen = rule->PathPrefix.Length;
        }
    }

    KgReleaseState(state);

    return (level >= 1);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：文件创建/打开前置回调（IRP_MJ_CREATE）
//       通过 KgTranslateCreateAccess 将 Create 的 DesiredAccess
//       翻译为具体的操作类型（Read/Write/Delete/Execute），
//       然后执行文件访问检查并记录日志
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreCreate(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    ACCESS_MASK desiredAccess = 0;
    if (Data->Iopb->Parameters.Create.SecurityContext)
        desiredAccess = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;

    // 纯遍历（FILE_TRAVERSE）且路径是绑定根祖先 → 直接放行
    if (KgIsPureTraversal(desiredAccess)) {
        PFLT_FILE_NAME_INFORMATION nameInfo;
        if (KgNormalizePath(Data, &nameInfo)) {
            KG_SYSTEM_STATE* state = KgAcquireState();
            if (state) {
                ULONG slot = KgIsPidInSandBox(PsGetCurrentProcessId());
                if (slot != (ULONG)-1 && KgIsTraversalAllowed(state, slot, &nameInfo->Name)) {
                    KgReleaseState(state);
                    FltReleaseFileNameInformation(nameInfo);
                    return FLT_PREOP_SUCCESS_NO_CALLBACK;
                }
                KgReleaseState(state);
            }
            FltReleaseFileNameInformation(nameInfo);
        }
    }

    // 规范化路径
    PFLT_FILE_NAME_INFORMATION nameInfo;
    if (!KgNormalizePath(Data, &nameInfo))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    // 绿色通道：访问 .svn 目录（无论层级多深）直接放行
    // svn 会逐级向上探测 .svn，绝大多数路径不存在，让 OS 返回"文件未找到"即可
    if (nameInfo->Name.Length >= 8) {
        PWCHAR buf = nameInfo->Name.Buffer;
        USHORT len = nameInfo->Name.Length / sizeof(WCHAR);
        if (buf[len - 4] == L'.' && buf[len - 3] == L's' &&
            buf[len - 2] == L'v' && buf[len - 1] == L'n' &&
            (len == 4 || buf[len - 5] == L'\\')) {
            FltReleaseFileNameInformation(nameInfo);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }

    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    HANDLE pid = PsGetCurrentProcessId();
    ULONG slot = KgIsPidInSandBox(pid);
    BOOLEAN isSandboxed = (slot != (ULONG)-1);

    KG_SANDBOX_LEVEL level = 0;

    if (isSandboxed) {
        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
        KIRQL dIrql;
        KeAcquireSpinLock(&domain->Lock, &dIrql);
        level = KgFindPathRule(domain, &nameInfo->Name);
        KeReleaseSpinLock(&domain->Lock, dIrql);
    }

    if (level == 0) {
        KG_POLICY_DOMAIN* global = &state->Domain[0];
        KIRQL dIrql;
        KeAcquireSpinLock(&global->Lock, &dIrql);
        level = KgFindPathRule(global, &nameInfo->Name);
        KeReleaseSpinLock(&global->Lock, dIrql);
    }

    // 非沙箱进程 + 无匹配规则 → 直接放行
    if (!isSandboxed && level == 0) {
        KgReleaseState(state);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    // 沙箱进程 + 路径是绑定根祖先目录 → 放行（需先遍历到子目录）
    if (isSandboxed && level == 0) {
        if (KgIsTraversalAllowed(state, slot, &nameInfo->Name)) {
            level = 1;  // 授予遍历级别的权限
        }
    }

    // 翻译 Create 意图
    ULONG createOptions = Data->Iopb->Parameters.Create.Options;
    KG_OPERATION_TYPE op = KgTranslateCreateAccess(desiredAccess, createOptions);

    // level-vs-operation 裁决
    BOOLEAN allowed;
    switch (op) {
    case KgOpCreate:
    case KgOpRead:
    case KgOpExecute:
        allowed = (level >= 1);
        break;
    case KgOpWrite:
    case KgOpDelete:
        allowed = (level >= 2);
        break;
    default:
        allowed = (level >= 2);
        break;
    }

    // 规范化路径通过后，再验一次 opened path（防符号链接绕过）
    if (allowed) {
        PFLT_FILE_NAME_INFORMATION openedInfo = NULL;
        NTSTATUS openStatus = FltGetFileNameInformation(Data,
            FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT, &openedInfo);
        if (NT_SUCCESS(openStatus)) {
            openStatus = FltParseFileNameInformation(openedInfo);
            if (NT_SUCCESS(openStatus)) {
                if (RtlCompareUnicodeString(&nameInfo->Name, &openedInfo->Name, TRUE) != 0) {
                    // 规范化路径和实际打开路径不一致 → 用 opened 路径再查一次
                    KIRQL dIrql;
                    KG_SANDBOX_LEVEL openedLevel = 0;
                    if (isSandboxed) {
                        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
                        KeAcquireSpinLock(&domain->Lock, &dIrql);
                        openedLevel = KgFindPathRule(domain, &openedInfo->Name);
                        KeReleaseSpinLock(&domain->Lock, dIrql);
                    }
                    if (openedLevel == 0) {
                        KG_POLICY_DOMAIN* global = &state->Domain[0];
                        KeAcquireSpinLock(&global->Lock, &dIrql);
                        openedLevel = KgFindPathRule(global, &openedInfo->Name);
                        KeReleaseSpinLock(&global->Lock, dIrql);
                    }
                    // opened 路径也没匹配到规则 → 同主路径一样检查遍历祖先
                    if (openedLevel == 0 && isSandboxed) {
                        if (KgIsTraversalAllowed(state, slot, &openedInfo->Name)) {
                            openedLevel = 1;
                        }
                    }
                    if (!isSandboxed && openedLevel == 0) {
                        // 非沙箱 + 无规则 → 放行
                    } else {
                        BOOLEAN openedAllowed;
                        switch (op) {
                        case KgOpCreate:
                        case KgOpRead:
                        case KgOpExecute:
                            openedAllowed = (openedLevel >= 1);
                            break;
                        default:
                            openedAllowed = (openedLevel >= 2);
                            break;
                        }
                        allowed = openedAllowed;
                    }
                }
            }
            FltReleaseFileNameInformation(openedInfo);
        }
    }

    // 拒绝：日志（在释放资源前，还能读到路径）
    PCSTR denyOp = (op == KgOpCreate) ? "Create" :
                   (op == KgOpRead)   ? "Read" :
                   (op == KgOpWrite)  ? "Write" :
                   (op == KgOpDelete) ? "Delete" :
                   (op == KgOpExecute) ? "Execute" : "?";
    if (!allowed) {
        DbgPrint("FileGuard: DENY PID=%lu Level=%hhu Op=%s File=%wZ\n",
                 (ULONG)(ULONG_PTR)pid, level, denyOp, &nameInfo->Name);
        KgPushDenyEvent(&state->Domain[isSandboxed ? slot : 0], pid, &nameInfo->Name);
    }

    KgReleaseState(state);
    FltReleaseFileNameInformation(nameInfo);

    if (!allowed) {
        Data->IoStatus.Status = (op == KgOpCreate) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：文件写入前置回调（IRP_MJ_WRITE）
//      非分页 IO 路径记录写操作日志，分页 IO 路径直接放行
// 参数：
//      Data              - 回调数据
//      FltObjects        - 相关对象
//      CompletionContext - 完成上下文
// 返回值：
//      FLT_PREOP_SUCCESS_NO_CALLBACK
// 重要说明:
//      大部分情况 PreWrite 之前，一定发生 PreCreate，只有以下情况它会独立发生
//      句柄继承 — 子进程继承父进程已打开的文件句柄，直接写入，不会触发子进程的 PreCreate
//      DuplicateHandle — 通过复制句柄获得写入权限，无 PreCreate
//      Cache Manager 回写 — 缓存管理器异步将脏页写回磁盘，此时原始句柄可能早已关闭，PreWrite 独立触发
//      内存映射文件 — CreateFileMapping + MapViewOfFile 后修改内存，脏页回写时走分页 IO 路径
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreWrite(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    // 分页 IO 路径中禁止路径解析和策略决策，直接放行。
    if (Data->Iopb->IrpFlags & IRP_PAGING_IO)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    PFLT_FILE_NAME_INFORMATION nameInfo;
    if (!KgNormalizePath(Data, &nameInfo))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    HANDLE pid = PsGetCurrentProcessId();
    ULONG slot = KgIsPidInSandBox(pid);
    BOOLEAN isSandboxed = (slot != (ULONG)-1);

    KG_SANDBOX_LEVEL level = 0;

    if (isSandboxed) {
        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
        KIRQL dIrql;
        KeAcquireSpinLock(&domain->Lock, &dIrql);
        level = KgFindPathRule(domain, &nameInfo->Name);
        KeReleaseSpinLock(&domain->Lock, dIrql);
    }

    if (level == 0) {
        KG_POLICY_DOMAIN* global = &state->Domain[0];
        KIRQL dIrql;
        KeAcquireSpinLock(&global->Lock, &dIrql);
        level = KgFindPathRule(global, &nameInfo->Name);
        KeReleaseSpinLock(&global->Lock, dIrql);
    }

    if (!isSandboxed && level == 0) {
        KgReleaseState(state);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    BOOLEAN allowed = (level >= 2);

    if (allowed) {
        PFLT_FILE_NAME_INFORMATION openedInfo = NULL;
        NTSTATUS openStatus = FltGetFileNameInformation(Data,
            FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT, &openedInfo);
        if (NT_SUCCESS(openStatus)) {
            openStatus = FltParseFileNameInformation(openedInfo);
            if (NT_SUCCESS(openStatus)) {
                if (RtlCompareUnicodeString(&nameInfo->Name, &openedInfo->Name, TRUE) != 0) {
                    KIRQL dIrql;
                    KG_SANDBOX_LEVEL openedLevel = 0;
                    if (isSandboxed) {
                        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
                        KeAcquireSpinLock(&domain->Lock, &dIrql);
                        openedLevel = KgFindPathRule(domain, &openedInfo->Name);
                        KeReleaseSpinLock(&domain->Lock, dIrql);
                    }
                    if (openedLevel == 0) {
                        KG_POLICY_DOMAIN* global = &state->Domain[0];
                        KeAcquireSpinLock(&global->Lock, &dIrql);
                        openedLevel = KgFindPathRule(global, &openedInfo->Name);
                        KeReleaseSpinLock(&global->Lock, dIrql);
                    }
                    if (!isSandboxed && openedLevel == 0) {
                    } else {
                        allowed = (openedLevel >= 2);
                    }
                }
            }
            FltReleaseFileNameInformation(openedInfo);
        }
    }

    if (!allowed) {
        DbgPrint("FileGuard: DENY PID=%lu Level=%hhu Op=Write File=%wZ\n",
                 (ULONG)(ULONG_PTR)pid, level, &nameInfo->Name);
        KgPushDenyEvent(&state->Domain[isSandboxed ? slot : 0], pid, &nameInfo->Name);
    }

    KgReleaseState(state);
    FltReleaseFileNameInformation(nameInfo);

    if (!allowed) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：文件信息设置前置回调（IRP_MJ_SET_INFORMATION）
//       通过 KgNormalizeOperation 区分删除/重命名/元数据修改，
//       内联执行访问裁决，不依赖外部统一权限函数。
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK 或 FLT_PREOP_COMPLETE（拒绝时）
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreSetInfo(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    PFLT_FILE_NAME_INFORMATION nameInfo;
    if (!KgNormalizePath(Data, &nameInfo))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    HANDLE pid = PsGetCurrentProcessId();
    ULONG slot = KgIsPidInSandBox(pid);
    BOOLEAN isSandboxed = (slot != (ULONG)-1);

    KG_SANDBOX_LEVEL level = 0;

    if (isSandboxed) {
        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
        KIRQL dIrql;
        KeAcquireSpinLock(&domain->Lock, &dIrql);
        level = KgFindPathRule(domain, &nameInfo->Name);
        KeReleaseSpinLock(&domain->Lock, dIrql);
    }

    if (level == 0) {
        KG_POLICY_DOMAIN* global = &state->Domain[0];
        KIRQL dIrql;
        KeAcquireSpinLock(&global->Lock, &dIrql);
        level = KgFindPathRule(global, &nameInfo->Name);
        KeReleaseSpinLock(&global->Lock, dIrql);
    }

    KG_OPERATION_TYPE op = KgNormalizeOperation(Data->Iopb);

    // 对于重命名/移动操作，从 IRP 参数中提取目标路径并查规则
    KG_SANDBOX_LEVEL targetLevel = 0;
    BOOLEAN hasTarget = FALSE;
    if (op == KgOpRename) {
        PFLT_PARAMETERS param = &Data->Iopb->Parameters;
        PVOID buf = NULL;
        __try {
            buf = param->SetFileInformation.InfoBuffer;
            if (buf)
                ProbeForRead(buf, param->SetFileInformation.Length, sizeof(UCHAR));
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            buf = NULL;
        }
        if (buf && param->SetFileInformation.Length >= sizeof(FILE_RENAME_INFORMATION)) {
            FILE_RENAME_INFORMATION* rinfo = (FILE_RENAME_INFORMATION*)buf;
            ULONG fnameByteLen = rinfo->FileNameLength;
            if (fnameByteLen > 0 &&
                fnameByteLen + FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) <= param->SetFileInformation.Length &&
                rinfo->FileName[0] == L'\\') {
                UNICODE_STRING tgtPath;
                tgtPath.Buffer = rinfo->FileName;
                tgtPath.Length = (USHORT)fnameByteLen;
                tgtPath.MaximumLength = (USHORT)fnameByteLen;
                if (isSandboxed) {
                    KG_POLICY_DOMAIN* domain = &state->Domain[slot];
                    KIRQL dIrql;
                    KeAcquireSpinLock(&domain->Lock, &dIrql);
                    targetLevel = KgFindPathRule(domain, &tgtPath);
                    KeReleaseSpinLock(&domain->Lock, dIrql);
                }
                if (targetLevel == 0) {
                    KG_POLICY_DOMAIN* global = &state->Domain[0];
                    KIRQL dIrql;
                    KeAcquireSpinLock(&global->Lock, &dIrql);
                    targetLevel = KgFindPathRule(global, &tgtPath);
                    KeReleaseSpinLock(&global->Lock, dIrql);
                }
                hasTarget = TRUE;
                KG_LOG("DomainID=%lu PID=%lu RenameTarget=%wZ | L%hhu\n",
                         state->Domain[isSandboxed ? slot : 0].Id,
                         (ULONG)(ULONG_PTR)pid,
                         &tgtPath,
                         targetLevel);
            }
        }
    }

    BOOLEAN allowed = TRUE;
    if (hasTarget) {
        // 重命名三规则：
        //   源 L0/L1 → 拒绝（2）；目标 L0/L1 → 拒绝（3）；两者皆 L2 → 放行（1）
        if (level < 2 || targetLevel < 2)
            allowed = FALSE;
    } else if (isSandboxed || level != 0) {
        switch (op) {
        case KgOpDelete:
        case KgOpRename:
        case KgOpSetInfo:
            allowed = (level >= 2);
            break;
        default:
            break;
        }
    }

    if (allowed) {
        PFLT_FILE_NAME_INFORMATION openedInfo = NULL;
        NTSTATUS openStatus = FltGetFileNameInformation(Data,
            FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT, &openedInfo);
        if (NT_SUCCESS(openStatus)) {
            openStatus = FltParseFileNameInformation(openedInfo);
            if (NT_SUCCESS(openStatus)) {
                if (RtlCompareUnicodeString(&nameInfo->Name, &openedInfo->Name, TRUE) != 0) {
                    KIRQL dIrql;
                    KG_SANDBOX_LEVEL openedLevel = 0;
                    if (isSandboxed) {
                        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
                        KeAcquireSpinLock(&domain->Lock, &dIrql);
                        openedLevel = KgFindPathRule(domain, &openedInfo->Name);
                        KeReleaseSpinLock(&domain->Lock, dIrql);
                    }
                    if (openedLevel == 0) {
                        KG_POLICY_DOMAIN* global = &state->Domain[0];
                        KeAcquireSpinLock(&global->Lock, &dIrql);
                        openedLevel = KgFindPathRule(global, &openedInfo->Name);
                        KeReleaseSpinLock(&global->Lock, dIrql);
                    }
                    if (isSandboxed || openedLevel != 0) {
                        switch (op) {
                        case KgOpDelete:
                        case KgOpRename:
                        case KgOpSetInfo:
                            allowed = (openedLevel >= 2);
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            FltReleaseFileNameInformation(openedInfo);
        }
    }

    if (!allowed) {
        PCSTR denyOp = (op == KgOpDelete) ? "Delete" :
                       (op == KgOpRename) ? "Rename" : "SetInfo";
        if (hasTarget) {
            DbgPrint("FileGuard: DENY PID=%lu Level=%hhu TargetLevel=%hhu Op=%s File=%wZ\n",
                     (ULONG)(ULONG_PTR)pid, level, targetLevel, denyOp, &nameInfo->Name);
        } else {
        DbgPrint("FileGuard: DENY PID=%lu Level=%hhu Op=%s File=%wZ\n",
                 (ULONG)(ULONG_PTR)pid, level, denyOp, &nameInfo->Name);
        }
        KgPushDenyEvent(&state->Domain[isSandboxed ? slot : 0], pid, &nameInfo->Name);
    }

    KgReleaseState(state);
    FltReleaseFileNameInformation(nameInfo);

    if (!allowed) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：文件清理前置回调（IRP_MJ_CLEANUP）
//       当文件句柄关闭时，检查是否设置了 FO_DELETE_ON_CLOSE 标志，
//       若是则内联执行删除权限裁决，无权时清除该标志阻止文件被删除。
//       本回调不依赖外部统一权限函数，全部逻辑在函数内部完成。
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreCleanup(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    // 从 minifilter 回调参数中获取文件对象指针。
    // FileObject 是当前 IO 操作的目标，其 Flags 字段包含文件对象的状态标志。
    PFILE_OBJECT fileObj = FltObjects->FileObject;
    // 检查文件对象是否存在，以及是否设置了 FO_DELETE_ON_CLOSE 标志。
    // FO_DELETE_ON_CLOSE 表示文件在句柄关闭时应当被自动删除。
    // 若文件对象不存在或未设置该标志，则无需干预，直接放行。
    if (!fileObj || !(fileObj->Flags & FO_DELETE_ON_CLOSE))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    // 规范化路径：获取文件对象对应的完整路径，用于后续规则查找和权限裁决。
    PFLT_FILE_NAME_INFORMATION nameInfo;
    if (!KgNormalizePath(Data, &nameInfo))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    HANDLE pid = PsGetCurrentProcessId();
    ULONG slot = KgIsPidInSandBox(pid);
    BOOLEAN isSandboxed = (slot != (ULONG)-1);

    KG_SANDBOX_LEVEL level = 0;
    if (isSandboxed) {
        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
        KIRQL dIrql;
        KeAcquireSpinLock(&domain->Lock, &dIrql);
        level = KgFindPathRule(domain, &nameInfo->Name);
        KeReleaseSpinLock(&domain->Lock, dIrql);
    }

    if (level == 0) {
        KG_POLICY_DOMAIN* global = &state->Domain[0];
        KIRQL dIrql;
        KeAcquireSpinLock(&global->Lock, &dIrql);
        level = KgFindPathRule(global, &nameInfo->Name);
        KeReleaseSpinLock(&global->Lock, dIrql);
    }

    // 文件设置了 FO_DELETE_ON_CLOSE，通过内联的 level-vs-operation 裁决
    // 判断该进程对当前文件是否具备删除权限。删除需要 level >= 2（L2）。
    // 若有权删除，则无需干预，放行让文件系统在关闭句柄时完成删除操作。
    BOOLEAN allowed = TRUE;
    if (isSandboxed || level != 0)
        allowed = (level >= 2);

    if (allowed) {
        PFLT_FILE_NAME_INFORMATION openedInfo = NULL;
        NTSTATUS openStatus = FltGetFileNameInformation(Data,
            FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT, &openedInfo);
        if (NT_SUCCESS(openStatus)) {
            openStatus = FltParseFileNameInformation(openedInfo);
            if (NT_SUCCESS(openStatus)) {
                if (RtlCompareUnicodeString(&nameInfo->Name, &openedInfo->Name, TRUE) != 0) {
                    KIRQL dIrql;
                    KG_SANDBOX_LEVEL openedLevel = 0;
                    if (isSandboxed) {
                        KG_POLICY_DOMAIN* domain = &state->Domain[slot];
                        KeAcquireSpinLock(&domain->Lock, &dIrql);
                        openedLevel = KgFindPathRule(domain, &openedInfo->Name);
                        KeReleaseSpinLock(&domain->Lock, dIrql);
                    }
                    if (openedLevel == 0) {
                        KG_POLICY_DOMAIN* global = &state->Domain[0];
                        KeAcquireSpinLock(&global->Lock, &dIrql);
                        openedLevel = KgFindPathRule(global, &openedInfo->Name);
                        KeReleaseSpinLock(&global->Lock, dIrql);
                    }
                    if (isSandboxed || openedLevel != 0)
                        allowed = (openedLevel >= 2);
                }
            }
            FltReleaseFileNameInformation(openedInfo);
        }
    }

    // 权限不足：从文件对象的 Flags 中清除 FO_DELETE_ON_CLOSE 标志。
    // 这样文件系统在后续处理 Cleanup IRP 时不会执行删除动作，
    // 文件被保留。句柄本身正常关闭，调用者收到成功的返回值。
    if (!allowed) {
        DbgPrint("FileGuard: DENY PID=%lu Level=%hhu Op=DeleteCleanup File=%wZ\n",
                 (ULONG)(ULONG_PTR)pid, level, &nameInfo->Name);
        KgPushDenyEvent(&state->Domain[isSandboxed ? slot : 0], pid, &nameInfo->Name);
        fileObj->Flags &= ~FO_DELETE_ON_CLOSE;
    }

    KgReleaseState(state);
    FltReleaseFileNameInformation(nameInfo);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：目录枚举前置回调（IRP_MJ_DIRECTORY_CONTROL）
//       保存当前目录路径和请求进程到 CompletionContext 中，供 Post 回调使用
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 输出参数，接收 KG_DIR_ENUM_CTX 指针
// 返回值：FLT_PREOP_SUCCESS_WITH_CALLBACK（注册 Post 回调）
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreDirCtrl(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);

    HANDLE pid = PsGetCurrentProcessId();
    KG_SYSTEM_STATE* s = KgAcquireState();
    BOOLEAN isSandboxed = FALSE;
    if (s) {
        isSandboxed = (KgIsPidInSandBox(pid) != (ULONG)-1);
        KgReleaseState(s);
    }
    if (!isSandboxed)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    PFLT_FILE_NAME_INFORMATION nameInfo;
    NTSTATUS status = FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT, &nameInfo);
    if (!NT_SUCCESS(status)) {
        KG_DIR_ENUM_CTX* ctx = (KG_DIR_ENUM_CTX*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_DIR_ENUM_CTX), KG_POOL_TAG);
        if (!ctx) return FLT_PREOP_SUCCESS_NO_CALLBACK;
        ctx->DirectoryPath.Length = 0;
        ctx->DirectoryPath.Buffer = NULL;
        ctx->RequestorProcess = FltGetRequestorProcess(Data);
        *CompletionContext = ctx;
        return FLT_PREOP_SUCCESS_WITH_CALLBACK;
    }
    status = FltParseFileNameInformation(nameInfo);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        KG_DIR_ENUM_CTX* ctx = (KG_DIR_ENUM_CTX*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_DIR_ENUM_CTX), KG_POOL_TAG);
        if (!ctx) return FLT_PREOP_SUCCESS_NO_CALLBACK;
        ctx->DirectoryPath.Length = 0;
        ctx->DirectoryPath.Buffer = NULL;
        ctx->RequestorProcess = FltGetRequestorProcess(Data);
        *CompletionContext = ctx;
        return FLT_PREOP_SUCCESS_WITH_CALLBACK;
    }

    USHORT pathByteLen = nameInfo->Name.Length;
    ULONG ctxSize = sizeof(KG_DIR_ENUM_CTX) + pathByteLen + sizeof(WCHAR);
    KG_DIR_ENUM_CTX* ctx = (KG_DIR_ENUM_CTX*)ExAllocatePool2(POOL_FLAG_NON_PAGED, ctxSize, KG_POOL_TAG);
    if (!ctx) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlCopyMemory(ctx->Buffer, nameInfo->Name.Buffer, pathByteLen);
    ctx->Buffer[pathByteLen / sizeof(WCHAR)] = L'\0';
    ctx->DirectoryPath.Buffer = ctx->Buffer;
    ctx->DirectoryPath.Length = pathByteLen;
    ctx->DirectoryPath.MaximumLength = pathByteLen + sizeof(WCHAR);
    ctx->RequestorProcess = FltGetRequestorProcess(Data);
    *CompletionContext = ctx;
    FltReleaseFileNameInformation(nameInfo);
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：
//      节同步前置回调 IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION
// 发生时机：
//      当 Windows 内存管理器为磁盘上的数据文件创建节对象（Section Object）时，就会向文件系统设备栈发送此 IRP。
// 具体触发路径包括：
//      用户态调用 CreateFileMapping 传入有效的文件句柄
//      内核态调用 ZwCreateSection / MmCreateSection 并指定数据文件作为后备存储
//      系统在实现内存映射文件、共享内存、缓存管理器扩展等场景中创建数据节
// 参数：
//      Data              - 回调数据
//      FltObjects        - 相关对象
//      CompletionContext - 完成上下文
// 返回值：
//      FLT_PREOP_SUCCESS_NO_CALLBACK
// 重要提醒：
//      此回调运行在 NtCreateUserProcess 的同步路径上，不可使用任何可能阻塞的操作
//      包括 ExAcquireRundownProtection，否则会导致 CreateProcess 卡死。
// 典型文件访问流程：
//      CreateProcess 加载 EXE	PreCreate → PreExecute → SectionSync
//      LoadLibrary 加载 DLL	PreCreate → PreExecute → SectionSync
//      CreateFileMapping	    PreCreate → SectionSync
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreAcquireForSectionSync(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    /*
     * 这个函数是 IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION 的 minifilter
     * 前置回调。当内存管理器将文件作为"节"（Section）映射到地址空间时触发。
     *
     * 触发场景：
     *   CreateProcess / LoadLibrary  → 映射 PE 的代码段
     *   CreateFileMapping            → 映射数据文件为共享内存
     *
     * ── 为什么这个函数什么都不做 ──
     *
     * 这个回调运行在内存管理器的 Section 创建锁（Section Creation Lock）
     * 之下。该锁是系统级全局锁，在 NtCreateUserProcess / LdrLoadDll 等
     * 关键路径上被持有。在此锁内执行任何可能阻塞的操作都会导致死锁。
     *
     * 在 PreAcquireForSectionSync 中禁止的操作：
     *   ❌ FltGetFileNameInformation / FltParseFileNameInformation
     *       会走到文件系统栈获取路径，而文件系统可能正在等待 Section 锁
     *   ❌ KeAcquireSpinLock / ExAcquireRundownProtection
     *       可能与其他持有 SpinLock 并等待 Section 锁的线程形成循环等待
     *   ❌ ExAllocatePool2（非安全）
     *       部分分配路径内部可能触发 Page Fault，而在 Section 锁下
     *       处理 Page Fault 会导致递归死锁
     *   ❌ ProbeForRead / 访问用户态内存
     *       当前上下文可能处于 APC 级别之上，访问用户态内存会崩溃
     *   ✅ 唯一允许的操作：返回 FLT_PREOP_SUCCESS_NO_CALLBACK
     *      或 FLT_PREOP_COMPLETE（设置 IoStatus 拒绝当前映射）
     *
     * ── 真正的拦截点在 PreCreate，而非这里 ──
     *
     * 文件访问的完整路径为：
     *   IRP_MJ_CREATE (PreCreate) → 打开文件
     *   IRP_MJ_ACQUIRE_FOR_SECTION_SYNC → 映射为节
     *
     * 策略决策在 PreCreate 阶段就已做出：
     *   L0 路径 → PreCreate 拒绝 FILE_EXECUTE 打开 → NtCreateFile 失败
     *            → 后续 SectionSync 根本不会到达
     *   L2/L1 路径 → PreCreate 放行 → SectionSync 直通放行
     *
     * 如果某文件的 CREATE 已被放行（如因 bwrap 模式：先 CreateProcess
     * 后 AttachProcess），那么在 SectionSync 阶段拒绝为时已晚：
     *   - 文件对象已创建、句柄已返回、进程已初始化
     *   - 此时拒绝映射不仅无法阻止执行（代码可能已部分加载），
     *     反而可能导致进程在不可恢复的状态下崩溃
     *   - 拦截必须发生在 CreateProcess 路径的 CREATE 阶段，而非映射阶段
     *
     * ── 关于 CreateFileMapping 的特殊考虑 ──
     *
     * CreateFileMapping 确实会先触发 PreCreate，再触发 SectionSync。
     * PreCreate 已经完成权限判断（GENERIC_READ|GENERIC_WRITE 对应写入意图），
     * 如果 PreCreate 放行，说明文件路径允许该访问级别，SectionSync 无需
     * 再做重复判断。
     *
     * 综合以上所有约束，此函数的最佳选择就是什么都不做，直接放行。
     */
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：文件读取前置回调（IRP_MJ_READ）
//       非分页 IO 路径记录读操作日志，分页 IO 路径直接放行
//       分页 IO 路径中禁止执行路径解析或获取锁等操作，
//       否则可能触发递归 page fault 导致死锁
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK
////////////////////////////////////////////////////////////////////////////////
/*
 * 分页 IO（IRP_PAGING_IO）由内存管理器在 page fault 时发起，
 * 用于从磁盘读取换出的代码/数据页，或将脏页写回磁盘。
 * 常见触发场景：
 *   1. 进程执行尚未加载到物理内存的代码（如 DLL 初始化阶段）
 *   2. 访问内存映射文件（CreateFileMapping + MapViewOfFile）的首次访问
 *   3. CreateProcess / LoadLibrary 加载 PE 文件时的代码段映射
 *
 * 在分页 IO 路径中，驱动不得执行路径解析（FltGetFileNameInformation）、
 * 获取锁、分配内存等操作，否则可能触发递归 page fault 导致死锁。
 * 因此分页 IO 必须直接放行，不做任何处理。
 */
static FLT_PREOP_CALLBACK_STATUS
PreRead(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    if (Data->Iopb->IrpFlags & IRP_PAGING_IO)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    KgCheckFileAccess(Data, KgOpRead);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：文件信息查询前置回调（IRP_MJ_QUERY_INFORMATION）
//       对沙箱内进程，检查目标路径的权限等级。
//       L0 路径返回 STATUS_OBJECT_NAME_NOT_FOUND，使 Test-Path 等探测
//       工具无法区分"路径不存在"和"路径存在但无权访问"，防止信息泄露。
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK 或 FLT_PREOP_COMPLETE（拒绝时）
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreQueryInformation(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    KG_SYSTEM_STATE* state = KgAcquireState();
    if (!state) return FLT_PREOP_SUCCESS_NO_CALLBACK;

    HANDLE pid = PsGetCurrentProcessId();
    ULONG slot = KgIsPidInSandBox(pid);

    // 非沙箱进程 → 直接放行，跳过路径解析和规则查找
    if (slot == (ULONG)-1) {
        KgReleaseState(state);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    PFLT_FILE_NAME_INFORMATION nameInfo;
    if (!KgNormalizePath(Data, &nameInfo)) {
        KgReleaseState(state);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    KG_POLICY_DOMAIN* domain = &state->Domain[slot];
    KIRQL dIrql;
    KeAcquireSpinLock(&domain->Lock, &dIrql);
    KG_SANDBOX_LEVEL level = KgFindPathRule(domain, &nameInfo->Name);
    KeReleaseSpinLock(&domain->Lock, dIrql);

    if (level == 0) {
        KG_POLICY_DOMAIN* global = &state->Domain[0];
        KeAcquireSpinLock(&global->Lock, &dIrql);
        level = KgFindPathRule(global, &nameInfo->Name);
        KeReleaseSpinLock(&global->Lock, dIrql);
    }

    // 祖先目录遍历放行：路径是绑定根目录的父目录 → 直接放行（与 PreCreate 一致）
    if (level == 0 && KgIsTraversalAllowed(state, slot, &nameInfo->Name)) {
        KgReleaseState(state);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    // L0 路径 → 返回"文件不存在"，与真正不存在的路径结果一致
    if (level == 0) {
        KG_LOG("FileGuard: DomainID=%lu PID=%lu FileName=%wZ | QueryInfo | L0 DENY\n",
                 state->Domain[slot].Id,
                 (ULONG)(ULONG_PTR)pid,
                 &nameInfo->Name);
        DbgPrint("FileGuard: DENY PID=%lu Level=0 Op=QueryInfo File=%wZ\n",
                 (ULONG)(ULONG_PTR)pid, &nameInfo->Name);
        KgPushDenyEvent(&state->Domain[slot], pid, &nameInfo->Name);
        KgReleaseState(state);
        FltReleaseFileNameInformation(nameInfo);
        Data->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;
    }

    KgReleaseState(state);
    FltReleaseFileNameInformation(nameInfo);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：安全属性设置前置回调（IRP_MJ_SET_SECURITY）
//       当前不处理，直接放行所有安全属性修改操作
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreSetSecurity(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    //UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    KgCheckFileAccess(Data, KgOpSetInfo);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：
//      缓冲区刷新前置回调 IRP_MJ_FLUSH_BUFFERS
// 参数：
//      Data              - 回调数据
//      FltObjects        - 相关对象
//      CompletionContext - 完成上下文
// 返回值：
//      FLT_PREOP_SUCCESS_NO_CALLBACK
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreFlushBuffers(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    //UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    KgCheckFileAccess(Data, KgOpWrite);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：
//      文件系统控制前置回调 IRP_MJ_FILE_SYSTEM_CONTROL
//      当前不处理，直接放行所有文件系统控制操作
// 参数：
//      Data              - 回调数据
//      FltObjects        - 相关对象
//      CompletionContext - 完成上下文
// 返回值：
//      FLT_PREOP_SUCCESS_NO_CALLBACK
////////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreFileSystemControl(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：目录枚举后置回调 — 对枚举结果进行可见性过滤
//       遍历文件系统返回的目录条目，对每个非 . / .. 条目拼接完整路径后
//       检查进程是否有权看到该路径，无权则从结果缓冲区中移除
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - Pre 回调保存的 KG_DIR_ENUM_CTX
//   Flags             - 后置操作标志
// 返回值：FLT_POSTOP_FINISHED_PROCESSING
////////////////////////////////////////////////////////////////////////////////
static FLT_POSTOP_CALLBACK_STATUS
KiloPostDirectoryControl(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID CompletionContext, FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);

    KG_DIR_ENUM_CTX* ctx = (KG_DIR_ENUM_CTX*)CompletionContext;

    if (!ctx) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (Data->Iopb->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        goto done;

    if (ctx->DirectoryPath.Length == 0) {
        goto done;
    }

    if (!NT_SUCCESS(Data->IoStatus.Status) || Data->IoStatus.Information == 0)
        goto done;

    FILE_INFORMATION_CLASS infoClass =
        Data->Iopb->Parameters.DirectoryControl.QueryDirectory.FileInformationClass;

    USHORT fnLenOffset, fnOffset;
    if (!KgLookupDirEntryOffsets(infoClass, &fnLenOffset, &fnOffset))
        goto done;

    PUCHAR buf = NULL;
    ULONG bufLen = (ULONG)Data->IoStatus.Information;
    PMDL mdl = Data->Iopb->Parameters.DirectoryControl.QueryDirectory.MdlAddress;
    if (mdl) {
        buf = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        if (!buf) goto done;
    } else {
        buf = (PUCHAR)Data->Iopb->Parameters.DirectoryControl.QueryDirectory.DirectoryBuffer;
        if (!buf || bufLen < sizeof(ULONG))
            goto done;
    }

    HANDLE childPid = PsGetProcessId(ctx->RequestorProcess);
    KG_SYSTEM_STATE* state = KgAcquireState();
    ULONG slotIndex = (ULONG)-1;
    if (state) {
        slotIndex = KgIsPidInSandBox(childPid);
        KgReleaseState(state);
    }

    PUCHAR readPtr = buf;
    PUCHAR writePtr = buf;
    PUCHAR prevEntry = NULL;
    ULONG remaining = bufLen;

    while (remaining >= sizeof(ULONG)) {
        ULONG nextOff = *(ULONG*)readPtr;
        ULONG thisEntrySize;

        if (nextOff == 0) {
            thisEntrySize = remaining;
        } else {
            ULONG baseSize = fnOffset;
            if (nextOff > remaining || nextOff < baseSize)
                break;
            if (readPtr + nextOff > buf + bufLen)
                break;
            thisEntrySize = nextOff;
        }

        if (!KgValidateDirEntry(readPtr, thisEntrySize, fnLenOffset))
            break;

        ULONG fnLen = *(ULONG*)(readPtr + fnLenOffset);
        PWCHAR fn = (PWCHAR)(readPtr + fnOffset);

        BOOLEAN keep = TRUE;

        if (fnLen == sizeof(WCHAR) && fn[0] == L'.') {
        } else if (fnLen == 2 * sizeof(WCHAR) && fn[0] == L'.' && fn[1] == L'.') {
        } else {
            UNICODE_STRING childPath;
            childPath.Length = (USHORT)(ctx->DirectoryPath.Length + sizeof(WCHAR) + fnLen);
            childPath.MaximumLength = (USHORT)(childPath.Length + sizeof(WCHAR));
            childPath.Buffer = (WCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, childPath.MaximumLength, KG_POOL_TAG);
            if (childPath.Buffer) {
                RtlCopyMemory(childPath.Buffer, ctx->DirectoryPath.Buffer, ctx->DirectoryPath.Length);
                childPath.Buffer[ctx->DirectoryPath.Length / sizeof(WCHAR)] = L'\\';
                RtlCopyMemory(childPath.Buffer + ctx->DirectoryPath.Length / sizeof(WCHAR) + 1, fn, fnLen);
                childPath.Buffer[childPath.Length / sizeof(WCHAR)] = L'\0';

                state = KgAcquireState();
                if (state) {
                    BOOLEAN travAllowed = KgIsTraversalAllowed(state, slotIndex, &childPath);
                    if (!travAllowed)
                        keep = KgCheckEnumVisibility(childPid, &childPath);
                    KgReleaseState(state);
                }
                ExFreePoolWithTag(childPath.Buffer, KG_POOL_TAG);
            }
        }

        if (keep) {
            if (writePtr != readPtr)
                RtlMoveMemory(writePtr, readPtr, thisEntrySize);
            if (prevEntry)
                *(ULONG*)prevEntry = (ULONG)(writePtr - prevEntry);
            prevEntry = writePtr;
            writePtr += thisEntrySize;
        }

        if (nextOff == 0)
            break;
        readPtr += nextOff;
        remaining -= nextOff;
    }

    if (prevEntry)
        *(ULONG*)prevEntry = 0;

    Data->IoStatus.Information = (ULONG)(writePtr - buf);

done:
    ExFreePoolWithTag(ctx, KG_POOL_TAG);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：驱动卸载例程
//       依次注销进程回调、清理受信 PID、关闭通信端口、
//       删除设备对象和符号链接、注销 minifilter、
//       等待所有 Rundown 引用释放后释放系统状态
// 参数：
//   Flags - 卸载标志（当前未使用）
// 返回值：STATUS_SUCCESS
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS
DriverUnload(FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    KgUnregisterNetCallbacks();
    KgUnregisterRegCallbacks();
    KgUnregisterProcessCallbacks();
    KgClearAllTrustedPids();
    // 通信端口清理（专为 minifilter 通信端口架构预留，当前 IOCTL 模式下 gServerPort 始终为 NULL）
    if (gServerPort) { FltCloseCommunicationPort(gServerPort); gServerPort = NULL; }
    if (gDeviceObject) {
        UNICODE_STRING symLink;
        RtlInitUnicodeString(&symLink, L"\\DosDevices\\KiloGuard");
        IoDeleteSymbolicLink(&symLink);
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
    }
    if (gFilter) FltUnregisterFilter(gFilter);

    KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)InterlockedExchangePointer((PVOID*)&gState, NULL);
    if (state) {
        ExWaitForRundownProtectionRelease(&state->RundownRef);
        KgFreeState(state);
    }
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// 功能：文件安全属性查询前置回调（IRP_MJ_QUERY_SECURITY）
//       当进程查询文件的安全描述符（所有者、组、DACL、SACL）时触发
//       阻止沙箱进程通过 GetFileSecurity 探测 L0 目录文件的存在性
// 参数：
//   Data              - 回调数据
//   FltObjects        - 相关对象
//   CompletionContext - 完成上下文
// 返回值：FLT_PREOP_SUCCESS_NO_CALLBACK
///////////////////////////////////////////////////////////////////////////////
static FLT_PREOP_CALLBACK_STATUS
PreQuerySecurity(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    KgCheckFileAccess(Data, KgOpRead);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 2.IRP 回调注册 (FLT_OPERATION_REGISTRATION)
//   IRP 主功能号的 Pre/Post 回调
//
//  IRP_MJ_DIRECTORY_CONTROL 需要关联 Pre + Post 两个，表示目录枚举，Pre 时还没返回数据
//          此时只有只有上下文，进程和目录路径，Post 阶段目录条目已经写入缓冲区
//  IRP_MJ_CREATE: 创建、打开或覆盖文件/目录
//      触发时机：CreateFile、ZwCreateFile、NtCreateFile 等 API 调用时
//  IRP_MJ_READ: 从文件中读取数据
//      触发时机：ReadFile、ZwReadFile、内存管理器分页 IO（page fault 时从磁盘读回代码/数据页）
//      注意：分页 IO 路径（IRP_PAGING_IO）直接放行，不做任何处理
//  IRP_MJ_WRITE: 向文件写入数据
//      触发时机：WriteFile、ZwWriteFile、内存管理器脏页回写
//      注意：分页 IO 路径直接放行
//  IRP_MJ_SET_INFORMATION: 修改文件的元数据或状态
//      触发时机：SetFileInformationByHandle、ZwSetInformationFile 等
//      可通过 FileInformationClass 细分：
//          FileDispositionInformation / FileDispositionInformationEx → 标记删除（句柄关闭时真正删除）
//          FileRenameInformation / FileRenameInformationEx → 重命名
//          FileBasicInformation / FileEndOfFileInformation / FileValidDataLengthInformation 等 → 修改属性/大小/时间
//  IRP_MJ_CLEANUP: 文件对象句柄关闭时的清理操作
//      触发时机：CloseHandle 关闭文件句柄时（在句柄真正释放之前）
//      特殊用途：检查 FO_DELETE_ON_CLOSE 标志，在权限不足时清除该标志阻止文件被删除
//  IRP_MJ_QUERY_INFORMATION: 查询文件的元数据（大小、属性、时间戳等）
//      触发时机：GetFileAttributesEx、GetFileInformationByHandle、FindFirstFile、ZwQueryInformationFile 等
//  IRP_MJ_SET_SECURITY: 修改文件/目录的安全描述符（ACL、所有者等）
//      触发时机：SetFileSecurity、ZwSetSecurityObject 等
//  IRP_MJ_FLUSH_BUFFERS: 将文件缓冲区中的数据刷新到磁盘
//      触发时机：FlushFileBuffers、ZwFlushBuffersFile 等
//  IRP_MJ_FILE_SYSTEM_CONTROL: 文件系统控制操作（卷挂载/卸载、锁卷等）
//      触发时机：FSCTL_* 控制码，如卷挂载通知、卷锁定等
//  IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION: 内存管理器在创建文件映射（section）时获取同步锁
//      触发时机：CreateFileMapping、LoadLibrary（PE 映像映射）、CreateProcess（EXE 映像映射）
//          CreateProcess 的执行过程中，需要把 EXE 文件和依赖的 DLL 映射到进程的地址空间。
//          这个映射操作通过内存管理器的 CreateSection（创建文件映射）完成，而每次创建 section 时，
//          minifilter 都会收到 IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION 回调。
//      关键限制：此回调在内存管理器的 section 创建锁下运行，不可执行任何路径解析、内存分配、获取锁等操作，
//          否则会导致 CreateProcess 死锁
///////////////////////////////////////////////////////////////////////////////
static const FLT_OPERATION_REGISTRATION Callbacks[] = {
    { IRP_MJ_CREATE,                              0, PreCreate,                NULL },
    { IRP_MJ_READ,                                0, PreRead,                  NULL },
    { IRP_MJ_WRITE,                               0, PreWrite,                 NULL },
    { IRP_MJ_SET_INFORMATION,                     0, PreSetInfo,               NULL },
    { IRP_MJ_CLEANUP,                             0, PreCleanup,               NULL },
    { IRP_MJ_QUERY_INFORMATION,                   0, PreQueryInformation,      NULL },
    { IRP_MJ_SET_SECURITY,                        0, PreSetSecurity,           NULL },
    { IRP_MJ_FLUSH_BUFFERS,                       0, PreFlushBuffers,          NULL },
    { IRP_MJ_FILE_SYSTEM_CONTROL,                 0, PreFileSystemControl,     NULL },
    { IRP_MJ_DIRECTORY_CONTROL,                   0, PreDirCtrl,               KiloPostDirectoryControl },
    { IRP_MJ_QUERY_SECURITY,                      0, PreQuerySecurity,         NULL },
    { IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION, 0, PreAcquireForSectionSync, NULL },
    { IRP_MJ_OPERATION_END }
};

////////////////////////////////////////////////////////////////////////////////
// Minifilter 注册结构
// 向 Filter Manager 注册驱动的实例回调、IRP 回调、上下文类型、卸载函数
// 前置概念：
//      什么是 minifilter 实例？Windows 的每个卷（C:、D: 等）都有一个文件系统栈。
//      minifilter 驱动通过"实例"附加到这个栈上。一个 minifilter 可以同时附加到多个卷，每个卷对应一个实例。
////////////////////////////////////////////////////////////////////////////////
static const FLT_REGISTRATION FilterRegistration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    ContextRegistration, // 专为通信端口架构预留，此字段暂为占位
    Callbacks,
    DriverUnload,
    KgInstanceSetup, // 卷挂载时，Filter Manager 通知 minifilter 附加到卷
    KgInstanceQueryTeardown, // 查询是否可以卸载实例
    KgInstanceTeardownStart, // 实例拆卸开始
    KgInstanceTeardownComplete, // 实例拆卸完成
    NULL, NULL, NULL, NULL, NULL, NULL
};

////////////////////////////////////////////////////////////////////////////////
// 功能：驱动入口点（DriverEntry）
//       初始化全局数据结构（PID 映射表、自旋锁、受信白名单、追踪器），
//       分配系统状态，向 Filter Manager 注册 minifilter，
//       创建通信端口（预留），启动过滤，创建 IOCTL 设备对象和符号链接，
//       注册进程/线程/映像加载回调
// 参数：
//   DriverObject  - 驱动对象（设置 MajorFunction 分发例程）
//   RegistryPath  - 注册表路径（当前未使用）
// 返回值：STATUS_SUCCESS 成功；STATUS_INSUFFICIENT_RESOURCES 内存不足；
//         其他错误码表示注册或设备创建失败
////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
// 一个完整有用的 minifilter 驱动必须包含：
// 1. 注册与生命周期
//      FltRegisterFilter — 向 Filter Manager 注册
//      FltStartFiltering — 开始拦截 IO
//      FltUnregisterFilter — 卸载时注销
///////////////////////////////////////////////////////////////////////////////
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    gPidMapEpoch = 0;
    KgPidMapInit();
    KeInitializeSpinLock(&gStateLock);
    KgInitTrustedPids();
    KgInitTracker();

    gState = KgAllocState();
    if (!gState) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = FltRegisterFilter(DriverObject, &FilterRegistration, &gFilter);
    if (!NT_SUCCESS(status)) { KgFreeState(gState); gState = NULL; return status; }

    UNICODE_STRING portName;
    RtlInitUnicodeString(&portName, L"\\KiloGuardPort");

    // Everyone SID: S-1-1-0 (revision=1, 1 subauth, WORLD authority, RID=0)
    UCHAR everyoneSid[] = { 0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00 };

    // Build custom SD: Everyone FLT_PORT_ALL_ACCESS
    SECURITY_DESCRIPTOR sd;
    RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);

    UCHAR aclBuf[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + 12];
    PACL acl = (PACL)aclBuf;
    RtlCreateAcl(acl, sizeof(aclBuf), ACL_REVISION);
    RtlAddAccessAllowedAce(acl, ACL_REVISION, FLT_PORT_ALL_ACCESS, (PSID)everyoneSid);
    RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &portName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, &sd);

    status = FltCreateCommunicationPort(gFilter, &gServerPort, &oa, NULL,
                                        KgConnectNotify, KgDisconnectNotify,
                                        KgMessageNotify, 64);
    if (!NT_SUCCESS(status)) {
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
        KgFreeState(gState); gState = NULL;
        return status;
    }

    status = FltStartFiltering(gFilter);
    if (!NT_SUCCESS(status)) {
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
        KgFreeState(gState); gState = NULL;
        return status;
    }

    KgRegisterProcessCallbacks();
    KgRegisterRegCallbacks();

    UNICODE_STRING devName;
    RtlInitUnicodeString(&devName, L"\\Device\\KiloGuard");
    {
        UNICODE_STRING sddl;
        RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;WD)");
        status = IoCreateDeviceSecure(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE,
                                      &sddl, NULL, &gDeviceObject);
    }
    if (!NT_SUCCESS(status)) {
        KgUnregisterProcessCallbacks();
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
        KgFreeState(gState); gState = NULL;
        return status;
    }

    status = KgRegisterNetCallbacks(gDeviceObject);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
        KgUnregisterProcessCallbacks();
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
        KgFreeState(gState); gState = NULL;
        return status;
    }

    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, L"\\DosDevices\\KiloGuard");
    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status)) {
        KgUnregisterNetCallbacks();
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
        KgUnregisterProcessCallbacks();
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
        FltUnregisterFilter(gFilter);
        gFilter = NULL;
        KgFreeState(gState); gState = NULL;
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = KgCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = KgCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KgDeviceIoControl;
    DriverObject->DriverUnload = NULL;

    return STATUS_SUCCESS;
}
