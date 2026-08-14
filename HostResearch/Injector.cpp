#include "Injector.h"
#include <TlHelp32.h>
#include <AclAPI.h>
#include <sddl.h>
#include <memory>

// Declare the external logger function implemented in Host.cpp
extern void LogHost(const char* category, const std::string& msg);

bool SetAppContainerPermission(const std::wstring& filePath) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea = { 0 };
    bool success = false;
    if (GetNamedSecurityInfoW(filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                               NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, const_cast<LPWSTR>(L"ALL APPLICATION PACKAGES"),
                                     GENERIC_READ | GENERIC_EXECUTE,
                                     SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        if (SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
            if (SetNamedSecurityInfoW(const_cast<LPWSTR>(filePath.c_str()),
                                  SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, pNewDACL, NULL) == ERROR_SUCCESS) {
                success = true;
            }
            LocalFree(pNewDACL);
        }
        LocalFree(pSD);
    }
    return success;
}

bool SetAppContainerWritePermission(const std::wstring& filePath) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea = { 0 };
    bool success = false;
    if (GetNamedSecurityInfoW(filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                               NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, const_cast<LPWSTR>(L"ALL APPLICATION PACKAGES"),
                                     GENERIC_READ | GENERIC_WRITE | GENERIC_ALL,
                                     SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        if (SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
            if (SetNamedSecurityInfoW(const_cast<LPWSTR>(filePath.c_str()),
                                  SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, pNewDACL, NULL) == ERROR_SUCCESS) {
                success = true;
            }
            LocalFree(pNewDACL);
        }
        LocalFree(pSD);
    }
    return success;
}

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0;

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

bool IsDLLAlreadyInjected(DWORD pid, const std::wstring& dllName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    return found;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    if (!SetAppContainerPermission(dllPath)) {
        LogHost("WARN", "SetAppContainerPermission failed for Agent.dll. Injection might fail if process runs in AppContainer.");
    }

    DWORD dwDesiredAccess = PROCESS_CREATE_THREAD | 
                            PROCESS_QUERY_INFORMATION | 
                            PROCESS_VM_OPERATION | 
                            PROCESS_VM_WRITE | 
                            PROCESS_VM_READ;

    SafeHandle shProcess(OpenProcess(dwDesiredAccess, FALSE, pid));
    if (!shProcess.IsValid()) {
        LogHost("INJECT", "OpenProcess failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return false;

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
        return false;
    }

    const size_t allocSize = (dllPath.length() + 1) * sizeof(wchar_t);

    struct RemoteAllocDeleter {
        HANDLE hProc;
        void operator()(void* ptr) const { if (ptr) VirtualFreeEx(hProc, ptr, 0, MEM_RELEASE); }
    };

    void* remoteMem = pVirtualAllocEx(shProcess.Get(), nullptr, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        LogHost("INJECT", "VirtualAllocEx failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }
    std::unique_ptr<void, RemoteAllocDeleter> remoteMemGuard(remoteMem, RemoteAllocDeleter{ shProcess.Get() });

    if (!pWriteProcessMemory(shProcess.Get(), remoteMem, dllPath.c_str(), allocSize, nullptr)) {
        LogHost("INJECT", "WriteProcessMemory failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    SafeHandle shThread(pCreateRemoteThread(shProcess.Get(), nullptr, 0,
                                           reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
                                           remoteMem, 0, nullptr));
    if (!shThread.IsValid()) {
        LogHost("INJECT", "CreateRemoteThread failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    WaitForSingleObject(shThread.Get(), INFINITE);
    return true;
}
