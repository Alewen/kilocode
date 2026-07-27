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
static BYTE*              g_TargetCCI = NULL;
static int                g_HookSizeCCI = 0;

static CoGetClassObjectFn g_OriginalCGO = NULL;
static BYTE*              g_TargetCGO = NULL;
static int                g_HookSizeCGO = 0;

static ShellExecuteExW_Fn g_OriginalSEEW = NULL;
static BYTE*              g_TargetSEEW = NULL;
static int                g_HookSizeSEEW = 0;

static volatile LONG      g_Ready = 0;

// ============================================================================
// x64 instruction length decoder
// Returns the byte length of the instruction starting at addr, or 0 if unknown.
// ============================================================================

static int KgModRmLen(BYTE* p)
{
    BYTE modrm = p[0];
    int mod = (modrm >> 6) & 3;
    int rm = modrm & 7;
    int len = 1;
    if (mod != 3 && rm == 4) len++;
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0 && rm == 5) len += 4;
    return len;
}

static int KgGetInstLen(BYTE* addr)
{
    BYTE b = addr[0];
    // Extract REX prefix first so all subsequent checks see the true opcode
    int rex = 0;
    if (b >= 0x40 && b <= 0x4F) { rex = 1; b = addr[1]; }
    // 1-byte: nop, ret, int3, push/pop reg, inc/dec reg
    if (b == 0x90 || b == 0xCC || b == 0xC3 || b == 0xCB || b == 0xCF) return 1 + rex;
    if (b >= 0x50 && b <= 0x5F) return 1 + rex;
    // leave, pushfq, popfq, clc, stc, cld, std
    if (b == 0xC9 || b == 0x9C || b == 0x9D || b == 0xF8 ||
        b == 0xF9 || b == 0xFC || b == 0xFD) return 1 + rex;
    // ret imm16
    if (b == 0xC2 || b == 0xCA) return 3 + rex;
    // push imm8, push imm32
    if (b == 0x6A) return 2 + rex;
    if (b == 0x68) return 5 + rex;
    // call rel32, jmp rel32
    if (b == 0xE8 || b == 0xE9) return 5 + rex;
    // jcc rel8 (0x70-0x7F)
    if (b >= 0x70 && b <= 0x7F) return 2 + rex;
    // jmp short, call/jmp reg (indirect)
    if (b == 0xEB) return 2 + rex;
    if (b == 0xFF) return 1 + rex + KgModRmLen(addr + 1 + rex);
    // 0F-prefixed two-byte opcodes
    if (b == 0x0F) {
        BYTE b2 = addr[1 + rex];
        // 0F 1F xx = NOP (multi-byte)
        if (b2 == 0x1F) return 1 + rex + KgModRmLen(addr + 1 + rex);
        // 0F 84-8F = jcc rel32
        if (b2 >= 0x80 && b2 <= 0x8F) return 2 + rex + 4;
        // 0F B6, 0F B7, 0F BE, 0F BF = movzx/movsx
        if (b2 == 0xB6 || b2 == 0xB7 || b2 == 0xBE || b2 == 0xBF)
            return 2 + rex + KgModRmLen(addr + 2 + rex);
        // 0F 05 = syscall
        if (b2 == 0x05) return 2 + rex;
        // 0F 31 = rdtsc
        if (b2 == 0x31) return 2 + rex;
        // 0F AE xx = xsave/xrstor/fxsave/fxrstor/clflush/mfence/lfence/sfence
        if (b2 == 0xAE) return 2 + rex + KgModRmLen(addr + 2 + rex);
        return 2 + rex + KgModRmLen(addr + 2 + rex);
    }
    // Common 1-byte+ModRM instructions
    if (b == 0x01 || b == 0x03 || b == 0x09 || b == 0x11 || b == 0x19 ||
        b == 0x21 || b == 0x29 || b == 0x31 || b == 0x39 || b == 0x3B ||
        b == 0x63 || b == 0x85 || b == 0x89 || b == 0x8B || b == 0x8D ||
        b == 0x88 || b == 0x8A)
        return 1 + rex + KgModRmLen(addr + 1 + rex);
    // 83 = opcode /0-/7 with imm8 (sub rsp, imm8; and/or/cmp/add)
    if (b == 0x83) return 1 + rex + KgModRmLen(addr + 1 + rex) + 1;
    // 81 = same with imm32
    if (b == 0x81) return 1 + rex + KgModRmLen(addr + 1 + rex) + 4;
    // C7 = mov r/m, imm32
    if (b == 0xC7) return 1 + rex + KgModRmLen(addr + 1 + rex) + 4;
    // A0-A3 = mov moffs (absolute address)
    if (b == 0xA0 || b == 0xA1 || b == 0xA2 || b == 0xA3) {
        return 1 + rex + (rex ? 8 : 4);
    }
    // B0-BF = mov reg, imm
    if (b >= 0xB0 && b <= 0xB7) return 1 + rex + 1;
    if (b >= 0xB8 && b <= 0xBF) return 1 + rex + (rex ? 8 : 4);
    // al/ax/eax/rax immediate arithmetic
    if (b == 0x04 || b == 0x0C || b == 0x14 || b == 0x1C ||
        b == 0x24 || b == 0x2C || b == 0x34 || b == 0x3C)
        return 1 + rex + 1;
    if (b == 0x05 || b == 0x0D || b == 0x15 || b == 0x1D ||
        b == 0x25 || b == 0x2D || b == 0x35 || b == 0x3D)
        return 1 + rex + 4;
    // 69 = imul r, r/m, imm32
    if (b == 0x69) return 1 + rex + KgModRmLen(addr + 1 + rex) + 4;
    // 6B = imul r, r/m, imm8
    if (b == 0x6B) return 1 + rex + KgModRmLen(addr + 1 + rex) + 1;
    // 8F = pop r/m
    if (b == 0x8F) return 1 + rex + KgModRmLen(addr + 1 + rex);
    // F3/F2/66/67/2E/36/3E/26/64/65/9B — legacy prefixes (recurse)
    static const BYTE prefixes[] = {0xF3, 0xF2, 0x66, 0x67, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x9B};
    for (int i = 0; i < (int)(sizeof(prefixes)/sizeof(prefixes[0])); i++) {
        if (b == prefixes[i]) {
            int rest = KgGetInstLen(addr + 1);
            return rest ? 1 + rest : 0;
        }
    }
    // Unknown
    return 0;
}

// Calculate the minimum number of bytes to cover from 'addr' onwards,
// such that the total >= minBytes and no instruction is split.
// Returns 0 if any instruction cannot be decoded.
static int KgCalcCover(BYTE* addr, int minBytes)
{
    int total = 0;
    while (total < minBytes) {
        int len = KgGetInstLen(addr + total);
        if (len == 0) return 0;
        total += len;
    }
    return total;
}

// ============================================================================
// Hook helpers (x64, absolute JMP via FF 25)
// The hook writes a 14-byte absolute jmp at the target, then NOP-pads
// the remaining bytes up to hookSize. The trampoline copies the original
// hookSize bytes, then jumps back to target + hookSize.
// ============================================================================

static void InstallHook(BYTE* target, void* detour, int hookSize, BYTE* saved, BYTE** trampOut, void** origOut)
{
    DWORD old;
    // Save original prologue
    memcpy(saved, target, hookSize);
    // Allocate trampoline
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, hookSize + 14, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    // Copy original prologue to trampoline
    memcpy(tramp, saved, hookSize);
    // Write jmp from trampoline back to target + hookSize
    VirtualProtect(tramp, hookSize + 14, PAGE_EXECUTE_READWRITE, &old);
    tramp[hookSize + 0] = 0xFF; tramp[hookSize + 1] = 0x25;
    *(DWORD*)(tramp + hookSize + 2) = 0;
    *(void**)(tramp + hookSize + 6) = target + hookSize;
    VirtualProtect(tramp, hookSize + 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, hookSize + 14);
    *trampOut = tramp;
    *origOut = (void*)tramp;
    // Write jmp from target to detour
    VirtualProtect(target, hookSize, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xFF; target[1] = 0x25;
    *(DWORD*)(target + 2) = 0;
    *(void**)(target + 6) = detour;
    for (int i = 14; i < hookSize; i++) target[i] = 0x90;
    VirtualProtect(target, hookSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, hookSize);
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
    if (!hMod) return;

    // ---- Hook CoCreateInstance ----
    CoCreateInstanceFn origCCI = (CoCreateInstanceFn)GetProcAddress(hMod, "CoCreateInstance");
    if (origCCI) {
        g_TargetCCI = (BYTE*)origCCI;
        int hookSize = KgCalcCover(g_TargetCCI, 14);
        if (hookSize > 0 && hookSize <= 24) {
            g_HookSizeCCI = hookSize;
            BYTE saved[24];
            InstallHook(g_TargetCCI, DetourCoCreateInstance, hookSize, saved,
                        (BYTE**)&g_OriginalCCI, (void**)&g_OriginalCCI);
        }
    }

    // ---- Hook CoGetClassObject ----
    CoGetClassObjectFn origCGO = (CoGetClassObjectFn)GetProcAddress(hMod, "CoGetClassObject");
    if (origCGO) {
        g_TargetCGO = (BYTE*)origCGO;
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
        int hookSize = KgCalcCover(g_TargetCGO, 14);
        if (hookSize > 0 && hookSize <= 24) {
            g_HookSizeCGO = hookSize;
            BYTE saved[24];
            InstallHook(g_TargetCGO, DetourCoGetClassObject, hookSize, saved,
                        (BYTE**)&g_OriginalCGO, (void**)&g_OriginalCGO);
        }
    }

    // ---- Hook ShellExecuteExW (shell32.dll) — block UAC runas ----
    {
        HMODULE hShell = GetModuleHandleA("shell32.dll");
        if (hShell) {
            ShellExecuteExW_Fn origSEEW = (ShellExecuteExW_Fn)GetProcAddress(hShell, "ShellExecuteExW");
            if (origSEEW) {
                g_TargetSEEW = (BYTE*)origSEEW;
                int hookSize = KgCalcCover(g_TargetSEEW, 14);
                if (hookSize > 0 && hookSize <= 24) {
                    g_HookSizeSEEW = hookSize;
                    BYTE saved[24];
                    InstallHook(g_TargetSEEW, DetourShellExecuteExW, hookSize, saved,
                                (BYTE**)&g_OriginalSEEW, (void**)&g_OriginalSEEW);
                }
            }
        }
    }

    InterlockedExchange(&g_Ready, 1);

    // Register detour + trampoline addresses with CFG so the FF 25
    // indirect JMP in combase.dll / shell32.dll won't crash on
    // CFG-enabled processes. Each call marks a single code region.
    {
        typedef BOOL (WINAPI *Fn)(HANDLE, PVOID, SIZE_T, ULONG, PULONG);
        HMODULE hK32 = GetModuleHandleA("kernel32.dll");
        if (!hK32) goto done;
        Fn fn = (Fn)GetProcAddress(hK32, "SetProcessValidCallTargets");
        if (!fn) goto done;

        HANDLE self = GetCurrentProcess();

        // Detour entry points — one byte at each function start
        if (g_TargetCCI) fn(self, DetourCoCreateInstance, 1, 0, NULL);
        if (g_TargetCGO) fn(self, DetourCoGetClassObject, 1, 0, NULL);
        if (g_TargetSEEW) fn(self, DetourShellExecuteExW, 1, 0, NULL);

        // Trampolines — VirtualAlloc'd pages, not in DLL .text
        if (g_OriginalCCI && g_HookSizeCCI)
            fn(self, (PVOID)(ULONG_PTR)g_OriginalCCI, g_HookSizeCCI + 14, 0, NULL);
        if (g_OriginalCGO && g_HookSizeCGO)
            fn(self, (PVOID)(ULONG_PTR)g_OriginalCGO, g_HookSizeCGO + 14, 0, NULL);
        if (g_OriginalSEEW && g_HookSizeSEEW)
            fn(self, (PVOID)(ULONG_PTR)g_OriginalSEEW, g_HookSizeSEEW + 14, 0, NULL);
    }
done:;
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