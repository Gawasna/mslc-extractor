#include "pch.h"
#include <Windows.h>
#include <string>
#include <fstream>
#include <vector>
#include <Psapi.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "MinHook.h"

#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

// =============================================================
// CONSTANTS
// =============================================================
static constexpr const wchar_t* PIPE_NAME  = L"\\\\.\\pipe\\LiveCaptionPipe";
static constexpr int MODULE_SCAN_RETRIES   = 20;
static constexpr int MODULE_SCAN_INTERVAL  = 1000;  // ms

// Helper to get AppData path for logging
std::string GetLogPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::string fullPath = path;
        fullPath += "\\mslc_hook_debug.txt";
        return fullPath;
    }
    return "C:\\Users\\Public\\mslc_hook_debug.txt"; // Fallback
}

static const std::string LOG_PATH = GetLogPath();

// =============================================================
// PERSISTENT PIPE CLIENT STATE
// One HANDLE kept alive across all SendToPipe calls.
// Reconnects lazily on first use or after a write failure.
// g_pipeCs guards g_hPipe against concurrent hook calls.
// =============================================================
static HANDLE         g_hPipe  = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_pipeCs;


// =============================================================
// STRUCTURED LOGGER
// Level: INFO | WARN | ERROR | FATAL
// Format: [2026-02-28T00:10:58] [INFO] [HookCore] msg
// =============================================================
void LogToFile(const char* level, const std::string& msg) {
    // Get local time for ISO 8601 timestamp
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
          << "] [HookCore] "
          << msg;

    std::ofstream logFile(LOG_PATH, std::ios_base::app);
    if (logFile.is_open()) {
        logFile << entry.str() << '\n';
    }
    // Intentional: do not re-throw if file open fails - hook must never crash target process
}

// Convenience wrappers matching log-level names
inline void LogInfo (const std::string& m) { LogToFile("INFO ", m); }
inline void LogWarn (const std::string& m) { LogToFile("WARN ", m); }
inline void LogError(const std::string& m) { LogToFile("ERROR", m); }
inline void LogFatal(const std::string& m) { LogToFile("FATAL", m); }

// =============================================================
// PIPE WRITER - Persistent connection, lazy reconnect
//
// Previous design: CreateFile -> WriteFile -> CloseHandle per packet.
// Problem: each CloseHandle disconnects the server pipe, triggering
//          Loader's DisconnectNamedPipe + splitter Reset() -> watermark
//          was zeroed after every packet, making the splitter useless.
//
// New design: keep g_hPipe open. On WriteFile failure (broken pipe),
//             close the stale handle and let the next call reconnect.
// =============================================================
void SendToPipe(const std::string& jsonPayload) {
    EnterCriticalSection(&g_pipeCs);

    // Lazy connect: attempt only if handle is not yet open
    if (g_hPipe == INVALID_HANDLE_VALUE) {
        g_hPipe = CreateFileW(
            PIPE_NAME,
            GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,      // Synchronous I/O
            NULL
        );
        if (g_hPipe != INVALID_HANDLE_VALUE) {
            LogInfo("Pipe connected to Loader.");
        }
        // If still INVALID: Loader not ready - drop packet silently this call
    }

    if (g_hPipe != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const BOOL ok = WriteFile(
            g_hPipe,
            jsonPayload.c_str(),
            static_cast<DWORD>(jsonPayload.size()),
            &written,
            NULL
        );
        if (!ok) {
            // Broken pipe (Loader restarted or crashed): close stale handle.
            // Next call will reconnect.
            LogWarn("Pipe write failed (err=" + std::to_string(GetLastError()) +
                    "). Closing stale handle, will reconnect on next packet.");
            CloseHandle(g_hPipe);
            g_hPipe = INVALID_HANDLE_VALUE;
        }
    }

    LeaveCriticalSection(&g_pipeCs);
}

// Build JSON payload inline to avoid heap allocations on hot path
// Format: {"text":"...","is_final":true,"bytes":N,"ts_ms":T}
static std::string BuildJsonPayload(const char* text, bool is_final, DWORD64 ts_ms) {
    const size_t text_bytes = strlen(text);

    // Escape any double-quotes in captured text to keep JSON valid
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
         << '}';
    return json.str();
}

// =============================================================
// HELPER: FIND MODULE BY PARTIAL NAME (dynamic scan)
// Uses vector to avoid C6262 (large stack allocation)
// =============================================================
HMODULE FindModuleByPartialName(const std::string& partialName) {
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded  = 0;

    // First call: query required buffer size
    EnumProcessModules(hProcess, nullptr, 0, &cbNeeded);
    if (cbNeeded == 0) return nullptr;

    // Allocate exactly the right number of slots on heap (fix C6262)
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
        std::string  sName(wName.begin(), wName.end());
        std::transform(sName.begin(), sName.end(), sName.begin(), ::tolower);

        if (sName.find(partialLower) != std::string::npos) {
            return hMod;
        }
    }
    return nullptr;
}

// =============================================================
// SPEECH SDK TYPES & HOOKS
// Ref: Azure Cognitive Services Speech SDK C API
// =============================================================
typedef void*  SPXRESULTHANDLE;

// Result reason enum (subset - matches SDK internal values)
enum Result_Reason : int {
    ResultReason_NoMatch           = 0,
    ResultReason_Canceled          = 1,
    ResultReason_RecognizingSpeech = 2,  // Partial / intermediate result
    ResultReason_RecognizedSpeech  = 3,  // Final committed result
    ResultReason_TranslatingSpeech = 6,
    ResultReason_TranslatedSpeech  = 7,
};

typedef int(__stdcall* result_get_text_t)  (SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen);
typedef int(__stdcall* result_get_reason_t)(SPXRESULTHANDLE hresult, int*  pReason);

result_get_text_t   fpOriginalResultGetText   = nullptr;
result_get_reason_t fpOriginalResultGetReason = nullptr;

// Detour: intercept text + query reason from the same handle
int __stdcall Detour_result_get_text(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen) {
    const int ret = fpOriginalResultGetText(hresult, buffer, bufferLen);

    if (ret != 0 || buffer == nullptr || buffer[0] == '\0') return ret;

    // Query recognition state via SDK API (do NOT guess from punctuation)
    int reason   = ResultReason_RecognizingSpeech;
    bool is_final = false;

    if (fpOriginalResultGetReason != nullptr) {
        if (fpOriginalResultGetReason(hresult, &reason) == 0) {
            is_final = (reason == ResultReason_RecognizedSpeech);
        }
    }

    const DWORD64 ts_ms = GetTickCount64();

    LogInfo(std::string(is_final ? "FINAL: " : "PARTIAL: ") + buffer);

    const std::string payload = BuildJsonPayload(buffer, is_final, ts_ms);
    SendToPipe(payload);

    return ret;
}

// =============================================================
// HOOK INSTALLATION THREAD
// =============================================================
DWORD WINAPI HookThread(LPVOID /*lpParam*/) {
    LogInfo("Thread started. Scanning for Core DLL handle...");

    // Phase 1 Mitigation: Obfuscate target DLL name
    // "microsoft.cognitiveservices.speech.core.dll"
    char p1[] = { 'm','i','c','r','o','s','o','f','t','.',0 };
    char p2[] = { 'c','o','g','n','i','t','i','v','e','s','e','r','v','i','c','e','s','.',0 };
    char p3[] = { 's','p','e','e','c','h','.',0 };
    char p4[] = { 'c','o','r','e','.',0 };
    char p5[] = { 'd','l','l',0 };
    std::string targetDll = std::string(p1) + p2 + p3 + p4 + p5;

    // --- Phase 1: Find core DLL ---
    HMODULE hCoreDLL = nullptr;
    for (int retry = 0; retry < MODULE_SCAN_RETRIES && hCoreDLL == nullptr; ++retry) {
        hCoreDLL = FindModuleByPartialName(targetDll);
        if (hCoreDLL == nullptr) {
            LogInfo("Scanning for DLL... Retry " + std::to_string(retry + 1)
                    + "/" + std::to_string(MODULE_SCAN_RETRIES));
            Sleep(MODULE_SCAN_INTERVAL);
        }
    }

    if (hCoreDLL == nullptr) {
        LogFatal("Could not find Core DLL handle after all retries.");
        return 0;
    }
    LogInfo("Core DLL handle found: 0x" + [&] {
        std::ostringstream ss;
        ss << std::hex << reinterpret_cast<uintptr_t>(hCoreDLL);
        return ss.str();
    }());

    // --- Phase 2: Resolve function addresses ---
    FARPROC pGetText   = GetProcAddress(hCoreDLL, "result_get_text");
    FARPROC pGetReason = GetProcAddress(hCoreDLL, "result_get_reason");

    if (!pGetText) {
        LogError("'result_get_text' not found. Module exports may have changed.");
        return 0;
    }
    LogInfo("'result_get_text' at: 0x" + [&] {
        std::ostringstream ss; ss << std::hex << reinterpret_cast<uintptr_t>(pGetText);
        return ss.str();
    }());

    if (!pGetReason) {
        // Non-fatal: is_final detection will default to RecognizingSpeech
        LogWarn("'result_get_reason' not found. is_final detection disabled.");
    } else {
        LogInfo("'result_get_reason' found. is_final detection enabled.");
    }

    // --- Phase 3: Install hooks ---
    if (MH_Initialize() != MH_OK) {
        LogError("MH_Initialize failed.");
        return 0;
    }

    if (MH_CreateHook(reinterpret_cast<LPVOID>(pGetText),
                      &Detour_result_get_text,
                      reinterpret_cast<LPVOID*>(&fpOriginalResultGetText)) != MH_OK) {
        LogError("MH_CreateHook for result_get_text failed.");
        return 0;
    }

    if (pGetReason) {
        // We do NOT intercept result_get_reason; we only need to call it from within
        // Detour_result_get_text on the same handle. Direct cast is correct here.
        fpOriginalResultGetReason = reinterpret_cast<result_get_reason_t>(pGetReason);
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogError("MH_EnableHook failed.");
        return 0;
    }

    LogInfo("HOOK ENABLED. Waiting for captions...");
    return 0;
}

// =============================================================
// DLL ENTRY POINT
// =============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        InitializeCriticalSection(&g_pipeCs);

        // Truncate log file at the start of each new session
        {
            std::ofstream logFile(LOG_PATH, std::ios_base::trunc);
            logFile << "[HookCore] === New Session Started ===\n";
        }

        LogInfo("DLL_PROCESS_ATTACH. Launching hook thread.");
        CreateThread(NULL, 0, HookThread, NULL, 0, NULL);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // Release persistent pipe handle on unload
        EnterCriticalSection(&g_pipeCs);
        if (g_hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(g_hPipe);
            g_hPipe = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&g_pipeCs);
        DeleteCriticalSection(&g_pipeCs);
    }
    return TRUE;
}
