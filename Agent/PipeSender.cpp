#include "pch.h"
#include "PipeSender.h"
#include "AgentLogger.h"

HANDLE g_hPipe = INVALID_HANDLE_VALUE;
std::deque<std::string> g_sendQueue;
std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::atomic<bool> g_exitSender{false};
SafeHandle g_hSenderThread;

static constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\LiveCaptionPipe";
static constexpr size_t QUEUE_MAX_SIZE = 100;

void PushToQueue(const std::string& payload) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    
    // Backpressure Handling: if queue is full, drop oldest packets to prevent memory bloat
    if (g_sendQueue.size() >= QUEUE_MAX_SIZE) {
        g_sendQueue.pop_front();
    }
    g_sendQueue.push_back(payload);
    g_queueCv.notify_one();
}

DWORD WINAPI SenderThread(LPVOID /*lpParam*/) {
    LogInfo("SenderThread: Started.");
    int retryCount = 0;

    while (!g_exitSender) {
        std::string payload;

        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return !g_sendQueue.empty() || g_exitSender; });

            if (g_exitSender && g_sendQueue.empty()) {
                break;
            }

            payload = g_sendQueue.front();
            g_sendQueue.pop_front();
        }

        bool sent = false;
        while (!sent && !g_exitSender) {
            // Lazy connect
            if (g_hPipe == INVALID_HANDLE_VALUE) {
                g_hPipe = CreateFileW(
                    PIPE_NAME,
                    GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    0, // Synchronous writing is safe on this dedicated thread
                    NULL
                );

                if (g_hPipe != INVALID_HANDLE_VALUE) {
                    LogInfo("SenderThread: Named Pipe connected successfully.");
                    retryCount = 0; // Reset backoff
                } else {
                    DWORD err = GetLastError();
                    // Smart Backoff (Exponential Backoff with Jitter)
                    retryCount = (std::min)(retryCount + 1, 2); // Max delay ~4s
                    int backoffMs = (1 << retryCount) * 1000;
                    
                    // Simple Jitter (0-500ms) using system tick to avoid Sonar cpp:S2245 (Weak Cryptography)
                    int jitter = static_cast<int>(GetTickCount64() % 500);
                    backoffMs += jitter;

                    LogWarn("SenderThread: Connection failed (err=" + std::to_string(err) + 
                            "). Backing off for " + std::to_string(backoffMs) + "ms");

                    int sleepRemain = backoffMs;
                    while (sleepRemain > 0 && !g_exitSender) {
                        int chunk = (std::min)(sleepRemain, 200);
                        Sleep(chunk);
                        sleepRemain -= chunk;
                    }
                    continue; // Retry connection
                }
            }

            DWORD written = 0;
            BOOL ok = WriteFile(
                g_hPipe,
                payload.c_str(),
                static_cast<DWORD>(payload.size()),
                &written,
                NULL
            );

            if (ok) {
                sent = true;
            } else {
                DWORD err = GetLastError();
                LogWarn("SenderThread: Write failed (err=" + std::to_string(err) + "). Resetting pipe handle.");
                CloseHandle(g_hPipe);
                g_hPipe = INVALID_HANDLE_VALUE;
            }
        }
    }

    if (g_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hPipe);
        g_hPipe = INVALID_HANDLE_VALUE;
    }

    LogInfo("SenderThread: Exiting.");
    return 0;
}
