#include "ProcessMonitor.h"
#include "HostLogger.h"
#include "PipeServer.h"   // g_exitHost
#include "SafeHandle.h"

#include <TlHelp32.h>
#include <sstream>
#include <algorithm>
#include <cctype>

// Time to wait after launching before probing for the new PID
static constexpr DWORD LAUNCH_SETTLE_MS  = 2500;
// WaitForSingleObject poll interval while watching
static constexpr DWORD WATCH_POLL_MS     = 500;
// Known packaged-app shell folder alias for LiveCaptions
static constexpr const wchar_t* LC_SHELL_ALIAS =
    L"shell:AppsFolder\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy!LiveCaptions";

// ---------------------------------------------------------------
OnExitAction ParseOnExitAction(const std::string& s) {
    std::string lo = s;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (lo == "relaunch") return OnExitAction::Relaunch;
    if (lo == "reinject") return OnExitAction::Reinject;
    return OnExitAction::Quit; // default + unknown
}

// ---------------------------------------------------------------
std::vector<DWORD> FindAllProcessInstances(const wchar_t* exeName) {
    std::vector<DWORD> pids;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LogHost("MONITOR", "FindAllProcessInstances: snapshot failed (err=" +
                std::to_string(GetLastError()) + ")");
        return pids;
    }

    PROCESSENTRY32W entry = { sizeof(entry) };
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0) {
                pids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pids;
}

// ---------------------------------------------------------------
// Internal: wait LAUNCH_SETTLE_MS then probe snapshot for first PID.
static DWORD ProbeAfterLaunch() {
    Sleep(LAUNCH_SETTLE_MS);
    auto pids = FindAllProcessInstances(L"LiveCaptions.exe");
    return pids.empty() ? 0 : pids.front();
}

bool LaunchLiveCaptions(DWORD* outPid) {
    if (outPid) *outPid = 0;

    // --- Strategy 1: CreateProcessW from system32 ---
    wchar_t sysDir[MAX_PATH] = {};
    if (GetSystemDirectoryW(sysDir, MAX_PATH)) {
        std::wstring exePath = std::wstring(sysDir) + L"\\LiveCaptions.exe";

        STARTUPINFOW        si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(exePath.c_str(), nullptr,
                           nullptr, nullptr,
                           FALSE, 0,
                           nullptr, nullptr,
                           &si, &pi)) {
            DWORD pid = pi.dwProcessId;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Sleep(LAUNCH_SETTLE_MS);
            if (outPid) *outPid = pid;
            LogHost("LAUNCH", "Strategy 1 (CreateProcess system32) succeeded. PID=" +
                    std::to_string(pid));
            return true;
        }
        LogHost("LAUNCH", "Strategy 1 (CreateProcess system32) failed err=" +
                std::to_string(GetLastError()) + ". Trying ShellExecute on same path.");

        // --- Strategy 2: ShellExecuteW on system32 path ---
        HINSTANCE hInst = ShellExecuteW(NULL, L"open",
                                         exePath.c_str(),
                                         nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(hInst) > 32) {
            DWORD pid = ProbeAfterLaunch();
            if (pid != 0) {
                if (outPid) *outPid = pid;
                LogHost("LAUNCH", "Strategy 2 (ShellExecute system32) succeeded. PID=" +
                        std::to_string(pid));
                return true;
            }
            LogHost("LAUNCH", "Strategy 2: ShellExecute returned OK but process not found after settle.");
        } else {
            LogHost("LAUNCH", "Strategy 2 (ShellExecute system32) failed hInst=" +
                    std::to_string(reinterpret_cast<INT_PTR>(hInst)));
        }
    }

    // --- Strategy 3: ShellExecuteW via packaged-app alias ---
    LogHost("LAUNCH", "Strategy 3: ShellExecute via shell:AppsFolder alias.");
    HINSTANCE hInst = ShellExecuteW(NULL, L"open",
                                     LC_SHELL_ALIAS,
                                     nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) > 32) {
        DWORD pid = ProbeAfterLaunch();
        if (pid != 0) {
            if (outPid) *outPid = pid;
            LogHost("LAUNCH", "Strategy 3 (shell alias) succeeded. PID=" +
                    std::to_string(pid));
            return true;
        }
        LogHost("LAUNCH", "Strategy 3: alias returned OK but process not found after settle.");
    } else {
        LogHost("LAUNCH", "Strategy 3 (shell alias) failed hInst=" +
                std::to_string(reinterpret_cast<INT_PTR>(hInst)));
    }

    LogHost("LAUNCH", "All launch strategies exhausted. LiveCaptions could not be started.");
    return false;
}

// ---------------------------------------------------------------
bool WatchProcessUntilExit(DWORD pid) {
    SafeHandle shProc(OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid));

    if (!shProc.IsValid()) {
        LogHost("WATCH", "OpenProcess failed for PID " + std::to_string(pid) +
                " (err=" + std::to_string(GetLastError()) + "). Treating as exited.");
        return true;
    }

    LogHost("WATCH", "Watching PID " + std::to_string(pid) + " for exit...");

    while (!g_exitHost) {
        DWORD res = WaitForSingleObject(shProc.Get(), WATCH_POLL_MS);
        if (res == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(shProc.Get(), &exitCode);
            LogHost("WATCH", "PID " + std::to_string(pid) +
                    " exited (exit_code=" + std::to_string(exitCode) + ")");
            return true;
        }
        // WAIT_TIMEOUT → process still alive, loop
    }

    LogHost("WATCH", "Watch loop interrupted by g_exitHost for PID " +
            std::to_string(pid));
    return false;
}
