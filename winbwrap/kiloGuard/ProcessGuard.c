#include "ProcessGuard.h"
#include "FileGuard.h"
#include "Domain.h"
#include "Pidmap.h"
#include "PidPortMap.h"
#include "KiloGuardSecret.h"
#include <ntimage.h>

NTSYSAPI NTSTATUS NTAPI ZwQueryInformationProcess(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

PVOID gObRegistration = NULL;

////////////////////////////////////////////////////////////////////////////////
// 功能：获取进程的映像文件完整路径
//       通过 SeLocateProcessImageName 从 EPROCESS 中提取
// 参数：
//   Process   - 进程 EPROCESS 对象
//   ImageName - 输出参数，接收指向映像路径 UNICODE_STRING 的指针
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID KgGetProcessImageName(PEPROCESS Process, PUNICODE_STRING* ImageName)
{
    *ImageName = NULL;
    SeLocateProcessImageName(Process, ImageName);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：获取进程的退出码
// 参数：
//   Process    - 进程 EPROCESS 对象
//   ExitStatus - 输出参数，接收退出码
// 返回值：STATUS_SUCCESS 成功；STATUS_INVALID_PARAMETER 进程对象为 NULL
////////////////////////////////////////////////////////////////////////////////
static NTSTATUS KgGetProcessExitStatus(PEPROCESS Process, LONG* ExitStatus)
{
    *ExitStatus = 0;
    if (!Process) return STATUS_INVALID_PARAMETER;
    *ExitStatus = (LONG)PsGetProcessExitStatus(Process);
    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：记录沙箱内进程加载的 DLL
//       从完整路径中提取 DLL 文件名，去重后插入追踪器的 DLL 链表
// 参数：
//   tracker - 目标进程追踪器
//   fullPath - 已加载映像的完整路径（含路径+DLL 名）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID KgRecordDllLoad(KG_PENDING_TRACKER* tracker, PUNICODE_STRING fullPath)
{
    if (!tracker || !fullPath || !fullPath->Buffer || fullPath->Length == 0)
        return;

    USHORT totalLen = fullPath->Length;
    PWCHAR p = fullPath->Buffer + (totalLen / sizeof(WCHAR)) - 1;
    while (p >= fullPath->Buffer && *p != L'\\' && *p != L'/')
        p--;
    p++;

    USHORT nameLen = (USHORT)((fullPath->Buffer + totalLen / sizeof(WCHAR) - p) * sizeof(WCHAR));
    if (nameLen < 4 || nameLen > 126)
        return;

    for (PLIST_ENTRY e = tracker->DllList.Flink; e != &tracker->DllList; e = e->Flink) {
        KG_DLL_ENTRY* entry = CONTAINING_RECORD(e, KG_DLL_ENTRY, Link);
        if (_wcsnicmp(entry->Name, p, nameLen / sizeof(WCHAR)) == 0 &&
            entry->Name[nameLen / sizeof(WCHAR)] == L'\0')
            return;
    }

    if (tracker->DllCount >= 64)
        return;

    KG_DLL_ENTRY* newEntry = (KG_DLL_ENTRY*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KG_DLL_ENTRY), KG_POOL_TAG);
    if (!newEntry) return;

    RtlZeroMemory(newEntry->Name, sizeof(newEntry->Name));
    RtlCopyMemory(newEntry->Name, p, nameLen);
    newEntry->Name[nameLen / sizeof(WCHAR)] = L'\0';
    InsertTailList(&tracker->DllList, &newEntry->Link);
    InterlockedIncrement(&tracker->DllCount);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：检查指定的 DLL 是否已被进程加载（在追踪器的 DLL 列表中）
//       用于启动失败分析时判断缺失的 DLL 是否确实未被加载
// 参数：
//   tracker - 进程追踪器（含已加载 DLL 链表）
//   dllName - 待检查的 DLL 文件名
// 返回值：TRUE=已加载，FALSE=未加载或参数无效
////////////////////////////////////////////////////////////////////////////////
static BOOLEAN KgIsDllLoaded(KG_PENDING_TRACKER* tracker, PCWSTR dllName)
{
    if (!tracker || !dllName) return FALSE;
    SIZE_T nameLen = wcslen(dllName);
    if (nameLen == 0) return FALSE;

    PLIST_ENTRY e = tracker->DllList.Flink;
    for (LONG i = 0; i < tracker->DllCount; i++) {
        if (!e) break;
        KG_DLL_ENTRY* entry = CONTAINING_RECORD(e, KG_DLL_ENTRY, Link);
        if (_wcsnicmp(entry->Name, dllName, nameLen) == 0 &&
            entry->Name[nameLen] == L'\0')
            return TRUE;
        e = e->Flink;
    }
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：判断 DLL 是否为 Windows 系统 DLL
//       与已知系统 DLL 列表（ntdll、kernel32、KERNELBASE 等）进行
//       大小写不敏感比较
// 参数：
//   dllName - DLL 文件名（不含路径）
// 返回值：TRUE=系统 DLL，FALSE=非系统 DLL
////////////////////////////////////////////////////////////////////////////////
static BOOLEAN KgIsSystemDll(PCWSTR dllName)
{
    if (!dllName) return FALSE;
    UNICODE_STRING sysList[] = {
        RTL_CONSTANT_STRING(L"ntdll.dll"),
        RTL_CONSTANT_STRING(L"kernel32.dll"),
        RTL_CONSTANT_STRING(L"KERNELBASE.dll"),
        RTL_CONSTANT_STRING(L"advapi32.dll"),
        RTL_CONSTANT_STRING(L"msvcrt.dll"),
        RTL_CONSTANT_STRING(L"sechost.dll"),
        RTL_CONSTANT_STRING(L"RPCRT4.dll"),
        RTL_CONSTANT_STRING(L"bcrypt.dll"),
        RTL_CONSTANT_STRING(L"ucrtbase.dll"),
        RTL_CONSTANT_STRING(L"bcryptprimitives.dll"),
    };
    for (ULONG i = 0; i < ARRAYSIZE(sysList); i++) {
        if (_wcsicmp(dllName, sysList[i].Buffer) == 0)
            return TRUE;
    }
    if (_wcsnicmp(dllName, L"api-ms-win-", 11) == 0)
        return TRUE;
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：将 PE 文件的相对虚拟地址（RVA）转换为文件偏移
//       遍历节表找到包含该 RVA 的节，计算对应文件偏移
// 参数：
//   base - PE 文件头部缓冲区起始地址
//   rva  - 相对虚拟地址
// 返回值：文件偏移；未找到返回 -1
////////////////////////////////////////////////////////////////////////////////
static LONG KgRvaToFileOffset(PUCHAR base, ULONG rva)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (USHORT i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (rva >= sec[i].VirtualAddress &&
            rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
            return rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
        }
    }
    return (LONG)-1;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：分析进程因 DLL 加载失败而退出的原因
//       打开进程映像 PE 文件，解析导入表（Import Directory），
//       遍历所有导入 DLL，排除系统 DLL 后，
//       若发现非系统 DLL 未被加载则推送失败事件到沙箱域事件环
// 参数：
//   tracker - 退出进程的追踪器（含映像路径和已加载 DLL 列表）
//   domain  - 退出进程所属的沙箱域
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID KgAnalyzeDllFailure(KG_PENDING_TRACKER* tracker, KG_POLICY_DOMAIN* domain)
{
    if (!tracker || !domain || tracker->ImageName[0] == L'\0')
        return;

    UNICODE_STRING path;
    RtlInitUnicodeString(&path, tracker->ImageName);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    HANDLE hFile = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = ZwCreateFile(&hFile, GENERIC_READ, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(status)) return;

    ULONG headerSize = 4096;
    PUCHAR headerBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, headerSize, KG_POOL_TAG);
    if (!headerBuf) { ZwClose(hFile); return; }

    status = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, headerBuf, headerSize, NULL, NULL);
    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(headerBuf, KG_POOL_TAG); ZwClose(hFile); return; }

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)headerBuf;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (ULONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > headerSize) {
        ExFreePoolWithTag(headerBuf, KG_POOL_TAG); ZwClose(hFile); return;
    }

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(headerBuf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        ExFreePoolWithTag(headerBuf, KG_POOL_TAG); ZwClose(hFile); return;
    }

    ULONG importRva = 0;
    ULONG importSize = 0;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        PIMAGE_NT_HEADERS64 nt64 = (PIMAGE_NT_HEADERS64)nt;
        if (nt64->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
            importRva = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
            importSize = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
        }
    } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        PIMAGE_NT_HEADERS32 nt32 = (PIMAGE_NT_HEADERS32)nt;
        if (nt32->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
            importRva = nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
            importSize = nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
        }
    }

    if (importRva == 0 || importSize == 0) {
        ExFreePoolWithTag(headerBuf, KG_POOL_TAG); ZwClose(hFile); return;
    }

    LONG importOffset = KgRvaToFileOffset(headerBuf, importRva);
    if (importOffset < 0) {
        ExFreePoolWithTag(headerBuf, KG_POOL_TAG); ZwClose(hFile); return;
    }

    PUCHAR importBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, importSize, KG_POOL_TAG);
    if (!importBuf) {
        ExFreePoolWithTag(headerBuf, KG_POOL_TAG); ZwClose(hFile); return;
    }

    LARGE_INTEGER byteOffset;
    byteOffset.QuadPart = importOffset;
    status = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, importBuf, importSize, &byteOffset, NULL);

    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(importBuf, KG_POOL_TAG);
        ExFreePoolWithTag(headerBuf, KG_POOL_TAG);
        ZwClose(hFile);
        return;
    }

    PIMAGE_IMPORT_DESCRIPTOR import = (PIMAGE_IMPORT_DESCRIPTOR)importBuf;
    ULONG importCount = 0;
    ULONG nonsysCount = 0;
    for (ULONG i = 0; import[i].Name != 0; i++) {
        importCount++;
        LONG nameFileOffset = KgRvaToFileOffset(headerBuf, import[i].Name);
        if (nameFileOffset < 0) continue;

        CHAR ansiName[128];
        RtlZeroMemory(ansiName, sizeof(ansiName));
        LARGE_INTEGER nameOffset;
        nameOffset.QuadPart = nameFileOffset;
        status = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, ansiName, sizeof(ansiName) - 1, &nameOffset, NULL);
        if (!NT_SUCCESS(status)) continue;

        WCHAR wideName[128];
        RtlZeroMemory(wideName, sizeof(wideName));
        ULONG j = 0;
        while (j < 127 && ansiName[j]) {
            wideName[j] = (WCHAR)ansiName[j];
            j++;
        }
        wideName[j] = L'\0';

        if (KgIsSystemDll(wideName)) continue;
        nonsysCount++;

        if (!KgIsDllLoaded(tracker, wideName)) {
            UNICODE_STRING dllNameUs;
            RtlInitUnicodeString(&dllNameUs, wideName);
            KgPushEvent(domain, tracker, &dllNameUs);

            KG_LOG("FileGuard: DomainID=%lu PID=%lu FAILED_TO_LOAD DllCount=%ld\n",
                     domain->Id,
                     (ULONG)(ULONG_PTR)tracker->Pid,
                     tracker->DllCount);
        }
    }

    ZwClose(hFile);
    ExFreePoolWithTag(importBuf, KG_POOL_TAG);
    ExFreePoolWithTag(headerBuf, KG_POOL_TAG);

    KG_LOG("FileGuard: DomainID=%lu PID=%lu IMPORT_PARSED Total=%lu Nonsys=%lu\n",
             domain->Id, (ULONG)(ULONG_PTR)tracker->Pid, importCount, nonsysCount);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：进程创建/退出通知回调（PsSetCreateProcessNotifyRoutineEx）
// 创建时（CreateInfo != NULL）：
//   检查父进程是否在沙箱内，若是则将子进程也加入同一沙箱域（进程继承）
//   同时为子进程分配追踪器用于记录 DLL 加载
// 退出时（CreateInfo == NULL）：
//   检查退出码是否为 STATUS_DLL_NOT_FOUND(0xC0000135) /
//   STATUS_ENTRYPOINT_NOT_FOUND(0xC0000139) / STATUS_DLL_INIT_FAILED(0xC0000142)，
//   若是则分析 PE 导入表找出缺失的非系统 DLL，推入事件环形缓冲区
//   最后从 PID 映射表和受信白名单中移除该进程
// 参数：
//   Process   - 进程 EPROCESS 对象
//   ProcessId - 进程 PID
//   CreateInfo - NULL=进程退出，非NULL=进程创建信息
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID
KiloProcessNotify(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo)
{
    if (CreateInfo) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&gStateLock, &oldIrql);
        KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)InterlockedCompareExchangePointer((PVOID*)&gState, NULL, NULL);
        if (!state || !state->Policy.ProcessInheritanceEnabled) { KeReleaseSpinLock(&gStateLock, oldIrql); return; }

        HANDLE parentPid = CreateInfo->ParentProcessId;
        ULONG parentSlot = KgLookupSlotByPid(state->PidMap, parentPid);

        if (parentSlot != (ULONG)-1) {
            KG_PID_MAP* newMap = KgClonePidMap(state->PidMap);
            if (!newMap) { KeReleaseSpinLock(&gStateLock, oldIrql); return; }

            KgSetPidSlot(newMap, ProcessId, parentSlot);

            KG_PID_MAP* oldMap = KgSwapPidMap(newMap);
            KeReleaseSpinLock(&gStateLock, oldIrql);
            KgFreeOldPidMap(oldMap);

            PUNICODE_STRING imageName = NULL;
            KgGetProcessImageName(Process, &imageName);

            UCHAR netStatus = KG_NET_BLOCK;
            if (imageName && state->Domain[parentSlot].NetBlockEnabled &&
                KgIsNetExeInList(&state->Domain[parentSlot], imageName))
                netStatus = KG_NET_ALLOW;
            KgSetPidNetCache(ProcessId, netStatus);

            KIRQL tIrql;
            KeAcquireSpinLock(&gTrackerLock, &tIrql);
            KgAllocTracker(ProcessId, parentSlot, imageName);
            KeReleaseSpinLock(&gTrackerLock, tIrql);

            if (imageName) {
                KG_LOG("FileGuard: DomainID=%lu PID=%lu ProcessName=%wZ\n",
                         state->Domain[parentSlot].Id,
                         (ULONG)(ULONG_PTR)ProcessId,
                         imageName);
                KgSendProcessEvent(parentSlot, (ULONG)(ULONG_PTR)ProcessId,
                                   KG_PORT_MSG_PROCESS_CREATE, imageName);
                ExFreePool(imageName);
            }
        } else {
            KeReleaseSpinLock(&gStateLock, oldIrql);
        }
    } else {
        KG_POLICY_DOMAIN* notifyDomain = NULL;
        KG_PENDING_TRACKER trackerCopy;
        RtlZeroMemory(&trackerCopy, sizeof(trackerCopy));
        BOOLEAN shouldAnalyze = FALSE;

        KIRQL tIrql;
        KeAcquireSpinLock(&gTrackerLock, &tIrql);
        KG_PENDING_TRACKER* tracker = KgFindTracker(ProcessId);
        if (tracker) {
            LONG exitStatus = 0;
            KgGetProcessExitStatus(Process, &exitStatus);
            {
                KIRQL sIrql;
                KeAcquireSpinLock(&gStateLock, &sIrql);
                KG_SYSTEM_STATE* s = (KG_SYSTEM_STATE*)InterlockedCompareExchangePointer((PVOID*)&gState, NULL, NULL);
                KG_SANDBOX_ID domainId = (s && tracker->SlotIndex < KG_MAX_DOMAIN)
                    ? s->Domain[tracker->SlotIndex].Id : 0;
                KeReleaseSpinLock(&gStateLock, sIrql);
                KG_LOG("FileGuard: DomainID=%lu PID=%lu ExitCode=%08lX\n",
                         domainId,
                         (ULONG)(ULONG_PTR)ProcessId,
                         exitStatus);
            }
            if (exitStatus == (LONG)0xC0000135 || exitStatus == (LONG)0xC0000139 ||
                exitStatus == (LONG)0xC0000142) {
                RtlCopyMemory(&trackerCopy, tracker, sizeof(KG_PENDING_TRACKER));
                shouldAnalyze = TRUE;
            }
        }
        if (!shouldAnalyze) {
            KgFreeTracker(ProcessId);
        }
        KeReleaseSpinLock(&gTrackerLock, tIrql);

        if (shouldAnalyze) {
            KIRQL oldIrql;
            KeAcquireSpinLock(&gStateLock, &oldIrql);
            KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)InterlockedCompareExchangePointer((PVOID*)&gState, NULL, NULL);
            if (state && trackerCopy.SlotIndex < KG_MAX_DOMAIN &&
                state->Domain[trackerCopy.SlotIndex].Active) {
                notifyDomain = &state->Domain[trackerCopy.SlotIndex];
            }
            KeReleaseSpinLock(&gStateLock, oldIrql);

            if (notifyDomain) {
                KgAnalyzeDllFailure(&trackerCopy, notifyDomain);
            }

            KeAcquireSpinLock(&gTrackerLock, &tIrql);
            KgFreeTracker(ProcessId);
            KeReleaseSpinLock(&gTrackerLock, tIrql);
        }

        KIRQL oldIrql;
        KeAcquireSpinLock(&gStateLock, &oldIrql);
        KG_SYSTEM_STATE* state = (KG_SYSTEM_STATE*)InterlockedCompareExchangePointer((PVOID*)&gState, NULL, NULL);
        if (!state) { KeReleaseSpinLock(&gStateLock, oldIrql); return; }

        KG_PID_MAP* newMap = KgClonePidMap(state->PidMap);
        if (!newMap) { KeReleaseSpinLock(&gStateLock, oldIrql); return; }

        ULONG exitSlot = KgLookupSlotByPid(state->PidMap, ProcessId);

        KgRemoveTrustedPid(ProcessId);
        KgRemovePid(newMap, ProcessId);
        KgSetPidNetCache(ProcessId, KG_NET_ALLOW);

        KG_PID_MAP* oldMap = KgSwapPidMap(newMap);
        KeReleaseSpinLock(&gStateLock, oldIrql);
        KgFreeOldPidMap(oldMap);

        if (exitSlot != (ULONG)-1)
            KgSendProcessEvent(exitSlot, (ULONG)(ULONG_PTR)ProcessId,
                               KG_PORT_MSG_PROCESS_EXIT, NULL);
    }
}

////////////////////////////////////////////////////////////////////////////////
// 功能：线程创建/退出通知回调
//       当前为占位实现，暂不处理线程相关事件
//       保留以备将来需要线程级沙箱控制时使用
// 参数：
//   ProcessId - 线程所属进程 PID
//   ThreadId  - 线程 ID
//   Create    - TRUE=创建，FALSE=退出
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID
KiloThreadNotify(HANDLE ProcessId, HANDLE ThreadId, BOOLEAN Create)
{
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(ThreadId);
    UNREFERENCED_PARAMETER(Create);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：映像加载通知回调
//       当沙箱内进程加载 DLL 或 EXE 映像时，
//       记录已加载的 DLL 名称到该进程的追踪器
//       用于后续启动失败分析（对比导入表判断缺失 DLL）
// 参数：
//   ImageName - 被加载映像的完整路径
//   ProcessId - 加载该映像的进程 PID
//   ImageInfo - 映像信息（当前未使用）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
static VOID
KiloImageLoadNotify(PUNICODE_STRING ImageName, HANDLE ProcessId, PIMAGE_INFO ImageInfo)
{
    UNREFERENCED_PARAMETER(ImageInfo);
    if (!ImageName || !ImageName->Buffer) return;

    /* Log RPC/COM DLL loading by sandboxed processes */
    if (KgIsPidInSandBox(ProcessId) != (ULONG)-1)
    {
        /* Extract filename from full path */
        PWCHAR p = ImageName->Buffer + (ImageName->Length / sizeof(WCHAR));
        while (p > ImageName->Buffer && p[-1] != L'\\') p--;

        /* Known RPC/COM DLLs to track */
        static const WCHAR* rpcDlls[] = {
            L"rpcrt4.dll", L"rpcss.dll", L"ole32.dll", L"combase.dll",
            L"oleaut32.dll", L"clbcatq.dll", L"comsvcs.dll",
        };
        for (ULONG i = 0; i < ARRAYSIZE(rpcDlls); i++) {
            if (_wcsicmp(p, rpcDlls[i]) == 0) {
                KG_LOG("FileGuard: RPC_LOAD: PID=%lu DLL=%s ImageName=%wZ\n",
                         (ULONG)(ULONG_PTR)ProcessId, rpcDlls[i], ImageName);
                break;
            }
        }
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&gTrackerLock, &oldIrql);
    KG_PENDING_TRACKER* tracker = KgFindTracker(ProcessId);
    if (tracker) {
        InterlockedIncrement(&tracker->ImageCount);
        KgRecordDllLoad(tracker, ImageName);
    }
    KeReleaseSpinLock(&gTrackerLock, oldIrql);
}

////////////////////////////////////////////////////////////////////////////////
// 功能：进程句柄创建前置回调(ObRegisterCallbacks)
//       当前为占位实现，句柄权限剥离已确认在 CreateProcess 路径上无法区分
//       OpenProcess vs CreateProcess（两者参数完全一致），因此放弃剥离。
// 参数：
//      RegistrationContext - 注册上下文
//      Info               - 操作信息
// 返回值：
//      OB_PREOP_SUCCESS（始终放行）
static OB_PREOP_CALLBACK_STATUS
KiloPreOperation(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION Info)
{
    /*
     * 这个函数是 ObRegisterCallbacks 的进程句柄创建前置回调。
     * 当任意进程打开另一个进程的句柄时触发。
     *
     * ── 设计目标 ──
     *
     * 最初的目标是：当沙箱内进程通过 OpenProcess 打开其他进程时，
     * 从 DesiredAccess 中剥离危险权限：
     *
     *   PROCESS_CREATE_PROCESS | PROCESS_CREATE_THREAD |
     *   PROCESS_VM_WRITE       | PROCESS_VM_OPERATION  |
     *   PROCESS_VM_READ        | PROCESS_DUP_HANDLE    |
     *   PROCESS_SUSPEND_RESUME | PROCESS_TERMINATE     |
     *   PROCESS_SET_INFORMATION| PROCESS_SET_QUOTA     |
     *   PROCESS_SET_SESSIONID
     *
     * 防止沙箱进程通过 OpenProcess 操纵其他进程。
     *
     * ── 为什么放弃 ──
     *
     * ObRegisterCallbacks 的 OB_OPERATION_HANDLE_CREATE 同时覆盖以下两个
     * 完全不同的 API，且两者的参数无法区分：
     *
     *   OpenProcess          → 打开已有进程的句柄
     *   NtCreateUserProcess  → 创建子进程时，父进程收到子进程的句柄
     *
     * 在这条回调路径上无法判断当前是"沙箱内的 A 打开沙箱外的 B"还是
     * "沙箱内的 A 创建了子进程 B"。两者的 ObjectType 都是 PsProcessType，
     * TargetProcess 都是被操作对象，CreateInfo 在 HandleCreate 下不可用。
     *
     * 如果在此回调中剥离了危险权限，那么当沙箱父进程创建子进程时，
     * 子进程句柄上的权限也会被剥离。子进程初始化过程中需要父进程句柄
     * 上的某些权限（如 PROCESS_VM_WRITE 用于写入子进程地址空间以设置
     * 参数和环境变量），剥离后导致子进程无法正常启动——这就是原版
     * KiloGuard.c（r315 加入，r318 删除）导致子进程死锁的根本原因之一。
     *
     * ── 当前状态 ──
     *
     * 此回调保留注册框架和调用链，但函数体为空，始终放行所有句柄创建。
     * 将来若找到区分 OpenProcess 与 NtCreateUserProcess 的可靠方法（如
     * 通过 ObjectInfo 中的额外字段或引入内核补丁），可在此处重新启用
     * 权限剥离逻辑。
     */
    UNREFERENCED_PARAMETER(RegistrationContext);
    UNREFERENCED_PARAMETER(Info);
    return OB_PREOP_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：注册所有进程相关的系统回调
//       1. 进程创建/退出通知 PsSetCreateProcessNotifyRoutineEx
//       2. 线程创建通知 PsSetCreateThreadNotifyRoutine
//       3. 映像加载通知 PsSetLoadImageNotifyRoutine
//       4. 进程句柄操作回调 ObRegisterCallbacks
//       任一回调注册失败时关闭对应的全局策略开关
// 参数：无
// 返回值：STATUS_SUCCESS（即使部分回调注册失败也返回成功）
////////////////////////////////////////////////////////////////////////////////
NTSTATUS KgRegisterProcessCallbacks(VOID)
{
    NTSTATUS status;

    // 进程创建/退出通知 -- 回调函数 KiloProcessNotify
    status = PsSetCreateProcessNotifyRoutineEx(KiloProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        KG_SYSTEM_STATE* s = KgAcquireState();
        if (s) { s->Policy.ProcessInheritanceEnabled = FALSE; KgReleaseState(s); }
    }

    // 线程创建/退出通知 -- 回调函数 KiloThreadNotify
    status = PsSetCreateThreadNotifyRoutine(KiloThreadNotify);
    if (!NT_SUCCESS(status)) { }

    // 映像加载通知 -- 回调函数：KiloImageLoadNotify
    status = PsSetLoadImageNotifyRoutine(KiloImageLoadNotify);
    if (!NT_SUCCESS(status)) { }

    OB_OPERATION_REGISTRATION opReg;
    RtlZeroMemory(&opReg, sizeof(opReg));
    opReg.ObjectType = PsProcessType;
    opReg.Operations = OB_OPERATION_HANDLE_CREATE;
    opReg.PreOperation = KiloPreOperation;
    opReg.PostOperation = NULL;

    OB_CALLBACK_REGISTRATION cbReg;
    RtlZeroMemory(&cbReg, sizeof(cbReg));
    cbReg.Version = OB_FLT_REGISTRATION_VERSION;
    cbReg.OperationRegistration = &opReg;
    cbReg.OperationRegistrationCount = 1;
    RtlInitUnicodeString(&cbReg.Altitude, L"360000");

    // 对象回调 -- 回调函数: KiloPreOperation
    // 触发：任意进程打开另一个进程的句柄时（OpenProcess）
    // 用途：从沙箱进程的期望访问掩码中剥离危险权限（PROCESS_VM_WRITE、PROCESS_TERMINATE 等）
    status = ObRegisterCallbacks(&cbReg, &gObRegistration);
    if (!NT_SUCCESS(status)) {
        KG_SYSTEM_STATE* s = KgAcquireState();
        if (s) { s->Policy.HandleProtectionEnabled = FALSE; KgReleaseState(s); }
    }

    return STATUS_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 功能：注销所有已注册的回调
//       包括 ObRegisterCallbacks 的句柄回调、进程创建/退出通知、
//       线程创建通知、映像加载通知
// 参数：无
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgUnregisterProcessCallbacks(VOID)
{
    if (gObRegistration) {
        ObUnRegisterCallbacks(gObRegistration);
        gObRegistration = NULL;
    }
    PsSetCreateProcessNotifyRoutineEx(KiloProcessNotify, TRUE);
    PsRemoveCreateThreadNotifyRoutine(KiloThreadNotify);
    PsRemoveLoadImageNotifyRoutine(KiloImageLoadNotify);
}
