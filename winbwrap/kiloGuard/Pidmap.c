
#include "KiloGuard.h"
#include "Pidmap.h"
#include "Domain.h"
#include "PidPortMap.h"

USHORT gPidSlotFast[65536];
UCHAR gBwrapFast[65536];
volatile LONG gPidMapEpoch = 0;

////////////////////////////////////////////////////////////////////////////////
// 功能：对 PID 进行哈希，计算在映射表中的桶索引
//       将 PID 右移 4 位后对 256 取模，以分散不同 PID 到不同桶
// 参数：
//   pid - 进程 PID
// 返回值：哈希桶索引（0~255）
////////////////////////////////////////////////////////////////////////////////
static ULONG KgHashPid(HANDLE pid)
{
    return (ULONG)((ULONG_PTR)pid >> 4) % KG_PID_BUCKETS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：分配并初始化 PID 映射表
//       初始化 Rundown 保护，初始化所有 256 个哈希桶链表头
// 参数：无
// 返回值：新分配的 PID 映射表指针；失败返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_PID_MAP* KgAllocPidMap(VOID)
{
    KG_PID_MAP* map = (KG_PID_MAP*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_PID_MAP), KG_POOL_TAG);
    if (!map) return NULL;
    ExInitializeRundownProtection(&map->RundownRef);
    for (ULONG i = 0; i < KG_PID_BUCKETS; i++) InitializeListHead(&map->Buckets[i]);
    return map;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：查找指定进程在 PID 映射表中的沙箱域槽位
//       通过哈希定位桶后遍历链表精确匹配 PID
// 参数：
//   map - PID 映射表
//   pid - 目标进程 PID
// 返回值：沙箱域槽位索引；未找到返回 (ULONG)-1
////////////////////////////////////////////////////////////////////////////////
// 查询进程 PID 是否在沙箱内
ULONG KgLookupSlotByPid(KG_PID_MAP* map, HANDLE pid)
{
    ULONG idx = KgHashPid(pid);
    for (PLIST_ENTRY e = map->Buckets[idx].Flink; e != &map->Buckets[idx]; e = e->Flink) {
        KG_PID_ENTRY* entry = CONTAINING_RECORD(e, KG_PID_ENTRY, Link);
        if (entry->Pid == pid) return entry->SlotIndex;
    }
    return (ULONG)-1;
}

////////////////////////////////////////////////////////////////////////////////
// 功能: 通过快速缓存判断进程是否在沙箱内
//      使用 gPidSlotFast 位号索引直接查表，O(1) 时间复杂度
//      缓存值为 slotIndex+1，0xFFFF 表示不在沙箱内
// 参数：
//      pid - 目标进程 PID
// 返回值：
//      沙箱域槽位索引，取值范围 0 - 1023
//      不在沙箱内返回 (ULONG)-1
////////////////////////////////////////////////////////////////////////////////
ULONG KgIsPidInSandBox(HANDLE pid)
{
    USHORT fastSlot = gPidSlotFast[(ULONG)(ULONG_PTR)pid & 0xFFFF];
    if (fastSlot == KG_PID_SLOT_EMPTY)
        return (ULONG)-1;
    return (ULONG)(fastSlot);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：设置/更新指定进程的沙箱域槽位
//       若 PID 已存在则更新槽位；否则新建条目插入哈希桶
//       每次操作同步更新快速缓存 gPidSlotFast
// 参数：
//   map       - PID 映射表
//   pid       - 目标进程 PID
//   slotIndex - 沙箱域槽位索引
// 返回值：STATUS_SUCCESS 成功；STATUS_INSUFFICIENT_RESOURCES 内存不足
////////////////////////////////////////////////////////////////////////////////
NTSTATUS KgSetPidSlot(KG_PID_MAP* map, HANDLE pid, ULONG slotIndex)
{
    ULONG idx = KgHashPid(pid);
    for (PLIST_ENTRY e = map->Buckets[idx].Flink; e != &map->Buckets[idx]; e = e->Flink) {
        KG_PID_ENTRY* entry = CONTAINING_RECORD(e, KG_PID_ENTRY, Link);
        if (entry->Pid == pid) {
            entry->SlotIndex = slotIndex;
            gPidSlotFast[(ULONG)(ULONG_PTR)pid & 0xFFFF] = (USHORT)(slotIndex);
            return STATUS_SUCCESS;
        }
    }
    KG_PID_ENTRY* entry = (KG_PID_ENTRY*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_PID_ENTRY), KG_POOL_TAG);
    if (!entry) return STATUS_INSUFFICIENT_RESOURCES;
    entry->Pid = pid;
    entry->SlotIndex = slotIndex;
    InsertTailList(&map->Buckets[idx], &entry->Link);
    gPidSlotFast[(ULONG)(ULONG_PTR)pid & 0xFFFF] = (USHORT)(slotIndex);
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：从 PID 映射表中移除指定进程条目
//       同时清除快速缓存中的对应位号，释放 KG_PID_ENTRY 内存
// 参数：
//   map - PID 映射表
//   pid - 要移除的进程 PID
// 返回值：无（未找到时静默返回）
////////////////////////////////////////////////////////////////////////////////
VOID KgRemovePid(KG_PID_MAP* map, HANDLE pid)
{
    ULONG idx = KgHashPid(pid);
    for (PLIST_ENTRY e = map->Buckets[idx].Flink; e != &map->Buckets[idx]; e = e->Flink) {
        KG_PID_ENTRY* entry = CONTAINING_RECORD(e, KG_PID_ENTRY, Link);
        if (entry->Pid == pid) {
            RemoveEntryList(&entry->Link);
            ExFreePoolWithTag(entry, KG_POOL_TAG);
            gPidSlotFast[(ULONG)(ULONG_PTR)pid & 0xFFFF] = KG_PID_SLOT_EMPTY;
            return;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// 功能：释放整个 PID 映射表
//       遍历 256 个哈希桶，逐条弹出链表节点并释放条目内存，
//       最后释放映射表本身
// 参数：
//   map - 待释放的映射表；允许 NULL
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreePidMap(KG_PID_MAP* map)
{
    if (!map) return;
    for (ULONG i = 0; i < KG_PID_BUCKETS; i++) {
        while (!IsListEmpty(&map->Buckets[i])) {
            PLIST_ENTRY e = RemoveHeadList(&map->Buckets[i]);
            ExFreePoolWithTag(CONTAINING_RECORD(e, KG_PID_ENTRY, Link), KG_POOL_TAG);
        }
    }
    ExFreePoolWithTag(map, KG_POOL_TAG);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：克隆整个 PID 映射表
//       遍历源表的所有哈希桶，逐条复制条目到新表
//       中间分配失败时回滚已分配内存并返回 NULL
// 参数：
//   src - 源 PID 映射表；允许 NULL（此时返回空表）
// 返回值：新分配的 PID 映射表指针；失败返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_PID_MAP* KgClonePidMap(KG_PID_MAP* src)
{
    KG_PID_MAP* dst = KgAllocPidMap();
    if (!dst) return NULL;
    if (!src) return dst;
    for (ULONG i = 0; i < KG_PID_BUCKETS; i++) {
        for (PLIST_ENTRY e = src->Buckets[i].Flink; e != &src->Buckets[i]; e = e->Flink) {
            KG_PID_ENTRY* srcEntry = CONTAINING_RECORD(e, KG_PID_ENTRY, Link);
            KG_PID_ENTRY* newEntry = (KG_PID_ENTRY*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_PID_ENTRY), KG_POOL_TAG);
            if (!newEntry) { KgFreePidMap(dst); return NULL; }
            newEntry->Pid = srcEntry->Pid;
            newEntry->SlotIndex = srcEntry->SlotIndex;
            InsertTailList(&dst->Buckets[i], &newEntry->Link);
        }
    }
    return dst;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：原子替换全局 PID 映射表指针
//       使用 InterlockedExchangePointer 实现无锁切换，
//       每次切换递增 gPidMapEpoch 以通知读者重新获取快照
// 参数：
//   newMap - 要替换的新映射表指针
// 返回值：旧的映射表指针，调用者负责延迟释放
////////////////////////////////////////////////////////////////////////////////
KG_PID_MAP* KgSwapPidMap(KG_PID_MAP* newMap)
{
    KG_PID_MAP* old = (KG_PID_MAP*)InterlockedExchangePointer((PVOID*)&gState->PidMap, newMap);
    InterlockedIncrement(&gPidMapEpoch);
    return old;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：等待旧映射表的 Rundown 保护释放后安全释放
//       确保所有正在通过 ExAcquireRundownProtection 读取旧表的线程
//       完成后才真正释放内存，避免 use-after-free
// 参数：
//   old - 待释放的旧映射表；允许 NULL
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreeOldPidMap(KG_PID_MAP* old)
{
    if (old) {
        ExWaitForRundownProtectionRelease(&old->RundownRef);
        KgFreePidMap(old);
    }
}

////////////////////////////////////////////////////////////////////////////////
// 功能：初始化全局快速 PID 缓存数组
//       将所有 65536 个槽位标记为 KG_PID_SLOT_EMPTY（0xFF）
// 参数：无
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgPidMapInit(VOID)
{
    for (ULONG i = 0; i < 65536; i++) {
        gPidSlotFast[i] = KG_PID_SLOT_EMPTY;
        gPidNetCache[i] = KG_NET_ALLOW;
        gBwrapFast[i] = 0;
    }
}
