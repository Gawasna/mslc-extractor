#include "pch.h"
#include <Windows.h>
#include <fstream>
#include <string>
#include "AgentLogger.h"
#include "NtDecls.h"
#include "SafeHandle.h"
#include "PipeSender.h"
#include "SdkHooks.h"
#include "DllWatcher.h"
#include "MinHook.h"

HMODULE g_hModule = NULL;
std::string g_logPath = "";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        g_logPath = GetLogPath();
        DisableThreadLibraryCalls(hModule);
        {
            std::ofstream logFile(g_logPath, std::ios_base::trunc);
            logFile << "[Agent] === New Session Started ===\n";
        }
        LogInfo("DllMain: DLL_PROCESS_ATTACH.");
        g_hSenderThread.Set(CreateThread(NULL, 0, SenderThread, NULL, 0, NULL));
        RegisterDllWatcher();
        TryHookIfAlreadyLoaded();
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        LogInfo("DllMain: DLL_PROCESS_DETACH. Unloading...");
        UnregisterDllWatcher();
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
