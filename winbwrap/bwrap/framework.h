#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "fltlib.lib")

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <fltUser.h>
#include <unordered_set>
#include <mutex>

// ---------------------------------------------------------------------------
// IOCTL definitions (must match KiloGuard.c)
// ---------------------------------------------------------------------------
#define IOCTL_DEVICE_TYPE         0x00000022
#define IOCTL_METHOD_BUFFERED     0
#define IOCTL_FILE_WRITE          0x0002
#define IOCTL_FILE_READ           0x0001
#define IOCTL_CODE(dev, func, meth, acc) \
    (((dev) << 16) | ((acc) << 14) | ((func) << 2) | (meth))

#define IOCTL_KG_SET_POLICY_BATCH   IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x801, IOCTL_METHOD_BUFFERED, IOCTL_FILE_WRITE)
#define IOCTL_KG_ATTACH_PROCESS     IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x802, IOCTL_METHOD_BUFFERED, IOCTL_FILE_WRITE)
#define IOCTL_KG_CONTROL_SANDBOX    IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x803, IOCTL_METHOD_BUFFERED, IOCTL_FILE_READ | IOCTL_FILE_WRITE)
#define IOCTL_KG_QUERY_STATS        IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x804, IOCTL_METHOD_BUFFERED, IOCTL_FILE_READ)
#define IOCTL_KG_AUTHENTICATE       IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x805, IOCTL_METHOD_BUFFERED, IOCTL_FILE_WRITE)
#define IOCTL_KG_DEAUTHENTICATE     IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x806, IOCTL_METHOD_BUFFERED, IOCTL_FILE_WRITE)
#define IOCTL_KG_QUERY_EVENTS       IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x807, IOCTL_METHOD_BUFFERED, IOCTL_FILE_READ | IOCTL_FILE_WRITE)
#define IOCTL_KG_QUERY_DENY_EVENTS  IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x808, IOCTL_METHOD_BUFFERED, IOCTL_FILE_READ)
#define IOCTL_KG_SET_NET_EXE_LIST   IOCTL_CODE(IOCTL_DEVICE_TYPE, 0x809, IOCTL_METHOD_BUFFERED, IOCTL_FILE_WRITE)

#define KG_MAX_NET_EXE  64

typedef ULONG KG_SANDBOX_ID;
typedef UCHAR KG_SANDBOX_LEVEL;

/* Must match KiloGuard.c _KG_POLICY_RULE_ENTRY layout:
   Sid(4) + Level(1) + pad(3) + Path(2048) = 2056 bytes */
typedef struct {
    KG_SANDBOX_ID Sid;
    KG_SANDBOX_LEVEL Level;
    WCHAR Path[1024];
} KG_POLICY_RULE_ENTRY;

typedef struct {
    ULONG RuleCount;
    KG_POLICY_RULE_ENTRY Rules[1];
} KG_POLICY_BATCH_INPUT;

typedef struct {
    HANDLE Pid;
    KG_SANDBOX_ID Sid;
    BOOLEAN Detach;
} KG_ATTACH_PROCESS_INPUT;

typedef struct {
    BOOLEAN Destroy;
    KG_SANDBOX_ID Sid;
} KG_CONTROL_SANDBOX;

typedef struct {
    ULONG Pid;
    WCHAR ImageName[256];
    WCHAR DllPath[256];
    LARGE_INTEGER Timestamp;
} KG_FAILURE_EVENT;

typedef struct {
    KG_SANDBOX_ID Sid;
} KG_QUERY_EVENTS_INPUT;

#define KG_EVENT_BATCH_SIZE 32

typedef struct {
    ULONG EventCount;
    KG_FAILURE_EVENT Events[KG_EVENT_BATCH_SIZE];
} KG_QUERY_EVENTS_OUTPUT;

#define KG_DENY_EVENT_BATCH_SIZE 32

typedef struct {
    ULONG Pid;
    WCHAR ProcessName[32];
    WCHAR FilePath[260];
    LARGE_INTEGER Timestamp;
    ULONG Flags;    // 0=file deny, 1=net deny
} KG_DENY_EVENT;

typedef struct {
    ULONG EventCount;
    KG_DENY_EVENT Events[KG_DENY_EVENT_BATCH_SIZE];
} KG_QUERY_DENY_EVENTS_OUTPUT;

typedef struct {
    KG_SANDBOX_ID Sid;
    ULONG Count;
    WCHAR Paths[KG_MAX_NET_EXE][260];
} KG_NET_EXE_LIST_INPUT;

// ---------------------------------------------------------------------------
// Process notification port message (must match Domain.h in KiloGuardEx)
// ---------------------------------------------------------------------------
#define KG_PORT_MSG_PROCESS_CREATE     1
#define KG_PORT_MSG_PROCESS_EXIT       2
#define KG_PORT_MSG_READY_FOR_INJECT   3

#pragma pack(push, 1)
typedef struct _KG_PORT_MESSAGE {
    ULONG MsgType;
    ULONG Pid;
    ULONG SID;
    WCHAR ImageName[260];
} KG_PORT_MESSAGE;
#pragma pack(pop)

extern bool g_showConsole;
extern bool g_noInject;

void Wprintln(const std::wstring& s);

// ---------------------------------------------------------------------------
// Path conversion: Win32 → NT (\Device\HarddiskVolumeX\...)
// ---------------------------------------------------------------------------
bool Win32ToNtPath(const std::wstring& win32, std::wstring& nt);

// ---------------------------------------------------------------------------
// NT path → Win32 path conversion (e.g. \Device\HarddiskVolume3\foo → C:\foo)
// ---------------------------------------------------------------------------
std::wstring NtPathToWin32(const std::wstring& ntPath);

// ---------------------------------------------------------------------------
// Check if a command-line tool exists via "where" command, return full path
// ---------------------------------------------------------------------------
bool KgFindTool(const wchar_t* tool, std::wstring& outPath);

// ---------------------------------------------------------------------------
// Auto-detect VCS metadata directories (.svn / .git) for all bound paths
// Walk up parent directories to find the VCS root.
// Only runs if the corresponding VCS tool (svn/git) exists on the system.
// ---------------------------------------------------------------------------
void KgAutoProtectVcs(std::vector<std::wstring>& roList, const std::vector<std::wstring>& rwList);

// ---------------------------------------------------------------------------
// Build a CreateProcess-compatible command line from exe + args
// ---------------------------------------------------------------------------
std::wstring BuildCmdLine(const std::wstring& exe, wchar_t* const* args, int argCount);

// ---------------------------------------------------------------------------
// Log invocation to file when --ses is used
// ---------------------------------------------------------------------------
void LogSession(const std::wstring& sid, int argc, wchar_t* argv[]);
