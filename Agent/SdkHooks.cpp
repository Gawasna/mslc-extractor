#include "pch.h"
#include "SdkHooks.h"
#include "AgentLogger.h"
#include "JsonBuilder.h"
#include "PipeSender.h"
#include "ModuleUtils.h"
#include "HeapScanner.h"
#include "MinHook.h"

// HeapScannerThread is defined later in this TU — forward-declare so HookThread can reference it
static DWORD WINAPI HeapScannerThread(LPVOID lpParam);

result_get_text_t      fpOriginalResultGetText      = nullptr;
result_get_reason_t    fpOriginalResultGetReason    = nullptr;
result_get_offset_t    fpOriginalResultGetOffset    = nullptr;
result_get_duration_t  fpOriginalResultGetDuration  = nullptr;
result_get_result_id_t fpOriginalResultGetResultId  = nullptr;
recognizer_session_started_set_callback_t fpOriginalRecognizerSessionStartedSetCallback = nullptr;

std::mutex g_sessionStartedMutex;
std::unordered_map<SPXRECOHANDLE, PSESSION_CALLBACK_FUNC> g_sessionStartedCallbacks;
std::atomic<bool> g_sessionStartedEmitted{ false };

inline void EnsureSessionStartedEmitted(SPXRECOHANDLE hreco, SPXEVENTHANDLE hevent) {
    if (!g_sessionStartedEmitted.exchange(true)) {
        const std::string hrecoStr = hreco ? HandleToHexString(hreco) : "0x0000000000000000";
        const std::string heventStr = hevent ? HandleToHexString(hevent) : "0x0000000000000000";
        LogInfo("Auto-emitting session_started event: hreco=" + hrecoStr + ", hevent=" + heventStr);
        const std::string payload = BuildEventJsonPayload("session_started", "hreco", hrecoStr, "hevent", heventStr);
        PushToQueue(payload);
    }
}

void __stdcall DetourSessionStartedCallback(SPXRECOHANDLE hreco, SPXEVENTHANDLE hevent, void* pvContext) {
    EnsureSessionStartedEmitted(hreco, hevent);
    const std::string hrecoStr = HandleToHexString(hreco);
    const std::string heventStr = HandleToHexString(hevent);
    LogInfo("DetourSessionStartedCallback called: hreco=" + hrecoStr + ", hevent=" + heventStr);

    const std::string payload = BuildEventJsonPayload("session_started", "hreco", hrecoStr, "hevent", heventStr);
    PushToQueue(payload);

    PSESSION_CALLBACK_FUNC orig = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sessionStartedMutex);
        auto it = g_sessionStartedCallbacks.find(hreco);
        if (it != g_sessionStartedCallbacks.end()) {
            orig = it->second;
        }
    }
    if (orig) {
        orig(hreco, hevent, pvContext);
    }
}

int __stdcall Detour_recognizer_session_started_set_callback(SPXRECOHANDLE hreco, PSESSION_CALLBACK_FUNC callback, void* pvContext) {
    const std::string hrecoStr = HandleToHexString(hreco);
    LogInfo("recognizer_session_started_set_callback: hreco=" + hrecoStr);
    {
        std::lock_guard<std::mutex> lock(g_sessionStartedMutex);
        g_sessionStartedCallbacks[hreco] = callback;
    }
    return fpOriginalRecognizerSessionStartedSetCallback(hreco, DetourSessionStartedCallback, pvContext);
}

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

DWORD WINAPI HookThread(LPVOID lpParam) {
    // Yield execution to allow DLL loading thread to complete DllMain and exit Windows Loader Lock
    Sleep(100);
    HMODULE hCoreDLL = reinterpret_cast<HMODULE>(lpParam);
    
    if (hCoreDLL == nullptr) {
        hCoreDLL = FindModuleByPartialName(GetObfuscatedTargetDllName());
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
    FARPROC pSessionStartedCallback = GetProcAddress(hCoreDLL, "recognizer_session_started_set_callback");

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

    if (pSessionStartedCallback) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(pSessionStartedCallback),
                          &Detour_recognizer_session_started_set_callback,
                          reinterpret_cast<LPVOID*>(&fpOriginalRecognizerSessionStartedSetCallback)) != MH_OK) {
            LogError("HookThread: MH_CreateHook for recognizer_session_started_set_callback failed.");
        } else {
            LogInfo("HookThread: Hook for recognizer_session_started_set_callback created.");
        }
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogError("HookThread: MH_EnableHook failed.");
        return 0;
    }

    LogInfo("HookThread: Hooks enabled successfully.");

    // Launch Heap Scanner for late injection fallback. 
    // It's declared in HeapScanner.h but we don't want to include it here to avoid circular dep.
    // Assuming HeapScannerThread is exported/linked. Wait, HeapScannerThread is defined in dllmain.cpp originally.
    // Wait! Let's check where HeapScannerThread is. It's in dllmain.cpp line 480. So we should move it here?
    // The prompt says "Trong HookThread(): thay thế 3 đoạn build obfuscated DLL name bằng GetObfuscatedTargetDllName() call".
    // Also "HookThread() lines 495-580" goes to SdkHooks.cpp. HeapScannerThread is lines 480-490.
    // Wait, let's include "HeapScanner.h" here to get HeapScannerThread if it's there. But HeapScannerThread was in dllmain.cpp. 
    // Ah, wait. Let's just define HeapScannerThread here!
    CreateThread(NULL, 0, HeapScannerThread, NULL, 0, NULL);

    return 0;
}

DWORD WINAPI HeapScannerThread(LPVOID /*lpParam*/) {
    LogInfo("HeapScannerThread: Started. Waiting 2 seconds before scanning...");
    Sleep(2000);
    if (!g_sessionStartedEmitted.load()) {
        LogInfo("HeapScannerThread: session_started not yet emitted. Scanning heap...");
        ScanHeapForRecognizerHandle(); // declared in HeapScanner.h
    } else {
        LogInfo("HeapScannerThread: session_started already emitted via hook. Skipping scan.");
    }
    return 0;
}
