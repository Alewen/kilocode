// KiloHook.c - COM + UAC interceptor DLL for KiloGuard sandbox.
// Hooks CoCreateInstance/CoGetClassObject in combase.dll to block WMI/SCM
// object creation, and ShellExecuteExW in shell32.dll to block UAC elevation.

#include <windows.h>
#include <shellapi.h>

// ============================================================================
// Blocked CLSIDs (COM class identifiers)
// ============================================================================

// WbemLocator:           {4590F811-1D3A-11D0-891F-00AA004B2E24}
// WbemAdministrativeLoc: {8BC3F05E-D86B-11D0-A075-00C04FB68820}
// SWbemLocator:          {76A64158-CB41-11D1-8B02-00600806D9B6}
// WScript.Shell:         {72C24DD5-D70A-438B-8A42-98424B88AFB8}
// Shell.Application:     {13709620-C279-11CE-A49E-444553540000}
// Schedule.Service:      {0F87369F-A4E5-4CFC-BD3E-73E6154572DD}

static const GUID kClsidWmiLocator =
    {0x4590F811, 0x1D3A, 0x11D0, {0x89,0x1F,0x00,0xAA,0x00,0x4B,0x2E,0x24}};

static const GUID kClsidWmiAdminLocator =
    {0x8BC3F05E, 0xD86B, 0x11D0, {0xA0,0x75,0x00,0xC0,0x4F,0xB6,0x88,0x20}};

static const GUID kClsidSWbemLocator =
    {0x76A64158, 0xCB41, 0x11D1, {0x8B,0x02,0x00,0x60,0x08,0x06,0xD9,0xB6}};

static const GUID kClsidWScriptShell =
    {0x72C24DD5, 0xD70A, 0x438B, {0x8A,0x42,0x98,0x42,0x4B,0x88,0xAF,0xB8}};

static const GUID kClsidShellApp =
    {0x13709620, 0xC279, 0x11CE, {0xA4,0x9E,0x44,0x45,0x53,0x54,0x00,0x00}};

static const GUID kClsidTaskScheduler =
    {0x0F87369F, 0xA4E5, 0x4CFC, {0xBD,0x3E,0x73,0xE6,0x15,0x45,0x72,0xDD}};

// SCM does not use CoCreateInstance for service creation.
// pwsh's Start-Service / Restart-Service go through SCM RPC, not COM.

// ============================================================================
// CoCreateInstance signature
// ============================================================================
typedef HRESULT (WINAPI *CoCreateInstanceFn)(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID *ppv);

typedef HRESULT (WINAPI *CoGetClassObjectFn)(
    REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID *ppv);

typedef BOOL (WINAPI *ShellExecuteExW_Fn)(LPSHELLEXECUTEINFOW pExecInfo);

// ============================================================================
// Hook globals
// ============================================================================

static CoCreateInstanceFn g_OriginalCCI = NULL;
static BYTE               g_SavedCCI[24];
static BYTE*              g_TargetCCI = NULL;

static CoGetClassObjectFn g_OriginalCGO = NULL;
static BYTE               g_SavedCGO[24];
static BYTE*              g_TargetCGO = NULL;

static ShellExecuteExW_Fn g_OriginalSEEW = NULL;
static BYTE               g_SavedSEEW[24];
static BYTE*              g_TargetSEEW = NULL;

static volatile LONG      g_Ready = 0;

// ============================================================================
// Hook helpers (x64, absolute JMP via FF 25 + padding to reach boundary)
// ============================================================================

static void WriteHook18(BYTE* addr, void* dest)
{
    DWORD old;
    VirtualProtect(addr, 20, PAGE_EXECUTE_READWRITE, &old);
    // FF 25 00 00 00 00 <8-byte absolute address> = 14 bytes
    addr[0] = 0xFF; addr[1] = 0x25;
    *(DWORD*)(addr + 2) = 0;
    *(void**)(addr + 6) = dest;
    // NOP padding to reach 18-byte boundary
    addr[14] = 0x90; addr[15] = 0x90;
    addr[16] = 0x90; addr[17] = 0x90;
    VirtualProtect(addr, 20, old, &old);
}

static void WriteHookN(BYTE* addr, void* dest, int nBytes)
{
    DWORD old;
    VirtualProtect(addr, nBytes, PAGE_EXECUTE_READWRITE, &old);
    addr[0] = 0xFF; addr[1] = 0x25;
    *(DWORD*)(addr + 2) = 0;
    *(void**)(addr + 6) = dest;
    for (int i = 14; i < nBytes; i++) addr[i] = 0x90;
    VirtualProtect(addr, nBytes, old, &old);
}

static void WriteTrampJmp(BYTE* where, void* dest)
{
    DWORD old;
    VirtualProtect(where, 14, PAGE_EXECUTE_READWRITE, &old);
    where[0] = 0xFF; where[1] = 0x25;
    *(DWORD*)(where + 2) = 0;
    *(void**)(where + 6) = dest;
    VirtualProtect(where, 14, old, &old);
}

// ============================================================================
// GUID comparison
// ============================================================================

static BOOL GuidEq(REFGUID a, REFGUID b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static BOOL IsBlockedClsid(REFCLSID rclsid)
{
    if (!rclsid) return FALSE;
    return GuidEq(rclsid, &kClsidWmiLocator)
        || GuidEq(rclsid, &kClsidWmiAdminLocator)
        || GuidEq(rclsid, &kClsidSWbemLocator)
        || GuidEq(rclsid, &kClsidWScriptShell)
        || GuidEq(rclsid, &kClsidShellApp)
        || GuidEq(rclsid, &kClsidTaskScheduler);
}

// ============================================================================
// Detour function
// ============================================================================

static HRESULT WINAPI DetourCoCreateInstance(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID *ppv)
{
    if (g_Ready && rclsid) {
        WCHAR buf[128];
        int blocked = IsBlockedClsid(rclsid);
        wsprintfW(buf, L"KiloHook: CCI CLSID=%08X %04X %04X %02X%02X %02X%02X%02X%02X%02X%02X blocked=%d\n",
            rclsid->Data1, rclsid->Data2, rclsid->Data3,
            rclsid->Data4[0], rclsid->Data4[1], rclsid->Data4[2], rclsid->Data4[3],
            rclsid->Data4[4], rclsid->Data4[5], rclsid->Data4[6], rclsid->Data4[7],
            blocked);
        OutputDebugStringW(buf);
    }
    if (g_Ready && IsBlockedClsid(rclsid))
        return REGDB_E_CLASSNOTREG;
    return g_OriginalCCI(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

static HRESULT WINAPI DetourCoGetClassObject(
    REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID *ppv)
{
    if (g_Ready && rclsid) {
        WCHAR buf[128];
        int blocked = IsBlockedClsid(rclsid);
        wsprintfW(buf, L"KiloHook: CGO CLSID=%08X %04X %04X %02X%02X %02X%02X%02X%02X%02X%02X blocked=%d\n",
            rclsid->Data1, rclsid->Data2, rclsid->Data3,
            rclsid->Data4[0], rclsid->Data4[1], rclsid->Data4[2], rclsid->Data4[3],
            rclsid->Data4[4], rclsid->Data4[5], rclsid->Data4[6], rclsid->Data4[7],
            blocked);
        OutputDebugStringW(buf);
    }
    if (g_Ready && IsBlockedClsid(rclsid))
        return REGDB_E_CLASSNOTREG;
    return g_OriginalCGO(rclsid, dwClsContext, pvReserved, riid, ppv);
}

// ============================================================================
// DetourShellExecuteExW — block UAC elevation
// ============================================================================

static BOOL WINAPI DetourShellExecuteExW(LPSHELLEXECUTEINFOW pExecInfo)
{
    if (g_Ready && pExecInfo && pExecInfo->lpVerb &&
        _wcsicmp(pExecInfo->lpVerb, L"runas") == 0) {
        OutputDebugStringW(L"KiloHook: ShellExecuteExW runas BLOCKED\n");
        SetLastError(ERROR_CANCELLED);
        return FALSE;
    }
    return g_OriginalSEEW(pExecInfo);
}

// ============================================================================
// Install hook
// ============================================================================

static void Install(void)
{
    HMODULE hMod = GetModuleHandleA("combase.dll");
    if (!hMod) hMod = LoadLibraryA("combase.dll");
    if (!hMod) return;

    // ---- Hook CoCreateInstance ----
    CoCreateInstanceFn origCCI = (CoCreateInstanceFn)GetProcAddress(hMod, "CoCreateInstance");
    if (origCCI) {
        g_TargetCCI = (BYTE*)origCCI;
        memcpy(g_SavedCCI, g_TargetCCI, 24);
        BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (tramp) {
            memcpy(tramp, g_SavedCCI, 24);
            WriteTrampJmp(tramp + 18, g_TargetCCI + 18);
            g_OriginalCCI = (CoCreateInstanceFn)tramp;
            WriteHook18(g_TargetCCI, DetourCoCreateInstance);
            FlushInstructionCache(GetCurrentProcess(), g_TargetCCI, 20);
        }
    }

    // ---- Hook CoGetClassObject ----
    CoGetClassObjectFn origCGO = (CoGetClassObjectFn)GetProcAddress(hMod, "CoGetClassObject");
    if (origCGO) {
        g_TargetCGO = (BYTE*)origCGO;
        // Dump prologue for analysis
        {
            WCHAR d[256];
            wsprintfW(d, L"KiloHook: CGO prologue: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                g_TargetCGO[0], g_TargetCGO[1], g_TargetCGO[2], g_TargetCGO[3],
                g_TargetCGO[4], g_TargetCGO[5], g_TargetCGO[6], g_TargetCGO[7],
                g_TargetCGO[8], g_TargetCGO[9], g_TargetCGO[10], g_TargetCGO[11],
                g_TargetCGO[12], g_TargetCGO[13], g_TargetCGO[14], g_TargetCGO[15],
                g_TargetCGO[16], g_TargetCGO[17], g_TargetCGO[18], g_TargetCGO[19],
                g_TargetCGO[20], g_TargetCGO[21], g_TargetCGO[22], g_TargetCGO[23]);
            OutputDebugStringW(d);
        }

        memcpy(g_SavedCGO, g_TargetCGO, 24);
        BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (tramp) {
            memcpy(tramp, g_SavedCGO, 24);
            WriteTrampJmp(tramp + 19, g_TargetCGO + 19);
            g_OriginalCGO = (CoGetClassObjectFn)tramp;
            WriteHookN(g_TargetCGO, DetourCoGetClassObject, 19);
            FlushInstructionCache(GetCurrentProcess(), g_TargetCGO, 20);
        }
    }

    // ---- Hook ShellExecuteExW (shell32.dll) — block UAC runas ----
    {
        HMODULE hShell = GetModuleHandleA("shell32.dll");
        if (hShell) {
            ShellExecuteExW_Fn origSEEW = (ShellExecuteExW_Fn)GetProcAddress(hShell, "ShellExecuteExW");
            if (origSEEW) {
                g_TargetSEEW = (BYTE*)origSEEW;
                memcpy(g_SavedSEEW, g_TargetSEEW, 24);
                BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (tramp) {
                    memcpy(tramp, g_SavedSEEW, 24);
                    WriteTrampJmp(tramp + 14, g_TargetSEEW + 14);
                    g_OriginalSEEW = (ShellExecuteExW_Fn)tramp;
                    WriteHookN(g_TargetSEEW, DetourShellExecuteExW, 14);
                    FlushInstructionCache(GetCurrentProcess(), g_TargetSEEW, 20);
                }
            }
        }
    }

    InterlockedExchange(&g_Ready, 1);
}

// ============================================================================
// DLL entry point
// ============================================================================

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        Install();
    }
    return TRUE;
}
