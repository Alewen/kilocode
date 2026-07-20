#include "AppX.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>

// ---------------------------------------------------------------------------
// Run "where.exe exeName" and return all found paths
// ---------------------------------------------------------------------------
static std::vector<std::wstring> FindExeInPath(const std::wstring& exeName)
{
    std::vector<std::wstring> results;
    if (exeName.empty()) return results;

    std::wstring cmd = L"cmd.exe /c where " + exeName + L" 2>nul";

    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&hRead, &hWrite, &sa, 4096)) return results;

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = NULL;

    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, cmdbuf.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        CloseHandle(hRead); CloseHandle(hWrite);
        return results;
    }
    CloseHandle(hWrite);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char buf[8192] = {};
    DWORD read = 0;
    ReadFile(hRead, buf, (DWORD)sizeof(buf) - 1, &read, NULL);
    CloseHandle(hRead);

    if (read == 0) return results;

    int wlen = MultiByteToWideChar(CP_ACP, 0, buf, (int)read, NULL, 0);
    std::vector<wchar_t> wbuf(wlen + 1);
    MultiByteToWideChar(CP_ACP, 0, buf, (int)read, wbuf.data(), wlen);
    wbuf[wlen] = L'\0';

    wchar_t* line = wbuf.data();
    wchar_t* end = wbuf.data() + wlen;
    while (line < end)
    {
        while (line < end && (*line == L'\r' || *line == L'\n')) line++;
        wchar_t* start = line;
        while (line < end && *line != L'\r' && *line != L'\n') line++;
        if (line > start)
        {
            std::wstring path(start, line - start);
            results.push_back(path);
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// Determine if a file path contains an App Execution Alias
// Uses file attributes + size: alias files are reparse points with size 0
// ---------------------------------------------------------------------------
bool IsAppExecutionAlias(const std::wstring& filePath)
{
    DWORD attr = GetFileAttributesW(filePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// ---------------------------------------------------------------------------
// Parse the App Execution Alias reparse point to extract the target path.
// Returns the real exe path, or empty string on failure.
// ---------------------------------------------------------------------------
static std::wstring GetAppExecTarget(const std::wstring& aliasPath)
{
    HANDLE h = CreateFileW(aliasPath.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    if (h == INVALID_HANDLE_VALUE) return L"";

    BYTE buf[4096] = {};
    DWORD bytesRet = 0;
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT,
        NULL, 0, buf, sizeof(buf), &bytesRet, NULL))
    {
        CloseHandle(h);
        return L"";
    }
    CloseHandle(h);

    if (bytesRet < 8) return L"";

    ULONG tag = *(ULONG*)buf;
    // IO_REPARSE_TAG_APPEXEC
    if (tag != 0x8000001B) return L"";

    // AppExecLink version 3:
    // Offset 8: reparse data starts with Version (ULONG = 3)
    // Then concatenated null-terminated WCHAR strings.
    // String[0] = PackageFamilyName
    // String[1] = PackageFullName
    // String[2] = Target executable path  <-- we want this
    const wchar_t* p = (const wchar_t*)(buf + 8 + 4);
    const wchar_t* end = (const wchar_t*)(buf + bytesRet);

    int strIndex = 0;
    while (p < end)
    {
        size_t len = wcslen(p);
        if (len == 0) break;
        if (strIndex == 2) return std::wstring(p, len);
        p += len + 1;
        strIndex++;
    }
    return L"";
}

// ---------------------------------------------------------------------------
// Resolve a single App Execution Alias name to the folder that contains its
// real target executable.
//   name : "pwsh" / "pwsh.exe" / "python" / "wt.exe" ... (".exe" auto-appended)
// Looks ONLY in %LOCALAPPDATA%\Microsoft\WindowsApps\<name>.
// Returns L"" if the alias stub does not exist or cannot be resolved;
// otherwise the directory of the real target exe (the "真身" package dir).
// ---------------------------------------------------------------------------
std::wstring ResolveAliasTargetDir(const std::wstring& name)
{
    if (name.empty()) return L"";

    // Normalize: append ".exe" if the name has no ".exe" suffix (case-insensitive)
    std::wstring exeName = name;
    bool hasExe = false;
    if (exeName.size() >= 4)
    {
        std::wstring tail = exeName.substr(exeName.size() - 4);
        for (auto& c : tail) c = towlower(c);
        hasExe = (tail == L".exe");
    }
    if (!hasExe) exeName += L".exe";

    // %LOCALAPPDATA%\Microsoft\WindowsApps\<name>
    WCHAR localAppData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";

    std::wstring aliasPath =
        std::wstring(localAppData) + L"\\Microsoft\\WindowsApps\\" + exeName;

    // The alias stub must exist in the WindowsApps directory
    if (GetFileAttributesW(aliasPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return L"";

    // Resolve the APPEXECLINK reparse point to the real target exe
    std::wstring target = GetAppExecTarget(aliasPath);
    if (target.empty()) return L"";

    // Return the folder containing the real target exe
    size_t pos = target.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    return target.substr(0, pos);
}

// ---------------------------------------------------------------------------
// Find first PATH match via "where.exe"
// ---------------------------------------------------------------------------
std::wstring FindExePathFromPath(const std::wstring& exeName)
{
    auto found = FindExeInPath(exeName);
    return found.empty() ? L"" : found[0];
}
