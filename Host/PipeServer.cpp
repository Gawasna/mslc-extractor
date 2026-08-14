#include "PipeServer.h"
#include "HostLogger.h"
#include "AppConfig.h"
#include "TextProcessor.h"
#include "ConsoleRenderer.h"
#include <sddl.h>
#include <vector>
#include <iostream>

std::deque<RawPacket>   g_packetQueue;
std::mutex              g_queueMutex;
std::condition_variable g_queueCv;
std::atomic<bool>       g_exitHost{false};
std::atomic<HANDLE>     g_hServerPipe{INVALID_HANDLE_VALUE};

bool IsProcessAlive(DWORD pid) {
    if (pid == 0) return false;
    SafeHandle shProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!shProcess.IsValid()) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(shProcess.Get(), &exitCode) && (exitCode == STILL_ACTIVE);
}

void PipeListener() {
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;S-1-15-2-1)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)S:(ML;;NW;;;LW)", 
            SDDL_REVISION_1, 
            &pSD, 
            NULL)) {
        LogHost("PIPE", "Fatal: ConvertStringSecurityDescriptorToSecurityDescriptor failed.");
        return;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };

    while (!g_exitHost) {
        if (g_needReinjection) {
            Sleep(500);
            continue;
        }

        HANDLE hPipe = CreateNamedPipeW(
            g_customPipeName.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            LogHost("PIPE", "CreateNamedPipeW failed. Retrying in 1s.");
            Sleep(1000);
            continue;
        }

        g_hServerPipe.store(hPipe);
        SafeHandle shPipe(hPipe);

        SafeHandle hConnectEvent(CreateEventW(NULL, TRUE, FALSE, NULL));
        OVERLAPPED ovConnect = { 0 };
        ovConnect.hEvent = hConnectEvent.Get();

        BOOL connected = ConnectNamedPipe(shPipe.Get(), &ovConnect);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                while (!g_exitHost && !g_needReinjection) {
                    DWORD waitRes = WaitForSingleObject(hConnectEvent.Get(), 500);
                    if (waitRes == WAIT_OBJECT_0) {
                        connected = TRUE;
                        break;
                    }
                }
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }

        if (!connected || g_exitHost || g_needReinjection) {
            g_hServerPipe.store(INVALID_HANDLE_VALUE);
            continue;
        }

        LogHost("PIPE", "Agent connected. Overlapped read session started.");

        static constexpr DWORD PIPE_BUF_BYTES = 65536;
        std::vector<char> rawBuf(PIPE_BUF_BYTES);
        SafeHandle hReadEvent(CreateEventW(NULL, TRUE, FALSE, NULL));
        std::string accumulatedBuffer;

        while (!g_exitHost && !g_needReinjection) {
            OVERLAPPED ovRead = { 0 };
            ovRead.hEvent = hReadEvent.Get();
            DWORD bytesRead = 0;

            BOOL readOk = ReadFile(
                shPipe.Get(),
                rawBuf.data(),
                static_cast<DWORD>(rawBuf.size() - 1),
                &bytesRead,
                &ovRead
            );

            if (!readOk) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    DWORD waitRes = WaitForSingleObject(hReadEvent.Get(), 10000);
                    if (waitRes == WAIT_OBJECT_0) {
                        if (GetOverlappedResult(shPipe.Get(), &ovRead, &bytesRead, FALSE)) {
                            readOk = TRUE;
                        } else {
                            readOk = FALSE;
                        }
                    } else if (waitRes == WAIT_TIMEOUT) {
                        LogHost("PIPE", "Read timeout (zombie connection). Force reconnecting.");
                        break;
                    } else {
                        readOk = FALSE;
                    }
                } else {
                    readOk = FALSE;
                }
            }

            if (!readOk || bytesRead == 0) {
                LogHost("PIPE", "Pipe closed or agent disconnected.");
                break;
            }

            accumulatedBuffer.append(rawBuf.data(), bytesRead);
            size_t pos;
            const DWORD64 recvTick = GetTickCount64();
            while ((pos = accumulatedBuffer.find('\n')) != std::string::npos) {
                std::string packetData = accumulatedBuffer.substr(0, pos);
                accumulatedBuffer.erase(0, pos + 1);

                if (!packetData.empty()) {
                    if (packetData.back() == '\r') {
                        packetData.pop_back();
                    }
                    if (!packetData.empty()) {
                        std::lock_guard<std::mutex> lock(g_queueMutex);
                        g_packetQueue.push_back({ packetData, recvTick });
                        g_queueCv.notify_one();
                    }
                }
            }
        }

        g_hServerPipe.store(INVALID_HANDLE_VALUE);
        DisconnectNamedPipe(shPipe.Get());
        
        {
            std::lock_guard<std::mutex> lock(g_csMutex);
            g_splitter.Reset();
        }

        if (g_targetPid != 0 && !IsProcessAlive(g_targetPid)) {
            LogHost("PIPE", "LiveCaptions.exe (PID: " + std::to_string(g_targetPid) + ") has terminated or crashed.");
            if (!g_stdoutOnly) {
                ClearLiveText();
                std::wcout << L"[-] LiveCaptions.exe (PID: " << g_targetPid << L") terminated or crashed. Re-discovery initiated..." << std::endl;
            }
            g_needReinjection = true;
            g_queueCv.notify_all();
        }
    }

    if (pSD) {
        LocalFree(pSD);
    }
}
