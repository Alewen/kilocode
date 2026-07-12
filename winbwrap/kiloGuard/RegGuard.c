#include "RegGuard.h"
#include "Domain.h"

static LARGE_INTEGER gRegCookie = { 0 };

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

static NTSTATUS
KgRegCallback(PVOID context, PVOID arg1, PVOID arg2)
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
            KG_LOG("RegGuard: DENY PID=%lu %s Key=%wZ\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass),
                     info->CompleteName);
        else
            KG_LOG("RegGuard: DENY PID=%lu %s\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass));
        return STATUS_ACCESS_DENIED;
    }
    case RegNtPreSetValueKey:
    {
        REG_SET_VALUE_KEY_INFORMATION* info = (REG_SET_VALUE_KEY_INFORMATION*)arg2;
        if (info->ValueName && info->ValueName->Buffer)
            KG_LOG("RegGuard: DENY PID=%lu %s Value=%wZ\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass),
                     info->ValueName);
        else
            KG_LOG("RegGuard: DENY PID=%lu %s\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass));
        return STATUS_ACCESS_DENIED;
    }
    case RegNtPreDeleteValueKey:
    {
        REG_DELETE_VALUE_KEY_INFORMATION* info = (REG_DELETE_VALUE_KEY_INFORMATION*)arg2;
        if (info->ValueName && info->ValueName->Buffer)
            KG_LOG("RegGuard: DENY PID=%lu %s Value=%wZ\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass),
                     info->ValueName);
        else
            KG_LOG("RegGuard: DENY PID=%lu %s\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass));
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
        KG_LOG("RegGuard: DENY PID=%lu %s\n",
                 (ULONG)(ULONG_PTR)pid,
                 KgRegNotifyClassToString(notifyClass));
        return STATUS_ACCESS_DENIED;
    }

    /* Read / query operations: allow, with log */
    switch (notifyClass)
    {
    case RegNtPreOpenKey:
    {
        REG_CREATE_KEY_INFORMATION* info = (REG_CREATE_KEY_INFORMATION*)arg2;
        if (info->CompleteName && info->CompleteName->Buffer) {
            /* Block COM CLSID reads to prevent CoCreateInstance-based escapes */
            if (wcsstr(info->CompleteName->Buffer, L"CLSID\\{")) {
                KG_LOG("RegGuard: DENY PID=%lu %s Key=%wZ (COM CLSID)\n",
                         (ULONG)(ULONG_PTR)pid,
                         KgRegNotifyClassToString(notifyClass),
                         info->CompleteName);
                return STATUS_ACCESS_DENIED;
            }
            /*KG_LOG("RegGuard: PID=%lu %s Key=%wZ\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass),
                     info->CompleteName);*/
        } /* else {
            KG_LOG("RegGuard: PID=%lu %s\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass));
        } */
        break;
    }

    case RegNtPreQueryValueKey:
    {
        //REG_QUERY_VALUE_KEY_INFORMATION* info = (REG_QUERY_VALUE_KEY_INFORMATION*)arg2;
        /*if (info->ValueName && info->ValueName->Buffer)
            KG_LOG("RegGuard: PID=%lu %s Value=%wZ\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass),
                     info->ValueName);
        else
            KG_LOG("RegGuard: PID=%lu %s\n",
                     (ULONG)(ULONG_PTR)pid,
                     KgRegNotifyClassToString(notifyClass));*/
        break;
    }

    case RegNtPreEnumerateValueKey:
    case RegNtPreEnumerateKey:
    case RegNtPreQueryKey:
    case RegNtPreQueryMultipleValueKey:
    case RegNtPreQueryKeyName:
    case RegNtPreFlushKey:
    case RegNtPreQueryKeySecurity:
        /*KG_LOG("RegGuard: PID=%lu %s\n",
                 (ULONG)(ULONG_PTR)pid,
                 KgRegNotifyClassToString(notifyClass));*/
        break;

    default:
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS KgRegisterRegCallbacks(VOID)
{
    NTSTATUS status = CmRegisterCallback(
        KgRegCallback,
        NULL,
        &gRegCookie
    );

    if (NT_SUCCESS(status)) {
        DbgPrint("RegGuard: Callback registered\n");
    } else {
        DbgPrint("RegGuard: FAILED (status=%08lX)\n", status);
    }

    return status;
}

VOID KgUnregisterRegCallbacks(VOID)
{
    if (gRegCookie.QuadPart != 0) {
        CmUnRegisterCallback(gRegCookie);
        DbgPrint("RegGuard: Callback unregistered\n");
        gRegCookie.QuadPart = 0;
    }
}
