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

static constexpr const wchar_t* TARGET_APP = L"LiveCaptions.exe";

int main(int argc, char* argv[]) {
    // 1. Parse CLI args
    ParseCliArgs(argc, argv);

    // 2. Initialize log path & folder
    g_logPath = GetLogPath();
    size_t lastSlash = g_logPath.find_last_of("\\");
    if (lastSlash != std::string::npos) {
        std::string logDir = g_logPath.substr(0, lastSlash);
        CreateDirectoryA(logDir.c_str(), NULL);
    }
    std::wstring wLogPath(g_logPath.begin(), g_logPath.end());
    RotateLogs(wLogPath);

    // Prepare Agent log file and grant write access to AppContainers
    std::wstring agentLogPath;
    {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
            std::wstring wPath(path);
            size_t pos = wPath.find_last_of(L"\\");
            if (pos != std::wstring::npos) {
                std::wstring dir = wPath.substr(0, pos);
                if (dir.find(L"x64\\Release") != std::wstring::npos || dir.find(L"x64\\Debug") != std::wstring::npos) {
                    pos = dir.find_last_of(L"\\");
                    if (pos != std::wstring::npos) {
                        std::wstring root = dir.substr(0, pos);
                        pos = root.find_last_of(L"\\");
                        if (pos != std::wstring::npos) {
                            std::wstring projectRoot = root.substr(0, pos);
                            agentLogPath = projectRoot + L"\\logs\\mslc_agent_debug.log";
                        }
                    }
                } else {
                    agentLogPath = dir + L"\\logs\\mslc_agent_debug.log";
                }
            }
        }
    }
    if (!agentLogPath.empty()) {
        RotateLogs(agentLogPath);
        HANDLE hFile = CreateFileW(agentLogPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            if (!SetAppContainerWritePermission(agentLogPath)) {
                LogHost("WARN", "SetAppContainerWritePermission failed for project Agent log path. Falling back to public path.");
                agentLogPath = L"C:\\Users\\Public\\mslc_agent_debug.log";
                RotateLogs(agentLogPath);
                HANDLE hFallbackFile = CreateFileW(agentLogPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFallbackFile != INVALID_HANDLE_VALUE) {
                    CloseHandle(hFallbackFile);
                    SetAppContainerWritePermission(agentLogPath);
                }
            }
        } else {
            agentLogPath = L"C:\\Users\\Public\\mslc_agent_debug.log";
            RotateLogs(agentLogPath);
            HANDLE hFallbackFile = CreateFileW(agentLogPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFallbackFile != INVALID_HANDLE_VALUE) {
                CloseHandle(hFallbackFile);
                SetAppContainerWritePermission(agentLogPath);
            }
        }
    }

    LogHost("SESSION", "=== Host started ===");

    if (!g_stdoutOnly && !g_injectOnly) {
        SetConsoleOutputCP(CP_UTF8);
    }

    // 3. Start Pipe Server
    std::thread pipeServerThread(PipeListener);

    // 4. Main Connection & Lifecycle Loop
    while (!g_exitHost) {
        if (!g_stdoutOnly && !g_injectOnly) {
            std::cout << "[*] Waiting for LiveCaptions.exe..." << std::endl;
        }

        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring strPath(exePath);
        std::wstring dllPath = strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\Agent.dll";

        DWORD pid = g_targetPid;
        bool settingsOpened = false;
        int injectRetries = 0;

        while (!g_exitHost) {
            if (pid == 0) {
                pid = GetProcessIdByName(TARGET_APP);
            }

            if (pid != 0) {
                g_targetPid = pid;
                if (!g_stdoutOnly && !g_injectOnly) {
                    std::cout << "[+] Found LiveCaptions.exe (PID: " << pid << ")" << std::endl;
                }
                LogHost("HOST", "Found LiveCaptions.exe (PID: " + std::to_string(pid) + ")");

                if (IsDLLAlreadyInjected(pid, L"Agent.dll")) {
                    LogHost("HOST", "Agent.dll is already injected. Reusing existing connection.");
                    if (!g_stdoutOnly && !g_injectOnly) {
                        std::cout << "[~] Agent already injected. Reusing connection..." << std::endl;
                    }
                    g_needReinjection = false;
                    break;
                }

                if (!g_noSpawn && !settingsOpened) {
                    std::wstring msSettingsUri = L"ms-settings:easeofaccess-audio";
                    ShellExecuteW(NULL, L"open", msSettingsUri.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    LogHost("HOST", "Opened Windows Settings to trigger DWM/LiveCaptions reload.");
                    settingsOpened = true;
                    Sleep(2000);
                }

                bool success = InjectDLL(pid, dllPath);
                if (success) {
                    LogHost("HOST", "Agent.dll injected successfully into PID " + std::to_string(pid));
                    if (!g_stdoutOnly && !g_injectOnly) {
                        std::cout << "[+] Injection successful!" << std::endl;
                    }
                    g_needReinjection = false;
                    break;
                } else {
                    LogHost("HOST", "Failed to inject Agent.dll into PID " + std::to_string(pid));
                    injectRetries++;
                    if (injectRetries >= 3) {
                        LogHost("HOST", "Max injection retries reached. Exiting.");
                        if (!g_stdoutOnly) {
                            std::cerr << "[-] Injection failed after 3 retries. Exiting..." << std::endl;
                        }
                        g_exitHost = true;
                        break;
                    }
                    Sleep(2000);
                }
            } else {
                Sleep(2000);
            }
        }

        if (g_injectOnly || g_exitHost) {
            break;
        }

        // Processing Loop
        while (!g_exitHost && !g_needReinjection) {
            std::string pktStr;
            DWORD64 pktRecvTick = 0;
            {
                std::unique_lock<std::mutex> lock(g_queueMutex);
                if (g_queueCv.wait_for(lock, std::chrono::milliseconds(200), []{ return !g_packetQueue.empty() || g_needReinjection.load() || g_exitHost.load(); })) {
                    if (g_needReinjection || g_exitHost) break;
                    if (!g_packetQueue.empty()) {
                        pktStr = g_packetQueue.front().data;
                        pktRecvTick = g_packetQueue.front().recvTick;
                        g_packetQueue.pop_front();
                    }
                } else {
                    CheckSilenceTimeout(GetTickCount64());
                    continue;
                }
            }

            if (!pktStr.empty()) {
                PipePacket pkt;
                int wideSize = MultiByteToWideChar(CP_UTF8, 0, pktStr.c_str(), -1, NULL, 0);
                if (wideSize > 0) {
                    std::wstring wpkt(wideSize, 0);
                    MultiByteToWideChar(CP_UTF8, 0, pktStr.c_str(), -1, &wpkt[0], wideSize);
                    
                    if (ParsePacket(wpkt, pkt)) {
                        DWORD64 processTick = GetTickCount64();
                        DWORD64 delayMs = (processTick >= pktRecvTick) ? (processTick - pktRecvTick) : 0;
                        
                        ProcessTranslationAndSplitting(
                            pkt.text, pkt.is_final, pkt.offset, pkt.duration, pkt.ts_ms,
                            pktRecvTick, delayMs, pktStr.size(), pkt.result_id
                        );
                    }
                }
            }
        }
        
        if (g_needReinjection) {
            g_targetPid = 0;
            g_needReinjection = false;
        }
    }

    g_exitHost = true;
    g_queueCv.notify_all();
    HANDLE hPipe = g_hServerPipe.exchange(INVALID_HANDLE_VALUE);
    if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL) CloseHandle(hPipe);
    if (pipeServerThread.joinable()) pipeServerThread.join();
    return 0;
}
