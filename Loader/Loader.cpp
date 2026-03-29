#include <iostream>
#include <Windows.h>
#include <string>
#include <thread>
#include <TlHelp32.h>
#include <AclAPI.h>
#include <deque>
#include <sstream>
#include <iomanip>
#include <fstream>

#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

// =============================================================
// CONSTANTS
// =============================================================
static constexpr const wchar_t* TARGET_APP  = L"LiveCaptions.exe";

// Helper to get AppData path for logging
std::string GetLogPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::string fullPath = path;
        fullPath += "\\mslc_loader_debug.txt";
        return fullPath;
    }
    return "C:\\Users\\Public\\mslc_loader_debug.txt"; // Fallback
}

// =============================================================
// LOADER DEBUG LOGGER
// Keeps a rolling window of the last LOG_MAX_LINES log entries.
// Each write flushes the full ring to disk (file stays small: <=100 lines).
// Log file: %LOCALAPPDATA%\mslc_loader_debug.txt
// =============================================================
static const std::string     LOADER_LOG_PATH   = GetLogPath();
static constexpr size_t      LOG_MAX_LINES     = 100;
static CRITICAL_SECTION      g_logCs;
static std::deque<std::string> g_logRing;

void LogLoader(const char* category, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    // Format: [HH:MM:SS.mmm] [CATEGORY] msg
    std::ostringstream entry;
    entry << '['
          << std::setfill('0')
          << std::setw(2) << st.wHour   << ':'
          << std::setw(2) << st.wMinute << ':'
          << std::setw(2) << st.wSecond << '.'
          << std::setw(3) << st.wMilliseconds
          << "] ["
          << std::left << std::setw(8) << category
          << "] "
          << msg;

    EnterCriticalSection(&g_logCs);
    g_logRing.push_back(entry.str());
    if (g_logRing.size() > LOG_MAX_LINES) g_logRing.pop_front();

    // Rewrite full ring to disk on every call.
    // File is at most ~8KB (100 lines x ~80 chars) - acceptable for debug.
    std::ofstream f(LOADER_LOG_PATH, std::ios_base::trunc);
    if (f.is_open()) {
        for (const auto& line : g_logRing) f << line << '\n';
    }
    LeaveCriticalSection(&g_logCs);
}

// =============================================================
// PERMISSION & INJECTION
// =============================================================
void SetAppContainerPermission(const std::wstring& filePath) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea = { 0 };
    if (GetNamedSecurityInfoW(filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, const_cast<LPWSTR>(L"ALL APPLICATION PACKAGES"),
                                     GENERIC_READ | GENERIC_EXECUTE,
                                     SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        if (SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(filePath.c_str()),
                                  SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, pNewDACL, NULL);
            LocalFree(pNewDACL);
        }
        LocalFree(pSD);
    }
}

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0;

    // Phase 1 Mitigation: Dynamic API resolution and string obfuscation for Toolhelp32
    // "CreateToolhelp32Snapshot", "Process32FirstW", "Process32NextW"
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return 0;

    typedef HANDLE(WINAPI* CreateSnapshot_t)(DWORD, DWORD);
    typedef BOOL(WINAPI* ProcessFirst_t)(HANDLE, LPPROCESSENTRY32W);
    typedef BOOL(WINAPI* ProcessNext_t)(HANDLE, LPPROCESSENTRY32W);

    char p1[] = { 'C','r','e','a','t','e','T','o','o','l','h','e','l','p','3','2','S','n','a','p','s','h','o','t',0 };
    char p2[] = { 'P','r','o','c','e','s','s','3','2','F','i','r','s','t','W',0 };
    char p3[] = { 'P','r','o','c','e','s','s','3','2','N','e','x','t','W',0 };

    auto pCreateSnapshot = (CreateSnapshot_t)GetProcAddress(hKernel32, p1);
    auto pProcessFirst    = (ProcessFirst_t)GetProcAddress(hKernel32, p2);
    auto pProcessNext     = (ProcessNext_t)GetProcAddress(hKernel32, p3);

    if (!pCreateSnapshot || !pProcessFirst || !pProcessNext) return 0;

    HANDLE snapshot = pCreateSnapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = { sizeof(entry) };
        if (pProcessFirst(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, processName) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (pProcessNext(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return pid;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    SetAppContainerPermission(dllPath);

    // Phase 1 Mitigation: Use minimum required permissions instead of PROCESS_ALL_ACCESS
    DWORD dwDesiredAccess = PROCESS_CREATE_THREAD | 
                            PROCESS_QUERY_INFORMATION | 
                            PROCESS_VM_OPERATION | 
                            PROCESS_VM_WRITE | 
                            PROCESS_VM_READ;

    HANDLE hProcess = OpenProcess(dwDesiredAccess, FALSE, pid);
    if (!hProcess) {
        LogLoader("INJECT", "OpenProcess failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    // Phase 1 Mitigation: Dynamic API resolution and string obfuscation for injection APIs
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        CloseHandle(hProcess);
        return false;
    }

    typedef LPVOID(WINAPI* VirtualAllocEx_t)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
    typedef BOOL(WINAPI* WriteProcessMemory_t)(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T*);
    typedef HANDLE(WINAPI* CreateRemoteThread_t)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

    char v_a_e[] = { 'V','i','r','t','u','a','l','A','l','l','o','c','E','x',0 };
    char w_p_m[] = { 'W','r','i','t','e','P','r','o','c','e','s','s','M','e','m','o','r','y',0 };
    char c_r_t[] = { 'C','r','e','a','t','e','R','e','m','o','t','e','T','h','r','e','a','d',0 };
    char l_l_w[] = { 'L','o','a','d','L','i','b','r','a','r','y','W',0 };

    auto pVirtualAllocEx    = (VirtualAllocEx_t)GetProcAddress(hKernel32, v_a_e);
    auto pWriteProcessMemory = (WriteProcessMemory_t)GetProcAddress(hKernel32, w_p_m);
    auto pCreateRemoteThread = (CreateRemoteThread_t)GetProcAddress(hKernel32, c_r_t);
    auto pLoadLibraryW       = GetProcAddress(hKernel32, l_l_w);

    if (!pVirtualAllocEx || !pWriteProcessMemory || !pCreateRemoteThread || !pLoadLibraryW) {
        CloseHandle(hProcess);
        return false;
    }

    const size_t allocSize = (dllPath.length() + 1) * sizeof(wchar_t);
    void* remoteMem = pVirtualAllocEx(hProcess, nullptr, allocSize,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        LogLoader("INJECT", "VirtualAllocEx failed (err=" + std::to_string(GetLastError()) + ")");
        CloseHandle(hProcess);
        return false;
    }

    if (!pWriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), allocSize, nullptr)) {
        LogLoader("INJECT", "WriteProcessMemory failed (err=" + std::to_string(GetLastError()) + ")");
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = pCreateRemoteThread(hProcess, nullptr, 0,
                                        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
                                        remoteMem, 0, nullptr);
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return true;
    }

    LogLoader("INJECT", "CreateRemoteThread failed (err=" + std::to_string(GetLastError()) + ")");
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
}

// =============================================================
// MAIN ENTRY
// =============================================================
int main() {
    InitializeCriticalSection(&g_logCs);
    LogLoader("SESSION", "=== Loader started ===");

    // Resolve DLL path relative to this executable
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring strPath(exePath);
    std::wstring dllPath = strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\HookCore.dll";

    // Discovery and injection loop
    DWORD pid = 0;
    while (true) {
        pid = GetProcessIdByName(TARGET_APP);
        
        if (pid == 0) {
            // Process not found, wait and retry
            Sleep(2000);
            continue;
        }

        // Process found
        LogLoader("INJECT", "LiveCaptions.exe found (PID: " + std::to_string(pid) + ")");
        
        if (InjectDLL(pid, dllPath)) {
            // Injection succeeded
            LogLoader("INJECT", "HookCore.dll injected successfully");
            LogLoader("SESSION", "=== Loader exiting ===");
            break;
        } else {
            // Injection failed, retry
            LogLoader("INJECT", "Injection failed, retrying in 2 seconds");
            Sleep(2000);
        }
    }

    DeleteCriticalSection(&g_logCs);
    return 0;
}
