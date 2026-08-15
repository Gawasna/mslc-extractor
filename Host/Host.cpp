#include <iostream>
#include <Windows.h>
#include <string>
#include <thread>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

#include "AppConfig.h"
#include "HostLogger.h"
#include "ConsoleRenderer.h"
#include "PacketParser.h"
#include "PipeServer.h"
#include "TextProcessor.h"
#include "Injector.h"
#include "ProcessMonitor.h"

static constexpr const wchar_t* TARGET_APP   = L"LiveCaptions.exe";
static constexpr int            MAX_INJECT_RETRIES = 3;

// ---------------------------------------------------------------
// Ctrl+C handler — sets g_exitHost for a clean shutdown
// ---------------------------------------------------------------
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT) {
        LogHost("HOST", "Ctrl+C / close signal received. Initiating shutdown.");
        g_exitHost = true;
        g_queueCv.notify_all();
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------
// Resolve DLL path relative to Host.exe location
// ---------------------------------------------------------------
static std::wstring ResolveDllPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring strPath(exePath);
    return strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\Agent.dll";
}

// ---------------------------------------------------------------
// Duplicate-process check at startup.
// Logs a warning and returns the first PID to use.
// Returns 0 if no instance found.
// ---------------------------------------------------------------
static DWORD CheckAndResolveDuplicates() {
    auto pids = FindAllProcessInstances(TARGET_APP);
    if (pids.size() > 1) {
        std::string pidList;
        for (DWORD p : pids) pidList += std::to_string(p) + " ";
        LogHost("WARN", "Multiple LiveCaptions.exe instances detected: [" + pidList + "]. "
                        "Using first PID. Exit code 4 is non-fatal.");
        if (!g_stdoutOnly && !g_silent)
            std::cout << "[!] WARNING: Multiple LiveCaptions.exe instances found ("
                      << pids.size() << "). Using PID " << pids.front() << ".\n";
    }
    return pids.empty() ? 0 : pids.front();
}

// ---------------------------------------------------------------
// Discover or launch LiveCaptions.exe.
// Returns PID on success, 0 on failure.
// Sets exitCode on unrecoverable failure.
// ---------------------------------------------------------------
static DWORD AcquireTargetPid(HostExitCode& exitCode, bool& settingsOpened) {
    DWORD pid = g_targetPid.load();
    if (pid != 0) return pid;

    auto pids = FindAllProcessInstances(TARGET_APP);
    if (!pids.empty()) return pids.front();

    // Not found — decide how to proceed
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
    if (g_autoLaunch) {
        if (!g_stdoutOnly && !g_injectOnly && !g_silent)
            std::cout << "[*] LiveCaptions.exe not found. Attempting auto-launch...\n";
        DWORD newPid = 0;
        if (!LaunchLiveCaptions(&newPid)) {
            LogHost("HOST", "--auto-launch failed. No viable launch strategy succeeded.");
            exitCode = HostExitCode::LaunchFailed;
            return 0;
        }
        // LiveCaptions launched by a non-UI parent often starts with suspended threads
        // and no visible window (AppContainer + WDM initialization deferral).
        // WakeupSuspendedProcess resumes those threads and attempts window restoration.
        WakeupSuspendedProcess(newPid);
        return newPid;
    }
#endif

    // Open Settings page once to prompt user to enable Live Captions
    if (!g_noSpawn && !settingsOpened) {
        ShellExecuteW(NULL, L"open", L"ms-settings:easeofaccess-audio",
                      NULL, NULL, SW_SHOWNORMAL);
        LogHost("HOST", "Opened ms-settings to prompt LiveCaptions activation.");
        settingsOpened = true;
    }
    return 0; // will retry on next outer loop iteration
}

// ---------------------------------------------------------------
// Injection phase. Returns true when injection is confirmed.
// Sets exitCode + g_exitHost on unrecoverable failure.
// ---------------------------------------------------------------
static bool InjectPhase(DWORD pid, const std::wstring& dllPath,
                         bool& settingsOpened, HostExitCode& exitCode) {
    int retries = 0;
    while (!g_exitHost) {
        if (IsDLLAlreadyInjected(pid, L"Agent.dll")) {
            LogHost("HOST", "Agent.dll already injected in PID " +
                    std::to_string(pid) + ". Reusing connection.");
            if (!g_stdoutOnly && !g_injectOnly && !g_silent)
                std::cout << "[~] Agent already injected. Reusing...\n";
            g_needReinjection = false;
            return true;
        }

        // Open settings once per discovery cycle (not on re-injection)
        if (!g_noSpawn && !settingsOpened) {
            ShellExecuteW(NULL, L"open", L"ms-settings:easeofaccess-audio",
                          NULL, NULL, SW_SHOWNORMAL);
            settingsOpened = true;
            Sleep(2000);
        }

        if (InjectDLL(pid, dllPath)) {
            LogHost("HOST", "Agent.dll injected successfully into PID " +
                    std::to_string(pid));
            if (!g_stdoutOnly && !g_injectOnly && !g_silent)
                std::cout << "[+] Injection successful!\n";
            g_needReinjection = false;
            return true;
        }

        LogHost("HOST", "Injection failed for PID " + std::to_string(pid) +
                " (attempt " + std::to_string(retries + 1) + "/" +
                std::to_string(MAX_INJECT_RETRIES) + ")");
        if (++retries >= MAX_INJECT_RETRIES) {
            LogHost("HOST", "Max injection retries reached.");
            if (!g_stdoutOnly && !g_silent)
                std::cerr << "[-] Injection failed after " << MAX_INJECT_RETRIES
                          << " retries.\n";
            exitCode  = HostExitCode::InjectionFailed;
            g_exitHost = true;
            return false;
        }
        Sleep(2000);
    }
    return false;
}

// ---------------------------------------------------------------
// Packet consumer loop — runs on main thread until
// g_exitHost or g_needReinjection is set.
// ---------------------------------------------------------------
static void ConsumerLoop() {
    while (!g_exitHost && !g_needReinjection) {
        std::string  pktStr;
        DWORD64      pktRecvTick = 0;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait_for(lock, std::chrono::milliseconds(200), [] {
                return !g_packetQueue.empty()
                    || g_needReinjection.load()
                    || g_exitHost.load();
            });
            if (g_needReinjection || g_exitHost) break;
            if (!g_packetQueue.empty()) {
                pktStr       = g_packetQueue.front().data;
                pktRecvTick  = g_packetQueue.front().recvTick;
                g_packetQueue.pop_front();
            } else {
                CheckSilenceTimeout(GetTickCount64());
                continue;
            }
        }

        if (pktStr.empty()) continue;

        PipePacket pkt;
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, pktStr.c_str(), -1, NULL, 0);
        if (wideSize <= 0) continue;

        std::wstring wpkt(wideSize, 0);
        MultiByteToWideChar(CP_UTF8, 0, pktStr.c_str(), -1, &wpkt[0], wideSize);

        if (ParsePacket(wpkt, pkt)) {
            DWORD64 delayMs = GetTickCount64() - pktRecvTick;
            ProcessTranslationAndSplitting(
                pkt.text, pkt.is_final, pkt.offset, pkt.duration,
                pkt.ts_ms, pktRecvTick, delayMs,
                pktStr.size(), pkt.result_id);
        }
    }
}

// ---------------------------------------------------------------
int main(int argc, char* argv[]) {
    // 1. Parse CLI args
    ParseCliArgs(argc, argv);

    // 2. Register Ctrl+C handler
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // 3. Initialize logs
    g_logPath = GetLogPath();
    {
        size_t lastSlash = g_logPath.find_last_of("\\");
        if (lastSlash != std::string::npos)
            CreateDirectoryA(g_logPath.substr(0, lastSlash).c_str(), NULL);
    }
    std::wstring wLogPath(g_logPath.begin(), g_logPath.end());
    RotateLogs(wLogPath);

    // Agent log path + AppContainer write permission
    std::wstring agentLogPath;
    {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
            std::wstring wPath(path);
            size_t pos = wPath.find_last_of(L"\\");
            if (pos != std::wstring::npos) {
                std::wstring dir = wPath.substr(0, pos);
                bool isBuiltDir  = dir.find(L"x64\\Release") != std::wstring::npos
                                || dir.find(L"x64\\Debug")   != std::wstring::npos;
                std::wstring root = isBuiltDir
                    ? wPath.substr(0, wPath.find_last_of(L"\\",
                          wPath.find_last_of(L"\\", pos - 1) - 1))
                    : dir;
                agentLogPath = root + L"\\logs\\mslc_agent_debug.log";
            }
        }
        if (agentLogPath.empty())
            agentLogPath = L"C:\\Users\\Public\\mslc_agent_debug.log";

        RotateLogs(agentLogPath);
        SafeHandle hFile(CreateFileW(agentLogPath.c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
        if (!hFile.IsValid() || !SetAppContainerWritePermission(agentLogPath)) {
            LogHost("WARN", "AppContainer write permission failed. Falling back to Public path.");
            agentLogPath = L"C:\\Users\\Public\\mslc_agent_debug.log";
            RotateLogs(agentLogPath);
            SafeHandle hFb(CreateFileW(agentLogPath.c_str(),
                GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
            if (hFb.IsValid()) SetAppContainerWritePermission(agentLogPath);
        }
    }

    LogHost("SESSION", "=== Host started ==="
            + std::string(g_watchMode   ? " [--watch]"       : "")
            + std::string(g_autoLaunch  ? " [--auto-launch]"  : "")
            + (g_onExitAction == OnExitAction::Relaunch ? " [--on-exit=relaunch]" :
               g_onExitAction == OnExitAction::Reinject ? " [--on-exit=reinject]" : ""));

    if (!g_stdoutOnly && !g_injectOnly)
        SetConsoleOutputCP(CP_UTF8);

    // 4. Duplicate check at startup (always performed)
    {
        DWORD firstPid = CheckAndResolveDuplicates();
        if (firstPid != 0 && g_targetPid == 0)
            g_targetPid = firstPid;
    }

    // 5. Start Named Pipe Server
    std::thread pipeServerThread(PipeListener);

    const std::wstring dllPath = ResolveDllPath();
    HostExitCode exitCode      = HostExitCode::Success;

    // 6. Main lifecycle loop
    while (!g_exitHost) {
        if (!g_stdoutOnly && !g_injectOnly && !g_silent)
            std::cout << "[*] Waiting for LiveCaptions.exe...\n";

        // --- Phase A: Discover / launch target ---
        bool settingsOpened = false;
        DWORD pid = 0;

        while (!g_exitHost && pid == 0) {
            pid = AcquireTargetPid(exitCode, settingsOpened);
            if (exitCode == HostExitCode::LaunchFailed) {
                g_exitHost = true;
                break;
            }
            if (pid == 0) Sleep(2000);
        }
        if (g_exitHost) break;

        g_targetPid = pid;
        if (!g_stdoutOnly && !g_injectOnly && !g_silent)
            std::cout << "[+] Found LiveCaptions.exe (PID: " << pid << ")\n";
        LogHost("HOST", "Target PID: " + std::to_string(pid));

        // --- Phase B: Inject ---
        if (!InjectPhase(pid, dllPath, settingsOpened, exitCode)) break;
        if (g_injectOnly) break;

        // --- Phase C: Watcher thread (--watch only) ---
        std::thread watcherThread;
        if (g_watchMode) {
            watcherThread = std::thread([pid, &exitCode]() {
                bool exited = WatchProcessUntilExit(pid);
                if (!exited || g_exitHost) return;

                LogHost("HOST", "LiveCaptions.exe (PID " + std::to_string(pid) +
                        ") exited. --on-exit=" +
                        (g_onExitAction == OnExitAction::Relaunch ? "relaunch" :
                         g_onExitAction == OnExitAction::Reinject ? "reinject" : "quit"));

                switch (g_onExitAction) {
                    case OnExitAction::Quit:
                        exitCode   = HostExitCode::ProcessExited;
                        g_exitHost = true;
                        break;
                    case OnExitAction::Reinject:
                    case OnExitAction::Relaunch:
                        g_targetPid       = 0;
                        g_needReinjection = true;
                        break;
                }
                g_queueCv.notify_all();
            });
        }

        // --- Phase D: Consumer loop ---
        ConsumerLoop();

        if (watcherThread.joinable()) watcherThread.join();

        // --- Phase E: Post-exit decision ---
        if (g_needReinjection && !g_exitHost) {
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
            if (g_onExitAction == OnExitAction::Relaunch && g_watchMode) {
                LogHost("HOST", "Relaunch: attempting to start new LiveCaptions instance.");
                DWORD newPid = 0;
                if (LaunchLiveCaptions(&newPid)) {
                    WakeupSuspendedProcess(newPid); // handle headless AppContainer spawn
                    g_targetPid = newPid;
                } else {
                    LogHost("HOST", "Relaunch failed. Falling back to re-discovery.");
                    g_targetPid = 0;
                }
            } else {
#endif
                g_targetPid = 0;
#if 0
            }
#endif
            g_needReinjection = false;
        }
    }

    // 7. Shutdown
    g_exitHost = true;
    g_queueCv.notify_all();
    HANDLE hPipe = g_hServerPipe.exchange(INVALID_HANDLE_VALUE);
    if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL) CloseHandle(hPipe);
    if (pipeServerThread.joinable()) pipeServerThread.join();

    LogHost("SESSION", "Host exited with code " + std::to_string(static_cast<int>(exitCode)));
    return static_cast<int>(exitCode);
}
