#include "pch.h"
#include <Windows.h>
#include <string>
#include <fstream>
#include <vector>
#include <Psapi.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include "MinHook.h"
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

// =============================================================
// NT INTERNAL DECLARATIONS (for LdrRegisterDllNotification)
// =============================================================
typedef LONG NTSTATUS;

#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING* PCUNICODE_STRING;

typedef struct _LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef VOID (NTAPI *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    PLDR_DLL_NOTIFICATION_DATA NotificationData,
    PVOID Context
);

typedef NTSTATUS (NTAPI *PLDR_REGISTER_DLL_NOTIFICATION)(
    ULONG Flags,
    PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context,
    PVOID *Cookie
);

typedef NTSTATUS (NTAPI *PLDR_UNREGISTER_DLL_NOTIFICATION)(
    PVOID Cookie
);

#define LDR_DLL_NOTIFICATION_REASON_LOADED 1

// =============================================================
// CONSTANTS
// =============================================================
static constexpr const wchar_t* PIPE_NAME  = L"\\\\.\\pipe\\LiveCaptionPipe";
static constexpr size_t QUEUE_MAX_SIZE      = 100;

static HMODULE g_hModule = NULL;
static std::string g_logPath = "";
static std::mutex g_logMutex;

// Helper to get workspace/logs path for logging
std::string GetLogPath() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, path, MAX_PATH)) {
        std::wstring wPath(path);
        size_t pos = wPath.find_last_of(L"\\");
        if (pos != std::wstring::npos) {
            std::wstring dir = wPath.substr(0, pos); // C:\Users\...\x64\Release
            pos = dir.find_last_of(L"\\");
            if (pos != std::wstring::npos) {
                std::wstring root = dir.substr(0, pos); // C:\Users\...\x64
                pos = root.find_last_of(L"\\");
                if (pos != std::wstring::npos) {
                    std::wstring projectRoot = root.substr(0, pos); // C:\Users\...\mslc-extractor
                    std::wstring logFile = projectRoot + L"\\logs\\mslc_agent_debug.txt";
                    return std::string(logFile.begin(), logFile.end());
                }
            }
        }
    }
    return "C:\\Users\\Public\\mslc_agent_debug.txt"; // Fallback
}

// =============================================================
// STRUCTURED LOGGER (Thread-Safe)
// =============================================================
void LogToFile(const char* level, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    std::ostringstream entry;
    entry << '['
          << std::setfill('0')
          << std::setw(4) << st.wYear  << '-'
          << std::setw(2) << st.wMonth << '-'
          << std::setw(2) << st.wDay   << 'T'
          << std::setw(2) << st.wHour  << ':'
          << std::setw(2) << st.wMinute << ':'
          << std::setw(2) << st.wSecond
          << "] ["
          << level
          << "] [Agent] "
          << msg;

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream logFile(g_logPath, std::ios_base::app);
    if (logFile.is_open()) {
        logFile << entry.str() << '\n';
    }
}

inline void LogInfo (const std::string& m) { LogToFile("INFO ", m); }
inline void LogWarn (const std::string& m) { LogToFile("WARN ", m); }
inline void LogError(const std::string& m) { LogToFile("ERROR", m); }
inline void LogFatal(const std::string& m) { LogToFile("FATAL", m); }

// =============================================================
// RAII WRAPPER FOR WINDOWS API HANDLES
// =============================================================
class SafeHandle {
    HANDLE m_handle;
public:
    explicit SafeHandle(HANDLE h = INVALID_HANDLE_VALUE) : m_handle(h) {}
    ~SafeHandle() { Close(); }

    void Close() {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != NULL) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE Get() const { return m_handle; }
    void Set(HANDLE h) { Close(); m_handle = h; }
    bool IsValid() const { return m_handle != INVALID_HANDLE_VALUE && m_handle != NULL; }

    SafeHandle(const SafeHandle&) = delete;
    SafeHandle& operator=(const SafeHandle&) = delete;
    SafeHandle(SafeHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID_HANDLE_VALUE; }
    SafeHandle& operator=(SafeHandle&& other) noexcept {
        if (this != &other) {
            Close();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};

// =============================================================
// BACKGROUND SENDER THREAD & RING BUFFER (BACKPRESSURE POLICY)
// =============================================================
static HANDLE                  g_hPipe      = INVALID_HANDLE_VALUE;
static std::deque<std::string> g_sendQueue;
static std::mutex              g_queueMutex;
static std::condition_variable g_queueCv;
static std::atomic<bool>       g_exitSender{false};
static SafeHandle              g_hSenderThread;

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

// Build JSON payload inline to avoid heap allocations on hot path
static std::string BuildJsonPayload(const char* text, bool is_final, DWORD64 ts_ms, uint64_t offset, uint64_t duration, const char* result_id) {
    const size_t text_bytes = strlen(text);

    std::string escaped;
    escaped.reserve(text_bytes);
    for (const char* p = text; *p; ++p) {
        if (*p == '"')  escaped += "\\\"";
        else if (*p == '\\') escaped += "\\\\";
        else            escaped += *p;
    }

    std::ostringstream json;
    json << "{\"text\":\""   << escaped
         << "\",\"is_final\":"  << (is_final ? "true" : "false")
         << ",\"bytes\":"    << text_bytes
         << ",\"ts_ms\":"    << ts_ms
         << ",\"offset\":"   << offset
         << ",\"duration\":" << duration
         << ",\"result_id\":\"" << result_id << "\""
         << "}\n";
    return json.str();
}

// =============================================================
// HELPER: FIND MODULE BY PARTIAL NAME (Dynamic Scan)
// =============================================================
HMODULE FindModuleByPartialName(const std::string& partialName) {
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded  = 0;

    EnumProcessModules(hProcess, nullptr, 0, &cbNeeded);
    if (cbNeeded == 0) return nullptr;

    std::vector<HMODULE> hMods(cbNeeded / sizeof(HMODULE));
    if (!EnumProcessModules(hProcess, hMods.data(), cbNeeded, &cbNeeded)) {
        return nullptr;
    }

    std::string partialLower = partialName;
    std::transform(partialLower.begin(), partialLower.end(), partialLower.begin(), ::tolower);

    for (HMODULE hMod : hMods) {
        TCHAR szModName[MAX_PATH] = {};
        if (!GetModuleFileNameEx(hProcess, hMod, szModName, MAX_PATH)) continue;

        std::wstring wName(szModName);
        std::string  sName;
        sName.reserve(wName.size());
        for (wchar_t wc : wName) {
            sName.push_back(static_cast<char>(wc));
        }
        std::transform(sName.begin(), sName.end(), sName.begin(), ::tolower);

        if (sName.find(partialLower) != std::string::npos) {
            return hMod;
        }
    }
    return nullptr;
}

// =============================================================
// SPEECH SDK TYPES & HOOKS
// =============================================================
typedef void*  SPXRESULTHANDLE;

enum Result_Reason : int {
    ResultReason_NoMatch           = 0,
    ResultReason_Canceled          = 1,
    ResultReason_RecognizingSpeech = 2,
    ResultReason_RecognizedSpeech  = 3,
    ResultReason_TranslatingSpeech = 6,
    ResultReason_TranslatedSpeech  = 7,
};

typedef int(__stdcall* result_get_text_t)     (SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen);
typedef int(__stdcall* result_get_reason_t)   (SPXRESULTHANDLE hresult, int*  pReason);
typedef int(__stdcall* result_get_offset_t)   (SPXRESULTHANDLE hresult, uint64_t* pOffset);
typedef int(__stdcall* result_get_duration_t) (SPXRESULTHANDLE hresult, uint64_t* pDuration);
typedef int(__stdcall* result_get_result_id_t)(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen);

result_get_text_t      fpOriginalResultGetText      = nullptr;
result_get_reason_t    fpOriginalResultGetReason    = nullptr;
result_get_offset_t    fpOriginalResultGetOffset    = nullptr;
result_get_duration_t  fpOriginalResultGetDuration  = nullptr;
result_get_result_id_t fpOriginalResultGetResultId  = nullptr;

int __stdcall Detour_result_get_text(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen) {
    const int ret = fpOriginalResultGetText(hresult, buffer, bufferLen);

    if (ret != 0 || buffer == nullptr || buffer[0] == '\0') return ret;

    int reason   = ResultReason_RecognizingSpeech;
    bool is_final = false;

    if (fpOriginalResultGetReason != nullptr) {
        if (fpOriginalResultGetReason(hresult, &reason) == 0) {
            is_final = (reason == ResultReason_RecognizedSpeech);
        }
    }

    uint64_t offset = 0;
    uint64_t duration = 0;
    char resultId[128] = { 0 };

    if (fpOriginalResultGetOffset != nullptr) {
        fpOriginalResultGetOffset(hresult, &offset);
    }
    if (fpOriginalResultGetDuration != nullptr) {
        fpOriginalResultGetDuration(hresult, &duration);
    }
    if (fpOriginalResultGetResultId != nullptr) {
        fpOriginalResultGetResultId(hresult, resultId, sizeof(resultId));
    }

    const DWORD64 ts_ms = GetTickCount64();

    LogInfo(std::string(is_final ? "FINAL: " : "PARTIAL: ") + buffer + 
            " (Id: " + resultId + ", Offset: " + std::to_string(offset) + 
            ", Duration: " + std::to_string(duration) + ")");

    const std::string payload = BuildJsonPayload(buffer, is_final, ts_ms, offset, duration, resultId);
    PushToQueue(payload); // Push to background queue, zero latency on target thread

    return ret;
}

// =============================================================
// HOOK INSTALLATION THREAD (Safe environment outside Windows Loader Lock)
// =============================================================
DWORD WINAPI HookThread(LPVOID lpParam) {
    // Yield execution to allow DLL loading thread to complete DllMain and exit Windows Loader Lock
    Sleep(100);
    HMODULE hCoreDLL = reinterpret_cast<HMODULE>(lpParam);
    
    if (hCoreDLL == nullptr) {
        // Obfuscate module name
        char p1[] = { 'm','i','c','r','o','s','o','f','t','.',0 };
        char p2[] = { 'c','o','g','n','i','t','i','v','e','s','e','r','v','i','c','e','s','.',0 };
        char p3[] = { 's','p','e','e','c','h','.',0 };
        char p4[] = { 'c','o','r','e','.',0 };
        char p5[] = { 'd','l','l',0 };
        std::string targetDll = std::string(p1) + p2 + p3 + p4 + p5;

        hCoreDLL = FindModuleByPartialName(targetDll);
    }

    if (hCoreDLL == nullptr) {
        LogError("HookThread: Core DLL not found.");
        return 0;
    }

    LogInfo("HookThread: Core DLL found. Resolving exports...");
    FARPROC pGetText   = GetProcAddress(hCoreDLL, "result_get_text");
    FARPROC pGetReason = GetProcAddress(hCoreDLL, "result_get_reason");
    FARPROC pGetOffset  = GetProcAddress(hCoreDLL, "result_get_offset");
    FARPROC pGetDuration = GetProcAddress(hCoreDLL, "result_get_duration");
    FARPROC pGetResultId = GetProcAddress(hCoreDLL, "result_get_result_id");

    if (!pGetText) {
        LogError("HookThread: 'result_get_text' export not found.");
        return 0;
    }

    if (MH_Initialize() != MH_OK) {
        LogError("HookThread: MH_Initialize failed.");
        return 0;
    }

    if (MH_CreateHook(reinterpret_cast<LPVOID>(pGetText),
                      &Detour_result_get_text,
                      reinterpret_cast<LPVOID*>(&fpOriginalResultGetText)) != MH_OK) {
        LogError("HookThread: MH_CreateHook for result_get_text failed.");
        return 0;
    }

    if (pGetReason) {
        fpOriginalResultGetReason = reinterpret_cast<result_get_reason_t>(pGetReason);
        LogInfo("HookThread: 'result_get_reason' resolved.");
    }
    if (pGetOffset) {
        fpOriginalResultGetOffset = reinterpret_cast<result_get_offset_t>(pGetOffset);
        LogInfo("HookThread: 'result_get_offset' resolved.");
    }
    if (pGetDuration) {
        fpOriginalResultGetDuration = reinterpret_cast<result_get_duration_t>(pGetDuration);
        LogInfo("HookThread: 'result_get_duration' resolved.");
    }
    if (pGetResultId) {
        fpOriginalResultGetResultId = reinterpret_cast<result_get_result_id_t>(pGetResultId);
        LogInfo("HookThread: 'result_get_result_id' resolved.");
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogError("HookThread: MH_EnableHook failed.");
        return 0;
    }

    LogInfo("HookThread: Hooks enabled successfully.");
    return 0;
}

// =============================================================
// ZERO-INVASIVE DLL NOTIFICATION CALLBACK
// =============================================================
static PVOID g_ldrCookie = nullptr;
static std::atomic<bool> g_hookInstalled{false};

VOID NTAPI DllNotificationCallback(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID /*Context*/) {
    if (NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
        if (NotificationData && NotificationData->BaseDllName && NotificationData->BaseDllName->Buffer) {
            std::wstring dllName(NotificationData->BaseDllName->Buffer, NotificationData->BaseDllName->Length / sizeof(wchar_t));
            std::transform(dllName.begin(), dllName.end(), dllName.begin(), ::tolower);
            
            if (dllName.find(L"microsoft.cognitiveservices.speech.core.dll") != std::wstring::npos) {
                if (!g_hookInstalled.exchange(true)) {
                    LogInfo("DllNotification: Target DLL loaded. Launching HookThread.");
                    // Start hook thread outside of Windows Loader Lock
                    CreateThread(NULL, 0, HookThread, NotificationData->DllBase, 0, NULL);
                }
            }
        }
    }
}

// =============================================================
// DLL ENTRY POINT
// =============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        g_logPath = GetLogPath();

        DisableThreadLibraryCalls(hModule);

        // Truncate log file at session start
        {
            std::ofstream logFile(g_logPath, std::ios_base::trunc);
            logFile << "[Agent] === New Session Started ===\n";
        }

        LogInfo("DllMain: DLL_PROCESS_ATTACH.");

        // Start background sender thread
        g_hSenderThread.Set(CreateThread(NULL, 0, SenderThread, NULL, 0, NULL));

        // Register DLL notification to hook dynamically without active scanning
        HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
        if (hNtDll) {
            auto pLdrRegisterDllNotification = reinterpret_cast<PLDR_REGISTER_DLL_NOTIFICATION>(
                GetProcAddress(hNtDll, "LdrRegisterDllNotification")
            );
            if (pLdrRegisterDllNotification) {
                pLdrRegisterDllNotification(0, DllNotificationCallback, NULL, &g_ldrCookie);
                LogInfo("DllMain: Registered DLL notification callback.");
            }
        }

        // Edge case: DLL might be already loaded before we registered the callback
        char p1[] = { 'm','i','c','r','o','s','o','f','t','.',0 };
        char p2[] = { 'c','o','g','n','i','t','i','v','e','s','e','r','v','i','c','e','s','.',0 };
        char p3[] = { 's','p','e','e','c','h','.',0 };
        char p4[] = { 'c','o','r','e','.',0 };
        char p5[] = { 'd','l','l',0 };
        std::string targetDll = std::string(p1) + p2 + p3 + p4 + p5;

        HMODULE hCoreDLL = FindModuleByPartialName(targetDll);
        if (hCoreDLL != nullptr) {
            if (!g_hookInstalled.exchange(true)) {
                LogInfo("DllMain: Core DLL already loaded. Launching HookThread.");
                CreateThread(NULL, 0, HookThread, hCoreDLL, 0, NULL);
            }
        }
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        LogInfo("DllMain: DLL_PROCESS_DETACH. Unloading...");

        // Unregister DLL notification callback
        HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
        if (hNtDll && g_ldrCookie) {
            auto pLdrUnregisterDllNotification = reinterpret_cast<PLDR_UNREGISTER_DLL_NOTIFICATION>(
                GetProcAddress(hNtDll, "LdrUnregisterDllNotification")
            );
            if (pLdrUnregisterDllNotification) {
                pLdrUnregisterDllNotification(g_ldrCookie);
            }
        }

        // Signal sender thread to exit and wait
        g_exitSender = true;
        g_queueCv.notify_all();
        if (g_hSenderThread.IsValid()) {
            WaitForSingleObject(g_hSenderThread.Get(), 1000);
            g_hSenderThread.Close();
        }

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        LogInfo("DllMain: Hook uninstalled, agent detached.");
    }
    return TRUE;
}
