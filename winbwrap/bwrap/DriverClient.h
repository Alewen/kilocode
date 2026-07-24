#pragma once

#include "framework.h"
#include "bwrap_secret.h"
#include "AppX.h"

// ---------------------------------------------------------------------------
// Driver handle wrapper
// ---------------------------------------------------------------------------
class DriverClient
{
private:
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
    DriverClient();

    void SetHookDllPath(const std::wstring& p);

    ~DriverClient();

    bool IsPidInSandbox(DWORD pid);
    void AddPid(DWORD pid);
    void Open();
    void Authenticate();
    void Deauthenticate();
    KG_SANDBOX_ID CreateSandbox();

    void DestroySandbox();
    void SetNetExeList(const std::vector<std::wstring>& ntPaths);
    void SetDenyLogEnabled(KG_SANDBOX_ID sid, BOOLEAN enabled);
    void AddRules(const std::vector<KG_POLICY_RULE_ENTRY>& rules);
    void AttachProcess(DWORD pid, KG_SANDBOX_ID sid);
    void QueryDenyEvents(KG_QUERY_DENY_EVENTS_OUTPUT& out);
    void DrainDenyEvents();
    void StartDenyThread();
    void StopDenyThread();
    void QueryEvents(KG_QUERY_EVENTS_OUTPUT& out);
    void DrainEvents();
    void StartNotifyThread();
    void StopNotifyThread();
    void ConnectNotificationPort();
    void DisconnectPort();

    static DWORD WINAPI PortThreadProc(LPVOID param)
    {
        DriverClient* self = (DriverClient*)param;
        if (!self->m_portHandle) return 0;

        BYTE buffer[sizeof(FILTER_MESSAGE_HEADER) + sizeof(KG_PORT_MESSAGE)];
        FILTER_MESSAGE_HEADER* hdr = (FILTER_MESSAGE_HEADER*)buffer;

        WCHAR buf[128];
        int len = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Port thread started\n", self->m_sid);
        if (len > 0) Wprintln(buf);
        while (true)
        {
            HRESULT hr = FilterGetMessage(self->m_portHandle, hdr,
                sizeof(buffer), NULL);
            if (FAILED(hr)) break;

            KG_PORT_MESSAGE* msg = (KG_PORT_MESSAGE*)(hdr + 1);
            {
                std::lock_guard<std::mutex> lock(self->m_pidMutex);
                if (msg->MsgType == KG_PORT_MSG_PROCESS_CREATE)
                {
                    self->m_pids.insert(msg->Pid);
                    std::wstring winPath = NtPathToWin32(msg->ImageName);
                    WCHAR buf[512];
                    int blen = swprintf_s(buf, ARRAYSIZE(buf), L"bwrap.exe: Sandbox SID=%lu Create PID=%lu ProcName=%s\n", msg->SID, msg->Pid, winPath.c_str());
                    if (blen > 0) Wprintln(buf);

                }
                else if (msg->MsgType == KG_PORT_MSG_READY_FOR_INJECT)
                {
                    if (!self->m_hookDllPath.empty())
                    {
                        if (g_noInject)
                        {
                            WCHAR sbuf[128];
                            int slen = swprintf_s(sbuf, ARRAYSIZE(sbuf), L"bwrap.exe: Sandbox SID=%lu Skip Inject PID=%lu (--no-inject)\n", self->m_sid, msg->Pid);
                            if (slen > 0) Wprintln(sbuf);
                        }
                        else
                        {
                            WCHAR ibuf[128];
                            int ilen = swprintf_s(ibuf, ARRAYSIZE(ibuf), L"bwrap.exe: Sandbox SID=%lu Inject PID=%lu (ready)\n", self->m_sid, msg->Pid);
                            if (ilen > 0) Wprintln(ibuf);

                            HANDLE hProc = OpenProcess(
                                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                FALSE, msg->Pid);
                            if (hProc)
                            {
                                size_t cbBuf = (self->m_hookDllPath.size() + 1) * sizeof(WCHAR);
                                LPVOID pRemote = VirtualAllocEx(hProc, NULL, cbBuf,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                                if (pRemote)
                                {
                                    WriteProcessMemory(hProc, pRemote,
                                        self->m_hookDllPath.c_str(), cbBuf, NULL);
                                    HANDLE hTh = CreateRemoteThread(hProc, NULL, 0,
                                        (LPTHREAD_START_ROUTINE)GetProcAddress(
                                            GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
                                        pRemote, 0, NULL);
                                    if (hTh)
                                    {
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
                }
                else if (msg->MsgType == KG_PORT_MSG_PROCESS_EXIT)
                {
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

    static DWORD WINAPI NotifyThreadProc(LPVOID param)
    {
        DriverClient* self = (DriverClient*)param;
        self->DrainEvents();
        while (WaitForSingleObject(self->m_stopEvent, 500) == WAIT_TIMEOUT)
        {
            self->DrainEvents();
        }
        return 0;
    }

    static DWORD WINAPI DenyThreadProc(LPVOID param)
    {
        DriverClient* self = (DriverClient*)param;
        self->DrainDenyEvents();
        while (WaitForSingleObject(self->m_denyStopEvent, 100) == WAIT_TIMEOUT)
        {
            self->DrainDenyEvents();
        }
        self->DrainDenyEvents();
        return 0;
    }
};
