#include "pch.h"
#include <Windows.h>
#include <string>
#include <fstream>
#include <vector>
#include <Psapi.h>
#include <algorithm> 
#include "MinHook.h"

// =============================================================
// LOGGER RA FILE
// =============================================================
void LogToFile(const std::string& msg) {
    std::ofstream logFile("C:\\Users\\Public\\live_caption_debug.txt", std::ios_base::app);
    if (logFile.is_open()) {
        logFile << "[HookCore] " << msg << std::endl;
        logFile.close();
    }
}

// =============================================================
// HELPER MỚI: TÌM MODULE CHỦ ĐỘNG (Thay thế GetModuleHandle)
// =============================================================
HMODULE FindModuleByPartialName(const std::string& partialName) {
    HMODULE hMods[1024];
    DWORD cbNeeded;
    HANDLE hProcess = GetCurrentProcess();

    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            TCHAR szModName[MAX_PATH];
            if (GetModuleFileNameEx(hProcess, hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) {
                std::wstring wName(szModName);
                std::string sName(wName.begin(), wName.end());

                std::string sNameLower = sName;
                std::transform(sNameLower.begin(), sNameLower.end(), sNameLower.begin(), ::tolower);

                std::string partialNameLower = partialName;
                std::transform(partialNameLower.begin(), partialNameLower.end(), partialNameLower.begin(), ::tolower);

                if (sNameLower.find(partialNameLower) != std::string::npos) {
                    return hMods[i];
                }
            }
        }
    }
    return nullptr;
}

// =============================================================
// CORE LOGIC
// =============================================================
typedef void* SPXRESULTHANDLE;
typedef int(__stdcall* result_get_text_t)(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen);
result_get_text_t fpOriginalResultGetText = nullptr;

// Hook Function
int __stdcall Detour_result_get_text(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen) {
    int ret = fpOriginalResultGetText(hresult, buffer, bufferLen);

    if (ret == 0 && buffer != nullptr && buffer[0] != '\0') {
        LogToFile("CAPTURED TEXT: " + std::string(buffer));
    }
    return ret;
}

DWORD WINAPI HookThread(LPVOID lpParam) {
    LogToFile("Thread started. Hunting for Core DLL Handle...");

    HMODULE hModule = nullptr;
    int retry = 0;
    while (hModule == nullptr && retry < 20) {
        hModule = FindModuleByPartialName("microsoft.cognitiveservices.speech.core.dll");
        if (hModule == nullptr) {
            Sleep(1000);
            LogToFile("Scanning for DLL... Retry " + std::to_string(retry));
        }
        retry++;
    }

    if (hModule == nullptr) {
        LogToFile("FATAL: Could not find handle for Core DLL even via EnumProcessModules.");
        return 0;
    }

    LogToFile("SUCCESS: Core DLL Handle found at: " + std::to_string((unsigned long long)hModule));

    // Get func address
    FARPROC pFunc = GetProcAddress(hModule, "result_get_text");

    if (pFunc) {
        LogToFile("Function 'result_get_text' found at: " + std::to_string((unsigned long long)pFunc));

        if (MH_Initialize() == MH_OK) {
            MH_CreateHook((LPVOID)pFunc, &Detour_result_get_text, (LPVOID*)&fpOriginalResultGetText);
            if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK) {
                LogToFile("HOOK ENABLED! Waiting for subtitles...");
            }
            else {
                LogToFile("ERROR: MH_EnableHook Failed");
            }
        }
        else {
            LogToFile("ERROR: MH_Initialize Failed");
        }
    }
    else {
        LogToFile("ERROR: 'result_get_text' not found by GetProcAddress. Dumping exports might be needed.");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        std::ofstream logFile("C:\\Users\\Public\\live_caption_debug.txt", std::ios_base::trunc);
        logFile << "[HookCore] New Session Started" << std::endl;
        logFile.close();

        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)HookThread, NULL, 0, NULL);
    }
    return TRUE;
}