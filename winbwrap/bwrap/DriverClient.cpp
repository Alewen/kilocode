
#include "DriverClient.h"

// ---------------------------------------------------------------------------
// Error helper
// ---------------------------------------------------------------------------
[[noreturn]] static void Die(const char* msg)
{
    std::cerr << "bwrap.exe: " << msg << " (error " << GetLastError() << ")" << std::endl;
    ExitProcess(GetLastError() ? GetLastError() : 1);
}

DriverClient::DriverClient()
    : m_h(INVALID_HANDLE_VALUE)
    , m_sid(0)
    , m_notifyThread(NULL)
    , m_stopEvent(NULL)
    , m_denyThread(NULL)
    , m_denyStopEvent(NULL)
    , m_portHandle(NULL)
    , m_portThread(NULL)
{ }

void DriverClient::SetHookDllPath(const std::wstring& p)
{
    m_hookDllPath = p;
}

DriverClient::~DriverClient()
{
    StopDenyThread();
    StopNotifyThread();
    DisconnectPort();
    if (m_h != INVALID_HANDLE_VALUE)
    {
        DestroySandbox();
        Deauthenticate();
        CloseHandle(m_h);
    }
}

bool DriverClient::IsPidInSandbox(DWORD pid)
{
    std::lock_guard<std::mutex> lock(m_pidMutex);
    return m_pids.find(pid) != m_pids.end();
}

void DriverClient::AddPid(DWORD pid)
{
    std::lock_guard<std::mutex> lock(m_pidMutex);
    m_pids.insert(pid);
    WCHAR buf[128];
    int len = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Create PID=%lu (initial)\n", m_sid, pid);
    if (len > 0) Wprintln(buf);
}

void DriverClient::Open()
{
    m_h = CreateFileW(
        L"\\\\.\\KiloGuard",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (m_h == INVALID_HANDLE_VALUE)
        Die("Cannot open \\\\.\\KiloGuard (driver not loaded?)");
}

void DriverClient::Authenticate()
{
    DWORD junk;
    if (!DeviceIoControl(m_h, IOCTL_KG_AUTHENTICATE,
        (LPVOID)gExpectedToken, BW_TOKEN_SIZE,
        NULL, 0, &junk, NULL))
        Die("IOCTL_KG_AUTHENTICATE failed (token mismatch?)");
}

void DriverClient::Deauthenticate()
{
    DWORD junk;
    DeviceIoControl(m_h, IOCTL_KG_DEAUTHENTICATE,
        NULL, 0, NULL, 0, &junk, NULL);
}

KG_SANDBOX_ID DriverClient::CreateSandbox()
{
    KG_CONTROL_SANDBOX in = {};
    in.Destroy = FALSE;
    DWORD junk;
    if (!DeviceIoControl(m_h, IOCTL_KG_CONTROL_SANDBOX,
        &in, sizeof(in), &in, sizeof(in), &junk, NULL))
        Die("IOCTL_KG_CONTROL_SANDBOX (create) failed");
    m_sid = in.Sid;
    return m_sid;
}

void DriverClient::DestroySandbox()
{
    if (m_sid == 0) return;
    KG_CONTROL_SANDBOX in = {};
    in.Destroy = TRUE;
    in.Sid = m_sid;
    DWORD junk;
    DeviceIoControl(m_h, IOCTL_KG_CONTROL_SANDBOX,
        &in, sizeof(in), NULL, 0, &junk, NULL);
    m_sid = 0;
}

void DriverClient::SetNetExeList(const std::vector<std::wstring>& ntPaths)
{
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

void DriverClient::SetDenyLogEnabled(KG_SANDBOX_ID sid, BOOLEAN enabled)
{
    KG_SET_DENY_LOG_INPUT in;
    in.Sid = sid;
    in.IsEnableDeny = enabled;
    DWORD junk;
    if (!DeviceIoControl(m_h, IOCTL_KG_SET_DENY_LOG, &in, sizeof(in), NULL, 0, &junk, NULL))
        Die("IOCTL_KG_SET_DENY_LOG failed");
}

void DriverClient::AddRules(const std::vector<KG_POLICY_RULE_ENTRY>& rules)
{
    if (rules.empty()) return;
    const size_t BATCH_MAX = 30;
    for (size_t offset = 0; offset < rules.size(); offset += BATCH_MAX)
    {
        size_t count = std::min(BATCH_MAX, rules.size() - offset);
        size_t bufSize = sizeof(KG_POLICY_BATCH_INPUT) +
            (count - 1) * sizeof(KG_POLICY_RULE_ENTRY);
        auto buf = std::make_unique<char[]>(bufSize);
        auto batch = reinterpret_cast<KG_POLICY_BATCH_INPUT*>(buf.get());
        batch->RuleCount = (ULONG)count;
        for (size_t i = 0; i < count; i++)
            batch->Rules[i] = rules[offset + i];
        DWORD junk;
        if (!DeviceIoControl(m_h, IOCTL_KG_SET_POLICY_BATCH,
            batch, (DWORD)bufSize, NULL, 0, &junk, NULL))
            Die("IOCTL_KG_SET_POLICY_BATCH failed");
    }
}

void DriverClient::AttachProcess(DWORD pid, KG_SANDBOX_ID sid)
{
    KG_ATTACH_PROCESS_INPUT in;
    in.Pid = (HANDLE)(ULONG_PTR)pid;
    in.Sid = sid;
    in.Detach = FALSE;
    DWORD junk;
    if (!DeviceIoControl(m_h, IOCTL_KG_ATTACH_PROCESS,
        &in, sizeof(in), NULL, 0, &junk, NULL))
        Die("IOCTL_KG_ATTACH_PROCESS failed");
}

void DriverClient::QueryDenyEvents(KG_QUERY_DENY_EVENTS_OUTPUT& out)
{
    KG_QUERY_EVENTS_INPUT in;
    in.Sid = m_sid;
    DWORD junk;
    DeviceIoControl(m_h, IOCTL_KG_QUERY_DENY_EVENTS,
        &in, sizeof(in), &out, sizeof(out), &junk, NULL);
}

void DriverClient::DrainDenyEvents()
{
    for (;;)
    {
        KG_QUERY_DENY_EVENTS_OUTPUT out = {};
        QueryDenyEvents(out);
        if (out.EventCount == 0) break;
        for (ULONG i = 0; i < out.EventCount; i++)
        {
            WCHAR line[512];
            int len;
            if (out.Events[i].Flags == 1)
            {
                len = swprintf_s(line, ARRAYSIZE(line),
                    L"bwrap.exe: DENY PID=%lu ProcessName=%s Net=%s\n",
                    out.Events[i].Pid,
                    out.Events[i].ProcessName,
                    out.Events[i].FilePath);
            }
            else
            {
                len = swprintf_s(line, ARRAYSIZE(line),
                    L"bwrap.exe: DENY PID=%lu ProcessName=%s File=%s\n",
                    out.Events[i].Pid,
                    out.Events[i].ProcessName,
                    out.Events[i].FilePath);
            }
            if (len > 0)
            {
                Wprintln(line);
            }
        }
    }
}

void DriverClient::StartDenyThread()
{
    m_denyStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_denyThread = CreateThread(NULL, 0, DenyThreadProc, this, 0, NULL);
}

void DriverClient::StopDenyThread()
{
    if (m_denyStopEvent)
    {
        SetEvent(m_denyStopEvent);
        if (m_denyThread)
        {
            WaitForSingleObject(m_denyThread, 5000);
            CloseHandle(m_denyThread);
            m_denyThread = NULL;
        }
        CloseHandle(m_denyStopEvent);
        m_denyStopEvent = NULL;
    }
}

void DriverClient::QueryEvents(KG_QUERY_EVENTS_OUTPUT& out)
{
    KG_QUERY_EVENTS_INPUT in;
    in.Sid = m_sid;
    DWORD junk;
    if (!DeviceIoControl(m_h, IOCTL_KG_QUERY_EVENTS,
        &in, sizeof(in), &out, sizeof(out), &junk, NULL))
        return;
}

void DriverClient::DrainEvents()
{
    for (;;)
    {
        KG_QUERY_EVENTS_OUTPUT out = {};
        QueryEvents(out);
        if (out.EventCount == 0)
            break;
        for (ULONG i = 0; i < out.EventCount; i++)
        {
            std::wstring winPath = NtPathToWin32(out.Events[i].ImageName);
            std::wcerr << L"PID " << out.Events[i].Pid
                << L" : " << winPath << L" (DLL not found: " << out.Events[i].DllPath << L")" << std::endl;
        }
    }
}

void DriverClient::StartNotifyThread()
{
    m_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_notifyThread = CreateThread(NULL, 0, NotifyThreadProc, this, 0, NULL);
    ConnectNotificationPort();
}

void DriverClient::StopNotifyThread()
{
    if (m_stopEvent)
    {
        SetEvent(m_stopEvent);
        if (m_notifyThread)
        {
            WaitForSingleObject(m_notifyThread, 5000);
            CloseHandle(m_notifyThread);
            m_notifyThread = NULL;
        }
        CloseHandle(m_stopEvent);
        m_stopEvent = NULL;
    }
}

void DriverClient::ConnectNotificationPort()
{
    if (m_portHandle) return;
    HRESULT hr = FilterConnectCommunicationPort(
        L"\\KiloGuardPort", 0, &m_sid, sizeof(m_sid),
        NULL, &m_portHandle);
    if (FAILED(hr))
    {
        std::wcerr << L"bwrap.exe: Port connect failed ("
            << std::hex << hr << std::dec << L")\n";
        m_portHandle = NULL;
        return;
    }
    WCHAR buf[128];
    int len = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Notification port connected\n", m_sid);
    if (len > 0) {
        Wprintln(buf);
    }
    m_portThread = CreateThread(NULL, 0, PortThreadProc, this, 0, NULL);
}

void DriverClient::DisconnectPort()
{
    if (m_portHandle)
    {
        CloseHandle(m_portHandle);
        m_portHandle = NULL;
    }
    if (m_portThread)
    {
        WaitForSingleObject(m_portThread, 5000);
        CloseHandle(m_portThread);
        m_portThread = NULL;
    }
}
