#include <iostream>
#include <Windows.h>
#include <string>
#include <thread>
#include <TlHelp32.h>
#include <AclAPI.h>
#include <fcntl.h>
#include <io.h>

// =============================================================
// PERMISSION & INJECTION
// =============================================================
void SetAppContainerPermission(const std::wstring& filePath) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea = { 0 };
    if (GetNamedSecurityInfoW(filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, (LPWSTR)L"ALL APPLICATION PACKAGES", GENERIC_READ | GENERIC_EXECUTE, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        if (SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW((LPWSTR)filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDACL, NULL);
            LocalFree(pNewDACL);
        }
        LocalFree(pSD);
    }
}

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = { sizeof(entry) };
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, processName) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return pid;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    SetAppContainerPermission(dllPath);
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) return false;
    size_t size = (dllPath.length() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProcess, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteMem) {
        WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), size, nullptr);
        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC pLoadLib = GetProcAddress(hKernel32, "LoadLibraryW");
        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)pLoadLib, remoteMem, 0, nullptr);
        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
            CloseHandle(hProcess);
            return true;
        }
    }
    CloseHandle(hProcess);
    return false;
}

// =============================================================
// PIPE SERVER - Lắng nghe kết quả từ DLL (JSON Parse tay)
// =============================================================
void PipeListener() {
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    while (true) {
        HANDLE hPipe = CreateNamedPipeW(L"\\\\.\\pipe\\LiveCaptionPipe",
            PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 65536, 65536, 0, &sa);

        if (hPipe != INVALID_HANDLE_VALUE) {
            std::wcout << L"[i] Dang doi ket noi tu LiveCaptions..." << std::endl;

            if (ConnectNamedPipe(hPipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
                wchar_t buffer[8192];
                DWORD bytesRead;
                size_t lastTextLen = 0; // Để ghi đè text thừa mượt mà hơn

                while (ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL) && bytesRead > 0) {
                    buffer[bytesRead / sizeof(wchar_t)] = L'\0';
                    std::wstring data(buffer);

                    // Parse JSON cơ bản
                    bool isFinal = (data.find(L"\"is_final\": true") != std::wstring::npos);

                    std::wstring text = L"";
                    size_t textPos = data.find(L"\"text\": \"");
                    if (textPos != std::wstring::npos) {
                        textPos += 9;
                        size_t endPos = data.find(L"\"}", textPos);
                        if (endPos != std::wstring::npos) {
                            text = data.substr(textPos, endPos - textPos);
                        }
                    }

                    if (!text.empty()) {
                        std::wstring padding(lastTextLen > text.length() ? lastTextLen - text.length() : 0, L' ');

                        if (isFinal) {
                            // Câu chốt: In ra và xuống dòng
                            std::wcout << L"\r[FINAL]: " << text << padding << std::endl;
                            lastTextLen = 0;
                        }
                        else {
                            // Đang nghe: Ghi đè lên dòng hiện tại
                            std::wcout << L"\r[Partial]: " << text << padding << std::flush;
                            lastTextLen = text.length();
                        }
                    }
                }
            }
            std::wcout << L"\n[!] Pipe disconnected. Reconnecting..." << std::endl;
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
    }
}

// =============================================================
// MAIN ENTRY
// =============================================================
int main() {
    // Ép console xuất unicode chuẩn
    _setmode(_fileno(stdout), _O_U16TEXT);
    std::wcout << L"=== Live Caption Extractor [Core API Hooking] ===" << std::endl;

    // Chạy thread lắng nghe Named Pipe
    std::thread(PipeListener).detach();

    const wchar_t* targetApp = L"LiveCaptions.exe";
    DWORD pid = GetProcessIdByName(targetApp);

    // Mở Live Caption nếu chưa chạy
    if (pid == 0) {
        std::wcout << L"[i] Opening Live Captions..." << std::endl;
        ShellExecuteW(NULL, L"open", L"ms-settings:privacy-livecaptions", NULL, NULL, SW_SHOWNORMAL);
        while (pid == 0) {
            pid = GetProcessIdByName(targetApp);
            Sleep(1000);
        }
    }

    // Lấy đường dẫn DLL
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring strPath(exePath);
    std::wstring dllPath = strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\HookCore.dll";

    // Thực hiện Inject
    if (InjectDLL(pid, dllPath)) {
        std::wcout << L"[+] Injected HookCore.dll successfully!" << std::endl;
    }
    else {
        std::wcout << L"[-] Injection Failed. Kiem tra lai duong dan DLL: " << dllPath << std::endl;
    }

    // Giữ cho Loader luôn chạy
    while (true) {
        Sleep(1000);
    }

    return 0;
}