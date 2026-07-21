
#include "framework.h"

bool g_showConsole = false;
bool g_noInject = false;

void Wprintln(const std::wstring& s)
{
    if (!g_showConsole) return;
    std::wcout << s;
    std::wcout.flush();
}

// ---------------------------------------------------------------------------
// Path conversion: Win32 → NT (\Device\HarddiskVolumeX\...)
// ---------------------------------------------------------------------------
bool Win32ToNtPath(const std::wstring& win32, std::wstring& nt)
{
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
// NT path → Win32 path conversion (e.g. \Device\HarddiskVolume3\foo → C:\foo)
// ---------------------------------------------------------------------------
std::wstring NtPathToWin32(const std::wstring& ntPath)
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
// Check if a command-line tool exists via "where" command, return full path
// ---------------------------------------------------------------------------
bool KgFindTool(const wchar_t* tool, std::wstring& outPath)
{
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

    char ansiBuf[1024] = {};
    DWORD read = 0;
    ReadFile(hRead, ansiBuf, sizeof(ansiBuf) - 1, &read, NULL);
    CloseHandle(hRead);

    if (read > 0) {
        int wlen = MultiByteToWideChar(CP_ACP, 0, ansiBuf, (int)read, NULL, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wbuf(wlen + 1);
            MultiByteToWideChar(CP_ACP, 0, ansiBuf, (int)read, wbuf.data(), wlen);
            wbuf[wlen] = L'\0';
            // Take only the first line (before first \r or \n)
            wchar_t* nl = wcschr(wbuf.data(), L'\r');
            if (!nl) nl = wcschr(wbuf.data(), L'\n');
            if (nl) *nl = L'\0';
            outPath = wbuf.data();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Auto-detect VCS metadata directories (.svn / .git) for all bound paths
// Walk up parent directories to find the VCS root.
// Only runs if the corresponding VCS tool (svn/git) exists on the system.
// ---------------------------------------------------------------------------
void KgAutoProtectVcs(std::vector<std::wstring>& roList, const std::vector<std::wstring>& rwList)
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
        std::vector<std::wstring> found;
        for (auto& p : paths) {
            if (hasSvn) {
                std::wstring svnDir = findVcsMeta(p, L".svn");
                if (!svnDir.empty() &&
                    std::find(roList.begin(), roList.end(), svnDir) == roList.end() &&
                    std::find(found.begin(), found.end(), svnDir) == found.end()) {
                    found.push_back(svnDir);
                    Wprintln(L"bwrap.exe: Auto-protected " + svnDir + L" (read-only)\n");
                }
            }
            if (hasGit) {
                std::wstring gitDir = findVcsMeta(p, L".git");
                if (!gitDir.empty() &&
                    std::find(roList.begin(), roList.end(), gitDir) == roList.end() &&
                    std::find(found.begin(), found.end(), gitDir) == found.end()) {
                    found.push_back(gitDir);
                    Wprintln(L"bwrap.exe: Auto-protected " + gitDir + L" (read-only)\n");
                }
            }
        }
        for (auto& f : found)
            roList.push_back(f);
    };

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
std::wstring BuildCmdLine(const std::wstring& exe, wchar_t* const* args, int argCount)
{
    std::wstring line;
    if (exe.find(L' ') != std::wstring::npos)
        line = L"\"" + exe + L"\"";
    else
        line = exe;

    for (int i = 0; i < argCount; ++i) {
        std::wstring a = args[i];
        line += L" ";
        bool hasSpace = (a.find(L' ') != std::wstring::npos ||
                         a.find(L'\t') != std::wstring::npos);
        bool hasQuote = (a.find(L'"') != std::wstring::npos);
        if (!hasSpace && !hasQuote) {
            line += a;
            continue;
        }
        line += L'"';
        size_t j = 0;
        while (j < a.size()) {
            size_t backs = 0;
            while (j < a.size() && a[j] == L'\\') {
                backs++;
                j++;
            }
            if (j >= a.size()) {
                line.append(backs * 2, L'\\');
                break;
            }
            if (a[j] == L'"') {
                line.append(backs * 2 + 1, L'\\');
                line += L'"';
            } else {
                line.append(backs, L'\\');
                line += a[j];
            }
            j++;
        }
        line += L'"';
    }
    return line;
}

// ---------------------------------------------------------------------------
// Log invocation to file when --ses is used
// ---------------------------------------------------------------------------
void LogSession(const std::wstring& sid, int argc, wchar_t* argv[])
{
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
