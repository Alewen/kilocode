
#include "DriverClient.h"

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
static void ShowHelp()
{
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
        << "  --cwd <path>              Set working directory for child process\n"
        << "  -h, --help          Show this help message\n"
        << "\n"
        << "Always denied (escape tools):\n"
        << "  schtasks.exe wmic.exe bitsadmin.exe regsvr32.exe\n"
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
int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        ShowHelp();
        return 0;
    }

    std::wstring first = argv[1];
    if (first == L"--help" || first == L"-help" || first == L"-h" ||
        first == L"--h" || first == L"/?")
    {
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
    std::wstring cwdUser;
    bool userBind = false;
    bool userRoBind = false;

    int i = 1;
    for (; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--")
        {
            ++i;
            break;
        }

        auto drain = [&](std::vector<std::wstring>& list)
            {
                while (++i < argc)
                {
                    std::wstring next = argv[i];
                    if (next == L"--" ||
                        (next.size() >= 2 && next[0] == L'-' && next[1] == L'-'))
                    {
                        --i; break;
                    }
                    list.push_back(next);
                }
                if (list.empty())
                {
                    std::wcerr << L"bwrap.exe: " << a << L" requires at least one path\n";
                    std::wcerr << L"Try --help for usage.\n";
                    ExitProcess(1);
                }
            };

        if (a == L"--ro-bind")
        {
            userRoBind = true;
            drain(roList);
        }
        else if (a == L"--bind")
        {
            userBind = true;
            drain(rwList);
        }
        else if (a == L"--tmpfs")
        {
            drain(denyList);
        }
        else if (a == L"--net-allow")
        {
            drain(netAllowList);
        }
        else if (a == L"--alias")
        {
            drain(aliasList);
        }
        else if (a == L"--showbox")
        {
            showbox = true;
        }
        else if (a == L"--showConsole")
        {
            g_showConsole = true;
        }
        else if (a == L"--no-inject")
        {
            g_noInject = true;
        }
        else if (a == L"--ses")
        {
            if (++i >= argc || std::wstring(argv[i]).find(L"--") == 0)
            {
                std::wcerr << L"bwrap.exe: --ses requires a session ID\n";
                ExitProcess(1);
            }
            ses = argv[i];
        }
        else if (a == L"--cwd")
        {
            if (++i >= argc)
            {
                std::wcerr << L"bwrap.exe: --cwd requires a directory path\n";
                ExitProcess(1);
            }
            cwdUser = argv[i];
        }
        else
        {
            std::wcerr << L"bwrap.exe: unknown flag: " << a << L"\n";
            std::wcerr << L"Try --help for usage.\n";
            return 1;
        }
    }

    if (!ses.empty())
    {
        LogSession(ses, argc, argv);
    }

    // -----------------------------------------------------------------------
    // Implicitly deny known escape tools (both 64-bit and 32-bit paths)
    // -----------------------------------------------------------------------
    {
        WCHAR winDir[MAX_PATH];
        UINT winDirLen = GetWindowsDirectoryW(winDir, MAX_PATH);
        if (winDirLen > 0 && winDirLen < MAX_PATH)
        {
            std::wstring wd = winDir;
            auto addDenyIfExists = [&](const std::wstring& relPath)
                {
                    std::wstring full = wd + L"\\" + relPath;
                    DWORD attr = GetFileAttributesW(full.c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES)
                    {
                        denyList.push_back(full);
                    }
                };
            addDenyIfExists(L"System32\\schtasks.exe");
            addDenyIfExists(L"SysWOW64\\schtasks.exe");
            addDenyIfExists(L"System32\\wbem\\wmic.exe");
            addDenyIfExists(L"SysWOW64\\wbem\\wmic.exe");
            addDenyIfExists(L"System32\\bitsadmin.exe");
            addDenyIfExists(L"SysWOW64\\bitsadmin.exe");
            addDenyIfExists(L"System32\\regsvr32.exe");
            addDenyIfExists(L"SysWOW64\\regsvr32.exe");
            /*addDenyIfExists(L"System32\\cscript.exe");
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
        }
    }

    if (i >= argc)
    {
        std::wcerr << L"bwrap.exe: missing '--' separator and command.\n";
        std::wcerr << L"Try --help for usage.\n";
        return 1;
    }

    std::wstring exe = argv[i];

    // -----------------------------------------------------------------------
    // Phase 1: Convert all Win32 paths to NT paths
    // -----------------------------------------------------------------------
    auto toNt = [](const std::vector<std::wstring>& paths,
        std::vector<std::wstring>& out)
        {
            for (auto& p : paths)
            {
                std::wstring nt;
                if (!Win32ToNtPath(p, nt))
                {
                    std::wcerr << L"bwrap.exe: cannot resolve path: " << p << L"\n";
                    ExitProcess(1);
                }
                out.push_back(nt);
            }
        };

    // Auto-add bwrap.exe's own directory as read-only (for KiloHook.dll access)
    {
        WCHAR selfPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, selfPath, MAX_PATH))
        {
            std::wstring selfDir(selfPath);
            size_t pos = selfDir.find_last_of(L'\\');
            if (pos != std::wstring::npos)
            {
                selfDir = selfDir.substr(0, pos);
                if (std::find(roList.begin(), roList.end(), selfDir) == roList.end())
                {
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
        if (n > 0 && n < MAX_PATH)
        {
            std::wstring winApps = std::wstring(localAppData) + L"\\Microsoft\\WindowsApps";
            if (GetFileAttributesW(winApps.c_str()) != INVALID_FILE_ATTRIBUTES &&
                std::find(roList.begin(), roList.end(), winApps) == roList.end())
            {
                roList.push_back(winApps);
                Wprintln(L"bwrap.exe: Auto-bind alias directory " + winApps + L" (read-only)\n");
            }
        }

        // "pwsh" is always resolved by default, then any user --alias names.
        std::vector<std::wstring> effectiveAliases;
        effectiveAliases.push_back(L"pwsh");
        for (auto& a : aliasList) effectiveAliases.push_back(a);

        for (auto& name : effectiveAliases)
        {
            std::wstring dir = ResolveAliasTargetDir(name);
            if (dir.empty())
            {
                WCHAR nbuf[512];
                int nlen = swprintf_s(nbuf, ARRAYSIZE(nbuf),
                    L"bwrap.exe: alias %s not found in WindowsApps, skipped\n", name.c_str());
                if (nlen > 0) Wprintln(nbuf);
                continue;
            }
            if (std::find(roList.begin(), roList.end(), dir) == roList.end())
            {
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
            std::find(roList.begin(), roList.end(), appRepo) == roList.end())
        {
            roList.push_back(appRepo);
            Wprintln(L"bwrap.exe: Auto-bind AppRepository " + appRepo + L" (read-only)\n");
        }
    }

    // Auto-bind %TEMP% as read-write (for child process scratch files)
    {
        WCHAR tempDir[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"TEMP", tempDir, MAX_PATH);
        if (n > 0 && n < MAX_PATH &&
            std::find(rwList.begin(), rwList.end(), tempDir) == rwList.end())
        {
            rwList.push_back(tempDir);
            Wprintln(L"bwrap.exe: Auto-bind TEMP directory " + std::wstring(tempDir) + L" (read-write)\n");
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
    auto addRule = [&](const std::wstring& nt, KG_SANDBOX_LEVEL level)
        {
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
        if (GetWindowsDirectoryW(winDir, MAX_PATH))
        {
            std::wstring nt;
            if (Win32ToNtPath(winDir, nt))
            {
                addRule(nt, 1);
            }
        }
    }

    drv.AddRules(rules);

    // --net-allow paths: convert to NT and send to driver
    if (!netAllowList.empty())
    {
        std::vector<std::wstring> netAllowNt;
        for (auto& p : netAllowList)
        {
            std::wstring nt;
            if (!Win32ToNtPath(p, nt))
            {
                std::wcerr << L"bwrap.exe: cannot resolve path: " << p << L"\n";
                ExitProcess(1);
            }
            netAllowNt.push_back(nt);
        }
        drv.SetNetExeList(netAllowNt);
        for (auto& nt : netAllowNt)
            Wprintln(L"bwrap.exe: Net-whitelisted " + nt + L"\n");
    }

    // Deny log: enable only when --showConsole is set
    drv.SetDenyLogEnabled(sid, g_showConsole ? TRUE : FALSE);

    // Attach bwrap itself to the sandbox.
    // Subsequent CreateProcess will have bwrap as parent, which IS in PidMap,
    // so child processes automatically inherit sandbox via KiloProcessNotify.
    drv.AttachProcess(GetCurrentProcessId(), sid);
    WCHAR abuf[128];
    int alen = swprintf_s(abuf, ARRAYSIZE(abuf), L"bwrap.exe: Attached own PID %lu to sandbox SID=%lu\n",
        GetCurrentProcessId(), sid);
    if (alen > 0)
    {
        Wprintln(abuf);
    }

    if (showbox)
    {
        std::cout << "Enter Sandbox SID=" << sid << std::endl;
    }

    // Set KiloHook.dll path from bwrap.exe's own directory for injection
    {
        WCHAR selfPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, selfPath, MAX_PATH))
        {
            std::wstring selfDir(selfPath);
            size_t pos = selfDir.find_last_of(L'\\');
            if (pos != std::wstring::npos)
            {
                selfDir = selfDir.substr(0, pos);
                std::wstring dllPath = selfDir + L"\\KiloHook.dll";
                if (GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    drv.SetHookDllPath(dllPath);
                else
                {
                    WCHAR dbuf[512];
                    int dlen = swprintf_s(dbuf, ARRAYSIZE(dbuf), L"bwrap.exe: KiloHook.dll not found at %s\n", dllPath.c_str());
                    if (dlen > 0) Wprintln(dbuf);
                }
            }
        }
    }

    if (g_showConsole)
    {
        drv.StartNotifyThread();
    }
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

        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::wstring shortName = exe;
            size_t s = shortName.find_last_of(L"\\/");
            if (s != std::wstring::npos)
                shortName = shortName.substr(s + 1);

            std::wstring foundPath = FindExePathFromPath(shortName);
            if (!foundPath.empty())
            {
                exe = foundPath;
                if (IsAppExecutionAlias(exe))
                    Wprintln(L"bwrap.exe: Resolved AppExec alias " + shortName + L" -> " + exe + L"\n");
                else
                    Wprintln(L"bwrap.exe: Resolved from PATH: " + shortName + L" -> " + exe + L"\n");
            }
            else
            {
                std::wcerr << L"bwrap.exe: command not found: " << shortName << L"\n";
                return 1;
            }
        }
    }

    auto cmdLine = BuildCmdLine(exe, argv + i + 1, argc - i - 1);
    std::wstring exeLower = exe;
    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

    auto hasExt = [&](const wchar_t* ext) -> bool
        {
            size_t elen = wcslen(ext);
            return exeLower.size() >= elen &&
                exeLower.substr(exeLower.size() - elen) == ext;
        };

    if (hasExt(L".exe") || hasExt(L".com") || hasExt(L".scr"))
    {
        // Direct PE launch — use as-is
    }
    else if (hasExt(L".bat") || hasExt(L".cmd"))
    {
        cmdLine = L"cmd.exe /c " + cmdLine;
    }
    else if (hasExt(L".ps1") || hasExt(L".psm1"))
    {
        std::wstring shell;
        if (KgFindTool(L"pwsh.exe", shell))
            cmdLine = (shell.find(L' ') == std::wstring::npos ? shell : L"\"" + shell + L"\"") + L" -File " + cmdLine;
        else if (KgFindTool(L"powershell.exe", shell))
            cmdLine = (shell.find(L' ') == std::wstring::npos ? shell : L"\"" + shell + L"\"") + L" -File " + cmdLine;
        else
        {
            std::wcerr << L"bwrap.exe: cannot execute " << exe
                << L" — no pwsh.exe or powershell.exe found\n";
            return 1;
        }
    }
    else
    {
        std::wcerr << L"bwrap.exe: cannot execute " << exe << L" — unsupported file extension\n";
        return 1;
    }

    std::wstring cwd;
    if (!cwdUser.empty())
    {
        DWORD attr = GetFileAttributesW(cwdUser.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            cwd = cwdUser;
    }
    if (cwd.empty() && userBind && !rwList.empty())
        cwd = rwList[0];
    if (cwd.empty() && userRoBind && !roList.empty())
        cwd = roList[0];
    if (cwd.empty())
    {
        if (!rwList.empty())
            cwd = rwList[0];
        else if (!roList.empty())
            cwd = roList[0];
    }
    LPCWSTR lpCwd = cwd.empty() ? NULL : cwd.c_str();

    std::vector<wchar_t> cmdbuf(cmdLine.begin(), cmdLine.end());
    cmdbuf.push_back(L'\0');

    // Create Job Object — auto-terminate child tree on bwrap exit/crash
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (hJob)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo)))
        {
            CloseHandle(hJob);
            hJob = NULL;
        }
    }

    STARTUPINFOW si = {sizeof(si)};
    UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(NULL, cmdbuf.data(), NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, lpCwd, &si, &pi);
    SetErrorMode(oldErrorMode);

    if (!ok)
    {
        if (hJob) CloseHandle(hJob);
        std::wcerr << L"bwrap.exe: CreateProcess failed (" << GetLastError() << L") for: " << cmdLine << L"\n";
        return 1;
    }
    else
    {
        WCHAR cbuf[512];
        int clen = swprintf_s(cbuf, ARRAYSIZE(cbuf), L"bwrap.exe: Created process PID=%lu ProcessName=%s\n", pi.dwProcessId, exe.c_str());
        if (clen > 0) Wprintln(cbuf);
    }

    // Child inherits sandbox automatically (KiloProcessNotify sees bwrap in PidMap)
    // and driver sends CREATE notification via port. No manual AttachProcess needed.

    // Assign to Job Object
    if (hJob)
    {
        if (!AssignProcessToJobObject(hJob, pi.hProcess))
        {
            std::wcerr << L"bwrap.exe: AssignProcessToJobObject failed (" << GetLastError() << L")" << std::endl;
        }
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
    if (hJob)
    {
        CloseHandle(hJob);
    }

    return (int)ec;
}
