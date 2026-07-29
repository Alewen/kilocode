#include <windows.h>
#include <stdio.h>
#include <objbase.h>
#include <shlobj.h>
#include <wbemidl.h>
#include <taskschd.h>
#include <comdef.h>
#include <strsafe.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static VOID PrintHr(const char* label, HRESULT hr) {
    if (SUCCEEDED(hr)) {
        printf("[OK]   %s (hr=0x%08lX)\n", label, hr);
    } else {
        printf("[FAIL] %s (hr=0x%08lX)\n", label, hr);
    }
}

// ============================================================================
// Test 1: Scripting.FileSystemObject
// ============================================================================
static VOID TestFso() {
    printf("\n=== Test 1: Scripting.FileSystemObject ===\n");
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // Already initialized at main; ignore

    CLSID clsid;
    hr = CLSIDFromProgID(L"Scripting.FileSystemObject", &clsid);
    PrintHr("CLSIDFromProgID(FSO)", hr);
    if (FAILED(hr)) return;

    IDispatch* pDisp = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&pDisp);
    PrintHr("CoCreateInstance(FSO)", hr);
    if (FAILED(hr)) return;

    // Call Drives property
    DISPID dispid = 0;
    LPOLESTR name = L"Drives";
    hr = pDisp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispid);
    PrintHr("GetIDsOfNames(Drives)", hr);

    if (pDisp) pDisp->Release();
}

// ============================================================================
// Test 2: Shell.Application
// ============================================================================
static VOID TestShellApp() {
    printf("\n=== Test 2: Shell.Application ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"Shell.Application", &clsid);
    PrintHr("CLSIDFromProgID(Shell.Application)", hr);
    if (FAILED(hr)) return;

    IShellDispatch* pShell = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IShellDispatch, (void**)&pShell);
    PrintHr("CoCreateInstance(Shell.Application)", hr);
    if (FAILED(hr)) return;

    // Try Windows() via IDispatch
    IDispatch* pDisp = NULL;
    LPOLESTR name = L"Windows";
    DISPID dispid = 0;
    hr = pShell->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispid);
    PrintHr("IShellDispatch::get_Windows (via IDispatch)", hr);

    if (pShell) pShell->Release();
}

// ============================================================================
// Test 3: WbemScripting.SWbemLocator (WMI)
// ============================================================================
static VOID TestWmi() {
    printf("\n=== Test 3: WbemScripting.SWbemLocator (WMI) ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"WbemScripting.SWbemLocator", &clsid);
    PrintHr("CLSIDFromProgID(SWbemLocator)", hr);
    if (FAILED(hr)) return;

    ISWbemLocator* pLoc = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_ISWbemLocator, (void**)&pLoc);
    PrintHr("CoCreateInstance(SWbemLocator)", hr);
    if (FAILED(hr)) return;

    ISWbemServices* pSvc = NULL;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = pLoc->ConnectServer(_bstr_t(L"."), _bstr_t(L"ROOT\\CIMV2"),
        NULL, NULL, NULL, NULL, 0, NULL, &pSvc);
    PrintHr("ConnectServer(root\\cimv2)", hr);
    if (SUCCEEDED(hr) && pSvc) {
        ISWbemObjectSet* pSet = NULL;
        hr = pSvc->ExecQuery(_bstr_t(L"SELECT Caption FROM Win32_OperatingSystem"),
            _bstr_t(L"WQL"), wbemFlagReturnImmediately | wbemFlagForwardOnly, NULL, &pSet);
        PrintHr("ExecQuery(Win32_OperatingSystem)", hr);
        if (pSet) pSet->Release();
        pSvc->Release();
    }
    SysFreeString(ns);
    if (pLoc) pLoc->Release();
}

// ============================================================================
// Test 4: Schedule.Service (Task Scheduler)
// ============================================================================
static VOID TestTaskScheduler() {
    printf("\n=== Test 4: Schedule.Service (Task Scheduler) ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"Schedule.Service", &clsid);
    PrintHr("CLSIDFromProgID(Schedule.Service)", hr);
    if (FAILED(hr)) return;

    ITaskService* pSvc = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pSvc);
    PrintHr("CoCreateInstance(TaskScheduler)", hr);
    if (FAILED(hr)) return;

    hr = pSvc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    PrintHr("ITaskService::Connect", hr);
    if (FAILED(hr)) { if (pSvc) pSvc->Release(); return; }

    ITaskFolder* pRoot = NULL;
    hr = pSvc->GetFolder(_bstr_t(L"\\"), &pRoot);
    PrintHr("GetFolder(\\)", hr);
    if (SUCCEEDED(hr) && pRoot) {
        IRegisteredTaskCollection* pTasks = NULL;
        hr = pRoot->GetTasks(0, &pTasks);
        PrintHr("GetTasks", hr);
        if (SUCCEEDED(hr) && pTasks) {
            LONG count = 0;
            pTasks->get_Count(&count);
            printf("[INFO] Tasks in root folder: %ld\n", count);
            pTasks->Release();
        }
        pRoot->Release();
    }

    // Try creating a task
    printf("\n  --- Attempting task creation ---\n");
    ITaskDefinition* pDef = NULL;
    hr = pSvc->NewTask(0, &pDef);
    if (SUCCEEDED(hr) && pDef) {
        IRegistrationInfo* pRegInfo = NULL;
        pDef->get_RegistrationInfo(&pRegInfo);
        if (pRegInfo) {
            pRegInfo->put_Description(_bstr_t(L"Sandbox test task"));
            pRegInfo->Release();
        }

        ITaskSettings* pSettings = NULL;
        pDef->get_Settings(&pSettings);
        if (pSettings) {
            pSettings->put_AllowDemandStart(VARIANT_TRUE);
            pSettings->Release();
        }

        ITriggerCollection* pTriggers = NULL;
        pDef->get_Triggers(&pTriggers);
        if (pTriggers) {
            ITrigger* pTrig = NULL;
            pTriggers->Create(TASK_TRIGGER_TIME, &pTrig);
            if (pTrig) pTrig->Release();
            pTriggers->Release();
        }

        IActionCollection* pActions = NULL;
        pDef->get_Actions(&pActions);
        if (pActions) {
            IAction* pAction = NULL;
            pActions->Create(TASK_ACTION_EXEC, &pAction);
            if (pAction) {
                IExecAction* pExec = NULL;
                pAction->QueryInterface(IID_IExecAction, (void**)&pExec);
                if (pExec) {
                    pExec->put_Path(_bstr_t(L"cmd.exe"));
                    pExec->put_Arguments(_bstr_t(L"/c echo hello"));
                    pExec->Release();
                }
                pAction->Release();
            }
            pActions->Release();
        }

        IRegisteredTask* pTask = NULL;
        hr = pRoot->RegisterTaskDefinition(
            _bstr_t(L"SandboxTestTask"),
            pDef,
            TASK_CREATE_OR_UPDATE,
            _variant_t(),
            _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(L""),
            &pTask);
        PrintHr("RegisterTaskDefinition", hr);
        if (pTask) pTask->Release();
        pDef->Release();
    }

    if (pSvc) pSvc->Release();
}

// ============================================================================
// Test 5: WScript.Shell
// ============================================================================
static VOID TestWshShell() {
    printf("\n=== Test 5: WScript.Shell ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"WScript.Shell", &clsid);
    PrintHr("CLSIDFromProgID(WScript.Shell)", hr);
    if (FAILED(hr)) return;

    IDispatch* pDisp = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&pDisp);
    PrintHr("CoCreateInstance(WScript.Shell)", hr);
    if (FAILED(hr)) return;

    if (pDisp) pDisp->Release();
}

// ============================================================================
// Test 6: OpenSCManager with various access
// ============================================================================
static VOID TestScm() {
    printf("\n=== Test 6: SCM (Service Control Manager) ===\n");

    SC_HANDLE hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (hScm) {
        printf("[ALERT] OpenSCManager(CREATE_SERVICE) SUCCESS!\n");
        CloseServiceHandle(hScm);
    } else {
        printf("[INFO]  OpenSCManager(CREATE_SERVICE) denied: %lu\n", GetLastError());
    }

    hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (hScm) {
        printf("[OK]   OpenSCManager(ENUMERATE_SERVICE) success (read-only)\n");
        CloseServiceHandle(hScm);
    } else {
        printf("[FAIL] OpenSCManager(ENUMERATE_SERVICE) denied: %lu\n", GetLastError());
    }
}

// ============================================================================
// Test 7: Named pipe connection test
// ============================================================================
static VOID TestNamedPipes() {
    printf("\n=== Test 7: Named pipe connection tests ===\n");
    const wchar_t* pipes[] = {
        L"\\\\.\\pipe\\spoolss",
        L"\\\\.\\pipe\\lsarpc",
        L"\\\\.\\pipe\\samr",
        L"\\\\.\\pipe\\netlogon",
        L"\\\\.\\pipe\\atsvc",
        L"\\\\.\\pipe\\eventlog",
        L"\\\\.\\pipe\\srvsvc",
        L"\\\\.\\pipe\\wkssvc",
        L"\\\\.\\pipe\\epmapper",
    };
    for (int i = 0; i < ARRAYSIZE(pipes); i++) {
        HANDLE h = CreateFileW(pipes[i], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            printf("[OPEN] %ls\n", pipes[i]);
            CloseHandle(h);
        } else {
            printf("[CLOSED] %ls (err=%lu)\n", pipes[i], GetLastError());
        }
    }
}

// ============================================================================
// Test 8: Windows Installer COM
// ============================================================================
static VOID TestInstaller() {
    printf("\n=== Test 8: WindowsInstaller.Installer ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"WindowsInstaller.Installer", &clsid);
    PrintHr("CLSIDFromProgID(WindowsInstaller)", hr);
    if (FAILED(hr)) return;

    IDispatch* pDisp = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&pDisp);
    PrintHr("CoCreateInstance(WindowsInstaller)", hr);
    if (pDisp) pDisp->Release();
}

// ============================================================================
// Test 9: BITS (Background Intelligent Transfer Service)
// ============================================================================
static VOID TestBits() {
    printf("\n=== Test 9: BackgroundCopyManager (BITS) ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"BackgroundCopyManager", &clsid);
    PrintHr("CLSIDFromProgID(BackgroundCopyManager)", hr);
    if (FAILED(hr)) return;

    IUnknown* pUnk = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER, IID_IUnknown, (void**)&pUnk);
    PrintHr("CoCreateInstance(BITS, IUnknown)", hr);
    if (pUnk) pUnk->Release();
}

// ============================================================================
// Test 10: ADODB.Connection
// ============================================================================
static VOID TestAdoDb() {
    printf("\n=== Test 10: ADODB.Connection ===\n");
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"ADODB.Connection", &clsid);
    PrintHr("CLSIDFromProgID(ADODB.Connection)", hr);
    if (FAILED(hr)) return;

    IDispatch* pDisp = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&pDisp);
    PrintHr("CoCreateInstance(ADODB.Connection)", hr);
    if (pDisp) pDisp->Release();
}

// ============================================================================
// Main
// ============================================================================
int main() {
    printf("============================================================\n");
    printf("COM / RPC Test Tool (Native C++, not PowerShell)\n");
    printf("PID: %lu\n", GetCurrentProcessId());
    printf("============================================================\n");

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("CoInitializeEx failed: 0x%08lX\n", hr);
        return 1;
    }

    TestNamedPipes();   // Test 7 first - pure RPC, no COM
    TestScm();          // Test 6 - SCM RPC
    TestFso();          // Test 1
    TestShellApp();     // Test 2
    TestWmi();          // Test 3
    TestTaskScheduler();// Test 4
    TestWshShell();     // Test 5
    TestInstaller();    // Test 8
    TestBits();         // Test 9
    TestAdoDb();        // Test 10

    CoUninitialize();
    printf("\n============================================================\n");
    printf("Done.\n");
    printf("============================================================\n");
    return 0;
}
