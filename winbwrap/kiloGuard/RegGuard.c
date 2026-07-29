
#include "KiloGuard.h"
#include "RegGuard.h"
#include "Domain.h"

static LARGE_INTEGER gRegCookie = { 0 };

/* ============================================================================
   敏感注册表路径黑名单（P0 / P1 / P2）
   沙箱内进程读取匹配以下前缀的键时，返回 STATUS_ACCESS_DENIED。
   路径使用 NT 格式（以 \Registry\ 开头，如 \Registry\Machine\...）。
   ============================================================================ */

static const WCHAR* gRegDenyPrefixes[] = {
    /* --- P0: 凭据 / 密钥 / 沙箱自身 --- */
    L"\\Registry\\Machine\\SECURITY\\Policy\\Secrets",
    L"\\Registry\\Machine\\SECURITY\\SAM",
    L"\\Registry\\Machine\\SAM",
    L"\\Registry\\Machine\\System\\ControlSet*\\Services\\KiloGuard",
    L"\\Registry\\Machine\\Software\\SimonTatham\\PuTTY\\Sessions",
    L"\\Registry\\User\\*\\Software\\SimonTatham\\PuTTY\\Sessions",
    L"\\Registry\\Machine\\Software\\Martin Prikryl\\WinSCP 2\\Sessions",
    L"\\Registry\\User\\*\\Software\\Martin Prikryl\\WinSCP 2\\Sessions",
    L"\\Registry\\Machine\\Software\\FileZilla",
    L"\\Registry\\User\\*\\Software\\FileZilla",

    /* --- P1: 隐私 / 行为轨迹 --- */
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RecentDocs",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RunMRU",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TypedPaths",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\Shell\\BagMRU",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\Shell\\Bags",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Internet Explorer\\TypedURLs",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ComDlg32\\OpenSavePidlMRU",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ComDlg32\\LastVisitedPidlMRU",
    L"\\Registry\\User\\*\\Network",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist",

    /* --- P2: 安全防御情报 / 补丁 / 防火墙 --- */
    L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\Packages",
    L"\\Registry\\Machine\\Software\\Policies\\Microsoft\\Windows Defender",
    L"\\Registry\\Machine\\Software\\Microsoft\\Windows Defender",
    L"\\Registry\\User\\*\\Software\\Microsoft\\Windows Defender",
    L"\\Registry\\Machine\\System\\ControlSet*\\Services\\SharedAccess\\Parameters\\FirewallPolicy",
    L"\\Registry\\Machine\\System\\ControlSet*\\Control\\Lsa",
    L"\\Registry\\Machine\\System\\ControlSet*\\Control\\SecurePipeServers",
    L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
    L"\\Registry\\User\\*\\Software\\Sysinternals",
};

static const ULONG gRegDenyPrefixCount =
    sizeof(gRegDenyPrefixes) / sizeof(gRegDenyPrefixes[0]);

/* ============================================================================
   KgRegMatchPrefix — 判断 path 是否以 prefix 为前缀（不区分大小写）
   prefix 中可包含通配符 '*'，仅用于匹配注册表用户 SID 段（单段匹配）
   ============================================================================ */
static BOOLEAN KgRegMatchPrefix(const WCHAR* path, ULONG pathLen, const WCHAR* prefix)
{
    ULONG i = 0, j = 0;

    while (i < pathLen && prefix[j] != L'\0')
    {
        if (prefix[j] == L'*')
        {
            j++;
            ULONG k = i;
            while (k < pathLen && path[k] != L'\\')
                k++;
            i = k;
        }
        else if (towupper(path[i]) == towupper(prefix[j]))
        {
            i++;
            j++;
        }
        else
        {
            return FALSE;
        }
    }

    return (prefix[j] == L'\0');
}

/* ============================================================================
   KgRegIsSensitivePath — 检查路径是否命中黑名单
   返回 TRUE 表示命中（应拒绝访问）
   ============================================================================ */
static BOOLEAN KgRegIsSensitivePath(PCUNICODE_STRING path)
{
    if (!path || !path->Buffer || path->Length == 0)
        return FALSE;

    ULONG pathChars = path->Length / sizeof(WCHAR);

    for (ULONG idx = 0; idx < gRegDenyPrefixCount; idx++)
    {
        if (KgRegMatchPrefix(path->Buffer, pathChars, gRegDenyPrefixes[idx]))
            return TRUE;
    }
    return FALSE;
}

/* ============================================================================
   KgRegCheckObject — 从键对象指针获取完整路径并检查是否命中黑名单
   使用 CmCallbackGetKeyObjectID 获取键的完整 NT 路径（如 \Registry\Machine\...）。
   返回 TRUE 表示命中（应拒绝访问）。
   ============================================================================ */
static BOOLEAN KgRegCheckObject(PVOID keyObject)
{
    if (!keyObject)
        return FALSE;

    PCUNICODE_STRING name = NULL;
    NTSTATUS status = CmCallbackGetKeyObjectID(
        &gRegCookie, keyObject, NULL, (PUNICODE_STRING*)&name);
    if (!NT_SUCCESS(status) || !name ||
        !name->Buffer || name->Length == 0)
    {
        return FALSE;
    }

    /* Block COM CLSID reads */
    if (wcsstr(name->Buffer, L"CLSID\\{"))
        return TRUE;

    return KgRegIsSensitivePath(name);
}

/* ============================================================================
   KgRegCheckPreOpen — 检查 PreOpenKey / PreCreateKey 是否应该被拒绝
   优先使用 CompleteName，否则用 RootObject + CmCallbackGetKeyObjectID
   拼出完整路径后检查黑名单。
   ============================================================================ */
static BOOLEAN KgRegCheckPreOpen(PVOID arg2)
{
    PREG_CREATE_KEY_INFORMATION_V1 info = (PREG_CREATE_KEY_INFORMATION_V1)arg2;
    if (!info)
        return FALSE;

    /* Fast path: CompleteName is populated for full-path creates/opens */
    if (info->CompleteName && info->CompleteName->Buffer &&
        info->CompleteName->Length > 0)
    {
        if (wcsstr(info->CompleteName->Buffer, L"CLSID\\{"))
            return TRUE;
        return KgRegIsSensitivePath(info->CompleteName);
    }

    /* Slow path: resolve RootObject full name, append RemainingName */
    if (!info->RootObject)
        return FALSE;

    PCUNICODE_STRING rootName = NULL;
    NTSTATUS status = CmCallbackGetKeyObjectID(
        &gRegCookie, info->RootObject, NULL, (PUNICODE_STRING*)&rootName);
    if (!NT_SUCCESS(status) || !rootName ||
        !rootName->Buffer || rootName->Length == 0)
    {
        return FALSE;
    }

    WCHAR fullBuf[1024];
    ULONG off = 0;
    ULONG rootChars = rootName->Length / sizeof(WCHAR);

    if (rootChars >= ARRAYSIZE(fullBuf))
        return FALSE;

    RtlCopyMemory(fullBuf, rootName->Buffer, rootName->Length);
    off = rootChars;

    if (info->RemainingName && info->RemainingName->Buffer &&
        info->RemainingName->Length > 0)
    {
        if (off + 1 >= ARRAYSIZE(fullBuf))
            return FALSE;
        fullBuf[off++] = L'\\';

        ULONG remChars = info->RemainingName->Length / sizeof(WCHAR);
        if (off + remChars >= ARRAYSIZE(fullBuf))
            remChars = ARRAYSIZE(fullBuf) - off - 1;

        RtlCopyMemory(fullBuf + off, info->RemainingName->Buffer,
            remChars * sizeof(WCHAR));
        off += remChars;
    }

    fullBuf[off] = L'\0';

    UNICODE_STRING fullUs;
    RtlInitUnicodeString(&fullUs, fullBuf);

    if (wcsstr(fullBuf, L"CLSID\\{"))
        return TRUE;

    return KgRegIsSensitivePath(&fullUs);
}

static PCSTR KgRegNotifyClassToString(REG_NOTIFY_CLASS notifyClass)
{
    switch (notifyClass) {
    case RegNtPreDeleteKey:             return "PreDeleteKey";
    case RegNtPreSetValueKey:           return "PreSetValueKey";
    case RegNtPreDeleteValueKey:        return "PreDeleteValueKey";
    case RegNtPreSetInformationKey:     return "PreSetInformationKey";
    case RegNtPreRenameKey:             return "PreRenameKey";
    case RegNtPreEnumerateKey:          return "PreEnumerateKey";
    case RegNtPreEnumerateValueKey:     return "PreEnumerateValueKey";
    case RegNtPreQueryKey:              return "PreQueryKey";
    case RegNtPreQueryValueKey:         return "PreQueryValueKey";
    case RegNtPreQueryMultipleValueKey: return "PreQueryMultipleValueKey";
    case RegNtPreCreateKey:             return "PreCreateKey";
    case RegNtPostCreateKey:            return "PostCreateKey";
    case RegNtPreOpenKey:               return "PreOpenKey";
    case RegNtPostOpenKey:              return "PostOpenKey";
    case RegNtPreKeyHandleClose:        return "PreKeyHandleClose";
    case RegNtPostDeleteKey:            return "PostDeleteKey";
    case RegNtPostSetValueKey:          return "PostSetValueKey";
    case RegNtPostDeleteValueKey:       return "PostDeleteValueKey";
    case RegNtPostSetInformationKey:    return "PostSetInformationKey";
    case RegNtPostRenameKey:            return "PostRenameKey";
    case RegNtPostEnumerateKey:         return "PostEnumerateKey";
    case RegNtPostEnumerateValueKey:    return "PostEnumerateValueKey";
    case RegNtPostQueryKey:             return "PostQueryKey";
    case RegNtPostQueryValueKey:        return "PostQueryValueKey";
    case RegNtPostQueryMultipleValueKey:return "PostQueryMultipleValueKey";
    case RegNtPostKeyHandleClose:       return "PostKeyHandleClose";
    case RegNtPreFlushKey:              return "PreFlushKey";
    case RegNtPostFlushKey:             return "PostFlushKey";
    case RegNtPreLoadKey:               return "PreLoadKey";
    case RegNtPostLoadKey:              return "PostLoadKey";
    case RegNtPreUnLoadKey:             return "PreUnLoadKey";
    case RegNtPostUnLoadKey:            return "PostUnLoadKey";
    case RegNtPreQueryKeySecurity:      return "PreQueryKeySecurity";
    case RegNtPostQueryKeySecurity:     return "PostQueryKeySecurity";
    case RegNtPreSetKeySecurity:        return "PreSetKeySecurity";
    case RegNtPostSetKeySecurity:       return "PostSetKeySecurity";
    case RegNtPreRestoreKey:            return "PreRestoreKey";
    case RegNtPostRestoreKey:           return "PostRestoreKey";
    case RegNtPreSaveKey:               return "PreSaveKey";
    case RegNtPostSaveKey:              return "PostSaveKey";
    case RegNtPreReplaceKey:            return "PreReplaceKey";
    case RegNtPostReplaceKey:           return "PostReplaceKey";
    case RegNtPreQueryKeyName:          return "PreQueryKeyName";
    case RegNtPostQueryKeyName:         return "PostQueryKeyName";
    default:                            return "Unknown";
    }
}

static NTSTATUS KgRegCallback(PVOID context, PVOID arg1, PVOID arg2)
{
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)arg1;
    UNREFERENCED_PARAMETER(context);

    HANDLE pid = PsGetCurrentProcessId();

    /* Fast path: non-sandboxed process allow everything */
    if (KgIsPidInSandBox(pid) == (ULONG)-1)
        return STATUS_SUCCESS;

    /* Post operations & handle close: always allow, suppress noise */
    switch (notifyClass)
    {
    case RegNtPostCreateKey:
    case RegNtPostOpenKey:
    case RegNtPostDeleteKey:
    case RegNtPostSetValueKey:
    case RegNtPostDeleteValueKey:
    case RegNtPostSetInformationKey:
    case RegNtPostRenameKey:
    case RegNtPostEnumerateKey:
    case RegNtPostEnumerateValueKey:
    case RegNtPostQueryKey:
    case RegNtPostQueryValueKey:
    case RegNtPostQueryMultipleValueKey:
    case RegNtPostKeyHandleClose:
    case RegNtPostFlushKey:
    case RegNtPostLoadKey:
    case RegNtPostUnLoadKey:
    case RegNtPostQueryKeySecurity:
    case RegNtPostSetKeySecurity:
    case RegNtPostRestoreKey:
    case RegNtPostSaveKey:
    case RegNtPostReplaceKey:
    case RegNtPostQueryKeyName:
    case RegNtPreKeyHandleClose:
        return STATUS_SUCCESS;
    }

    /* Write operations: deny ? disabled */
    /* Write operations: deny */
    switch (notifyClass)
    {
    case RegNtPreCreateKey:
    case RegNtPreDeleteKey:
    {
        REG_CREATE_KEY_INFORMATION* info = (REG_CREATE_KEY_INFORMATION*)arg2;
        if (info->CompleteName && info->CompleteName->Buffer)
        {
            KG_LOG("RegGuard: DENY PID=%lu %s Key=%wZ\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass), info->CompleteName);
        }
        else
        {
            KG_LOG("RegGuard: DENY PID=%lu %s\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass));
        }
        return STATUS_ACCESS_DENIED;
    }
    case RegNtPreSetValueKey:
    {
        REG_SET_VALUE_KEY_INFORMATION* info = (REG_SET_VALUE_KEY_INFORMATION*)arg2;
        if (info->ValueName && info->ValueName->Buffer)
        {
            KG_LOG("RegGuard: DENY PID=%lu %s Value=%wZ\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass), info->ValueName);
        }
        else
        {
            KG_LOG("RegGuard: DENY PID=%lu %s\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass));
        }
        return STATUS_ACCESS_DENIED;
    }
    case RegNtPreDeleteValueKey:
    {
        REG_DELETE_VALUE_KEY_INFORMATION* info = (REG_DELETE_VALUE_KEY_INFORMATION*)arg2;
        if (info->ValueName && info->ValueName->Buffer)
        {
            KG_LOG("RegGuard: DENY PID=%lu %s Value=%wZ\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass), info->ValueName);
        }
        else
        {
            KG_LOG("RegGuard: DENY PID=%lu %s\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass));
        }
        return STATUS_ACCESS_DENIED;
    }
    /* RegNtPreSetInformationKey: allowed — needed by App Execution Alias resolution */
    case RegNtPreRenameKey:
    case RegNtPreSetKeySecurity:
    case RegNtPreLoadKey:
    case RegNtPreUnLoadKey:
    case RegNtPreRestoreKey:
    case RegNtPreSaveKey:
    case RegNtPreReplaceKey:
        KG_LOG("RegGuard: DENY PID=%lu %s\n", (ULONG)(ULONG_PTR)pid,
            KgRegNotifyClassToString(notifyClass));
        return STATUS_ACCESS_DENIED;
    }

    /* Read / query operations: check against sensitive path blacklist */
    switch (notifyClass)
    {
    case RegNtPreOpenKey:
    {
        if (KgRegCheckPreOpen(arg2))
        {
            KG_LOG("RegGuard: DENY PID=%lu %s (sensitive path)\n", (ULONG)(ULONG_PTR)pid,
                KgRegNotifyClassToString(notifyClass));
            return STATUS_ACCESS_DENIED;
        }
        break;
    }

    case RegNtPreQueryValueKey:
    {
        PREG_QUERY_VALUE_KEY_INFORMATION info = (PREG_QUERY_VALUE_KEY_INFORMATION)arg2;
        if (KgRegCheckObject(info->Object))
            return STATUS_ACCESS_DENIED;
        break;
    }

    case RegNtPreQueryMultipleValueKey:
    {
        PREG_QUERY_MULTIPLE_VALUE_KEY_INFORMATION info = (PREG_QUERY_MULTIPLE_VALUE_KEY_INFORMATION)arg2;
        if (KgRegCheckObject(info->Object))
            return STATUS_ACCESS_DENIED;
        break;
    }

    case RegNtPreEnumerateKey:
    case RegNtPreEnumerateValueKey:
    {
        PREG_ENUMERATE_KEY_INFORMATION info = (PREG_ENUMERATE_KEY_INFORMATION)arg2;
        if (KgRegCheckObject(info->Object))
            return STATUS_ACCESS_DENIED;
        break;
    }

    case RegNtPreQueryKey:
    case RegNtPreQueryKeyName:
    {
        PREG_QUERY_KEY_INFORMATION info = (PREG_QUERY_KEY_INFORMATION)arg2;
        if (KgRegCheckObject(info->Object))
            return STATUS_ACCESS_DENIED;
        break;
    }

    case RegNtPreFlushKey:
    case RegNtPreQueryKeySecurity:
        break;

    default:
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS KgRegisterRegCallbacks(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING altitude;
    RtlInitUnicodeString(&altitude, L"360000");

    NTSTATUS status = CmRegisterCallbackEx(
        KgRegCallback,     // 1. Function
        &altitude,         // 2. Altitude
        DriverObject,      // 3. Driver
        NULL,              // 4. Context
        &gRegCookie,       // 5. Cookie
        NULL               // 6. Reserved
    );

    // NTSTATUS status = CmRegisterCallback(
    //     KgRegCallback,
    //     NULL,
    //     &gRegCookie
    // );

    if (NT_SUCCESS(status)) {
        KG_LOG("RegGuard: Callback registered\n");
    } else {
        KG_LOG("RegGuard: FAILED (status=%08lX)\n", status);
    }

    return status;
}

VOID KgUnregisterRegCallbacks(VOID)
{
    if (gRegCookie.QuadPart != 0) {
        CmUnRegisterCallback(gRegCookie);
        KG_LOG("RegGuard: Callback unregistered\n");
        gRegCookie.QuadPart = 0;
    }
}
