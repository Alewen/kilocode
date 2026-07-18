#include "framework.h"
#include "bwrap_secret.h"
#include "AppX.h"

#include <fltUser.h>
#include <unordered_set>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------------------------
// NT path → Win32 path conversion (e.g. \Device\HarddiskVolume3\foo → C:\foo)
// ---------------------------------------------------------------------------
static std::wstring NtPathToWin32(const std::wstring& ntPath)
{
    if (ntPath.empty()) return ntPath;

    WCHAR drives[512];
    if (!GetLogicalDriveStringsW(ARRAYSIZE(drives), drives))
        return ntPath;

    for (const WCHAR* d = drives; *d; d += wcslen(d) + 1) {
        std::wstring drive = d;
        if (drive.size() >= 2 && drive[1] == L':')
            drive = drive.substr(0, 2);

        WCHAR target[256] = {};
        if (QueryDosDeviceW(drive.c_str(), target, ARRAYSIZE(target))) {
            std::wstring ntDev(target);
            if (ntPath.size() > ntDev.size() &&
                _wcsnicmp(ntPath.c_str(), ntDev.c_str(), ntDev.size()) == 0 &&
                ntPath[ntDev.size()] == L'\\') {
                return drive + L"\\" + ntPath.substr(ntDev.size() + 1);
            }
        }
    }
    return ntPath;
}

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

// ---------------------------------------------------------------------------
// Error helper
// ---------------------------------------------------------------------------
[[noreturn]] static void Die(const char* msg) {
    std::cerr << "bwrap.exe: " << msg << " (error "
              << GetLastError() << ")" << std::endl;
    ExitProcess(GetLastError() ? GetLastError() : 1);
}

static bool g_showConsole = false;
static bool g_noInject = false;

static void Wprintln(const std::wstring& s) {
    if (!g_showConsole) return;
    std::wcout << s;
    std::wcout.flush();
}

// ---------------------------------------------------------------------------
// Path conversion: Win32 → NT (\Device\HarddiskVolumeX\...)
// ---------------------------------------------------------------------------
static bool Win32ToNtPath(const std::wstring& win32, std::wstring& nt) {
    WCHAR full[1024];
    DWORD flen = GetFullPathNameW(win32.c_str(), 1024, full, NULL);
    if (flen == 0 || flen >= 1024) return false;

    std::wstring norm = full;
    if (norm.size() > 3 && norm.back() == L'\\')
        norm.pop_back();

    WCHAR volume[64];
    if (!GetVolumePathNameW(norm.c_str(), volume, 64))
        return false;

    std::wstring dosName = volume;
    if (!dosName.empty() && dosName.back() == L'\\')
        dosName.pop_back();

    WCHAR device[1024];
    if (QueryDosDeviceW(dosName.c_str(), device, 1024) == 0)
        return false;

    size_t volLen = dosName.length();
    nt = device;
    if (norm.size() > volLen) {
        nt += norm.c_str() + volLen;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Driver handle wrapper
// ---------------------------------------------------------------------------

class DriverClient {
    HANDLE m_h;
    KG_SANDBOX_ID m_sid;
    HANDLE m_notifyThread;
    HANDLE m_stopEvent;
    HANDLE m_portHandle;
    HANDLE m_portThread;
    std::unordered_set<DWORD> m_pids;
    std::mutex m_pidMutex;
    std::wstring m_hookDllPath;

public:
    DriverClient() : m_h(INVALID_HANDLE_VALUE), m_sid(0),
                     m_notifyThread(NULL), m_stopEvent(NULL),
                     m_denyThread(NULL), m_denyStopEvent(NULL),
                     m_portHandle(NULL), m_portThread(NULL) {}

    void SetHookDllPath(const std::wstring& p) { m_hookDllPath = p; }

    ~DriverClient() {
        StopDenyThread();
        StopNotifyThread();
        DisconnectPort();
        if (m_h != INVALID_HANDLE_VALUE) {
            DestroySandbox();
            Deauthenticate();
            CloseHandle(m_h);
        }
    }

    bool IsPidInSandbox(DWORD pid) {
        std::lock_guard<std::mutex> lock(m_pidMutex);
        return m_pids.find(pid) != m_pids.end();
    }

    void AddPid(DWORD pid) {
        std::lock_guard<std::mutex> lock(m_pidMutex);
        m_pids.insert(pid);
        WCHAR buf[128];
        int len = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Create PID=%lu (initial)\n", m_sid, pid);
        if (len > 0) Wprintln(buf);
    }

    void Open() {
        m_h = CreateFileW(
            L"\\\\.\\KiloGuard",
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (m_h == INVALID_HANDLE_VALUE)
            Die("Cannot open \\\\.\\KiloGuard (driver not loaded?)");
    }

    void Authenticate() {
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_AUTHENTICATE,
                (LPVOID)gExpectedToken, BW_TOKEN_SIZE,
                NULL, 0, &junk, NULL))
            Die("IOCTL_KG_AUTHENTICATE failed (token mismatch?)");
    }

    void Deauthenticate() {
        DWORD junk;
        DeviceIoControl(m_h, IOCTL_KG_DEAUTHENTICATE,
            NULL, 0, NULL, 0, &junk, NULL);
    }

    KG_SANDBOX_ID CreateSandbox() {
        KG_CONTROL_SANDBOX in = {};
        in.Destroy = FALSE;
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_CONTROL_SANDBOX,
                &in, sizeof(in), &in, sizeof(in), &junk, NULL))
            Die("IOCTL_KG_CONTROL_SANDBOX (create) failed");
        m_sid = in.Sid;
        return m_sid;
    }

    void DestroySandbox() {
        if (m_sid == 0) return;
        KG_CONTROL_SANDBOX in = {};
        in.Destroy = TRUE;
        in.Sid = m_sid;
        DWORD junk;
        DeviceIoControl(m_h, IOCTL_KG_CONTROL_SANDBOX,
            &in, sizeof(in), NULL, 0, &junk, NULL);
        m_sid = 0;
    }

    void SetNetExeList(const std::vector<std::wstring>& ntPaths) {
        if (ntPaths.empty()) return;
        ULONG count = (ULONG)ntPaths.size();
        if (count > KG_MAX_NET_EXE) count = KG_MAX_NET_EXE;
        size_t bufSize = sizeof(KG_NET_EXE_LIST_INPUT);
        auto buf = std::make_unique<char[]>(bufSize);
        auto in = reinterpret_cast<KG_NET_EXE_LIST_INPUT*>(buf.get());
        in->Sid = m_sid;
        in->Count = count;
        for (ULONG i = 0; i < count; i++)
            wcsncpy_s(in->Paths[i], 260, ntPaths[i].c_str(), _TRUNCATE);
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_SET_NET_EXE_LIST,
                in, (DWORD)bufSize, NULL, 0, &junk, NULL))
            Die("IOCTL_KG_SET_NET_EXE_LIST failed");
    }

    void AddRules(const std::vector<KG_POLICY_RULE_ENTRY>& rules) {
        if (rules.empty()) return;
        size_t bufSize = sizeof(KG_POLICY_BATCH_INPUT) +
                         (rules.size() - 1) * sizeof(KG_POLICY_RULE_ENTRY);
        auto buf = std::make_unique<char[]>(bufSize);
        auto batch = reinterpret_cast<KG_POLICY_BATCH_INPUT*>(buf.get());
        batch->RuleCount = (ULONG)rules.size();
        for (size_t i = 0; i < rules.size(); i++)
            batch->Rules[i] = rules[i];
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_SET_POLICY_BATCH,
                batch, (DWORD)bufSize, NULL, 0, &junk, NULL))
            Die("IOCTL_KG_SET_POLICY_BATCH failed");
    }

    void AttachProcess(DWORD pid, KG_SANDBOX_ID sid) {
        KG_ATTACH_PROCESS_INPUT in;
        in.Pid = (HANDLE)(ULONG_PTR)pid;
        in.Sid = sid;
        in.Detach = FALSE;
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_ATTACH_PROCESS,
                &in, sizeof(in), NULL, 0, &junk, NULL))
            Die("IOCTL_KG_ATTACH_PROCESS failed");
    }

    void QueryDenyEvents(KG_QUERY_DENY_EVENTS_OUTPUT& out) {
        KG_QUERY_EVENTS_INPUT in;
        in.Sid = m_sid;
        DWORD junk;
        DeviceIoControl(m_h, IOCTL_KG_QUERY_DENY_EVENTS,
            &in, sizeof(in), &out, sizeof(out), &junk, NULL);
    }

    void DrainDenyEvents() {
        for (;;) {
            KG_QUERY_DENY_EVENTS_OUTPUT out = {};
            QueryDenyEvents(out);
            if (out.EventCount == 0) break;
            for (ULONG i = 0; i < out.EventCount; i++) {
                WCHAR line[512];
                int len;
                if (out.Events[i].Flags == 1)
                    len = swprintf_s(line, ARRAYSIZE(line),
                        L"bwrap.exe: DENY PID=%lu ProcessName=%s Net=%s\n",
                        out.Events[i].Pid,
                        out.Events[i].ProcessName,
                        out.Events[i].FilePath);
                else
                    len = swprintf_s(line, ARRAYSIZE(line),
                        L"bwrap.exe: DENY PID=%lu ProcessName=%s File=%s\n",
                        out.Events[i].Pid,
                        out.Events[i].ProcessName,
                        out.Events[i].FilePath);
                if (len > 0) Wprintln(line);
            }
        }
    }

    void StartDenyThread() {
        m_denyStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        m_denyThread = CreateThread(NULL, 0, DenyThreadProc, this, 0, NULL);
    }

    void StopDenyThread() {
        if (m_denyStopEvent) {
            SetEvent(m_denyStopEvent);
            if (m_denyThread) {
                WaitForSingleObject(m_denyThread, 5000);
                CloseHandle(m_denyThread);
                m_denyThread = NULL;
            }
            CloseHandle(m_denyStopEvent);
            m_denyStopEvent = NULL;
        }
    }

    void QueryEvents(KG_QUERY_EVENTS_OUTPUT& out) {
        KG_QUERY_EVENTS_INPUT in;
        in.Sid = m_sid;
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_QUERY_EVENTS,
                &in, sizeof(in), &out, sizeof(out), &junk, NULL))
            return;
    }

    void DrainEvents() {
        for (;;) {
            KG_QUERY_EVENTS_OUTPUT out = {};
            QueryEvents(out);
            if (out.EventCount == 0) break;
            for (ULONG i = 0; i < out.EventCount; i++) {
                std::wstring winPath = NtPathToWin32(out.Events[i].ImageName);
                std::wcerr << L"PID " << out.Events[i].Pid
                           << L" : " << winPath
                           << L" (DLL not found: " << out.Events[i].DllPath
                           << L")" << std::endl;
            }
        }
    }

    void StartNotifyThread() {
        m_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        m_notifyThread = CreateThread(NULL, 0, NotifyThreadProc, this, 0, NULL);
        ConnectNotificationPort();
    }

    void StopNotifyThread() {
        if (m_stopEvent) {
            SetEvent(m_stopEvent);
            if (m_notifyThread) {
                WaitForSingleObject(m_notifyThread, 5000);
                CloseHandle(m_notifyThread);
                m_notifyThread = NULL;
            }
            CloseHandle(m_stopEvent);
            m_stopEvent = NULL;
        }
    }

    void ConnectNotificationPort() {
        if (m_portHandle) return;
        HRESULT hr = FilterConnectCommunicationPort(
            L"\\KiloGuardPort", 0, &m_sid, sizeof(m_sid),
            NULL, &m_portHandle);
        if (FAILED(hr)) {
            std::wcerr << L"bwrap.exe: Port connect failed ("
                       << std::hex << hr << std::dec << L")\n";
            m_portHandle = NULL;
            return;
        }
        WCHAR buf[128];
        int len = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Notification port connected SID=%lu\n", m_sid);
        if (len > 0) Wprintln(buf);
        m_portThread = CreateThread(NULL, 0, PortThreadProc, this, 0, NULL);
    }

    void DisconnectPort() {
        if (m_portHandle) {
            CloseHandle(m_portHandle);
            m_portHandle = NULL;
        }
        if (m_portThread) {
            WaitForSingleObject(m_portThread, 5000);
            CloseHandle(m_portThread);
            m_portThread = NULL;
        }
    }

    static DWORD WINAPI PortThreadProc(LPVOID param) {
        DriverClient* self = (DriverClient*)param;
        if (!self->m_portHandle) return 0;

        BYTE buffer[sizeof(FILTER_MESSAGE_HEADER) + sizeof(KG_PORT_MESSAGE)];
        FILTER_MESSAGE_HEADER* hdr = (FILTER_MESSAGE_HEADER*)buffer;

        WCHAR buf[128];
        int len = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Port thread started\n", self->m_sid);
        if (len > 0) Wprintln(buf);
        while (true) {
            HRESULT hr = FilterGetMessage(self->m_portHandle, hdr,
                sizeof(buffer), NULL);
            if (FAILED(hr)) break;

            KG_PORT_MESSAGE* msg = (KG_PORT_MESSAGE*)(hdr + 1);
            {
                std::lock_guard<std::mutex> lock(self->m_pidMutex);
                if (msg->MsgType == KG_PORT_MSG_PROCESS_CREATE) {
                    self->m_pids.insert(msg->Pid);
                    std::wstring winPath = NtPathToWin32(msg->ImageName);
                    WCHAR buf[512];
                    int blen = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Create PID=%lu ProcName=%s\n", msg->SID, msg->Pid, winPath.c_str());
                    if (blen > 0) Wprintln(buf);

                } else if (msg->MsgType == KG_PORT_MSG_READY_FOR_INJECT) {
                    if (!self->m_hookDllPath.empty()) {
                        if (g_noInject) {
                            WCHAR sbuf[128];
                            int slen = swprintf_s(sbuf, ARRAYSIZE(sbuf), L"bwrap.exe: Sandbox SID=%lu Skip Inject PID=%lu (--no-inject)\n", self->m_sid, msg->Pid);
                            if (slen > 0) Wprintln(sbuf);
                        } else {
                            WCHAR ibuf[128];
                            int ilen = swprintf_s(ibuf, ARRAYSIZE(ibuf), L"bwrap.exe: Sandbox SID=%lu Inject PID=%lu (ready)\n", self->m_sid, msg->Pid);
                            if (ilen > 0) Wprintln(ibuf);

                            HANDLE hProc = OpenProcess(
                                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                FALSE, msg->Pid);
                            if (hProc) {
                                size_t cbBuf = (self->m_hookDllPath.size() + 1) * sizeof(WCHAR);
                                LPVOID pRemote = VirtualAllocEx(hProc, NULL, cbBuf,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                                if (pRemote) {
                                    WriteProcessMemory(hProc, pRemote,
                                        self->m_hookDllPath.c_str(), cbBuf, NULL);
                                    HANDLE hTh = CreateRemoteThread(hProc, NULL, 0,
                                        (LPTHREAD_START_ROUTINE)GetProcAddress(
                                            GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
                                        pRemote, 0, NULL);
                                    if (hTh) {
                                        WaitForSingleObject(hTh, 5000);
                                        CloseHandle(hTh);
                                    }
                                    VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
                                }
                                CloseHandle(hProc);
                            }
                            WCHAR dibuf[128];
                            int dilen = swprintf_s(dibuf, ARRAYSIZE(dibuf), L"bwrap.exe: Sandbox SID=%lu Inject Done PID=%lu\n", self->m_sid, msg->Pid);
                            if (dilen > 0) Wprintln(dibuf);
                        }
                    }
                } else if (msg->MsgType == KG_PORT_MSG_PROCESS_EXIT) {
                    self->m_pids.erase(msg->Pid);
                    WCHAR ebuf[128];
                    int elen = swprintf_s(ebuf, ARRAYSIZE(ebuf), L"bwrap.exe: Sandbox SID=%lu Exit PID=%lu\n", msg->SID, msg->Pid);
                    if (elen > 0) Wprintln(ebuf);
                }
            }
        }
        return 0;
    }

private:
    HANDLE m_denyThread;
    HANDLE m_denyStopEvent;

    static DWORD WINAPI NotifyThreadProc(LPVOID param) {
        DriverClient* self = (DriverClient*)param;
        self->DrainEvents();
        while (WaitForSingleObject(self->m_stopEvent, 500) == WAIT_TIMEOUT) {
            self->DrainEvents();
        }
        return 0;
    }

    static DWORD WINAPI DenyThreadProc(LPVOID param) {
        DriverClient* self = (DriverClient*)param;
        self->DrainDenyEvents();
        while (WaitForSingleObject(self->m_denyStopEvent, 100) == WAIT_TIMEOUT) {
            self->DrainDenyEvents();
        }
        self->DrainDenyEvents();
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Check if a command-line tool exists via "where" command, return full path
// ---------------------------------------------------------------------------
static bool KgFindTool(const wchar_t* tool, std::wstring& outPath) {
    std::wstring cmd = std::wstring(L"cmd.exe /c where ") + tool + L" 2>nul";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 4096)) return false;

    PROCESS_INFORMATION pi = {};
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = NULL;

    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        return false;
    }
    CloseHandle(hWrite);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    wchar_t pathBuf[1024] = {};
    DWORD read = 0;
    ReadFile(hRead, pathBuf, (DWORD)sizeof(pathBuf) - sizeof(wchar_t), &read, NULL);
    CloseHandle(hRead);

    if (read > 0) {
        while (read > 0 && (pathBuf[read / sizeof(wchar_t) - 1] == L'\r' ||
                            pathBuf[read / sizeof(wchar_t) - 1] == L'\n'))
            read -= sizeof(wchar_t);
        pathBuf[read / sizeof(wchar_t)] = L'\0';
        outPath = pathBuf;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Auto-detect VCS metadata directories (.svn / .git) for all bound paths
// Walk up parent directories to find the VCS root.
// Only runs if the corresponding VCS tool (svn/git) exists on the system.
// ---------------------------------------------------------------------------
static void KgAutoProtectVcs(std::vector<std::wstring>& roList,
                             const std::vector<std::wstring>& rwList)
{
    std::wstring svnPath, gitPath;
    bool hasSvn = KgFindTool(L"svn.exe", svnPath);
    bool hasGit = KgFindTool(L"git.exe", gitPath);

    auto findVcsMeta = [](const std::wstring& path, const std::wstring& vcsDir) -> std::wstring {
        std::wstring p = path;
        while (p.size() >= 3) {
            std::wstring meta = p + L"\\" + vcsDir;
            DWORD attr = GetFileAttributesW(meta.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                return meta;
            size_t pos = p.find_last_of(L'\\');
            if (pos == std::wstring::npos || pos < 2) break;
            p = p.substr(0, pos);
        }
        return L"";
    };

    auto processPaths = [&](const std::vector<std::wstring>& paths) {
        for (auto& p : paths) {
            if (hasSvn) {
                std::wstring svnDir = findVcsMeta(p, L".svn");
                if (!svnDir.empty() &&
                    std::find(roList.begin(), roList.end(), svnDir) == roList.end()) {
                    roList.push_back(svnDir);
                    Wprintln(L"bwrap.exe: Auto-protected " + svnDir + L" (read-only)\n");
                }
            }
            if (hasGit) {
                std::wstring gitDir = findVcsMeta(p, L".git");
                if (!gitDir.empty() &&
                    std::find(roList.begin(), roList.end(), gitDir) == roList.end()) {
                    roList.push_back(gitDir);
                    Wprintln(L"bwrap.exe: Auto-protected " + gitDir + L" (read-only)\n");
                }
            }
        }
    };

    processPaths(roList);
    processPaths(rwList);

    if (hasSvn) {
        WCHAR appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
            std::wstring svnCfg = std::wstring(appData) + L"\\Subversion";
            DWORD attr = GetFileAttributesW(svnCfg.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                if (std::find(roList.begin(), roList.end(), svnCfg) == roList.end()) {
                    roList.push_back(svnCfg);
                    Wprintln(L"bwrap.exe: Auto-protected " + svnCfg + L" (read-only)\n");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Build a CreateProcess-compatible command line from exe + args
// ---------------------------------------------------------------------------
static std::wstring BuildCmdLine(const std::wstring& exe,
                                  wchar_t* const* args, int argCount) {
    std::wstring line;
    if (exe.find(L' ') != std::wstring::npos)
        line = L"\"" + exe + L"\"";
    else
        line = exe;

    for (int i = 0; i < argCount; ++i) {
        std::wstring a = args[i];
        line += L" ";
        if (a.find(L' ') != std::wstring::npos ||
            a.find(L'\t') != std::wstring::npos) {
            size_t pos = 0;
            while ((pos = a.find(L'"', pos)) != std::wstring::npos) {
                a.insert(pos, 1, L'"');
                pos += 2;
            }
            line += L"\"" + a + L"\"";
        } else {
            line += a;
        }
    }
    return line;
}

// ---------------------------------------------------------------------------
// Log invocation to file when --ses is used
// ---------------------------------------------------------------------------
static void LogSession(const std::wstring& sid, int argc, wchar_t* argv[]) {
    WCHAR profile[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, profile)))
        return;
    std::wstring dir = std::wstring(profile) + L"\\.local\\share\\kilo\\winbwrap";
    SHCreateDirectoryExW(NULL, dir.c_str(), NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);

    WCHAR ts[64];
    swprintf_s(ts, ARRAYSIZE(ts), L"%04d-%02d-%02d %02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);

    std::wstring line;
    line += L"======================\n";
    line += ts;
    line += L"\n======================\n";
    line += L"& ";
    line += self;
    line += L" `\n";

    int i = 1;
    auto q = [](const std::wstring& v) {
        if (v.find(L' ') != std::wstring::npos)
            return L"'" + v + L"'";
        return v;
    };
    for (; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--") {
            i++;
            break;
        }
        line += a;
        bool first = true;
        while (i + 1 < argc) {
            std::wstring next = argv[i + 1];
            if (next == L"--" || (next.size() >= 2 && next[0] == L'-' && next[1] == L'-'))
                break;
            i++;
            if (!first) line += a;
            line += L" " + q(argv[i]) + L" `\n";
            first = false;
        }
        if (first) line += L" `\n";
    }

    if (i < argc) {
        line += L"-- ";
        line += q(argv[i]);
        for (int j = i + 1; j < argc; j++) {
            line += L" ";
            line += q(argv[j]);
        }
    }
    line += L"\n"; 

    std::wstring path = dir + L"\\" + sid + L".log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        std::string utf8;
        int ulen = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                        NULL, 0, NULL, NULL);
        utf8.resize(ulen);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                             &utf8[0], ulen, NULL, NULL);
        DWORD w;
        WriteFile(h, utf8.c_str(), (DWORD)utf8.size(), &w, NULL);
        CloseHandle(h);
    }
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
static void ShowHelp() {
    std::cout
        << "bwrap.exe - Run commands inside a KiloGuard sandbox\n\n"
        << "Usage:\n"
        << "  bwrap.exe [flags] -- <exe> [args...]\n\n"
        << "Flags:\n"
        << "  --ro-bind <path> [path...]  Read-only access to path(s)\n"
        << "  --bind <path> [path...]     Read-write access to path(s)\n"
        << "  --tmpfs <path> [path...]    Deny all access to path(s)\n"
        << "  --net-allow <exe> [exe...]  Exempt executable(s) from network blocking\n"
        << "  --alias <name> [name...]    Resolve App Execution Alias(es) (e.g. python3 wt\n"
        << "                              winget) and auto --ro-bind their real dirs.\n"
        << "                              pwsh is always resolved by default.\n"
        << "  --showbox                 Print sandbox SID to stdout after creation\n"
        << "  --showConsole             Enable diagnostic messages on the console\n"
        << "  --no-inject               Skip KiloHook.dll injection into pwsh\n"
        << "  --ses <id>                Log invocation to session file for Agent Manager\n"
        << "  -h, --help          Show this help message\n"
        << "\n"
        << "Always denied (escape tools):\n"
        << "  schtasks.exe wmic.exe bitsadmin.exe cscript.exe wscript.exe\n"
        << "  mshta.exe certutil.exe rundll32.exe msiexec.exe regsvr32.exe\n"
        << "\n"
        << "Security model:\n"
        << "  default   -> deny-all (no access)\n"
        << "  --ro-bind -> read-only\n"
        << "  --bind    -> read-write\n"
        << "  --tmpfs   -> explicit deny\n"
        << "\n"
        << "Examples:\n"
        << "  bwrap.exe --bind C:\\project --ro-bind C:\\Windows"
        << " -- C:\\Windows\\System32\\cmd.exe /c \"echo hello\"\n"
        << "  bwrap.exe --bind C:\\project"
        << " -- C:\\Program Files\\Git\\bin\\bash.exe -c \"npm run build\"\n"
        << std::flush;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        ShowHelp();
        return 0;
    }

    std::wstring first = argv[1];
    if (first == L"--help" || first == L"-help" || first == L"-h" ||
        first == L"--h" || first == L"/?") {
        ShowHelp();
        return 0;
    }

    std::vector<std::wstring> roList;
    std::vector<std::wstring> rwList;
    std::vector<std::wstring> denyList;
    std::vector<std::wstring> netAllowList;
    std::vector<std::wstring> aliasList;
    bool showbox = false;
    std::wstring ses;

    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--") {
            ++i;
            break;
        }

        auto drain = [&](std::vector<std::wstring>& list) {
            while (++i < argc) {
                std::wstring next = argv[i];
                if (next == L"--" ||
                    (next.size() >= 2 && next[0] == L'-' && next[1] == L'-')) {
                    --i; break;
                }
                list.push_back(next);
            }
            if (list.empty()) {
                std::wcerr << L"bwrap.exe: " << a << L" requires at least one path\n";
                std::wcerr << L"Try --help for usage.\n";
                ExitProcess(1);
            }
        };

        if (a == L"--ro-bind") {
            drain(roList);
        } else if (a == L"--bind") {
            drain(rwList);
        } else if (a == L"--tmpfs") {
            drain(denyList);
        } else if (a == L"--net-allow") {
            drain(netAllowList);
        } else if (a == L"--alias") {
            drain(aliasList);
        } else if (a == L"--showbox") {
            showbox = true;
        } else if (a == L"--showConsole") {
            g_showConsole = true;
        } else if (a == L"--no-inject") {
            g_noInject = true;
        } else if (a == L"--ses") {
            if (++i >= argc || std::wstring(argv[i]).find(L"--") == 0) {
                std::wcerr << L"bwrap.exe: --ses requires a session ID\n";
                ExitProcess(1);
            }
            ses = argv[i];
        } else {
            std::wcerr << L"bwrap.exe: unknown flag: " << a << L"\n";
            std::wcerr << L"Try --help for usage.\n";
            return 1;
        }
    }

    if (!ses.empty())
        LogSession(ses, argc, argv);

    // -----------------------------------------------------------------------
    // Implicitly deny known escape tools (both 64-bit and 32-bit paths)
    // -----------------------------------------------------------------------
    {
        WCHAR winDir[MAX_PATH];
        UINT winDirLen = GetWindowsDirectoryW(winDir, MAX_PATH);
        if (winDirLen > 0 && winDirLen < MAX_PATH) {
            std::wstring wd = winDir;
            auto addDenyIfExists = [&](const std::wstring& relPath) {
                std::wstring full = wd + L"\\" + relPath;
                DWORD attr = GetFileAttributesW(full.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES) {
                    denyList.push_back(full);
                }
            };
            addDenyIfExists(L"System32\\schtasks.exe");
            addDenyIfExists(L"SysWOW64\\schtasks.exe");
            addDenyIfExists(L"System32\\wbem\\wmic.exe");
            addDenyIfExists(L"SysWOW64\\wbem\\wmic.exe");
            /*addDenyIfExists(L"System32\\bitsadmin.exe");
            addDenyIfExists(L"SysWOW64\\bitsadmin.exe");
            addDenyIfExists(L"System32\\cscript.exe");
            addDenyIfExists(L"SysWOW64\\cscript.exe");
            addDenyIfExists(L"System32\\mshta.exe");
            addDenyIfExists(L"SysWOW64\\mshta.exe");
            addDenyIfExists(L"System32\\certutil.exe");
            addDenyIfExists(L"SysWOW64\\certutil.exe");
            addDenyIfExists(L"System32\\rundll32.exe");
            addDenyIfExists(L"SysWOW64\\rundll32.exe");
            addDenyIfExists(L"System32\\msiexec.exe");
            addDenyIfExists(L"SysWOW64\\msiexec.exe");
            addDenyIfExists(L"System32\\wscript.exe");
            addDenyIfExists(L"SysWOW64\\wscript.exe");*/
            addDenyIfExists(L"System32\\regsvr32.exe");
            addDenyIfExists(L"SysWOW64\\regsvr32.exe");
        }
    }

    if (i >= argc) {
        std::wcerr << L"bwrap.exe: missing '--' separator and command.\n";
        std::wcerr << L"Try --help for usage.\n";
        return 1;
    }

    std::wstring exe = argv[i];

    // -----------------------------------------------------------------------
    // Phase 1: Convert all Win32 paths to NT paths
    // -----------------------------------------------------------------------
    auto toNt = [](const std::vector<std::wstring>& paths,
                   std::vector<std::wstring>& out) {
        for (auto& p : paths) {
            std::wstring nt;
            if (!Win32ToNtPath(p, nt)) {
                std::wcerr << L"bwrap.exe: cannot resolve path: " << p << L"\n";
                ExitProcess(1);
            }
            out.push_back(nt);
        }
    };

    // Auto-add bwrap.exe's own directory as read-only (for KiloHook.dll access)
    {
        WCHAR selfPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, selfPath, MAX_PATH)) {
            std::wstring selfDir(selfPath);
            size_t pos = selfDir.find_last_of(L'\\');
            if (pos != std::wstring::npos) {
                selfDir = selfDir.substr(0, pos);
                if (std::find(roList.begin(), roList.end(), selfDir) == roList.end()) {
                    roList.push_back(selfDir);
                    Wprintln(L"bwrap.exe: Auto-protected own directory " + selfDir + L" (read-only)\n");
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // App Execution Alias handling
    //   * Always ro-bind the WindowsApps alias-entry directory
    //     (%LOCALAPPDATA%\Microsoft\WindowsApps — holds every 0-byte alias stub)
    //   * Always resolve "pwsh" by default, plus every --alias <name>
    //   * For each alias, resolve its real target dir and add it as --ro-bind
    // -----------------------------------------------------------------------
    {
        WCHAR localAppData[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring winApps = std::wstring(localAppData) + L"\\Microsoft\\WindowsApps";
            if (GetFileAttributesW(winApps.c_str()) != INVALID_FILE_ATTRIBUTES &&
                std::find(roList.begin(), roList.end(), winApps) == roList.end()) {
                roList.push_back(winApps);
                Wprintln(L"bwrap.exe: Auto-bind alias directory " + winApps + L" (read-only)\n");
            }
        }

        // "pwsh" is always resolved by default, then any user --alias names.
        std::vector<std::wstring> effectiveAliases;
        effectiveAliases.push_back(L"pwsh");
        for (auto& a : aliasList) effectiveAliases.push_back(a);

        for (auto& name : effectiveAliases) {
            std::wstring dir = ResolveAliasTargetDir(name);
            if (dir.empty()) {
                WCHAR nbuf[512];
                int nlen = swprintf_s(nbuf, ARRAYSIZE(nbuf), L"bwrap.exe: alias %s not found in WindowsApps, skipped\n", name.c_str());
                if (nlen > 0) Wprintln(nbuf);
                continue;
            }
            if (std::find(roList.begin(), roList.end(), dir) == roList.end()) {
                roList.push_back(dir);
                Wprintln(L"bwrap.exe: alias " + name + L" -> --ro-bind " + dir + L" (read-only)\n");
            }
        }
    }

    // AppRepository — required by ApiSetHost.AppExecutionAlias.dll
    // to resolve aliases at runtime (reads .pckgdep files)
    {
        std::wstring appRepo = L"C:\\ProgramData\\Microsoft\\Windows\\AppRepository";
        if (GetFileAttributesW(appRepo.c_str()) != INVALID_FILE_ATTRIBUTES &&
            std::find(roList.begin(), roList.end(), appRepo) == roList.end()) {
            roList.push_back(appRepo);
            Wprintln(L"bwrap.exe: Auto-bind AppRepository " + appRepo + L" (read-only)\n");
        }
    }

    // Auto-detect VCS metadata directories (.svn / .git) and add --ro-bind
    KgAutoProtectVcs(roList, rwList);

    std::vector<std::wstring> roNt, rwNt, denyNt;
    toNt(roList, roNt);
    toNt(rwList, rwNt);
    toNt(denyList, denyNt);

    // -----------------------------------------------------------------------
    // Phase 2: Connect to driver and configure sandbox
    // -----------------------------------------------------------------------
    DriverClient drv;
    drv.Open();
    drv.Authenticate();
    KG_SANDBOX_ID sid = drv.CreateSandbox();

    // Build rules.
    // Driver evaluation: longest prefix match wins, default level = 0 (deny).
    // All rules are prefix matches (no exact-match mode in driver).
    std::vector<KG_POLICY_RULE_ENTRY> rules;
    auto addRule = [&](const std::wstring& nt, KG_SANDBOX_LEVEL level) {
        KG_POLICY_RULE_ENTRY re = {};
        re.Sid = sid;
        re.Level = level;
        wcsncpy_s(re.Path, 1024, nt.c_str(), _TRUNCATE);
        rules.push_back(re);
    };

    // --tmpfs paths: level 0 (deny)
    for (auto& nt : denyNt)
        addRule(nt, 0);

    // --bind paths: level 2 (read-write)
    for (auto& nt : rwNt)
        addRule(nt, 2);

    // --ro-bind paths: level 1 (read-only)
    for (auto& nt : roNt)
        addRule(nt, 1);

    // Auto-detect Windows directory and add as read-only
    {
        WCHAR winDir[MAX_PATH];
        if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
            std::wstring nt;
            if (Win32ToNtPath(winDir, nt)) {
                addRule(nt, 1);
            }
        }
    }

    drv.AddRules(rules);

    // --net-allow paths: convert to NT and send to driver
    if (!netAllowList.empty()) {
        std::vector<std::wstring> netAllowNt;
        for (auto& p : netAllowList) {
            std::wstring nt;
            if (!Win32ToNtPath(p, nt)) {
                std::wcerr << L"bwrap.exe: cannot resolve path: " << p << L"\n";
                ExitProcess(1);
            }
            netAllowNt.push_back(nt);
        }
        drv.SetNetExeList(netAllowNt);
        for (auto& nt : netAllowNt)
            Wprintln(L"bwrap.exe: Net-whitelisted " + nt + L"\n");
    }

    // Attach bwrap itself to the sandbox.
    // Subsequent CreateProcess will have bwrap as parent, which IS in PidMap,
    // so child processes automatically inherit sandbox via KiloProcessNotify.
    drv.AttachProcess(GetCurrentProcessId(), sid);
    WCHAR abuf[128];
    int alen = swprintf_s(abuf, ARRAYSIZE(abuf), L"bwrap.exe: Attached own PID %lu to sandbox SID=%lu\n", GetCurrentProcessId(), sid);
    if (alen > 0) Wprintln(abuf);

    if (showbox)
        std::cout << "Enter Sandbox SID=" << sid << std::endl;

    // Set KiloHook.dll path from bwrap.exe's own directory for injection
    {
        WCHAR selfPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, selfPath, MAX_PATH)) {
            std::wstring selfDir(selfPath);
            size_t pos = selfDir.find_last_of(L'\\');
            if (pos != std::wstring::npos) {
                selfDir = selfDir.substr(0, pos);
                std::wstring dllPath = selfDir + L"\\KiloHook.dll";
                if (GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    drv.SetHookDllPath(dllPath);
                else {
                    WCHAR dbuf[512];
                    int dlen = swprintf_s(dbuf, ARRAYSIZE(dbuf), L"bwrap.exe: KiloHook.dll not found at %s\n", dllPath.c_str());
                    if (dlen > 0) Wprintln(dbuf);
                }
            }
        }
    }

    drv.StartNotifyThread();
    drv.StartDenyThread();

    // -----------------------------------------------------------------------
    // Phase 3: Launch child process and attach to sandbox
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Resolve launcher based on command extension
    // -----------------------------------------------------------------------
    // Resolve exe to full path if it's relative
    {
        WCHAR fullPath[MAX_PATH];
        DWORD flen = GetFullPathNameW(exe.c_str(), MAX_PATH, fullPath, NULL);
        if (flen > 0 && flen < MAX_PATH)
            exe = fullPath;

        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring shortName = exe;
            size_t s = shortName.find_last_of(L"\\/");
            if (s != std::wstring::npos)
                shortName = shortName.substr(s + 1);

            std::wstring foundPath = FindExePathFromPath(shortName);
            if (!foundPath.empty()) {
                exe = foundPath;
                if (IsAppExecutionAlias(exe))
                    Wprintln(L"bwrap.exe: Resolved AppExec alias " + shortName + L" -> " + exe + L"\n");
                else
                    Wprintln(L"bwrap.exe: Resolved from PATH: " + shortName + L" -> " + exe + L"\n");
            } else {
                std::wcerr << L"bwrap.exe: command not found: "
                           << shortName << L"\n";
                return 1;
            }
        }
    }

    auto cmdLine = BuildCmdLine(exe, argv + i + 1, argc - i - 1);

    std::wstring exeLower = exe;
    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

    auto hasExt = [&](const wchar_t* ext) -> bool {
        size_t elen = wcslen(ext);
        return exeLower.size() >= elen &&
               exeLower.substr(exeLower.size() - elen) == ext;
    };

    if (hasExt(L".exe") || hasExt(L".com") || hasExt(L".scr")) {
        // Direct PE launch — use as-is
    } else if (hasExt(L".bat") || hasExt(L".cmd")) {
        cmdLine = L"cmd.exe /c " + cmdLine;
    } else if (hasExt(L".ps1") || hasExt(L".psm1")) {
        std::wstring shell;
        if (KgFindTool(L"pwsh.exe", shell))
            cmdLine = shell + L" -File " + cmdLine;
        else if (KgFindTool(L"powershell.exe", shell))
            cmdLine = shell + L" -File " + cmdLine;
        else {
            std::wcerr << L"bwrap.exe: cannot execute " << exe
                       << L" — no pwsh.exe or powershell.exe found\n";
            return 1;
        }
    } else {
        std::wcerr << L"bwrap.exe: cannot execute " << exe
                   << L" — unsupported file extension\n";
        return 1;
    }

    std::wstring cwd;
    if (!rwList.empty())
        cwd = rwList[0];
    else if (!roList.empty())
        cwd = roList[0];
    LPCWSTR lpCwd = cwd.empty() ? NULL : cwd.c_str();

    std::vector<wchar_t> cmdbuf(cmdLine.begin(), cmdLine.end());
    cmdbuf.push_back(L'\0');

    // Create Job Object — auto-terminate child tree on bwrap exit/crash
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo))) {
            CloseHandle(hJob);
            hJob = NULL;
        }
    }

    STARTUPINFOW si = { sizeof(si) };

    UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(NULL, cmdbuf.data(),
        NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        NULL, lpCwd, &si, &pi);

    SetErrorMode(oldErrorMode);

    if (!ok) {
        if (hJob) CloseHandle(hJob);
        std::wcerr << L"bwrap.exe: CreateProcess failed ("
                   << GetLastError() << L") for: " << cmdLine << L"\n";
        return 1;
    } else {
        WCHAR cbuf[512];
        int clen = swprintf_s(cbuf, ARRAYSIZE(cbuf), L"bwrap.exe: Created process PID=%lu ProcessName=%s\n", pi.dwProcessId, exe.c_str());
        if (clen > 0) Wprintln(cbuf);
    }

    // Child inherits sandbox automatically (KiloProcessNotify sees bwrap in PidMap)
    // and driver sends CREATE notification via port. No manual AttachProcess needed.

    // Assign to Job Object
    if (hJob) {
        if (!AssignProcessToJobObject(hJob, pi.hProcess))
            std::wcerr << L"bwrap.exe: AssignProcessToJobObject failed ("
                       << GetLastError() << L")" << std::endl;
    }

    // Resume child
    ResumeThread(pi.hThread);

    // -----------------------------------------------------------------------
    // Phase 4: Wait for child to exit
    // -----------------------------------------------------------------------
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);

    drv.DrainEvents();

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hJob) CloseHandle(hJob);

    return (int)ec;
}
