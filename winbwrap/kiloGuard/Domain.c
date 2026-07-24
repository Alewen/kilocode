
#include "Domain.h"
#include "Pidmap.h"

NTSYSAPI PCSTR NTAPI PsGetProcessImageFileName(PEPROCESS Process);

KG_SYSTEM_STATE* gState = NULL;
KSPIN_LOCK gStateLock;
KG_SANDBOX_ID gNextSid = 1;

KG_PENDING_TRACKER gTracker[KG_TRACKER_SIZE];
KSPIN_LOCK gTrackerLock;

////////////////////////////////////////////////////////////////////////////////
// 功能：根据沙箱域 ID 查找对应槽位索引
//       先用哈希探测（id % KG_MAX_DOMAIN），若失败则线性扫描全表
//       Domain[0]（默认域, id=0）强制返回槽位 0
// 参数：
//   state - 系统状态（含 Domain[1024] 数组）
//   id    - 目标沙箱域 ID
// 返回值：域槽位索引（0~1023）；未找到返回 (ULONG)-1
////////////////////////////////////////////////////////////////////////////////
ULONG KgFindDomainSlot(KG_SYSTEM_STATE* state, KG_SANDBOX_ID id)
{
    if (id == 0) return 0;
    ULONG slot = id % KG_MAX_DOMAIN;
    if (state->Domain[slot].Active && state->Domain[slot].Id == id)
        return slot;
    for (ULONG i = 1; i < KG_MAX_DOMAIN; i++) {
        if (state->Domain[i].Active && state->Domain[i].Id == id)
            return i;
    }
    return (ULONG)-1;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：向沙箱域添加一条路径规则
//       分配并初始化 KG_PATH_RULE，深拷贝路径前缀字符串，
//       将规则插入域规则链表尾部
// 参数：
//   domain     - 目标沙箱域
//   pathPrefix - 路径前缀（UNICODE_STRING，需含结尾 NUL 空间）
//   level      - 沙箱级别
// 返回值：STATUS_SUCCESS 成功；
//         STATUS_TOO_MANY_COMMANDS 规则数已达 256 上限；
//         STATUS_INSUFFICIENT_RESOURCES 内存分配失败
////////////////////////////////////////////////////////////////////////////////
NTSTATUS KgAddPathRule(KG_POLICY_DOMAIN* domain, PUNICODE_STRING pathPrefix, KG_SANDBOX_LEVEL level)
{
    if (domain->RuleCount >= KG_MAX_RULES_PER_DOMAIN) return STATUS_TOO_MANY_COMMANDS;
    KG_PATH_RULE* rule = (KG_PATH_RULE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_PATH_RULE), KG_POOL_TAG);
    if (!rule) return STATUS_INSUFFICIENT_RESOURCES;
    rule->PathPrefix.Buffer = (WCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, pathPrefix->Length + sizeof(WCHAR), KG_POOL_TAG);
    if (!rule->PathPrefix.Buffer) { ExFreePoolWithTag(rule, KG_POOL_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlCopyMemory(rule->PathPrefix.Buffer, pathPrefix->Buffer, pathPrefix->Length + sizeof(WCHAR));
    rule->PathPrefix.Length = pathPrefix->Length;
    rule->PathPrefix.MaximumLength = pathPrefix->Length + sizeof(WCHAR);
    rule->Level = level;
    InsertTailList(&domain->RuleList, &rule->Link);
    domain->RuleCount++;
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：在域的规则链表中执行最长前缀匹配
//       遍历所有规则，选择与 path 前缀匹配且前缀长度最长的规则，
//       返回其 Level 值。无匹配时返回 0（禁止访问）。
// 参数：
//   domain - 目标沙箱域
//   path   - 待匹配的文件完整路径（UNICODE_STRING）
// 返回值：匹配规则的 Level（0/1/2）；无匹配返回 0
////////////////////////////////////////////////////////////////////////////////
KG_SANDBOX_LEVEL KgFindPathRule(KG_POLICY_DOMAIN* domain, PUNICODE_STRING path)
{
    KG_SANDBOX_LEVEL level = 0;
    USHORT bestLen = 0;

    for (PLIST_ENTRY e = domain->RuleList.Flink; e != &domain->RuleList; e = e->Flink)
    {
        KG_PATH_RULE* rule = CONTAINING_RECORD(e, KG_PATH_RULE, Link);
        if (RtlPrefixUnicodeString(&rule->PathPrefix, path, TRUE) && rule->PathPrefix.Length > bestLen)
        {
            if (rule->PathPrefix.Length < path->Length &&
                path->Buffer[rule->PathPrefix.Length / sizeof(WCHAR)] != L'\\')
                continue;
            level = rule->Level;
            bestLen = rule->PathPrefix.Length;
        }
    }

    return level;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：释放沙箱域中所有路径规则
//       遍历规则链表释放每条规则的 PathPrefix 缓冲区和规则结构体，
//       最后将规则计数清零
// 参数：
//   domain - 目标沙箱域
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreePathRules(KG_POLICY_DOMAIN* domain)
{
    while (!IsListEmpty(&domain->RuleList)) {
        PLIST_ENTRY e = RemoveHeadList(&domain->RuleList);
        KG_PATH_RULE* rule = CONTAINING_RECORD(e, KG_PATH_RULE, Link);
        if (rule->PathPrefix.Buffer) ExFreePoolWithTag(rule->PathPrefix.Buffer, KG_POOL_TAG);
        ExFreePoolWithTag(rule, KG_POOL_TAG);
    }
    domain->RuleCount = 0;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：分配并初始化系统全局状态
//       分配非分页内存，初始化 Rundown 保护，
//       创建 PID 映射表，初始化 Domain[0]（默认域，Id=0 Active=TRUE），
//       开启所有全局策略开关（文件访问、句柄保护、进程继承）
// 参数：无
// 返回值：新分配的状态指针；失败返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_SYSTEM_STATE* KgAllocState(VOID)
{
    KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_SYSTEM_STATE), KG_POOL_TAG);
    if (!state) return NULL;
    RtlZeroMemory(state, sizeof(KG_SYSTEM_STATE));
    ExInitializeRundownProtection(&state->RundownRef);
    state->PidMap = KgAllocPidMap();
    if (!state->PidMap) { ExFreePoolWithTag(state, KG_POOL_TAG); return NULL; }
    state->Domain[0].Id = 0;
    state->Domain[0].Active = TRUE;
    KeInitializeSpinLock(&state->Domain[0].Lock);
    InitializeListHead(&state->Domain[0].RuleList);
    state->Domain[0].RuleCount = 0;
    state->Domain[0].RuleEpoch = 0;
    state->Domain[0].NetBlockEnabled = FALSE;
    state->Domain[0].NetExeCount = 0;
    state->Domain[0].DenyLogEnabled = FALSE;
    KeInitializeSpinLock(&state->Domain[0].EventRing.Lock);
    state->Domain[0].EventRing.WriteIndex = 0;
    state->Domain[0].EventRing.ReadIndex = 0;
    KeInitializeSpinLock(&state->Domain[0].DenyRing.Lock);
    state->Domain[0].DenyRing.WriteIndex = 0;
    state->Domain[0].DenyRing.ReadIndex = 0;
    state->Policy.FileAccessEnabled = TRUE;
    state->Policy.HandleProtectionEnabled = TRUE;
    state->Policy.ProcessInheritanceEnabled = TRUE;
    state->DroppedRuleCount = 0;
    return state;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：释放系统状态结构体
//       先释放 PID 映射表，再遍历所有激活域（含 Domain[0]）释放路径规则，
//       最后释放状态结构体本身
// 参数：
//   state - 待释放的系统状态；允许 NULL
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreeState(KG_SYSTEM_STATE* state)
{
    if (!state) return;
    if (state->PidMap) KgFreePidMap(state->PidMap);
    for (ULONG i = 0; i < KG_MAX_DOMAIN; i++) {
        if (state->Domain[i].Active || i == 0) {
            KgFreePathRules(&state->Domain[i]);
        }
    }
    ExFreePoolWithTag(state, KG_POOL_TAG);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：安全获取全局系统状态指针，并获取 Rundown 保护
//       通过 InterlockedCompareExchangePointer 无锁读取 gState，
//       获取成功后调用 ExAcquireRundownProtection 防止并发卸载释放
// 参数：无
// 返回值：系统状态指针（已获取 Rundown 保护）；
//         若驱动正在卸载返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_SYSTEM_STATE* KgAcquireState(VOID)
{
    KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)InterlockedCompareExchangePointer((PVOID*)&gState, NULL, NULL);
    if (!state) return NULL;
    if (!ExAcquireRundownProtection(&state->RundownRef)) return NULL;
    return state;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：释放通过 KgAcquireState 获取的 Rundown 保护引用
//       与 KgAcquireState 成对使用，通常在 IOCTL 处理结束时调用
// 参数：
//   state - 系统状态指针（不可为 NULL）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgReleaseState(KG_SYSTEM_STATE* state)
{
    ExReleaseRundownProtection(&state->RundownRef);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：初始化进程追踪器数组
//       初始化保护追踪器的自旋锁，清零 256 个追踪器槽位
// 参数：无
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgInitTracker(VOID)
{
    KeInitializeSpinLock(&gTrackerLock);
    RtlZeroMemory(gTracker, sizeof(gTracker));
}

////////////////////////////////////////////////////////////////////////////////
// 功能：根据 PID 查找已分配的进程追踪器
//       线性扫描追踪器数组，匹配 Used=TRUE 且 PID 相同的槽位
// 参数：
//   pid - 目标进程 PID
// 返回值：追踪器指针；未找到返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_PENDING_TRACKER* KgFindTracker(HANDLE pid)
{
    for (ULONG i = 0; i < KG_TRACKER_SIZE; i++) {
        if (gTracker[i].Used && gTracker[i].Pid == pid)
            return &gTracker[i];
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：为指定进程分配一个追踪器槽位，记录映像加载信息
//       线性扫描追踪器数组寻找第一个空闲槽位
//       在进程创建通知（CreateInfo 非空）或 IOCTL_ATTACH_PROCESS 时调用
// 参数：
//   pid       - 目标进程 PID
//   slotIndex - 沙箱域槽位索引
//   imageName - 进程映像路径（可选，允许 NULL）
// 返回值：追踪器指针；追踪器数组已满返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_PENDING_TRACKER* KgAllocTracker(HANDLE pid, ULONG slotIndex, PUNICODE_STRING imageName)
{
    for (ULONG i = 0; i < KG_TRACKER_SIZE; i++) {
        if (!gTracker[i].Used) {
            gTracker[i].Used = TRUE;
            gTracker[i].Pid = pid;
            gTracker[i].SlotIndex = slotIndex;
            gTracker[i].ImageCount = 0;
            KeQuerySystemTime(&gTracker[i].CreationTime);
            InitializeListHead(&gTracker[i].DllList);
            gTracker[i].DllCount = 0;
            if (imageName && imageName->Buffer) {
                wcsncpy_s(gTracker[i].ImageName, 256, imageName->Buffer, _TRUNCATE);
            } else {
                gTracker[i].ImageName[0] = L'\0';
            }
            return &gTracker[i];
        }
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：释放指定 PID 的追踪器
//       释放其关联的 DLL 链表中的所有条目内存，标记槽位为空闲
// 参数：
//   pid - 目标进程 PID
// 返回值：无（未找到时静默返回）
////////////////////////////////////////////////////////////////////////////////
VOID KgFreeTracker(HANDLE pid)
{
    for (ULONG i = 0; i < KG_TRACKER_SIZE; i++) {
        if (gTracker[i].Used && gTracker[i].Pid == pid) {
            while (!IsListEmpty(&gTracker[i].DllList)) {
                PLIST_ENTRY e = RemoveHeadList(&gTracker[i].DllList);
                KG_DLL_ENTRY* dllEntry = CONTAINING_RECORD(e, KG_DLL_ENTRY, Link);
                ExFreePoolWithTag(dllEntry, KG_POOL_TAG);
            }
            gTracker[i].Used = FALSE;
            return;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// 功能：向沙箱域的事件环形缓冲区推入一条进程启动失败事件
//       环形缓冲区满时自动覆盖最旧事件（写索引追平读索引时读索引前移）
//       在 KgAnalyzeDllFailure 中识别到缺失 DLL 时调用
// 参数：
//   domain     - 目标沙箱域
//   tracker    - 进程追踪器（提供 PID、映像名、创建时间）
//   missingDll - 缺失导致失败的 DLL 名称（可选，允许 NULL）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgPushEvent(KG_POLICY_DOMAIN* domain, KG_PENDING_TRACKER* tracker, PCUNICODE_STRING missingDll)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&domain->EventRing.Lock, &oldIrql);

    LONG writeIdx = domain->EventRing.WriteIndex;
    KG_FAILURE_EVENT* evt = &domain->EventRing.Events[writeIdx];
    evt->Pid = (ULONG)(ULONG_PTR)tracker->Pid;
    wcsncpy_s(evt->ImageName, 256, tracker->ImageName, _TRUNCATE);
    if (missingDll && missingDll->Buffer) {
        wcsncpy_s(evt->DllPath, 256, missingDll->Buffer, _TRUNCATE);
    } else {
        evt->DllPath[0] = L'\0';
    }
    evt->Timestamp = tracker->CreationTime;

    domain->EventRing.WriteIndex = (writeIdx + 1) % KG_EVENT_RING_SIZE;

    if (domain->EventRing.WriteIndex == domain->EventRing.ReadIndex) {
        domain->EventRing.ReadIndex = (domain->EventRing.ReadIndex + 1) % KG_EVENT_RING_SIZE;
    }

    KeReleaseSpinLock(&domain->EventRing.Lock, oldIrql);
}

BOOLEAN KgIsAncestorOfBindRoot(KG_POLICY_DOMAIN* domain, PUNICODE_STRING path)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&domain->Lock, &oldIrql);
    for (PLIST_ENTRY e = domain->RuleList.Flink; e != &domain->RuleList; e = e->Flink) {
        KG_PATH_RULE* rule = CONTAINING_RECORD(e, KG_PATH_RULE, Link);
        if (RtlPrefixUnicodeString(path, &rule->PathPrefix, TRUE) && path->Length < rule->PathPrefix.Length) {
            KeReleaseSpinLock(&domain->Lock, oldIrql);
            return TRUE;
        }
    }
    KeReleaseSpinLock(&domain->Lock, oldIrql);
    return FALSE;
}

BOOLEAN KgIsTraversalAllowed(KG_SYSTEM_STATE* state, ULONG slotIndex, PUNICODE_STRING path)
{
    if (slotIndex != (ULONG)-1 && state->Domain[slotIndex].Active) {
        if (KgIsAncestorOfBindRoot(&state->Domain[slotIndex], path))
            return TRUE;
    }
    if (state->Domain[0].Active) {
        if (KgIsAncestorOfBindRoot(&state->Domain[0], path))
            return TRUE;
    }
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// 记录文件访问被拒绝时信息到所属域的 deny 环
////////////////////////////////////////////////////////////////////////////////

VOID KgPushDenyEvent(KG_POLICY_DOMAIN* domain, HANDLE pid, PUNICODE_STRING filePath)
{
    if (!domain->DenyLogEnabled) return;
    KIRQL oldIrql;
    KeAcquireSpinLock(&domain->DenyRing.Lock, &oldIrql);

    LONG writeIdx = domain->DenyRing.WriteIndex;
    KG_DENY_EVENT* evt = &domain->DenyRing.Events[writeIdx];

    evt->Pid = (ULONG)(ULONG_PTR)pid;
    evt->Flags = 0;
    KeQuerySystemTime(&evt->Timestamp);

    PEPROCESS proc = PsGetCurrentProcess();
    PCSTR name = PsGetProcessImageFileName(proc);
    ULONG i = 0;
    if (name) {
        while (name[i] && i < 31) {
            evt->ProcessName[i] = (WCHAR)name[i];
            i++;
        }
    }
    evt->ProcessName[i] = L'\0';

    if (filePath && filePath->Buffer) {
        wcsncpy_s(evt->FilePath, 260, filePath->Buffer, _TRUNCATE);
    } else {
        evt->FilePath[0] = L'\0';
    }

    domain->DenyRing.WriteIndex = (writeIdx + 1) % KG_DENY_RING_SIZE;

    if (domain->DenyRing.WriteIndex == domain->DenyRing.ReadIndex) {
        domain->DenyRing.ReadIndex = (domain->DenyRing.ReadIndex + 1) % KG_DENY_RING_SIZE;
    }

    KeReleaseSpinLock(&domain->DenyRing.Lock, oldIrql);
}

BOOLEAN KgIsNetExeInList(KG_POLICY_DOMAIN* domain, PUNICODE_STRING imagePath)
{
    if (!domain || !imagePath || !imagePath->Buffer || imagePath->Length == 0)
        return FALSE;

    for (ULONG i = 0; i < domain->NetExeCount; i++) {
        UNICODE_STRING entry;
        RtlInitUnicodeString(&entry, domain->NetExeList[i]);
        if (RtlCompareUnicodeString(imagePath, &entry, TRUE) == 0)
            return TRUE;
    }
    return FALSE;
}

VOID KgPushNetDenyEvent(ULONG slotIndex, HANDLE pid, PCUNICODE_STRING infoStr)
{
    KG_SYSTEM_STATE* s = KgAcquireState();
    if (!s) return;

    if (slotIndex >= KG_MAX_DOMAIN || !s->Domain[slotIndex].Active || !s->Domain[slotIndex].DenyLogEnabled)
    {
        KgReleaseState(s);
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&s->Domain[slotIndex].DenyRing.Lock, &oldIrql);

    LONG writeIdx = s->Domain[slotIndex].DenyRing.WriteIndex;
    KG_DENY_EVENT* evt = &s->Domain[slotIndex].DenyRing.Events[writeIdx];
    evt->Pid = (ULONG)(ULONG_PTR)pid;
    evt->Flags = 1;
    KeQuerySystemTime(&evt->Timestamp);

    /* 获取当前进程名（与文件拒绝事件一致） */
    {
        PEPROCESS proc = PsGetCurrentProcess();
        PCSTR name = PsGetProcessImageFileName(proc);
        ULONG i = 0;
        if (name) {
            while (name[i] && i < 31) {
                evt->ProcessName[i] = (WCHAR)name[i];
                i++;
            }
        }
        evt->ProcessName[i] = L'\0';
    }

    if (infoStr && infoStr->Buffer) {
        ULONG copyChars = infoStr->Length / sizeof(WCHAR);
        if (copyChars > 259) copyChars = 259;
        RtlCopyMemory(evt->FilePath, infoStr->Buffer, copyChars * sizeof(WCHAR));
        evt->FilePath[copyChars] = L'\0';
    } else {
        evt->FilePath[0] = L'\0';
    }

    s->Domain[slotIndex].DenyRing.WriteIndex = (writeIdx + 1) % KG_DENY_RING_SIZE;
    if (s->Domain[slotIndex].DenyRing.WriteIndex == s->Domain[slotIndex].DenyRing.ReadIndex)
        s->Domain[slotIndex].DenyRing.ReadIndex = (s->Domain[slotIndex].DenyRing.ReadIndex + 1) % KG_DENY_RING_SIZE;

    KeReleaseSpinLock(&s->Domain[slotIndex].DenyRing.Lock, oldIrql);
    KgReleaseState(s);
}
